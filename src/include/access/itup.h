/* -------------------------------------------------------------------------
 *
 * itup.h
 *	  openGauss index tuple definitions.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/itup.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef ITUP_H
#define ITUP_H

#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/buf/bufpage.h"
#include "storage/item/itemptr.h"

/*
 * Index tuple header structure
 *
 * All index tuples start with IndexTupleData.	If the HasNulls bit is set,
 * this is followed by an IndexAttributeBitMapData.  The index attribute
 * values follow, beginning at a MAXALIGN boundary.
 *
 * Note that the space allocated for the bitmap does not vary with the number
 * of attributes; that is because we don't have room to store the number of
 * attributes in the header.  Given the MAXALIGN constraint there's no space
 * savings to be had anyway, for usual values of INDEX_MAX_KEYS.
 */

typedef struct IndexTupleData {
    ItemPointerData t_tid; /* reference TID to heap tuple */

    /* ---------------
     * t_info is laid out in the following fashion:
     *
     * 15th (high) bit: has nulls
     * 14th bit: has var-width attributes
     * 13th bit: AM-defined meaning
     * 12-0 bit: size of tuple
     * ---------------
     */

    unsigned short t_info; /* various info about tuple */

} IndexTupleData; /* MORE DATA FOLLOWS AT END OF STRUCT */

typedef IndexTupleData* IndexTuple;

/* full 8B xid used in Create Index */
typedef struct IndexTransInfo {
    TransactionId xmin;
    TransactionId xmax;
} IndexTransInfo;

/* short 4B xid used in IndexTuple */
typedef struct UstoreIndexXidData {
    ShortTransactionId xmin;
    ShortTransactionId xmax;
} UstoreIndexXidData;

typedef UstoreIndexXidData* UstoreIndexXid;

typedef struct IndexAttributeBitMapData {
    bits8 bits[(INDEX_MAX_KEYS + 8 - 1) / 8];
} IndexAttributeBitMapData;

typedef IndexAttributeBitMapData* IndexAttributeBitMap;

/*
 * t_info manipulation macros
 */
#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_AM_RESERVED_BIT 0x2000 /* reserved for index-AM specific usage */
#define INDEX_VAR_MASK 0x4000
#define INDEX_NULL_MASK 0x8000

#define IndexTupleSize(itup) ((Size)(((IndexTuple)(itup))->t_info & INDEX_SIZE_MASK))
#define IndexTupleDSize(itup) ((Size)((itup).t_info & INDEX_SIZE_MASK))
#define IndexTupleHasNulls(itup) ((((IndexTuple)(itup))->t_info & INDEX_NULL_MASK))
#define IndexTupleHasVarwidths(itup) ((((IndexTuple)(itup))->t_info & INDEX_VAR_MASK))

#define IndexTupleSetSize(itup, newsz) (itup->t_info = ((itup->t_info) & (~INDEX_SIZE_MASK)) | (newsz))

#define UstoreIndexTupleGetXid(itup) (((char*)(itup)) + (IndexTupleSize(itup)))

/*
 * Takes an infomask as argument (primarily because this needs to be usable
 * at index_form_tuple time so enough space is allocated).
 */
#define IndexInfoFindDataOffset(t_info)                                       \
    ((!((t_info)&INDEX_NULL_MASK)) ? ((Size)MAXALIGN(sizeof(IndexTupleData))) \
                                   : ((Size)MAXALIGN(sizeof(IndexTupleData) + sizeof(IndexAttributeBitMapData))))

/* ----------------
 *		index_getattr
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call nocache_index_getattr() for the rest.
 *
 * ----------------
 */
#define index_getattr(tup, attnum, tupleDesc, isnull)                                                    \
    (AssertMacro(PointerIsValid(isnull) && (attnum) > 0),                                                \
        *(isnull) = false,                                                                               \
        !IndexTupleHasNulls(tup) ? (TupleDescAttr((tupleDesc), (attnum)-1)->attcacheoff >= 0             \
                                           ? (fetchatt(TupleDescAttr((tupleDesc), (attnum)-1),           \
                                                 (char*)(tup) + IndexInfoFindDataOffset((tup)->t_info) + \
                                             TupleDescAttr((tupleDesc), (attnum)-1)->attcacheoff))       \
                                           : nocache_index_getattr((tup), (attnum), (tupleDesc)))        \
                                 : ((att_isnull((attnum)-1, (char*)(tup) + sizeof(IndexTupleData)))      \
                                           ? (*(isnull) = true, (Datum)NULL)                             \
                                           : (nocache_index_getattr((tup), (attnum), (tupleDesc)))))

/*
 * MaxIndexTuplesPerPage is an upper bound on the number of tuples that can
 * fit on one index page.  An index tuple must have either data or a null
 * bitmap, so we can safely assume it's at least 1 byte bigger than a bare
 * IndexTupleData struct.  We arrive at the divisor because each tuple
 * must be maxaligned, and it must have an associated item pointer.
 */
#define MinIndexTupleSize MAXALIGN(sizeof(IndexTupleData) + 1)
#define MaxIndexTuplesPerPage \
    ((int)((BLCKSZ - SizeOfPageHeaderData) / (MAXALIGN(sizeof(IndexTupleData) + 1) + sizeof(ItemIdData))))

/* routines in indextuple.c */
extern IndexTuple index_form_tuple(TupleDesc tuple_descriptor, Datum* values, const bool* isnull,
                                   bool is_ubtree = false, bool is_ubtree_pcr = false);
extern Datum nocache_index_getattr(IndexTuple tup, uint32 attnum, TupleDesc tuple_desc);
extern void index_deform_tuple(IndexTuple tup, TupleDesc tuple_descriptor, Datum* values, bool* isnull);
extern IndexTuple index_truncate_tuple(TupleDesc tupleDescriptor, IndexTuple olditup, int new_indnatts);
extern IndexTuple UBTreeIndexTruncateTuple(TupleDesc tupleDescriptor, IndexTuple olditup, int new_indnatts,
    bool itup_extended);
extern IndexTuple CopyIndexTuple(IndexTuple source);
extern Oid index_getattr_tableoid(Relation irel, IndexTuple itup);
extern int2 index_getattr_bucketid(Relation irel, IndexTuple itup);
extern IndexTuple CopyIndexTupleAndReserveSpace(IndexTuple source, Size reserved_size);

#endif /* ITUP_H */
