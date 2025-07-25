/* -------------------------------------------------------------------------
 *
 * parse_clause.h
 *	  handle clauses in parser
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 * src/include/parser/parse_clause.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "nodes/relation.h"
#include "parser/parse_node.h"

extern void transformFromClause(ParseState* pstate, List* frmList, bool isFirstNode = true, bool isCreateView = false,
    bool isUpdate = false);
extern List* setTargetTables(ParseState* pstate, List* relation, bool expandInh, bool alsoSource, AclMode requiredPerms);
extern bool interpretInhOption(InhOption inhOpt);
extern bool interpretOidsOption(List* defList);

extern Node* transformFromClauseItem(ParseState* pstate, Node* n, RangeTblEntry** top_rte, int* top_rti,
    RangeTblEntry** right_rte, int* right_rti, List** relnamespace, bool isFirstNode = true,
    bool isCreateView = false, bool isMergeInto = false, bool isUpdate = false);

extern Node* transformJoinOnClause(ParseState* pstate, JoinExpr* j, RangeTblEntry* l_rte, RangeTblEntry* r_rte,
    List* relnamespace);
extern Node* transformWhereClause(ParseState* pstate, Node* clause, ParseExprKind exprKind, const char* constructName);
extern Node* transformLimitClause(ParseState* pstate, Node* clause, ParseExprKind exprKind, const char* constructName);
extern List* transformGroupClause(
    ParseState* pstate, List* grouplist, List** groupingSets, List** targetlist, List* sortClause, ParseExprKind exprKind, bool useSQL99);
extern List* transformSortClause(
    ParseState* pstate, List* orderlist, List** targetlist, ParseExprKind exprKind, bool resolveUnknown, bool useSQL99);

extern List* transformWindowDefinitions(ParseState* pstate, List* windowdefs, List** targetlist);

extern List* transformDistinctClause(ParseState* pstate, List** targetlist, List* sortClause, bool is_agg);
extern List* transformDistinctOnClause(ParseState* pstate, List* distinctlist, List** targetlist, List* sortClause);

extern Index assignSortGroupRef(TargetEntry* tle, List* tlist);
extern bool targetIsInSortList(TargetEntry* tle, Oid sortop, List* sortList);

extern List* addTargetToSortList(
    ParseState* pstate, TargetEntry* tle, List* sortlist, List* targetlist, SortBy* sortby, bool resolveUnknown);
extern ParseNamespaceItem *makeNamespaceItem(RangeTblEntry *rte, bool lateral_only, bool lateral_ok);

/*
 * StartWith support transformStartWith() is the only entry point for START WITH...CONNECT BY
 * processing in parser/transformar layer
 */
extern void transformStartWith(ParseState *pstate, ParseState *origin_pstate, SelectStmt *stmt,
                                SelectStmt *origin_stmt, Query *qry, bool is_first_node, bool is_creat_view);
extern void AddStartWithCTEPseudoReturnColumns(CommonTableExpr *cte, RangeTblEntry *rte, Index rte_index);
extern void pretransformAggWithUserSet(ParseState* pstate, List** targetList, Node* groupClause, ParseExprKind exprKind);
extern void setNamespaceLateralState(List *l_namespace, bool lateral_only, bool lateral_ok);
#endif /* PARSE_CLAUSE_H */
