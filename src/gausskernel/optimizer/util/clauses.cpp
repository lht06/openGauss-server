/* -------------------------------------------------------------------------
 *
 * clauses.cpp
 *	  routines to manipulate qualification clauses
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/util/clauses.cpp
 *
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/transam.h"
#include "auditfuncs.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "executor/node/nodeCtescan.h"
#include "executor/node/nodeModifyTable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/pgxcplan.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "pgxc/pgxc.h"
#include "rewrite/rewriteManip.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/tzparser.h"
#include "catalog/pg_proc_fn.h"

typedef struct {
    PlannerInfo* root;
    AggClauseCosts* costs;
} count_agg_clauses_context;

typedef struct {
    int nargs;
    List* args;
    int* usecounts;
} substitute_actual_parameters_context;

typedef struct {
    List* planTargetList;
} substitute_var_context;

typedef struct {
    int nargs;
    List* args;
    int sublevels_up;
} substitute_actual_srf_parameters_context;

typedef struct {
    char* proname;
    char* prosrc;
} inline_error_callback_arg;

typedef enum { CONTAIN_FUNCTION_ID, CONTAIN_MUTABLE_FUNCTION, CONTAIN_VOLATILE_FUNTION } checkFuntionType;

typedef struct {
    Oid funcid;
    checkFuntionType checktype;
    bool deep;
} check_function_context;

typedef struct not_strict_context {
    bool check_agg;
} not_strict_context;

static Node* substitute_var_mutator(Node* node, void* context);
static bool contain_agg_clause_walker(Node* node, void* context);
static bool contain_specified_agg_clause_walker(Node* node, void* context);
static bool count_agg_clauses_walker(Node* node, count_agg_clauses_context* context);
static bool find_window_functions_walker(Node* node, WindowLists* lists);
static bool expression_returns_set_rows_walker(Node* node, double* count);
static bool contain_subplans_walker(Node* node, void* context);
template <bool isSimpleVar>
static bool contain_specified_functions_walker(Node* node, check_function_context* context);
static bool contain_nonstrict_functions_walker(Node* node, void* context);
static bool contain_leaky_functions_walker(Node* node, void* context);
static Relids find_nonnullable_rels_walker(Node* node, bool top_level);
static List* find_nonnullable_vars_walker(Node* node, bool top_level);
static bool is_strict_saop(ScalarArrayOpExpr* expr, bool falseOK);
static bool convert_saop_to_hashed_saop_walker(Node *node);
Node* eval_const_expressions_mutator(Node* node, eval_const_expressions_context* context);
static List* simplify_or_arguments(
    List* args, eval_const_expressions_context* context, bool* haveNull, bool* forceTrue);
static List* simplify_and_arguments(
    List* args, eval_const_expressions_context* context, bool* haveNull, bool* forceFalse);
static Node* simplify_boolean_equality(Oid opno, List* args);
static List* expand_function_arguments(List* args, Oid result_type, HeapTuple func_tuple);
static List* reorder_function_arguments(List* args, HeapTuple func_tuple);
static List* add_function_defaults(List* args, HeapTuple func_tuple);
static List* fetch_function_defaults(HeapTuple func_tuple);
static void recheck_cast_function_args(List* args, Oid result_type, HeapTuple func_tuple);
static Expr* evaluate_function(Oid funcid, Oid result_type, int32 result_typmod, Oid result_collid, Oid input_collid,
    List* args, HeapTuple func_tuple, eval_const_expressions_context* context);
static Expr* inline_function(Oid funcid, Oid result_type, Oid result_collid, Oid input_collid, List* args,
    HeapTuple func_tuple, eval_const_expressions_context* context);
static Node* substitute_actual_parameters(Node* expr, int nargs, List* args, int* usecounts);
static Node* substitute_actual_parameters_mutator(Node* node, substitute_actual_parameters_context* context);
static void sql_inline_error_callback(void* arg);
static Query* substitute_actual_srf_parameters(Query* expr, int nargs, List* args);
static Node* substitute_actual_srf_parameters_mutator(Node* node, substitute_actual_srf_parameters_context* context);
static bool tlist_matches_coltypelist(List* tlist, List* coltypelist);

static bool is_exec_external_param_const(PlannerInfo* root, Node* node);
static bool is_operator_pushdown(Oid opno);
static bool contain_var_unsubstitutable_functions_walker(Node* node, void* context);
static bool is_accurate_estimatable_func(Oid funcId);

/*****************************************************************************
 *		OPERATOR clause functions
 *****************************************************************************/
/*
 * make_opclause
 *	  Creates an operator clause given its operator info, left operand
 *	  and right operand (pass NULL to create single-operand clause),
 *	  and collation info.
 */
Expr* make_opclause(
    Oid opno, Oid opresulttype, bool opretset, Expr* leftop, Expr* rightop, Oid opcollid, Oid inputcollid)
{
    OpExpr* expr = makeNode(OpExpr);

    expr->opno = opno;
    expr->opfuncid = InvalidOid;
    expr->opresulttype = opresulttype;
    expr->opretset = opretset;
    expr->opcollid = opcollid;
    expr->inputcollid = inputcollid;
    if (rightop != NULL)
        expr->args = list_make2(leftop, rightop);
    else
        expr->args = list_make1(leftop);
    expr->location = -1;
    return (Expr*)expr;
}

/*
 * get_leftop
 *
 * Returns the left operand of a clause of the form (op expr expr)
 *		or (op expr)
 */
Node* get_leftop(const Expr* clause)
{
    const OpExpr* expr = (const OpExpr*)clause;

    if (expr->args != NIL)
        return (Node*)linitial(expr->args);
    else
        return NULL;
}

/*
 * get_rightop
 *
 * Returns the right operand in a clause of the form (op expr expr).
 * NB: result will be NULL if applied to a unary op clause.
 */
Node* get_rightop(const Expr* clause)
{
    const OpExpr* expr = (const OpExpr*)clause;

    if (list_length(expr->args) >= 2)
        return (Node*)lsecond(expr->args);
    else
        return NULL;
}

/*****************************************************************************
 *		NOT clause functions
 *****************************************************************************/
/*
 * not_clause
 *
 * Returns t iff this is a 'not' clause: (NOT expr).
 */
bool not_clause(Node* clause)
{
    return (clause != NULL && IsA(clause, BoolExpr) && ((BoolExpr*)clause)->boolop == NOT_EXPR);
}

/*
 * make_notclause
 *
 * Create a 'not' clause given the expression to be negated.
 */
Expr* make_notclause(Expr* notclause)
{
    BoolExpr* expr = makeNode(BoolExpr);

    expr->boolop = NOT_EXPR;
    expr->args = list_make1(notclause);
    expr->location = -1;
    return (Expr*)expr;
}

/*
 * get_notclausearg
 *
 * Retrieve the clause within a 'not' clause
 */
Expr* get_notclausearg(Expr* notclause)
{
    return (Expr*)linitial(((BoolExpr*)notclause)->args);
}

/*****************************************************************************
 *		OR clause functions
 *****************************************************************************/
/*
 * or_clause
 *
 * Returns t iff the clause is an 'or' clause: (OR { expr }).
 */
bool or_clause(Node* clause)
{
    return (clause != NULL && IsA(clause, BoolExpr) && ((BoolExpr*)clause)->boolop == OR_EXPR);
}

/*
 * make_orclause
 *
 * Creates an 'or' clause given a list of its subclauses.
 */
Expr* make_orclause(List* orclauses)
{
    BoolExpr* expr = makeNode(BoolExpr);

    expr->boolop = OR_EXPR;
    expr->args = orclauses;
    expr->location = -1;
    return (Expr*)expr;
}

/*****************************************************************************
 *		AND clause functions
 *****************************************************************************/
/*
 * and_clause
 *
 * Returns t iff its argument is an 'and' clause: (AND { expr }).
 */
bool and_clause(Node* clause)
{
    return (clause != NULL && IsA(clause, BoolExpr) && ((BoolExpr*)clause)->boolop == AND_EXPR);
}

/*
 * make_andclause
 *
 * Creates an 'and' clause given a list of its subclauses.
 */
Expr* make_andclause(List* andclauses)
{
    BoolExpr* expr = makeNode(BoolExpr);

    expr->boolop = AND_EXPR;
    expr->args = andclauses;
    expr->location = -1;
    return (Expr*)expr;
}

/*
 * make_and_qual
 *
 * Variant of make_andclause for ANDing two qual conditions together.
 * Qual conditions have the property that a NULL nodetree is interpreted
 * as 'true'.
 *
 * NB: this makes no attempt to preserve AND/OR flatness; so it should not
 * be used on a qual that has already been run through prepqual.c.
 */
Node* make_and_qual(Node* qual1, Node* qual2)
{
    if (qual1 == NULL)
        return qual2;
    if (qual2 == NULL)
        return qual1;
    return (Node*)make_andclause(list_make2(qual1, qual2));
}

/*
 * Sometimes (such as in the input of ExecQual), we use lists of expression
 * nodes with implicit AND semantics.
 *
 * These functions convert between an AND-semantics expression list and the
 * ordinary representation of a boolean expression.
 *
 * Note that an empty list is considered equivalent to TRUE.
 */
Expr* make_ands_explicit(List* andclauses)
{
    if (andclauses == NIL)
        return (Expr*)makeBoolConst(true, false);
    else if (list_length(andclauses) == 1)
        return (Expr*)linitial(andclauses);
    else
        return make_andclause(andclauses);
}

/*
 * flatten_ands_clause
 *
 * Traverse an AND clause and extract the leaf nodes of the clause into a list.
 */
List* flatten_ands_clause(Expr* clause)
{
    List* list_clause = NIL;

    if (and_clause((Node*)clause)) {
        ListCell* lc = NULL;
        foreach (lc, ((BoolExpr*)clause)->args) {
            List* child_list_clause = NIL;
            Expr* expr = (Expr*)lfirst(lc);
            if (and_clause((Node*)expr)) {
                child_list_clause = flatten_ands_clause(expr);
            } else {
                child_list_clause = lappend(child_list_clause, expr);
            }
            list_clause = list_concat(list_clause, child_list_clause);
        }
    } else {
        list_clause = lappend(list_clause, clause);
    }

    return list_clause;
}

/*
 * @Description: Append this clause into list.
 * @in clause: Qual exprs.
 * @return: Expr list.
 */
List* make_ands_implicit(Expr* clause)
{
    /*
     * NB: because the parser sets the qual field to NULL in a query that has
     * no WHERE clause, we must consider a NULL input clause as TRUE, even
     * though one might more reasonably think it FALSE.  Grumble. If this
     * causes trouble, consider changing the parser's behavior.
     */
    if (clause == NULL)
        return NIL; /* NULL -> NIL list == TRUE */
    else if (and_clause((Node*)clause))
        return flatten_ands_clause(clause);
    else if (IsA(clause, Const) && !((Const*)clause)->constisnull && DatumGetBool(((Const*)clause)->constvalue))
        return NIL; /* constant TRUE input -> NIL list */
    else {
        if (!IsA(clause, List)) {
            return list_make1(clause);
        } else {
            /*
             * To HDFS foreign table, this clause already be a list in non-stream plan.
             * In this case, we only need return clause, else will lead to execute crash.
             */
            return (List*)clause;
        }
    }
}

/*****************************************************************************
 *		Aggregate-function clause manipulation
 *****************************************************************************/
/*
 * @Description: check whether use dn agg shipping logic for some special agg function.
 * @in funcId : OID of agg function.
 * @in args   : args of agg function.
 * @in hasgroupclause : agg function with group by clause or not.
 * @in hasorderclause : agg function with order by clause or not.
 * @return : whether use original dn agg shipping logic.
 */
static bool is_dn_agg_function_only(Oid funcId, List* args, bool hasgroupclause, bool hasorderclause)
{
    static uint support_dn_agg_only[] = {
        ARRAYAGGFUNCOID,                  // array_agg
        LISTAGGFUNCOID,                   // listagg
        LISTAGGNOARG2FUNCOID,             // listagg_without_delimiter
        INT2LISTAGGFUNCOID,               // int2_listagg
        INT2LISTAGGNOARG2FUNCOID,         // int2_listagg_without_delimiter
        INT4LISTAGGFUNCOID,               // int4_listagg
        INT4LISTAGGNOARG2FUNCOID,         // int4_listagg_without_delimiter
        INT8LISTAGGFUNCOID,               // int8_listagg
        INT8LISTAGGNOARG2FUNCOID,         // int8_listagg_without_delimiter
        FLOAT4LISTAGGFUNCOID,             // float4_listagg
        FLOAT4LISTAGGNOARG2FUNCOID,       // float4_listagg_without_delimiter
        FLOAT8LISTAGGFUNCOID,             // float8_listagg
        FLOAT8LISTAGGNOARG2FUNCOID,       // float8_listagg_without_delimiter
        NUMERICLISTAGGFUNCOID,            // numeric_listagg
        NUMERICLISTAGGNOARG2FUNCOID,      // numeric_listagg_without_delimiter
        DATELISTAGGFUNCOID,               // date_listagg
        DATELISTAGGNOARG2FUNCOID,         // date_listagg_without_delimiter
        TIMESTAMPLISTAGGFUNCOID,          // timestamp_listagg
        TIMESTAMPLISTAGGNOARG2FUNCOID,    // timestamp_listagg_without_delimiter
        TIMESTAMPTZLISTAGGFUNCOID,        // timestamptz_listagg
        TIMESTAMPTZLISTAGGNOARG2FUNCOID,  // timestamptz_listagg_without_delimiter
        INTERVALLISTAGGFUNCOID,           // interval_listagg
        INTERVALLISTAGGNOARG2FUNCOID,     // interval_listagg_without_delimiter
        GROUPCONCATFUNCOID                // group_concat
    };

    if (funcId >= FirstNormalObjectId)
        return false;

    for (uint i = 0; i < lengthof(support_dn_agg_only); i++) {
        if (funcId == support_dn_agg_only[i])
            return true;
    }

    /*
     * For string_agg's shippable scene:
     * 1. with groupby we can ship through original dn agg logic.
     * 2. without groupby and without orderby we can ship through new agg ship logic.
     * 3. without groupby and with orderby we can not ship and using original dn agg logic
     */
    if (funcId == STRINGAGGFUNCOID) {
        if (hasgroupclause || hasorderclause) {
            return true;
        }

        /* Use original dn agg logic when string_agg's second param is not const. */
        if (!IsA(((TargetEntry*)lsecond(args))->expr, Const))
            return true;
    }

    return false;
}

/*
 * @Description: check whether need to adjust the agg finalize/collect/transation
 *               function or function type in stream plan
 * @in aggref : infomation of the agg.
 * @return : true for adjust and false for do nothing.
 */
bool need_adjust_agg_inner_func_type(Aggref* aggref)
{
    /* Adjust the inner funcid or type when having collect function */
    if (aggref->agghas_collectfn)
        return true;

    /*
     * Agg function like string_agg have no collect function and need call both
     * transition and finalize function at both CN and DN in shipping stream
     * plan, we don't need adjust this scene and let it stay the same as before.
     *
     * You can extend here when you need support stream plan for other agg which
     * also have no collect function, e.g. array_agg,listagg and so on.
     */
    if (aggref->aggfnoid == STRINGAGGFUNCOID || aggref->aggfnoid == ARRAYAGGFUNCOID)
        return false;
    else
        return true;
}

/*
 * contain_agg_clause
 *	  Recursively search for Aggref nodes within a clause.
 *
 *	  Returns true if any aggregate found.
 *
 * This does not descend into subqueries, and so should be used only after
 * reduction of sublinks to subplans, or in contexts where it's known there
 * are no subqueries.  There mustn't be outer-aggregate references either.
 *
 * (If you want something like this but able to deal with subqueries,
 * see rewriteManip.c's contain_aggs_of_level().)
 */
bool contain_agg_clause(Node* clause)
{
    return contain_agg_clause_walker(clause, NULL);
}

static bool contain_agg_clause_walker(Node* node, void* context)
{
    if (node == NULL)
        return false;
    if (IsA(node, Aggref)) {
        AssertEreport(((Aggref*)node)->agglevelsup == 0, MOD_OPT, "AgglevelsUp Incorrect.");
        return true; /* abort the tree traversal and return true */
    }
    if (IsA(node, GroupingFunc)) {
        AssertEreport(((GroupingFunc*)node)->agglevelsup == 0, MOD_OPT, "AgglevelsUp Incorrect.");
        return true; /* abort the tree traversal and return true */
    }

    AssertEreport(!IsA(node, SubLink), MOD_OPT, "");
    return expression_tree_walker(node, (bool (*)())contain_agg_clause_walker, context);
}

/*
 * contain_specified_agg_clause
 *	  Recursively search for Aggref nodes within a clause.
 *
 *	  Returns true if node fails in count_agg_clauses().
 */
bool contain_specified_agg_clause(Node* clause)
{
    return contain_specified_agg_clause_walker(clause, NULL);
}

static bool contain_specified_agg_clause_walker(Node* node, void* context)
{
    if (node == NULL)
        return false;
    if (IsA(node, Aggref)) {
        if (((Aggref*)node)->agglevelsup > 0)
            return true;
    }
    if (IsA(node, GroupingFunc)) {
        if (((GroupingFunc*)node)->agglevelsup > 0)
            return true;
    }

    if (IsA(node, SubLink))
        return true;
    return expression_tree_walker(node, (bool (*)())contain_specified_agg_clause_walker, context);
}

/*
 * count_agg_clauses
 *	  Recursively count the Aggref nodes in an expression tree, and
 *	  accumulate other cost information about them too.
 *
 *	  Note: this also checks for nested aggregates, which are an error.
 *
 * We not only count the nodes, but estimate their execution costs, and
 * attempt to estimate the total space needed for their transition state
 * values if all are evaluated in parallel (as would be done in a HashAgg
 * plan).  See AggClauseCosts for the exact set of statistics collected.
 *
 * NOTE that the counts/costs are ADDED to those already in *costs ... so
 * the caller is responsible for zeroing the struct initially.
 *
 * This does not descend into subqueries, and so should be used only after
 * reduction of sublinks to subplans, or in contexts where it's known there
 * are no subqueries.  There mustn't be outer-aggregate references either.
 */
void count_agg_clauses(PlannerInfo* root, Node* clause, AggClauseCosts* costs)
{
    count_agg_clauses_context context;

    context.root = root;
    context.costs = costs;
    (void)count_agg_clauses_walker(clause, &context);
}

static void count_agg_clauses_walker_isa(Node* node, count_agg_clauses_context* context)
{
    Aggref* aggref = (Aggref*)node;
    AggClauseCosts* costs = context->costs;
    HeapTuple aggTuple;
    Form_pg_aggregate aggform;
    Oid aggtransfn;
    Oid aggfinalfn;
    Oid aggtranstype;
    QualCost argcosts;
    Oid inputTypes[FUNC_MAX_ARGS];
    int numArguments;
    bool hasgroupclause = (context->root->parse->groupClause != NIL);
    bool hasorderclause = (costs->numOrderedAggs > 0);
    bool isOrderedSet = false;
    bool isnull = false;
    char aggkind = 'n';

    AssertEreport(aggref->agglevelsup == 0, MOD_OPT, "");

    /* fetch info about aggregate from pg_aggregate */
    aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));
    if (!HeapTupleIsValid(aggTuple))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for aggregate %u", aggref->aggfnoid)));
    aggform = (Form_pg_aggregate)GETSTRUCT(aggTuple);
    aggtransfn = aggform->aggtransfn;
    aggfinalfn = aggform->aggfinalfn;
    aggtranstype = aggform->aggtranstype;
    /* 91269 version support orderedset agg  */
    aggkind = DatumGetChar(SysCacheGetAttr(AGGFNOID, aggTuple, Anum_pg_aggregate_aggkind, &isnull));
    if (!AGGKIND_IS_ORDERED_SET(aggkind))
        aggkind = 'n';
    isOrderedSet = AGGKIND_IS_ORDERED_SET(aggkind);
    ReleaseSysCache(aggTuple);

    if (IsPolymorphicType(aggref->aggtrantype)) {
        costs->hasPolymorphicType = true;
    }

    /* count it ,note ordered-set aggs always have nonempty aggorder */
    costs->numAggs++;
    if (aggref->aggorder != NIL || (isOrderedSet && aggref->aggdistinct != NIL))
        costs->numOrderedAggs++;
    if (aggref->aggdistinct != NIL) {
        Node* distinct_node =
            get_sortgroupclause_expr((SortGroupClause*)linitial(aggref->aggdistinct), aggref->args);
        costs->exprAggs = list_append_unique(costs->exprAggs, distinct_node);
        costs->unhashable = costs->unhashable || !grouping_is_hashable(aggref->aggdistinct);
        if (is_dn_agg_function_only(aggref->aggfnoid, aggref->args, hasgroupclause, hasorderclause))
            costs->hasdctDnAggs = true;
    } else if (is_dn_agg_function_only(aggref->aggfnoid, aggref->args, hasgroupclause, hasorderclause))
        costs->hasDnAggs = true;

    /* add component function execution costs to appropriate totals */
    costs->transCost.per_tuple += get_func_cost(aggtransfn) * u_sess->attr.attr_sql.cpu_operator_cost;
    if (OidIsValid(aggfinalfn))
        costs->finalCost += get_func_cost(aggfinalfn) * u_sess->attr.attr_sql.cpu_operator_cost;

    /* also add the input expressions' cost to per-input-row costs */
    cost_qual_eval_node(&argcosts, (Node*)aggref->args, context->root);
    costs->transCost.startup += argcosts.startup;
    costs->transCost.per_tuple += argcosts.per_tuple;

    /*
        * If there are direct arguments, treat their evaluation cost like the
        * cost of the finalfn.
        */
    if (aggref->aggdirectargs) {
        cost_qual_eval_node(&argcosts, (Node*)aggref->aggdirectargs, context->root);
        costs->transCost.startup += argcosts.startup;
        costs->finalCost += argcosts.per_tuple;
    }

    /* extract argument types (ignoring any ORDER BY expressions) */
    numArguments = get_aggregate_argtypes(aggref, inputTypes, FUNC_MAX_ARGS);

    /* resolve actual type of transition state, if polymorphic */
    aggtranstype = resolve_aggregate_transtype(aggref->aggfnoid, aggtranstype, inputTypes, numArguments);

    /*
        * If the transition type is pass-by-value then it doesn't add
        * anything to the required size of the hashtable.	If it is
        * pass-by-reference then we have to add the estimated size of the
        * value itself, plus palloc overhead.
        */
    if (!get_typbyval(aggtranstype)) {
        /*
            * If transition state is of same type as first aggregated
            * input, assume it's the same typmod (same width) as well.
            * This works for cases like MAX/MIN and is probably somewhat
            * reasonable otherwise.
            */
        int numdirectargs = list_length(aggref->aggdirectargs);
        int32 aggtranstypmod;
        Size avgwidth;

        /*
            * If transition state is of same type as first input, assume it's
            * the same typmod (same width) as well.  This works for cases
            * like MAX/MIN and is probably somewhat reasonable otherwise.
            */
        if (numArguments > 0 && aggtranstype == inputTypes[numdirectargs])
            aggtranstypmod = exprTypmod((Node*)linitial(aggref->args));
        else
            aggtranstypmod = -1;

        avgwidth = get_typavgwidth(aggtranstype, aggtranstypmod);
        avgwidth = MAXALIGN(avgwidth);

        costs->transitionSpace += avgwidth + 2 * sizeof(void*);
        costs->aggWidth += avgwidth;
    } else if (aggtranstype == INTERNALOID) {
#ifndef ENABLE_MULTIPLE_NODES
        /*
         * XXX: we apply the pg commit 69c8fbac201652282e18b0e2e301d4ada991fbde
         * but the difference of pg_aggregate bwtween pg and og
         * we have to do some hard code here(og's pg_aggregate doesn't have the column aggtransspace)
         */
        if (u_sess->attr.attr_common.enable_expr_fusion && u_sess->attr.attr_sql.query_dop_tmp == 1) {
            switch (aggtransfn) {
                case 5439: /* int8_avg_accum_numeric */
                    costs->transitionSpace +=48;
                    costs->aggWidth += 48;
                    break;
                case 5440: /* numeric_accum_numeric */
                case 5442: /* numeric_avg_accum_numeric */
                    costs->transitionSpace += 128;
                    costs->aggWidth += 128;
                    break;
                default:
                    /*
                    * INTERNAL transition type is a special case: although INTERNAL
                    * is pass-by-value, it's almost certainly being used as a pointer
                    * to some large data structure.  We assume usage of
                    * ALLOCSET_DEFAULT_INITSIZE, which is a good guess if the data is
                    * being kept in a private memory context, as is done by
                    * array_agg() for instance.
                    */
                    costs->transitionSpace += ALLOCSET_DEFAULT_INITSIZE;
                    costs->aggWidth += ALLOCSET_DEFAULT_INITSIZE;
            }
        } else {
            costs->transitionSpace += ALLOCSET_DEFAULT_INITSIZE;
            costs->aggWidth += ALLOCSET_DEFAULT_INITSIZE;
        }
#else
        /*
            * INTERNAL transition type is a special case: although INTERNAL
            * is pass-by-value, it's almost certainly being used as a pointer
            * to some large data structure.  We assume usage of
            * ALLOCSET_DEFAULT_INITSIZE, which is a good guess if the data is
            * being kept in a private memory context, as is done by
            * array_agg() for instance.
            */
        costs->transitionSpace += ALLOCSET_DEFAULT_INITSIZE;
        costs->aggWidth += ALLOCSET_DEFAULT_INITSIZE;
#endif
    } else {
        costs->aggWidth += get_typavgwidth(aggtranstype, -1);
    }

    /*
        * Complain if the aggregate's arguments contain any aggregates;
        * nested agg functions are semantically nonsensical.
        */
    if (contain_agg_clause((Node*)aggref->args))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                (errcode(ERRCODE_GROUPING_ERROR), errmsg("aggregate function calls cannot be nested"))));
}

static bool count_agg_clauses_walker(Node* node, count_agg_clauses_context* context)
{
    if (node == NULL) {
        return false;
    }
    if (IsA(node, Aggref)) {
        count_agg_clauses_walker_isa(node, context);
        /*
        * Having checked that, we need not recurse into the argument.
        */
        return false;
    }

    AssertEreport(!IsA(node, SubLink), MOD_OPT, "");
    return expression_tree_walker(node, (bool (*)())count_agg_clauses_walker, (void*)context);
}

/*****************************************************************************
 *		Window-function clause manipulation
 *****************************************************************************/
/*
 * contain_window_function
 *	  Recursively search for WindowFunc nodes within a clause.
 *
 * Since window functions don't have level fields, but are hard-wired to
 * be associated with the current query level, this is just the same as
 * rewriteManip.c's function.
 */
bool contain_window_function(Node* clause)
{
    return checkExprHasWindowFuncs(clause);
}

/*
 * @Description: Free WindowFuncLists windowFuncs.
 * @in lists: WindowLists.
 */
void free_windowFunc_lists(WindowLists* lists)
{
    for (Index i = 0; i < lists->maxWinRef + 1; i++) {
        list_free_ext(lists->windowFuncs[i]);
        lists->windowFuncs[i] = NIL;
    }

    lists->numWindowFuncs = 0;
}

/*
 * @Description: Make WindowFuncLists struct.
 * @in maxWinRef: Max win number.
 * @return: WindowFuncLists.
 */
WindowLists* make_windows_lists(Index maxWinRef)
{
    WindowLists* lists = (WindowLists*)palloc(sizeof(WindowLists));

    lists->numWindowFuncs = 0;
    lists->maxWinRef = maxWinRef;
    lists->windowFuncs = (List**)palloc0((maxWinRef + 1) * sizeof(List*));
    lists->activeWindows = NIL;

    return lists;
}

/*
 * find_window_functions
 *	  Locate all the WindowFunc nodes in an expression tree, and organize
 *	  them by winref ID number.
 *
 * Caller must provide an upper bound on the winref IDs expected in the tree.
 */
void find_window_functions(Node* clause, WindowLists* lists)
{
    (void)find_window_functions_walker(clause, lists);
}

static bool find_window_functions_walker(Node* node, WindowLists* lists)
{
    if (node == NULL)
        return false;
    if (IsA(node, WindowFunc)) {
        WindowFunc* wfunc = (WindowFunc*)node;

        /* winref is unsigned, so one-sided test is OK */
        if (wfunc->winref > lists->maxWinRef)
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                    errmsg("WindowFunc contains out-of-range winref %u", wfunc->winref)));
        lists->windowFuncs[wfunc->winref] = lappend(lists->windowFuncs[wfunc->winref], wfunc);
        lists->numWindowFuncs++;

        /*
         * Complain if the window function's arguments contain window
         * functions
         */
        if (contain_window_function((Node*)wfunc->args))
            ereport(ERROR, (errcode(ERRCODE_WINDOWING_ERROR), errmsg("window function calls cannot be nested")));

        /*
         * Having checked that, we need not recurse into the argument.
         */
        return false;
    }
    AssertEreport(!IsA(node, SubLink), MOD_OPT, "");
    return expression_tree_walker(node, (bool (*)())find_window_functions_walker, (void*)lists);
}

/*****************************************************************************
 *		Support for expressions returning sets
 *****************************************************************************/
/*
 * expression_returns_set_rows
 *	  Estimate the number of rows returned by a set-returning expression.
 *	  The result is 1 if it's not a set-returning expression.
 *
 * We should only examine the top-level function or operator; it used to be
 * appropriate to recurse, but not anymore.  (Even if there are more SRFs in
 * the function's inputs, their multipliers are accounted for separately.)
 *
 * Note: keep this in sync with expression_returns_set() in nodes/nodeFuncs.c.
 */
double expression_returns_set_rows(Node* clause)
{
    if (u_sess->attr.attr_common.enable_expr_fusion && u_sess->attr.attr_sql.query_dop_tmp == 1) {
        if (clause == NULL)
            return 1.0;
        if (IsA(clause, FuncExpr)) {
            FuncExpr *expr = (FuncExpr *)clause;

            if (expr->funcretset)
                return clamp_row_est(get_func_rows(expr->funcid));
        }
        if (IsA(clause, OpExpr)) {
            OpExpr *expr = (OpExpr *)clause;
            if (expr->opretset) {
                set_opfuncid(expr);
                return clamp_row_est(get_func_rows(expr->opfuncid));
            }
        }
        return 1.0;
    } else {
        double result = 1;

        (void)expression_returns_set_rows_walker(clause, &result);
        return clamp_row_est(result);
    }
}

static void expression_returns_set_rows_walker_isa(Node* node, double* count)
{
    if (IsA(node, FuncExpr)) {
        FuncExpr* expr = (FuncExpr*)node;

        if (expr->funcretset)
            *count *= get_func_rows(expr->funcid);
    }
    if (IsA(node, OpExpr)) {
        OpExpr* expr = (OpExpr*)node;

        if (expr->opretset) {
            set_opfuncid(expr);
            *count *= get_func_rows(expr->opfuncid);
        }
    }
}

static bool expression_returns_set_rows_walker(Node* node, double* count)
{
    if (node == NULL)
        return false;

    expression_returns_set_rows_walker_isa(node, count);

    /* Avoid recursion for some cases that can't return a set */
    if (IsA(node, Aggref))
        return false;
    if (IsA(node, WindowFunc))
        return false;
    if (IsA(node, DistinctExpr))
        return false;
    if (IsA(node, NullIfExpr))
        return false;
    if (IsA(node, ScalarArrayOpExpr))
        return false;
    if (IsA(node, BoolExpr))
        return false;
    if (IsA(node, SubLink))
        return false;
    if (IsA(node, SubPlan))
        return false;
    if (IsA(node, AlternativeSubPlan))
        return false;
    if (IsA(node, ArrayExpr))
        return false;
    if (IsA(node, RowExpr))
        return false;
    if (IsA(node, RowCompareExpr))
        return false;
    if (IsA(node, CoalesceExpr))
        return false;
    if (IsA(node, MinMaxExpr))
        return false;
    if (IsA(node, XmlExpr))
        return false;

    return expression_tree_walker(node, (bool (*)())expression_returns_set_rows_walker, (void*)count);
}

/*
 * tlist_returns_set_rows
 *	  Estimate the number of rows returned by a set-returning targetlist.
 *	  The result is 1 if there are no set-returning functions.
 *
 * Here, the result is the largest rowcount estimate of any of the tlist's
 * expressions, not the product as you would get from naively applying
 * expression_returns_set_rows() to the whole tlist.  The behavior actually
 * implemented by ExecTargetList produces a number of rows equal to the least
 * common multiple of the expression rowcounts, so that the product would be
 * a worst-case estimate that is typically not realistic.  Taking the max as
 * we do here is a best-case estimate that might not be realistic either,
 * but it's probably closer for typical usages.  We don't try to compute the
 * actual LCM because we're working with very approximate estimates, so their
 * LCM would be unduly noisy.
 */
double tlist_returns_set_rows(List* tlist)
{
    double result = 1;
    ListCell* lc = NULL;

    foreach (lc, tlist) {
        TargetEntry* tle = (TargetEntry*)lfirst(lc);
        double colresult;

        colresult = expression_returns_set_rows((Node*)tle->expr);
        if (result < colresult)
            result = colresult;
    }
    return result;
}

/*****************************************************************************
 *		Subplan clause manipulation
 *****************************************************************************/
/*
 * contain_subplans
 *	  Recursively search for subplan nodes within a clause.
 *
 * If we see a SubLink node, we will return TRUE.  This is only possible if
 * the expression tree hasn't yet been transformed by subselect.c.  We do not
 * know whether the node will produce a true subplan or just an initplan,
 * but we make the conservative assumption that it will be a subplan.
 *
 * Returns true if any subplan found.
 */
bool contain_subplans(Node* clause)
{
    return contain_subplans_walker(clause, NULL);
}

static bool contain_subplans_walker(Node* node, void* context)
{
    if (node == NULL)
        return false;
    if (IsA(node, SubPlan) || IsA(node, AlternativeSubPlan) || IsA(node, SubLink))
        return true; /* abort the tree traversal and return true */
    return expression_tree_walker(node, (bool (*)())contain_subplans_walker, context);
}

/*****************************************************************************
 *		Check clauses for mutable functions
 *****************************************************************************/
/*
 * contain_mutable_functions
 *	  Recursively search for mutable functions within a clause.
 *
 * Returns true if any mutable function (or operator implemented by a
 * mutable function) is found.	This test is needed so that we don't
 * mistakenly think that something like "WHERE random() < 0.5" can be treated
 * as a constant qualification.
 *
 * XXX we do not examine sub-selects to see if they contain uses of
 * mutable functions.  It's not real clear if that is correct or not...
 */
bool contain_mutable_functions(Node* clause)
{
    check_function_context context;
    context.checktype = CONTAIN_MUTABLE_FUNCTION;
    context.deep = false;
    return contain_specified_functions_walker<false>(clause, &context);
}

/*****************************************************************************
 *		Check clauses for volatile functions
 *****************************************************************************/
/*
 * contain_volatile_functions
 *	  Recursively search for volatile functions within a clause.
 *
 * Returns true if any volatile function (or operator implemented by a
 * volatile function) is found. This test prevents invalid conversions
 * of volatile expressions into indexscan quals.
 *
 * XXX we do not examine sub-selects to see if they contain uses of
 * volatile functions.	It's not real clear if that is correct or not...
 */
bool contain_volatile_functions(Node* clause, bool deep)
{
    check_function_context context;
    context.checktype = CONTAIN_VOLATILE_FUNTION;
    context.deep = deep;
    return contain_specified_functions_walker<false>(clause, &context);
}

/*
 * @Discription : check if the clause contains specified function.
 * @in  clause - the clause to search
 * @in  funcid - the function id to search for
 * @out 		: None
 * @return		: true if found the function in the clause
 */
bool contain_specified_function(Node* clause, Oid funcid)
{
    check_function_context context;
    context.funcid = funcid;
    context.checktype = CONTAIN_FUNCTION_ID;
    context.deep = false;
    return contain_specified_functions_walker<false>(clause, &context);
}

/*
 * @Discription : check if current functionid satisfied with the input condition
 * @in  functionid - the function id to check against
 * @in  context - the input condition to check
 * @return		: true if the function meet the input condition
 */
static bool func_meet_check_conditon(Oid functionid, check_function_context* context)
{
    if (context->checktype == CONTAIN_FUNCTION_ID) {
        return functionid == context->funcid;
    } else if (context->checktype == CONTAIN_VOLATILE_FUNTION) {
        return func_volatile(functionid) == PROVOLATILE_VOLATILE;
    } else if (context->checktype == CONTAIN_MUTABLE_FUNCTION) {
        return func_volatile(functionid) != PROVOLATILE_IMMUTABLE;
    } else {
        return false;
    }
}
/*
 * @Discription : walk through the node structure to find it contains specified
 *                function which meet the input condition
 * @in  node    - the node structure to check against
 * @in  context - the input condition to check
 * @return		: true if there's function that meet the input condition
 */
template <bool isSimpleVar>
static bool contain_specified_functions_walker(Node* node, check_function_context* context)
{
    if (node == NULL)
        return false;
    if (IsA(node, FuncExpr)) {
        if (isSimpleVar) {
            return true;
        }
        FuncExpr* expr = (FuncExpr*)node;

        if (func_meet_check_conditon(expr->funcid, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, OpExpr)) {
        OpExpr* expr = (OpExpr*)node;

        set_opfuncid(expr);
        if (func_meet_check_conditon(expr->opfuncid, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, DistinctExpr)) {
        DistinctExpr* expr = (DistinctExpr*)node;

        set_opfuncid((OpExpr*)expr); /* rely on struct equivalence */
        if (func_meet_check_conditon(expr->opfuncid, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, NullIfExpr)) {
        NullIfExpr* expr = (NullIfExpr*)node;

        set_opfuncid((OpExpr*)expr); /* rely on struct equivalence */
        if (func_meet_check_conditon(expr->opfuncid, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, ScalarArrayOpExpr)) {
        ScalarArrayOpExpr* expr = (ScalarArrayOpExpr*)node;

        set_sa_opfuncid(expr);
        if (func_meet_check_conditon(expr->opfuncid, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, CoerceViaIO)) {
        CoerceViaIO* expr = (CoerceViaIO*)node;
        Oid iofunc;
        Oid typioparam;
        bool typisvarlena = false;

        /* check the result type's input function */
        getTypeInputInfo(expr->resulttype, &iofunc, &typioparam);
        if (func_meet_check_conditon(iofunc, context))
            return true;
        /* check the input type's output function */
        getTypeOutputInfo(exprType((Node*)expr->arg), &iofunc, &typisvarlena);
        if (func_meet_check_conditon(iofunc, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, ArrayCoerceExpr)) {
        ArrayCoerceExpr* expr = (ArrayCoerceExpr*)node;

        if (OidIsValid(expr->elemfuncid) && func_meet_check_conditon(expr->elemfuncid, context))
            return true;
        /* else fall through to check args */
    } else if (IsA(node, RowCompareExpr)) {
        RowCompareExpr* rcexpr = (RowCompareExpr*)node;
        ListCell* opid = NULL;

        foreach (opid, rcexpr->opnos) {
            Oid opno = lfirst_oid(opid);
            RegProcedure regProc = get_opcode(opno);
            if (regProc == (RegProcedure)InvalidOid)
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_INVALID_OPERATION),
                        (errmsg("operator %u does not exist", opno))));

            if (func_meet_check_conditon((Oid)regProc, context))
                return true;
        }
        /* else fall through to check args */
    } else if (IsA(node, Rownum)) {
        /* ROWNUM is volatile */
        return context->checktype == CONTAIN_VOLATILE_FUNTION;
    } else if (IsA(node, Query) && context->deep) {
        /* Recurse into subselects */
        return query_tree_walker((Query*)node, (bool (*)())contain_specified_functions_walker<isSimpleVar>, context, 0);
    } else if (IsA(node, UserSetElem)) {
        /* UserSetElem is volatile */
        return context->checktype == CONTAIN_VOLATILE_FUNTION;
    }
    return expression_tree_walker(node, (bool (*)())contain_specified_functions_walker<isSimpleVar>, context);
}

/*
 * exec_simple_check_mutable_function
 *	  Recursively search for mutable functions within plan.
 *
 * This is used for evaluating simple expression in plsql.
 * Returns true if any mutable function (or operator implemented by a
 * mutable function) is found.
 * The result is used to control whether snapshot is needed for simple
 * expression. If expression is FuncExpr, return true directly. Because
 * the behavior of a function is undefined, not support now.
 */
bool exec_simple_check_mutable_function(Node* clause)
{
    check_function_context context;
    context.checktype = CONTAIN_MUTABLE_FUNCTION;
    context.deep = false;
    return contain_specified_functions_walker<true>(clause, &context);
}

/*****************************************************************************
 *		Check clauses for nonstrict functions
 *****************************************************************************/
/*
 * contain_nonstrict_functions
 *	  Recursively search for nonstrict functions within a clause.
 *
 * Returns true if any nonstrict construct is found --- ie, anything that
 * could produce non-NULL output with a NULL input.
 *
 * The idea here is that the caller has verified that the expression contains
 * one or more Var or Param nodes (as appropriate for the caller's need), and
 * now wishes to prove that the expression result will be NULL if any of these
 * inputs is NULL.	If we return false, then the proof succeeded.
 */
bool contain_nonstrict_functions(Node* clause, bool check_agg)
{
    not_strict_context context;

    context.check_agg = check_agg;
    return contain_nonstrict_functions_walker(clause, &context);
}

static bool contain_nonstrict_functions_walker(Node* node, void* context)
{
    if (node == NULL)
        return false;
    if (IsA(node, Aggref)) {
        if (((not_strict_context*)context)->check_agg) {
            Aggref* expr = (Aggref*)node;
            if (expr->aggfnoid == ANYCOUNTOID || expr->aggfnoid == COUNTOID) {
                return true;
            }
        } else {
            /* an aggregate could return non-null with null input */
            return true;
        }
    }
    if (IsA(node, WindowFunc)) {
        /* a window function could return non-null with null input */
        return true;
    }
    if (IsA(node, ArrayRef)) {
        /* array assignment is nonstrict, but subscripting is strict */
        if (((ArrayRef*)node)->refassgnexpr != NULL)
            return true;
        /* else fall through to check args */
    }
    if (IsA(node, FuncExpr)) {
        FuncExpr* expr = (FuncExpr*)node;

        if (!func_strict(expr->funcid))
            return true;
        /* else fall through to check args */
    }
    if (IsA(node, OpExpr)) {
        OpExpr* expr = (OpExpr*)node;

        set_opfuncid(expr);
        if (!func_strict(expr->opfuncid))
            return true;
        /* else fall through to check args */
    }
    if (IsA(node, DistinctExpr)) {
        /* IS DISTINCT FROM is inherently non-strict */
        return true;
    }
    if (IsA(node, NullIfExpr))
        return true;
    if (IsA(node, ScalarArrayOpExpr)) {
        ScalarArrayOpExpr* expr = (ScalarArrayOpExpr*)node;

        if (!is_strict_saop(expr, false))
            return true;
        /* else fall through to check args */
    }
    if (IsA(node, BoolExpr)) {
        BoolExpr* expr = (BoolExpr*)node;

        switch (expr->boolop) {
            case AND_EXPR:
            case OR_EXPR:
                /* AND, OR are inherently non-strict */
                return true;
            default:
                break;
        }
    }
    if (IsA(node, SubLink)) {
        /* In some cases a sublink might be strict, but in general not */
        return true;
    }
    if (IsA(node, SubPlan))
        return true;
    if (IsA(node, AlternativeSubPlan))
        return true;
    /* ArrayCoerceExpr is strict at the array level, regardless of elemfunc */
    if (IsA(node, FieldStore))
        return true;
    if (IsA(node, CaseExpr))
        return true;
    if (IsA(node, ArrayExpr))
        return true;
    if (IsA(node, RowExpr))
        return true;
    if (IsA(node, RowCompareExpr))
        return true;
    if (IsA(node, CoalesceExpr))
        return true;
    if (IsA(node, MinMaxExpr))
        return true;
    if (IsA(node, XmlExpr))
        return true;
    if (IsA(node, NullTest))
        return true;
    if (IsA(node, BooleanTest))
        return true;
    return expression_tree_walker(node, (bool (*)())contain_nonstrict_functions_walker, context);
}

/*****************************************************************************
 *		  Check clauses for non-leakproof functions
 *****************************************************************************/
/*
 * contain_leaky_functions
 *		Recursively search for leaky functions within a clause.
 *
 * Returns true if any function call with side-effect may be present in the
 * clause.	Qualifiers from outside the a security_barrier view should not
 * be pushed down into the view, lest the contents of tuples intended to be
 * filtered out be revealed via side effects.
 */
bool contain_leaky_functions(Node* clause)
{
    return contain_leaky_functions_walker(clause, NULL);
}

static bool contain_leaky_functions_walker(Node* node, void* context)
{
    if (node == NULL)
        return false;

    switch (nodeTag(node)) {
        case T_Var:
        case T_Const:
        case T_Param:
        case T_ArrayExpr:
        case T_NamedArgExpr:
        case T_BoolExpr:
        case T_RelabelType:
        case T_CaseExpr:
        case T_CaseTestExpr:
        case T_RowExpr:
        case T_NullTest:
        case T_BooleanTest:
        case T_List:
        case T_HashFilter:
        case T_UserVar:

            /*
             * We know these node types don't contain function calls; but
             * something further down in the node tree might.
             */
            break;

        case T_FuncExpr: {
            FuncExpr* expr = (FuncExpr*)node;

            if (!get_func_leakproof(expr->funcid))
                return true;
        } break;

        case T_OpExpr:
        case T_DistinctExpr: /* struct-equivalent to OpExpr */
        case T_NullIfExpr:   /* struct-equivalent to OpExpr */
        {
            OpExpr* expr = (OpExpr*)node;

            set_opfuncid(expr);
            if (!get_func_leakproof(expr->opfuncid))
                return true;
        } break;

        case T_ScalarArrayOpExpr: {
            ScalarArrayOpExpr* expr = (ScalarArrayOpExpr*)node;

            set_sa_opfuncid(expr);
            if (!get_func_leakproof(expr->opfuncid))
                return true;
        } break;

        case T_CoerceViaIO: {
            CoerceViaIO* expr = (CoerceViaIO*)node;
            Oid funcid;
            Oid ioparam;
            bool varlena = false;

            getTypeInputInfo(exprType((Node*)expr->arg), &funcid, &ioparam);
            if (!get_func_leakproof(funcid))
                return true;

            getTypeOutputInfo(expr->resulttype, &funcid, &varlena);
            if (!get_func_leakproof(funcid))
                return true;
        } break;

        case T_ArrayCoerceExpr: {
            ArrayCoerceExpr* expr = (ArrayCoerceExpr*)node;
            Oid funcid;
            Oid ioparam;
            bool varlena = false;

            getTypeInputInfo(exprType((Node*)expr->arg), &funcid, &ioparam);
            if (!get_func_leakproof(funcid))
                return true;
            getTypeOutputInfo(expr->resulttype, &funcid, &varlena);
            if (!get_func_leakproof(funcid))
                return true;
        } break;

        case T_RowCompareExpr: {
            RowCompareExpr* rcexpr = (RowCompareExpr*)node;
            ListCell* opid = NULL;

            foreach (opid, rcexpr->opnos) {
                Oid funcid = get_opcode(lfirst_oid(opid));
                if (!get_func_leakproof(funcid))
                    return true;
            }
        } break;

        case T_MinMaxExpr: {
            /*
             * MinMaxExpr is leakproof if the comparison function it calls
             * is leakproof.
             */
            MinMaxExpr* minmaxexpr = (MinMaxExpr*)node;
            TypeCacheEntry* typentry = NULL;
            bool leakproof = false;

            /* Look up the btree comparison function for the datatype */
            typentry = lookup_type_cache(minmaxexpr->minmaxtype, TYPECACHE_CMP_PROC);
            if (OidIsValid(typentry->cmp_proc))
                leakproof = get_func_leakproof(typentry->cmp_proc);
            else {
                /*
                 * The executor will throw an error, but here we just
                 * treat the missing function as leaky.
                 */
                leakproof = false;
            }

            if (!leakproof)
                return true;
        } break;

        default:

            /*
             * If we don't recognize the node tag, assume it might be leaky.
             * This prevents an unexpected security hole if someone adds a new
             * node type that can call a function.
             */
            return true;
    }
    return expression_tree_walker(node, (bool (*)())contain_leaky_functions_walker, context);
}

/*
 * find_nonnullable_rels
 *		Determine which base rels are forced nonnullable by given clause.
 *
 * Returns the set of all Relids that are referenced in the clause in such
 * a way that the clause cannot possibly return TRUE if any of these Relids
 * is an all-NULL row.	(It is OK to err on the side of conservatism; hence
 * the analysis here is simplistic.)
 *
 * The semantics here are subtly different from contain_nonstrict_functions:
 * that function is concerned with NULL results from arbitrary expressions,
 * but here we assume that the input is a Boolean expression, and wish to
 * see if NULL inputs will provably cause a FALSE-or-NULL result.  We expect
 * the expression to have been AND/OR flattened and converted to implicit-AND
 * format.
 *
 * Note: this function is largely duplicative of find_nonnullable_vars().
 * The reason not to simplify this function into a thin wrapper around
 * find_nonnullable_vars() is that the tested conditions really are different:
 * a clause like "t1.v1 IS NOT NULL OR t1.v2 IS NOT NULL" does not prove
 * that either v1 or v2 can't be NULL, but it does prove that the t1 row
 * as a whole can't be all-NULL.
 *
 * top_level is TRUE while scanning top-level AND/OR structure; here, showing
 * the result is either FALSE or NULL is good enough.  top_level is FALSE when
 * we have descended below a NOT or a strict function: now we must be able to
 * prove that the subexpression goes to NULL.
 *
 * We don't use expression_tree_walker here because we don't want to descend
 * through very many kinds of nodes; only the ones we can be sure are strict.
 */
Relids find_nonnullable_rels(Node* clause)
{
    return find_nonnullable_rels_walker(clause, true);
}

static Relids find_nonnullable_rels_walker(Node* node, bool top_level)
{
    Relids result = NULL;
    ListCell* l = NULL;

    if (node == NULL)
        return NULL;
    if (IsA(node, Var)) {
        Var* var = (Var*)node;

        if (var->varlevelsup == 0)
            result = bms_make_singleton(var->varno);
    } else if (IsA(node, List)) {
        /*
         * At top level, we are examining an implicit-AND list: if any of the
         * arms produces FALSE-or-NULL then the result is FALSE-or-NULL. If
         * not at top level, we are examining the arguments of a strict
         * function: if any of them produce NULL then the result of the
         * function must be NULL.  So in both cases, the set of nonnullable
         * rels is the union of those found in the arms, and we pass down the
         * top_level flag unmodified.
         */
        foreach (l, (List*)node) {
            result = bms_join(result, find_nonnullable_rels_walker((Node*)lfirst(l), top_level));
        }
    } else if (IsA(node, FuncExpr)) {
        FuncExpr* expr = (FuncExpr*)node;

        if (func_strict(expr->funcid))
            result = find_nonnullable_rels_walker((Node*)expr->args, false);
    } else if (IsA(node, OpExpr)) {
        OpExpr* expr = (OpExpr*)node;

        set_opfuncid(expr);
        if (func_strict(expr->opfuncid))
            result = find_nonnullable_rels_walker((Node*)expr->args, false);
    } else if (IsA(node, ScalarArrayOpExpr)) {
        ScalarArrayOpExpr* expr = (ScalarArrayOpExpr*)node;

        if (is_strict_saop(expr, true))
            result = find_nonnullable_rels_walker((Node*)expr->args, false);
    } else if (IsA(node, BoolExpr)) {
        BoolExpr* expr = (BoolExpr*)node;

        switch (expr->boolop) {
            case AND_EXPR:
                /* At top level we can just recurse (to the List case) */
                if (top_level) {
                    result = find_nonnullable_rels_walker((Node*)expr->args, top_level);
                    break;
                }

                /*
                 * Below top level, even if one arm produces NULL, the result
                 * could be FALSE (hence not NULL).  However, if *all* the
                 * arms produce NULL then the result is NULL, so we can take
                 * the intersection of the sets of nonnullable rels, just as
                 * for OR.	Fall through to share code.
                 */
                /* FALL THRU */
            case OR_EXPR:

                /*
                 * OR is strict if all of its arms are, so we can take the
                 * intersection of the sets of nonnullable rels for each arm.
                 * This works for both values of top_level.
                 */
                foreach (l, expr->args) {
                    Relids subresult;

                    subresult = find_nonnullable_rels_walker((Node*)lfirst(l), top_level);
                    if (result == NULL) /* first subresult? */
                        result = subresult;
                    else
                        result = bms_int_members(result, subresult);

                    /*
                     * If the intersection is empty, we can stop looking. This
                     * also justifies the test for first-subresult above.
                     */
                    if (bms_is_empty(result))
                        break;
                }
                break;
            case NOT_EXPR:
                /* NOT will return null if its arg is null */
                result = find_nonnullable_rels_walker((Node*)expr->args, false);
                break;
            default:
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized boolop: %d", (int)expr->boolop)));
                break;
        }
    } else if (IsA(node, RelabelType)) {
        RelabelType* expr = (RelabelType*)node;

        result = find_nonnullable_rels_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, CoerceViaIO)) {
        /* not clear this is useful, but it can't hurt */
        CoerceViaIO* expr = (CoerceViaIO*)node;

        result = find_nonnullable_rels_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, ArrayCoerceExpr)) {
        /* ArrayCoerceExpr is strict at the array level */
        ArrayCoerceExpr* expr = (ArrayCoerceExpr*)node;

        result = find_nonnullable_rels_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, ConvertRowtypeExpr)) {
        /* not clear this is useful, but it can't hurt */
        ConvertRowtypeExpr* expr = (ConvertRowtypeExpr*)node;

        result = find_nonnullable_rels_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, CollateExpr)) {
        CollateExpr* expr = (CollateExpr*)node;

        result = find_nonnullable_rels_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, NullTest)) {
        /* IS NOT NULL can be considered strict, but only at top level */
        NullTest* expr = (NullTest*)node;

        if (top_level && expr->nulltesttype == IS_NOT_NULL && !expr->argisrow)
            result = find_nonnullable_rels_walker((Node*)expr->arg, false);
    } else if (IsA(node, BooleanTest)) {
        /* Boolean tests that reject NULL are strict at top level */
        BooleanTest* expr = (BooleanTest*)node;

        if (top_level &&
            (expr->booltesttype == IS_TRUE || expr->booltesttype == IS_FALSE || expr->booltesttype == IS_NOT_UNKNOWN))
            result = find_nonnullable_rels_walker((Node*)expr->arg, false);
    } else if (IsA(node, PlaceHolderVar)) {
        PlaceHolderVar* phv = (PlaceHolderVar*)node;

        /*
         * If the contained expression forces any rels non-nullable, so does
         * the PHV.
         */
        result = find_nonnullable_rels_walker((Node*)phv->phexpr, top_level);
        /*
         * If the PHV's syntactic scope is exactly one rel, it will be forced
         * to be evaluated at that rel, and so it will behave like a Var of
         * that rel: if the rel's entire output goes to null, so will the PHV.
         * (If the syntactic scope is a join, we know that the PHV will go to
         * null if the whole join does; but that is AND semantics while we
         * need OR semantics for find_nonnullable_rels' result, so we can't do
         * anything with the knowledge.)
         */
        if (phv->phlevelsup == 0 &&
            bms_membership(phv->phrels) == BMS_SINGLETON)
            result = bms_add_members(result, phv->phrels);

    }
    return result;
}

/*
 * find_nonnullable_vars
 *		Determine which Vars are forced nonnullable by given clause.
 *
 * Returns a list of all level-zero Vars that are referenced in the clause in
 * such a way that the clause cannot possibly return TRUE if any of these Vars
 * is NULL.  (It is OK to err on the side of conservatism; hence the analysis
 * here is simplistic.)
 *
 * The semantics here are subtly different from contain_nonstrict_functions:
 * that function is concerned with NULL results from arbitrary expressions,
 * but here we assume that the input is a Boolean expression, and wish to
 * see if NULL inputs will provably cause a FALSE-or-NULL result.  We expect
 * the expression to have been AND/OR flattened and converted to implicit-AND
 * format.
 *
 * The result is a palloc'd List, but we have not copied the member Var nodes.
 * Also, we don't bother trying to eliminate duplicate entries.
 *
 * top_level is TRUE while scanning top-level AND/OR structure; here, showing
 * the result is either FALSE or NULL is good enough.  top_level is FALSE when
 * we have descended below a NOT or a strict function: now we must be able to
 * prove that the subexpression goes to NULL.
 *
 * We don't use expression_tree_walker here because we don't want to descend
 * through very many kinds of nodes; only the ones we can be sure are strict.
 */
List* find_nonnullable_vars(Node* clause)
{
    return find_nonnullable_vars_walker(clause, true);
}

static List* find_nonnullable_vars_walker(Node* node, bool top_level)
{
    List* result = NIL;
    ListCell* l = NULL;

    if (node == NULL)
        return NIL;
    if (IsA(node, Var)) {
        Var* var = (Var*)node;

        if (var->varlevelsup == 0)
            result = list_make1(var);
    } else if (IsA(node, List)) {
        /*
         * At top level, we are examining an implicit-AND list: if any of the
         * arms produces FALSE-or-NULL then the result is FALSE-or-NULL. If
         * not at top level, we are examining the arguments of a strict
         * function: if any of them produce NULL then the result of the
         * function must be NULL.  So in both cases, the set of nonnullable
         * vars is the union of those found in the arms, and we pass down the
         * top_level flag unmodified.
         */
        foreach (l, (List*)node) {
            result = list_concat(result, find_nonnullable_vars_walker((Node*)lfirst(l), top_level));
        }
    } else if (IsA(node, FuncExpr)) {
        FuncExpr* expr = (FuncExpr*)node;

        if (func_strict(expr->funcid))
            result = find_nonnullable_vars_walker((Node*)expr->args, false);
    } else if (IsA(node, OpExpr)) {
        OpExpr* expr = (OpExpr*)node;

        set_opfuncid(expr);
        if (func_strict(expr->opfuncid))
            result = find_nonnullable_vars_walker((Node*)expr->args, false);
    } else if (IsA(node, ScalarArrayOpExpr)) {
        ScalarArrayOpExpr* expr = (ScalarArrayOpExpr*)node;

        if (is_strict_saop(expr, true))
            result = find_nonnullable_vars_walker((Node*)expr->args, false);
    } else if (IsA(node, BoolExpr)) {
        BoolExpr* expr = (BoolExpr*)node;

        switch (expr->boolop) {
            case AND_EXPR:
                /* At top level we can just recurse (to the List case) */
                if (top_level) {
                    result = find_nonnullable_vars_walker((Node*)expr->args, top_level);
                    break;
                }

                /*
                 * Below top level, even if one arm produces NULL, the result
                 * could be FALSE (hence not NULL).  However, if *all* the
                 * arms produce NULL then the result is NULL, so we can take
                 * the intersection of the sets of nonnullable vars, just as
                 * for OR.	Fall through to share code.
                 */
                /* FALL THRU */
            case OR_EXPR:

                /*
                 * OR is strict if all of its arms are, so we can take the
                 * intersection of the sets of nonnullable vars for each arm.
                 * This works for both values of top_level.
                 */
                foreach (l, expr->args) {
                    List* subresult = NIL;

                    subresult = find_nonnullable_vars_walker((Node*)lfirst(l), top_level);
                    if (result == NIL) /* first subresult? */
                        result = subresult;
                    else
                        result = list_intersection(result, subresult);

                    /*
                     * If the intersection is empty, we can stop looking. This
                     * also justifies the test for first-subresult above.
                     */
                    if (result == NIL)
                        break;
                }
                break;
            case NOT_EXPR:
                /* NOT will return null if its arg is null */
                result = find_nonnullable_vars_walker((Node*)expr->args, false);
                break;
            default:
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized boolop: %d", (int)expr->boolop)));
                break;
        }
    } else if (IsA(node, RelabelType)) {
        RelabelType* expr = (RelabelType*)node;

        result = find_nonnullable_vars_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, CoerceViaIO)) {
        /* not clear this is useful, but it can't hurt */
        CoerceViaIO* expr = (CoerceViaIO*)node;

        result = find_nonnullable_vars_walker((Node*)expr->arg, false);
    } else if (IsA(node, ArrayCoerceExpr)) {
        /* ArrayCoerceExpr is strict at the array level */
        ArrayCoerceExpr* expr = (ArrayCoerceExpr*)node;

        result = find_nonnullable_vars_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, ConvertRowtypeExpr)) {
        /* not clear this is useful, but it can't hurt */
        ConvertRowtypeExpr* expr = (ConvertRowtypeExpr*)node;

        result = find_nonnullable_vars_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, CollateExpr)) {
        CollateExpr* expr = (CollateExpr*)node;

        result = find_nonnullable_vars_walker((Node*)expr->arg, top_level);
    } else if (IsA(node, NullTest)) {
        /* IS NOT NULL can be considered strict, but only at top level */
        NullTest* expr = (NullTest*)node;

        if (top_level && expr->nulltesttype == IS_NOT_NULL && !expr->argisrow)
            result = find_nonnullable_vars_walker((Node*)expr->arg, false);
    } else if (IsA(node, BooleanTest)) {
        /* Boolean tests that reject NULL are strict at top level */
        BooleanTest* expr = (BooleanTest*)node;

        if (top_level &&
            (expr->booltesttype == IS_TRUE || expr->booltesttype == IS_FALSE || expr->booltesttype == IS_NOT_UNKNOWN))
            result = find_nonnullable_vars_walker((Node*)expr->arg, false);
    } else if (IsA(node, PlaceHolderVar)) {
        PlaceHolderVar* phv = (PlaceHolderVar*)node;

        result = find_nonnullable_vars_walker((Node*)phv->phexpr, top_level);
    }
    return result;
}

/*
 * find_forced_null_vars
 *		Determine which Vars must be NULL for the given clause to return TRUE.
 *
 * This is the complement of find_nonnullable_vars: find the level-zero Vars
 * that must be NULL for the clause to return TRUE.  (It is OK to err on the
 * side of conservatism; hence the analysis here is simplistic.  In fact,
 * we only detect simple "var IS NULL" tests at the top level.)
 *
 * The result is a palloc'd List, but we have not copied the member Var nodes.
 * Also, we don't bother trying to eliminate duplicate entries.
 */
List* find_forced_null_vars(Node* node)
{
    List* result = NIL;
    Var* var = NULL;
    ListCell* l = NULL;

    if (node == NULL)
        return NIL;
    /* Check single-clause cases using subroutine */
    var = find_forced_null_var(node);
    if (var != NULL) {
        result = list_make1(var);
    } else if (IsA(node, List)) { /* Otherwise, handle AND-conditions */
        /*
         * At top level, we are examining an implicit-AND list: if any of the
         * arms produces FALSE-or-NULL then the result is FALSE-or-NULL.
         */
        foreach (l, (List*)node) {
            result = list_concat(result, find_forced_null_vars((Node*)lfirst(l)));
        }
    } else if (IsA(node, BoolExpr)) {
        BoolExpr* expr = (BoolExpr*)node;

        /*
         * We don't bother considering the OR case, because it's fairly
         * unlikely anyone would write "v1 IS NULL OR v1 IS NULL". Likewise,
         * the NOT case isn't worth expending code on.
         */
        if (expr->boolop == AND_EXPR) {
            /* At top level we can just recurse (to the List case) */
            result = find_forced_null_vars((Node*)expr->args);
        }
    }
    return result;
}

/*
 * find_forced_null_var
 *		Return the Var forced null by the given clause, or NULL if it's
 *		not an IS NULL-type clause.  For success, the clause must enforce
 *		*only* nullness of the particular Var, not any other conditions.
 *
 * This is just the single-clause case of find_forced_null_vars(), without
 * any allowance for AND conditions.  It's used by initsplan.c on individual
 * qual clauses.  The reason for not just applying find_forced_null_vars()
 * is that if an AND of an IS NULL clause with something else were to somehow
 * survive AND/OR flattening, initsplan.c might get fooled into discarding
 * the whole clause when only the IS NULL part of it had been proved redundant.
 */
Var* find_forced_null_var(Node* node)
{
    if (node == NULL)
        return NULL;
    if (IsA(node, NullTest)) {
        /* check for var IS NULL */
        NullTest* expr = (NullTest*)node;

        if (expr->nulltesttype == IS_NULL && !expr->argisrow) {
            Var* var = (Var*)expr->arg;

            if (var && IsA(var, Var) && var->varlevelsup == 0)
                return var;
        }
    } else if (IsA(node, BooleanTest)) {
        /* var IS UNKNOWN is equivalent to var IS NULL */
        BooleanTest* expr = (BooleanTest*)node;

        if (expr->booltesttype == IS_UNKNOWN) {
            Var* var = (Var*)expr->arg;

            if (var && IsA(var, Var) && var->varlevelsup == 0)
                return var;
        }
    }
    return NULL;
}

/*
 * Can we treat a ScalarArrayOpExpr as strict?
 *
 * If "falseOK" is true, then a "false" result can be considered strict,
 * else we need to guarantee an actual NULL result for NULL input.
 *
 * "foo op ALL array" is strict if the op is strict *and* we can prove
 * that the array input isn't an empty array.  We can check that
 * for the cases of an array constant and an ARRAY[] construct.
 *
 * "foo op ANY array" is strict in the falseOK sense if the op is strict.
 * If not falseOK, the test is the same as for "foo op ALL array".
 */
static bool is_strict_saop(ScalarArrayOpExpr* expr, bool falseOK)
{
    Node* rightop = NULL;

    /* The contained operator must be strict. */
    set_sa_opfuncid(expr);
    if (!func_strict(expr->opfuncid))
        return false;
    /* If ANY and falseOK, that's all we need to check. */
    if (expr->useOr && falseOK)
        return true;
    /* Else, we have to see if the array is provably non-empty. */
    AssertEreport(list_length(expr->args) == 2, MOD_OPT, "");
    rightop = (Node*)lsecond(expr->args);
    if (rightop && IsA(rightop, Const)) {
        Datum arraydatum = ((Const*)rightop)->constvalue;
        bool arrayisnull = ((Const*)rightop)->constisnull;
        ArrayType* arrayval = NULL;
        int nitems;

        if (arrayisnull) {
            return false;
        }
        arrayval = DatumGetArrayTypeP(arraydatum);
        nitems = ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
        if (nitems > 0) {
            return true;
        }
    } else if (rightop && IsA(rightop, ArrayExpr)) {
        ArrayExpr* arrayexpr = (ArrayExpr*)rightop;

        if (arrayexpr->elements != NIL && !arrayexpr->multidims)
            return true;
    }
    return false;
}

/*****************************************************************************
 *		Check for "pseudo-constant" clauses
 *****************************************************************************/
/*
 * is_pseudo_constant_clause
 *	  Detect whether an expression is "pseudo constant", ie, it contains no
 *	  variables of the current query level and no uses of volatile functions.
 *	  Such an expr is not necessarily a true constant: it can still contain
 *	  Params and outer-level Vars, not to mention functions whose results
 *	  may vary from one statement to the next.	However, the expr's value
 *	  will be constant over any one scan of the current query, so it can be
 *	  used as, eg, an indexscan key.
 *
 * CAUTION: this function omits to test for one very important class of
 * not-constant expressions, namely aggregates (Aggrefs).  In current usage
 * this is only applied to WHERE clauses and so a check for Aggrefs would be
 * a waste of cycles; but be sure to also check contain_agg_clause() if you
 * want to know about pseudo-constness in other contexts.  The same goes
 * for window functions (WindowFuncs).
 */
bool is_pseudo_constant_clause(Node* clause)
{
    /*
     * We could implement this check in one recursive scan.  But since the
     * check for volatile functions is both moderately expensive and unlikely
     * to fail, it seems better to look for Vars first and only check for
     * volatile functions if we find no Vars.
     */
    if (!contain_var_clause(clause) && !contain_volatile_functions(clause))
        return true;
    return false;
}

/*
 * is_pseudo_constant_clause_relids
 *	  Same as above, except caller already has available the var membership
 *	  of the expression; this lets us avoid the contain_var_clause() scan.
 */
bool is_pseudo_constant_clause_relids(Node* clause, Relids relids)
{
    if (bms_is_empty(relids) && !contain_volatile_functions(clause))
        return true;
    return false;
}

/*****************************************************************************
 *																			 *
 *		General clause-manipulating routines								 *
 *																			 *
 *****************************************************************************/
/*
 * NumRelids
 *		(formerly clause_relids)
 *
 * Returns the number of different relations referenced in 'clause'.
 */
int NumRelids(Node* clause)
{
    Relids varnos = pull_varnos(clause);
    int result = bms_num_members(varnos);

    bms_free_ext(varnos);
    return result;
}

/*
 * CommuteOpExpr: commute a binary operator clause
 *
 * XXX the clause is destructively modified!
 */
void CommuteOpExpr(OpExpr* clause)
{
    Oid opoid;
    Node* temp = NULL;

    /* Sanity checks: caller is at fault if these fail */
    if (!is_opclause(clause) || list_length(clause->args) != 2)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                errmsg("cannot commute non-binary-operator clause")));

    opoid = get_commutator(clause->opno);
    if (!OidIsValid(opoid))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                errmsg("could not find commutator for operator %u", clause->opno)));

    /*
     * modify the clause in-place!
     */
    clause->opno = opoid;
    clause->opfuncid = InvalidOid;
    /* opresulttype, opretset, opcollid, inputcollid need not change */
    temp = (Node*)linitial(clause->args);
    linitial(clause->args) = lsecond(clause->args);
    lsecond(clause->args) = temp;
}

/*
 * CommuteRowCompareExpr: commute a RowCompareExpr clause
 *
 * XXX the clause is destructively modified!
 */
void CommuteRowCompareExpr(RowCompareExpr* clause)
{
    List* newops = NIL;
    List* temp = NIL;
    ListCell* l = NULL;

    /* Sanity checks: caller is at fault if these fail */
    if (!IsA(clause, RowCompareExpr))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                errmsg("expected a RowCompareExpr for para clause")));

    /* Build list of commuted operators */
    newops = NIL;
    foreach (l, clause->opnos) {
        Oid opoid = lfirst_oid(l);

        opoid = get_commutator(opoid);
        if (!OidIsValid(opoid))
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                    errmsg("could not find commutator for operator %u", lfirst_oid(l))));
        newops = lappend_oid(newops, opoid);
    }

    /*
     * modify the clause in-place!
     */
    switch (clause->rctype) {
        case ROWCOMPARE_LT:
            clause->rctype = ROWCOMPARE_GT;
            break;
        case ROWCOMPARE_LE:
            clause->rctype = ROWCOMPARE_GE;
            break;
        case ROWCOMPARE_GE:
            clause->rctype = ROWCOMPARE_LE;
            break;
        case ROWCOMPARE_GT:
            clause->rctype = ROWCOMPARE_LT;
            break;
        default:
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unexpected RowCompare type: %d", (int)clause->rctype)));
            break;
    }

    clause->opnos = newops;

    /*
     * Note: we need not change the opfamilies list; we assume any btree
     * opfamily containing an operator will also contain its commutator.
     * Collations don't change either.
     */
    temp = clause->largs;
    clause->largs = clause->rargs;
    clause->rargs = temp;
}

/*
 * strip_implicit_coercions: remove implicit coercions at top level of tree
 *
 * Note: there isn't any useful thing we can do with a RowExpr here, so
 * just return it unchanged, even if it's marked as an implicit coercion.
 */
Node* strip_implicit_coercions(Node* node)
{
    if (node == NULL)
        return NULL;
    if (IsA(node, FuncExpr)) {
        FuncExpr* f = (FuncExpr*)node;

        if (f->funcformat == COERCE_IMPLICIT_CAST)
            return strip_implicit_coercions((Node*)linitial(f->args));
    } else if (IsA(node, RelabelType)) {
        RelabelType* r = (RelabelType*)node;

        if (r->relabelformat == COERCE_IMPLICIT_CAST)
            return strip_implicit_coercions((Node*)r->arg);
    } else if (IsA(node, CoerceViaIO)) {
        CoerceViaIO* c = (CoerceViaIO*)node;

        if (c->coerceformat == COERCE_IMPLICIT_CAST)
            return strip_implicit_coercions((Node*)c->arg);
    } else if (IsA(node, ArrayCoerceExpr)) {
        ArrayCoerceExpr* c = (ArrayCoerceExpr*)node;

        if (c->coerceformat == COERCE_IMPLICIT_CAST)
            return strip_implicit_coercions((Node*)c->arg);
    } else if (IsA(node, ConvertRowtypeExpr)) {
        ConvertRowtypeExpr* c = (ConvertRowtypeExpr*)node;

        if (c->convertformat == COERCE_IMPLICIT_CAST)
            return strip_implicit_coercions((Node*)c->arg);
    } else if (IsA(node, CoerceToDomain)) {
        CoerceToDomain* c = (CoerceToDomain*)node;

        if (c->coercionformat == COERCE_IMPLICIT_CAST)
            return strip_implicit_coercions((Node*)c->arg);
    }
    return node;
}

/*
 * Helper for eval_const_expressions: check that datatype of an attribute
 * is still what it was when the expression was parsed.  This is needed to
 * guard against improper simplification after ALTER COLUMN TYPE.  (XXX we
 * may well need to make similar checks elsewhere?)
 */
static bool rowtype_field_matches(
    Oid rowtypeid, int fieldnum, Oid expectedtype, int32 expectedtypmod, Oid expectedcollation)
{
    TupleDesc tupdesc;
    Form_pg_attribute attr;

    /* No issue for RECORD, since there is no way to ALTER such a type */
    if (rowtypeid == RECORDOID)
        return true;
    tupdesc = lookup_rowtype_tupdesc(rowtypeid, -1);
    if (fieldnum <= 0 || fieldnum > tupdesc->natts) {
        ReleaseTupleDesc(tupdesc);
        return false;
    }
    attr = &tupdesc->attrs[fieldnum - 1];
    if (attr->attisdropped || attr->atttypid != expectedtype || attr->atttypmod != expectedtypmod ||
        attr->attcollation != expectedcollation) {
        ReleaseTupleDesc(tupdesc);
        return false;
    }
    ReleaseTupleDesc(tupdesc);
    return true;
}

static Node *GetNullExprFromRowExpr(RowExpr *rarg, NullTest *ntest)
{
    List *newargs = NIL;
    ListCell *l = NULL;
    NullTest* newntest = NULL;

    foreach (l, rarg->args) {
        Node *relem = (Node *)lfirst(l);

        /*
         * A constant field refutes the whole NullTest if it's
         * of the wrong nullness; else we can discard it.
         */
        if (relem && IsA(relem, Const)) {
            Const *carg = (Const *)relem;

            if (carg->constisnull ? (ntest->nulltesttype == IS_NOT_NULL) : (ntest->nulltesttype == IS_NULL)) {
                return makeBoolConst(false, false);
            }
            continue;
        }
        /*
         * Else, make a scalar (argisrow == false) NullTest
         * for this field.  Scalar semantics are required
         * because IS [NOT] NULL doesn't recurse; see comments
         * in ExecEvalNullTest().
         */
        newntest = makeNode(NullTest);
        newntest->arg = (Expr *)relem;
        newntest->nulltesttype = ntest->nulltesttype;
        newntest->argisrow = false;
        newargs = lappend(newargs, newntest);
    }
    /* If all the inputs were constants, result is TRUE */
    if (newargs == NIL) {
        return makeBoolConst(true, false);
    }
    /* If only one nonconst input, it's the result */
    if (list_length(newargs) == 1) {
        return (Node *)linitial(newargs);
    }
    /* Else we need an AND node */
    return (Node *)make_andclause(newargs);
}

static Node *GetNullExprFromRowExprForAFormat(RowExpr *rarg, NullTest *ntest)
{
    List *newargs = NIL;
    ListCell *l = NULL;
    NullTest* newntest = NULL;

    foreach (l, rarg->args) {
        Node *relem = (Node *)lfirst(l);

        /*
         * A constant field refutes the whole NullTest if it's
         * of the wrong nullness; else we can discard it.
         */
        if (relem && IsA(relem, Const)) {
            Const *carg = (Const *)relem;

            if (!(carg->constisnull)) {
                if (ntest->nulltesttype == IS_NOT_NULL) {
                    return makeBoolConst(true, false);
                }
                if (ntest->nulltesttype == IS_NULL) {
                    return makeBoolConst(false, false);
                }
            }
            continue;
        }
        /*
         * Else, make a scalar (argisrow == false) NullTest
         * for this field.  Scalar semantics are required
         * because IS [NOT] NULL doesn't recurse; see comments
         * in ExecEvalNullTest().
         */
        newntest = makeNode(NullTest);
        newntest->arg = (Expr *)relem;
        newntest->nulltesttype = IS_NULL;
        newntest->argisrow = false;
        newargs = lappend(newargs, newntest);
    }
    /* If all the inputs were constants, result is TRUE */
    if (newargs == NIL) {
        if (ntest->nulltesttype == IS_NOT_NULL) {
            return makeBoolConst(false, false);
        }
        if (ntest->nulltesttype == IS_NULL) {
            return makeBoolConst(true, false);
        }
    }
    /* If only one nonconst input, it's the result */
    if (list_length(newargs) == 1)
        return (Node *)linitial(newargs);
    if (ntest->nulltesttype == IS_NULL) {
        /* Else we need an AND node */
        return (Node *)make_andclause(newargs);
    } else {
        return (Node *)make_notclause((Expr *)make_andclause(newargs));
    }
}

/* --------------------
 * eval_const_expressions
 *
 * Reduce any recognizably constant subexpressions of the given
 * expression tree, for example "2 + 2" => "4".  More interestingly,
 * we can reduce certain boolean expressions even when they contain
 * non-constant subexpressions: "x OR true" => "true" no matter what
 * the subexpression x is.	(XXX We assume that no such subexpression
 * will have important side-effects, which is not necessarily a good
 * assumption in the presence of user-defined functions; do we need a
 * pg_proc flag that prevents discarding the execution of a function?)
 *
 * We do understand that certain functions may deliver non-constant
 * results even with constant inputs, "nextval()" being the classic
 * example.  Functions that are not marked "immutable" in pg_proc
 * will not be pre-evaluated here, although we will reduce their
 * arguments as far as possible.
 *
 * Whenever a function is eliminated from the expression by means of
 * constant-expression evaluation or inlining, we add the function to
 * root->glob->invalItems.	This ensures the plan is known to depend on
 * such functions, even though they aren't referenced anymore.
 *
 * We assume that the tree has already been type-checked and contains
 * only operators and functions that are reasonable to try to execute.
 *
 * NOTE: "root" can be passed as NULL if the caller never wants to do any
 * Param substitutions nor receive info about inlined functions.
 *
 * NOTE: the planner assumes that this will always flatten nested AND and
 * OR clauses into N-argument form.  See comments in prepqual.c.
 *
 * NOTE: another critical effect is that any function calls that require
 * default arguments will be expanded, and named-argument calls will be
 * converted to positional notation.  The executor won't handle either.
 * --------------------
 */
Node* eval_const_expressions(PlannerInfo* root, Node* node)
{
    eval_const_expressions_context context;

    if (root != NULL)
        context.boundParams = root->glob->boundParams; /* bound Params */
    else
        context.boundParams = NULL;
    context.root = root;      /* for inlined-function dependencies */
    context.active_fns = NIL; /* nothing being recursively simplified */
    context.case_val = NULL;  /* no CASE being examined */
    context.estimate = false; /* safe transformations only */
    return eval_const_expressions_mutator(node, &context);
}

Node* eval_const_expressions_params(PlannerInfo* root, Node* node, ParamListInfo boundParams)
{
    eval_const_expressions_context context;

    context.boundParams = boundParams;
    context.root = root;      /* for inlined-function dependencies */
    context.active_fns = NIL; /* nothing being recursively simplified */
    context.case_val = NULL;  /* no CASE being examined */
    context.estimate = false; /* safe transformations only */
    return eval_const_expressions_mutator(node, &context);
}

/*
 * eval_const_expression_value
 * only for set user_defined variables scenes.
 * the difference between eval_const_expression_value and eval_const_expressions_params is
 * that estimate is set to true.
 */
Node *eval_const_expression_value(PlannerInfo* root, Node* node, ParamListInfo boundParams)
{
    eval_const_expressions_context context;

    context.boundParams = boundParams;
    context.root = root;      /* for inlined-function dependencies */
    context.active_fns = NIL; /* nothing being recursively simplified */
    context.case_val = NULL;  /* no CASE being examined */
    context.estimate = true; /* unsafe transformations OK */

    return eval_const_expressions_mutator(node, &context);
}

/*--------------------
 * convert_saop_to_hashed_saop
 *
 * Recursively search 'node' for ScalarArrayOpExprs and fill in the hash
 * function for any ScalarArrayOpExpr that looks like it would be useful to
 * evaluate using a hash table rather than a linear search.
 *
 * We'll use a hash table if all of the following conditions are met:
 * 1. The 2nd argument of the array contain only Consts.
 * 2. useOr is true or there is a valid negator operator for the
 *	  ScalarArrayOpExpr's opno.
 * 3. There's valid hash function for both left and righthand operands and
 *	  these hash functions are the same.
 * 4. If the array contains enough elements for us to consider it to be
 *	  worthwhile using a hash table rather than a linear search.
 */
void convert_saop_to_hashed_saop(Node *node)
{
    (void)convert_saop_to_hashed_saop_walker(node);
}

static bool convert_saop_to_hashed_saop_walker(Node *node)
{
    constexpr int minArraySizeForHashedSaop = 9;

    if (node == NULL) {
        return false;
    }

    if (IsA(node, ScalarArrayOpExpr)) {
        ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *)node;
        Expr *arrayarg = (Expr *)lsecond(saop->args);
        Oid lefthashfunc;
        Oid righthashfunc;

        if (arrayarg && IsA(arrayarg, Const) && !((Const *)arrayarg)->constisnull) {
            if (saop->useOr) {
                Oid hashfunc = get_op_hash_functions(saop->opno, &lefthashfunc, &righthashfunc);
                if (hashfunc && lefthashfunc == righthashfunc) {
                    Datum arrdatum = ((Const *)arrayarg)->constvalue;
                    ArrayType *arr = (ArrayType *)DatumGetPointer(arrdatum);
                    int nitems;

                    /*
                     * Only fill in the hash functions if the array looks
                     * large enough for it to be worth hashing instead of
                     * doing a linear search.
                     */
                    nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
                    if (nitems >= minArraySizeForHashedSaop) {
                        /* Looks good. Fill in the hash functions */
                        saop->hashfuncid = lefthashfunc;
                        return true;
                    }
                    return false;
                }
            } else {
                /* !saop->useOr */
                Oid negator = get_negator(saop->opno);
                /*
                 * Check if this is a NOT IN using an operator whose negator
                 * is hashable.  If so we can still build a hash table and
                 * just ensure the lookup items are not in the hash table.
                 */
                if (OidIsValid(negator) && get_op_hash_functions(negator, &lefthashfunc, &righthashfunc) &&
                    lefthashfunc == righthashfunc) {
                    Datum arrdatum = ((Const *)arrayarg)->constvalue;
                    ArrayType *arr = (ArrayType *)DatumGetPointer(arrdatum);
                    int nitems;

                    /*
                     * Only fill in the hash functions if the array looks
                     * large enough for it to be worth hashing instead of
                     * doing a linear search.
                     */
                    nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
                    if (nitems >= minArraySizeForHashedSaop) {
                        /* Looks good. Fill in the hash functions */
                        saop->hashfuncid = lefthashfunc;

                        /*
                         * Also set the negfuncid.  The executor will need
                         * that to perform hashtable lookups.
                         */
                        saop->negfuncid = get_opcode(negator);
                        return true;
                    }
                    return false;
                }
            }
        }
    }

    return expression_tree_walker(node, (bool (*)())convert_saop_to_hashed_saop_walker, (void *)NULL);
}

/* --------------------
 * estimate_expression_value
 *
 * This function attempts to estimate the value of an expression for
 * planning purposes.  It is in essence a more aggressive version of
 * eval_const_expressions(): we will perform constant reductions that are
 * not necessarily 100% safe, but are reasonable for estimation purposes.
 *
 * Currently the extra steps that are taken in this mode are:
 * 1. Substitute values for Params, where a bound Param value has been made
 *	  available by the caller of planner(), even if the Param isn't marked
 *	  constant.  This effectively means that we plan using the first supplied
 *	  value of the Param.
 * 2. Fold stable, as well as immutable, functions to constants.
 * 3. Reduce PlaceHolderVar nodes to their contained expressions.
 * --------------------
 */
Node* estimate_expression_value(PlannerInfo* root, Node* node, EState* estate)
{
    eval_const_expressions_context context;

    if (estate == NULL) {
        context.boundParams = root->glob->boundParams; /* bound Params */
    } else {
        context.boundParams = estate->es_param_list_info; /* bound Params */
    }
    /* we do not need to mark the plan as depending on inlined functions */
    context.root = NULL;
    context.active_fns = NIL; /* nothing being recursively simplified */
    context.case_val = NULL;  /* no CASE being examined */
    context.estimate = true;  /* unsafe transformations OK */
    return eval_const_expressions_mutator(node, &context);
}

/* --------------------
 * simplify_select_into_expression
 *
 * Only for select ... into varlist statement.
 *
 * This function attempts to bind the value of parameter
 * in the part of Select part without into_clause
 * for the following process to calculate the value.
 * --------------------
 */
Node* simplify_select_into_expression(Node* node, ParamListInfo boundParams, int *targetlist_len)
{
    eval_const_expressions_context context;

    context.boundParams = boundParams;
    context.root = NULL;      /* for inlined-function dependencies */
    context.active_fns = NIL; /* nothing being recursively simplified */
    context.case_val = NULL;  /* no CASE being examined */
    context.estimate = true; /* unsafe transformations OK */

    Query *qt = (Query *)((SubLink *)node)->subselect;
    List *fromList = qt->jointree->fromlist;
    ListCell *fc = NULL;
    foreach (fc, fromList) {
        Node *jtnode = (Node *)lfirst(fc);
        if (IsA(jtnode, RangeTblRef)) {
            int varno = ((RangeTblRef *)jtnode)->rtindex;
            RangeTblEntry *rte = rt_fetch(varno, qt->rtable);
            if (rte->funcexpr != NULL && IsA(rte->funcexpr, FuncExpr)) {
                rte->funcexpr = eval_const_expressions_mutator((Node *)rte->funcexpr, &context);
            }
        }
    }

    ListCell *lc = NULL;
    List *newTargetList = NIL;
    foreach (lc, qt->targetList) {
        TargetEntry *te = (TargetEntry *)lfirst(lc);
        if (!te->resjunk) {
            (*targetlist_len)++; 
        }
        Node *tn = (Node *)te->expr;
        tn = eval_const_expressions_mutator(tn, &context);
        te->expr = (Expr *)tn;
        newTargetList = lappend(newTargetList, te);
    }
    qt->targetList = newTargetList;
    return node;
}

Node* eval_const_expressions_mutator(Node* node, eval_const_expressions_context* context)
{
    if (node == NULL)
        return NULL;
    switch (nodeTag(node)) {
        case T_Param: {
            Param* param = (Param*)node;

            if (ENABLE_CACHEDPLAN_MGR && context->boundParams != NULL && context->boundParams->params_lazy_bind) {
                return (Node *)copyObject(param);
            }
            /* Look to see if we've been given a value for this Param */
            if (param->paramkind == PARAM_EXTERN && context->boundParams != NULL && param->paramid > 0 &&
                param->paramid <= context->boundParams->numParams) {
                ParamListInfo paramInfo = context->boundParams;
                ParamExternData* prm = &paramInfo->params[param->paramid - 1];

                /* give hook a chance in case parameter in dynamic */
                if (!OidIsValid(prm->ptype) && paramInfo->paramFetch != NULL) {
                    (*paramInfo->paramFetch)(paramInfo, param->paramid);
                }

                if (OidIsValid(prm->ptype)) {
                    /* OK to substitute parameter value? */
                    if (context->estimate || (prm->pflags & PARAM_FLAG_CONST)) {
                        /*
                         * Return a Const representing the param value.
                         * Must copy pass-by-ref datatypes, since the
                         * Param might be in a memory context
                         * shorter-lived than our output plan should be.
                         */
                        int16 typLen;
                        bool typByVal = false;
                        Datum pval;

                        AssertEreport(prm->ptype == param->paramtype, MOD_OPT, "");
                        get_typlenbyval(param->paramtype, &typLen, &typByVal);
                        if (prm->isnull || typByVal)
                            pval = prm->value;
                        else
                            pval = datumCopy(prm->value, typByVal, typLen);
                        return (Node*)makeConst(param->paramtype,
                            param->paramtypmod,
                            param->paramcollid,
                            (int)typLen,
                            pval,
                            prm->isnull,
                            typByVal,
                            &prm->cursor_data);
                    }
                }
            }

            /*
             * Not replaceable, so just copy the Param (no need to
             * recurse)
             */
            return (Node*)copyObject(param);
        }
        case T_WindowFunc: {
            WindowFunc* expr = (WindowFunc*)node;
            Oid funcid = expr->winfnoid;

            /*
             * We can't really simplify a WindowFunc node, but we mustn't
             * just fall through to the default processing, because we
             * have to apply expand_function_arguments to its argument
             * list.  That takes care of inserting default arguments and
             * expanding named-argument notation.
             */
            HeapTuple func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
            if (!HeapTupleIsValid(func_tuple))
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                        (errmsg("cache lookup failed for function %u", funcid))));

            List* args = expand_function_arguments(expr->args, expr->wintype, func_tuple);

            ReleaseSysCache(func_tuple);

            /* Now, recursively simplify the args (which are a List) */
            args = (List*)expression_tree_mutator(
                (Node*)args, (Node* (*)(Node*, void*)) eval_const_expressions_mutator, (void*)context);

            /* And build the replacement WindowFunc node */
            WindowFunc* newexpr = makeNode(WindowFunc);
            newexpr->winfnoid = expr->winfnoid;
            newexpr->wintype = expr->wintype;
            newexpr->wincollid = expr->wincollid;
            newexpr->inputcollid = expr->inputcollid;
            newexpr->args = args;
            newexpr->winref = expr->winref;
            newexpr->winstar = expr->winstar;
            newexpr->winagg = expr->winagg;
            newexpr->keep_args = (List*)expression_tree_mutator(
                (Node*)expr->keep_args, (Node* (*)(Node*, void*)) eval_const_expressions_mutator, (void*)context);
            newexpr->winkporder = (List*)expression_tree_mutator(
                (Node*)expr->winkporder, (Node* (*)(Node*, void*)) eval_const_expressions_mutator, (void*)context);
            newexpr->winkpfirst = expr->winkpfirst;
            newexpr->is_from_last = expr->is_from_last;
            newexpr->is_ignore_nulls = expr->is_ignore_nulls;
            newexpr->location = expr->location;

            return (Node*)newexpr;
        }
        case T_FuncExpr: {
            FuncExpr* expr = (FuncExpr*)node;
            List* args = expr->args;
            Expr* simple = NULL;
            FuncExpr* newexpr = NULL;

            /*
             * Code for op/func reduction is pretty bulky, so split it out
             * as a separate function.	Note: exprTypmod normally returns
             * -1 for a FuncExpr, but not when the node is recognizably a
             * length coercion; we want to preserve the typmod in the
             * eventual Const if so.
             */
            simple = simplify_function(expr->funcid,
                expr->funcresulttype,
                exprTypmod(node),
                expr->funccollid,
                expr->inputcollid,
                &args,
                true,
                true,
                context);
            if (simple != NULL) /* successfully simplified it */
                return (Node*)simple;

            /*
             * The expression cannot be simplified any further, so build
             * and return a replacement FuncExpr node using the
             * possibly-simplified arguments.  Note that we have also
             * converted the argument list to positional notation.
             */
            newexpr = makeNode(FuncExpr);
            newexpr->funcid = expr->funcid;
            newexpr->funcresulttype = expr->funcresulttype;
            newexpr->funcresulttype_orig = expr->funcresulttype_orig;
            newexpr->funcretset = expr->funcretset;
            newexpr->funcvariadic = expr->funcvariadic;
            newexpr->funcformat = expr->funcformat;
            newexpr->funccollid = expr->funccollid;
            newexpr->inputcollid = expr->inputcollid;
            newexpr->args = args;
            newexpr->fmtstr = expr->fmtstr;
            newexpr->nlsfmtstr = expr->nlsfmtstr;
            newexpr->location = expr->location;
            return (Node*)newexpr;
        }
        case T_OpExpr: {
            OpExpr* expr = (OpExpr*)node;
            List* args = expr->args;
            Expr* simple = NULL;
            OpExpr* newexpr = NULL;

            /*
             * Need to get OID of underlying function.	Okay to scribble
             * on input to this extent.
             */
            set_opfuncid(expr);
            /*
             * Code for op/func reduction is pretty bulky, so split it out
             * as a separate function.
             */
            simple = simplify_function(
                expr->opfuncid, expr->opresulttype, -1, expr->opcollid, expr->inputcollid, &args, true, true, context);
            if (simple != NULL) /* successfully simplified it */
                return (Node*)simple;

            /*
             * If the operator is boolean equality or inequality, we know
             * how to simplify cases involving one constant and one
             * non-constant argument.
             */
            if (expr->opno == BooleanEqualOperator || expr->opno == BooleanNotEqualOperator) {
                simple = (Expr*)simplify_boolean_equality(expr->opno, args);
                if (simple != NULL) /* successfully simplified it */
                    return (Node*)simple;
            }

            /*
             * The expression cannot be simplified any further, so build
             * and return a replacement OpExpr node using the
             * possibly-simplified arguments.
             */
            newexpr = makeNode(OpExpr);
            newexpr->opno = expr->opno;
            newexpr->opfuncid = expr->opfuncid;
            newexpr->opresulttype = expr->opresulttype;
            newexpr->opretset = expr->opretset;
            newexpr->opcollid = expr->opcollid;
            newexpr->inputcollid = expr->inputcollid;
            newexpr->args = args;
            newexpr->location = expr->location;
            return (Node*)newexpr;
        }
        case T_DistinctExpr: {
            DistinctExpr* expr = (DistinctExpr*)node;
            List* args = NIL;
            ListCell* arg = NULL;
            bool has_null_input = false;
            bool all_null_input = true;
            bool has_nonconst_input = false;
            Expr* simple = NULL;
            DistinctExpr* newexpr = NULL;

            /*
             * Reduce constants in the DistinctExpr's arguments.  We know
             * args is either NIL or a List node, so we can call
             * expression_tree_mutator directly rather than recursing to
             * self.
             */
            args = (List*)expression_tree_mutator(
                (Node*)expr->args, (Node* (*)(Node*, void*)) eval_const_expressions_mutator, (void*)context);

            /*
             * We must do our own check for NULLs because DistinctExpr has
             * different results for NULL input than the underlying
             * operator does.
             */
            foreach (arg, args) {
                if (IsA(lfirst(arg), Const)) {
                    has_null_input = has_null_input || ((Const*)lfirst(arg))->constisnull;
                    all_null_input = all_null_input && ((Const*)lfirst(arg))->constisnull;
                } else
                    has_nonconst_input = true;
            }

            /* all constants? then can optimize this out */
            if (!has_nonconst_input) {
                /* all nulls? then not distinct */
                if (all_null_input)
                    return makeBoolConst(false, false);

                /* one null? then distinct */
                if (has_null_input)
                    return makeBoolConst(true, false);

                /* otherwise try to evaluate the '=' operator */
                /* (NOT okay to try to inline it, though!) */
                /*
                 * Need to get OID of underlying function.	Okay to
                 * scribble on input to this extent.
                 * rely on struct equivalence
                 */
                set_opfuncid((OpExpr*)expr);

                /*
                 * Code for op/func reduction is pretty bulky, so split it
                 * out as a separate function.
                 */
                simple = simplify_function(expr->opfuncid,
                    expr->opresulttype,
                    -1,
                    expr->opcollid,
                    expr->inputcollid,
                    &args,
                    false,
                    false,
                    context);
                if (simple != NULL) { /* successfully simplified it */
                    /*
                     * Since the underlying operator is "=", must negate
                     * its result
                     */
                    Const* csimple = (Const*)simple;

                    AssertEreport(IsA(csimple, Const), MOD_OPT, "");
                    csimple->constvalue = BoolGetDatum(!DatumGetBool(csimple->constvalue));
                    return (Node*)csimple;
                }
            }

            /*
             * The expression cannot be simplified any further, so build
             * and return a replacement DistinctExpr node using the
             * possibly-simplified arguments.
             */
            newexpr = makeNode(DistinctExpr);
            newexpr->opno = expr->opno;
            newexpr->opfuncid = expr->opfuncid;
            newexpr->opresulttype = expr->opresulttype;
            newexpr->opretset = expr->opretset;
            newexpr->opcollid = expr->opcollid;
            newexpr->inputcollid = expr->inputcollid;
            newexpr->args = args;
            newexpr->location = expr->location;
            return (Node*)newexpr;
        }
        case T_BoolExpr: {
            BoolExpr* expr = (BoolExpr*)node;

            switch (expr->boolop) {
                case OR_EXPR: {
                    List* newargs = NIL;
                    bool haveNull = false;
                    bool forceTrue = false;

                    newargs = simplify_or_arguments(expr->args, context, &haveNull, &forceTrue);
                    if (forceTrue)
                        return makeBoolConst(true, false);
                    if (haveNull)
                        newargs = lappend(newargs, makeBoolConst(false, true));
                    /* If all the inputs are FALSE, result is FALSE */
                    if (newargs == NIL)
                        return makeBoolConst(false, false);

                    /*
                     * If only one nonconst-or-NULL input, it's the
                     * result
                     */
                    if (list_length(newargs) == 1)
                        return (Node*)linitial(newargs);
                    /* Else we still need an OR node */
                    return (Node*)make_orclause(newargs);
                }
                case AND_EXPR: {
                    List* newargs = NIL;
                    bool haveNull = false;
                    bool forceFalse = false;

                    newargs = simplify_and_arguments(expr->args, context, &haveNull, &forceFalse);
                    if (forceFalse)
                        return makeBoolConst(false, false);
                    if (haveNull)
                        newargs = lappend(newargs, makeBoolConst(false, true));
                    /* If all the inputs are TRUE, result is TRUE */
                    if (newargs == NIL)
                        return makeBoolConst(true, false);

                    /*
                     * If only one nonconst-or-NULL input, it's the
                     * result
                     */
                    if (list_length(newargs) == 1)
                        return (Node*)linitial(newargs);
                    /* Else we still need an AND node */
                    return (Node*)make_andclause(newargs);
                }
                case NOT_EXPR: {
                    Node* arg = NULL;

                    AssertEreport(list_length(expr->args) == 1, MOD_OPT, "");
                    arg = eval_const_expressions_mutator((Node*)linitial(expr->args), context);

                    /*
                     * Use negate_clause() to see if we can simplify
                     * away the NOT.
                     */
                    return negate_clause(arg);
                }
                default:
                    ereport(ERROR,
                        (errmodule(MOD_OPT),
                            errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                            errmsg("unrecognized boolop: %d", (int)expr->boolop)));
                    break;
            }
            break;
        }
        case T_SubPlan:
        case T_AlternativeSubPlan:

            /*
             * Return a SubPlan unchanged --- too late to do anything with it.
             *
             * XXX should we ereport() here instead?  Probably this routine
             * should never be invoked after SubPlan creation.
             */
            return node;
        case T_RelabelType: {
            /*
             * If we can simplify the input to a constant, then we don't
             * need the RelabelType node anymore: just change the type
             * field of the Const node.  Otherwise, must copy the
             * RelabelType node.
             */
            RelabelType* relabel = (RelabelType*)node;
            Node* arg = NULL;

            arg = eval_const_expressions_mutator((Node*)relabel->arg, context);

            /*
             * If we find stacked RelabelTypes (eg, from foo :: int ::
             * oid) we can discard all but the top one.
             */
            while (arg && IsA(arg, RelabelType))
                arg = (Node*)((RelabelType*)arg)->arg;

            if (arg && IsA(arg, Const)) {
                Const* con = (Const*)arg;

                con->consttype = relabel->resulttype;
                con->consttypmod = relabel->resulttypmod;
                con->constcollid = relabel->resultcollid;
                return (Node*)con;
            } else {
                RelabelType* newrelabel = makeNode(RelabelType);

                newrelabel->arg = (Expr*)arg;
                newrelabel->resulttype = relabel->resulttype;
                newrelabel->resulttypmod = relabel->resulttypmod;
                newrelabel->resultcollid = relabel->resultcollid;
                newrelabel->relabelformat = relabel->relabelformat;
                newrelabel->location = relabel->location;
                return (Node*)newrelabel;
            }
        }
        case T_CoerceViaIO: {
            CoerceViaIO* expr = (CoerceViaIO*)node;
            List* args = NIL;
            Oid outfunc;
            bool outtypisvarlena = false;
            Oid infunc;
            Oid intypioparam;
            Expr* simple = NULL;
            CoerceViaIO* newexpr = NULL;

            /* Make a List so we can use simplify_function */
            args = list_make1(expr->arg);

            /*
             * CoerceViaIO represents calling the source type's output
             * function then the result type's input function.  So, try to
             * simplify it as though it were a stack of two such function
             * calls.  First we need to know what the functions are.
             *
             * Note that the coercion functions are assumed not to care
             * about input collation, so we just pass InvalidOid for that.
             */
            getTypeOutputInfo(exprType((Node*)expr->arg), &outfunc, &outtypisvarlena);
            getTypeInputInfo(expr->resulttype, &infunc, &intypioparam);

            simple = simplify_function(outfunc, CSTRINGOID, -1, InvalidOid, InvalidOid, &args, true, true, context);
            if (simple != NULL) { /* successfully simplified output fn */
                /*               
                 * Input functions may want 1 to 3 arguments.  We always
                 * supply all three, trusting that nothing downstream will
                 * complain.
                 */
                args = list_make3(simple,
                    makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(intypioparam), false, true),
                    makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(-1), false, true));

                simple = simplify_function(
                    infunc, expr->resulttype, -1, expr->resultcollid, InvalidOid, &args, false, true, context);
                if (simple != NULL) /* successfully simplified input fn */
                    return (Node*)simple;
            }

            /*
             * The expression cannot be simplified any further, so build
             * and return a replacement CoerceViaIO node using the
             * possibly-simplified argument.
             */
            newexpr = makeNode(CoerceViaIO);
            newexpr->arg = (Expr*)linitial(args);
            newexpr->fmtstr = expr->fmtstr;
            newexpr->nlsfmtstr = expr->nlsfmtstr;
            newexpr->resulttype = expr->resulttype;
            newexpr->resultcollid = expr->resultcollid;
            newexpr->coerceformat = expr->coerceformat;
            newexpr->location = expr->location;
            return (Node*)newexpr;
        }
        case T_ArrayCoerceExpr: {
            ArrayCoerceExpr* expr = (ArrayCoerceExpr*)node;
            Expr* arg = NULL;
            ArrayCoerceExpr* newexpr = NULL;

            /*
             * Reduce constants in the ArrayCoerceExpr's argument, then
             * build a new ArrayCoerceExpr.
             */
            arg = (Expr*)eval_const_expressions_mutator((Node*)expr->arg, context);

            newexpr = makeNode(ArrayCoerceExpr);
            newexpr->arg = arg;
            newexpr->fmtstr = expr->fmtstr;
            newexpr->nlsfmtstr = expr->nlsfmtstr;
            newexpr->elemfuncid = expr->elemfuncid;
            newexpr->resulttype = expr->resulttype;
            newexpr->resulttypmod = expr->resulttypmod;
            newexpr->resultcollid = expr->resultcollid;
            newexpr->isExplicit = expr->isExplicit;
            newexpr->coerceformat = expr->coerceformat;
            newexpr->location = expr->location;

            /*
             * If constant argument and it's a binary-coercible or
             * immutable conversion, we can simplify it to a constant.
             */
            if (arg && IsA(arg, Const) &&
                (!OidIsValid(newexpr->elemfuncid) || func_volatile(newexpr->elemfuncid) == PROVOLATILE_IMMUTABLE))
                return (Node*)evaluate_expr(
                    (Expr*)newexpr, newexpr->resulttype, newexpr->resulttypmod, newexpr->resultcollid);

            /* Else we must return the partially-simplified node */
            return (Node*)newexpr;
        }
        case T_CollateExpr: {
            /*
             * If we can simplify the input to a constant, then we don't
             * need the CollateExpr node at all: just change the
             * constcollid field of the Const node.  Otherwise, replace
             * the CollateExpr with a RelabelType. (We do that so as to
             * improve uniformity of expression representation and thus
             * simplify comparison of expressions.)
             */
            CollateExpr* collate = (CollateExpr*)node;
            Node* arg = NULL;
            arg = eval_const_expressions_mutator((Node*)collate->arg, context);

            if (arg && IsA(arg, Const)) {
                Const* con = (Const*)arg;
                con->constcollid = collate->collOid;
                return (Node*)con;
            } else if (collate->collOid == exprCollation(arg)) {
                /* Don't need a RelabelType either... */
                return arg;
            } else {
                RelabelType* relabel = makeNode(RelabelType);

                relabel->resulttype = exprType(arg);
                relabel->resulttypmod = exprTypmod(arg);
                relabel->resultcollid = collate->collOid;
                relabel->relabelformat = COERCE_DONTCARE;
                relabel->location = collate->location;

                /* Don't create stacked RelabelTypes */
                while (arg && IsA(arg, RelabelType))
                    arg = (Node*)((RelabelType*)arg)->arg;
                relabel->arg = (Expr*)arg;

                return (Node*)relabel;
            }
        }
        case T_CaseExpr: {
            /* ----------
             * CASE expressions can be simplified if there are constant
             * condition clauses:
             *		FALSE (or NULL): drop the alternative
             *		TRUE: drop all remaining alternatives
             * If the first non-FALSE alternative is a constant TRUE,
             * we can simplify the entire CASE to that alternative's
             * expression.	If there are no non-FALSE alternatives,
             * we simplify the entire CASE to the default result (ELSE).
             *
             * If we have a simple-form CASE with constant test
             * expression, we substitute the constant value for contained
             * CaseTestExpr placeholder nodes, so that we have the
             * opportunity to reduce constant test conditions.	For
             * example this allows
             *		CASE 0 WHEN 0 THEN 1 ELSE 1/0 END
             * to reduce to 1 rather than drawing a divide-by-0 error.
             * Note that when the test expression is constant, we don't
             * have to include it in the resulting CASE; for example
             *		CASE 0 WHEN x THEN y ELSE z END
             * is transformed by the parser to
             *		CASE 0 WHEN CaseTestExpr = x THEN y ELSE z END
             * which we can simplify to
             *		CASE WHEN 0 = x THEN y ELSE z END
             * It is not necessary for the executor to evaluate the "arg"
             * expression when executing the CASE, since any contained
             * CaseTestExprs that might have referred to it will have been
             * replaced by the constant.
             * ----------
             */
            CaseExpr* caseexpr = (CaseExpr*)node;
            CaseExpr* newcase = NULL;
            Node* save_case_val = NULL;
            Node* newarg = NULL;
            List* newargs = NIL;
            bool const_true_cond = false;
            Node* defresult = NULL;
            ListCell* arg = NULL;

            /* Simplify the test expression, if any */
            newarg = eval_const_expressions_mutator((Node*)caseexpr->arg, context);

            /* Set up for contained CaseTestExpr nodes */
            save_case_val = context->case_val;
            if (newarg && IsA(newarg, Const)) {
                context->case_val = newarg;
                newarg = NULL; /* not needed anymore, see above */
            } else
                context->case_val = NULL;

            /* Simplify the WHEN clauses */
            newargs = NIL;
            const_true_cond = false;
            foreach (arg, caseexpr->args) {
                CaseWhen* oldcasewhen = (CaseWhen*)lfirst(arg);
                Node* casecond = NULL;
                Node* caseresult = NULL;
                AssertEreport(IsA(oldcasewhen, CaseWhen), MOD_OPT, "");

                /* Simplify this alternative's test condition */
                casecond = eval_const_expressions_mutator((Node*)oldcasewhen->expr, context);

                /*
                 * If the test condition is constant FALSE (or NULL), then
                 * drop this WHEN clause completely, without processing
                 * the result.
                 */
                if (casecond && IsA(casecond, Const)) {
                    Const* const_input = (Const*)casecond;

                    if (const_input->constisnull || !DatumGetBool(const_input->constvalue))
                        continue; /* drop alternative with FALSE cond */
                    /* Else it's constant TRUE */
                    const_true_cond = true;
                }

                /* Simplify this alternative's result value */
                caseresult = eval_const_expressions_mutator((Node*)oldcasewhen->result, context);

                /* If non-constant test condition, emit a new WHEN node */
                if (!const_true_cond) {
                    CaseWhen* newcasewhen = makeNode(CaseWhen);

                    newcasewhen->expr = (Expr*)casecond;
                    newcasewhen->result = (Expr*)caseresult;
                    newcasewhen->location = oldcasewhen->location;
                    newargs = lappend(newargs, newcasewhen);
                    continue;
                }

                /*
                 * Found a TRUE condition, so none of the remaining
                 * alternatives can be reached.  We treat the result as
                 * the default result.
                 */
                defresult = caseresult;
                break;
            }

            /* Simplify the default result, unless we replaced it above */
            if (!const_true_cond)
                defresult = eval_const_expressions_mutator((Node*)caseexpr->defresult, context);

            context->case_val = save_case_val;

            /*
             * If no non-FALSE alternatives, CASE reduces to the default
             * result
             */
            if (newargs == NIL)
                return defresult;
            /* Otherwise we need a new CASE node */
            newcase = makeNode(CaseExpr);
            newcase->casetype = caseexpr->casetype;
            newcase->casecollid = caseexpr->casecollid;
            newcase->arg = (Expr*)newarg;
            newcase->args = newargs;
            newcase->defresult = (Expr*)defresult;
            newcase->location = caseexpr->location;
            return (Node*)newcase;
        }
        case T_CaseTestExpr: {
            /*
             * If we know a constant test value for the current CASE
             * construct, substitute it for the placeholder.  Else just
             * return the placeholder as-is.
             */
            if (context->case_val != NULL)
                return (Node*)copyObject(context->case_val);
            else
                return (Node*)copyObject(node);
        }
        case T_ArrayExpr: {
            ArrayExpr* arrayexpr = (ArrayExpr*)node;
            ArrayExpr* newarray = NULL;
            bool all_const = true;
            List* newelems = NIL;
            ListCell* element = NULL;

            newelems = NIL;
            foreach (element, arrayexpr->elements) {
                Node* e = NULL;

                e = eval_const_expressions_mutator((Node*)lfirst(element), context);
                if (e == NULL)
                    ereport(ERROR,
                        (errmodule(MOD_OPT),
                            errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            (errmsg("fail to eval const expressions."))));

                if (!IsA(e, Const))
                    all_const = false;
                newelems = lappend(newelems, e);
            }

            newarray = makeNode(ArrayExpr);
            newarray->array_typeid = arrayexpr->array_typeid;
            newarray->array_collid = arrayexpr->array_collid;
            newarray->element_typeid = arrayexpr->element_typeid;
            newarray->elements = newelems;
            newarray->multidims = arrayexpr->multidims;
            newarray->location = arrayexpr->location;

            if (all_const)
                return (Node*)evaluate_expr(
                    (Expr*)newarray, newarray->array_typeid, exprTypmod(node), newarray->array_collid);

            return (Node*)newarray;
        }
        case T_CoalesceExpr: {
            CoalesceExpr* coalesceexpr = (CoalesceExpr*)node;
            CoalesceExpr* newcoalesce = NULL;
            List* newargs = NIL;
            ListCell* arg = NULL;

            newargs = NIL;
            foreach (arg, coalesceexpr->args) {
                Node* e = NULL;

                e = eval_const_expressions_mutator((Node*)lfirst(arg), context);
                if (e == NULL)
                    ereport(ERROR,
                        (errmodule(MOD_OPT),
                            errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            (errmsg("Fail to eval const expressoin."))));
                /*
                 * We can remove null constants from the list. For a
                 * non-null constant, if it has not been preceded by any
                 * other non-null-constant expressions then it is the
                 * result. Otherwise, it's the next argument, but we can
                 * drop following arguments since they will never be
                 * reached.
                 */
                if (IsA(e, Const)) {
                    if (((Const*)e)->constisnull)
                        continue; /* drop null constant */
                    if (newargs == NIL)
                        return e; /* first expr */
                    newargs = lappend(newargs, e);
                    break;
                }
                newargs = lappend(newargs, e);
            }

            /*
             * If all the arguments were constant null, the result is just
             * null
             */
            if (newargs == NIL)
                return (Node*)makeNullConst(coalesceexpr->coalescetype, -1, coalesceexpr->coalescecollid);

            newcoalesce = makeNode(CoalesceExpr);
            newcoalesce->coalescetype = coalesceexpr->coalescetype;
            newcoalesce->coalescecollid = coalesceexpr->coalescecollid;
            newcoalesce->args = newargs;
            newcoalesce->location = coalesceexpr->location;
            return (Node*)newcoalesce;
        }
        case T_FieldSelect: {
            /*
             * We can optimize field selection from a whole-row Var into a
             * simple Var.	(This case won't be generated directly by the
             * parser, because ParseComplexProjection short-circuits it.
             * But it can arise while simplifying functions.)  Also, we
             * can optimize field selection from a RowExpr construct.
             *
             * We must however check that the declared type of the field
             * is still the same as when the FieldSelect was created ---
             * this can change if someone did ALTER COLUMN TYPE on the
             * rowtype.
             */
            FieldSelect* fselect = (FieldSelect*)node;
            FieldSelect* newfselect = NULL;
            Node* arg = NULL;

            arg = eval_const_expressions_mutator((Node*)fselect->arg, context);
            if (arg && IsA(arg, Var) && ((Var*)arg)->varattno == InvalidAttrNumber) {
                if (rowtype_field_matches(((Var*)arg)->vartype,
                    fselect->fieldnum,
                    fselect->resulttype,
                    fselect->resulttypmod,
                    fselect->resultcollid))
                    return (Node*)makeVar(((Var*)arg)->varno,
                        fselect->fieldnum,
                        fselect->resulttype,
                        fselect->resulttypmod,
                        fselect->resultcollid,
                        ((Var*)arg)->varlevelsup);
            }
            if (arg && IsA(arg, RowExpr)) {
                RowExpr* rowexpr = (RowExpr*)arg;

                if (fselect->fieldnum > 0 && fselect->fieldnum <= list_length(rowexpr->args)) {
                    Node* fld = (Node*)list_nth(rowexpr->args, fselect->fieldnum - 1);

                    if (rowtype_field_matches(rowexpr->row_typeid,
                        fselect->fieldnum,
                        fselect->resulttype,
                        fselect->resulttypmod,
                        fselect->resultcollid) &&
                        fselect->resulttype == exprType(fld) && fselect->resulttypmod == exprTypmod(fld) &&
                        fselect->resultcollid == exprCollation(fld))
                        return fld;
                }
            }
            newfselect = makeNode(FieldSelect);
            newfselect->arg = (Expr*)arg;
            newfselect->fieldnum = fselect->fieldnum;
            newfselect->resulttype = fselect->resulttype;
            newfselect->resulttypmod = fselect->resulttypmod;
            newfselect->resultcollid = fselect->resultcollid;
            return (Node*)newfselect;
        }
        case T_NullTest: {
            NullTest* ntest = (NullTest*)node;
            NullTest* newntest = NULL;
            Node* arg = NULL;

            arg = eval_const_expressions_mutator((Node*)ntest->arg, context);
            if (ntest->argisrow && arg && IsA(arg, RowExpr)) {
                /*
                 * We break ROW(...) IS [NOT] NULL into separate tests on
                 * its component fields.  This form is usually more
                 * efficient to evaluate, as well as being more amenable
                 * to optimization.
                 */
                if (AFORMAT_NULL_TEST_MODE) {
                    return GetNullExprFromRowExprForAFormat((RowExpr *)arg, ntest);
                } else {
                    return GetNullExprFromRowExpr((RowExpr *)arg, ntest);
                }
            }
            if (!ntest->argisrow && arg && IsA(arg, Const)) {
                Const* carg = (Const*)arg;
                bool result = false;

                switch (ntest->nulltesttype) {
                    case IS_NULL:
                        result = carg->constisnull;
                        break;
                    case IS_NOT_NULL:
                        result = !carg->constisnull;
                        break;
                    default:
                        ereport(ERROR,
                            (errmodule(MOD_OPT),
                                errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                                errmsg("unrecognized nulltesttype: %d", (int)ntest->nulltesttype)));
                        result = false; /* keep compiler quiet */
                        break;
                }

                return makeBoolConst(result, false);
            }

            newntest = makeNode(NullTest);
            newntest->arg = (Expr*)arg;
            newntest->nulltesttype = ntest->nulltesttype;
            newntest->argisrow = ntest->argisrow;
            return (Node*)newntest;
        }
        case T_BooleanTest: {
            BooleanTest* btest = (BooleanTest*)node;
            BooleanTest* newbtest = NULL;
            Node* arg = NULL;

            arg = eval_const_expressions_mutator((Node*)btest->arg, context);
            if (arg && IsA(arg, Const)) {
                Const* carg = (Const*)arg;
                bool result = false;

                switch (btest->booltesttype) {
                    case IS_TRUE:
                        result = (!carg->constisnull && DatumGetBool(carg->constvalue));
                        break;
                    case IS_NOT_TRUE:
                        result = (carg->constisnull || !DatumGetBool(carg->constvalue));
                        break;
                    case IS_FALSE:
                        result = (!carg->constisnull && !DatumGetBool(carg->constvalue));
                        break;
                    case IS_NOT_FALSE:
                        result = (carg->constisnull || DatumGetBool(carg->constvalue));
                        break;
                    case IS_UNKNOWN:
                        result = carg->constisnull;
                        break;
                    case IS_NOT_UNKNOWN:
                        result = !carg->constisnull;
                        break;
                    default:
                        ereport(ERROR,
                            (errmodule(MOD_OPT),
                                errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                                errmsg("unrecognized booltesttype: %d", (int)btest->booltesttype)));
                        result = false; /* keep compiler quiet */
                        break;
                }

                return makeBoolConst(result, false);
            }

            newbtest = makeNode(BooleanTest);
            newbtest->arg = (Expr*)arg;
            newbtest->booltesttype = btest->booltesttype;
            return (Node*)newbtest;
        }
        case T_PlaceHolderVar:

            /*
             * In estimation mode, just strip the PlaceHolderVar node
             * altogether; this amounts to estimating that the contained value
             * won't be forced to null by an outer join.  In regular mode we
             * just use the default behavior (ie, simplify the expression but
             * leave the PlaceHolderVar node intact).
             */
            if (context->estimate) {
                PlaceHolderVar* phv = (PlaceHolderVar*)node;

                return eval_const_expressions_mutator((Node*)phv->phexpr, context);
            }
            break;
        case T_TypeCast:
            {
                TypeCast *tc = (TypeCast*)node;
                Node *defResNode = tc->arg;
                Oid sourceTypeId = exprType(defResNode);
                Oid ptype = tc->typname->typeOid;
                char* fmtStr = NULL;
                char* nlsFmtStr = NULL;
                if (tc->fmt_str && IsA(tc->fmt_str, A_Const) &&
                    ((A_Const*)tc->fmt_str)->val.type == T_String) {
                    fmtStr = ((A_Const*)tc->fmt_str)->val.val.str;
                }
                if (tc->nls_fmt_str && IsA(tc->nls_fmt_str, A_Const) &&
                    ((A_Const*)tc->nls_fmt_str)->val.type == T_String) {
                    nlsFmtStr = pg_strtoupper(((A_Const*)tc->nls_fmt_str)->val.val.str);
                }
                if (can_coerce_type(1, &sourceTypeId, &ptype, COERCION_IMPLICIT)) {
                    return coerce_type(NULL, defResNode, sourceTypeId, ptype, -1,
                        COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, fmtStr, nlsFmtStr, -1);
                } else {
                    ereport(ERROR,
                        (errcode(ERRCODE_CANNOT_COERCE), errmsg("could not convert type %s to %s",
                            format_type_be(sourceTypeId), format_type_be(ptype))));
                }
                break;
            }
        default:
            break;
    }

    if (IS_SPQ_COORDINATOR) {
        /* recurse into query structure if requested */
        if (IsA(node, Query) && context->recurse_queries) {
            return (Node *)query_tree_mutator((Query *)node, (Node * (*)(Node *, void *)) eval_const_expressions_mutator, (void *)context, 0);
        }
    }

    ELOG_FIELD_NAME_START(IsA(node, TargetEntry) ? ((TargetEntry*)node)->resname : NULL);

    /*
     * For any node type not handled above, we recurse using
     * expression_tree_mutator, which will copy the node unchanged but try to
     * simplify its arguments (if any) using this routine. For example: we
     * cannot eliminate an ArrayRef node, but we might be able to simplify
     * constant expressions in its subscripts.
     */
    node = expression_tree_mutator(node, (Node* (*)(Node*, void*)) eval_const_expressions_mutator, (void*)context);

    ELOG_FIELD_NAME_END;

    return node;
}

/*
 * Subroutine for eval_const_expressions: process arguments of an OR clause
 *
 * This includes flattening of nested ORs as well as recursion to
 * eval_const_expressions to simplify the OR arguments.
 *
 * After simplification, OR arguments are handled as follows:
 *		non constant: keep
 *		FALSE: drop (does not affect result)
 *		TRUE: force result to TRUE
 *		NULL: keep only one
 * We must keep one NULL input because ExecEvalOr returns NULL when no input
 * is TRUE and at least one is NULL.  We don't actually include the NULL
 * here, that's supposed to be done by the caller.
 *
 * The output arguments *haveNull and *forceTrue must be initialized FALSE
 * by the caller.  They will be set TRUE if a null constant or true constant,
 * respectively, is detected anywhere in the argument list.
 */
static List* simplify_or_arguments(List* args, eval_const_expressions_context* context, bool* haveNull, bool* forceTrue)
{
    List* newargs = NIL;
    List* unprocessed_args = NIL;

    /*
     * Since the parser considers OR to be a binary operator, long OR lists
     * become deeply nested expressions.  We must flatten these into long
     * argument lists of a single OR operator.	To avoid blowing out the stack
     * with recursion of eval_const_expressions, we resort to some tenseness
     * here: we keep a list of not-yet-processed inputs, and handle flattening
     * of nested ORs by prepending to the to-do list instead of recursing.
     */
    unprocessed_args = list_copy(args);
    while (unprocessed_args != NULL) {
        Node* arg = (Node*)linitial(unprocessed_args);

        unprocessed_args = list_delete_first(unprocessed_args);

        /* flatten nested ORs as per above comment */
        if (or_clause(arg)) {
            List* subargs = list_copy(((BoolExpr*)arg)->args);

            /* overly tense code to avoid leaking unused list header */
            if (unprocessed_args == NULL)
                unprocessed_args = subargs;
            else {
                List* oldhdr = unprocessed_args;

                unprocessed_args = list_concat(subargs, unprocessed_args);
                pfree_ext(oldhdr);
            }
            continue;
        }

        /* If it's not an OR, simplify it */
        arg = eval_const_expressions_mutator(arg, context);
        if (arg == NULL)
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    (errmsg("fail to eval const expressions. "))));

        /*
         * It is unlikely but not impossible for simplification of a non-OR
         * clause to produce an OR.  Recheck, but don't be too tense about it
         * since it's not a mainstream case. In particular we don't worry
         * about const-simplifying the input twice.
         */
        if (or_clause(arg)) {
            List* subargs = list_copy(((BoolExpr*)arg)->args);

            unprocessed_args = list_concat(subargs, unprocessed_args);
            continue;
        }

        /*
         * OK, we have a const-simplified non-OR argument.	Process it per
         * comments above.
         */
        if (IsA(arg, Const)) {
            Const* const_input = (Const*)arg;

            if (const_input->constisnull)
                *haveNull = true;
            else if (DatumGetBool(const_input->constvalue)) {
                *forceTrue = true;

                /*
                 * Once we detect a TRUE result we can just exit the loop
                 * immediately.  However, if we ever add a notion of
                 * non-removable functions, we'd need to keep scanning.
                 */
                return NIL;
            }
            /* otherwise, we can drop the constant-false input */
            continue;
        }

        /* else emit the simplified arg into the result list */
        newargs = lappend(newargs, arg);
    }

    return newargs;
}

/*
 * Subroutine for eval_const_expressions: process arguments of an AND clause
 *
 * This includes flattening of nested ANDs as well as recursion to
 * eval_const_expressions to simplify the AND arguments.
 *
 * After simplification, AND arguments are handled as follows:
 *		non constant: keep
 *		TRUE: drop (does not affect result)
 *		FALSE: force result to FALSE
 *		NULL: keep only one
 * We must keep one NULL input because ExecEvalAnd returns NULL when no input
 * is FALSE and at least one is NULL.  We don't actually include the NULL
 * here, that's supposed to be done by the caller.
 *
 * The output arguments *haveNull and *forceFalse must be initialized FALSE
 * by the caller.  They will be set TRUE if a null constant or false constant,
 * respectively, is detected anywhere in the argument list.
 */
static List* simplify_and_arguments(
    List* args, eval_const_expressions_context* context, bool* haveNull, bool* forceFalse)
{
    List* newargs = NIL;
    List* unprocessed_args = NIL;

    /* See comments in simplify_or_arguments */
    unprocessed_args = list_copy(args);
    while (unprocessed_args != NULL) {
        Node* arg = (Node*)linitial(unprocessed_args);

        unprocessed_args = list_delete_first(unprocessed_args);

        /* flatten nested ANDs as per above comment */
        if (and_clause(arg)) {
            List* subargs = list_copy(((BoolExpr*)arg)->args);

            /* overly tense code to avoid leaking unused list header */
            if (unprocessed_args == NULL)
                unprocessed_args = subargs;
            else {
                List* oldhdr = unprocessed_args;

                unprocessed_args = list_concat(subargs, unprocessed_args);
                pfree_ext(oldhdr);
            }
            continue;
        }

        /* If it's not an AND, simplify it */
        arg = eval_const_expressions_mutator(arg, context);
        if (arg == NULL)
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    (errmsg("Eval const expression mutator failed. "))));

        /*
         * It is unlikely but not impossible for simplification of a non-AND
         * clause to produce an AND.  Recheck, but don't be too tense about it
         * since it's not a mainstream case. In particular we don't worry
         * about const-simplifying the input twice.
         */
        if (and_clause(arg)) {
            List* subargs = list_copy(((BoolExpr*)arg)->args);

            unprocessed_args = list_concat(subargs, unprocessed_args);
            continue;
        }

        /*
         * OK, we have a const-simplified non-AND argument.  Process it per
         * comments above.
         */
        if (IsA(arg, Const)) {
            Const* const_input = (Const*)arg;

            if (const_input->constisnull)
                *haveNull = true;
            else if (!DatumGetBool(const_input->constvalue)) {
                *forceFalse = true;

                /*
                 * Once we detect a FALSE result we can just exit the loop
                 * immediately.  However, if we ever add a notion of
                 * non-removable functions, we'd need to keep scanning.
                 */
                return NIL;
            }
            /* otherwise, we can drop the constant-true input */
            continue;
        }

        /* else emit the simplified arg into the result list */
        newargs = lappend(newargs, arg);
    }

    return newargs;
}

/*
 * Subroutine for eval_const_expressions: try to simplify boolean equality
 * or inequality condition
 *
 * Inputs are the operator OID and the simplified arguments to the operator.
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the expression.
 *
 * The idea here is to reduce "x = true" to "x" and "x = false" to "NOT x",
 * or similarly "x <> true" to "NOT x" and "x <> false" to "x".
 * This is only marginally useful in itself, but doing it in constant folding
 * ensures that we will recognize these forms as being equivalent in, for
 * example, partial index matching.
 *
 * We come here only if simplify_function has failed; therefore we cannot
 * see two constant inputs, nor a constant-NULL input.
 */
static Node* simplify_boolean_equality(Oid opno, List* args)
{
    Node* leftop = NULL;
    Node* rightop = NULL;

    AssertEreport(list_length(args) == 2, MOD_OPT, "");
    leftop = (Node*)linitial(args);
    rightop = (Node*)lsecond(args);
    if (leftop && IsA(leftop, Const)) {
        AssertEreport(!((Const*)leftop)->constisnull, MOD_OPT, "");
        if (opno == BooleanEqualOperator) {
            if (DatumGetBool(((Const*)leftop)->constvalue))
                return rightop; /* true = foo */
            else
                return negate_clause(rightop); /* false = foo */
        } else {
            if (DatumGetBool(((Const*)leftop)->constvalue))
                return negate_clause(rightop); /* true <> foo */
            else
                return rightop; /* false <> foo */
        }
    }
    if (rightop && IsA(rightop, Const)) {
        AssertEreport(!((Const*)rightop)->constisnull, MOD_OPT, "");
        if (opno == BooleanEqualOperator) {
            if (DatumGetBool(((Const*)rightop)->constvalue))
                return leftop; /* foo = true */
            else
                return negate_clause(leftop); /* foo = false */
        } else {
            if (DatumGetBool(((Const*)rightop)->constvalue))
                return negate_clause(leftop); /* foo <> true */
            else
                return leftop; /* foo <> false */
        }
    }
    return NULL;
}

/*
 * Subroutine for eval_const_expressions: try to simplify a function call
 * (which might originally have been an operator; we don't care)
 *
 * Inputs are the function OID, actual result type OID (which is needed for
 * polymorphic functions), result typmod, result collation, the input
 * collation to use for the function, the original argument list (not
 * const-simplified yet, unless process_args is false), and some flags;
 * also the context data for eval_const_expressions.
 *
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the function call.
 *
 * This function is also responsible for converting named-notation argument
 * lists into positional notation and/or adding any needed default argument
 * expressions; which is a bit grotty, but it avoids extra fetches of the
 * function's pg_proc tuple.  For this reason, the args list is
 * pass-by-reference.  Conversion and const-simplification of the args list
 * will be done even if simplification of the function call itself is not
 * possible.
 */
Expr* simplify_function(Oid funcid, Oid result_type, int32 result_typmod, Oid result_collid, Oid input_collid,
    List** args_p, bool process_args, bool allow_non_const, eval_const_expressions_context* context)
{
    List* args = *args_p;
    HeapTuple func_tuple;
    Form_pg_proc func_form;
    Expr* newexpr = NULL;

    /*
     * We have three strategies for simplification: execute the function to
     * deliver a constant result, use a transform function to generate a
     * substitute node tree, or expand in-line the body of the function
     * definition (which only works for simple SQL-language functions, but
     * that is a common case).	Each case needs access to the function's
     * pg_proc tuple, so fetch it just once.
     *
     * Note: the allow_non_const flag suppresses both the second and third
     * strategies; so if !allow_non_const, simplify_function can only return a
     * Const or NULL.  Argument-list rewriting happens anyway, though.
     */
    func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
    if (!HeapTupleIsValid(func_tuple))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for function %u", funcid)));
    func_form = (Form_pg_proc)GETSTRUCT(func_tuple);

    /*
     * Process the function arguments, unless the caller did it already.
     *
     * Here we must deal with named or defaulted arguments, and then
     * recursively apply eval_const_expressions to the whole argument list.
     */
    if (process_args) {
        args = expand_function_arguments(args, result_type, func_tuple);
        args = (List*)expression_tree_mutator(
            (Node*)args, (Node* (*)(Node*, void*)) eval_const_expressions_mutator, (void*)context);
        /* Argument processing done, give it back to the caller */
        *args_p = args;
    }

    /* Now attempt simplification of the function call proper. */
    newexpr =
        evaluate_function(funcid, result_type, result_typmod, result_collid, input_collid, args, func_tuple, context);

    if (newexpr == NULL && allow_non_const && OidIsValid(func_form->protransform)) {
        /*
         * Build a SupportRequestSimplify node to pass to the support
         * function, pointing to a dummy FuncExpr node containing the
         * simplified arg list.  We use this approach to present a uniform
         * interface to the support function regardless of how the target
         * function is actually being invoked.
         */
        SupportRequestSimplify req;
        FuncExpr fexpr;

        fexpr.xpr.type = T_FuncExpr;
        fexpr.funcid = funcid;
        fexpr.funcresulttype = result_type;
        fexpr.funcretset = func_form->proretset;
        fexpr.funcvariadic = false;
        fexpr.funcformat = COERCE_DONTCARE;
        fexpr.funccollid = result_collid;
        fexpr.inputcollid = input_collid;
        fexpr.args = args;
        fexpr.location = -1;

        req.type = T_SupportRequestSimplify;
        req.root = context->root;
        req.fcall = &fexpr;

        newexpr = (Expr*)DatumGetPointer(OidFunctionCall1(func_form->protransform, PointerGetDatum(&req)));

        /* catch a possible API misunderstanding */
        Assert(newexpr != (Expr *) &fexpr);
    }

    if (newexpr == NULL && allow_non_const)
        newexpr = inline_function(funcid, result_type, result_collid, input_collid, args, func_tuple, context);

    ReleaseSysCache(func_tuple);

    return newexpr;
}

static List* StartExtractFunctionOutarguments
    (const List* parameters, int narg, const char* p_argmodes, char** p_argnames, List** in_parameters)
{
    int i = 0;
    ListCell* cell = NULL;
    foreach (cell, parameters) {
        Node* arg = (Node*)lfirst(cell);
        char* argname = NULL;
        char argmode = 0;

        if (IsA(arg, NamedArgExpr)) {
            NamedArgExpr* na = (NamedArgExpr*)arg;

            argname = na->name;
            int k = 0;
            bool found = false;
            for (i = 0; i < narg; i++) {
                /* consider only input parameters */
                if (p_argmodes && (p_argmodes[i] != FUNC_PARAM_IN && p_argmodes[i] != FUNC_PARAM_INOUT &&
                                      p_argmodes[i] != FUNC_PARAM_VARIADIC))
                    continue;
                if (p_argnames[i] != NULL && strcmp(p_argnames[i], argname) == 0) {
                    na->argnumber = k;
                    found = true;
                    break;
                }
                /* increase pp only for input parameters */
                k++;
            }

            if (found && argmode != FUNC_PARAM_OUT)
                *in_parameters = lappend(*in_parameters, arg);
        } else {
            if (p_argmodes != NULL)
                argmode = p_argmodes[i];
            else
                argmode = FUNC_PARAM_IN;

            if (argmode != FUNC_PARAM_OUT)
                *in_parameters = lappend(*in_parameters, lfirst(cell));
        }

        i++;
    }
    return *in_parameters;
}


/*
 * @Description: extract out parameters from all parameters
 * @in funcid - function oid
 * @in parameters - parameter list
 * @in funcname - function name
 * @return - parameters not include out parameter
 */
List* extract_function_outarguments(Oid funcid, List* parameters, List* funcname)
{
    HeapTuple proctup = NULL;
    Form_pg_proc procStruct;
    Oid* p_argtypes = NULL;
    char** p_argnames = NULL;
    char* p_argmodes = NULL;
    List* in_parameters = NULL;
    char* schemaname = NULL;
    char* name = NULL;

    /* deconstruct the name list */
    DeconstructQualifiedName(funcname, &schemaname, &name);
    proctup = SearchSysCache(PROCOID, ObjectIdGetDatum(funcid), 0, 0, 0);

    /*
     * function may be deleted after clist be searched.
     */
    if (!HeapTupleIsValid(proctup)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("function \"%s\" doesn't exist", name)));
    }

    /* get the all args informations, only "in" parameters if p_argmodes is null */
    int narg = get_func_arg_info(proctup, &p_argtypes, &p_argnames, &p_argmodes);

    procStruct = (Form_pg_proc)GETSTRUCT(proctup);
    int ndefaultargs = procStruct->pronargdefaults;
    ReleaseSysCache(proctup);

    /* check the parameters' count */
    if (narg - ndefaultargs > (parameters ? parameters->length : 0)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_FUNCTION),
                errmsg("function \"%s\" with %d parameters doesn't exist", name, parameters ? parameters->length : 0)));
    }

    if (parameters && (narg < parameters->length)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_FUNCTION),
                errmsg("function \"%s\" with %d parameters doesn't exist", name, parameters->length)));
    }

    (void*)StartExtractFunctionOutarguments(parameters, narg, p_argmodes, p_argnames, &in_parameters);

    return in_parameters;
}

/*
 * expand_function_arguments: convert named-notation args to positional args
 * and/or insert default args, as needed
 *
 * If we need to change anything, the input argument list is copied, not
 * modified.
 *
 * Note: this gets applied to operator argument lists too, even though the
 * cases it handles should never occur there.  This should be OK since it
 * will fall through very quickly if there's nothing to do.
 */
static List* expand_function_arguments(List* args, Oid result_type, HeapTuple func_tuple)
{
    Form_pg_proc funcform = (Form_pg_proc)GETSTRUCT(func_tuple);
    bool has_named_args = false;
    ListCell* lc = NULL;

    /* Do we have any named arguments? */
    foreach (lc, args) {
        Node* arg = (Node*)lfirst(lc);

        if (IsA(arg, NamedArgExpr)) {
            has_named_args = true;
            break;
        }
    }

    /* If so, we must apply reorder_function_arguments */
    if (has_named_args) {
        args = reorder_function_arguments(args, func_tuple);
        /* Recheck argument types and add casts if needed */
        recheck_cast_function_args(args, result_type, func_tuple);
    } else if (list_length(args) < funcform->pronargs) {
        /* No named args, but we seem to be short some defaults */
        args = add_function_defaults(args, func_tuple);
        /* Recheck argument types and add casts if needed */
        recheck_cast_function_args(args, result_type, func_tuple);
    }

    return args;
}

/*
 * reorder_function_arguments: convert named-notation args to positional args
 *
 * This function also inserts default argument values as needed, since it's
 * impossible to form a truly valid positional call without that.
 */
static List* reorder_function_arguments(List* args, HeapTuple func_tuple)
{
    Form_pg_proc funcform = (Form_pg_proc)GETSTRUCT(func_tuple);
    int pronargs = funcform->pronargs;
    int nargsprovided = list_length(args);
    Node* argarray[FUNC_MAX_ARGS];
    ListCell* lc = NULL;
    int i;
    bool func_package = false;
    bool isNull = false;
    errno_t rc;

    Datum ispackage = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_package, &isNull);
    func_package = DatumGetBool(ispackage);
    if (!func_package) {
        Assert(nargsprovided <= pronargs);
    } else if (pronargs < nargsprovided) {
        pronargs = nargsprovided;
    }

    if (pronargs > FUNC_MAX_ARGS)
        ereport(
            ERROR, (errmodule(MOD_OPT), errcode(ERRCODE_TOO_MANY_ARGUMENTS), errmsg("too many function arguments")));
    rc = memset_s(argarray, pronargs * sizeof(Node*), 0, pronargs * sizeof(Node*));
    securec_check(rc, "", "");

    /* Deconstruct the argument list into an array indexed by argnumber */
    i = 0;
    foreach (lc, args) {
        Node* arg = (Node*)lfirst(lc);

        if (!IsA(arg, NamedArgExpr)) {
            /* positional argument, assumed to precede all named args */
            AssertEreport(argarray[i] == NULL, MOD_OPT, "");
            argarray[i++] = arg;
        } else {
            NamedArgExpr* na = (NamedArgExpr*)arg;

            AssertEreport(argarray[na->argnumber] == NULL, MOD_OPT, "");
            argarray[na->argnumber] = (Node*)na->arg;
        }
    }

    /*
     * Fetch default expressions, if needed, and insert into array at proper
     * locations (they aren't necessarily consecutive or all used)
     */
    if (nargsprovided < pronargs) {
        char* argmodes = NULL;
        char** argnames = NULL;
        Oid* argtypes = NULL;
        bool isnull = false;
        int proallargs = 0;
        Datum defposdatum;
        int2vector* defpos = NULL;
        int* argdefpos = NULL;
        List* defaults = NIL;
        int counter = 0;
        int pos = 0;

        proallargs = get_func_arg_info(func_tuple, &argtypes, &argnames, &argmodes);
        if (pronargs <= FUNC_MAX_ARGS_INROW) {
            defposdatum = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_prodefaultargpos, &isnull);
            AssertEreport(!isnull, MOD_OPT, "");
            defpos = (int2vector*)DatumGetPointer(defposdatum);
        } else {
            defposdatum = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_prodefaultargposext, &isnull);
            AssertEreport(!isnull, MOD_OPT, "");
            defpos = (int2vector*)PG_DETOAST_DATUM(defposdatum);
        }

        FetchDefaultArgumentPos(&argdefpos, defpos, argmodes, proallargs);

        defaults = fetch_function_defaults(func_tuple);

        foreach (lc, defaults) {
            pos = argdefpos[counter];
            if (argarray[pos] == NULL)
                argarray[pos] = (Node*)lfirst(lc);
            counter++;
        }

        pfree_ext(argdefpos);
        if (argtypes != NULL)
            pfree_ext(argtypes);
        if (argmodes != NULL)
            pfree_ext(argmodes);
        if (argnames != NULL) {
            for (counter = 0; counter < proallargs; counter++) {
                if (argnames[counter])
                    pfree_ext(argnames[counter]);
            }
            pfree_ext(argnames);
        }
        list_free_ext(defaults);
    }

    /* Now reconstruct the args list in proper order */
    args = NIL;
    for (i = 0; i < pronargs; i++) {
        AssertEreport(argarray[i] != NULL, MOD_OPT, "");
        args = lappend(args, argarray[i]);
    }

    return args;
}

/*
 * add_function_defaults: add missing function arguments from its defaults
 *
 * This is used only when the argument list was positional to begin with,
 * and so we know we just need to add defaults at the end.
 */
static List* add_function_defaults(List* args, HeapTuple func_tuple)
{
    Form_pg_proc funcform = (Form_pg_proc)GETSTRUCT(func_tuple);
    int nargsprovided = list_length(args);
    List* defaults = NIL;
    int ndelete;

    /* Get all the default expressions from the pg_proc tuple */
    defaults = fetch_function_defaults(func_tuple);

    /* Delete any unused defaults from the list */
    ndelete = nargsprovided + list_length(defaults) - funcform->pronargs;
    if (ndelete < 0)
        ereport(ERROR,
            (errmodule(MOD_OPT), errcode(ERRCODE_INVALID_FUNCTION_DEFINITION), errmsg("not enough default arguments")));
    while (ndelete-- > 0)
        defaults = list_delete_first(defaults);

    /* And form the combined argument list, not modifying the input list */
    return list_concat(list_copy(args), defaults);
}

/*
 * fetch_function_defaults: get function's default arguments as expression list
 */
static List* fetch_function_defaults(HeapTuple func_tuple)
{
    List* defaults = NIL;
    Datum proargdefaults;
    bool isnull = false;
    char* str = NULL;

    /* The error cases here shouldn't happen, but check anyway */
    proargdefaults = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_proargdefaults, &isnull);
    if (isnull)
        ereport(ERROR,
            (errmodule(MOD_OPT), errcode(ERRCODE_INVALID_FUNCTION_DEFINITION), errmsg("not enough default arguments")));
    str = TextDatumGetCString(proargdefaults);
    defaults = (List*)stringToNode(str);

    AssertEreport(IsA(defaults, List), MOD_OPT, "");
    pfree_ext(str);

    return defaults;
}

/*
 * recheck_cast_function_args: recheck function args and typecast as needed
 * after adding defaults.
 *
 * It is possible for some of the defaulted arguments to be polymorphic;
 * therefore we can't assume that the default expressions have the correct
 * data types already.	We have to re-resolve polymorphics and do coercion
 * just like the parser did.
 *
 * This should be a no-op if there are no polymorphic arguments,
 * but we do it anyway to be sure.
 *
 * Note: if any casts are needed, the args list is modified in-place;
 * caller should have already copied the list structure.
 */
static void recheck_cast_function_args(List* args, Oid result_type, HeapTuple func_tuple)
{
    Form_pg_proc funcform = (Form_pg_proc)GETSTRUCT(func_tuple);
    int nargs;
    Oid actual_arg_types[FUNC_MAX_ARGS];
    Oid declared_arg_types[FUNC_MAX_ARGS];
    Oid rettype;
    ListCell* lc = NULL;
    bool func_package = false;
    bool isNull = false;
    Oid* proargtypes = NULL;
    int proc_arg = funcform->pronargs;

    if (list_length(args) > FUNC_MAX_ARGS)
        ereport(
            ERROR, (errmodule(MOD_OPT), errcode(ERRCODE_TOO_MANY_ARGUMENTS), errmsg("too many function arguments")));
    nargs = 0;
    foreach (lc, args) {
        actual_arg_types[nargs++] = exprType((Node*)lfirst(lc));
    }

    Datum ispackage = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_package, &isNull);
    func_package = DatumGetBool(ispackage);
    if (!func_package || nargs <= funcform->pronargs) {
        Assert(nargs == funcform->pronargs);
        oidvector* proargs = ProcedureGetArgTypes(func_tuple);
        proargtypes = proargs->values;
    } else if (nargs > funcform->pronargs) {
        Datum proallargtypes;
        ArrayType* arr = NULL;
        proallargtypes = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_proallargtypes, &isNull);
        if (!isNull) {
            arr = DatumGetArrayTypeP(proallargtypes); /* ensure not toasted */
            proc_arg = ARR_DIMS(arr)[0];
            proargtypes = (Oid*)ARR_DATA_PTR(arr);
        }
    }

    errno_t errorno;
    errorno = memcpy_s(declared_arg_types, FUNC_MAX_ARGS * sizeof(Oid), proargtypes, proc_arg * sizeof(Oid));
    securec_check(errorno, "", "");

    /* if argtype is table of, change its element type */
    for (int i = 0; i < nargs; i++) {
        Oid baseOid = InvalidOid;
        if (isTableofType(declared_arg_types[i], &baseOid, NULL)) {
            declared_arg_types[i] = baseOid;
        }
    }

    rettype =
        enforce_generic_type_consistency(actual_arg_types, declared_arg_types, nargs, funcform->prorettype, false);
    /* let's just check we got the same answer as the parser did ... */
    if (rettype != result_type)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                errmsg("function's resolved result type changed during planning")));

    /* perform any necessary typecasting of arguments */
    make_fn_arguments(NULL, args, actual_arg_types, declared_arg_types);
}

/*
 * Determine if the func can be evaluted during the simplify function.
 */
static Oid pre_evaluate_func[] = {CURRENTSCHEMAFUNCOID,
    CURRENTUSERFUNCOID,
    GETPGUSERNAMEFUNCOID,
    SESSIONUSERFUNCOID,
    CURRENTDATABASEFUNCOID,
    PGCLIENTENCODINGFUNCOID};

static bool is_safe_simplify_func(Oid funcid, List *args)
{
    if (funcid == InvalidOid) {
        return false;
    }
    for (uint i = 0; i < lengthof(pre_evaluate_func); i++) {
        if (funcid == pre_evaluate_func[i]) {
            return true;
        }
    }

    /* handle some special func */
    if (funcid == CONCATFUNCOID || funcid == CONCATWSFUNCOID) {
        ListCell* arg = NULL;
        foreach (arg, args) {
            Oid typ = exprType((Node*)lfirst(arg));
            /*
             * binary: not ok, bytea_output will affect the result. raw, etc...
             * binary: BINARY, VARBINARY, BLOB, TINYBLOB, MEDIUMBLOB, LONGBLOB, bit. althought the concat_internal treat
             *          them specially, but the concat result is blob, so the result still affect by bytea_output.
             * time: not ok, DateStyle will affect the result. time, timestamp, date, etc...
             * num: ok, integer, float, numeric
             * bool: ok
             * string: ok, char/varchar/text/xml/json/set/enum/xml/unknown, etc...
             */
            switch (typ) {
                case BOOLOID:
                case CHAROID:
                case NAMEOID:
                case INT1OID:
                case INT2OID:
                case INT4OID:
                case INT8OID:
                case INT16OID:
                case TEXTOID:
                case OIDOID:
                case CLOBOID:
                case JSONOID:
                case XMLOID:
                case UNKNOWNOID:
                case VARCHAROID:
                case VARBITOID:
                case CSTRINGOID:
                case JSONBOID:
                case NVARCHAR2OID:
                case XIDOID:
                case SHORTXIDOID:
                    break;
                default:
                    if (type_is_set(typ) || type_is_enum(typ)) {
                        break;
                    }
                    /* other case, return false directly */
                    return false;
            }
        }
        /* all args outfunc are immutable, return true */
        return true;
    }
    
    return false;
}

/*
 * evaluate_function: try to pre-evaluate a function call
 *
 * We can do this if the function is strict and has any constant-null inputs
 * (just return a null constant), or if the function is immutable and has all
 * constant inputs (call it and return the result as a Const node).  In
 * estimation mode we are willing to pre-evaluate stable functions too.
 *
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the function.
 */
static Expr* evaluate_function(Oid funcid, Oid result_type, int32 result_typmod, Oid result_collid, Oid input_collid,
    List* args, HeapTuple func_tuple, eval_const_expressions_context* context)
{
    Form_pg_proc funcform = (Form_pg_proc)GETSTRUCT(func_tuple);
    bool has_nonconst_input = false;
    bool has_null_input = false;
    ListCell* arg = NULL;
    FuncExpr* newexpr = NULL;

    /*
     * Can't simplify if it returns a set.
     */
    if (funcform->proretset)
        return NULL;

    /*
     * Can't simplify if it returns RECORD.  The immediate problem is that it
     * will be needing an expected tupdesc which we can't supply here.
     *
     * In the case where it has OUT parameters, it could get by without an
     * expected tupdesc, but we still have issues: get_expr_result_type()
     * doesn't know how to extract type info from a RECORD constant, and in
     * the case of a NULL function result there doesn't seem to be any clean
     * way to fix that.  In view of the likelihood of there being still other
     * gotchas, seems best to leave the function call unreduced.
     */
    if (funcform->prorettype == RECORDOID)
        return NULL;

    /* For hierarchical query the value is calculated in runtime stage, so just return */
    if (IsHierarchicalQueryFuncOid(funcid)) {
        return NULL;
    }

    /*
     * Check for constant inputs and especially constant-NULL inputs.
     */
    foreach (arg, args) {
        if (IsA(lfirst(arg), Const))
            has_null_input = has_null_input || ((Const*)lfirst(arg))->constisnull;
        else
            has_nonconst_input = true;
    }

    /*
     * If the function is strict and has a constant-NULL input, it will never
     * be called at all, so we can replace the call by a NULL constant, even
     * if there are other inputs that aren't constant, and even if the
     * function is not otherwise immutable.
     */
    if (funcform->proisstrict && has_null_input)
        return (Expr*)makeNullConst(result_type, result_typmod, result_collid);

    /*
     * Otherwise, can simplify only if all inputs are constants. (For a
     * non-strict function, constant NULL inputs are treated the same as
     * constant non-NULL inputs.)
     */
    if (has_nonconst_input)
        return NULL;

    /*
     * Ordinarily we are only allowed to simplify immutable functions. But for
     * purposes of estimation, we consider it okay to simplify functions that
     * are merely stable; the risk that the result might change from planning
     * time to execution time is worth taking in preference to not being able
     * to estimate the value at all.
     * Actually, it is safe to simplify some stable or volatile functions,
     * is_safe_simplify_func() will determines it.
     */
    if (funcform->provolatile == PROVOLATILE_IMMUTABLE)
        /* okay */;
    else if (context->estimate && funcform->provolatile == PROVOLATILE_STABLE)
        /* okay */;
    else if (is_safe_simplify_func(funcid, args))
        /* okay */;
    else
        return NULL;

    /*
     * OK, looks like we can simplify this operator/function.
     *
     * Build a new FuncExpr node containing the already-simplified arguments.
     */
    newexpr = makeNode(FuncExpr);
    newexpr->funcid = funcid;
    newexpr->funcresulttype = result_type;
    newexpr->funcretset = false;
    newexpr->funcformat = COERCE_DONTCARE; /* doesn't matter */
    newexpr->funccollid = result_collid;   /* doesn't matter */
    newexpr->inputcollid = input_collid;
    newexpr->args = args;
    newexpr->location = -1;

    bool can_ignore = false;
    if (context != NULL && context->root != NULL && context->root->parse != NULL && context->root->parse->hasIgnore) {
        can_ignore = true;
    }
    return evaluate_expr((Expr*)newexpr, result_type, result_typmod, result_collid, can_ignore);
}

/*
 * inline_function: try to expand a function call inline
 *
 * If the function is a sufficiently simple SQL-language function
 * (just "SELECT expression"), then we can inline it and avoid the rather
 * high per-call overhead of SQL functions.  Furthermore, this can expose
 * opportunities for constant-folding within the function expression.
 *
 * We have to beware of some special cases however.  A directly or
 * indirectly recursive function would cause us to recurse forever,
 * so we keep track of which functions we are already expanding and
 * do not re-expand them.  Also, if a parameter is used more than once
 * in the SQL-function body, we require it not to contain any volatile
 * functions (volatiles might deliver inconsistent answers) nor to be
 * unreasonably expensive to evaluate.	The expensiveness check not only
 * prevents us from doing multiple evaluations of an expensive parameter
 * at runtime, but is a safety value to limit growth of an expression due
 * to repeated inlining.
 *
 * We must also beware of changing the volatility or strictness status of
 * functions by inlining them.
 *
 * Also, at the moment we can't inline functions returning RECORD.  This
 * doesn't work in the general case because it discards information such
 * as OUT-parameter declarations.
 *
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the function.
 */
static Expr* inline_function(Oid funcid, Oid result_type, Oid result_collid, Oid input_collid, List* args,
    HeapTuple func_tuple, eval_const_expressions_context* context)
{
    Form_pg_proc funcform = (Form_pg_proc)GETSTRUCT(func_tuple);
    char* src = NULL;
    Datum tmp;
    bool isNull = false;
    bool modifyTargetList = false;
    MemoryContext oldcxt;
    MemoryContext mycxt;
    inline_error_callback_arg callback_arg;
    ErrorContextCallback sqlerrcontext;
    FuncExpr* fexpr = NULL;
    SQLFunctionParseInfoPtr pinfo;
    ParseState* pstate = NULL;
    List* raw_parsetree_list = NIL;
    Query* querytree = NULL;
    Node* newexpr = NULL;
    int* usecounts = NULL;
    ListCell* arg = NULL;
    int i;

    /*
     * Forget it if the function is not SQL-language or has other showstopper
     * properties.	(The nargs check is just paranoia.)
     */
    if (funcform->prolang != SQLlanguageId || funcform->prosecdef || funcform->proretset ||
        funcform->prorettype == RECORDOID || !heap_attisnull(func_tuple, Anum_pg_proc_proconfig, NULL) ||
        funcform->pronargs != list_length(args))
        return NULL;

    /* Check for recursive function, and give up trying to expand if so */
    if (list_member_oid(context->active_fns, funcid))
        return NULL;

    /* Check permission to call function (fail later, if not) */
    if (pg_proc_aclcheck(funcid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
        return NULL;

    /* Check whether a plugin wants to hook function entry/exit */
    if (FmgrHookIsNeeded(funcid))
        return NULL;

    /*
     * Make a temporary memory context, so that we don't leak all the stuff
     * that parsing might create.
     */
    mycxt = AllocSetContextCreate(CurrentMemoryContext,
        "inline_function",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    oldcxt = MemoryContextSwitchTo(mycxt);

    /* Fetch the function body */
    tmp = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_prosrc, &isNull);
    if (isNull)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_UNEXPECTED_NULL_VALUE),
                errmsg("null prosrc for function %u", funcid)));
    src = TextDatumGetCString(tmp);

    /*
     * Setup error traceback support for ereport().  This is so that we can
     * finger the function that bad information came from.
     */
    callback_arg.proname = NameStr(funcform->proname);
    callback_arg.prosrc = src;

    sqlerrcontext.callback = sql_inline_error_callback;
    sqlerrcontext.arg = (void*)&callback_arg;
    sqlerrcontext.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &sqlerrcontext;

    /*
     * Set up to handle parameters while parsing the function body.  We need a
     * dummy FuncExpr node containing the already-simplified arguments to pass
     * to prepare_sql_fn_parse_info.  (It is really only needed if there are
     * some polymorphic arguments, but for simplicity we always build it.)
     */
    fexpr = makeNode(FuncExpr);
    fexpr->funcid = funcid;
    fexpr->funcresulttype = result_type;
    fexpr->funcretset = false;
    fexpr->funcformat = COERCE_DONTCARE; /* doesn't matter */
    fexpr->funccollid = result_collid;   /* doesn't matter */
    fexpr->inputcollid = input_collid;
    fexpr->args = args;
    fexpr->location = -1;

    pinfo = prepare_sql_fn_parse_info(func_tuple, (Node*)fexpr, input_collid);

    /*
     * We just do parsing and parse analysis, not rewriting, because rewriting
     * will not affect table-free-SELECT-only queries, which is all that we
     * care about.	Also, we can punt as soon as we detect more than one
     * command in the function body.
     */
    raw_parsetree_list = pg_parse_query(src);
    if (list_length(raw_parsetree_list) != 1)
        goto fail;

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = src;
    sql_fn_parser_setup(pstate, pinfo);

    querytree = transformTopLevelStmt(pstate, (Node*)linitial(raw_parsetree_list));

    free_parsestate(pstate);

    /*
     * The single command must be a simple "SELECT expression".
     */
    if (!IsA(querytree, Query) || querytree->commandType != CMD_SELECT || querytree->utilityStmt ||
        querytree->hasAggs || querytree->groupingSets || querytree->hasWindowFuncs || querytree->hasSubLinks ||
        querytree->cteList || querytree->rtable || querytree->jointree->fromlist || querytree->jointree->quals ||
        querytree->groupClause || querytree->havingQual || querytree->windowClause || querytree->distinctClause ||
        querytree->sortClause || querytree->limitOffset || querytree->limitCount || querytree->setOperations ||
        (querytree->is_flt_frame && querytree->hasTargetSRFs) ||
        list_length(querytree->targetList) != 1)
        goto fail;

    /*
     * Make sure the function (still) returns what it's declared to.  This
     * will raise an error if wrong, but that's okay since the function would
     * fail at runtime anyway.	Note that check_sql_fn_retval will also insert
     * a RelabelType if needed to make the tlist expression match the declared
     * type of the function.
     *
     * Note: we do not try this until we have verified that no rewriting was
     * needed; that's probably not important, but let's be careful.
     */
    if (check_sql_fn_retval(funcid, result_type, list_make1(querytree), &modifyTargetList, NULL))
        goto fail; /* reject whole-tuple-result cases */

    /* Now we can grab the tlist expression */
    newexpr = (Node*)((TargetEntry*)linitial(querytree->targetList))->expr;

    /* Assert that check_sql_fn_retval did the right thing */
    AssertEreport(exprType(newexpr) == result_type, MOD_OPT, "");
    /* It couldn't have made any dangerous tlist changes, either */
    AssertEreport(!modifyTargetList, MOD_OPT, "");

    /*
     * Additional validity checks on the expression.  It mustn't be more
     * volatile than the surrounding function (this is to avoid breaking hacks
     * that involve pretending a function is immutable when it really ain't).
     * If the surrounding function is declared strict, then the expression
     * must contain only strict constructs and must use all of the function
     * parameters (this is overkill, but an exact analysis is hard).
     */
    if (!querytree->is_flt_frame) {
        if (expression_returns_set(newexpr))
            goto fail;
    }

    if (funcform->provolatile == PROVOLATILE_IMMUTABLE && contain_mutable_functions(newexpr))
        goto fail;
    else if (funcform->provolatile == PROVOLATILE_STABLE && contain_volatile_functions(newexpr))
        goto fail;

    if (funcform->proisstrict && contain_nonstrict_functions(newexpr))
        goto fail;

    /*
     * We may be able to do it; there are still checks on parameter usage to
     * make, but those are most easily done in combination with the actual
     * substitution of the inputs.	So start building expression with inputs
     * substituted.
     */
    usecounts = (int*)palloc0(funcform->pronargs * sizeof(int));
    newexpr = substitute_actual_parameters(newexpr, funcform->pronargs, args, usecounts);

    /* Now check for parameter usage */
    i = 0;
    foreach (arg, args) {
        Node* param = (Node*)lfirst(arg);

        if (usecounts[i] == 0) {
            /* Param not used at all: uncool if func is strict */
            if (funcform->proisstrict)
                goto fail;
        } else if (usecounts[i] != 1) {
            /* Param used multiple times: uncool if expensive or volatile */
            QualCost eval_cost;

            /*
             * We define "expensive" as "contains any subplan or more than 10
             * operators".  Note that the subplan search has to be done
             * explicitly, since cost_qual_eval() will barf on unplanned
             * subselects.
             */
            if (contain_subplans(param))
                goto fail;
            cost_qual_eval(&eval_cost, list_make1(param), NULL);
            if (eval_cost.startup + eval_cost.per_tuple > 10 * u_sess->attr.attr_sql.cpu_operator_cost)
                goto fail;

            /*
             * Check volatility last since this is more expensive than the
             * above tests
             */
            if (contain_volatile_functions(param))
                goto fail;
        }
        i++;
    }

    /*
     * Whew --- we can make the substitution.  Copy the modified expression
     * out of the temporary memory context, and clean up.
     */
    (void)MemoryContextSwitchTo(oldcxt);

    newexpr = (Node*)copyObject(newexpr);

    MemoryContextDelete(mycxt);

    /*
     * If the result is of a collatable type, force the result to expose the
     * correct collation.  In most cases this does not matter, but it's
     * possible that the function result is used directly as a sort key or in
     * other places where we expect exprCollation() to tell the truth.
     */
    if (OidIsValid(result_collid)) {
        Oid exprcoll = exprCollation(newexpr);

        if (OidIsValid(exprcoll) && exprcoll != result_collid) {
            CollateExpr* newnode = makeNode(CollateExpr);
            newnode->arg = (Expr*)newexpr;
            newnode->collOid = result_collid;
            newnode->location = -1;
            newexpr = (Node*)newnode;
        }
    }

    /*
     * Since there is now no trace of the function in the plan tree, we must
     * explicitly record the plan's dependency on the function.
     */
    if (context->root != NULL)
        record_plan_function_dependency(context->root, funcid);

    /*
     * Recursively try to simplify the modified expression.  Here we must add
     * the current function to the context list of active functions.
     */
    context->active_fns = lcons_oid(funcid, context->active_fns);
    newexpr = eval_const_expressions_mutator(newexpr, context);
    context->active_fns = list_delete_first(context->active_fns);

    t_thrd.log_cxt.error_context_stack = sqlerrcontext.previous;

    if (AUDIT_EXEC_ENABLED && funcid >= FirstNormalObjectId) {
        char details[64];
        errno_t rc;
        rc = snprintf_s(details, sizeof(details), sizeof(details) - 1, "Execute inline function(oid = %u). ", funcid);
        securec_check_ss(rc, "", "");
        audit_report(AUDIT_FUNCTION_EXEC, AUDIT_OK, NameStr(funcform->proname), details);
    }

    return (Expr*)newexpr;

    /* Here if func is not inlinable: release temp memory and return NULL */
fail:
    (void)MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(mycxt);
    t_thrd.log_cxt.error_context_stack = sqlerrcontext.previous;

    return NULL;
}

/*
 * Replace var nodes by appropriate join var
 */
Node* substitute_var(Node* expr, List* planTargetList)
{
    substitute_var_context svc;

    svc.planTargetList = planTargetList;

    return substitute_var_mutator(expr, (void*)&svc);
}

static Node* substitute_var_mutator(Node* node, void* context)
{
    if (node == NULL)
        return NULL;

    if (IsA(node, Var)) {
        substitute_var_context* svc = NULL;
        ListCell* lc = NULL;
        int idx = 0;
        Var* oldvar = (Var*)node;
        Var* newvar = NULL;
        List* planTargetList = NIL;

        svc = (substitute_var_context*)context;
        planTargetList = svc->planTargetList;

        foreach (lc, planTargetList) {
            idx++;
            TargetEntry* te = (TargetEntry*)lfirst(lc);
            Var* joinvar = NULL;

            if (!IsA(te->expr, Var))
                continue;

            joinvar = (Var*)te->expr;

            if (joinvar->varno == oldvar->varno && joinvar->varattno == oldvar->varattno) {
                newvar = (Var*)copyObject(oldvar);

                newvar->varno = MERGEJOIN_VAR; /* mainly for ExecQual who does not care this */
                newvar->varattno = idx;
                break;
            }
        }

        if (newvar == NULL)
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    (errmsg("var on action's qual cannot find in target list"))));

        /* repalce var with param */
        return (Node*)newvar;
    }
    return expression_tree_mutator(node, (Node* (*)(Node*, void*)) substitute_var_mutator, (void*)context);
}

/*
 * Replace Param nodes by appropriate actual parameters
 */
static Node* substitute_actual_parameters(Node* expr, int nargs, List* args, int* usecounts)
{
    substitute_actual_parameters_context context;

    context.nargs = nargs;
    context.args = args;
    context.usecounts = usecounts;

    return substitute_actual_parameters_mutator(expr, &context);
}

static Node* substitute_actual_parameters_mutator(Node* node, substitute_actual_parameters_context* context)
{
    if (node == NULL)
        return NULL;
    if (IsA(node, Param)) {
        Param* param = (Param*)node;

        if (param->paramkind != PARAM_EXTERN)
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                    errmsg("unexpected paramkind: %d", (int)param->paramkind)));
        if (param->paramid <= 0 || param->paramid > context->nargs)
            ereport(ERROR,
                (errmodule(MOD_OPT),
                    errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                    errmsg("invalid paramid: %d", param->paramid)));

        /* Count usage of parameter */
        context->usecounts[param->paramid - 1]++;

        /* Select the appropriate actual arg and replace the Param with it */
        /* We don't need to copy at this time (it'll get done later) */
        return (Node*)list_nth(context->args, param->paramid - 1);
    }
    return expression_tree_mutator(
        node, (Node* (*)(Node*, void*)) substitute_actual_parameters_mutator, (void*)context);
}

/*
 * error context callback to let us supply a call-stack traceback
 */
static void sql_inline_error_callback(void* arg)
{
    inline_error_callback_arg* callback_arg = (inline_error_callback_arg*)arg;
    int syntaxerrposition;

    /* If it's a syntax error, convert to internal syntax error report */
    syntaxerrposition = geterrposition();
    if (syntaxerrposition > 0) {
        (void)errposition(0);
        (void)internalerrposition(syntaxerrposition);
        (void)internalerrquery(callback_arg->prosrc);
    }

    errcontext("SQL function \"%s\" during inlining", callback_arg->proname);
}

ExprContext* MakePerTupleExprContextForOpFusion(EState* estate)
{
    ExprContext* econtext = NULL;
    MemoryContext oldcontext;

    /* Create the ExprContext node within the per-query memory context */
    oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

    econtext = makeNode(ExprContext);

    /* Initialize fields of ExprContext */
    econtext->ecxt_scantuple = NULL;
    econtext->ecxt_innertuple = NULL;
    econtext->ecxt_outertuple = NULL;

    econtext->ecxt_per_query_memory = estate->es_query_cxt;

    /*
     * Create working memory for expression evaluation in this context.
     */
    econtext->ecxt_per_tuple_memory = u_sess->iud_expr_reuse_ctx;
    econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
    econtext->ecxt_param_list_info = estate->es_param_list_info;

    econtext->ecxt_aggvalues = NULL;
    econtext->ecxt_aggnulls = NULL;

    econtext->caseValue_datum = (Datum)0;
    econtext->caseValue_isNull = true;

    econtext->domainValue_datum = (Datum)0;
    econtext->domainValue_isNull = true;

    econtext->ecxt_estate = estate;

    econtext->ecxt_callbacks = NULL;
    econtext->plpgsql_estate = NULL;

    /*
     * Link the ExprContext into the EState to ensure it is shut down when the
     * EState is freed.  Because we use lcons(), shutdowns will occur in
     * reverse order of creation, which may not be essential but can't hurt.
     */
    estate->es_exprcontexts = lcons(econtext, estate->es_exprcontexts);

    MemoryContextSwitchTo(oldcontext);
    estate->es_per_tuple_exprcontext = econtext;

    return econtext;
}

/*
 * evaluate_expr: pre-evaluate a constant expression
 *
 * We use the executor's routine ExecEvalExpr() to avoid duplication of
 * code and ensure we get the same result as the executor would get.
 *
 * Note: parameter can_ignore indicates whether ERROR is ignorable when casting type. TRUE if SQL has keyword IGNORE.
 */
Expr* evaluate_expr(Expr* expr, Oid result_type, int32 result_typmod, Oid result_collation, bool can_ignore)
{
    EState* estate = NULL;
    ExprState* exprstate = NULL;
    MemoryContext oldcontext;
    Datum const_val;
    bool const_is_null = false;
    int16 resultTypLen;
    bool resultTypByVal = false;
    bool isFusion = false;
    if (u_sess->iud_expr_reuse_ctx == NULL) {
         u_sess->iud_expr_reuse_ctx = AllocSetContextCreate(u_sess->top_transaction_mem_cxt, "IudExprReuseContext", ALLOCSET_DEFAULT_MINSIZE,
            ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    }
    /*
     * To use the executor, we need an EState.
     */
     if (u_sess->iud_expr_reuse_ctx != NULL) {
        estate = CreateExecutorState();
        isFusion = true;
    } else {
        estate = CreateExecutorState();
    }

    /* We can use the estate's working context to avoid memory leaks. */
    oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

    /* Make sure any opfuncids are filled in. */
    fix_opfuncids((Node*)expr);

    /*
     * Prepare expr for execution.	(Note: we can't use ExecPrepareExpr
     * because it'd result in recursively invoking eval_const_expressions.)
     */
    exprstate = ExecInitExpr(expr, NULL);

    /*
     * And evaluate it.
     *
     * It is OK to use a default econtext because none of the ExecEvalExpr()
     * code used in this situation will use econtext.  That might seem
     * fortuitous, but it's not so unreasonable --- a constant expression does
     * not depend on context, by definition, n'est ce pas?
     */
    if (isFusion && estate->es_per_tuple_exprcontext == NULL) {
        MakePerTupleExprContextForOpFusion(estate);
    }
    ExprContext* econtext = GetPerTupleExprContext(estate);
    if (econtext != NULL) {
        econtext->can_ignore = can_ignore;
    }
    const_val = ExecEvalExprSwitchContext(exprstate, econtext, &const_is_null);

    if (IsA(exprstate, FuncExprState)) {
        FunctionCallInfo fcinfo = &((FuncExprState*)exprstate)->fcinfo_data;
        if (fcinfo->context && IsA(fcinfo->context, FunctionScanState)) {
            pfree_ext(fcinfo->context);
        }
    }

    /* Get info needed about result datatype */
    get_typlenbyval(result_type, &resultTypLen, &resultTypByVal);

    /* Get back to outer memory context */
    (void)MemoryContextSwitchTo(oldcontext);

    /*
     * Must copy result out of sub-context used by expression eval.
     *
     * Also, if it's varlena, forcibly detoast it.  This protects us against
     * storing TOAST pointers into plans that might outlive the referenced
     * data.
     */
    if (!const_is_null) {
        if (resultTypLen == -1)
            const_val = PointerGetDatum(PG_DETOAST_DATUM_COPY(const_val));
        else
            const_val = datumCopy(const_val, resultTypByVal, resultTypLen);
    }

    /* Release all the junk we just created */
    if (!isFusion) {
        FreeExecutorState(estate);
    }

    /*
     * Make the constant result node.
     */
    return (Expr*)makeConst(
        result_type, result_typmod, result_collation, resultTypLen, const_val, const_is_null, resultTypByVal);
}

/*
 * inline_set_returning_function
 *		Attempt to "inline" a set-returning function in the FROM clause.
 *
 * "rte" is an RTE_FUNCTION rangetable entry.  If it represents a call of a
 * set-returning SQL function that can safely be inlined, expand the function
 * and return the substitute Query structure.  Otherwise, return NULL.
 *
 * This has a good deal of similarity to inline_function(), but that's
 * for the non-set-returning case, and there are enough differences to
 * justify separate functions.
 */
Query* inline_set_returning_function(PlannerInfo* root, RangeTblEntry* rte)
{
    FuncExpr* fexpr = NULL;
    Oid func_oid;
    HeapTuple func_tuple;
    Form_pg_proc funcform;
    char* src = NULL;
    Datum tmp;
    bool isNull = false;
    bool modifyTargetList = false;
    MemoryContext oldcxt;
    MemoryContext mycxt;
    List* saveInvalItems = NIL;
    inline_error_callback_arg callback_arg;
    ErrorContextCallback sqlerrcontext;
    SQLFunctionParseInfoPtr pinfo;
    List* raw_parsetree_list = NIL;
    List* querytree_list = NIL;
    Query* querytree = NULL;

    AssertEreport(rte->rtekind == RTE_FUNCTION, MOD_OPT, "");

    /*
     * It doesn't make a lot of sense for a SQL SRF to refer to itself in its
     * own FROM clause, since that must cause infinite recursion at runtime.
     * It will cause this code to recurse too, so check for stack overflow.
     * (There's no need to do more.)
     */
    check_stack_depth();

    /* Fail if FROM item isn't a simple FuncExpr */
    fexpr = (FuncExpr*)rte->funcexpr;
    if (fexpr == NULL || !IsA(fexpr, FuncExpr))
        return NULL;
    func_oid = fexpr->funcid;

    /*
     * The function must be declared to return a set, else inlining would
     * change the results if the contained SELECT didn't return exactly one
     * row.
     */
    if (!fexpr->funcretset)
        return NULL;

    /*
     * Refuse to inline if the arguments contain any volatile functions or
     * sub-selects.  Volatile functions are rejected because inlining may
     * result in the arguments being evaluated multiple times, risking a
     * change in behavior.	Sub-selects are rejected partly for implementation
     * reasons (pushing them down another level might change their behavior)
     * and partly because they're likely to be expensive and so multiple
     * evaluation would be bad.
     */
    if (contain_volatile_functions((Node*)fexpr->args) || contain_subplans((Node*)fexpr->args))
        return NULL;

    /* Check permission to call function (fail later, if not) */
    if (pg_proc_aclcheck(func_oid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
        return NULL;

    /* Check whether a plugin wants to hook function entry/exit */
    if (FmgrHookIsNeeded(func_oid))
        return NULL;

    /*
     * OK, let's take a look at the function's pg_proc entry.
     */
    func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
    if (!HeapTupleIsValid(func_tuple))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for function %u", func_oid)));
    funcform = (Form_pg_proc)GETSTRUCT(func_tuple);

    /*
     * Forget it if the function is not SQL-language or has other showstopper
     * properties.	In particular it mustn't be declared STRICT, since we
     * couldn't enforce that.  It also mustn't be VOLATILE, because that is
     * supposed to cause it to be executed with its own snapshot, rather than
     * sharing the snapshot of the calling query.  (Rechecking proretset is
     * just paranoia.)
     */
    if (funcform->prolang != SQLlanguageId || funcform->proisstrict || funcform->provolatile == PROVOLATILE_VOLATILE ||
        funcform->prosecdef || !funcform->proretset || !heap_attisnull(func_tuple, Anum_pg_proc_proconfig, NULL)) {
        ReleaseSysCache(func_tuple);
        return NULL;
    }

    /*
     * Make a temporary memory context, so that we don't leak all the stuff
     * that parsing might create.
     */
    mycxt = AllocSetContextCreate(CurrentMemoryContext,
        "inline_set_returning_function",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    oldcxt = MemoryContextSwitchTo(mycxt);

    /*
     * When we call eval_const_expressions below, it might try to add items to
     * root->glob->invalItems.	Since it is running in the temp context, those
     * items will be in that context, and will need to be copied out if we're
     * successful.	Temporarily reset the list so that we can keep those items
     * separate from the pre-existing list contents.
     */
    saveInvalItems = root->glob->invalItems;
    root->glob->invalItems = NIL;

    /* Fetch the function body */
    tmp = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_prosrc, &isNull);
    if (isNull)
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_UNEXPECTED_NULL_VALUE),
                errmsg("null prosrc for function %u", func_oid)));
    src = TextDatumGetCString(tmp);

    /*
     * Setup error traceback support for ereport().  This is so that we can
     * finger the function that bad information came from.
     */
    callback_arg.proname = NameStr(funcform->proname);
    callback_arg.prosrc = src;

    sqlerrcontext.callback = sql_inline_error_callback;
    sqlerrcontext.arg = (void*)&callback_arg;
    sqlerrcontext.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &sqlerrcontext;

    /*
     * Run eval_const_expressions on the function call.  This is necessary to
     * ensure that named-argument notation is converted to positional notation
     * and any default arguments are inserted.	It's a bit of overkill for the
     * arguments, since they'll get processed again later, but no harm will be
     * done.
     */
    fexpr = (FuncExpr*)eval_const_expressions(root, (Node*)fexpr);

    /* It should still be a call of the same function, but let's check */
    if (!IsA(fexpr, FuncExpr) || fexpr->funcid != func_oid)
        goto fail;

    /* Arg list length should now match the function */
    if (list_length(fexpr->args) != funcform->pronargs)
        goto fail;

    /*
     * Set up to handle parameters while parsing the function body.  We can
     * use the FuncExpr just created as the input for
     * prepare_sql_fn_parse_info.
     */
    pinfo = prepare_sql_fn_parse_info(func_tuple, (Node*)fexpr, fexpr->inputcollid);

    /*
     * Parse, analyze, and rewrite (unlike inline_function(), we can't skip
     * rewriting here).  We can fail as soon as we find more than one query,
     * though.
     */
    raw_parsetree_list = pg_parse_query(src);
    if (list_length(raw_parsetree_list) != 1)
        goto fail;

    querytree_list = pg_analyze_and_rewrite_params(
        (Node*)linitial(raw_parsetree_list), src, (ParserSetupHook)sql_fn_parser_setup, pinfo);
    if (list_length(querytree_list) != 1)
        goto fail;
    querytree = (Query*)linitial(querytree_list);

    /*
     * The single command must be a plain SELECT.
     */
    if (!IsA(querytree, Query) || querytree->commandType != CMD_SELECT || querytree->utilityStmt)
        goto fail;

    /*
     * Make sure the function (still) returns what it's declared to.  This
     * will raise an error if wrong, but that's okay since the function would
     * fail at runtime anyway.	Note that check_sql_fn_retval will also insert
     * RelabelType(s) and/or NULL columns if needed to make the tlist
     * expression(s) match the declared type of the function.
     *
     * If the function returns a composite type, don't inline unless the check
     * shows it's returning a whole tuple result; otherwise what it's
     * returning is a single composite column which is not what we need.
     */
    if (!check_sql_fn_retval(func_oid, fexpr->funcresulttype, querytree_list, &modifyTargetList, NULL) &&
        (get_typtype(fexpr->funcresulttype) == TYPTYPE_COMPOSITE
            || get_typtype(fexpr->funcresulttype) == TYPTYPE_ABSTRACT_OBJECT || fexpr->funcresulttype == RECORDOID))
        goto fail; /* reject not-whole-tuple-result cases */

    /*
     * If we had to modify the tlist to make it match, and the statement is
     * one in which changing the tlist contents could change semantics, we
     * have to punt and not inline.
     */
    if (modifyTargetList) {
        goto fail;
    }

    /*
     * If it returns RECORD, we have to check against the column type list
     * provided in the RTE; check_sql_fn_retval can't do that.  (If no match,
     * we just fail to inline, rather than complaining; see notes for
     * tlist_matches_coltypelist.)	We don't have to do this for functions
     * with declared OUT parameters, even though their funcresulttype is
     * RECORDOID, so check get_func_result_type too.
     */
    if (fexpr->funcresulttype == RECORDOID && get_func_result_type(func_oid, NULL, NULL) == TYPEFUNC_RECORD &&
        !tlist_matches_coltypelist(querytree->targetList, rte->funccoltypes))
        goto fail;

    /*
     * Looks good --- substitute parameters into the query.
     */
    querytree = substitute_actual_srf_parameters(querytree, funcform->pronargs, fexpr->args);

    /*
     * Copy the modified query out of the temporary memory context, and clean
     * up.
     */
    (void)MemoryContextSwitchTo(oldcxt);

    querytree = (Query*)copyObject(querytree);

    /* copy up any new invalItems, too */
    root->glob->invalItems = list_concat(saveInvalItems, (List*)copyObject(root->glob->invalItems));

    MemoryContextDelete(mycxt);
    t_thrd.log_cxt.error_context_stack = sqlerrcontext.previous;
    ReleaseSysCache(func_tuple);

    /*
     * We don't have to fix collations here because the upper query is already
     * parsed, ie, the collations in the RTE are what count.
     */
    /*
     * Since there is now no trace of the function in the plan tree, we must
     * explicitly record the plan's dependency on the function.
     */
    record_plan_function_dependency(root, func_oid);

    return querytree;

    /* Here if func is not inlinable: release temp memory and return NULL */
fail:
    (void)MemoryContextSwitchTo(oldcxt);
    root->glob->invalItems = saveInvalItems;
    MemoryContextDelete(mycxt);
    t_thrd.log_cxt.error_context_stack = sqlerrcontext.previous;
    ReleaseSysCache(func_tuple);

    return NULL;
}

/*
 * Replace Param nodes by appropriate actual parameters
 *
 * This is just enough different from substitute_actual_parameters()
 * that it needs its own code.
 */
static Query* substitute_actual_srf_parameters(Query* expr, int nargs, List* args)
{
    substitute_actual_srf_parameters_context context;

    context.nargs = nargs;
    context.args = args;
    context.sublevels_up = 1;

    return query_tree_mutator(expr, (Node* (*)(Node*, void*)) substitute_actual_srf_parameters_mutator, &context, 0);
}

static Node* substitute_actual_srf_parameters_mutator(Node* node, substitute_actual_srf_parameters_context* context)
{
    Node* result = NULL;

    if (node == NULL)
        return NULL;
    if (IsA(node, Query)) {
        context->sublevels_up++;
        result = (Node*)query_tree_mutator(
            (Query*)node, (Node* (*)(Node*, void*)) substitute_actual_srf_parameters_mutator, (void*)context, 0);
        context->sublevels_up--;
        return result;
    }
    if (IsA(node, Param)) {
        Param* param = (Param*)node;

        if (param->paramkind == PARAM_EXTERN) {
            if (param->paramid <= 0 || param->paramid > context->nargs)
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                        errmsg("invalid paramid: %d", param->paramid)));

            /*
             * Since the parameter is being inserted into a subquery, we must
             * adjust levels.
             */
            result = (Node*)copyObject(list_nth(context->args, param->paramid - 1));
            IncrementVarSublevelsUp(result, context->sublevels_up, 0);
            return result;
        }
    }
    return expression_tree_mutator(
        node, (Node* (*)(Node*, void*)) substitute_actual_srf_parameters_mutator, (void*)context);
}

/*
 * Check whether a SELECT targetlist emits the specified column types,
 * to see if it's safe to inline a function returning record.
 *
 * We insist on exact match here.  The executor allows binary-coercible
 * cases too, but we don't have a way to preserve the correct column types
 * in the correct places if we inline the function in such a case.
 *
 * Note that we only check type OIDs not typmods; this agrees with what the
 * executor would do at runtime, and attributing a specific typmod to a
 * function result is largely wishful thinking anyway.
 */
static bool tlist_matches_coltypelist(List* tlist, List* coltypelist)
{
    ListCell* tlistitem = NULL;
    ListCell* clistitem = NULL;

    clistitem = list_head(coltypelist);
    foreach (tlistitem, tlist) {
        TargetEntry* tle = (TargetEntry*)lfirst(tlistitem);
        Oid coltype;

        if (tle->resjunk)
            continue; /* ignore junk columns */

        if (clistitem == NULL)
            return false; /* too many tlist items */

        coltype = lfirst_oid(clistitem);
        clistitem = lnext(clistitem);

        if (exprType((Node*)tle->expr) != coltype)
            return false; /* column type mismatch */
    }

    if (clistitem != NULL)
        return false; /* too few tlist items */

    return true;
}

/* Support predicate pushing down to cstore scan.
 * Fileter the supported clause.
 */
bool filter_cstore_clause(PlannerInfo* root, Expr* clause)
{
    bool plain_op = false;

    /* Here only support the and clause. If the qual is or clause, the tag of clause is T_BoolExpr */
    if (IsA(clause, OpExpr)) {
        Node* leftop = NULL;
        Node* rightop = NULL;
        OpExpr* op_clause = (OpExpr*)clause;

        if (list_length(op_clause->args) != 2 || !is_operator_pushdown(op_clause->opno))
            return plain_op;

        leftop = get_leftop(clause);
        rightop = get_rightop(clause);

        /* Support var op const, const op var */
        if (leftop && is_var_node(leftop)) {
            if (rightop && (IsA(rightop, Const) || is_exec_external_param_const(root, rightop)))
                plain_op = true;
        }

        if (rightop && is_var_node(rightop)) {
            if (leftop && (IsA(leftop, Const) || is_exec_external_param_const(root, leftop))) {
                plain_op = true;
                CommuteOpExpr(op_clause);
                set_opfuncid(op_clause);
            }
        }
    }

    return plain_op;
}

bool is_var_node(Node* node)
{
    bool is_var = false;

    if (IsA(node, Var) && ((Var*)node)->varattno > 0) {
        is_var = true;
    } else if (IsA(node, RelabelType)) {
        RelabelType* reltype = (RelabelType*)node;

        if (IsA(reltype->arg, Var) && ((Var*)reltype->arg)->varattno > 0)
            is_var = true;
    }

    return is_var;
}

static bool is_exec_external_param_const(PlannerInfo* root, Node* node)
{
    bool is_param_const = false;

    if (node && nodeTag(node) == T_Param) {
        Param* param = (Param*)node;
        if (param->paramkind == PARAM_EXTERN || param->paramkind == PARAM_EXEC)
            is_param_const = true;
    }

    return is_param_const;
}

static bool is_operator_pushdown(Oid opno)
{
    bool is_pushdown = false;
    HeapTuple tp;
    tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));

    if (HeapTupleIsValid(tp)) {
        Form_pg_operator optup = (Form_pg_operator)GETSTRUCT(tp);
        if ((strncmp(NameStr(optup->oprname), "<", NAMEDATALEN) == 0) ||
            (strncmp(NameStr(optup->oprname), ">", NAMEDATALEN) == 0) ||
            (strncmp(NameStr(optup->oprname), "=", NAMEDATALEN) == 0) ||
            (strncmp(NameStr(optup->oprname), ">=", NAMEDATALEN) == 0) ||
            (strncmp(NameStr(optup->oprname), "<=", NAMEDATALEN) == 0))
            is_pushdown = true;
    } else {
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                (errmsg("cache lookup failed for type %u", opno))));
    }

    ReleaseSysCache(tp);

    return is_pushdown;
}

/*
 * check_param_for_specfunc
 *	Check all the other params must be Const except the first param for the special function.
 *
 * Parameters:
 *	@in args: all the parameters for the special function's agrs
 *
 * Return:
 *    return true if all the other params of special function are Const except the first param.
 */
static bool check_param_for_specfunc(List* args)
{
    ListCell* l = NULL;

    for_each_cell(l, lnext(list_head(args)))
    {
        /*
         * Check all the other params must be Const except
         * the first param for the special function.
         */
        if (!IsA(((Node*)lfirst(l)), Const))
            return false;
    }

    return true;
}

/*
 * contain_var_unsubstitutable_functions
 *	  Recursively search for functions within a clause that could not use
 * column stats as function stats.
 *
 * Notes : We only consider btrim/rtrim/ltrim functions, since coalesce
 * expression can use column stats as a substitution.
 */
bool contain_var_unsubstitutable_functions(Node* clause)
{
    return contain_var_unsubstitutable_functions_walker(clause, NULL);
}

static bool contain_var_unsubstitutable_functions_walker(Node* node, void* context)
{
    if (IsA(node, Var)) {
        return false;
    } else if (IsA(node, FuncExpr)) {
        FuncExpr* expr = (FuncExpr*)node;
        List* args = ((FuncExpr*)node)->args;

        if (is_accurate_estimatable_func(expr->funcid) && check_param_for_specfunc(args))
            return contain_var_unsubstitutable_functions_walker((Node*)linitial(args), context);

        return true;
    } else if (IsA(node, CoalesceExpr)) {
        List* args = ((CoalesceExpr*)node)->args;

        if (check_param_for_specfunc(args))
            return contain_var_unsubstitutable_functions_walker((Node*)linitial(((CoalesceExpr*)node)->args), context);

        return true;
    } else if (IsA(node, RelabelType)) {
        return contain_var_unsubstitutable_functions_walker((Node*)((RelabelType*)node)->arg, context);
    } else {
        // these cases include NULL/Const/Param/CaseTestExpr/Aggref/WindowFunc/ArrayRef/FuncExpr
        // OpExpr/DistinctExpr/NullIfExpr/ScalarArrayOpExpr/BoolExpr/SubLink/SubPlan/AlternativeSubPlan
        // FieldSelect/FieldStore/ArrayExpr/CaseExpr/RowExpr/RowCompareExpr/MinMaxExpr/XmlExpr
        // NullTest/BooleanTest/CoerceViaIO/ConvertRowtypeExpr/CoerceToDomain/CoerceToDomainValue
        // CurrentOfExpr/ArrayCoerceExpr/List/TargetEntry
        return true;
    }
}

static bool is_accurate_estimatable_func(Oid funcId)
{
    static uint estimatable_func[] = { // ltrim
        LTRIMFUNCOID,
        LTRIMPARAFUNCOID,
        // rtrim
        RTRIMFUNCOID,
        RTRIMPARAFUNCOID,
        // btrim
        BTRIMFUNCOID,
        BTRIMPARAFUNCOID,
        /* lpad */
        LPADFUNCOID,
        LPADPARAFUNCOID,
        /* rpad */
        RPADFUNCOID,
        RPADPARAFUNCOID};

    if (funcId >= FirstNormalObjectId)
        return false;

    for (uint i = 0; i < lengthof(estimatable_func); i++) {
        if (funcId == estimatable_func[i])
            return true;
    }

    return false;
}

/*
 * check whether the node have rownum expr or not, test all kinds of nodes,
 * check if it has the volatile rownum. If the node include rownum
 * function, it will return true, else it will return false.
 */
bool contain_rownum_walker(Node *node, void *context)
{
    if (node == NULL) {
        return false;
    }

    if (IsA(node, Rownum)) {
        return true;
    }

    return expression_tree_walker(node, (bool (*)())contain_rownum_walker, context);
}


extern bool ContainRownumExpr(Node *node)
{
    return contain_rownum_walker(node, NULL);
}

/*
 * get the jtnode's qualifiers list, make query tree's table jion tree's
 * qualifiers to be a list. Then return the list. (parse->jointree->quals:
 * 'where clause' and 'and clause')
 */
List *get_quals_lists(Node *jtnode)
{
    if (jtnode == NULL || !IsA(jtnode, FromExpr)) {
        return NULL;
    }

    FromExpr *expr = (FromExpr *)jtnode;
    List *quallist = make_ands_implicit((Expr *)expr->quals);

    if (expr->quals != NULL && and_clause((Node *)expr->quals)) {
        quallist = list_copy(quallist);
    }

    return quallist;
}

#ifdef USE_SPQ

/*
 * fold_constants
 *
 * Recurses into query tree and folds all constant expressions.
 */
Query *fold_constants(PlannerInfo *root, Query *q, ParamListInfo boundParams, Size max_size)
{
    eval_const_expressions_context context;

    context.root = root;
    context.boundParams = boundParams;
    context.active_fns = NIL;	/* nothing being recursively simplified */
    context.case_val = NULL;	/* no CASE being examined */
    context.estimate = false;	/* safe transformations only */
    context.recurse_queries = true; /* recurse into query structures */
    context.recurse_sublink_testexpr = false; /* do not recurse into sublink test expressions */

    context.max_size = max_size;

    return (Query *) query_or_expression_tree_mutator(
        (Node *) q,
        (Node* (*)(Node*, void*)) eval_const_expressions_mutator,
        &context,0);
}

/*
 * flatten_join_alias_var_optimizer
 *	  Replace Vars that reference JOIN outputs with references to the original
 *	  relation variables instead.
 */
Query * flatten_join_alias_var_optimizer(Query *query, int queryLevel)
{
    Query *queryNew = (Query *) copyObject(query);
 
    /*
     * Flatten join alias for expression in
     * 1. targetlist
     * 2. returningList
     * 3. having qual
     * 4. scatterClause
     * 5. limit offset
     * 6. limit count
     *
     * We flatten the above expressions since these entries may be moved during the query
     * normalization step before algebrization. In contrast, the planner flattens alias
     * inside quals to allow predicates involving such vars to be pushed down.
     *
     * Here we ignore the flattening of quals due to the following reasons:
     * 1. we assume that the function will be called before Query->DXL translation:
     * 2. the quals never gets moved from old query to the new top-level query in the
     * query normalization phase before algebrization. In other words, the quals hang of
     * the same query structure that is now the new derived table.
     * 3. the algebrizer can resolve the abiquity of join aliases in quals since we maintain
     * all combinations of <query level, varno, varattno> to DXL-ColId during Query->DXL translation.
     *
     */
 
    return queryNew;
}
 
Expr *transform_array_Const_to_ArrayExpr(Const *c)
{
    Oid elemtype;
    int16 elemlen;
    bool elembyval;
    char elemalign;
    int nelems;
    Datum *elems;
    bool *nulls;
    ArrayType *ac;
    ArrayExpr *aexpr;
    int i;
 
    Assert(IsA(c, Const));
 
    /* Does it look like the right kind of an array Const? */
    if (c->constisnull)
        return (Expr *)c; /* NULL const */
 
    elemtype = get_element_type(c->consttype);
    if (elemtype == InvalidOid)
        return (Expr *)c; /* not an array */
 
    ac = DatumGetArrayTypeP(c->constvalue);
    nelems = ArrayGetNItems(ARR_NDIM(ac), ARR_DIMS(ac));
 
    /* All set, extract the elements, and an ArrayExpr to hold them. */
    get_typlenbyvalalign(elemtype, &elemlen, &elembyval, &elemalign);
    deconstruct_array(ac, elemtype, elemlen, elembyval, elemalign, &elems, &nulls, &nelems);
 
    aexpr = makeNode(ArrayExpr);
    aexpr->array_typeid = c->consttype;
    aexpr->element_typeid = elemtype;
    aexpr->multidims = false;
    aexpr->location = c->location;
 
    for (i = 0; i < nelems; i++) {
        aexpr->elements =
            lappend(aexpr->elements, makeConst(elemtype, -1, c->constcollid, elemlen, elems[i], nulls[i], elembyval));
    }
 
    return (Expr *)aexpr;
}
#endif
