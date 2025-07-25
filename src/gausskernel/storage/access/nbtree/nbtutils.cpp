/* -------------------------------------------------------------------------
 *
 * nbtutils.cpp
 *	  Utility code for openGauss btree implementation.
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/access/nbtree/nbtutils.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <time.h>
#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/fmgroids.h"

typedef struct BTSortArrayContext {
    FmgrInfo flinfo;
    Oid collation;
    bool reverse;
} BTSortArrayContextInfo;

static Datum _bt_find_extreme_element(IndexScanDesc scan, ScanKey skey, StrategyNumber strat, const Datum *elems,
                                      int nelems);
static int _bt_compare_array_elements(const void *a, const void *b, void *arg);
static bool _bt_compare_scankey_args(IndexScanDesc scan, ScanKey op, ScanKey leftarg, ScanKey rightarg, bool *result);
static bool _bt_fix_scankey_strategy(ScanKey skey, const int16 *indoption);
static void _bt_mark_scankey_required(ScanKey skey);
static int btree_num_keep_atts(Relation rel, IndexTuple lastleft, IndexTuple firstright, BTScanInsert itup_key);

/*
 * _bt_mkscankey
 *		Build an insertion scan key that contains comparison data from itup
 *		as well as comparator routines appropriate to the key datatypes.
 *
 *		The result is intended for use with _bt_compare().
 */
BTScanInsert _bt_mkscankey(Relation rel, IndexTuple itup)
{
    BTScanInsert key;
    TupleDesc itupdesc;
    int indnatts PG_USED_FOR_ASSERTS_ONLY;
    int indnkeyatts;
    int num_tuple_attrs = itup ? BTREE_TUPLE_GET_NUM_OF_ATTS(itup, rel) : 0;
    int16* indoption = NULL;

    itupdesc = RelationGetDescr(rel);
    indnatts = IndexRelationGetNumberOfAttributes(rel);
    indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
    indoption = rel->rd_indoption;

    Assert(indnkeyatts != 0);
    Assert(indnkeyatts <= indnatts);
    Assert(num_tuple_attrs <= indnatts);
    /*
     * We'll execute search using ScanKey constructed on key columns. Non key
     * (included) columns must be omitted.
     */
    key = (BTScanInsert)palloc(offsetof(BTScanInsertData, scankeys) + indnkeyatts * sizeof(ScanKeyData));

    if (IsSystemRelation(rel) || t_thrd.proc->workingVersionNum < NBTREE_INSERT_OPTIMIZATION_VERSION_NUM) {
        key->heapkeyspace = false;
        key->allequalimage = false;
    } else if (itup) {
        btree_meta_version(rel, &key->heapkeyspace, &key->allequalimage);
    } else {
        key->heapkeyspace = true;
        key->allequalimage = false;
    }
	key->anynullkeys = false;	/* initial assumption */
	key->nextkey = false;
	key->pivotsearch = false;
	key->keysz = Min(indnkeyatts, num_tuple_attrs);
    key->scantid = key->heapkeyspace && itup ? btree_tuple_get_heap_tid(itup) : NULL;

    for (int i = 0; i < indnkeyatts; i++) {
        FmgrInfo* procinfo = NULL;
        Datum arg;
        bool null = false;
        uint32 flags;

        /*
         * We can use the cached (default) support procs since no cross-type
         * comparison can be needed.
         */
        procinfo = index_getprocinfo(rel, i + 1, (uint16)BTORDER_PROC);

		if (i < num_tuple_attrs)
			arg = index_getattr(itup, i + 1, itupdesc, &null);
		else
		{
			arg = (Datum) 0;
			null = true;
		}
        flags = (null ? SK_ISNULL : 0) | (((uint16)indoption[i]) << SK_BT_INDOPTION_SHIFT);
        ScanKeyEntryInitializeWithInfo(&key->scankeys[i], flags, (AttrNumber)(i + 1), InvalidStrategy, InvalidOid,
                                       rel->rd_indcollation[i], procinfo, arg);
    }

    return key;
}

/*
 * _bt_mkscankey_nodata
 *		Build an insertion scan key that contains 3-way comparator routines
 *		appropriate to the key datatypes, but no comparison data.  The
 *		comparison data ultimately used must match the key datatypes.
 *
 *		The result cannot be used with _bt_compare(), unless comparison
 *		data is first stored into the key entries.	Currently this
 *		routine is only called by nbtsort.c and tuplesort.c, which have
 *		their own comparison routines.
 */
ScanKey _bt_mkscankey_nodata(Relation rel)
{
    ScanKey skey;
    int indnkeyatts;
    int16* indoption = NULL;
    int i;

    indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
    indoption = rel->rd_indoption;

    skey = (ScanKey) palloc(indnkeyatts * sizeof(ScanKeyData));

    for (i = 0; i < indnkeyatts; i++) {
        FmgrInfo* procinfo = NULL;
        uint32 flags;

        /*
         * We can use the cached (default) support procs since no cross-type
         * comparison can be needed.
         */
        procinfo = index_getprocinfo(rel, i + 1, (uint16)BTORDER_PROC);
        flags = SK_ISNULL | (((uint16)indoption[i]) << SK_BT_INDOPTION_SHIFT);
        ScanKeyEntryInitializeWithInfo(&skey[i], flags, (AttrNumber)(i + 1), InvalidStrategy, InvalidOid,
                                       rel->rd_indcollation[i], procinfo, (Datum)0);
    }

    return skey;
}

/*
 * free a scan key made by either _bt_mkscankey or _bt_mkscankey_nodata.
 */
void _bt_freeskey(ScanKey skey)
{
    pfree(skey);
    skey = NULL;
}

/*
 * free a retracement stack made by _bt_search.
 */
void _bt_freestack(BTStack stack)
{
    BTStack ostack;

    while (stack != NULL) {
        ostack = stack;
        stack = stack->bts_parent;
        pfree(ostack);
        ostack = NULL;
    }
}

/*
 *	_bt_preprocess_array_keys() -- Preprocess SK_SEARCHARRAY scan keys
 *
 * If there are any SK_SEARCHARRAY scan keys, deconstruct the array(s) and
 * set up BTArrayKeyInfo info for each one that is an equality-type key.
 * Prepare modified scan keys in so->arrayKeyData, which will hold the current
 * array elements during each primitive indexscan operation.  For inequality
 * array keys, it's sufficient to find the extreme element value and replace
 * the whole array with that scalar value.
 *
 * Note: the reason we need so->arrayKeyData, rather than just scribbling
 * on scan->keyData, is that callers are permitted to call btrescan without
 * supplying a new set of scankey data.
 */
void _bt_preprocess_array_keys(IndexScanDesc scan)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    int numberOfKeys = scan->numberOfKeys;
    int16 *indoption = scan->indexRelation->rd_indoption;
    int numArrayKeys;
    ScanKey cur;
    int i;
    MemoryContext oldContext;
    errno_t rc;

    /* Quick check to see if there are any array keys */
    numArrayKeys = 0;
    for (i = 0; i < numberOfKeys; i++) {
        cur = &scan->keyData[i];
        if (cur->sk_flags & SK_SEARCHARRAY) {
            numArrayKeys++;
            Assert(!(cur->sk_flags & (SK_ROW_HEADER | SK_SEARCHNULL | SK_SEARCHNOTNULL)));
            /* If any arrays are null as a whole, we can quit right now. */
            if (cur->sk_flags & SK_ISNULL) {
                so->numArrayKeys = -1;
                so->arrayKeyData = NULL;
                return;
            }
        }
    }

    /* Quit if nothing to do. */
    if (numArrayKeys == 0) {
        so->numArrayKeys = 0;
        so->arrayKeyData = NULL;
        return;
    }

    /*
     * Make a scan-lifespan context to hold array-associated data, or reset it
     * if we already have one from a previous rescan cycle.
     */
    if (so->arrayContext == NULL) {
        so->arrayContext = AllocSetContextCreate(CurrentMemoryContext, "BTree Array Context", ALLOCSET_SMALL_MINSIZE,
                                                 ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);
    } else {
        MemoryContextReset(so->arrayContext);
    }

    oldContext = MemoryContextSwitchTo(so->arrayContext);

    /* Create modifiable copy of scan->keyData in the workspace context */
    so->arrayKeyData = (ScanKey)palloc(scan->numberOfKeys * sizeof(ScanKeyData));
    rc = memcpy_s(so->arrayKeyData, scan->numberOfKeys * sizeof(ScanKeyData), scan->keyData,
                  scan->numberOfKeys * sizeof(ScanKeyData));
    securec_check(rc, "", "");

    /* Allocate space for per-array data in the workspace context */
    so->arrayKeys = (BTArrayKeyInfo *)palloc0(numArrayKeys * sizeof(BTArrayKeyInfo));

    /* Now process each array key */
    numArrayKeys = 0;
    for (i = 0; i < numberOfKeys; i++) {
        ArrayType *arrayval = NULL;
        int16 elmlen;
        bool elmbyval = false;
        char elmalign;
        int num_elems;
        Datum *elem_values = NULL;
        bool *elem_nulls = NULL;
        int num_nonnulls;
        int j;

        cur = &so->arrayKeyData[i];
        if (!(cur->sk_flags & SK_SEARCHARRAY)) {
            continue;
        }

        /*
         * First, deconstruct the array into elements.	Anything allocated
         * here (including a possibly detoasted array value) is in the
         * workspace context.
         */
        arrayval = DatumGetArrayTypeP(cur->sk_argument);
        /* We could cache this data, but not clear it's worth it */
        get_typlenbyvalalign(ARR_ELEMTYPE(arrayval), &elmlen, &elmbyval, &elmalign);
        deconstruct_array(arrayval, ARR_ELEMTYPE(arrayval), elmlen, elmbyval, elmalign, &elem_values, &elem_nulls,
                          &num_elems);

        /*
         * Compress out any null elements.	We can ignore them since we assume
         * all btree operators are strict.
         */
        num_nonnulls = 0;
        for (j = 0; j < num_elems; j++) {
            if (!elem_nulls[j]) {
                elem_values[num_nonnulls++] = elem_values[j];
            }
        }

        /* We could pfree(elem_nulls) now, but not worth the cycles
         * If there's no non-nulls, the scan qual is unsatisfiable
         */
        if (num_nonnulls == 0) {
            numArrayKeys = -1;
            break;
        }

        /*
         * If the comparison operator is not equality, then the array qual
         * degenerates to a simple comparison against the smallest or largest
         * non-null array element, as appropriate.
         */
        switch (cur->sk_strategy) {
            case BTLessStrategyNumber:
            case BTLessEqualStrategyNumber:
                cur->sk_argument = _bt_find_extreme_element(scan, cur, BTGreaterStrategyNumber, elem_values,
                                                            num_nonnulls);
                continue;
            case BTEqualStrategyNumber:
                /* proceed with rest of loop */
                break;
            case BTGreaterEqualStrategyNumber:
            case BTGreaterStrategyNumber:
                cur->sk_argument = _bt_find_extreme_element(scan, cur, BTLessStrategyNumber, elem_values, num_nonnulls);
                continue;
            default:
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("unrecognized StrategyNumber: %d", (int)cur->sk_strategy)));
                break;
        }

        /*
         * Sort the non-null elements and eliminate any duplicates.  We must
         * sort in the same ordering used by the index column, so that the
         * successive primitive indexscans produce data in index order.
         */
        num_elems = _bt_sort_array_elements(scan, cur, (indoption[cur->sk_attno - 1] & INDOPTION_DESC) != 0,
                                            elem_values, num_nonnulls);

        /*
         * And set up the BTArrayKeyInfo data.
         */
        so->arrayKeys[numArrayKeys].scan_key = i;
        so->arrayKeys[numArrayKeys].num_elems = num_elems;
        so->arrayKeys[numArrayKeys].elem_values = elem_values;
        numArrayKeys++;
    }

    so->numArrayKeys = numArrayKeys;

    MemoryContextSwitchTo(oldContext);
}

/*
 * _bt_find_extreme_element() -- get least or greatest array element
 *
 * scan and skey identify the index column, whose opfamily determines the
 * comparison semantics.  strat should be BTLessStrategyNumber to get the
 * least element, or BTGreaterStrategyNumber to get the greatest.
 */
static Datum _bt_find_extreme_element(IndexScanDesc scan, const ScanKey skey, StrategyNumber strat, const Datum *elems,
                                      int nelems)
{
    Relation rel = scan->indexRelation;
    Oid elemtype, cmp_op;
    RegProcedure cmp_proc;
    FmgrInfo flinfo;
    Datum result;
    int i;

    /*
     * Determine the nominal datatype of the array elements.  We have to
     * support the convention that sk_subtype == InvalidOid means the opclass
     * input type; this is a hack to simplify life for ScanKeyInit().
     */
    elemtype = skey->sk_subtype;
    if (elemtype == InvalidOid) {
        elemtype = rel->rd_opcintype[skey->sk_attno - 1];
    }

    /*
     * Look up the appropriate comparison operator in the opfamily.
     *
     * Note: it's possible that this would fail, if the opfamily is
     * incomplete, but it seems quite unlikely that an opfamily would omit
     * non-cross-type comparison operators for any datatype that it supports
     * at all.
     */
    cmp_op = get_opfamily_member(rel->rd_opfamily[skey->sk_attno - 1], elemtype, elemtype, strat);
    if (!OidIsValid(cmp_op)) {
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR), errmsg("missing operator %d(%u,%u) in opfamily %u", strat, elemtype,
                                                       elemtype, rel->rd_opfamily[skey->sk_attno - 1])));
    }
    cmp_proc = get_opcode(cmp_op);
    if (!RegProcedureIsValid(cmp_proc)) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("missing oprcode for operator %u", cmp_op)));
    }

    fmgr_info(cmp_proc, &flinfo);

    Assert(nelems > 0);
    result = elems[0];
    for (i = 1; i < nelems; i++) {
        if (DatumGetBool(FunctionCall2Coll(&flinfo, skey->sk_collation, elems[i], result))) {
            result = elems[i];
        }
    }

    return result;
}

/*
 * _bt_sort_array_elements() -- sort and de-dup array elements
 *
 * The array elements are sorted in-place, and the new number of elements
 * after duplicate removal is returned.
 *
 * scan and skey identify the index column, whose opfamily determines the
 * comparison semantics.  If reverse is true, we sort in descending order.
 */
int _bt_sort_array_elements(IndexScanDesc scan, const ScanKey skey, bool reverse, Datum *elems, int nelems)
{
    Relation rel = scan->indexRelation;
    Oid elemtype;
    RegProcedure cmp_proc;
    BTSortArrayContext cxt = {0};
    int last_non_dup;
    int i;

    if (nelems <= 1) {
        return nelems; /* no work to do */
    }

    /*
     * Determine the nominal datatype of the array elements.  We have to
     * support the convention that sk_subtype == InvalidOid means the opclass
     * input type; this is a hack to simplify life for ScanKeyInit().
     */
    elemtype = skey->sk_subtype;
    if (elemtype == InvalidOid) {
        elemtype = rel->rd_opcintype[skey->sk_attno - 1];
    }

    /*
     * Look up the appropriate comparison function in the opfamily.
     *
     * Note: it's possible that this would fail, if the opfamily is
     * incomplete, but it seems quite unlikely that an opfamily would omit
     * non-cross-type support functions for any datatype that it supports at
     * all.
     */
    Oid opclass = GetDefaultOpClass(elemtype, BTREE_AM_OID);
    if (InvalidOid == opclass) {
        ereport(ERROR,
            (errcode(ERRCODE_CASE_NOT_FOUND), errmsg("Invalid opclass for type %d.", elemtype)));
    }

    Oid opfamily = get_opclass_family(opclass);
    cmp_proc = get_opfamily_proc(opfamily, elemtype, elemtype, BTORDER_PROC);
    if (!RegProcedureIsValid(cmp_proc)) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("missing support function %d(%u,%u) in opfamily %u", BTORDER_PROC, elemtype, elemtype,
                               opfamily)));
    }

    /* Sort the array elements */
    fmgr_info(cmp_proc, &cxt.flinfo);
    cxt.collation = skey->sk_collation;
    cxt.reverse = reverse;
    qsort_arg((void *)elems, (size_t)nelems, sizeof(Datum), _bt_compare_array_elements, (void *)&cxt);

    /* Now scan the sorted elements and remove duplicates */
    last_non_dup = 0;
    for (i = 1; i < nelems; i++) {
        int32 compare;

        compare = DatumGetInt32(FunctionCall2Coll(&cxt.flinfo, cxt.collation, elems[last_non_dup], elems[i]));
        if (compare != 0) {
            elems[++last_non_dup] = elems[i];
        }
    }

    return last_non_dup + 1;
}

/*
 * qsort_arg comparator for sorting array elements
 */
static int _bt_compare_array_elements(const void *a, const void *b, void *arg)
{
    Datum da = *((const Datum *)a);
    Datum db = *((const Datum *)b);
    BTSortArrayContext *cxt = (BTSortArrayContext *)arg;
    int32 compare;

    compare = DatumGetInt32(FunctionCall2Coll(&cxt->flinfo, cxt->collation, da, db));
    if (cxt->reverse) {
        compare = -compare;
    }
    return compare;
}

/*
 * _bt_start_array_keys() -- Initialize array keys at start of a scan
 *
 * Set up the cur_elem counters and fill in the first sk_argument value for
 * each array scankey.	We can't do this until we know the scan direction.
 */
void _bt_start_array_keys(IndexScanDesc scan, ScanDirection dir)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    for (int i = 0; i < so->numArrayKeys; i++) {
        BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
        ScanKey skey = &so->arrayKeyData[curArrayKey->scan_key];

        Assert(curArrayKey->num_elems > 0);
        if (ScanDirectionIsBackward(dir)) {
            curArrayKey->cur_elem = curArrayKey->num_elems - 1;
        } else {
            curArrayKey->cur_elem = 0;
        }
        skey->sk_argument = curArrayKey->elem_values[curArrayKey->cur_elem];
    }
}

/*
 * _bt_advance_array_keys() -- Advance to next set of array elements
 *
 * Returns TRUE if there is another set of values to consider, FALSE if not.
 * On TRUE result, the scankeys are initialized with the next set of values.
 */
bool _bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    bool found = false;
    int i;

    /*
     * We must advance the last array key most quickly, since it will
     * correspond to the lowest-order index column among the available
     * qualifications. This is necessary to ensure correct ordering of output
     * when there are multiple array keys.
     */
    for (i = so->numArrayKeys - 1; i >= 0; i--) {
        BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
        ScanKey skey = &so->arrayKeyData[curArrayKey->scan_key];
        int cur_elem = curArrayKey->cur_elem;
        int num_elems = curArrayKey->num_elems;

        if (ScanDirectionIsBackward(dir)) {
            if (--cur_elem < 0) {
                cur_elem = num_elems - 1;
                found = false; /* need to advance next array key */
            } else {
                found = true;
            }
        } else {
            if (++cur_elem >= num_elems) {
                cur_elem = 0;
                found = false; /* need to advance next array key */
            } else {
                found = true;
            }
        }

        curArrayKey->cur_elem = cur_elem;
        skey->sk_argument = curArrayKey->elem_values[cur_elem];
        if (found) {
            break;
        }
    }

    /* advance parallel scan */
    if (scan->parallelScan != NULL)
        _bt_parallel_advance_array_keys(scan);

    return found;
}

/*
 * _bt_mark_array_keys() -- Handle array keys during btmarkpos
 *
 * Save the current state of the array keys as the "mark" position.
 */
void _bt_mark_array_keys(IndexScanDesc scan)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    int i;

    for (i = 0; i < so->numArrayKeys; i++) {
        BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
        curArrayKey->mark_elem = curArrayKey->cur_elem;
    }
}

/*
 * _bt_restore_array_keys() -- Handle array keys during btrestrpos
 *
 * Restore the array keys to where they were when the mark was set.
 */
void _bt_restore_array_keys(IndexScanDesc scan)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    bool changed = false;
    int i;

    /* Restore each array key to its position when the mark was set */
    for (i = 0; i < so->numArrayKeys; i++) {
        BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
        ScanKey skey = &so->arrayKeyData[curArrayKey->scan_key];
        int mark_elem = curArrayKey->mark_elem;
        if (curArrayKey->cur_elem != mark_elem) {
            curArrayKey->cur_elem = mark_elem;
            skey->sk_argument = curArrayKey->elem_values[mark_elem];
            changed = true;
        }
    }

    /*
     * If we changed any keys, we must redo _bt_preprocess_keys.  That might
     * sound like overkill, but in cases with multiple keys per index column
     * it seems necessary to do the full set of pushups.
     */
    if (changed) {
        _bt_preprocess_keys(scan);
        /* The mark should have been set on a consistent set of keys... */
        Assert(so->qual_ok);
    }
}

/* use to reduce the number of memset operations */
#define RESET_STRATEGY_KEYS(xform) do { \
    xform[0] = NULL;           \
    xform[1] = NULL;           \
    xform[2] = NULL;           \
    xform[3] = NULL;           \
    xform[4] = NULL;           \
} while (0)

/*
 *	_bt_preprocess_keys() -- Preprocess scan keys
 *
 * The given search-type keys (in scan->keyData[] or so->arrayKeyData[])
 * are copied to so->keyData[] with possible transformation.
 * scan->numberOfKeys is the number of input keys, so->numberOfKeys gets
 * the number of output keys (possibly less, never greater).
 *
 * The output keys are marked with additional sk_flag bits beyond the
 * system-standard bits supplied by the caller.  The DESC and NULLS_FIRST
 * indoption bits for the relevant index attribute are copied into the flags.
 * Also, for a DESC column, we commute (flip) all the sk_strategy numbers
 * so that the index sorts in the desired direction.
 *
 * One key purpose of this routine is to discover which scan keys must be
 * satisfied to continue the scan.	It also attempts to eliminate redundant
 * keys and detect contradictory keys.	(If the index opfamily provides
 * incomplete sets of cross-type operators, we may fail to detect redundant
 * or contradictory keys, but we can survive that.)
 *
 * The output keys must be sorted by index attribute.  Presently we expect
 * (but verify) that the input keys are already so sorted --- this is done
 * by match_clauses_to_index() in indxpath.c.  Some reordering of the keys
 * within each attribute may be done as a byproduct of the processing here,
 * but no other code depends on that.
 *
 * The output keys are marked with flags SK_BT_REQFWD and/or SK_BT_REQBKWD
 * if they must be satisfied in order to continue the scan forward or backward
 * respectively.  _bt_checkkeys uses these flags.  For example, if the quals
 * are "x = 1 AND y < 4 AND z < 5", then _bt_checkkeys will reject a tuple
 * (1,2,7), but we must continue the scan in case there are tuples (1,3,z).
 * But once we reach tuples like (1,4,z) we can stop scanning because no
 * later tuples could match.  This is reflected by marking the x and y keys,
 * but not the z key, with SK_BT_REQFWD.  In general, the keys for leading
 * attributes with "=" keys are marked both SK_BT_REQFWD and SK_BT_REQBKWD.
 * For the first attribute without an "=" key, any "<" and "<=" keys are
 * marked SK_BT_REQFWD while any ">" and ">=" keys are marked SK_BT_REQBKWD.
 * This can be seen to be correct by considering the above example.  Note
 * in particular that if there are no keys for a given attribute, the keys for
 * subsequent attributes can never be required; for instance "WHERE y = 4"
 * requires a full-index scan.
 *
 * If possible, redundant keys are eliminated: we keep only the tightest
 * >/>= bound and the tightest </<= bound, and if there's an = key then
 * that's the only one returned.  (So, we return either a single = key,
 * or one or two boundary-condition keys for each attr.)  However, if we
 * cannot compare two keys for lack of a suitable cross-type operator,
 * we cannot eliminate either.	If there are two such keys of the same
 * operator strategy, the second one is just pushed into the output array
 * without further processing here.  We may also emit both >/>= or both
 * </<= keys if we can't compare them.  The logic about required keys still
 * works if we don't eliminate redundant keys.
 *
 * Note that one reason we need direction-sensitive required-key flags is
 * precisely that we may not be able to eliminate redundant keys.  Suppose
 * we have "x > 4::int AND x > 10::bigint", and we are unable to determine
 * which key is more restrictive for lack of a suitable cross-type operator.
 * _bt_first will arbitrarily pick one of the keys to do the initial
 * positioning with.  If it picks x > 4, then the x > 10 condition will fail
 * until we reach index entries > 10; but we can't stop the scan just because
 * x > 10 is failing.  On the other hand, if we are scanning backwards, then
 * failure of either key is indeed enough to stop the scan.  (In general, when
 * inequality keys are present, the initial-positioning code only promises to
 * position before the first possible match, not exactly at the first match,
 * for a forward scan; or after the last match for a backward scan.)
 *
 * As a byproduct of this work, we can detect contradictory quals such
 * as "x = 1 AND x > 2".  If we see that, we return so->qual_ok = FALSE,
 * indicating the scan need not be run at all since no tuples can match.
 * (In this case we do not bother completing the output key array!)
 * Again, missing cross-type operators might cause us to fail to prove the
 * quals contradictory when they really are, but the scan will work correctly.
 *
 * Row comparison keys are currently also treated without any smarts:
 * we just transfer them into the preprocessed array without any
 * editorialization.  We can treat them the same as an ordinary inequality
 * comparison on the row's first index column, for the purposes of the logic
 * about required keys.
 *
 * Note: the reason we have to copy the preprocessed scan keys into private
 * storage is that we are modifying the array based on comparisons of the
 * key argument values, which could change on a rescan or after moving to
 * new elements of array keys.	Therefore we can't overwrite the source data.
 */
void _bt_preprocess_keys(IndexScanDesc scan)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    int numberOfKeys = scan->numberOfKeys;
    int16 *indoption = scan->indexRelation->rd_indoption;
    int new_numberOfKeys;
    int numberOfEqualCols;
    ScanKey inkeys;
    ScanKey outkeys;
    ScanKey cur;
    bool test_result = false;
    int i, j;
    AttrNumber attno;

    /* initialize result variables */
    so->qual_ok = true;
    so->numberOfKeys = 0;

    if (numberOfKeys < 1) {
        return; /* done if qual-less scan */
    }

    /* Read so->arrayKeyData if array keys are present, else scan->keyData */
    inkeys = (so->arrayKeyData != NULL) ? so->arrayKeyData : scan->keyData;

    outkeys = so->keyData;
    cur = &inkeys[0];
    /* we check that input keys are correctly ordered */
    if (SECUREC_UNLIKELY(cur->sk_attno < 1)){
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("btree index keys must be ordered by attribute")));
    }

    /* We can short-circuit most of the work if there's just one key */
    if (numberOfKeys == 1) {
        /* Apply indoption to scankey (might change sk_strategy!) */
        if (!_bt_fix_scankey_strategy(cur, indoption)) {
            so->qual_ok = false;
        }
         *outkeys = *cur;
        so->numberOfKeys = 1;
        /* We can mark the qual as required if it's for first index col */
        if (cur->sk_attno == 1) {
            _bt_mark_scankey_required(outkeys);
        }
        return;
    }

    /* Otherwise, do the full set of pushups. */
    ScanKey xform[BTMaxStrategyNumber];
    RESET_STRATEGY_KEYS(xform);
    new_numberOfKeys = 0;
    numberOfEqualCols = 0;

    /*
     * Initialize for processing of keys for attr 1.
     *
     * xform[i] points to the currently best scan key of strategy type i+1; it
     * is NULL if we haven't yet found such a key for this attr.
     */
    attno = 1;

    /*
     * Loop iterates from 0 to numberOfKeys inclusive; we use the last pass to
     * handle after-last-key processing.  Actual exit from the loop is at the
     * "break" statement below.
     */
    for (i = 0;; cur++, i++) {
        if (i < numberOfKeys) {
            /* Apply indoption to scankey (might change sk_strategy!) */
            if (!_bt_fix_scankey_strategy(cur, indoption)) {
                /* NULL can't be matched, so give up */
                so->qual_ok = false;
                return;
            }
        }

        /*
         * If we are at the end of the keys for a particular attr, finish up
         * processing and emit the cleaned-up keys.
         */
        if (i == numberOfKeys || cur->sk_attno != attno) {
            int priorNumberOfEqualCols = numberOfEqualCols;

            /* check input keys are correctly ordered */
            if (SECUREC_UNLIKELY(i < numberOfKeys && cur->sk_attno < attno)) {
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR), errmsg("btree index keys must be ordered by attribute")));
            }

            /*
             * If = has been specified, all other keys can be eliminated as
             * redundant.  If we have a case like key = 1 AND key > 2, we can
             * set qual_ok to false and abandon further processing.
             *
             * We also have to deal with the case of "key IS NULL", which is
             * unsatisfiable in combination with any other index condition. By
             * the time we get here, that's been classified as an equality
             * check, and we've rejected any combination of it with a regular
             * equality condition; but not with other types of conditions.
             */
            if (xform[BTEqualStrategyNumber - 1]) {
                ScanKey eq = xform[BTEqualStrategyNumber - 1];

                for (j = BTMaxStrategyNumber; --j >= 0;) {
                    ScanKey chk = xform[j];

                    if (!chk || j == (BTEqualStrategyNumber - 1)) {
                        continue;
                    }

                    if (eq->sk_flags & SK_SEARCHNULL) {
                        /* IS NULL is contradictory to anything else */
                        so->qual_ok = false;
                        return;
                    }

                    if (_bt_compare_scankey_args(scan, chk, eq, chk, &test_result)) {
                        if (!test_result) {
                            /* keys proven mutually contradictory */
                            so->qual_ok = false;
                            return;
                        }
                        /* else discard the redundant non-equality key */
                        xform[j] = NULL;
                    }
                    /* else, cannot determine redundancy, keep both keys */
                }
                /* track number of attrs for which we have "=" keys */
                numberOfEqualCols++;
            }

            /* try to keep only one of <, <= */
            if (xform[BTLessStrategyNumber - 1] && xform[BTLessEqualStrategyNumber - 1]) {
                ScanKey lt = xform[BTLessStrategyNumber - 1];
                ScanKey le = xform[BTLessEqualStrategyNumber - 1];

                if (_bt_compare_scankey_args(scan, le, lt, le, &test_result)) {
                    if (test_result) {
                        xform[BTLessEqualStrategyNumber - 1] = NULL;
                    } else {
                        xform[BTLessStrategyNumber - 1] = NULL;
                    }
                }
            }

            /* try to keep only one of >, >= */
            if (xform[BTGreaterStrategyNumber - 1] && xform[BTGreaterEqualStrategyNumber - 1]) {
                ScanKey gt = xform[BTGreaterStrategyNumber - 1];
                ScanKey ge = xform[BTGreaterEqualStrategyNumber - 1];

                if (_bt_compare_scankey_args(scan, ge, gt, ge, &test_result)) {
                    if (test_result) {
                        xform[BTGreaterEqualStrategyNumber - 1] = NULL;
                    } else {
                        xform[BTGreaterStrategyNumber - 1] = NULL;
                    }
                }
            }

            /*
             * Emit the cleaned-up keys into the outkeys[] array, and then
             * mark them if they are required.	They are required (possibly
             * only in one direction) if all attrs before this one had "=".
             */
            for (j = BTMaxStrategyNumber; --j >= 0;) {
                if (xform[j]) {
                    ScanKey outkey = &outkeys[new_numberOfKeys++];
                    *outkey = *xform[j];
                    if (priorNumberOfEqualCols == attno - 1) {
                        _bt_mark_scankey_required(outkey);
                    }
                }
            }

            /* Exit loop here if done. */
            if (i == numberOfKeys) {
                break;
            }

            /* Re-initialize for new attno */
            attno = cur->sk_attno;
            RESET_STRATEGY_KEYS(xform);
        }

        /* check strategy this key's operator corresponds to */
        j = cur->sk_strategy - 1;

        /* if row comparison, push it directly to the output array */
        if (cur->sk_flags & SK_ROW_HEADER) {
            ScanKey outkey = &outkeys[new_numberOfKeys++];
            *outkey = *cur;
            if (numberOfEqualCols == attno - 1) {
                _bt_mark_scankey_required(outkey);
            }

            /* We don't support RowCompare using equality; such a qual would
             * mess up the numberOfEqualCols tracking. */
            Assert(j != (BTEqualStrategyNumber - 1));
            continue;
        }

        /* have we seen one of these before? */
        if (xform[j] == NULL) {
            /* nope, so remember this scankey */
            xform[j] = cur;
        } else {
            /* yup, keep only the more restrictive key */
            if (_bt_compare_scankey_args(scan, cur, cur, xform[j], &test_result)) {
                if (test_result) {
                    xform[j] = cur;
                } else if (j == (BTEqualStrategyNumber - 1)) {
                    /* key == a && key == b, but a != b */
                    so->qual_ok = false;
                    return;
                }
                /* else old key is more restrictive, keep it */
            } else {
                /*
                 * We can't determine which key is more restrictive.  Keep the
                 * previous one in xform[j] and push this one directly to the
                 * output array.
                 */
                ScanKey outkey = &outkeys[new_numberOfKeys++];
                *outkey = *cur;
                if (numberOfEqualCols == attno - 1) {
                    _bt_mark_scankey_required(outkey);
                }
            }
        }
    }

    so->numberOfKeys = new_numberOfKeys;
}

/*
 * Compare two scankey values using a specified operator.
 *
 * The test we want to perform is logically "leftarg op rightarg", where
 * leftarg and rightarg are the sk_argument values in those ScanKeys, and
 * the comparison operator is the one in the op ScanKey.  However, in
 * cross-data-type situations we may need to look up the correct operator in
 * the index's opfamily: it is the one having amopstrategy = op->sk_strategy
 * and amoplefttype/amoprighttype equal to the two argument datatypes.
 *
 * If the opfamily doesn't supply a complete set of cross-type operators we
 * may not be able to make the comparison.	If we can make the comparison
 * we store the operator result in *result and return TRUE.  We return FALSE
 * if the comparison could not be made.
 *
 * Note: op always points at the same ScanKey as either leftarg or rightarg.
 * Since we don't scribble on the scankeys, this aliasing should cause no
 * trouble.
 *
 * Note: this routine needs to be insensitive to any DESC option applied
 * to the index column.  For example, "x < 4" is a tighter constraint than
 * "x < 5" regardless of which way the index is sorted.
 */
static bool _bt_compare_scankey_args(IndexScanDesc scan, ScanKey op, const ScanKey leftarg, const ScanKey rightarg,
                                     bool *result)
{
    Relation rel = scan->indexRelation;
    Oid lefttype, righttype, optype, opcintype, cmp_op;
    StrategyNumber strat;

    /*
     * First, deal with cases where one or both args are NULL.	This should
     * only happen when the scankeys represent IS NULL/NOT NULL conditions.
     */
    if ((leftarg->sk_flags | rightarg->sk_flags) & SK_ISNULL) {
        int leftnull;
        int rightnull;

        if (leftarg->sk_flags & SK_ISNULL) {
            Assert(leftarg->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
            leftnull = 1;
        } else {
            leftnull = 0;
        }
        if (rightarg->sk_flags & SK_ISNULL) {
            Assert(rightarg->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
            rightnull = 1;
        } else {
            rightnull = 0;
        }

        /*
         * We treat NULL as either greater than or less than all other values.
         * Since true > false, the tests below work correctly for NULLS LAST
         * logic.  If the index is NULLS FIRST, we need to flip the strategy.
         */
        strat = op->sk_strategy;
        if (op->sk_flags & SK_BT_NULLS_FIRST) {
            strat = (uint16)BTCommuteStrategyNumber(strat);
        }

        switch (strat) {
            case BTLessStrategyNumber:
                *result = (leftnull < rightnull);
                break;
            case BTLessEqualStrategyNumber:
                *result = (leftnull <= rightnull);
                break;
            case BTEqualStrategyNumber:
                *result = (leftnull == rightnull);
                break;
            case BTGreaterEqualStrategyNumber:
                *result = (leftnull >= rightnull);
                break;
            case BTGreaterStrategyNumber:
                *result = (leftnull > rightnull);
                break;
            default: {
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("unrecognized StrategyNumber: %d", (int)strat)));
                *result = false; /* keep compiler quiet */
                break;
            }
        }
        return true;
    }

    /* The opfamily we need to worry about is identified by the index column. */
    Assert(leftarg->sk_attno == rightarg->sk_attno);
    opcintype = rel->rd_opcintype[leftarg->sk_attno - 1];

    /*
     * Determine the actual datatypes of the ScanKey arguments.  We have to
     * support the convention that sk_subtype == InvalidOid means the opclass
     * input type; this is a hack to simplify life for ScanKeyInit().
     */
    lefttype = leftarg->sk_subtype;
    if (lefttype == InvalidOid) {
        lefttype = opcintype;
    }
    righttype = rightarg->sk_subtype;
    if (righttype == InvalidOid) {
        righttype = opcintype;
    }
    optype = op->sk_subtype;
    if (optype == InvalidOid) {
        optype = opcintype;
    }

    /*
     * If leftarg and rightarg match the types expected for the "op" scankey,
     * we can use its already-looked-up comparison function.
     */
    if (lefttype == opcintype && righttype == optype) {
        *result = DatumGetBool(
            FunctionCall2Coll(&op->sk_func, op->sk_collation, leftarg->sk_argument, rightarg->sk_argument));
        return true;
    }

    /*
     * Otherwise, we need to go to the syscache to find the appropriate
     * operator.  (This cannot result in infinite recursion, since no
     * indexscan initiated by syscache lookup will use cross-data-type
     * operators.)
     *
     * If the sk_strategy was flipped by _bt_fix_scankey_strategy, we have to
     * un-flip it to get the correct opfamily member.
     */
    strat = op->sk_strategy;
    if (op->sk_flags & SK_BT_DESC) {
        strat = (uint16)BTCommuteStrategyNumber(strat);
    }

    cmp_op = get_opfamily_member(rel->rd_opfamily[leftarg->sk_attno - 1], lefttype, righttype, strat);
    if (OidIsValid(cmp_op)) {
        RegProcedure cmp_proc = get_opcode(cmp_op);
        if (RegProcedureIsValid(cmp_proc)) {
            *result = DatumGetBool(
                OidFunctionCall2Coll(cmp_proc, op->sk_collation, leftarg->sk_argument, rightarg->sk_argument));
            return true;
        }
    }

    /* Can't make the comparison */
    *result = false; /* suppress compiler warnings */
    return false;
}

/*
 * Adjust a scankey's strategy and flags setting as needed for indoptions.
 *
 * We copy the appropriate indoption value into the scankey sk_flags
 * (shifting to avoid clobbering system-defined flag bits).  Also, if
 * the DESC option is set, commute (flip) the operator strategy number.
 *
 * A secondary purpose is to check for IS NULL/NOT NULL scankeys and set up
 * the strategy field correctly for them.
 *
 * Lastly, for ordinary scankeys (not IS NULL/NOT NULL), we check for a
 * NULL comparison value.  Since all btree operators are assumed strict,
 * a NULL means that the qual cannot be satisfied.	We return TRUE if the
 * comparison value isn't NULL, or FALSE if the scan should be abandoned.
 *
 * This function is applied to the *input* scankey structure; therefore
 * on a rescan we will be looking at already-processed scankeys.  Hence
 * we have to be careful not to re-commute the strategy if we already did it.
 * It's a bit ugly to modify the caller's copy of the scankey but in practice
 * there shouldn't be any problem, since the index's indoptions are certainly
 * not going to change while the scankey survives.
 */
static bool _bt_fix_scankey_strategy(ScanKey skey, const int16 *indoption)
{
    int addflags;
    addflags = (uint16)(indoption[skey->sk_attno - 1]) << SK_BT_INDOPTION_SHIFT;

    /*
     * We treat all btree operators as strict (even if they're not so marked
     * in pg_proc). This means that it is impossible for an operator condition
     * with a NULL comparison constant to succeed, and we can reject it right
     * away.
     *
     * However, we now also support "x IS NULL" clauses as search conditions,
     * so in that case keep going. The planner has not filled in any
     * particular strategy in this case, so set it to BTEqualStrategyNumber
     * --- we can treat IS NULL as an equality operator for purposes of search
     * strategy.
     *
     * Likewise, "x IS NOT NULL" is supported.	We treat that as either "less
     * than NULL" in a NULLS LAST index, or "greater than NULL" in a NULLS
     * FIRST index.
     *
     * Note: someday we might have to fill in sk_collation from the index
     * column's collation.  At the moment this is a non-issue because we'll
     * never actually call the comparison operator on a NULL.
     */
    if (skey->sk_flags & SK_ISNULL) {
        /* SK_ISNULL shouldn't be set in a row header scankey */
        Assert(!(skey->sk_flags & SK_ROW_HEADER));

        /* Set indoption flags in scankey (might be done already) */
        skey->sk_flags |= addflags;

        /* Set correct strategy for IS NULL or NOT NULL search */
        if (skey->sk_flags & SK_SEARCHNULL) {
            skey->sk_strategy = BTEqualStrategyNumber;
            skey->sk_subtype = InvalidOid;
            skey->sk_collation = InvalidOid;
        } else if (skey->sk_flags & SK_SEARCHNOTNULL) {
            skey->sk_strategy = (skey->sk_flags & SK_BT_NULLS_FIRST) ? BTGreaterStrategyNumber : BTLessStrategyNumber;
            skey->sk_subtype = InvalidOid;
            skey->sk_collation = InvalidOid;
        } else {
            /* regular qual, so it cannot be satisfied */
            return false;
        }

        /* Needn't do the rest */
        return true;
    }

    /* Adjust strategy for DESC, if we didn't already */
    if ((addflags & SK_BT_DESC) && !(skey->sk_flags & SK_BT_DESC)) {
        skey->sk_strategy = (uint16)BTCommuteStrategyNumber(skey->sk_strategy);
    }
    skey->sk_flags |= addflags;

    /* If it's a row header, fix row member flags and strategies similarly */
    if (skey->sk_flags & SK_ROW_HEADER) {
        ScanKey subkey = (ScanKey)DatumGetPointer(skey->sk_argument);
        for (;;) {
            Assert(subkey->sk_flags & SK_ROW_MEMBER);
            addflags = (uint16)(indoption[subkey->sk_attno - 1]) << SK_BT_INDOPTION_SHIFT;
            if ((addflags & SK_BT_DESC) && !(subkey->sk_flags & SK_BT_DESC)) {
                subkey->sk_strategy = (uint16)BTCommuteStrategyNumber(subkey->sk_strategy);
            }
            subkey->sk_flags |= addflags;
            if (subkey->sk_flags & SK_ROW_END) {
                break;
            }
            subkey++;
        }
    }

    return true;
}

/*
 * Mark a scankey as "required to continue the scan".
 *
 * Depending on the operator type, the key may be required for both scan
 * directions or just one.	Also, if the key is a row comparison header,
 *
 * we have to mark its first subsidiary ScanKey as required.  (Subsequent
 * subsidiary ScanKeys are normally for lower-order columns, and thus
 * cannot be required, since they're after the first non-equality scankey.)
 *
 * Note: when we set required-key flag bits in a subsidiary scankey, we are
 * scribbling on a data structure belonging to the index AM's caller, not on
 * our private copy.  This should be OK because the marking will not change
 * from scan to scan within a query, and so we'd just re-mark the same way
 * anyway on a rescan.	Something to keep an eye on though.
 */
static void _bt_mark_scankey_required(ScanKey skey)
{
    int addflags;

    switch (skey->sk_strategy) {
        case BTLessStrategyNumber:
        case BTLessEqualStrategyNumber:
            addflags = SK_BT_REQFWD;
            break;
        case BTEqualStrategyNumber:
            addflags = SK_BT_REQFWD | SK_BT_REQBKWD;
            break;
        case BTGreaterEqualStrategyNumber:
        case BTGreaterStrategyNumber:
            addflags = SK_BT_REQBKWD;
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("unrecognized StrategyNumber: %d", (int)skey->sk_strategy)));
            addflags = 0; /* keep compiler quiet */
            break;
    }

    skey->sk_flags |= addflags;
    if (skey->sk_flags & SK_ROW_HEADER) {
        ScanKey subkey = (ScanKey)DatumGetPointer(skey->sk_argument);

        /* First subkey should be same column/operator as the header */
        Assert(subkey->sk_flags & SK_ROW_MEMBER);
        Assert(subkey->sk_attno == skey->sk_attno);
        Assert(subkey->sk_strategy == skey->sk_strategy);
        subkey->sk_flags |= addflags;
    }
}

/*
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * If so, return the address of the index tuple on the index page.
 * If not, return NULL.
 *
 * If the tuple fails to pass the qual, we also determine whether there's
 * any need to continue the scan beyond this tuple, and set *continuescan
 * accordingly.  See comments for _bt_preprocess_keys(), above, about how
 * this is done.
 *
 * scan: index scan descriptor (containing a search-type scankey)
 * page: buffer page containing index tuple
 * offnum: offset number of index tuple (must be a valid item!)
 * dir: direction we are scanning in
 * continuescan: output parameter (will be set correctly in all cases)
 *
 * Caller must hold pin and lock on the index page.
 */
IndexTuple _bt_checkkeys(IndexScanDesc scan, Page page, OffsetNumber offnum, ScanDirection dir, bool *continuescan)
{
    ItemId iid = PageGetItemId(page, offnum);
    bool tuple_alive = false;
    IndexTuple tuple;
    TupleDesc tupdesc;
    BTScanOpaque so;
    int keysz;
    int ikey;
    ScanKey key;

    *continuescan = true; /* default assumption */

    /*
     * If the scan specifies not to return killed tuples, then we treat a
     * killed tuple as not passing the qual.  Most of the time, it's a win to
     * not bother examining the tuple's index keys, but just return
     * immediately with continuescan = true to proceed to the next tuple.
     * However, if this is the last tuple on the page, we should check the
     * index keys to prevent uselessly advancing to the next page.
     */
    if (scan->ignore_killed_tuples && ItemIdIsDead(iid)) {
        /* return immediately if there are more tuples on the page */
        if (ScanDirectionIsForward(dir)) {
            if (offnum < PageGetMaxOffsetNumber(page)) {
                return NULL;
            }
        } else {
            BTPageOpaqueInternal opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
            if (offnum > P_FIRSTDATAKEY(opaque)) {
                return NULL;
            }
        }

        /*
         * OK, we want to check the keys so we can set continuescan correctly,
         * but we'll return NULL even if the tuple passes the key tests.
         */
        tuple_alive = false;
    } else {
        tuple_alive = true;
    }

    tuple = (IndexTuple)PageGetItem(page, iid);
    tupdesc = RelationGetDescr(scan->indexRelation);
    so = (BTScanOpaque)scan->opaque;
    keysz = so->numberOfKeys;

    for (key = so->keyData, ikey = 0; ikey < keysz; key++, ikey++) {
        Datum datum;
        bool isNull = false;
        bool test;

        /* row-comparison keys need special processing */
        if (key->sk_flags & SK_ROW_HEADER) {
            if (_bt_check_rowcompare(key, tuple, tupdesc, dir, continuescan)) {
                continue;
            }
            return NULL;
        }

        datum = index_getattr(tuple, key->sk_attno, tupdesc, &isNull);

        if (key->sk_flags & SK_ISNULL) {
            /* Handle IS NULL/NOT NULL tests */
            if (key->sk_flags & SK_SEARCHNULL) {
                if (isNull) {
                    continue; /* tuple satisfies this qual */
                }
            } else {
                Assert(key->sk_flags & SK_SEARCHNOTNULL);
                if (!isNull) {
                    continue; /* tuple satisfies this qual */
                }
            }

            /*
             * Tuple fails this qual.  If it's a required qual for the current
             * scan direction, then we can conclude no further tuples will
             * pass, either.
             */
            if ((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) {
                *continuescan = false;
            } else if ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)) {
                *continuescan = false;
            }

            /*
             * In any case, this indextuple doesn't match the qual.
             */
            return NULL;
        }

        if (isNull) {
            if (key->sk_flags & SK_BT_NULLS_FIRST) {
                /*
                 * Since NULLs are sorted before non-NULLs, we know we have
                 * reached the lower limit of the range of values for this
                 * index attr.	On a backward scan, we can stop if this qual
                 * is one of the "must match" subset.  We can stop regardless
                 * of whether the qual is > or <, so long as it's required,
                 * because it's not possible for any future tuples to pass. On
                 * a forward scan, however, we must keep going, because we may
                 * have initially positioned to the start of the index.
                 */
                if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) && ScanDirectionIsBackward(dir)) {
                    *continuescan = false;
                }
            } else {
                /*
                 * Since NULLs are sorted after non-NULLs, we know we have
                 * reached the upper limit of the range of values for this
                 * index attr.	On a forward scan, we can stop if this qual is
                 * one of the "must match" subset.	We can stop regardless of
                 * whether the qual is > or <, so long as it's required,
                 * because it's not possible for any future tuples to pass. On
                 * a backward scan, however, we must keep going, because we
                 * may have initially positioned to the end of the index.
                 */
                if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) && ScanDirectionIsForward(dir)) {
                    *continuescan = false;
                }
            }

            /*
             * In any case, this indextuple doesn't match the qual.
             */
            return NULL;
        }
        
        switch (key->sk_func.fn_oid) {
            case F_INT8EQ:
                test = (int64)datum == (int64)key->sk_argument;
                break;
            case F_INT48EQ:
                test = (int64)(int32)datum == (int64)key->sk_argument;
                break;
            case F_INT84EQ:
                test = (int64)datum == (int64)(int32)key->sk_argument;
                break;
            case F_INT84LE:
                test = (int64)datum <= (int64)(int32)key->sk_argument;
                break;
            case F_INT84GE:
                test = (int64)datum >= (int64)(int32)key->sk_argument;
                break;
            case F_INT4EQ:
                test = (int32)datum == (int32)key->sk_argument;
                break;
            case F_INT4GT:
                test = (int32)datum > (int32)key->sk_argument;
                break;
            case F_INT4LT:
                test = (int32)datum < (int32)key->sk_argument;
                break;
            case F_INT4LE:
                test = (int32)datum <= (int32)key->sk_argument;
                break;
            case F_INT4GE:
                test = (int32)datum >= (int32)key->sk_argument;
                break;
            case F_DATE_LE:
                test = (int32)datum <= (int32)key->sk_argument;
                break;
            case F_DATE_GE:
                test = (int32)datum >= (int32)key->sk_argument;
                break;
            case F_DATE_EQ:
                test = (int32)datum == (int32)key->sk_argument;
                break;
            
            default:
                test = DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, datum, key->sk_argument));
        }
        
        if (!test) {
            /*
             * Tuple fails this qual.  If it's a required qual for the current
             * scan direction, then we can conclude no further tuples will
             * pass, either.
             *
             * Note: because we stop the scan as soon as any required equality
             * qual fails, it is critical that equality quals be used for the
             * initial positioning in _bt_first() when they are available. See
             * comments in _bt_first().
             */
            if ((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) {
                *continuescan = false;
            } else if ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)) {
                *continuescan = false;
            }

            /*
             * In any case, this indextuple doesn't match the qual.
             */
            return NULL;
        }
    }

    /* Check for failure due to it being a killed tuple. */
    if (!tuple_alive) {
        return NULL;
    }

    /* If we get here, the tuple passes all index quals. */
    return tuple;
}

/*
 * Test whether an indextuple satisfies a row-comparison scan condition.
 *
 * Return true if so, false if not.  If not, also clear *continuescan if
 * it's not possible for any future tuples in the current scan direction
 * to pass the qual.
 *
 * This is a subroutine for _bt_checkkeys, which see for more info.
 */
bool _bt_check_rowcompare(ScanKey skey, IndexTuple tuple, TupleDesc tupdesc, ScanDirection dir,
                                 bool *continuescan)
{
    ScanKey subkey = (ScanKey)DatumGetPointer(skey->sk_argument);
    int32 cmpresult = 0;
    bool result = false;

    /* First subkey should be same as the header says */
    Assert(subkey->sk_attno == skey->sk_attno);

    /* Loop over columns of the row condition */
    for (;;) {
        Datum datum;
        bool isNull = false;

        Assert(subkey->sk_flags & SK_ROW_MEMBER);

        datum = index_getattr(tuple, subkey->sk_attno, tupdesc, &isNull);
        if (isNull) {
            if (subkey->sk_flags & SK_BT_NULLS_FIRST) {
                /*
                 * Since NULLs are sorted before non-NULLs, we know we have
                 * reached the lower limit of the range of values for this
                 * index attr.	On a backward scan, we can stop if this qual
                 * is one of the "must match" subset.  We can stop regardless
                 * of whether the qual is > or <, so long as it's required,
                 * because it's not possible for any future tuples to pass. On
                 * a forward scan, however, we must keep going, because we may
                 * have initially positioned to the start of the index.
                 */
                if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) && ScanDirectionIsBackward(dir)) {
                    *continuescan = false;
                }
            } else {
                /*
                 * Since NULLs are sorted after non-NULLs, we know we have
                 * reached the upper limit of the range of values for this
                 * index attr.	On a forward scan, we can stop if this qual is
                 * one of the "must match" subset.	We can stop regardless of
                 * whether the qual is > or <, so long as it's required,
                 * because it's not possible for any future tuples to pass. On
                 * a backward scan, however, we must keep going, because we
                 * may have initially positioned to the end of the index.
                 */
                if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) && ScanDirectionIsForward(dir)) {
                    *continuescan = false;
                }
            }

            /*
             * In any case, this indextuple doesn't match the qual.
             */
            return false;
        }

        if (subkey->sk_flags & SK_ISNULL) {
            /*
             * Unlike the simple-scankey case, this isn't a disallowed case.
             * But it can never match.	If all the earlier row comparison
             * columns are required for the scan direction, we can stop the
             * scan, because there can't be another tuple that will succeed.
             */
            if (subkey != (ScanKey)DatumGetPointer(skey->sk_argument)) {
                subkey--;
            }
            if ((subkey->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) {
                *continuescan = false;
            } else if ((subkey->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)) {
                *continuescan = false;
            }
            return false;
        }

        /* Perform the test --- three-way comparison not bool operator */
        cmpresult =
            DatumGetInt32(FunctionCall2Coll(&subkey->sk_func, subkey->sk_collation, datum, subkey->sk_argument));

        if (subkey->sk_flags & SK_BT_DESC) {
            cmpresult = -cmpresult;
        }

        /* Done comparing if unequal, else advance to next column */
        if (cmpresult != 0) {
            break;
        }

        if (subkey->sk_flags & SK_ROW_END) {
            break;
        }
        subkey++;
    }

    /*
     * At this point cmpresult indicates the overall result of the row
     * comparison, and subkey points to the deciding column (or the last
     * column if the result is "=").
     */
    switch (subkey->sk_strategy) {
        /* EQ and NE cases aren't allowed here */
        case BTLessStrategyNumber:
            result = (cmpresult < 0);
            break;
        case BTLessEqualStrategyNumber:
            result = (cmpresult <= 0);
            break;
        case BTGreaterEqualStrategyNumber:
            result = (cmpresult >= 0);
            break;
        case BTGreaterStrategyNumber:
            result = (cmpresult > 0);
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("unrecognized RowCompareType: %d", (int)subkey->sk_strategy)));
            result = false; /* keep compiler quiet */
            break;
    }

    if (!result) {
        /*
         * Tuple fails this qual.  If it's a required qual for the current
         * scan direction, then we can conclude no further tuples will pass,
         * either.	Note we have to look at the deciding column, not
         * necessarily the first or last column of the row condition.
         */
        if ((subkey->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir)) {
            *continuescan = false;
        } else if ((subkey->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir)) {
            *continuescan = false;
        }
    }

    return result;
}

/*
 * _bt_killitems - set LP_DEAD state for items an indexscan caller has
 * told us were killed
 *
 * scan->so contains information about the current page and killed tuples
 * thereon (generally, this should only be called if so->numKilled > 0).
 *
 * The caller must have pin on so->currPos.buf, but may or may not have
 * read-lock, as indicated by haveLock.  Note that we assume read-lock
 * is sufficient for setting LP_DEAD status (which is only a hint).
 *
 * We match items by heap TID before assuming they are the right ones to
 * delete.	We cope with cases where items have moved right due to insertions.
 * If an item has moved off the current page due to a split, we'll fail to
 * find it and do nothing (this is not an error case --- we assume the item
 * will eventually get marked in a future indexscan).  Note that because we
 * hold pin on the target page continuously from initially reading the items
 * until applying this function, VACUUM cannot have deleted any items from
 * the page, and so there is no need to search left from the recorded offset.
 * (This observation also guarantees that the item is still the right one
 * to delete, which might otherwise be questionable since heap TIDs can get
 * recycled.)
 */
void _bt_killitems(IndexScanDesc scan, bool haveLock)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    Page page;
    BTPageOpaqueInternal opaque;
    OffsetNumber minoff;
    OffsetNumber maxoff;
    int i;
    bool killedsomething = false;
    Oid heapOid = IndexScanGetPartHeapOid(scan);

    Assert(BufferIsValid(so->currPos.buf));

    if (!haveLock) {
        LockBuffer(so->currPos.buf, BT_READ);
    }

    page = BufferGetPage(so->currPos.buf);
    opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
    minoff = P_FIRSTDATAKEY(opaque);
    maxoff = PageGetMaxOffsetNumber(page);

    for (i = 0; i < so->numKilled; i++) {
        int itemIndex = so->killedItems[i];
        BTScanPosItem *kitem = &so->currPos.items[itemIndex];
        OffsetNumber offnum = kitem->indexOffset;
        Oid partOid = kitem->partitionOid;
        int2 bucketid = kitem->bucketid;

        Assert(itemIndex >= so->currPos.firstItem && itemIndex <= so->currPos.lastItem);
        if (offnum < minoff) {
            continue; /* pure paranoia */
        }
        while (offnum <= maxoff) {
            ItemId iid = PageGetItemId(page, offnum);
            IndexTuple ituple = (IndexTuple)PageGetItem(page, iid);

            bool killtuple = false;

            if (btree_tuple_is_posting(ituple)) {
                int posting_idx = i + 1;
                int nposting = btree_tuple_get_nposting(ituple);
                int j;

                for (j = 0; j < nposting; j++) {
                    ItemPointer item = btree_tuple_get_posting_n(ituple, j);

                    if (!ItemPointerEquals(item, &kitem->heapTid))
                        break;

                    if (posting_idx < so->numKilled)
                        kitem = &so->currPos.items[so->killedItems[posting_idx++]];
                }

                if (j == nposting)
                    killtuple = true;
            } else if (ItemPointerEquals(&ituple->t_tid, &kitem->heapTid)) {
                Oid currPartOid = scan->xs_want_ext_oid ? index_getattr_tableoid(scan->indexRelation, ituple) : heapOid;
                int2 currbktid =
                    scan->xs_want_bucketid ? index_getattr_bucketid(scan->indexRelation, ituple) : bucketid;
                if (currPartOid == partOid && currbktid == bucketid) {
                    killtuple = true;
                }
            }

            if (killtuple && !ItemIdIsDead(iid)) {
                /* found the item */
                ItemIdMarkDead(iid);
                killedsomething = true;
                break; /* out of inner search loop */
            }
            offnum = OffsetNumberNext(offnum);
        }
    }

    /*
     * Since this can be redone later if needed, it's treated the same as a
     * commit-hint-bit status update for heap tuples: we mark the buffer dirty
     * but don't make a WAL log entry.
     *
     * Whenever we mark anything LP_DEAD, we also set the page's
     * BTP_HAS_GARBAGE flag, which is likewise just a hint.
     */
    if (killedsomething) {
        opaque->btpo_flags |= BTP_HAS_GARBAGE;
        MarkBufferDirtyHint(so->currPos.buf, true);
    }

    if (!haveLock) {
        LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
    }

    /*
     * Always reset the scan state, so we don't look for same items on other
     * pages.
     */
    so->numKilled = 0;
}

/*
 * The following routines manage a shared-memory area in which we track
 * assignment of "vacuum cycle IDs" to currently-active btree vacuuming
 * operations.	There is a single counter which increments each time we
 * start a vacuum to assign it a cycle ID.	Since multiple vacuums could
 * be active concurrently, we have to track the cycle ID for each active
 * vacuum; this requires at most g_instance.shmem_cxt.MaxBackends entries (usually far fewer).
 * We assume at most one vacuum can be active for a given index.
 *
 * Access to the shared memory area is controlled by BtreeVacuumLock.
 * In principle we could use a separate lmgr locktag for each index,
 * but a single LWLock is much cheaper, and given the short time that
 * the lock is ever held, the concurrency hit should be minimal.
 */
typedef struct BTOneVacInfo {
    LockRelId relid;   /* global identifier of an index */
    BTCycleId cycleid; /* cycle ID for its active VACUUM */
} BTOneVacInfoNode;

typedef struct BTVacInfo {
    BTCycleId cycle_ctr;     /* cycle ID most recently assigned */
    int num_vacuums;         /* number of currently active VACUUMs */
    int max_vacuums;         /* allocated length of vacuums[] array */
    BTOneVacInfo vacuums[1]; /* VARIABLE LENGTH ARRAY */
} BTVacInfoNode;

/*
 * _bt_vacuum_cycleid --- get the active vacuum cycle ID for an index,
 *		or zero if there is no active VACUUM
 *
 * Note: for correct interlocking, the caller must already hold pin and
 * exclusive lock on each buffer it will store the cycle ID into.  This
 * ensures that even if a VACUUM starts immediately afterwards, it cannot
 * process those pages until the page split is complete.
 */
BTCycleId _bt_vacuum_cycleid(const Relation rel)
{
    BTCycleId result = 0;
    int i;

    /* Share lock is enough since this is a read-only operation */
    LWLockAcquire(BtreeVacuumLock, LW_SHARED);

    for (i = 0; i < t_thrd.index_cxt.btvacinfo->num_vacuums; i++) {
        BTOneVacInfo *vac = &t_thrd.index_cxt.btvacinfo->vacuums[i];

        if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
            vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId &&
            vac->relid.bktId == rel->rd_lockInfo.lockRelId.bktId) {
            result = vac->cycleid;
            break;
        }
    }

    LWLockRelease(BtreeVacuumLock);
    return result;
}

/*
 * _bt_start_vacuum --- assign a cycle ID to a just-starting VACUUM operation
 *
 * Note: the caller must guarantee that it will eventually call
 * _bt_end_vacuum, else we'll permanently leak an array slot.  To ensure
 * that this happens even in elog(FATAL) scenarios, the appropriate coding is not just
 * a PG_TRY, but PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel))
 */
BTCycleId _bt_start_vacuum(Relation rel)
{
    BTCycleId result;
    int i;
    BTOneVacInfo *vac = NULL;

    LWLockAcquire(BtreeVacuumLock, LW_EXCLUSIVE);

    /*
     * Assign the next cycle ID, being careful to avoid zero as well as the
     * reserved high values.
     */
    result = ++(t_thrd.index_cxt.btvacinfo->cycle_ctr);
    if (result == 0 || result > MAX_BT_CYCLE_ID) {
        result = t_thrd.index_cxt.btvacinfo->cycle_ctr = 1;
    }

    /* Le's just make sure there'ts no entry already for this index */
    for (i = 0; i < t_thrd.index_cxt.btvacinfo->num_vacuums; i++) {
        vac = &t_thrd.index_cxt.btvacinfo->vacuums[i];
        if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
            vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId &&
            vac->relid.bktId == rel->rd_lockInfo.lockRelId.bktId) {
            /*
             * Unlike most places in the backend, we have to explicitly
             * release our LWLock before throwing an error.  This is because
             * we expect _bt_end_vacuum() to be called before transaction
             * abort cleanup can run to release LWLocks.
             */
            LWLockRelease(BtreeVacuumLock);
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("multiple active vacuums for index \"%s\"", RelationGetRelationName(rel))));
        }
    }

    /* OK, add an entry */
    if (t_thrd.index_cxt.btvacinfo->num_vacuums >= t_thrd.index_cxt.btvacinfo->max_vacuums) {
        LWLockRelease(BtreeVacuumLock);
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("out of btvacinfo slots")));
    }
    vac = &t_thrd.index_cxt.btvacinfo->vacuums[t_thrd.index_cxt.btvacinfo->num_vacuums];
    vac->relid = rel->rd_lockInfo.lockRelId;
    vac->cycleid = result;
    t_thrd.index_cxt.btvacinfo->num_vacuums++;

    LWLockRelease(BtreeVacuumLock);
    return result;
}

/*
 * _bt_end_vacuum --- mark a btree VACUUM operation as done
 *
 * Note: this is deliberately coded not to complain if no entry is found;
 * this allows the caller to put PG_TRY around the start_vacuum operation.
 */
void _bt_end_vacuum(const Relation rel)
{
    int i;

    LWLockAcquire(BtreeVacuumLock, LW_EXCLUSIVE);

    /* Find the array entry */
    for (i = 0; i < t_thrd.index_cxt.btvacinfo->num_vacuums; i++) {
        BTOneVacInfo *vac = &t_thrd.index_cxt.btvacinfo->vacuums[i];
        if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
            vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId &&
            vac->relid.bktId == rel->rd_lockInfo.lockRelId.bktId) {
            /* Remove it by shifting down the last entry */
            *vac = t_thrd.index_cxt.btvacinfo->vacuums[t_thrd.index_cxt.btvacinfo->num_vacuums - 1];
            t_thrd.index_cxt.btvacinfo->num_vacuums--;
            break;
        }
    }

    LWLockRelease(BtreeVacuumLock);
}

/*
 * _bt_end_vacuum wrapped as an on_shmem_exit callback function
 */
void _bt_end_vacuum_callback(int code, Datum arg)
{
    _bt_end_vacuum((Relation)DatumGetPointer(arg));
}

/*
 * BTreeShmemSize --- report amount of shared memory space needed
 */
Size BTreeShmemSize(void)
{
    Size size = offsetof(BTVacInfo, vacuums[0]);
    size = add_size(size, mul_size((Size)g_instance.shmem_cxt.MaxBackends, sizeof(BTOneVacInfo)));
    return size;
}

/*
 * BTreeShmemInit --- initialize this module's shared memory
 */
void BTreeShmemInit(void)
{
    bool found = false;

    t_thrd.index_cxt.btvacinfo = (BTVacInfo *)ShmemInitStruct("BTree Vacuum State", BTreeShmemSize(), &found);

    if (!IsUnderPostmaster) {
        /* Initialize shared memory area */
        Assert(!found);

        /*
         * It doesn't really matter what the cycle counter starts at, but
         * having it always start the same doesn't seem good.  Seed with
         * low-order bits of time() instead.
         */
        t_thrd.index_cxt.btvacinfo->cycle_ctr = (BTCycleId)time(NULL);
        t_thrd.index_cxt.btvacinfo->num_vacuums = 0;
        t_thrd.index_cxt.btvacinfo->max_vacuums = g_instance.shmem_cxt.MaxBackends;
    } else {
        Assert(found);
    }
}

Datum btoptions(PG_FUNCTION_ARGS)
{
    Datum reloptions = PG_GETARG_DATUM(0);
    bool validate = PG_GETARG_BOOL(1);
    bytea *result = NULL;

    result = default_reloptions(reloptions, validate, RELOPT_KIND_BTREE);
    if (result != NULL) {
        PG_RETURN_BYTEA_P(result);
    }
    PG_RETURN_NULL();
}

/*
 *	_bt_nonkey_truncate() -- remove non-key (INCLUDE) attributes from index
 *							tuple.
 *
 *	Transforms an ordinal B-tree leaf index tuple into pivot tuple to be used
 *	as hikey or non-leaf page tuple with downlink.  Note that t_tid offset
 *	will be overritten in order to represent number of present tuple attributes.
 */
IndexTuple btree_truncate(Relation rel, IndexTuple lastleft, IndexTuple firstright, BTScanInsert itup_key)
{
    Assert(!btree_tuple_is_pivot(lastleft));
    Assert(!btree_tuple_is_pivot(firstright));

    TupleDesc itupdesc = RelationGetDescr(rel);
    int nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
    int keepnatts = btree_num_keep_atts(rel, lastleft, firstright, itup_key);

#ifdef DEBUG_NO_TRUNCATE
    keepnatts = nkeyatts + 1;
#endif

    IndexTuple pivot = index_truncate_tuple(itupdesc, firstright, Min(keepnatts, nkeyatts));

    if (btree_tuple_is_posting(pivot)) {
        Assert(keepnatts == nkeyatts || keepnatts == nkeyatts + 1);
        Assert(IndexRelationGetNumberOfAttributes(rel) == nkeyatts);
        pivot->t_info &= ~INDEX_SIZE_MASK;
        pivot->t_info |= MAXALIGN(btree_tuple_get_posting_off(firstright));
    }

    if (keepnatts <= nkeyatts) {
        btree_tuple_set_num_of_atts(pivot, (uint16)keepnatts, false);
        return pivot;
    }

    Size newsize = MAXALIGN(IndexTupleSize(pivot)) + MAXALIGN(sizeof(ItemPointerData));
    IndexTuple tid_pivot = (IndexTuple)palloc0(newsize);

    errno_t rc = memcpy_s(tid_pivot, newsize, pivot, MAXALIGN(IndexTupleSize(pivot)));
    securec_check(rc, "", "");
    pfree(pivot);

    tid_pivot->t_info &= ~INDEX_SIZE_MASK;
    tid_pivot->t_info |= newsize;
    btree_tuple_set_num_of_atts(tid_pivot, (uint16)nkeyatts, true);
    ItemPointer pivot_heaptid = btree_tuple_get_heap_tid(tid_pivot);
    Assert(pivot_heaptid);

    ItemPointerCopy(btree_tuple_get_max_heap_tid(lastleft), pivot_heaptid);

#ifndef DEBUG_NO_TRUNCATE
    Assert(ItemPointerCompare(btree_tuple_get_max_heap_tid(lastleft), btree_tuple_get_heap_tid(firstright)) <= 0);
    Assert(ItemPointerCompare(pivot_heaptid, btree_tuple_get_heap_tid(lastleft)) >= 0);
    Assert(ItemPointerCompare(pivot_heaptid, btree_tuple_get_heap_tid(firstright)) <= 0);
#else

    ItemPointerCopy(btree_tuple_get_heap_tid(firstright), pivot_heaptid);
    ItemPointerSetOffsetNumber(pivot_heaptid, OffsetNumberPrev(ItemPointerGetOffsetNumber(pivot_heaptid)));
    Assert(ItemPointerCompare(pivot_heaptid, btree_tuple_get_heap_tid(firstright)) < 0);
#endif

    return tid_pivot;
}

static int btree_num_keep_atts(Relation rel, IndexTuple lastleft, IndexTuple firstright, BTScanInsert itup_key)
{
    int num_key_atts = IndexRelationGetNumberOfKeyAttributes(rel);
    if (!itup_key->heapkeyspace) {
        return num_key_atts;
    }

    TupleDesc itupdesc = RelationGetDescr(rel);
    int num_keep_atts = 1;
    ScanKey scankey = itup_key->scankeys;

    for (int attnum = 1; attnum <= num_key_atts; attnum++, scankey++) {

        bool is_null_1, is_null_2;
        Datum datum1 = index_getattr(lastleft, attnum, itupdesc, &is_null_1);
        Datum datum2 = index_getattr(firstright, attnum, itupdesc, &is_null_2);

        if (is_null_1 != is_null_2)
            break;

        if (!is_null_1 && DatumGetInt32(FunctionCall2Coll(&scankey->sk_func, scankey->sk_collation, datum1, datum2)) != 0)
            break;

        num_keep_atts++;
    }

    Assert(!itup_key->allequalimage || num_keep_atts == btree_num_keep_atts_fast(rel, lastleft, firstright));

    return num_keep_atts;
}

int btree_num_keep_atts_fast(Relation rel, IndexTuple lastleft, IndexTuple firstright)
{
    int num_key_atts = IndexRelationGetNumberOfKeyAttributes(rel);
    TupleDesc itup_desc = RelationGetDescr(rel);
    int num_keep_atts = 1;

    for (int attnum = 1; attnum <= num_key_atts; attnum++) {

        bool is_null_1, is_null_2;
        Form_pg_attribute att;
        Datum datum1 = index_getattr(lastleft, attnum, itup_desc, &is_null_1);
        Datum datum2 = index_getattr(firstright, attnum, itup_desc, &is_null_2);
        att = TupleDescAttr(itup_desc, attnum - 1);

        if (is_null_1 != is_null_2)
            break;

        if (!is_null_1 && !DatumImageEq(datum1, datum2, att->attbyval, att->attlen))
            break;

        num_keep_atts++;
    }

    return num_keep_atts;
}


void btree_check_third_page(Relation rel, Relation heap, bool need_heaptid_space, Page page, IndexTuple tuple)
{
    Size itemsz = MAXALIGN(IndexTupleSize(tuple));
    if (itemsz <= BTREE_MAX_ITEM_SIZE(page))
        return;

    if (!need_heaptid_space && itemsz <= BTREE_MAX_ITEM_SIZE_NO_HEAP_TID(page)) {
        return;
    }

    if (!P_ISLEAF((BTPageOpaqueInternal)PageGetSpecialPointer(page))) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("cannot insert oversized tuple of size %lu on internal page of index \"%s\"",
                               (unsigned long)itemsz, RelationGetRelationName(rel))));
    }

    ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
					(unsigned long)itemsz,
					(unsigned long)BTREE_MAX_ITEM_SIZE(page),
                    RelationGetRelationName(rel)),
			 errdetail("Index row references tuple (%u,%hu) in relation \"%s\".",
					   ItemPointerGetBlockNumber(&tuple->t_tid),
					   ItemPointerGetOffsetNumber(&tuple->t_tid),
					   heap ? RelationGetRelationName(heap) : "unknown"),
			 errhint("Values larger than 1/3 of a buffer page cannot be indexed.\n"
					 "Consider a function index of an MD5 hash of the value, "
					 "or use full text indexing.")));
}

bool btree_allequalimage(Relation rel, bool debugmessage)
{
    if (t_thrd.proc->workingVersionNum < NBTREE_DEDUPLICATION_VERSION_NUM ||
        IndexRelationGetNumberOfAttributes(rel) != IndexRelationGetNumberOfKeyAttributes(rel) || IsSystemRelation(rel))
        return false;

    bool allequalimage = true;

    for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(rel); i++) {
        Oid opfamily = rel->rd_opfamily[i];
        Oid opcintype = rel->rd_opcintype[i];
        Oid collation = rel->rd_indcollation[i];
        Oid equalimage_proc = get_opfamily_proc(opfamily, opcintype, opcintype, BTEQUALIMAGE_PROC);

        if (!OidIsValid(equalimage_proc) ||
            !DatumGetBool(OidFunctionCall1Coll(equalimage_proc, collation, ObjectIdGetDatum(opcintype)))) {
            allequalimage = false;
            break;
        }
    }

    if (debugmessage) {
        if (allequalimage)
            elog(DEBUG1, "index \"%s\" can safely use deduplication", RelationGetRelationName(rel));
        else
            elog(DEBUG1, "index \"%s\" cannot use deduplication", RelationGetRelationName(rel));
    }

    return allequalimage;
}