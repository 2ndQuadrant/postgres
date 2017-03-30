/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute clause selectivities
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/clausesel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic_ext.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/var.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "statistics/statistics.h"
#include "utils/typcache.h"


/*
 * Data structure for accumulating info about possible range-query
 * clause pairs in clauselist_selectivity.
 */
typedef struct RangeQueryClause
{
	struct RangeQueryClause *next;		/* next in linked list */
	Node	   *var;			/* The common variable of the clauses */
	bool		have_lobound;	/* found a low-bound clause yet? */
	bool		have_hibound;	/* found a high-bound clause yet? */
	Selectivity lobound;		/* Selectivity of a var > something clause */
	Selectivity hibound;		/* Selectivity of a var < something clause */
} RangeQueryClause;

static void addRangeClause(RangeQueryClause **rqlist, Node *clause,
			   bool varonleft, bool isLTsel, Selectivity s2);
static bool clause_is_ext_compatible(Node *clause, Index relid, AttrNumber *attnum);
static Bitmapset *collect_ext_attnums(List *clauses, Index relid);
static int count_ext_attnums(List *clauses, Index relid);
static bool get_singleton_varno(List *clauses, Index *relid);
static StatisticExtInfo *choose_ext_statistics(List *stats,
									Bitmapset *attnums, char requiredkind);
static List *clauselist_ext_split(PlannerInfo *root, Index relid,
					List *clauses, List **mvclauses,
					StatisticExtInfo *stats);
static Selectivity clauselist_ext_selectivity_deps(PlannerInfo *root,
						Index relid, List *clauses, StatisticExtInfo *stats,
						Index varRelid, JoinType jointype, SpecialJoinInfo *sjinfo);
static bool has_stats(List *stats, char requiredkind);


/****************************************************************************
 *		ROUTINES TO COMPUTE SELECTIVITIES
 ****************************************************************************/

/*
 * clauselist_selectivity -
 *	  Compute the selectivity of an implicitly-ANDed list of boolean
 *	  expression clauses.  The list can be empty, in which case 1.0
 *	  must be returned.  List elements may be either RestrictInfos
 *	  or bare expression clauses --- the former is preferred since
 *	  it allows caching of results.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 *
 * Our basic approach is to take the product of the selectivities of the
 * subclauses.  However, that's only right if the subclauses have independent
 * probabilities, and in reality they are often NOT independent.  So,
 * we want to be smarter where we can.
 *
 * The first thing we try to do is applying extended statistics, in a way
 * that intends to minimize the overhead when there are no extended stats
 * on the relation. Thus we do several simple (and inexpensive) checks first,
 * to verify that suitable extended statistics exist.
 *
 * If we identify such extended statistics apply, we try to apply them.
 * Currently we only have (soft) functional dependencies, so we try to reduce
 * the list of clauses.
 *
 * Then we remove the clauses estimated using extended stats, and process
 * the rest of the clauses using the regular per-column stats.
 *
 * We also recognize "range queries", such as "x > 34 AND x < 42".  Clauses
 * are recognized as possible range query components if they are restriction
 * opclauses whose operators have scalarltsel() or scalargtsel() as their
 * restriction selectivity estimator.  We pair up clauses of this form that
 * refer to the same variable.  An unpairable clause of this kind is simply
 * multiplied into the selectivity product in the normal way.  But when we
 * find a pair, we know that the selectivities represent the relative
 * positions of the low and high bounds within the column's range, so instead
 * of figuring the selectivity as hisel * losel, we can figure it as hisel +
 * losel - 1.  (To visualize this, see that hisel is the fraction of the range
 * below the high bound, while losel is the fraction above the low bound; so
 * hisel can be interpreted directly as a 0..1 value but we need to convert
 * losel to 1-losel before interpreting it as a value.  Then the available
 * range is 1-losel to hisel.  However, this calculation double-excludes
 * nulls, so really we need hisel + losel + null_frac - 1.)
 *
 * If either selectivity is exactly DEFAULT_INEQ_SEL, we forget this equation
 * and instead use DEFAULT_RANGE_INEQ_SEL.  The same applies if the equation
 * yields an impossible (negative) result.
 *
 * A free side-effect is that we can recognize redundant inequalities such
 * as "x < 4 AND x < 5"; only the tighter constraint will be counted.
 *
 * Of course this is all very dependent on the behavior of
 * scalarltsel/scalargtsel; perhaps some day we can generalize the approach.
 */
Selectivity
clauselist_selectivity(PlannerInfo *root,
					   List *clauses,
					   int varRelid,
					   JoinType jointype,
					   SpecialJoinInfo *sjinfo)
{
	Selectivity s1 = 1.0;
	RangeQueryClause *rqlist = NULL;
	ListCell   *l;
	Index		relid;

	/*
	 * If there's exactly one clause, then extended statistics is futile
	 * at this level (we might be able to apply them later if it's AND/OR
	 * clause). So just go directly to clause_selectivity().
	 */
	if (list_length(clauses) == 1)
		return clause_selectivity(root, (Node *) linitial(clauses),
								  varRelid, jointype, sjinfo);

	/*
	 * To fetch the statistics, we first need to determine the rel. Currently
	 * we only support estimates of simple restrictions referencing a single
	 * baserel (no join statistics). However set_baserel_size_estimates() sets
	 * varRelid=0 so we have to actually inspect the clauses by pull_varnos
	 * and see if there's just a single varno referenced.
	 *
	 * XXX Maybe there's a better way to find the relid?
	 */
	if (get_singleton_varno(clauses, &relid) &&
		(varRelid == 0 || varRelid == relid))
	{
		RelOptInfo *rel = find_base_rel(root, relid);
		List	   *stats = rel->statlist;

		/*
		 * Check that there are extended statistics usable for selectivity
		 * estimation, i.e. anything except ndistinct coefficients.
		 *
		 * Also check the number of attributes in clauses that might be
		 * estimated using those statistics, and that there are at least two
		 * such attributes.  It may easily happen that we won't be able to
		 * estimate the clauses using the extended statistics anyway, but that
		 * requires a more expensive check to verify (so the check should be
		 * worth it).
		 *
		 * If there are no such stats or not enough attributes, don't waste time
		 * simply skip to estimation using the plain per-column stats.
		 */
		if (stats != NULL &&
			has_stats(stats, STATS_EXT_DEPENDENCIES) &&
			(count_ext_attnums(clauses, relid) >= 2))
		{
			StatisticExtInfo *stat;
			Bitmapset  *mvattnums;

			/* collect attributes from the compatible conditions */
			mvattnums = collect_ext_attnums(clauses, relid);

			/* and search for the statistic covering the most attributes */
			stat = choose_ext_statistics(stats, mvattnums,
										 STATS_EXT_DEPENDENCIES);

			if (stat != NULL)		/* we have a matching stats */
			{
				/* clauses compatible with multi-variate stats */
				List	   *mvclauses = NIL;

				/* split the clauselist into regular and mv-clauses */
				clauses = clauselist_ext_split(root, relid, clauses,
											&mvclauses, stat);

				/* Empty list of clauses is a clear sign something went wrong. */
				Assert(list_length(mvclauses));

				/* compute the extended stats (dependencies) */
				s1 *= clauselist_ext_selectivity_deps(root, relid, mvclauses, stat,
													 varRelid, jointype, sjinfo);
			}
		}
	}

	/*
	 * Initial scan over clauses.  Anything that doesn't look like a potential
	 * rangequery clause gets multiplied into s1 and forgotten. Anything that
	 * does gets inserted into an rqlist entry.
	 */
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		RestrictInfo *rinfo;
		Selectivity s2;

		/* Always compute the selectivity using clause_selectivity */
		s2 = clause_selectivity(root, clause, varRelid, jointype, sjinfo);

		/*
		 * Check for being passed a RestrictInfo.
		 *
		 * If it's a pseudoconstant RestrictInfo, then s2 is either 1.0 or
		 * 0.0; just use that rather than looking for range pairs.
		 */
		if (IsA(clause, RestrictInfo))
		{
			rinfo = (RestrictInfo *) clause;
			if (rinfo->pseudoconstant)
			{
				s1 = s1 * s2;
				continue;
			}
			clause = (Node *) rinfo->clause;
		}
		else
			rinfo = NULL;

		/*
		 * See if it looks like a restriction clause with a pseudoconstant on
		 * one side.  (Anything more complicated than that might not behave in
		 * the simple way we are expecting.)  Most of the tests here can be
		 * done more efficiently with rinfo than without.
		 */
		if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
		{
			OpExpr	   *expr = (OpExpr *) clause;
			bool		varonleft = true;
			bool		ok;

			if (rinfo)
			{
				ok = (bms_membership(rinfo->clause_relids) == BMS_SINGLETON) &&
					(is_pseudo_constant_clause_relids(lsecond(expr->args),
													  rinfo->right_relids) ||
					 (varonleft = false,
					  is_pseudo_constant_clause_relids(linitial(expr->args),
													   rinfo->left_relids)));
			}
			else
			{
				ok = (NumRelids(clause) == 1) &&
					(is_pseudo_constant_clause(lsecond(expr->args)) ||
					 (varonleft = false,
					  is_pseudo_constant_clause(linitial(expr->args))));
			}

			if (ok)
			{
				/*
				 * If it's not a "<" or ">" operator, just merge the
				 * selectivity in generically.  But if it's the right oprrest,
				 * add the clause to rqlist for later processing.
				 */
				switch (get_oprrest(expr->opno))
				{
					case F_SCALARLTSEL:
						addRangeClause(&rqlist, clause,
									   varonleft, true, s2);
						break;
					case F_SCALARGTSEL:
						addRangeClause(&rqlist, clause,
									   varonleft, false, s2);
						break;
					default:
						/* Just merge the selectivity in generically */
						s1 = s1 * s2;
						break;
				}
				continue;		/* drop to loop bottom */
			}
		}

		/* Not the right form, so treat it generically. */
		s1 = s1 * s2;
	}

	/*
	 * Now scan the rangequery pair list.
	 */
	while (rqlist != NULL)
	{
		RangeQueryClause *rqnext;

		if (rqlist->have_lobound && rqlist->have_hibound)
		{
			/* Successfully matched a pair of range clauses */
			Selectivity s2;

			/*
			 * Exact equality to the default value probably means the
			 * selectivity function punted.  This is not airtight but should
			 * be good enough.
			 */
			if (rqlist->hibound == DEFAULT_INEQ_SEL ||
				rqlist->lobound == DEFAULT_INEQ_SEL)
			{
				s2 = DEFAULT_RANGE_INEQ_SEL;
			}
			else
			{
				s2 = rqlist->hibound + rqlist->lobound - 1.0;

				/* Adjust for double-exclusion of NULLs */
				s2 += nulltestsel(root, IS_NULL, rqlist->var,
								  varRelid, jointype, sjinfo);

				/*
				 * A zero or slightly negative s2 should be converted into a
				 * small positive value; we probably are dealing with a very
				 * tight range and got a bogus result due to roundoff errors.
				 * However, if s2 is very negative, then we probably have
				 * default selectivity estimates on one or both sides of the
				 * range that we failed to recognize above for some reason.
				 */
				if (s2 <= 0.0)
				{
					if (s2 < -0.01)
					{
						/*
						 * No data available --- use a default estimate that
						 * is small, but not real small.
						 */
						s2 = DEFAULT_RANGE_INEQ_SEL;
					}
					else
					{
						/*
						 * It's just roundoff error; use a small positive
						 * value
						 */
						s2 = 1.0e-10;
					}
				}
			}
			/* Merge in the selectivity of the pair of clauses */
			s1 *= s2;
		}
		else
		{
			/* Only found one of a pair, merge it in generically */
			if (rqlist->have_lobound)
				s1 *= rqlist->lobound;
			else
				s1 *= rqlist->hibound;
		}
		/* release storage and advance */
		rqnext = rqlist->next;
		pfree(rqlist);
		rqlist = rqnext;
	}

	return s1;
}

/*
 * addRangeClause --- add a new range clause for clauselist_selectivity
 *
 * Here is where we try to match up pairs of range-query clauses
 */
static void
addRangeClause(RangeQueryClause **rqlist, Node *clause,
			   bool varonleft, bool isLTsel, Selectivity s2)
{
	RangeQueryClause *rqelem;
	Node	   *var;
	bool		is_lobound;

	if (varonleft)
	{
		var = get_leftop((Expr *) clause);
		is_lobound = !isLTsel;	/* x < something is high bound */
	}
	else
	{
		var = get_rightop((Expr *) clause);
		is_lobound = isLTsel;	/* something < x is low bound */
	}

	for (rqelem = *rqlist; rqelem; rqelem = rqelem->next)
	{
		/*
		 * We use full equal() here because the "var" might be a function of
		 * one or more attributes of the same relation...
		 */
		if (!equal(var, rqelem->var))
			continue;
		/* Found the right group to put this clause in */
		if (is_lobound)
		{
			if (!rqelem->have_lobound)
			{
				rqelem->have_lobound = true;
				rqelem->lobound = s2;
			}
			else
			{

				/*------
				 * We have found two similar clauses, such as
				 * x < y AND x < z.
				 * Keep only the more restrictive one.
				 *------
				 */
				if (rqelem->lobound > s2)
					rqelem->lobound = s2;
			}
		}
		else
		{
			if (!rqelem->have_hibound)
			{
				rqelem->have_hibound = true;
				rqelem->hibound = s2;
			}
			else
			{

				/*------
				 * We have found two similar clauses, such as
				 * x > y AND x > z.
				 * Keep only the more restrictive one.
				 *------
				 */
				if (rqelem->hibound > s2)
					rqelem->hibound = s2;
			}
		}
		return;
	}

	/* No matching var found, so make a new clause-pair data structure */
	rqelem = (RangeQueryClause *) palloc(sizeof(RangeQueryClause));
	rqelem->var = var;
	if (is_lobound)
	{
		rqelem->have_lobound = true;
		rqelem->have_hibound = false;
		rqelem->lobound = s2;
	}
	else
	{
		rqelem->have_lobound = false;
		rqelem->have_hibound = true;
		rqelem->hibound = s2;
	}
	rqelem->next = *rqlist;
	*rqlist = rqelem;
}

/*
 * bms_is_subset_singleton
 *
 * Same result as bms_is_subset(s, bms_make_singleton(x)),
 * but a little faster and doesn't leak memory.
 *
 * Is this of use anywhere else?  If so move to bitmapset.c ...
 */
static bool
bms_is_subset_singleton(const Bitmapset *s, int x)
{
	switch (bms_membership(s))
	{
		case BMS_EMPTY_SET:
			return true;
		case BMS_SINGLETON:
			return bms_is_member(x, s);
		case BMS_MULTIPLE:
			return false;
	}
	/* can't get here... */
	return false;
}

/*
 * treat_as_join_clause -
 *	  Decide whether an operator clause is to be handled by the
 *	  restriction or join estimator.  Subroutine for clause_selectivity().
 */
static inline bool
treat_as_join_clause(Node *clause, RestrictInfo *rinfo,
					 int varRelid, SpecialJoinInfo *sjinfo)
{
	if (varRelid != 0)
	{
		/*
		 * Caller is forcing restriction mode (eg, because we are examining an
		 * inner indexscan qual).
		 */
		return false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * It must be a restriction clause, since it's being evaluated at a
		 * scan node.
		 */
		return false;
	}
	else
	{
		/*
		 * Otherwise, it's a join if there's more than one relation used. We
		 * can optimize this calculation if an rinfo was passed.
		 *
		 * XXX	Since we know the clause is being evaluated at a join, the
		 * only way it could be single-relation is if it was delayed by outer
		 * joins.  Although we can make use of the restriction qual estimators
		 * anyway, it seems likely that we ought to account for the
		 * probability of injected nulls somehow.
		 */
		if (rinfo)
			return (bms_membership(rinfo->clause_relids) == BMS_MULTIPLE);
		else
			return (NumRelids(clause) > 1);
	}
}


/*
 * clause_selectivity -
 *	  Compute the selectivity of a general boolean expression clause.
 *
 * The clause can be either a RestrictInfo or a plain expression.  If it's
 * a RestrictInfo, we try to cache the selectivity for possible re-use,
 * so passing RestrictInfos is preferred.
 *
 * varRelid is either 0 or a rangetable index.
 *
 * When varRelid is not 0, only variables belonging to that relation are
 * considered in computing selectivity; other vars are treated as constants
 * of unknown values.  This is appropriate for estimating the selectivity of
 * a join clause that is being used as a restriction clause in a scan of a
 * nestloop join's inner relation --- varRelid should then be the ID of the
 * inner relation.
 *
 * When varRelid is 0, all variables are treated as variables.  This
 * is appropriate for ordinary join clauses and restriction clauses.
 *
 * jointype is the join type, if the clause is a join clause.  Pass JOIN_INNER
 * if the clause isn't a join clause.
 *
 * sjinfo is NULL for a non-join clause, otherwise it provides additional
 * context information about the join being performed.  There are some
 * special cases:
 *	1. For a special (not INNER) join, sjinfo is always a member of
 *	   root->join_info_list.
 *	2. For an INNER join, sjinfo is just a transient struct, and only the
 *	   relids and jointype fields in it can be trusted.
 * It is possible for jointype to be different from sjinfo->jointype.
 * This indicates we are considering a variant join: either with
 * the LHS and RHS switched, or with one input unique-ified.
 *
 * Note: when passing nonzero varRelid, it's normally appropriate to set
 * jointype == JOIN_INNER, sjinfo == NULL, even if the clause is really a
 * join clause; because we aren't treating it as a join clause.
 */
Selectivity
clause_selectivity(PlannerInfo *root,
				   Node *clause,
				   int varRelid,
				   JoinType jointype,
				   SpecialJoinInfo *sjinfo)
{
	Selectivity s1 = 0.5;		/* default for any unhandled clause type */
	RestrictInfo *rinfo = NULL;
	bool		cacheable = false;

	if (clause == NULL)			/* can this still happen? */
		return s1;

	if (IsA(clause, RestrictInfo))
	{
		rinfo = (RestrictInfo *) clause;

		/*
		 * If the clause is marked pseudoconstant, then it will be used as a
		 * gating qual and should not affect selectivity estimates; hence
		 * return 1.0.  The only exception is that a constant FALSE may be
		 * taken as having selectivity 0.0, since it will surely mean no rows
		 * out of the plan.  This case is simple enough that we need not
		 * bother caching the result.
		 */
		if (rinfo->pseudoconstant)
		{
			if (!IsA(rinfo->clause, Const))
				return (Selectivity) 1.0;
		}

		/*
		 * If the clause is marked redundant, always return 1.0.
		 */
		if (rinfo->norm_selec > 1)
			return (Selectivity) 1.0;

		/*
		 * If possible, cache the result of the selectivity calculation for
		 * the clause.  We can cache if varRelid is zero or the clause
		 * contains only vars of that relid --- otherwise varRelid will affect
		 * the result, so mustn't cache.  Outer join quals might be examined
		 * with either their join's actual jointype or JOIN_INNER, so we need
		 * two cache variables to remember both cases.  Note: we assume the
		 * result won't change if we are switching the input relations or
		 * considering a unique-ified case, so we only need one cache variable
		 * for all non-JOIN_INNER cases.
		 */
		if (varRelid == 0 ||
			bms_is_subset_singleton(rinfo->clause_relids, varRelid))
		{
			/* Cacheable --- do we already have the result? */
			if (jointype == JOIN_INNER)
			{
				if (rinfo->norm_selec >= 0)
					return rinfo->norm_selec;
			}
			else
			{
				if (rinfo->outer_selec >= 0)
					return rinfo->outer_selec;
			}
			cacheable = true;
		}

		/*
		 * Proceed with examination of contained clause.  If the clause is an
		 * OR-clause, we want to look at the variant with sub-RestrictInfos,
		 * so that per-subclause selectivities can be cached.
		 */
		if (rinfo->orclause)
			clause = (Node *) rinfo->orclause;
		else
			clause = (Node *) rinfo->clause;
	}

	if (IsA(clause, Var))
	{
		Var		   *var = (Var *) clause;

		/*
		 * We probably shouldn't ever see an uplevel Var here, but if we do,
		 * return the default selectivity...
		 */
		if (var->varlevelsup == 0 &&
			(varRelid == 0 || varRelid == (int) var->varno))
		{
			/* Use the restriction selectivity function for a bool Var */
			s1 = boolvarsel(root, (Node *) var, varRelid);
		}
	}
	else if (IsA(clause, Const))
	{
		/* bool constant is pretty easy... */
		Const	   *con = (Const *) clause;

		s1 = con->constisnull ? 0.0 :
			DatumGetBool(con->constvalue) ? 1.0 : 0.0;
	}
	else if (IsA(clause, Param))
	{
		/* see if we can replace the Param */
		Node	   *subst = estimate_expression_value(root, clause);

		if (IsA(subst, Const))
		{
			/* bool constant is pretty easy... */
			Const	   *con = (Const *) subst;

			s1 = con->constisnull ? 0.0 :
				DatumGetBool(con->constvalue) ? 1.0 : 0.0;
		}
		else
		{
			/* XXX any way to do better than default? */
		}
	}
	else if (not_clause(clause))
	{
		/* inverse of the selectivity of the underlying clause */
		s1 = 1.0 - clause_selectivity(root,
								  (Node *) get_notclausearg((Expr *) clause),
									  varRelid,
									  jointype,
									  sjinfo);
	}
	else if (and_clause(clause))
	{
		/* share code with clauselist_selectivity() */
		s1 = clauselist_selectivity(root,
									((BoolExpr *) clause)->args,
									varRelid,
									jointype,
									sjinfo);
	}
	else if (or_clause(clause))
	{
		/*
		 * Selectivities for an OR clause are computed as s1+s2 - s1*s2 to
		 * account for the probable overlap of selected tuple sets.
		 *
		 * XXX is this too conservative?
		 */
		ListCell   *arg;

		s1 = 0.0;
		foreach(arg, ((BoolExpr *) clause)->args)
		{
			Selectivity s2 = clause_selectivity(root,
												(Node *) lfirst(arg),
												varRelid,
												jointype,
												sjinfo);

			s1 = s1 + s2 - s1 * s2;
		}
	}
	else if (is_opclause(clause) || IsA(clause, DistinctExpr))
	{
		OpExpr	   *opclause = (OpExpr *) clause;
		Oid			opno = opclause->opno;

		if (treat_as_join_clause(clause, rinfo, varRelid, sjinfo))
		{
			/* Estimate selectivity for a join clause. */
			s1 = join_selectivity(root, opno,
								  opclause->args,
								  opclause->inputcollid,
								  jointype,
								  sjinfo);
		}
		else
		{
			/* Estimate selectivity for a restriction clause. */
			s1 = restriction_selectivity(root, opno,
										 opclause->args,
										 opclause->inputcollid,
										 varRelid);
		}

		/*
		 * DistinctExpr has the same representation as OpExpr, but the
		 * contained operator is "=" not "<>", so we must negate the result.
		 * This estimation method doesn't give the right behavior for nulls,
		 * but it's better than doing nothing.
		 */
		if (IsA(clause, DistinctExpr))
			s1 = 1.0 - s1;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = scalararraysel(root,
							(ScalarArrayOpExpr *) clause,
							treat_as_join_clause(clause, rinfo,
												 varRelid, sjinfo),
							varRelid,
							jointype,
							sjinfo);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = rowcomparesel(root,
						   (RowCompareExpr *) clause,
						   varRelid,
						   jointype,
						   sjinfo);
	}
	else if (IsA(clause, NullTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = nulltestsel(root,
						 ((NullTest *) clause)->nulltesttype,
						 (Node *) ((NullTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, BooleanTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = booltestsel(root,
						 ((BooleanTest *) clause)->booltesttype,
						 (Node *) ((BooleanTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, CurrentOfExpr))
	{
		/* CURRENT OF selects at most one row of its table */
		CurrentOfExpr *cexpr = (CurrentOfExpr *) clause;
		RelOptInfo *crel = find_base_rel(root, cexpr->cvarno);

		if (crel->tuples > 0)
			s1 = 1.0 / crel->tuples;
	}
	else if (IsA(clause, RelabelType))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity(root,
								(Node *) ((RelabelType *) clause)->arg,
								varRelid,
								jointype,
								sjinfo);
	}
	else if (IsA(clause, CoerceToDomain))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity(root,
								(Node *) ((CoerceToDomain *) clause)->arg,
								varRelid,
								jointype,
								sjinfo);
	}
	else
	{
		/*
		 * For anything else, see if we can consider it as a boolean variable.
		 * This only works if it's an immutable expression in Vars of a single
		 * relation; but there's no point in us checking that here because
		 * boolvarsel() will do it internally, and return a suitable default
		 * selectivity if not.
		 */
		s1 = boolvarsel(root, clause, varRelid);
	}

	/* Cache the result if possible */
	if (cacheable)
	{
		if (jointype == JOIN_INNER)
			rinfo->norm_selec = s1;
		else
			rinfo->outer_selec = s1;
	}

#ifdef SELECTIVITY_DEBUG
	elog(DEBUG4, "clause_selectivity: s1 %f", s1);
#endif   /* SELECTIVITY_DEBUG */

	return s1;
}

/*
 * When applying functional dependencies, we start with the strongest ones
 * strongest dependencies. That is, we select the dependency that:
 *
 * (a) has all attributes covered by the clauses
 *
 * (b) has the most attributes
 *
 * (c) has the higher degree of validity
 *
 * TODO Explain why we select the dependencies this way.
 */
static MVDependency *
find_strongest_dependency(StatisticExtInfo *stats, MVDependencies *dependencies,
						  Bitmapset *attnums)
{
	int i;
	MVDependency *strongest = NULL;

	/* number of attnums in clauses */
	int nattnums = bms_num_members(attnums);

	/*
	 * Iterate over the MVDependency items and find the strongest one from
	 * the fully-matched dependencies. We do the cheap checks first, before
	 * matching it against the attnums.
	 */
	for (i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency   *dependency = dependencies->deps[i];

		/*
		 * Skip dependencies referencing more attributes than available clauses,
		 * as those can't be fully matched.
		 */
		if (dependency->nattributes > nattnums)
			continue;

		/* We can skip dependencies on fewer attributes than the best one. */
		if (strongest && (strongest->nattributes > dependency->nattributes))
			continue;

		/* And also weaker dependencies on the same number of attributes. */
		if (strongest &&
			(strongest->nattributes == dependency->nattributes) &&
			(strongest->degree > dependency->degree))
			continue;

		/*
		 * Check if the depdendency is full matched to the attnums. If so we
		 * can save it as the strongest match, since we rejected any weaker
		 * matches above.
		 */
		if (dependency_is_fully_matched(dependency, attnums))
			strongest = dependency;
	}

	return strongest;
}

/*
 * clauselist_ext_selectivity_deps
 *		estimate selectivity using functional dependencies
 *
 * Given equality clauses on attributes (a,b) we find the strongest dependency
 * between them, i.e. either (a=>b) or (b=>a). Assuming (a=>b) is the selected
 * dependency, we then combine the per-clause selectivities using the formula
 *
 *     P(a,b) = P(a) * [f + (1-f)*P(b)]
 *
 * where 'f' is the degree of the dependency.
 *
 * With clauses on more than two attributes, the dependencies are applied
 * recursively, starting with the widest/strongest dependencies. For example
 * P(a,b,c) is first split like this:
 *
 *     P(a,b,c) = P(a,b) * [f + (1-f)*P(c)]
 *
 * assuming (a,b=>c) is the strongest dependency.
 */
static Selectivity
clauselist_ext_selectivity_deps(PlannerInfo *root, Index relid,
								List *clauses, StatisticExtInfo *stats,
								Index varRelid, JoinType jointype,
								SpecialJoinInfo *sjinfo)
{
	ListCell	   *lc;
	Selectivity		s1 = 1.0;
	MVDependencies *dependencies;

	Assert(stats->kind == STATS_EXT_DEPENDENCIES);

	/* load the dependency items stored in the statistics */
	dependencies = staext_dependencies_load(stats->statOid);

	Assert(dependencies);

	/*
	 * Apply the dependencies recursively, starting with the widest/strongest
	 * ones, and proceeding to the smaller/weaker ones. At the end of each
	 * round we factor in the selectivity of clauses on the implied attribute,
	 * and remove the clauses from the list.
	 */
	while (true)
	{
		Selectivity		s2 = 1.0;
		Bitmapset	   *attnums;
		MVDependency   *dependency;

		/* clauses remaining after removing those on the "implied" attribute */
		List		   *clauses_filtered = NIL;

		attnums = collect_ext_attnums(clauses, relid);

		/* no point in looking for dependencies with fewer than 2 attributes */
		if (bms_num_members(attnums) < 2)
			break;

		/* the widest/strongest dependency, fully matched by clauses */
		dependency = find_strongest_dependency(stats, dependencies, attnums);

		/* if no suitable dependency was found, we're done */
		if (!dependency)
			break;

		/*
		 * We found an applicable dependency, so find all the clauses on the
		 * implied attribute, so with dependency (a,b => c) we search for
		 * clauses on 'c'. We only really expect a single such clause, but in
		 * case there are more we simply multiply the selectivities as usual.
		 *
		 * XXX Maybe we should use the maximum, minimum or just error out?
		 */
		foreach(lc, clauses)
		{
			AttrNumber	attnum_clause = InvalidAttrNumber;
			Node	   *clause = (Node *) lfirst(lc);

			/*
			 * XXX We need the attnum referenced by the clause, and this is the
			 * easiest way to get it (but maybe not the best one). At this point
			 * we should only see equality clauses compatible with functional
			 * dependencies, so just error out if we stumble upon something else.
			 */
			if (!clause_is_ext_compatible(clause, relid, &attnum_clause))
				elog(ERROR, "clause not compatible with functional dependencies");

			Assert(AttributeNumberIsValid(attnum_clause));

			/*
			 * If the clause is not on the implied attribute, add it to the list
			 * of filtered clauses (for the next round) and continue with the
			 * next one.
			 */
			if (!dependency_implies_attribute(dependency, attnum_clause))
			{
				clauses_filtered = lappend(clauses_filtered, clause);
				continue;
			}

			/*
			 * Otherwise compute selectivity of the clause, and multiply it with
			 * other clauses on the same attribute.
			 *
			 * XXX Not sure if we need to worry about multiple clauses, though.
			 * Those are all equality clauses, and if they reference different
			 * constants, that's not going to work.
			 */
			s2 *= clause_selectivity(root, clause, varRelid, jointype, sjinfo);
		}

		/*
		 * Now factor in the selectivity for all the "implied" clauses into the
		 * final one, using this formula:
		 *
		 *     P(a,b) = P(a) * (f + (1-f) * P(b))
		 *
		 * where 'f' is the degree of validity of the dependency.
		*/
		s1 *= (dependency->degree + (1 - dependency->degree) * s2);

		/* And only keep the filtered clauses for the next round. */
		clauses = clauses_filtered;
	}

	/* And now simply multiply with selectivities of the remaining clauses. */
	foreach (lc, clauses)
	{
		Node   *clause = (Node *) lfirst(lc);

		s1 *= clause_selectivity(root, clause, varRelid, jointype, sjinfo);
	}

	return s1;
}

/*
 * Collect attributes from mv-compatible clauses.
 */
static Bitmapset *
collect_ext_attnums(List *clauses, Index relid)
{
	Bitmapset  *attnums = NULL;
	ListCell   *l;

	/*
	 * Walk through the clauses and identify the ones we can estimate using
	 * extended stats, and remember the relid/columns. We'll then
	 * cross-check if we have suitable stats, and only if needed we'll split
	 * the clauses into extended and regular lists.
	 *
	 * For now we're only interested in RestrictInfo nodes with nested OpExpr,
	 * using either a range or equality.
	 */
	foreach(l, clauses)
	{
		AttrNumber	attnum;
		Node	   *clause = (Node *) lfirst(l);

		/* ignore the result for now - we only need the info */
		if (clause_is_ext_compatible(clause, relid, &attnum))
			attnums = bms_add_member(attnums, attnum);
	}

	/*
	 * If there are not at least two attributes referenced by the clause(s),
	 * we can throw everything out (as we'll revert to simple stats).
	 */
	if (bms_num_members(attnums) <= 1)
	{
		bms_free(attnums);
		return NULL;
	}

	return attnums;
}

/*
 * Count the number of attributes in clauses compatible with extended stats.
 */
static int
count_ext_attnums(List *clauses, Index relid)
{
	int			c;
	Bitmapset  *attnums = collect_ext_attnums(clauses, relid);

	c = bms_num_members(attnums);

	bms_free(attnums);

	return c;
}

/*
 * get_singleton_varno
 *		Returns true if clauses list contains only references to a single
 *		varno and sets 'relid' to that Varno, otherwise returns false.
 */
static bool
get_singleton_varno(List *clauses, Index *relid)
{
	Bitmapset  *varnos;
	int rel;

	varnos = pull_varnos((Node *) clauses);

	if (bms_get_singleton_member(varnos, &rel))
	{
		*relid = rel;
		bms_free(varnos);
		return true;
	}

	bms_free(varnos);

	return false;
}

static int
count_attnums_covered_by_stats(StatisticExtInfo *info, Bitmapset *attnums)
{
	Bitmapset *covered;

	covered = bms_intersect(attnums, info->keys);

	return bms_num_members(covered);
}

/*
 * We're looking for statistics matching at least 2 attributes, referenced in
 * clauses compatible with extended statistics. The current selection
 * criteria is very simple - we choose the statistics referencing the most
 * attributes.
 *
 * If there are multiple statistics referencing the same number of columns
 * (from the clauses), the one with less source columns (as listed in the
 * ADD STATISTICS when creating the statistics) wins. Else the first one wins.
 *
 * This is a very simple criteria, and has several weaknesses:
 *
 * (a) does not consider the accuracy of the statistics
 *
 *	   If there are two histograms built on the same set of columns, but one
 *	   has 100 buckets and the other one has 1000 buckets (thus likely
 *	   providing better estimates), this is not currently considered.
 *
 * (b) does not consider the type of statistics
 *
 *	   If there are three statistics - one containing just an MCV list,
 *	   and another one with just a histogram, and a third one with both,
 *	   we treat them equally.
 *
 * (c) does not consider the number of clauses
 *
 *	   As explained, only the number of referenced attributes counts, so if
 *	   there are multiple clauses on a single attribute, this still counts as
 *	   a single attribute.
 *
 * (d) does not consider type of condition
 *
 *	   Some clauses may work better with some statistics - for example equality
 *	   clauses probably work better with MCV lists than with histograms. But
 *	   IS [NOT] NULL conditions may often work better with histograms (thanks
 *	   to NULL-buckets).
 *
 * So for example with five WHERE conditions
 *
 *	   WHERE (a = 1) AND (b = 1) AND (c = 1) AND (d = 1) AND (e = 1)
 *
 * and statistics on (a,b), (a,b,e) and (a,b,c,d), the last one will be selected
 * as it references the most columns.
 *
 * Once we have selected the extended statistics, we split the list of
 * clauses into two parts - conditions that are compatible with the selected
 * stats, and conditions are estimated using simple statistics.
 *
 * From the example above, conditions
 *
 *	   (a = 1) AND (b = 1) AND (c = 1) AND (d = 1)
 *
 * will be estimated using the extended statistics (a,b,c,d) while the last
 * condition (e = 1) will get estimated using the regular ones.
 *
 * There are various alternative selection criteria (e.g. counting conditions
 * instead of just referenced attributes), but eventually the best option should
 * be to combine multiple statistics. But that's much harder to do correctly.
 *
 * TODO: Select multiple statistics and combine them when computing the estimate.
 *
 * TODO: This will probably have to consider compatibility of clauses, because
 * 'dependencies' will probably work only with equality clauses.
 */
static StatisticExtInfo *
choose_ext_statistics(List *stats, Bitmapset *attnums, char requiredkind)
{
	ListCell   *lc;
	StatisticExtInfo *choice = NULL;
	int			current_matches = 2;	/* goal #1: maximize */
	int			current_dims = (STATS_MAX_DIMENSIONS + 1);	/* goal #2: minimize */

	foreach(lc, stats)
	{
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		int			matches;
		int			numattrs;

		/* skip statistics not matching any of the requested types */
		if (info->kind != requiredkind)
			continue;

		/* determine how many attributes of these stats can be matched to */
		matches = count_attnums_covered_by_stats(info, attnums);

		/*
		 * save the actual number of keys in the stats so that we can choose
		 * the narrowest stats with the most matching keys.
		 */
		numattrs = bms_num_members(info->keys);

		/*
		 * Use these statistics when it increases the number of matched clauses
		 * or when it matches the same number of attributes but these stats
		 * have fewer keys than any previous match.
		 */
		if (matches > current_matches ||
			(matches == current_matches && current_dims > numattrs))
		{
			choice = info;
			current_matches = matches;
			current_dims = numattrs;
		}
	}

	return choice;
}


/*
 * clauselist_ext_split
 *		split the clause list into a part to be estimated using the provided
 *		statistics, and remaining clauses (estimated in some other way)
 */
static List *
clauselist_ext_split(PlannerInfo *root, Index relid,
					List *clauses, List **mvclauses,
					StatisticExtInfo *stats)
{
	ListCell   *l;
	List	   *non_mvclauses = NIL;

	/* erase the list of mv-compatible clauses */
	*mvclauses = NIL;

	foreach(l, clauses)
	{
		bool		match = false;		/* by default not mv-compatible */
		AttrNumber	attnum = InvalidAttrNumber;
		Node	   *clause = (Node *) lfirst(l);

		if (clause_is_ext_compatible(clause, relid, &attnum))
		{
			/* are all the attributes part of the selected stats? */
			if (bms_is_member(attnum, stats->keys))
				match = true;
		}

		/*
		 * The clause matches the selected stats, so put it to the list of
		 * mv-compatible clauses. Otherwise, keep it in the list of 'regular'
		 * clauses (that may be selected later).
		 */
		if (match)
			*mvclauses = lappend(*mvclauses, clause);
		else
			non_mvclauses = lappend(non_mvclauses, clause);
	}

	/*
	 * Perform regular estimation using the clauses incompatible with the
	 * chosen histogram (or MV stats in general).
	 */
	return non_mvclauses;

}

typedef struct
{
	Index		varno;			/* relid we're interested in */
	Bitmapset  *varattnos;		/* attnums referenced by the clauses */
} mv_compatible_context;

/*
 * Recursive walker that checks compatibility of the clause with extended
 * statistics, and collects attnums from the Vars.
 *
 * XXX The original idea was to combine this with expression_tree_walker, but
 *	   I've been unable to make that work - seems that does not quite allow
 *	   checking the structure. Hence the explicit calls to the walker.
 */
static bool
mv_compatible_walker(Node *node, mv_compatible_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		/* Pseudoconstants are not really interesting here. */
		if (rinfo->pseudoconstant)
			return true;

		/* clauses referencing multiple varnos are incompatible */
		if (bms_membership(rinfo->clause_relids) != BMS_SINGLETON)
			return true;

		/* check the clause inside the RestrictInfo */
		return mv_compatible_walker((Node *) rinfo->clause, (void *) context);
	}

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/*
		 * Also, the variable needs to reference the right relid (this might
		 * be unnecessary given the other checks, but let's be sure).
		 */
		if (var->varno != context->varno)
			return true;

		/* Also skip system attributes (we don't allow stats on those). */
		if (!AttrNumberIsForUserDefinedAttr(var->varattno))
			return true;

		/* Seems fine, so let's remember the attnum. */
		context->varattnos = bms_add_member(context->varattnos, var->varattno);

		return false;
	}

	/*
	 * And finally the operator expressions - we only allow simple expressions
	 * with two arguments, where one is a Var and the other is a constant, and
	 * it's a simple comparison (which we detect using estimator function).
	 */
	if (is_opclause(node))
	{
		OpExpr	   *expr = (OpExpr *) node;
		Var		   *var;
		bool		varonleft = true;
		bool		ok;

		/* Only expressions with two arguments are considered compatible. */
		if (list_length(expr->args) != 2)
			return true;

		/* see if it actually has the right */
		ok = (NumRelids((Node *) expr) == 1) &&
			(is_pseudo_constant_clause(lsecond(expr->args)) ||
			 (varonleft = false,
			  is_pseudo_constant_clause(linitial(expr->args))));

		/* unsupported structure (two variables or so) */
		if (!ok)
			return true;

		/*
		 * If it's not a "<" or ">" or "=" operator, just ignore the clause.
		 * Otherwise note the relid and attnum for the variable. This uses the
		 * function for estimating selectivity, ont the operator directly (a
		 * bit awkward, but well ...).
		 */
		switch (get_oprrest(expr->opno))
		{
			case F_EQSEL:

				/* equality conditions are compatible with all statistics */
				break;

			default:

				/* unknown estimator */
				return true;
		}

		var = (varonleft) ? linitial(expr->args) : lsecond(expr->args);

		return mv_compatible_walker((Node *) var, context);
	}

	/* Node not explicitly supported, so terminate */
	return true;
}

/*
 * Determines whether the clause is compatible with extended stats,
 * and if it is, returns some additional information - varno (index
 * into simple_rte_array) and a bitmap of attributes. This is then
 * used to fetch related extended statistics.
 *
 * At this moment we only support basic conditions of the form
 *
 *	   variable OP constant
 *
 * where OP is one of [=,<,<=,>=,>] (which is however determined by
 * looking at the associated function for estimating selectivity, just
 * like with the single-dimensional case).
 *
 * TODO: Support 'OR clauses' - shouldn't be all that difficult to
 * evaluate them using extended stats.
 */
static bool
clause_is_ext_compatible(Node *clause, Index relid, AttrNumber *attnum)
{
	mv_compatible_context context;

	context.varno = relid;
	context.varattnos = NULL;	/* no attnums */

	if (mv_compatible_walker(clause, (void *) &context))
		return false;

	/* remember the newly collected attnums */
	*attnum = bms_singleton_member(context.varattnos);

	return true;
}

/*
 * Check for any stats with the required kind
 */
static bool
has_stats(List *stats, char requiredkind)
{
	ListCell   *s;

	foreach(s, stats)
	{
		StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(s);

		if (stat->kind == requiredkind)
			return true;
	}

	return false;
}
