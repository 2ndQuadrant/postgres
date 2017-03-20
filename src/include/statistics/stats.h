/*-------------------------------------------------------------------------
 *
 * stats.h
 *	  Extended statistics and selectivity estimation functions.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/statistics/stats.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STATS_H
#define STATS_H

#include "commands/vacuum.h"

#define STATS_MAX_DIMENSIONS	8		/* max number of attributes */

#define STATS_NDISTINCT_MAGIC		0xA352BFA4	/* marks serialized bytea */
#define STATS_NDISTINCT_TYPE_BASIC	1	/* basic MCV list type */

/* Multivariate distinct coefficients. */
typedef struct MVNDistinctItem
{
	double		ndistinct;
	int16		nattrs;
	int16	   *attrs;
} MVNDistinctItem;

typedef struct MVNDistinctData
{
	uint32		magic;			/* magic constant marker */
	uint32		type;			/* type of ndistinct (BASIC) */
	uint32		nitems;			/* number of items in the statistic */
	MVNDistinctItem items[FLEXIBLE_ARRAY_MEMBER];
} MVNDistinctData;

typedef MVNDistinctData *MVNDistinct;

extern MVNDistinct statext_ndistinct_load(Oid mvoid);

extern void BuildRelationExtStatistics(Relation onerel, double totalrows,
						   int numrows, HeapTuple *rows,
						   int natts, VacAttrStats **vacattrstats);
extern bool statext_is_kind_built(HeapTuple htup, char kind);

#endif   /* STATS_H */
