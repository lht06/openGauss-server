/* -------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_func.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "catalog/gs_encrypted_proc.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "catalog/gs_encrypted_columns.h"

static Oid FuncNameAsType(List* funcname);
static Node* ParseComplexProjection(ParseState* pstate, char* funcname, Node* first_arg, int location);
static List* GetDefaultVale(Oid funcoid, const int* argnumbers, int ndargs, bool* defaultValid);
static Oid cl_get_input_param_original_type(Oid func_oid, int argno);
static bool CheckDefaultArgsPosition(int2vector* defaultargpos, int pronargdefaults, int ndargs,
    int pronallargs, int pronargs, HeapTuple procTup);
static void unify_hypothetical_args(ParseState *pstate,
    List *fargs, int numAggregatedArgs, Oid *actual_arg_types, Oid *declared_arg_types);
/*
 *	Parse a function call
 *
 *	For historical reasons, Postgres tries to treat the notations tab.col
 *	and col(tab) as equivalent: if a single-argument function call has an
 *	argument of complex type and the (unqualified) function name matches
 *	any attribute of the type, we take it as a column projection.  Conversely
 *	a function of a single complex-type argument can be written like a
 *	column reference, allowing functions to act like computed columns.
 *
 *	Hence, both cases come through here.  The is_column parameter tells us
 *	which syntactic construct is actually being dealt with, but this is
 *	intended to be used only to deliver an appropriate error message,
 *	not to affect the semantics.  When is_column is true, we should have
 *	a single argument (the putative table), unqualified function name
 *	equal to the column name, and no aggregate or variadic decoration.
 *	Also, when is_column is true, we return NULL on failure rather than
 *	reporting a no-such-function error.
 *
 *	The argument expressions (in fargs) must have been transformed already.
 *	But the agg_order expressions, if any, have not been.
 */
Node* ParseFuncOrColumn(ParseState* pstate, List* funcname, List* fargs, Node* last_srf, FuncCall* fn, int location,
                        bool call_func)
{
    bool is_column = (fn == NULL);
    List* agg_order = (fn ? fn->agg_order : NIL);
    Expr *agg_filter = NULL;
    bool agg_within_group = (fn ? fn->agg_within_group : false);
    bool agg_star = (fn ? fn->agg_star : false);
    bool agg_distinct = (fn ? fn->agg_distinct : false);
    bool func_variadic = (fn ? fn->func_variadic : false);
    bool is_from_last = (fn ? fn->is_from_last : false);
    bool is_ignore_nulls = (fn ? fn->is_ignore_nulls : false);
    WindowDef* over = (fn ? fn->over : NULL);
    KeepClause *aggKeep = (fn ? fn->aggKeep : NULL);
    Oid rettype;
    int rettype_orig = -1;
    Oid funcid;
    ListCell* l = NULL;
    ListCell* nextl = NULL;
    Node* first_arg = NULL;
    int nargs;
    int nargsplusdefs;
    Oid actual_arg_types[FUNC_MAX_ARGS];
    Oid* declared_arg_types = NULL;
    List* argnames = NIL;
    List* argdefaults = NIL;
    Node* retval = NULL;
    bool retset = false;
    int nvargs;
    Oid vatype;
    FuncDetailCode fdresult;
    char aggkind = 'n';
    char* name_string = NULL;
    Oid refSynOid = InvalidOid;
    /*
     * If there's an aggregate filter, transform it using transformWhereClause
     */
    if (fn && fn->agg_filter != NULL)
        agg_filter = (Expr *)transformWhereClause(pstate, fn->agg_filter, EXPR_KIND_FILTER, "FILTER");
    /*
     * Most of the rest of the parser just assumes that functions do not have
     * more than FUNC_MAX_ARGS parameters.	We have to test here to protect
     * against array overruns, etc.  Of course, this may not be a function,
     * but the test doesn't hurt.
     */
    if (list_length(fargs) > FUNC_MAX_ARGS)
        ereport(ERROR,
            (errcode(ERRCODE_TOO_MANY_ARGUMENTS),
                errmsg_plural("cannot pass more than %d argument to a function",
                    "cannot pass more than %d arguments to a function",
                    FUNC_MAX_ARGS,
                    FUNC_MAX_ARGS),
                parser_errposition(pstate, location)));

    /*
     * Extract arg type info in preparation for function lookup.
     *
     * If any arguments are Param markers of type VOID, we discard them from
     * the parameter list.	This is a hack to allow the JDBC driver to not
     * have to distinguish "input" and "output" parameter symbols while
     * parsing function-call constructs.  We can't use foreach() because we
     * may modify the list ...
     */
    nargs = 0;
    for (l = list_head(fargs); l != NULL; l = nextl) {
        Node* arg = (Node*)lfirst(l);
        Oid argtype = exprType(arg);
        nextl = lnext(l);

        if (argtype == VOIDOID && IsA(arg, Param) && !is_column && !agg_within_group) {
            fargs = list_delete_ptr(fargs, arg);
            continue;
        }

        actual_arg_types[nargs++] = argtype;
    }

    /*
     * Check for named arguments; if there are any, build a list of names.
     *
     * We allow mixed notation (some named and some not), but only with all
     * the named parameters after all the unnamed ones.  So the name list
     * corresponds to the last N actual parameters and we don't need any extra
     * bookkeeping to match things up.
     */
    argnames = NIL;
    foreach (l, fargs) {
        Node* arg = (Node*)lfirst(l);

        if (IsA(arg, NamedArgExpr)) {
            NamedArgExpr* na = (NamedArgExpr*)arg;
            ListCell* lc = NULL;

            /* Reject duplicate arg names */
            foreach (lc, argnames) {
                if (strcmp(na->name, (char*)lfirst(lc)) == 0)
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("argument name \"%s\" used more than once", na->name),
                            parser_errposition(pstate, na->location)));
            }
            argnames = lappend(argnames, na->name);
        } else {
            if (argnames != NIL)
                ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("positional argument cannot follow named argument"),
                        parser_errposition(pstate, exprLocation(arg))));
        }
    }

    if (fargs != NIL) {
        first_arg = (Node*)linitial(fargs);
        Assert(first_arg != NULL);
    }

    /*
     * Check for column projection: if function has one argument, and that
     * argument is of complex type, and function name is not qualified, then
     * the "function call" could be a projection.  We also check that there
     * wasn't any aggregate or variadic decoration, nor an argument name.
     */
    if (nargs == 1 && agg_order == NIL && !agg_star && !agg_distinct && over == NULL && !func_variadic &&
        agg_filter == NULL &&
        argnames == NIL && list_length(funcname) == 1) {
        Oid argtype = actual_arg_types[0];

        if (argtype == RECORDOID || ISCOMPLEX(argtype)) {
            retval = ParseComplexProjection(pstate, strVal(linitial(funcname)), first_arg, location);
            if (retval != NULL)
                return retval;

            /*
             * If ParseComplexProjection doesn't recognize it as a projection,
             * just press on.
             */
        }
    }

    /*
     * Okay, it's not a column projection, so it must really be a function.
     * func_get_detail looks up the function in the catalogs, does
     * disambiguation for polymorphic functions, handles inheritance, and
     * returns the funcid and type and set or singleton status of the
     * function's return value.  It also returns the true argument types to
     * the function.
     *
     * Note: for a named-notation or variadic function call, the reported
     * "true" types aren't really what is in pg_proc: the types are reordered
     * to match the given argument order of named arguments, and a variadic
     * argument is replaced by a suitable number of copies of its element
     * type.  We'll fix up the variadic case below.  We may also have to deal
     * with default arguments.
     */
    fdresult = func_get_detail(funcname,
        fargs,
        argnames,
        nargs,
        actual_arg_types,
        !func_variadic,
        true,
        &funcid,
        &rettype,
        &retset,
        &nvargs,
        &vatype,
        &declared_arg_types,
        &argdefaults,
        call_func,
        &refSynOid,
        &rettype_orig);
    name_string = NameListToString(funcname);
    if (fdresult == FUNCDETAIL_COERCION) {
        /*
         * We interpreted it as a type coercion. coerce_type can handle these
         * cases, so why duplicate code...
         */
        return coerce_type(pstate,
            (Node*)linitial(fargs),
            actual_arg_types[0],
            rettype,
            rettype_orig,
            COERCION_EXPLICIT,
            COERCE_EXPLICIT_CALL,
            NULL,
            NULL,
            location);
    } else if (fdresult == FUNCDETAIL_NORMAL) {
        /*
         * Normal function found; was there anything indicating it must be an
         * aggregate?
         */
        if (agg_star)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("%s(*) specified, but %s is not an aggregate function", name_string, name_string),
                    parser_errposition(pstate, location)));
        if (agg_distinct)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("DISTINCT specified, but %s is not an aggregate function", name_string),
                    parser_errposition(pstate, location)));
        if (agg_within_group)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("WITHIN GROUP specified, but %s is not an aggregate function", name_string),
                    parser_errposition(pstate, location)));
        if (agg_order != NIL)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("ORDER BY specified, but %s is not an aggregate function", name_string),
                    parser_errposition(pstate, location)));
        if (agg_filter)
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("FILTER specified, but %s is not an aggregate function", NameListToString(funcname)),
                            parser_errposition(pstate, location)));
        if (over != NULL)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("OVER specified, but %s is not a window function nor an aggregate function", name_string),
                    parser_errposition(pstate, location)));
    } else if (fdresult == FUNCDETAIL_AGGREGATE) {
        /* It's an aggregate; fetch needed info from the pg_aggregate entry. */
        HeapTuple tup;
        int catDirectArgs = 0;
        int pronargs = nargs;
        bool isOrderedSet = false;
        bool isnull = false;

        tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(funcid));
        if (!HeapTupleIsValid(tup)) /* should not happen */
            elog(ERROR, "cache lookup failed for aggregate %u", funcid);
        /* 91269 version support orderedset agg  */
        aggkind = DatumGetChar(SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_aggkind, &isnull));
        if (!AGGKIND_IS_ORDERED_SET(aggkind))
            aggkind = 'n';
        isOrderedSet = AGGKIND_IS_ORDERED_SET(aggkind);
        catDirectArgs = DatumGetInt8(SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_aggnumdirectargs, &isnull));
        ReleaseSysCache(tup);

        /* Now do the saftey check. */
        if (isOrderedSet) {
            int numDirectArgs = 0;
            int numAggregatedArgs = 0;

            if (!agg_within_group)
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("WITHIN GROUP is required for ordered-set aggregate %s", name_string),
                        parser_errposition(pstate, location)));
            if (over != NULL)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("OVER is not supported for ordered-set aggregate %s", name_string),
                        parser_errposition(pstate, location)));

            if (agg_distinct)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("DISTINCT + WITHIN GROUP is not supported for ordered-set aggregate %s", name_string),
                        parser_errposition(pstate, location)));

            if (func_variadic)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("VARIADIC + WITHIN GROUP is not supported for ordered-set aggregate %s", name_string),
                        parser_errposition(pstate, location)));

            /*
             * func_get_detail might have selected an aggregate that doesn't
             * really match because it requires a different division of direct
             * and aggregated arguments.  Check if the number of direct arguments
             * is actually equal to the number record in the pp_aggregate table.
             * While agg_order is the number of aggregated args, we can conduct
             * the number of direct args.
             */
            numAggregatedArgs = list_length(agg_order);
            numDirectArgs = nargs - numAggregatedArgs;
            Assert(numDirectArgs >= 0);
            if (nvargs > 1)
                pronargs -= nvargs - 1;
            /* If it isn't variadic or the agg was "... ORDER BY VARIADIC" */
            if (!OidIsValid(vatype) || catDirectArgs < pronargs) {
                if (numDirectArgs != catDirectArgs)
                    ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_FUNCTION),
                            errmsg("function %s does not exist",
                                func_signature_string(funcname, nargs, argnames, actual_arg_types)),
                            errhint("Ordered-set aggregate %s requires %d direct arguments, not %d.",
                                name_string,
                                catDirectArgs,
                                numDirectArgs),
                            parser_errposition(pstate, location)));
            } else {
                if (aggkind == AGGKIND_HYPOTHETICAL) {
                    int argsfactor = 2;
                    if (nvargs != argsfactor * numAggregatedArgs)
                        ereport(ERROR,
                                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                                 errmsg("function %s does not exist",
                                        func_signature_string(funcname, nargs, argnames, actual_arg_types)),
                                 errhint("To use the hypothetical-set aggregate %s, the number of hypothetical direct "
                                         "arguments (here %d) must match the number of ordering columns (here %d).",
                                         NameListToString(funcname), nvargs - numAggregatedArgs, numAggregatedArgs),
                                 parser_errposition(pstate, location)));
                }
                /* the agg was "..., VARIADIC ORDER BY VARIADIC" */
                if (nvargs <= numAggregatedArgs)
                    ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_FUNCTION),
                            errmsg("function %s does not exist",
                                func_signature_string(funcname, nargs, argnames, actual_arg_types)),
                            errhint("Ordered-set aggregate %s requires at least %d direct arguments.",
                                name_string,
                                catDirectArgs),
                            parser_errposition(pstate, location)));
            }
            /* Check type matching of hypothetical arguments */
            if (aggkind == AGGKIND_HYPOTHETICAL)
                unify_hypothetical_args(pstate, fargs, numAggregatedArgs,
                                        actual_arg_types, declared_arg_types);

        } else {
            /* Normal aggregate, so it can't have WITHIN GROUP */
            if (agg_within_group)
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("%s is not an ordered-set aggregate, so it cannot have WITHIN GROUP", name_string),
                        parser_errposition(pstate, location)));
             if (aggKeep != NULL && over != NULL && over->orderClause != NIL)
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("ORDER BY of OVER clause is prohibited in function %s with keep", name_string),
                        parser_errposition(pstate, location)));
        }
    } else if (fdresult == FUNCDETAIL_WINDOWFUNC) {
        /*
         * A true window function should be called with a window definition,
         * which is assigned by over().
         */
        if (over == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("window function %s requires an OVER clause", name_string),
                    parser_errposition(pstate, location)));
        /* And, per spec, WITHIN GROUP isn't allowed */
        if (agg_within_group)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("window function %s cannot have WITHIN GROUP", name_string),
                    parser_errposition(pstate, location)));
    } else {
        /*
         * Oops.  Time to die.
         *
         * If we are dealing with the attribute notation rel.function, let the
         * caller handle failure.
         */
        if (is_column)
            return NULL;

        /*
         * Else generate a detailed complaint for a function
         */
        if (fdresult == FUNCDETAIL_MULTIPLE)
            ereport(ERROR,
                (errcode(ERRCODE_AMBIGUOUS_FUNCTION),
                    errmsg("function %s is not unique",
                        func_signature_string(funcname, nargs, argnames, actual_arg_types)),
                    errhint("Could not choose a best candidate function. "
                            "You might need to add explicit type casts."),
                    parser_errposition(pstate, location)));
        else if (list_length(agg_order) > 1 && !agg_within_group) {
            /* It's agg(x, ORDER BY y,z) ... perhaps misplaced ORDER BY */
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("function %s does not exist",
                        func_signature_string(funcname, nargs, argnames, actual_arg_types)),
                    errhint("No aggregate function matches the given name and argument types. "
                            "Perhaps you misplaced ORDER BY; ORDER BY must appear "
                            "after all regular arguments of the aggregate."),
                    parser_errposition(pstate, location)));
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("function %s does not exist",
                        func_signature_string(funcname, nargs, argnames, actual_arg_types)),
                    errhint("No function matches the given name and argument types. "
                            "You might need to add explicit type casts."),
                    parser_errposition(pstate, location)));
        }
    }

    /*
     * If there are default arguments, we have to include their types in
     * actual_arg_types for the purpose of checking generic type consistency.
     * However, we do NOT put them into the generated parse node, because
     * their actual values might change before the query gets run.	The
     * planner has to insert the up-to-date values at plan time.
     */
    nargsplusdefs = nargs;
    foreach (l, argdefaults) {
        Node* expr = (Node*)lfirst(l);

        /* probably shouldn't happen ... */
        if (nargsplusdefs >= FUNC_MAX_ARGS)
            ereport(ERROR,
                (errcode(ERRCODE_TOO_MANY_ARGUMENTS),
                    errmsg_plural("cannot pass more than %d argument to a function",
                        "cannot pass more than %d arguments to a function",
                        FUNC_MAX_ARGS,
                        FUNC_MAX_ARGS),
                    parser_errposition(pstate, location)));

        actual_arg_types[nargsplusdefs++] = exprType(expr);
    }

    /*
     * enforce consistency with polymorphic argument and return types,
     * possibly adjusting return type or declared_arg_types (which will be
     * used as the cast destination by make_fn_arguments)
     */
    rettype = enforce_generic_type_consistency(actual_arg_types, declared_arg_types, nargs, rettype, false);

    /* perform the necessary typecasting of arguments */
    make_fn_arguments(pstate, fargs, actual_arg_types, declared_arg_types);

    /*
     * If it's a variadic function call, transform the last nvargs arguments
     * into an array --- unless it's an "any" variadic.
     */
    if (nvargs > 0 && declared_arg_types[nargs - 1] != ANYOID) {
        ArrayExpr* newa = makeNode(ArrayExpr);
        int non_var_args = nargs - nvargs;
        List* vargs = NIL;

        Assert(non_var_args >= 0);
        vargs = list_copy_tail(fargs, non_var_args);
        fargs = list_truncate(fargs, non_var_args);

        newa->elements = vargs;
        /* assume all the variadic arguments were coerced to the same type */
        newa->element_typeid = exprType((Node*)linitial(vargs));
        newa->array_typeid = get_array_type(newa->element_typeid);
        if (!OidIsValid(newa->array_typeid))
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("could not find array type for data type %s", format_type_be(newa->element_typeid)),
                    parser_errposition(pstate, exprLocation((Node*)vargs))));
        /* array_collid will be set by parse_collate.c */
        newa->multidims = false;
        newa->location = exprLocation((Node*)vargs);

        fargs = lappend(fargs, newa);

	/* We could not have had VARIADIC marking before ... For age */
        Assert(!func_variadic);
        /* ... but now, it's a VARIADIC call */
        func_variadic = true;
    }

    /* if it returns a set, check that's OK */
    if (retset && pstate && pstate->p_is_flt_frame) {
        check_srf_call_placement(pstate, last_srf, location);
    }

    /* build the appropriate output structure */
    if (fdresult == FUNCDETAIL_NORMAL) {
        FuncExpr* funcexpr = makeNode(FuncExpr);

        funcexpr->funcid = funcid;
        funcexpr->funcresulttype = rettype;
        funcexpr->funcresulttype_orig = rettype_orig;
        funcexpr->funcretset = retset;
        funcexpr->funcvariadic = func_variadic;
        funcexpr->funcformat = COERCE_EXPLICIT_CALL;
        /* funccollid and inputcollid will be set by parse_collate.c */
        funcexpr->args = fargs;
        funcexpr->location = location;
        /* refSynOid will be set when need to record dependency */
        funcexpr->refSynOid = refSynOid;

        retval = (Node*)funcexpr;
        /*Return type of to_date function  will be changed from timestamp to date type in C_FORMAT*/
        if (u_sess->attr.attr_sql.sql_compatibility != A_FORMAT &&
            (funcid == TODATEFUNCOID || funcid == TODATEDEFAULTFUNCOID)) {
            FuncExpr* timestamp_date_fun = makeNode(FuncExpr);

            timestamp_date_fun->funcid = TIMESTAMP2DATEOID;
            timestamp_date_fun->funcresulttype = DATEOID;
            timestamp_date_fun->funcretset = false;
            timestamp_date_fun->funcformat = COERCE_EXPLICIT_CAST;
            timestamp_date_fun->args = list_make1(funcexpr);
            timestamp_date_fun->location = location;
            retval = (Node*)timestamp_date_fun;
        }
    } else if (fdresult == FUNCDETAIL_AGGREGATE && over == NULL) {
        /* aggregate function */
        Aggref* aggref = makeNode(Aggref);

        aggref->aggfnoid = funcid;
        aggref->aggtype = rettype;
        /* aggcollid and inputcollid will be set by parse_collate.c */
        /* args, aggorder, aggdistinct will be set by transformAggregateCall */
        aggref->aggfilter = agg_filter;
        aggref->aggstar = agg_star;
        aggref->aggvariadic = func_variadic;
        aggref->aggkind = aggkind;
        /* agglevelsup will be set by transformAggregateCall */
        aggref->location = location;

        /*
         * Reject attempt to call a parameterless aggregate without (*)
         * syntax.	This is mere pedantry but some folks insisted ...
         */
        if (fargs == NIL && !agg_star && !agg_within_group)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("%s(*) must be used to call a parameterless aggregate function", NameListToString(funcname)),
                    parser_errposition(pstate, location)));

        if (retset)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                    errmsg("aggregates cannot return sets"),
                    parser_errposition(pstate, location)));

        /*
         * Currently it's not possible to define an aggregate with named
         * arguments, so this case should be impossible.  Check anyway because
         * the planner and executor wouldn't cope with NamedArgExprs in an
         * Aggref node.
         */
        if (argnames != NIL)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("aggregates cannot use named arguments"),
                    parser_errposition(pstate, location)));
        if (aggKeep != NULL) {
            aggref->aggiskeep = true;
            agg_order = aggKeep->keep_order;
            aggref->aggkpfirst = aggKeep->rank_first;
        } else {
            aggref->aggiskeep = false;
        }
        /* parse_agg.c does additional aggregate-specific processing */
        transformAggregateCall(pstate, aggref, fargs, agg_order, agg_distinct);

        retval = (Node*)aggref;
    } else {
        /* window function */
        WindowFunc* wfunc = makeNode(WindowFunc);

        /*
         * True window functions must be called with a window definition.
         */
        if (over == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("window function call requires an OVER clause"),
                    parser_errposition(pstate, location)));

        Assert(!agg_within_group);

        wfunc->winfnoid = funcid;
        wfunc->wintype = rettype;
        /* wincollid and inputcollid will be set by parse_collate.c */
        wfunc->args = fargs;
        /* winref will be set by transformWindowFuncCall */
        wfunc->winstar = agg_star;
        wfunc->winagg = (fdresult == FUNCDETAIL_AGGREGATE);
        wfunc->location = location;
        wfunc->is_from_last = is_from_last;
        wfunc->is_ignore_nulls = is_ignore_nulls;
        /*
         * agg_star is allowed for aggregate functions but distinct isn't
         */
        if (agg_distinct)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("DISTINCT is not implemented for window functions"),
                    parser_errposition(pstate, location)));

        /*
         * Reject attempt to call a parameterless aggregate without (*)
         * syntax.	This is mere pedantry but some folks insisted ...
         */
        if (wfunc->winagg && fargs == NIL && !agg_star)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("%s(*) must be used to call a parameterless aggregate function", NameListToString(funcname)),
                    parser_errposition(pstate, location)));

        /*
         * ordered aggs not allowed in windows yet, execpt listagg
         */
        if (pg_strcasecmp(NameListToString(funcname), "listagg") != 0 && agg_order != NIL)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("aggregate ORDER BY is not implemented for window functions"),
                    parser_errposition(pstate, location)));
        if (pstate->p_is_flt_frame) {
            /*
            * Window functions can't either take or return sets
            */
            if (pstate->p_last_srf != last_srf)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("window function calls cannot contain set-returning function calls"),
                                parser_errposition(pstate, exprLocation(pstate->p_last_srf))));
        }

        if (retset)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                    errmsg("window functions cannot return sets"),
                    parser_errposition(pstate, location)));

        /*
         * We might want to support this later, but for now reject it because
         * the planner and executor wouldn't cope with NamedArgExprs in a
         * WindowFunc node.
         */
        if (argnames != NIL)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("window functions cannot use named arguments"),
                    parser_errposition(pstate, location)));

        /* For listagg, multiple different order info is not allowed.*/
        ListCell* lc = NULL;
        bool isInvalidOrder = false;

        foreach (lc, agg_order) {
            SortBy* aggorder = (SortBy*)lfirst(lc);
            if (over->orderClause != NIL && !list_member(over->orderClause, aggorder)) {
                isInvalidOrder = true;
                break;
            }
        }

        if (isInvalidOrder)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("window functions cannot allow multiple different order info"),
                    parser_errposition(pstate, location)));

        if (over->orderClause == NIL)
            over->orderClause = agg_order;

        if (aggKeep != NULL) {
            ListCell* lc = NULL;
            List* tlist = NIL;

            wfunc->winkpfirst = aggKeep->rank_first;
            
            foreach (lc, aggKeep->keep_order) {
                SortBy* arg = (SortBy*)lfirst(lc);
                wfunc->keep_args = lappend(wfunc->keep_args, transformExprRecurse(pstate, arg->node));
            }
            int save_next_resno = pstate->p_next_resno;
            wfunc->winkporder = transformSortClause(pstate,
                                                    aggKeep->keep_order,
                                                    &tlist,
                                                    EXPR_KIND_ORDER_BY,
                                                    true,
                                                    true);
            pstate->p_next_resno = save_next_resno;
            list_free_deep(tlist);
        }
        /* parse_agg.c does additional window-func-specific processing */
        transformWindowFuncCall(pstate, wfunc, over);

        retval = (Node*)wfunc;
    }

    if (retset && pstate && pstate->p_is_flt_frame) {
        /* if it returns a set, remember it for error checks at higher levels */
        pstate->p_last_srf = retval;
    }


    return retval;
}

/* func_match_argtypes()
 *
 * Given a list of candidate functions (having the right name and number
 * of arguments) and an array of input datatype OIDs, produce a shortlist of
 * those candidates that actually accept the input datatypes (either exactly
 * or by coercion), and return the number of such candidates.
 *
 * Note that can_coerce_type will assume that UNKNOWN inputs are coercible to
 * anything, so candidates will not be eliminated on that basis.
 *
 * NB: okay to modify input list structure, as long as we find at least
 * one match.  If no match at all, the list must remain unmodified.
 */
int func_match_argtypes(
    int nargs, Oid* input_typeids, FuncCandidateList raw_candidates, FuncCandidateList* candidates) /* return value */
{
    FuncCandidateList current_candidate;
    FuncCandidateList next_candidate;
    int ncandidates = 0;

    *candidates = NULL;

    for (current_candidate = raw_candidates; current_candidate != NULL; current_candidate = next_candidate) {
        next_candidate = current_candidate->next;
        if (can_coerce_type(nargs, input_typeids, current_candidate->args, COERCION_IMPLICIT)) {
            current_candidate->next = *candidates;
            *candidates = current_candidate;
            ncandidates++;
        }
    }

    return ncandidates;
} /* func_match_argtypes() */

/* get category priority
 *
 * for a given categoryoid of any
 * 'X': unknown
 * 'U': User
 * 'B' Boolean
 * 'G' Geometric
 * 'I' Network
 * the category priority should be 0
 */
static int GetCategoryPriority(TYPCATEGORY categoryoid)
{
    int result = 0;

    if (u_sess->attr.attr_sql.convert_string_to_digit) {
        switch (categoryoid) {
            case ('N'): /*Numeric*/
                result = 4;
                break;
            case ('T'): /*Timespan*/
                result = 3;
                break;
            case ('D'): /*Datetime*/
                result = 2;
                break;
            case ('S'): /*String*/
                result = 1;
                break;
            default:
                result = 0;
                break;
        }
    } else {
        switch (categoryoid) {
            case ('D'): /*Datetime*/
                result = 1;
                break;
            case ('T'): /*Timespan*/
                result = 2;
                break;
            case ('N'): /*Numeric*/
                result = 3;
                break;
            case ('S'): /*String*/
                result = 4;
                break;
            default:
                result = 0;
                break;
        }
    }

    return result;
}

/* get type priority */
int GetPriority(Oid typeoid)
{
    int result = 0;

    switch (typeoid) {
        /* bool */
        case (BOOLOID):
            result = 0;
            break;

        /* string */
        case (CHAROID):
            result = 0;
            break;
        case (NAMEOID):
            result = 1;
            break;
        case (BPCHAROID):
            result = 2;
            break;
        case (VARCHAROID):
        case (NVARCHAR2OID):
            result = 3;
            break;
        case (TEXTOID):
            result = 4;
            break;

        /* bitstring */
        case (BITOID):
            result = 0;
            break;
        case (VARBITOID):
            result = 1;
            break;

        /* numeric */
        case (CASHOID):
            result = 0;
            break;
        case (INT2OID):
            result = 1;
            break;
        case (INT4OID):
            result = 2;
            break;
        case (OIDOID):
            result = 3;
            break;
        case (INT8OID):
            result = 4;
            break;
        case (FLOAT4OID):
            result = 5;
            break;
        case (FLOAT8OID):
            result = 6;
            break;
        case (NUMERICOID):
            result = 7;
            break;

        /* datetime */
        case (TIMEOID):
            result = 0;
            break;
        case (TIMETZOID):
            result = 1;
            break;
        case (ABSTIMEOID):
            result = 2;
            break;
        case (DATEOID):
            result = 3;
            break;
        /*
         * According to precision,  priority of smalldatetime between date and timestamp.
         * Smalldatetime will return 0 before that is very error.
         */
        case (SMALLDATETIMEOID):
            result = 4;
            break;
        case (TIMESTAMPOID):
            result = 5;
            break;
        case (TIMESTAMPTZOID):
            result = 6;
            break;

        /* timespan */
        case (RELTIMEOID):
            result = 0;
            break;
        case (TINTERVALOID):
            result = 1;
            break;
        case (INTERVALOID):
            result = 2;
            break;

        /* the types below are not used by now */
        case (POINTOID):
        case (LSEGOID):
        case (PATHOID):
        case (BOXOID):
        case (POLYGONOID):
        case (LINEOID):
        case (CIRCLEOID):
            result = 0;
            break;

        case (INETOID):
        case (CIDROID):
            result = 0;
            break;

        case (UNKNOWNOID):
        case (InvalidOid):
            result = 0;
            break;

        /* OID */
        case (REGPROCOID):
        case (REGPROCEDUREOID):
        case (REGOPEROID):
        case (REGOPERATOROID):
        case (REGCLASSOID):
        case (REGTYPEOID):
        case (REGCONFIGOID):
        case (REGDICTIONARYOID):
            result = 0;
            break;

        case (RECORDOID):
        case (CSTRINGOID):
        case (ANYOID):
        case (ANYARRAYOID):
        case (VOIDOID):
        case (TRIGGEROID):
        case (LANGUAGE_HANDLEROID):
        case (INTERNALOID):
        case (OPAQUEOID):
        case (ANYELEMENTOID):
        case (ANYNONARRAYOID):
        case (ANYENUMOID):
            result = 0;
            break;

        default:
            result = 0;
            break;
    }
    return result;
}

/* get type with highest priority within specific category */
static Oid get_highest_type(TYPCATEGORY category)
{
    Oid result = InvalidOid;

    switch (category) {
        case 'N':
            result = NUMERICOID;
            break;
        case 'S':
            result = TEXTOID;
            break;
        case 'V':
            result = VARBITOID;
            break;
        case 'D':
            /* let it be the original type */
            break;
        case 'T':
            result = INTERVALOID;
            break;
        default:
            break;
    }
    return result;
}

/* @Description: keep the candidate into the list.
 * @in nmatch: number of matche found already.
 * @inout nbestMatch: number of best match found already
 * @in current_candidate: current candidate to be put in.
 * @out last_candidate: last candidate in the list.
 * @out candidates: first candidate in the list.
 * @out ncandidates: number of candidates in the list.
 */
static void keep_candidate(int nmatch, int& nbestMatch, FuncCandidateList current_candidate,
    FuncCandidateList& last_candidate, FuncCandidateList& candidates, int& ncandidates)
{
    if ((nmatch > nbestMatch) || (last_candidate == NULL)) {
        /* take this one as the best choice so far? */
        nbestMatch = nmatch;
        candidates = current_candidate;
        last_candidate = current_candidate;
        ncandidates = 1;
    } else if (nmatch == nbestMatch) {
        /* no worse than the last choice, so keep this one too? */
        last_candidate->next = current_candidate;
        last_candidate = current_candidate;
        ncandidates++;
    }
    /* otherwise, don't bother keeping this one... */
}

/* func_select_candidate()
 *		Given the input argtype array and more than one candidate
 *		for the function, attempt to resolve the conflict.
 *
 * Returns the selected candidate if the conflict can be resolved,
 * otherwise returns NULL.
 *
 * Note that the caller has already determined that there is no candidate
 * exactly matching the input argtypes, and has pruned away any "candidates"
 * that aren't actually coercion-compatible with the input types.
 *
 * This is also used for resolving ambiguous operator references.  Formerly
 * parse_oper.c had its own, essentially duplicate code for the purpose.
 * The following comments (formerly in parse_oper.c) are kept to record some
 * of the history of these heuristics.
 *
 * OLD COMMENTS:
 *
 * This routine is new code, replacing binary_oper_select_candidate()
 * which dates from v4.2/v1.0.x days. It tries very hard to match up
 * operators with types, including allowing type coercions if necessary.
 * The important thing is that the code do as much as possible,
 * while _never_ doing the wrong thing, where "the wrong thing" would
 * be returning an operator when other better choices are available,
 * or returning an operator which is a non-intuitive possibility.
 * - thomas 1998-05-21
 *
 * The comments below came from binary_oper_select_candidate(), and
 * illustrate the issues and choices which are possible:
 * - thomas 1998-05-20
 *
 * current wisdom holds that the default operator should be one in which
 * both operands have the same type (there will only be one such
 * operator)
 *
 * 7.27.93 - I have decided not to do this; it's too hard to justify, and
 * it's easy enough to typecast explicitly - avi
 * [the rest of this routine was commented out since then - ay]
 *
 * 6/23/95 - I don't complete agree with avi. In particular, casting
 * floats is a pain for users. Whatever the rationale behind not doing
 * this is, I need the following special case to work.
 *
 * In the WHERE clause of a query, if a float is specified without
 * quotes, we treat it as float8. I added the float48* operators so
 * that we can operate on float4 and float8. But now we have more than
 * one matching operator if the right arg is unknown (eg. float
 * specified with quotes). This break some stuff in the regression
 * test where there are floats in quotes not properly casted. Below is
 * the solution. In addition to requiring the operator operates on the
 * same type for both operands [as in the code Avi originally
 * commented out], we also require that the operators be equivalent in
 * some sense. (see equivalentOpersAfterPromotion for details.)
 * - ay 6/95
 */
FuncCandidateList func_select_candidate(int nargs, Oid* input_typeids, FuncCandidateList candidates)
{
    FuncCandidateList current_candidate, first_candidate, last_candidate;
    Oid* current_typeids = NULL;
    Oid current_type;
    int i;
    int ncandidates;
    int nbestMatch, nmatch, nunknowns;
    Oid input_base_typeids[FUNC_MAX_ARGS];
    TYPCATEGORY slot_category[FUNC_MAX_ARGS], current_category = TYPCATEGORY_INVALID;
    bool current_is_preferred = false;
    bool slot_has_preferred_type[FUNC_MAX_ARGS];
    bool resolved_unknowns = false;
    int type_priority[FUNC_MAX_ARGS];
    bool different_category = false;

    /* protect local fixed-size arrays */
    if (nargs > FUNC_MAX_ARGS)
        ereport(ERROR,
            (errcode(ERRCODE_TOO_MANY_ARGUMENTS),
                errmsg_plural("cannot pass more than %d argument to a function",
                    "cannot pass more than %d arguments to a function",
                    FUNC_MAX_ARGS,
                    FUNC_MAX_ARGS)));

    /*
     * If any input types are domains, reduce them to their base types. This
     * ensures that we will consider functions on the base type to be "exact
     * matches" in the exact-match heuristic; it also makes it possible to do
     * something useful with the type-category heuristics. Note that this
     * makes it difficult, but not impossible, to use functions declared to
     * take a domain as an input datatype.	Such a function will be selected
     * over the base-type function only if it is an exact match at all
     * argument positions, and so was already chosen by our caller.
     *
     * While we're at it, count the number of unknown-type arguments for use
     * later.
     */
    nunknowns = 0;
    for (i = 0; i < nargs; i++) {
        if (input_typeids[i] != UNKNOWNOID)
            input_base_typeids[i] = getBaseType(input_typeids[i]);
        else {
            /* no need to call getBaseType on UNKNOWNOID */
            input_base_typeids[i] = UNKNOWNOID;
            nunknowns++;
        }
    }

    /*
     * Run through all candidates and keep those with the most matches on
     * exact types. Keep all candidates if none match.
     */
    for (i = 0; i < nargs; i++) /* avoid multiple lookups */
    {
        slot_category[i] = TypeCategory(input_base_typeids[i]);
        /*
         * For C, we should choose numeric + numeric for varchar + int,
         * so we should also admit highest type conversion for operations
         * between different type categories
         */
        if ((u_sess->attr.attr_sql.sql_compatibility == C_FORMAT || u_sess->attr.attr_sql.transform_to_numeric_operators)
            && !different_category && slot_category[i] != TYPCATEGORY_UNKNOWN) {
            if (current_category == TYPCATEGORY_INVALID)
                current_category = slot_category[i];
            else if (slot_category[i] != current_category)
                different_category = true;
        }
    }

    ncandidates = 0;
    nbestMatch = 0;
    last_candidate = NULL;
    for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {

        current_typeids = current_candidate->args;
        nmatch = 0;
        for (i = 0; i < nargs; i++) {
            if (input_base_typeids[i] != UNKNOWNOID &&
                (current_typeids[i] == input_base_typeids[i] ||
                    (different_category && current_typeids[i] == get_highest_type(slot_category[i]))))
                nmatch++;
        }

        keep_candidate(nmatch, nbestMatch, current_candidate, last_candidate, candidates, ncandidates);
    }

    if (last_candidate) /* terminate rebuilt list */
        last_candidate->next = NULL;

    if (ncandidates == 1)
        return candidates;

    /*
     * Still too many candidates? Now look for candidates which have either
     * exact matches or preferred types at the args that will require
     * coercion. (Restriction added in 7.4: preferred type must be of same
     * category as input type; give no preference to cross-category
     * conversions to preferred types.)  Keep all candidates if none match.
     */
    ncandidates = 0;
    nbestMatch = 0;
    last_candidate = NULL;
    for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {
        current_typeids = current_candidate->args;
        nmatch = 0;
        for (i = 0; i < nargs; i++) {
            if (input_base_typeids[i] != UNKNOWNOID) {
                if (current_typeids[i] == input_base_typeids[i] ||
                    IsPreferredType(slot_category[i], current_typeids[i]) ||
                    (different_category && current_typeids[i] == get_highest_type(slot_category[i])))
                    nmatch++;
            }
        }

        keep_candidate(nmatch, nbestMatch, current_candidate, last_candidate, candidates, ncandidates);
    }

    if (last_candidate) /* terminate rebuilt list */
        last_candidate->next = NULL;

    if (ncandidates == 1)
        return candidates;

    /*
     * Still too many candidates?  Try assigning types for the unknown inputs.
     *
     * If there are no unknown inputs, we have no more heuristics that apply,
     * and must fail.
     */

    // Add the following codes dealing with candidates:
    // 1) Try to use the priority,for example , in numeric type family,we have the priority
    // from low to high like int2->int4->int8->numeric->float4->float8
    // 2) Still too many candidates,try to use category priority
    for (i = 0; i < nargs; i++)
        type_priority[i] = GetPriority(input_base_typeids[i]); /* get priority in type family */

    ncandidates = 0;
    nbestMatch = 0;
    last_candidate = NULL;
    for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {

        current_typeids = current_candidate->args;
        nmatch = 0;
        for (i = 0; i < nargs; i++) {

            if (input_base_typeids[i] != UNKNOWNOID) {

                if (current_typeids[i] == input_base_typeids[i] ||
                    (slot_category[i] == TypeCategory(current_typeids[i]) &&
                        GetPriority(current_typeids[i]) > type_priority[i]))
                    nmatch++;
            }
        }

        keep_candidate(nmatch, nbestMatch, current_candidate, last_candidate, candidates, ncandidates);
    }

    if (last_candidate) /* terminate rebuilt list */
        last_candidate->next = NULL;

    if (ncandidates == 1)
        return candidates;

    /*
     * The next step examines each unknown argument position to see if we can
     * determine a "type category" for it.	If any candidate has an input
     * datatype of STRING category, use STRING category (this bias towards
     * STRING is appropriate since unknown-type literals look like strings).
     * Otherwise, if all the candidates agree on the type category of this
     * argument position, use that category.  Otherwise, fail because we
     * cannot determine a category.
     *
     * If we are able to determine a type category, also notice whether any of
     * the candidates takes a preferred datatype within the category.
     *
     * Having completed this examination, remove candidates that accept the
     * wrong category at any unknown position.	Also, if at least one
     * candidate accepted a preferred type at a position, remove candidates
     * that accept non-preferred types.  If just one candidate remains, return
     * that one.  However, if this rule turns out to reject all candidates,
     * keep them all instead.
     */
    resolved_unknowns = false;
    for (i = 0; i < nargs; i++) {
        bool have_conflict = false;

        if (input_base_typeids[i] != UNKNOWNOID)
            continue;
        resolved_unknowns = true; /* assume we can do it */
        slot_category[i] = TYPCATEGORY_INVALID;
        slot_has_preferred_type[i] = false;
        have_conflict = false;
        for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {
            current_typeids = current_candidate->args;
            current_type = current_typeids[i];
            get_type_category_preferred(current_type, &current_category, &current_is_preferred);
            if (slot_category[i] == TYPCATEGORY_INVALID) {
                /* first candidate */
                slot_category[i] = current_category;
                slot_has_preferred_type[i] = current_is_preferred;
            } else if (current_category == slot_category[i]) {
                /* more candidates in same category */
                slot_has_preferred_type[i] = slot_has_preferred_type[i] || current_is_preferred;
            } else {
                /* category conflict! */
                if (current_category == TYPCATEGORY_STRING) {
                    /* STRING always wins if available */
                    slot_category[i] = current_category;
                    slot_has_preferred_type[i] = current_is_preferred;
                } else {
                    /*
                     * Remember conflict, but keep going (might find STRING)
                     */
                    have_conflict = true;
                }
            }
        }
        if (have_conflict && slot_category[i] != TYPCATEGORY_STRING) {
            /* Failed to resolve category conflict at this position */
            resolved_unknowns = false;
            break;
        }
    }

    if (resolved_unknowns) {
        /* Strip non-matching candidates */
        ncandidates = 0;
        first_candidate = candidates;
        last_candidate = NULL;
        for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {
            bool keepit = true;
            current_typeids = current_candidate->args;
            for (i = 0; i < nargs; i++) {
                if (input_base_typeids[i] != UNKNOWNOID)
                    continue;
                current_type = current_typeids[i];
                get_type_category_preferred(current_type, &current_category, &current_is_preferred);
                if (current_category != slot_category[i]) {
                    keepit = false;
                    break;
                }
                if (slot_has_preferred_type[i] && !current_is_preferred) {
                    keepit = false;
                    break;
                }
            }
            if (keepit) {
                /* keep this candidate */
                last_candidate = current_candidate;
                ncandidates++;
            } else {
                /* forget this candidate */
                if (last_candidate)
                    last_candidate->next = current_candidate->next;
                else
                    first_candidate = current_candidate->next;
            }
        }

        /* if we found any matches, restrict our attention to those */
        if (last_candidate) {
            candidates = first_candidate;
            /* terminate rebuilt list */
            last_candidate->next = NULL;
        }

        if (ncandidates == 1)
            return candidates;
    }

    /*
     * Last gasp: if there are both known- and unknown-type inputs, and all
     * the known types are the same, assume the unknown inputs are also that
     * type, and see if that gives us a unique match.  If so, use that match.
     *
     * NOTE: for a binary operator with one unknown and one non-unknown input,
     * we already tried this heuristic in binary_oper_exact().	However, that
     * code only finds exact matches, whereas here we will handle matches that
     * involve coercion, polymorphic type resolution, etc.
     */
    if (nunknowns < nargs) {
        Oid known_type = UNKNOWNOID;

        for (i = 0; i < nargs; i++) {
            if (input_base_typeids[i] == UNKNOWNOID)
                continue;
            if (known_type == UNKNOWNOID) /* first known arg? */
                known_type = input_base_typeids[i];
            else if (known_type != input_base_typeids[i]) {
                /* oops, not all match */
                known_type = UNKNOWNOID;
                break;
            }
        }

        if (known_type != UNKNOWNOID) {
            /* okay, just one known type, apply the heuristic */
            for (i = 0; i < nargs; i++)
                input_base_typeids[i] = known_type;
            ncandidates = 0;
            last_candidate = NULL;
            for (current_candidate = candidates; current_candidate != NULL;
                 current_candidate = current_candidate->next) {
                current_typeids = current_candidate->args;
                if (can_coerce_type(nargs, input_base_typeids, current_typeids, COERCION_IMPLICIT)) {
                    if (++ncandidates > 1)
                        break; /* not unique, give up */
                    last_candidate = current_candidate;
                }
            }
            if (ncandidates == 1) {
                /* successfully identified a unique match */
                last_candidate->next = NULL;
                return last_candidate;
            }
        }
    }

    // still too many candidates, try to use category priority
    if (ncandidates > 1) {
        TYPCATEGORY maxCatalog[FUNC_MAX_ARGS];
        Oid maxTyp[FUNC_MAX_ARGS];
        last_candidate = NULL;
        ncandidates = 0;
        nbestMatch = 0;

        for (i = 0; i < nargs; i++) {
            input_base_typeids[i] = getBaseType(input_typeids[i]);
            slot_category[i] = TypeCategory(input_base_typeids[i]);
            maxCatalog[i] = 'X';
            maxTyp[i] = UNKNOWNOID;
        }

        /* Find out in type category which has the highest priority*/
        for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {
            current_typeids = current_candidate->args;
            for (i = 0; i < nargs; i++) {
                if (GetCategoryPriority(TypeCategory(current_typeids[i])) > GetCategoryPriority(maxCatalog[i])) {
                    maxCatalog[i] = TypeCategory(current_typeids[i]);
                    maxTyp[i] = current_typeids[i];
                } else if ((TypeCategory(current_typeids[i]) == maxCatalog[i]) &&
                           (GetPriority(maxTyp[i]) < GetPriority(current_typeids[i]))) {
                    maxTyp[i] = current_typeids[i];
                }
            }
        }

        /* If the input parameter's priority is higher than the biggest priority, the biggest priority shall prevail*/
        for (i = 0; i < nargs; i++) {
            if ((GetCategoryPriority(slot_category[i]) > GetCategoryPriority(maxCatalog[i])) ||
                (slot_category[i] == 'X')) {

                slot_category[i] = maxCatalog[i];
                input_base_typeids[i] = maxTyp[i];
            } else if ((slot_category[i] == maxCatalog[i]) &&
                       (GetPriority(input_base_typeids[i]) > GetPriority(maxTyp[i]))) {
                input_base_typeids[i] = maxTyp[i];
            } else {
                slot_category[i] = maxCatalog[i];
                input_base_typeids[i] = maxTyp[i];
            }

            type_priority[i] = GetPriority(input_base_typeids[i]);
        }

        for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {

            current_typeids = current_candidate->args;
            nmatch = 0;

            for (i = 0; i < nargs; i++) {
                /* If the type has higher priority is chosen */
                if (GetCategoryPriority(slot_category[i]) < GetCategoryPriority(TypeCategory(current_typeids[i]))) {
                    nmatch++;
                } else if (GetCategoryPriority(slot_category[i]) ==
                           GetCategoryPriority(TypeCategory(current_typeids[i]))) {
                    if (type_priority[i] <= GetPriority(current_typeids[i])) {
                        nmatch++;
                    }
                }
            }

            keep_candidate(nmatch, nbestMatch, current_candidate, last_candidate, candidates, ncandidates);
        }
        if (last_candidate) /* terminate rebuilt list */
            last_candidate->next = NULL;
    }

    if (ncandidates == 1)
        return candidates;

#ifndef ENABLE_MULTIPLE_NODES
    Oid caller_pkg_oid = InvalidOid;
    Oid caller_func_oid = InvalidOid;
    if (OidIsValid(u_sess->plsql_cxt.running_pkg_oid)) {
        caller_pkg_oid = u_sess->plsql_cxt.running_pkg_oid;
    } else if (OidIsValid(u_sess->plsql_cxt.running_func_oid)) {
        caller_func_oid = u_sess->plsql_cxt.running_func_oid;
    } else if (u_sess->plsql_cxt.curr_compile_context != NULL &&
        u_sess->plsql_cxt.curr_compile_context->plpgsql_curr_compile_package != NULL) {
        caller_pkg_oid = u_sess->plsql_cxt.curr_compile_context->plpgsql_curr_compile_package->pkg_oid;
    } else if (u_sess->plsql_cxt.curr_compile_context != NULL &&
        u_sess->plsql_cxt.curr_compile_context->plpgsql_curr_compile != NULL) {
        caller_func_oid = u_sess->plsql_cxt.curr_compile_context->plpgsql_curr_compile->fn_oid;
    }
    nbestMatch = 0;
    ncandidates = 0;
    last_candidate = NULL;
    for (current_candidate = candidates; current_candidate != NULL; current_candidate = current_candidate->next) {
        nmatch = 0;
        if (caller_func_oid) {
            if (current_candidate->funcOid == caller_func_oid) {
                nmatch++;
            }
        } else if (current_candidate->packageOid == caller_pkg_oid) {
            nmatch++;
        }
        keep_candidate(nmatch, nbestMatch, current_candidate, last_candidate, candidates, ncandidates);
    }
    if (last_candidate) /* terminate rebuilt list */
        last_candidate->next = NULL;
    if (ncandidates == 1)
        return candidates;
#endif

    return NULL; /* failed to select a best candidate */
} /* func_select_candidate() */


/*
 * sort_candidate_func_list
 *
 * sort the candidate functions by function's all param num.
 */
FuncCandidateList sort_candidate_func_list(FuncCandidateList oldCandidates)
{
    if (oldCandidates == NULL || oldCandidates->next == NULL) {
        return oldCandidates;
    }

    FuncCandidateList cur = oldCandidates;
    int size = 0;
    while (cur) {
        size++;
        cur = cur->next;
    }

    cur = oldCandidates;
    FuncCandidateList* candidates = (FuncCandidateList*)palloc0(sizeof(FuncCandidateList) * size);
    int index = 0;
    while (cur) {
        candidates[index++] = cur;
        cur = cur->next;
    }

    int i, j;
    FuncCandidateList temp;
    for (i = 0; i < index - 1; i++) {
        for (j = 0; j < index - (i + 1); j++) {
            if (candidates[j]->allArgNum > candidates[j + 1]->allArgNum) {
                temp = candidates[j];
                candidates[j] = candidates[j + 1];
                candidates[j + 1] = temp;
            }
        }
    }

    FuncCandidateList sorted_candidates = candidates[0];
    FuncCandidateList next_candidates = sorted_candidates;
    for (i = 0; i < index; i++) {
        candidates[i]->next = NULL;
        if (i != 0) {
            next_candidates->next = candidates[i];
            next_candidates = next_candidates->next;
        }
    }

    pfree(candidates);
    return sorted_candidates;
}

/*
 * getRecordOrRowBaseType
 * Get base type of a record or row(tableof) type. Just like getBaseType.
 */
Oid getRecordOrTableOfBaseType(Oid typid)
{
    /* Prechecke if typid is oid of record or tableof */
    HeapTuple tup;
    Form_pg_type typTup;
    Oid typtype = InvalidOid;
    Oid lastType = InvalidOid;
    tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
    if (!HeapTupleIsValid(tup)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for type %u", typid)));
    }
    typTup = (Form_pg_type)GETSTRUCT(tup);
    /* Return itself if not a subtype of record or tableof */
    if ((typTup->typtype != TYPTYPE_COMPOSITE && typTup->typtype!= TYPTYPE_TABLEOF) ||
        (typTup->typbasetype == InvalidOid)) {
        ReleaseSysCache(tup);
        return typid;
    }
    lastType = typid;
    typid = typTup->typbasetype;
    typtype = typTup->typtype;
    ReleaseSysCache(tup);
    /*
     * We loop to find the bottom base type in a stack of record or tableof.
     */
    for (;;) {
        tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
        if (!HeapTupleIsValid(tup)) {
            ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for type %u", typid)));
        }
        typTup = (Form_pg_type)GETSTRUCT(tup);
        /* Subtype's typtype should be same as basetype */
        if (typTup->typtype != typtype) {
            ReleaseSysCache(tup);
            ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("typtype of subtype %u is not same as basetype %u.", lastType, typid)));
        }
        if (typTup->typbasetype == InvalidOid) {
            /* Don't have basetype so it is basetype */
            ReleaseSysCache(tup);
            break;
        }
        lastType = typid;
        typid = typTup->typbasetype;
        ReleaseSysCache(tup);
    }
    return typid;
}

/*
 * ProcessRecordOrTableOfSubtype
 * Transform source args and target args to their basetype when they are record or tableof
 * and have same typtype.
 */
void ProcessRecordOrTableOfSubtype(Oid* sourceArgtypes, Oid* targetArgtypes, int nargs)
{
    Oid srcBasetype = InvalidOid;
    Oid tarBasetype = InvalidOid;
    for (int argIndex = 0; argIndex < nargs; argIndex++) {
        Oid srcTypeid = sourceArgtypes[argIndex];
        Oid tarTypeid = targetArgtypes[argIndex];
        /* If typtype of source type and target type is different, it can't transform to same basetype */
        if (!(get_typtype(srcTypeid) == TYPTYPE_COMPOSITE && get_typtype(tarTypeid) == TYPTYPE_COMPOSITE) &&
            !(get_typtype(srcTypeid) == TYPTYPE_TABLEOF && get_typtype(tarTypeid) == TYPTYPE_TABLEOF)) {
                break;
        }
        if (srcTypeid == tarTypeid) {
            continue;
        }
        srcBasetype = getRecordOrTableOfBaseType(srcTypeid);
        tarBasetype = getRecordOrTableOfBaseType(tarTypeid);
        if (srcBasetype != InvalidOid && srcBasetype == tarBasetype) {
            sourceArgtypes[argIndex] = srcBasetype;
            targetArgtypes[argIndex] = tarBasetype;
            continue;
        }
    }
}

/* func_get_detail()
 *
 * Find the named function in the system catalogs.
 *
 * Attempt to find the named function in the system catalogs with
 * arguments exactly as specified, so that the normal case (exact match)
 * is as quick as possible.
 *
 * If an exact match isn't found:
 *	1) check for possible interpretation as a type coercion request
 *	2) apply the ambiguous-function resolution rules
 *
 * Return values *funcid through *true_typeids receive info about the function.
 * If argdefaults isn't NULL, *argdefaults receives a list of any default
 * argument expressions that need to be added to the given arguments.
 *
 * When processing a named- or mixed-notation call (ie, fargnames isn't NIL),
 * the returned true_typeids and argdefaults are ordered according to the
 * call's argument ordering: first any positional arguments, then the named
 * arguments, then defaulted arguments (if needed and allowed by
 * expand_defaults).  Some care is needed if this information is to be compared
 * to the function's pg_proc entry, but in practice the caller can usually
 * just work with the call's argument ordering.
 *
 * We rely primarily on fargnames/nargs/argtypes as the argument description.
 * The actual expression node list is passed in fargs so that we can check
 * for type coercion of a constant.  Some callers pass fargs == NIL indicating
 * they don't need that check made.  Note also that when fargnames isn't NIL,
 * the fargs list must be passed if the caller wants actual argument position
 * information to be returned into the NamedArgExpr nodes.
 */
FuncDetailCode func_get_detail(List* funcname, List* fargs, List* fargnames, int nargs, Oid* argtypes,
    bool expand_variadic, bool expand_defaults, Oid* funcid, /* return value */
    Oid* rettype,                                            /* return value */
    bool* retset,                                            /* return value */
    int* nvargs,                                             /* return value */
    Oid* vatype,                                             /* return value */
    Oid** true_typeids,                                      /* return value */
    List** argdefaults, bool call_func,                      /* optional return value */
    Oid* refSynOid, int* rettype_orig)
{
    FuncCandidateList raw_candidates = NULL;
    FuncCandidateList all_candidates = NULL;
    FuncCandidateList best_candidate;

    /* initialize output arguments to silence compiler warnings */
    *funcid = InvalidOid;
    *rettype = InvalidOid;
    *retset = false;
    *nvargs = 0;
    *true_typeids = NULL;
    if (argdefaults != NULL)
        *argdefaults = NIL;

    if (refSynOid != NULL) {
        *refSynOid = InvalidOid;
    }

#ifndef ENABLE_MULTIPLE_NODES
    if (enable_out_param_override()) {
        /* For A compatiablity, CALL statement only can invoke Procedure in SQL or Function in PLSQL, */ 
        /* and SELECT statement can only invoke Function. but now it does't distinguish for compatible with the old code.*/
        raw_candidates = FuncnameGetCandidates(funcname, nargs, fargnames, expand_variadic, expand_defaults, false, true, PROKIND_UNKNOWN);
    } else {
        /* Get list of possible candidates from namespace search */
        raw_candidates = FuncnameGetCandidates(funcname, nargs, fargnames, expand_variadic, expand_defaults, false);

        /* Get list of possible candidates from namespace search including proallargtypes for package function */
        if (call_func && IsPackageFunction(funcname)) {
            all_candidates =
                FuncnameGetCandidates(funcname, nargs, fargnames, expand_variadic, expand_defaults, false, true);
            if (all_candidates != NULL) {
                if (raw_candidates != NULL) {
                    best_candidate = raw_candidates;
                    while (best_candidate && best_candidate->next) {
                        best_candidate = best_candidate->next;
                    }

                    best_candidate->next = all_candidates;
                } else {
                    raw_candidates = all_candidates;
                }
            }
        }
    }

    raw_candidates = sort_candidate_func_list(raw_candidates);
#else
    /* Get list of possible candidates from namespace search */
    raw_candidates = FuncnameGetCandidates(funcname, nargs, fargnames, expand_variadic, expand_defaults, false);

    /* Get list of possible candidates from namespace search including proallargtypes for package function */
    if (call_func && IsPackageFunction(funcname)) {
        all_candidates =
            FuncnameGetCandidates(funcname, nargs, fargnames, expand_variadic, expand_defaults, false, true);
        if (all_candidates != NULL) {
            if (raw_candidates != NULL) {
                best_candidate = raw_candidates;
                while (best_candidate && best_candidate->next) {
                    best_candidate = best_candidate->next;
                }

                best_candidate->next = all_candidates;
            } else {
                raw_candidates = all_candidates;
            }
        }
    }
#endif


    /*
     * Quickly check if there is an exact match to the input datatypes (there
     * can be only one)
     */
    for (best_candidate = raw_candidates; best_candidate != NULL; best_candidate = best_candidate->next) {
        ProcessRecordOrTableOfSubtype(argtypes, best_candidate->args, nargs);
        if (memcmp(argtypes, best_candidate->args, nargs * sizeof(Oid)) == 0) {
            break;
        }

        if (CheckTableOfType(best_candidate, argtypes)) {
            break;
        }
    }

    if (best_candidate == NULL) {
        /*
         * If we didn't find an exact match, next consider the possibility
         * that this is really a type-coercion request: a single-argument
         * function call where the function name is a type name.  If so, and
         * if the coercion path is RELABELTYPE or COERCEVIAIO, then go ahead
         * and treat the "function call" as a coercion.
         *
         * This interpretation needs to be given higher priority than
         * interpretations involving a type coercion followed by a function
         * call, otherwise we can produce surprising results. For example, we
         * want "text(varchar)" to be interpreted as a simple coercion, not as
         * "text(name(varchar))" which the code below this point is entirely
         * capable of selecting.
         *
         * We also treat a coercion of a previously-unknown-type literal
         * constant to a specific type this way.
         *
         * The reason we reject COERCION_PATH_FUNC here is that we expect the
         * cast implementation function to be named after the target type.
         * Thus the function will be found by normal lookup if appropriate.
         *
         * The reason we reject COERCION_PATH_ARRAYCOERCE is mainly that you
         * can't write "foo[] (something)" as a function call.  In theory
         * someone might want to invoke it as "_foo (something)" but we have
         * never supported that historically, so we can insist that people
         * write it as a normal cast instead.
         *
         * We also reject the specific case of COERCEVIAIO for a composite
         * source type and a string-category target type.  This is a case that
         * find_coercion_pathway() allows by default, but experience has shown
         * that it's too commonly invoked by mistake.  So, again, insist that
         * people use cast syntax if they want to do that.
         *
         * NB: it's important that this code does not exceed what coerce_type
         * can do, because the caller will try to apply coerce_type if we
         * return FUNCDETAIL_COERCION.	If we return that result for something
         * coerce_type can't handle, we'll cause infinite recursion between
         * this module and coerce_type!
         */
        if (nargs == 1 && fargs != NIL && fargnames == NIL) {
            Oid targetType = FuncNameAsType(funcname);

            if (OidIsValid(targetType)) {
                Oid sourceType = argtypes[0];
                Node* arg1 = (Node*)linitial(fargs);
                bool iscoercion = false;

                if (sourceType == UNKNOWNOID && IsA(arg1, Const)) {
                    /* always treat typename('literal') as coercion */
                    iscoercion = true;
                } else {
                    CoercionPathType cpathtype;
                    Oid cfuncid;

                    cpathtype = find_coercion_pathway(targetType, sourceType, COERCION_EXPLICIT, &cfuncid);
                    switch (cpathtype) {
                        case COERCION_PATH_RELABELTYPE:
                            iscoercion = true;
                            break;
                        case COERCION_PATH_COERCEVIAIO:
                            if ((sourceType == RECORDOID || ISCOMPLEX(sourceType)) &&
                                TypeCategory(targetType) == TYPCATEGORY_STRING)
                                iscoercion = false;
                            else
                                iscoercion = true;
                            break;
                        default:
                            iscoercion = false;
                            break;
                    }
                }

                if (iscoercion) {
                    /* Treat it as a type coercion */
                    *funcid = InvalidOid;
                    *rettype = targetType;
                    *retset = false;
                    *nvargs = 0;
                    *true_typeids = argtypes;
                    return FUNCDETAIL_COERCION;
                }
            }
        }

        /*
         * didn't find an exact match, so now try to match up candidates...
         */
        if (raw_candidates != NULL) {
            FuncCandidateList current_candidates;
            int ncandidates;

            ncandidates = func_match_argtypes(nargs, argtypes, raw_candidates, &current_candidates);

            /* one match only? then run with it... */
            if (ncandidates == 1)
                best_candidate = current_candidates;

            /*
             * multiple candidates? then better decide or throw an error...
             */
            else if (ncandidates > 1) {
                best_candidate = func_select_candidate(nargs, argtypes, current_candidates);
                /*
                 * If we were able to choose a best candidate, we're done.
                 * Otherwise, ambiguous function call.
                 */
                if (!best_candidate)
                    return FUNCDETAIL_MULTIPLE;
            }
        }
    }

    if (best_candidate) {
        HeapTuple ftup;
        Form_pg_proc pform;
        FuncDetailCode result;

        /*
         * If processing named args or expanding variadics or defaults, the
         * "best candidate" might represent multiple equivalently good
         * functions; treat this case as ambiguous.
         */
        if (!OidIsValid(best_candidate->oid))
            return FUNCDETAIL_MULTIPLE;

        /*
         * We disallow VARIADIC with named arguments unless the last argument
         * (the one with VARIADIC attached) actually matched the variadic
         * parameter.  This is mere pedantry, really, but some folks insisted.
         */
        if (fargnames != NIL && !expand_variadic && nargs > 0 && best_candidate->argnumbers != NULL &&
            best_candidate->argnumbers[nargs - 1] != nargs - 1)
            return FUNCDETAIL_NOTFOUND;

        *funcid = best_candidate->oid;
        *nvargs = best_candidate->nvargs;
        *true_typeids = best_candidate->args;

        /* Store OID of referenced synonym, if needed. */
        if (refSynOid != NULL) {
            *refSynOid = best_candidate->refSynOid;
        }

        /*
         * If processing named args, return actual argument positions into
         * NamedArgExpr nodes in the fargs list.  This is a bit ugly but not
         * worth the extra notation needed to do it differently.
         */
        if (best_candidate->argnumbers != NULL) {
            int i = 0;
            ListCell* lc = NULL;

            foreach (lc, fargs) {
                NamedArgExpr* na = (NamedArgExpr*)lfirst(lc);

                if (IsA(na, NamedArgExpr))
                    na->argnumber = best_candidate->argnumbers[i];
                i++;
            }
        }

        ftup = SearchSysCache1(PROCOID, ObjectIdGetDatum(best_candidate->oid));
        if (!HeapTupleIsValid(ftup)) /* should not happen */
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for function %u", best_candidate->oid)));
        pform = (Form_pg_proc)GETSTRUCT(ftup);
        *rettype = pform->prorettype;
        if (IsClientLogicType(*rettype) && rettype_orig) {
            HeapTuple gstup = SearchSysCache1(GSCLPROCID, ObjectIdGetDatum(best_candidate->oid));
            if (!HeapTupleIsValid(gstup)) /* should not happen */
                ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for function %u", best_candidate->oid)));
            Form_gs_encrypted_proc gsform = (Form_gs_encrypted_proc)GETSTRUCT(gstup);
            *rettype_orig = gsform->prorettype_orig;
            ReleaseSysCache(gstup);
        }
        *retset = pform->proretset;
        *vatype = pform->provariadic;
        /* fetch default args if caller wants 'em */
        if (argdefaults != NULL && best_candidate->ndargs > 0) {
            /* shouldn't happen, FuncnameGetCandidates messed up */
            if (best_candidate->ndargs > pform->pronargdefaults)
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION), errmodule(MOD_OPT), errmsg("not enough default arguments")));

            bool defaultValid = true;
            *argdefaults = GetDefaultVale(*funcid, best_candidate->argnumbers, best_candidate->ndargs, &defaultValid);
            if (!defaultValid) {
                ereport(DEBUG3,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                        errmodule(MOD_OPT), errmsg("default arguments pos is not right.")));
                return FUNCDETAIL_NOTFOUND;
            }
        }
        if (pform->proisagg)
            result = FUNCDETAIL_AGGREGATE;
        else if (pform->proiswindow)
            result = FUNCDETAIL_WINDOWFUNC;
        else
            result = FUNCDETAIL_NORMAL;
        ReleaseSysCache(ftup);
        return result;
    }

    return FUNCDETAIL_NOTFOUND;
}


/*
 * unify_hypothetical_args()
 *
 * Ensure that each hypothetical direct argument of a hypothetical-set
 * aggregate has the same type as the corresponding aggregated argument.
 * Modify the expressions in the fargs list, if necessary, and update
 * actual_arg_types[].
 *
 * If the agg declared its args non-ANY (even ANYELEMENT), we need only a
 * sanity check that the declared types match; make_fn_arguments will coerce
 * the actual arguments to match the declared ones.  But if the declaration
 * is ANY, nothing will happen in make_fn_arguments, so we need to fix any
 * mismatch here.  We use the same type resolution logic as UNION etc.
 */
static void unify_hypothetical_args(ParseState *pstate,
                                    List *fargs,
                                    int numAggregatedArgs,
                                    Oid *actual_arg_types,
                                    Oid *declared_arg_types)
{
    Node        *args[FUNC_MAX_ARGS];
    int         numDirectArgs, numNonHypotheticalArgs;
    int         i;
    ListCell    *lc;

    numDirectArgs = list_length(fargs) - numAggregatedArgs;
    numNonHypotheticalArgs = numDirectArgs - numAggregatedArgs;
    /* safety check (should only trigger with a misdeclared agg) */
    if (numNonHypotheticalArgs < 0)
        elog(ERROR, "incorrect number of arguments to hypothetical-set aggregate");

    /* Deconstruct fargs into an array for ease of subscripting */
    i = 0;
    foreach(lc, fargs) {
        args[i++] = (Node *) lfirst(lc);
    }

    /* Check each hypothetical arg and corresponding aggregated arg */
    for (i = numNonHypotheticalArgs; i < numDirectArgs; i++) {
        int			aargpos = numDirectArgs + (i - numNonHypotheticalArgs);
        Oid			commontype;

        /* A mismatch means AggregateCreate didn't check properly ... */
        if (declared_arg_types[i] != declared_arg_types[aargpos])
            elog(ERROR, "hypothetical-set aggregate has inconsistent declared argument types");

        /* No need to unify if make_fn_arguments will coerce */
        if (declared_arg_types[i] != ANYOID)
            continue;

        /*
         * Select common type, giving preference to the aggregated argument's
         * type (we'd rather coerce the direct argument once than coerce all
         * the aggregated values).
         * 
         * In A compatibility, we always coerce the direct argument to the type 
         * of the aggregated values. So that even if they are not in the same 
         * type category, we can still do the coercing.
         */
        if (DB_IS_CMPT(A_FORMAT)) {
            commontype = getBaseType(exprType(args[aargpos]));
        } else {
            commontype = select_common_type(pstate,
                                            list_make2(args[aargpos], args[i]),
                                            "WITHIN GROUP",
                                            NULL);
        }

        /*
         * Perform the coercions.  We don't need to worry about NamedArgExprs
         * here because they aren't supported with aggregates.
         */
        args[i] = coerce_type(pstate,
                              args[i],
                              actual_arg_types[i],
                              commontype, -1,
                              COERCION_IMPLICIT,
                              COERCE_IMPLICIT_CAST,
                              NULL,
                              NULL,
                              -1);
        actual_arg_types[i] = commontype;
        args[aargpos] = coerce_type(pstate,
                                    args[aargpos],
                                    actual_arg_types[aargpos],
                                    commontype, -1,
                                    COERCION_IMPLICIT,
                                    COERCE_IMPLICIT_CAST,
                                    NULL,
                                    NULL,
                                    -1);
        actual_arg_types[aargpos] = commontype;
    }

    /* Reconstruct fargs from array */
    i = 0;
    foreach(lc, fargs) {
        lfirst(lc) = args[i++];
    }
}

/*
 * make_fn_arguments()
 *
 * Given the actual argument expressions for a function, and the desired
 * input types for the function, add any necessary typecasting to the
 * expression tree.  Caller should already have verified that casting is
 * allowed.
 *
 * Caution: given argument list is modified in-place.
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
void make_fn_arguments(ParseState* pstate, List* fargs, Oid* actual_arg_types, Oid* declared_arg_types)
{
    ListCell* current_fargs = NULL;
    int i = 0;

    foreach (current_fargs, fargs) {
        /* types don't match? then force coercion using a function call... */
        if (actual_arg_types[i] != declared_arg_types[i]) {
            Node* node = (Node*)lfirst(current_fargs);

            /*
             * If arg is a NamedArgExpr, coerce its input expr instead --- we
             * want the NamedArgExpr to stay at the top level of the list.
             */
            if (IsA(node, NamedArgExpr)) {
                NamedArgExpr* na = (NamedArgExpr*)node;

                node = coerce_type(pstate,
                    (Node*)na->arg,
                    actual_arg_types[i],
                    declared_arg_types[i],
                    -1,
                    COERCION_IMPLICIT,
                    COERCE_IMPLICIT_CAST,
                    NULL,
                    NULL,
                    -1);
                na->arg = (Expr*)node;
            } else if (IsA(node, UserVar)) {
                UserVar* uvar = (UserVar*)node;

                node = coerce_type(pstate,
                    (Node*)uvar->value,
                    actual_arg_types[i],
                    declared_arg_types[i],
                    -1,
                    COERCION_IMPLICIT,
                    COERCE_IMPLICIT_CAST,
                    NULL,
                    NULL,
                    -1);
                uvar->value = (Expr*)node;
            } else {
                node = coerce_type(pstate,
                    node,
                    actual_arg_types[i],
                    declared_arg_types[i],
                    -1,
                    COERCION_IMPLICIT,
                    COERCE_IMPLICIT_CAST,
                    NULL,
                    NULL,
                    -1);
                lfirst(current_fargs) = node;
            }
        }
        i++;
    }
}

/*
 * FuncNameAsType -
 *	  convenience routine to see if a function name matches a type name
 *
 * Returns the OID of the matching type, or InvalidOid if none.  We ignore
 * shell types and complex types.
 */
static Oid FuncNameAsType(List* funcname)
{
    Oid result;
    Type typtup;

    /*
     * temp_ok=false protects the <refsect1 id="sql-createfunction-security">
     * contract for writing SECURITY DEFINER functions safely.
     */
#ifdef ENABLE_MULTIPLE_NODES
    typtup = LookupTypeName(NULL, makeTypeNameFromNameList(funcname), NULL);
#else
    typtup = LookupTypeNameExtended(NULL, makeTypeNameFromNameList(funcname), NULL, false);
#endif
    if (typtup == NULL)
        return InvalidOid;

    if (((Form_pg_type)GETSTRUCT(typtup))->typisdefined && !OidIsValid(typeTypeRelid(typtup)))
        result = typeTypeId(typtup);
    else
        result = InvalidOid;

    ReleaseSysCache(typtup);
    return result;
}

/*
 * ParseComplexProjection -
 *	  handles function calls with a single argument that is of complex type.
 *	  If the function call is actually a column projection, return a suitably
 *	  transformed expression tree.	If not, return NULL.
 */
static Node* ParseComplexProjection(ParseState* pstate, char* funcname, Node* first_arg, int location)
{
    TupleDesc tupdesc;
    int i;

    /*
     * Special case for whole-row Vars so that we can resolve (foo.*).bar even
     * when foo is a reference to a subselect, join, or RECORD function. A
     * bonus is that we avoid generating an unnecessary FieldSelect; our
     * result can omit the whole-row Var and just be a Var for the selected
     * field.
     *
     * This case could be handled by expandRecordVariable, but it's more
     * efficient to do it this way when possible.
     */
    if (IsA(first_arg, Var) && ((Var*)first_arg)->varattno == InvalidAttrNumber) {
        RangeTblEntry* rte = NULL;

        rte = GetRTEByRangeTablePosn(pstate, ((Var*)first_arg)->varno, ((Var*)first_arg)->varlevelsup);
        /* Return a Var if funcname matches a column, else NULL */
        return scanRTEForColumn(pstate, rte, funcname, location);
    }

    /*
     * Else do it the hard way with get_expr_result_type().
     *
     * If it's a Var of type RECORD, we have to work even harder: we have to
     * find what the Var refers to, and pass that to get_expr_result_type.
     * That task is handled by expandRecordVariable().
     */
    if (IsA(first_arg, Var) && ((Var*)first_arg)->vartype == RECORDOID)
        tupdesc = expandRecordVariable(pstate, (Var*)first_arg, 0);
    else if (get_expr_result_type(first_arg, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        return NULL; /* unresolvable RECORD type */
    AssertEreport(tupdesc, MOD_OPT, "");

    for (i = 0; i < tupdesc->natts; i++) {
        Form_pg_attribute att = &tupdesc->attrs[i];

        if (strcmp(funcname, NameStr(att->attname)) == 0 && !att->attisdropped) {
            /* Success, so generate a FieldSelect expression */
            FieldSelect* fselect = makeNode(FieldSelect);

            fselect->arg = (Expr*)first_arg;
            fselect->fieldnum = i + 1;
            fselect->resulttype = att->atttypid;
            fselect->resulttypmod = att->atttypmod;
            /* save attribute's collation for parse_collate.c */
            fselect->resultcollid = att->attcollation;
            return (Node*)fselect;
        }
    }

    return NULL; /* funcname does not match any column */
}

/*
 * funcname_signature_string
 *		Build a string representing a function name, including arg types.
 *		The result is something like "foo(integer)".
 *
 * If argnames isn't NIL, it is a list of C strings representing the actual
 * arg names for the last N arguments.	This must be considered part of the
 * function signature too, when dealing with named-notation function calls.
 *
 * This is typically used in the construction of function-not-found error
 * messages.
 */
const char* funcname_signature_string(const char* funcname, int nargs, List* argnames, const Oid* argtypes)
{
    StringInfoData argbuf;
    int numposargs;
    ListCell* lc = NULL;
    int i;

    initStringInfo(&argbuf);

    appendStringInfo(&argbuf, "%s(", funcname);

    numposargs = nargs - list_length(argnames);
    lc = list_head(argnames);

    for (i = 0; i < nargs; i++) {
        if (i)
            appendStringInfoString(&argbuf, ", ");
        if (i >= numposargs) {
            appendStringInfo(&argbuf, "%s := ", (char*)lfirst(lc));
            lc = lnext(lc);
        }
        appendStringInfoString(&argbuf, format_type_be(argtypes[i]));
    }

    appendStringInfoChar(&argbuf, ')');

    return argbuf.data; /* return palloc'd string buffer */
}

/*
 * func_signature_string
 *		As above, but function name is passed as a qualified name list.
 */
const char* func_signature_string(List* funcname, int nargs, List* argnames, const Oid* argtypes)
{
    return funcname_signature_string(NameListToString(funcname), nargs, argnames, argtypes);
}

/*
 * LookupFuncName
 *		Given a possibly-qualified function name and a set of argument types,
 *		look up the function.
 *
 * If the function name is not schema-qualified, it is sought in the current
 * namespace search path.
 *
 * If the function is not found, we return InvalidOid if noError is true,
 * else raise an error.
 */
Oid LookupFuncName(List* funcname, int nargs, const Oid* argtypes, bool noError)
{
    FuncCandidateList clist;

    clist = FuncnameGetCandidates(funcname, nargs, NIL, false, false, false);

    while (clist) {
        /* if argtype is CL type replace it with original type */
        for (int i = 0; i < nargs; i++) {
            if (IsClientLogicType(clist->args[i]) && !u_sess->attr.attr_common.IsInplaceUpgrade) {
                clist->args[i] = cl_get_input_param_original_type(clist->oid, i);
            }
        }
        if (memcmp(argtypes, clist->args, nargs * sizeof(Oid)) == 0 && OidIsValid(clist->oid))
            return clist->oid;
        clist = clist->next;
    }

    if (!noError)
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_FUNCTION),
                errmsg("function %s does not exist", func_signature_string(funcname, nargs, NIL, argtypes))));

    return InvalidOid;
}

/*
 * LookupTypeNameOid
 *		Convenience routine to look up a type, silently accepting shell types
 */
Oid LookupTypeNameOid(const TypeName* typname)
{
    Oid result;
    Type typtup;

    typtup = LookupTypeName(NULL, typname, NULL);
    if (typtup == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("type \"%s\" does not exist", TypeNameToString(typname))));
    result = typeTypeId(typtup);
    ReleaseSysCache(typtup);
    return result;
}

/*
 * LookupFuncNameTypeNames
 *		Like LookupFuncName, but the argument types are specified by a
 *		list of TypeName nodes.
 */
Oid LookupFuncNameTypeNames(List* funcname, List* argtypes, bool noError)
{
    Oid argoids[FUNC_MAX_ARGS];
    int argcount;
    int i;
    ListCell* args_item = NULL;

    argcount = list_length(argtypes);
    if (argcount > FUNC_MAX_ARGS)
        ereport(ERROR,
            (errcode(ERRCODE_TOO_MANY_ARGUMENTS),
                errmsg_plural("functions cannot have more than %d argument",
                    "functions cannot have more than %d arguments",
                    FUNC_MAX_ARGS,
                    FUNC_MAX_ARGS)));

    args_item = list_head(argtypes);
    for (i = 0; i < argcount; i++) {
        TypeName* t = (TypeName*)lfirst(args_item);

        argoids[i] = LookupTypeNameOid(t);
        args_item = lnext(args_item);
    }

    return LookupFuncName(funcname, argcount, argoids, noError);
}

// Find the function by name and with optional argument
//
// This function is like LookupFuncNameTypeNames, used in
// drop function, when no argument specified, compatible
// with A db
Oid LookupFuncNameOptTypeNames(List* funcname, List* argtypes, bool noError)
{
    FuncCandidateList cnddt_func_list = NULL;

    if (argtypes != NIL)
        return LookupFuncNameTypeNames(funcname, argtypes, noError);

    /* If argument is not specified (compitable with A db)*/
    cnddt_func_list = FuncnameGetCandidates(funcname, -1, NULL, false, false, false);
    if (cnddt_func_list == NULL) {
        if (!noError)
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("function %s does not exist", NameListToString(funcname))));

        return InvalidOid;
    }

    /*
     * IF there is only one candidate then return it,
     * else return the zero argument one.
     */
    if (cnddt_func_list->next) {
        while (cnddt_func_list) {
            if (0 == cnddt_func_list->nargs)
                break;
            cnddt_func_list = cnddt_func_list->next;
        }

        if (cnddt_func_list == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("function %s asks parameters", NameListToString(funcname))));
    }

    if (OidIsValid(cnddt_func_list->oid)) {
        return cnddt_func_list->oid;
    }
    /* In concurrence, can appear invalid Oid.*/
    else {
        if (!noError)
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("function %s does not exist", NameListToString(funcname))));

        return InvalidOid;
    }
}

/*
 * LookupAggNameTypeNames
 *		Find an aggregate function given a name and list of TypeName nodes.
 *
 * This is almost like LookupFuncNameTypeNames, but the error messages refer
 * to aggregates rather than plain functions, and we verify that the found
 * function really is an aggregate.
 */
Oid LookupAggNameTypeNames(List* aggname, List* argtypes, bool noError)
{
    Oid argoids[FUNC_MAX_ARGS];
    int argcount;
    int i;
    ListCell* lc = NULL;
    Oid oid;
    HeapTuple ftup;
    Form_pg_proc pform;

    argcount = list_length(argtypes);
    if (argcount > FUNC_MAX_ARGS)
        ereport(ERROR,
            (errcode(ERRCODE_TOO_MANY_ARGUMENTS),
                errmsg_plural("functions cannot have more than %d argument",
                    "functions cannot have more than %d arguments",
                    FUNC_MAX_ARGS,
                    FUNC_MAX_ARGS)));

    i = 0;
    foreach (lc, argtypes) {
        TypeName* t = (TypeName*)lfirst(lc);

        argoids[i] = LookupTypeNameOid(t);
        i++;
    }

    oid = LookupFuncName(aggname, argcount, argoids, true);

    if (!OidIsValid(oid)) {
        if (noError)
            return InvalidOid;
        if (argcount == 0)
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("aggregate %s(*) does not exist", NameListToString(aggname))));
        else
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("aggregate %s does not exist", func_signature_string(aggname, argcount, NIL, argoids))));
    }

    /* Make sure it's an aggregate */
    ftup = SearchSysCache1(PROCOID, ObjectIdGetDatum(oid));
    if (!HeapTupleIsValid(ftup)) /* should not happen */
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for function %u", oid)));

    pform = (Form_pg_proc)GETSTRUCT(ftup);
    if (!pform->proisagg) {
        ReleaseSysCache(ftup);
        if (noError)
            return InvalidOid;
        /* we do not use the (*) notation for functions... */
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("function %s is not an aggregate", func_signature_string(aggname, argcount, NIL, argoids))));
    }

    ReleaseSysCache(ftup);

    return oid;
}

// fetch default args if caller wants 'em
static List* GetDefaultVale(Oid funcoid, const int* argnumbers, int ndargs, bool* defaultValid)
{
    HeapTuple tuple;
    Form_pg_proc formproc;
    int pronallargs = 0;
    int pronargdefaults = 0;
    Oid* argtypes = NULL;
    char** argnames = NULL;
    char* argmodes = NULL;
    List* defaults = NIL;
    Datum defargposdatum;
    int2vector* defaultargpos = NULL;
    Datum proargdefaults;
    char* defaultsstr = NULL;
    int pronargs = 0;
    bool isnull = false;
    bool found = true;
    int* defaultpos = NULL;
    int counter1 = 0;
    int counter2 = 0;
    int pos = 0;

    if (0 == ndargs)
        return defaults;

    tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(
            ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("cache lookup failed for function \"%u\"", funcoid)));
        return defaults;
    }

    pronallargs = get_func_arg_info(tuple, &argtypes, &argnames, &argmodes);
    formproc = (Form_pg_proc)GETSTRUCT(tuple);
    pronargs = formproc->pronargs;
    pronargdefaults = formproc->pronargdefaults;
    proargdefaults = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proargdefaults, &isnull);
    AssertEreport(!isnull, MOD_OPT, "");
    defaultsstr = TextDatumGetCString(proargdefaults);
    defaults = (List*)stringToNode(defaultsstr);
    AssertEreport(IsA(defaults, List), MOD_OPT, "");
    pfree_ext(defaultsstr);

    if (pronargs <= FUNC_MAX_ARGS_INROW) {
        defargposdatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prodefaultargpos, &isnull);
        AssertEreport(!isnull, MOD_OPT, "");
        defaultargpos = (int2vector*)DatumGetPointer(defargposdatum);
    } else {
        defargposdatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prodefaultargposext, &isnull);
        AssertEreport(!isnull, MOD_OPT, "");
        defaultargpos = (int2vector*)PG_DETOAST_DATUM(defargposdatum);
    }

    AssertEreport(pronargs >= ndargs, MOD_OPT, "");
    AssertEreport(pronargdefaults >= ndargs, MOD_OPT, "");
    AssertEreport(pronallargs >= pronargs, MOD_OPT, "");

    FetchDefaultArgumentPos(&defaultpos, defaultargpos, argmodes, pronallargs);

    /*
     * This is a bit tricky in named notation, since the supplied
     * arguments could replace any subset of the defaults.
     */
    if (argnumbers != NULL) {
        bool* deleteflag = NULL;
        int rc = 0;

        deleteflag = (bool*)palloc(pronargdefaults * sizeof(bool));
        rc = memset_s(deleteflag, pronargdefaults * sizeof(bool), 0, pronargdefaults * sizeof(bool));
        securec_check_c(rc, "\0", "\0");

        for (counter1 = pronargdefaults; counter1 > 0; counter1--) {
            found = false;
            pos = defaultpos[counter1 - 1];
            for (counter2 = ndargs; counter2 > 0; counter2--) {
                if (argnumbers[pronargs - counter2] == pos) {
                    found = true;
                    break;
                }
            }
            if (!found)
                defaults = RemoveListCell(defaults, pos);
        }
    }

    /*
     * Defaults for positional notation are lots easier; just
     * remove any unwanted ones from the front.
     */
    else {
        int ndelete = 0;

        ndelete = pronargdefaults - ndargs;

        while (ndelete-- > 0) {
            defaults = list_delete_first(defaults);
        }
        
        *defaultValid = CheckDefaultArgsPosition(defaultargpos, pronargdefaults, ndargs,
                                                 pronallargs, pronargs, tuple);
    }

    if (argtypes != NULL)
        pfree_ext(argtypes);
    if (argmodes != NULL)
        pfree_ext(argmodes);
    if (argnames != NULL) {
        for (counter1 = 0; counter1 < pronallargs; counter1++) {
            if (argnames[counter1])
                pfree_ext(argnames[counter1]);
        }
        pfree_ext(argnames);
    }
    pfree_ext(defaultpos);

    ReleaseSysCache(tuple);
    return defaults;
}

static bool CheckDefaultArgs(int2vector* defaultargpos, int pronargdefaults, int pronallargs,
    int pronargs, HeapTuple proctup)
{
    if (u_sess->attr.attr_sql.sql_compatibility != A_FORMAT || PROC_UNCHECK_DEFAULT_PARAM) {
        return true;
    }
    // if the func has out param, but did not enable_out_param_override, we don't check the defaultpos
    if (pronallargs > pronargs && !enable_out_param_override()) {
        return true;
    }
    // the pg_catalog's func can omit out param, so we don't check the defaultpos
    bool isnull = false;
    Datum namespaceDatum = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_pronamespace, &isnull);
    if (!isnull && IsAformatStyleFunctionOid(DatumGetObjectId(namespaceDatum), HeapTupleGetOid(proctup))) {
        return true;
    }
    // if the func has the default args without position, we consider the position is at the end, no error
    if (defaultargpos->dim1 == 0 && pronargdefaults > 0) {
        return true;
    }
    return false;
}

/*
 * Check whether the parameter is in the appropriate position with default values
 */
static bool CheckDefaultArgsPosition(int2vector* defaultargpos, int pronargdefaults, int ndargs,
    int pronallargs, int pronargs, HeapTuple proctup)
{
    if (CheckDefaultArgs(defaultargpos, pronargdefaults, pronallargs, pronargs, proctup)) {
        return true;
    }
    // Check whether the defaultpos are at the end of the parameter list from back to front
    int argIndex = pronallargs - 1;
    int defaultposIndex = pronargdefaults - 1;
    for (; argIndex >= pronallargs - ndargs; argIndex--, defaultposIndex--) {
        if (defaultargpos->values[defaultposIndex] != argIndex) {
            return false;
        }
    }
    return true;
}

/*
 * Replace column managed type with original type
 * to identify overloaded functions
 */
static Oid cl_get_input_param_original_type(Oid func_id, int argno)
{
    HeapTuple gs_oldtup = SearchSysCache1(GSCLPROCID, ObjectIdGetDatum(func_id));
    Oid ret = InvalidOid;
    if (HeapTupleIsValid(gs_oldtup)) {
        bool isnull = false;
        oidvector* proargcachedcol = (oidvector*)DatumGetPointer(
            SysCacheGetAttr(GSCLPROCID, gs_oldtup, Anum_gs_encrypted_proc_proargcachedcol, &isnull));
        if (!isnull && proargcachedcol->dim1 > argno) {
            Oid cachedColId = proargcachedcol->values[argno];
            HeapTuple tup = SearchSysCache1(CEOID, ObjectIdGetDatum(cachedColId));
            if (HeapTupleIsValid(tup)) {
                Form_gs_encrypted_columns ec_form = (Form_gs_encrypted_columns)GETSTRUCT(tup);
                ret = ec_form->data_type_original_oid;
                ReleaseSysCache(tup);
            }
        }
        ReleaseSysCache(gs_oldtup);
    }
    return ret;
}

/*
 * check_srf_call_placement
 *		Verify that a set-returning function is called in a valid place,
 *		and throw a nice error if not.
 *
 * A side-effect is to set pstate->p_hasTargetSRFs true if appropriate.
 */
void check_srf_call_placement(ParseState* pstate, Node* last_srf, int location)
{
    const char* err;
    bool errkind;

    /*
     * Check to see if the set-returning function is in an invalid place
     * within the query.  Basically, we don't allow SRFs anywhere except in
     * the targetlist (which includes GROUP BY/ORDER BY expressions), VALUES,
     * and functions in FROM.
     *
     * For brevity we support two schemes for reporting an error here: set
     * "err" to a custom message, or set "errkind" true if the error context
     * is sufficiently identified by what ParseExprKindName will return, *and*
     * what it will return is just a SQL keyword.  (Otherwise, use a custom
     * message to avoid creating translation problems.)
     */
    err = NULL;
    errkind = false;
    switch (pstate->p_expr_kind) {
        case EXPR_KIND_NONE:
            Assert(false); /* can't happen */
            break;
        case EXPR_KIND_OTHER:
            /* Accept SRF here; caller must throw error if wanted */
            break;
        case EXPR_KIND_JOIN_ON:
        case EXPR_KIND_JOIN_USING:
            err = _("set-returning functions are not allowed in JOIN conditions");
            break;
        case EXPR_KIND_FROM_SUBSELECT:
            /* can't get here, but just in case, throw an error */
            errkind = true;
            break;
        case EXPR_KIND_FROM_FUNCTION:
            if (pstate->p_is_flt_frame) {
                /* okay, but we don't allow nested SRFs here */
                /* errmsg is chosen to match transformRangeFunction() */
                /* errposition should point to the inner SRF */
                if (pstate->p_last_srf != last_srf)
                    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                    errmsg("set-returning functions must appear at top level of FROM"),
                                    parser_errposition(pstate, exprLocation(pstate->p_last_srf))));
            }
            break;
        case EXPR_KIND_WHERE:
            errkind = true;
            break;
        case EXPR_KIND_POLICY:
            err = _("set-returning functions are not allowed in policy expressions");
            break;
        case EXPR_KIND_HAVING:
            errkind = true;
            break;
        case EXPR_KIND_FILTER:
            errkind = true;
            break;
        case EXPR_KIND_WINDOW_PARTITION:
        case EXPR_KIND_WINDOW_ORDER:
            /* okay, these are effectively GROUP BY/ORDER BY */
            pstate->p_hasTargetSRFs = true;
            break;
        case EXPR_KIND_WINDOW_FRAME_RANGE:
        case EXPR_KIND_WINDOW_FRAME_ROWS:
            err = _("set-returning functions are not allowed in window definitions");
            break;
        case EXPR_KIND_SELECT_TARGET:
        case EXPR_KIND_INSERT_TARGET:
            /* okay */
            pstate->p_hasTargetSRFs = true;
            break;
        case EXPR_KIND_UPDATE_SOURCE:
        case EXPR_KIND_UPDATE_TARGET:
            /* disallowed because it would be ambiguous what to do */
            errkind = true;
            break;
        case EXPR_KIND_GROUP_BY:
        case EXPR_KIND_ORDER_BY:
            /* okay */
            pstate->p_hasTargetSRFs = true;
            break;
        case EXPR_KIND_DISTINCT_ON:
            /* okay */
            pstate->p_hasTargetSRFs = true;
            break;
        case EXPR_KIND_LIMIT:
        case EXPR_KIND_OFFSET:
            errkind = true;
            break;
        case EXPR_KIND_RETURNING:
            errkind = true;
            break;
        case EXPR_KIND_VALUES:
            /* SRFs are presently not supported by nodeValuesscan.c */
            errkind = true;
            break;
        case EXPR_KIND_VALUES_SINGLE:
            /* okay, since we process this like a SELECT tlist */
            pstate->p_hasTargetSRFs = true;
            break;
        case EXPR_KIND_CHECK_CONSTRAINT:
        case EXPR_KIND_DOMAIN_CHECK:
            err = _("set-returning functions are not allowed in check constraints");
            break;
        case EXPR_KIND_COLUMN_DEFAULT:
        case EXPR_KIND_FUNCTION_DEFAULT:
            err = _("set-returning functions are not allowed in DEFAULT expressions");
            break;
        case EXPR_KIND_INDEX_EXPRESSION:
            err = _("set-returning functions are not allowed in index expressions");
            break;
        case EXPR_KIND_INDEX_PREDICATE:
            err = _("set-returning functions are not allowed in index predicates");
            break;
        case EXPR_KIND_STATS_EXPRESSION:
            err = _("set-returning functions are not allowed in statistics expressions");
            break;
        case EXPR_KIND_ALTER_COL_TRANSFORM:
            err = _("set-returning functions are not allowed in transform expressions");
            break;
        case EXPR_KIND_EXECUTE_PARAMETER:
            err = _("set-returning functions are not allowed in EXECUTE parameters");
            break;
        case EXPR_KIND_TRIGGER_WHEN:
            err = _("set-returning functions are not allowed in trigger WHEN conditions");
            break;
        case EXPR_KIND_PARTITION_EXPRESSION:
            err = _("set-returning functions are not allowed in partition key expressions");
            break;
        case EXPR_KIND_PARTITION_BOUND:
            err = _("set-returning functions are not allowed in partition bound expression");
            break;
        case EXPR_KIND_CALL_ARGUMENT:
            err = _("set-returning functions are not allowed in CALL procedure argument");
            break;
        case EXPR_KIND_COPY_WHERE:
            err = _("set-returning functions are not allowed in WHERE condition in COPY FROM");
            break;
        case EXPR_KIND_GENERATED_COLUMN:
            err = _("set-returning functions are not allowed in generation expression");
            break;
        case EXPR_KIND_WINDOW_FRAME_GROUPS:
            err = _("set-returning functions are not allowed in window frame clause with GROUPS");
            break;
        case EXPR_KIND_MERGE_WHEN:
            err = _("set-returning functions are not allowed in merge when expression");
            break;
        case EXPR_KIND_CYCLE_MARK:
            errkind = true;
            /*
             * There is intentionally no default: case here, so that the
             * compiler will warn if we add a new ParseExprKind without
             * extending this switch.  If we do see an unrecognized value at
             * runtime, the behavior will be the same as for EXPR_KIND_OTHER,
             * which is sane anyway.
             */
    }
    if (err)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg_internal("%s", err),
                parser_errposition(pstate, location)));
    if (errkind)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("set-returning functions are not allowed"),
                 parser_errposition(pstate, location)));
}
