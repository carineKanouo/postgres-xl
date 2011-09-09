/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/createplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/skey.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/predtest.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#include "pgxc/planner.h"
#include "pgxc/postgresql_fdw.h"
#include "access/sysattr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#ifdef XCP
#include "catalog/pg_aggregate.h"
#include "parser/parse_coerce.h"
#else
#include "rewrite/rewriteManip.h"
#endif /* XCP */
#include "commands/tablecmds.h"
#endif /* PGXC */
#include "utils/lsyscache.h"


static Plan *create_plan_recurse(PlannerInfo *root, Path *best_path);
static Plan *create_scan_plan(PlannerInfo *root, Path *best_path);
static List *build_relation_tlist(RelOptInfo *rel);
static bool use_physical_tlist(PlannerInfo *root, RelOptInfo *rel);
static void disuse_physical_tlist(Plan *plan, Path *path);
static Plan *create_gating_plan(PlannerInfo *root, Plan *plan, List *quals);
static Plan *create_join_plan(PlannerInfo *root, JoinPath *best_path);
static Plan *create_append_plan(PlannerInfo *root, AppendPath *best_path);
static Plan *create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path);
static Result *create_result_plan(PlannerInfo *root, ResultPath *best_path);
static Material *create_material_plan(PlannerInfo *root, MaterialPath *best_path);
static Plan *create_unique_plan(PlannerInfo *root, UniquePath *best_path);
#ifdef XCP
static RemoteSubplan *create_remotescan_plan(PlannerInfo *root,
					   RemoteSubPath *best_path);
#endif
static SeqScan *create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses);
static IndexScan *create_indexscan_plan(PlannerInfo *root, IndexPath *best_path,
					  List *tlist, List *scan_clauses);
static BitmapHeapScan *create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist, List *scan_clauses);
static Plan *create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual);
static TidScan *create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses);
static ValuesScan *create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses);
static CteScan *create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses);
static WorkTableScan *create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses);
#ifdef PGXC
#ifndef XCP
static RemoteQuery *create_remotequery_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses);
static Plan *create_remotejoin_plan(PlannerInfo *root, JoinPath *best_path,
					Plan *parent, Plan *outer_plan, Plan *inner_plan);
static void create_remote_target_list(PlannerInfo *root,
					StringInfo targets, List *out_tlist, List *in_tlist,
					char *out_alias, int out_index,
					char *in_alias, int in_index);
static Alias *generate_remote_rte_alias(RangeTblEntry *rte, int varno,
					char *aliasname, int reduce_level);
static void pgxc_locate_grouping_columns(PlannerInfo *root, List *tlist,
											AttrNumber *grpColIdx);
static List *pgxc_process_grouping_targetlist(PlannerInfo *root,
												List **local_tlist);
static List *pgxc_process_having_clause(PlannerInfo *root, List *remote_tlist,
												Node *havingQual, List **local_qual,
												List **remote_qual, bool *reduce_plan);
#endif /* PGXC */
#endif /* XCP */
static ForeignScan *create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(PlannerInfo *root, NestPath *best_path,
					 Plan *outer_plan, Plan *inner_plan);
static MergeJoin *create_mergejoin_plan(PlannerInfo *root, MergePath *best_path,
					  Plan *outer_plan, Plan *inner_plan);
static HashJoin *create_hashjoin_plan(PlannerInfo *root, HashPath *best_path,
					 Plan *outer_plan, Plan *inner_plan);
static Node *replace_nestloop_params(PlannerInfo *root, Node *expr);
static Node *replace_nestloop_params_mutator(Node *node, PlannerInfo *root);
static List *fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
						 List *indexquals);
static List *fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path,
							List *indexorderbys);
static Node *fix_indexqual_operand(Node *node, IndexOptInfo *index);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(PlannerInfo *root, List *clauses);
static void copy_path_costsize(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   Oid indexid, List *indexqual, List *indexqualorig,
			   List *indexorderby, List *indexorderbyorig,
			   ScanDirection indexscandir);
static BitmapIndexScan *make_bitmap_indexscan(Index scanrelid, Oid indexid,
					  List *indexqual,
					  List *indexqualorig);
static BitmapHeapScan *make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
			 List *tidquals);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
				  Index scanrelid, Node *funcexpr, List *funccolnames,
				  List *funccoltypes, List *funccoltypmods,
				  List *funccolcollations);
static ValuesScan *make_valuesscan(List *qptlist, List *qpqual,
				Index scanrelid, List *values_lists);
static CteScan *make_ctescan(List *qptlist, List *qpqual,
			 Index scanrelid, int ctePlanId, int cteParam);
static WorkTableScan *make_worktablescan(List *qptlist, List *qpqual,
				   Index scanrelid, int wtParam);
#ifdef PGXC
#ifndef XCP
static RemoteQuery *make_remotequery(List *qptlist, RangeTblEntry *rte,
				   List *qpqual, Index scanrelid);
#endif
#endif
static ForeignScan *make_foreignscan(List *qptlist, List *qpqual,
				 Index scanrelid, bool fsSystemCol, FdwPlan *fdwplan);
static BitmapAnd *make_bitmap_and(List *bitmapplans);
static BitmapOr *make_bitmap_or(List *bitmapplans);
static NestLoop *make_nestloop(List *tlist,
			  List *joinclauses, List *otherclauses, List *nestParams,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static HashJoin *make_hashjoin(List *tlist,
			  List *joinclauses, List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static Hash *make_hash(Plan *lefttree,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit,
		  Oid skewColType,
		  int32 skewColTypmod);
static MergeJoin *make_mergejoin(List *tlist,
			   List *joinclauses, List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree, Plan *righttree,
			   JoinType jointype);
static Sort *make_sort(PlannerInfo *root, Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst,
		  double limit_tuples);
static Plan *prepare_sort_from_pathkeys(PlannerInfo *root,
						   Plan *lefttree, List *pathkeys,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst);
static Material *make_material(Plan *lefttree);

#ifdef PGXC
#ifndef XCP
static void findReferencedVars(List *parent_vars, Plan *plan, List **out_tlist, Relids *out_relids);
static void create_remote_clause_expr(PlannerInfo *root, Plan *parent, StringInfo clauses,
	  List *qual, RemoteQuery *scan);
static void create_remote_expr(PlannerInfo *root, Plan *parent, StringInfo expr,
	  Node *node, RemoteQuery *scan);
#endif /* XCP */
#endif /* PGXC */

#ifdef XCP
static int add_sort_column(AttrNumber colIdx, Oid sortOp, Oid coll,
				bool nulls_first,int numCols, AttrNumber *sortColIdx,
				Oid *sortOperators, Oid *collations, bool *nullsFirst);
#endif

/*
 * create_plan
 *	  Creates the access plan for a query by recursively processing the
 *	  desired tree of pathnodes, starting at the node 'best_path'.	For
 *	  every pathnode found, we create a corresponding plan node containing
 *	  appropriate id, target list, and qualification information.
 *
 *	  The tlists and quals in the plan tree are still in planner format,
 *	  ie, Vars still correspond to the parser's numbering.  This will be
 *	  fixed later by setrefs.c.
 *
 *	  best_path is the best access path
 *
 *	  Returns a Plan tree.
 */
Plan *
create_plan(PlannerInfo *root, Path *best_path)
{
	Plan	   *plan;

	/* Initialize this module's private workspace in PlannerInfo */
	root->curOuterRels = NULL;
	root->curOuterParams = NIL;

	/* Recursively process the path tree */
	plan = create_plan_recurse(root, best_path);

	/* Check we successfully assigned all NestLoopParams to plan nodes */
	if (root->curOuterParams != NIL)
		elog(ERROR, "failed to assign all NestLoopParams to plan nodes");

	return plan;
}

/*
 * create_plan_recurse
 *	  Recursive guts of create_plan().
 */
static Plan *
create_plan_recurse(PlannerInfo *root, Path *best_path)
{
	Plan	   *plan;

	switch (best_path->pathtype)
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_ForeignScan:
#ifdef PGXC
#ifndef XCP
		case T_RemoteQuery:
#endif /* XCP */
#endif /* PGXC */
			plan = create_scan_plan(root, best_path);
			break;
#ifdef XCP
		case T_RemoteSubplan:
			plan = (Plan *) create_remotescan_plan(root,
												   (RemoteSubPath *) best_path);
			break;
#endif
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = create_join_plan(root,
									(JoinPath *) best_path);
			break;
		case T_Append:
			plan = create_append_plan(root,
									  (AppendPath *) best_path);
			break;
		case T_MergeAppend:
			plan = create_merge_append_plan(root,
											(MergeAppendPath *) best_path);
			break;
		case T_Result:
			plan = (Plan *) create_result_plan(root,
											   (ResultPath *) best_path);
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path);
			break;
		case T_Unique:
			plan = create_unique_plan(root,
									  (UniquePath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	return plan;
}

/*
 * create_scan_plan
 *	 Create a scan plan for the parent relation of 'best_path'.
 */
static Plan *
create_scan_plan(PlannerInfo *root, Path *best_path)
{
	RelOptInfo *rel = best_path->parent;
	List	   *tlist;
	List	   *scan_clauses;
	Plan	   *plan;

	/*
	 * For table scans, rather than using the relation targetlist (which is
	 * only those Vars actually needed by the query), we prefer to generate a
	 * tlist containing all Vars in order.	This will allow the executor to
	 * optimize away projection of the table tuples, if possible.  (Note that
	 * planner.c may replace the tlist we generate here, forcing projection to
	 * occur.)
	 */
	if (use_physical_tlist(root, rel))
	{
		tlist = build_physical_tlist(root, rel);
		/* if fail because of dropped cols, use regular method */
		if (tlist == NIL)
			tlist = build_relation_tlist(rel);
	}
	else
		tlist = build_relation_tlist(rel);

	/*
	 * Extract the relevant restriction clauses from the parent relation. The
	 * executor must apply all these restrictions during the scan, except for
	 * pseudoconstants which we'll take care of below.
	 */
	scan_clauses = rel->baserestrictinfo;

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Plan *) create_seqscan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_IndexScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses);
			break;

		case T_BitmapHeapScan:
			plan = (Plan *) create_bitmap_scan_plan(root,
												(BitmapHeapPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_TidScan:
			plan = (Plan *) create_tidscan_plan(root,
												(TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Plan *) create_subqueryscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Plan *) create_functionscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_ValuesScan:
			plan = (Plan *) create_valuesscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_CteScan:
			plan = (Plan *) create_ctescan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_WorkTableScan:
			plan = (Plan *) create_worktablescan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

#ifdef PGXC
#ifndef XCP
		case T_RemoteQuery:
			plan = (Plan *) create_remotequery_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;
#endif /* XCP */
#endif /* PGXC */

		case T_ForeignScan:
			plan = (Plan *) create_foreignscan_plan(root,
													(ForeignPath *) best_path,
													tlist,
													scan_clauses);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * If there are any pseudoconstant clauses attached to this node, insert a
	 * gating Result node that evaluates the pseudoconstants as one-time
	 * quals.
	 */
	if (root->hasPseudoConstantQuals)
		plan = create_gating_plan(root, plan, scan_clauses);

	return plan;
}

/*
 * Build a target list (ie, a list of TargetEntry) for a relation.
 */
static List *
build_relation_tlist(RelOptInfo *rel)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *v;

	foreach(v, rel->reltargetlist)
	{
		/* Do we really need to copy here?	Not sure */
		Node	   *node = (Node *) copyObject(lfirst(v));

		tlist = lappend(tlist, makeTargetEntry((Expr *) node,
											   resno,
											   NULL,
											   false));
		resno++;
	}
	return tlist;
}

/*
 * use_physical_tlist
 *		Decide whether to use a tlist matching relation structure,
 *		rather than only those Vars actually referenced.
 */
static bool
use_physical_tlist(PlannerInfo *root, RelOptInfo *rel)
{
	int			i;
	ListCell   *lc;

	/*
	 * We can do this for real relation scans, subquery scans, function scans,
	 * values scans, and CTE scans (but not for, eg, joins).
	 */
	if (rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY &&
		rel->rtekind != RTE_FUNCTION &&
		rel->rtekind != RTE_VALUES &&
		rel->rtekind != RTE_CTE)
		return false;

	/*
	 * Can't do it with inheritance cases either (mainly because Append
	 * doesn't project).
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;

	/*
	 * Can't do it if any system columns or whole-row Vars are requested.
	 * (This could possibly be fixed but would take some fragile assumptions
	 * in setrefs.c, I think.)
	 */
	for (i = rel->min_attr; i <= 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
			return false;
	}

	/*
	 * Can't do it if the rel is required to emit any placeholder expressions,
	 * either.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (bms_nonempty_difference(phinfo->ph_needed, rel->relids) &&
			bms_is_subset(phinfo->ph_eval_at, rel->relids))
			return false;
	}

	return true;
}

/*
 * disuse_physical_tlist
 *		Switch a plan node back to emitting only Vars actually referenced.
 *
 * If the plan node immediately above a scan would prefer to get only
 * needed Vars and not a physical tlist, it must call this routine to
 * undo the decision made by use_physical_tlist().	Currently, Hash, Sort,
 * and Material nodes want this, so they don't have to store useless columns.
 */
static void
disuse_physical_tlist(Plan *plan, Path *path)
{
	/* Only need to undo it for path types handled by create_scan_plan() */
	switch (path->pathtype)
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_ForeignScan:
			plan->targetlist = build_relation_tlist(path->parent);
			break;
		default:
			break;
	}
}

/*
 * create_gating_plan
 *	  Deal with pseudoconstant qual clauses
 *
 * If the node's quals list includes any pseudoconstant quals, put them
 * into a gating Result node atop the already-built plan.  Otherwise,
 * return the plan as-is.
 *
 * Note that we don't change cost or size estimates when doing gating.
 * The costs of qual eval were already folded into the plan's startup cost.
 * Leaving the size alone amounts to assuming that the gating qual will
 * succeed, which is the conservative estimate for planning upper queries.
 * We certainly don't want to assume the output size is zero (unless the
 * gating qual is actually constant FALSE, and that case is dealt with in
 * clausesel.c).  Interpolating between the two cases is silly, because
 * it doesn't reflect what will really happen at runtime, and besides which
 * in most cases we have only a very bad idea of the probability of the gating
 * qual being true.
 */
static Plan *
create_gating_plan(PlannerInfo *root, Plan *plan, List *quals)
{
	List	   *pseudoconstants;

	/* Sort into desirable execution order while still in RestrictInfo form */
	quals = order_qual_clauses(root, quals);

	/* Pull out any pseudoconstant quals from the RestrictInfo list */
	pseudoconstants = extract_actual_clauses(quals, true);

	if (!pseudoconstants)
		return plan;

	return (Plan *) make_result(root,
								plan->targetlist,
								(Node *) pseudoconstants,
								plan);
}

/*
 * create_join_plan
 *	  Create a join plan for 'best_path' and (recursively) plans for its
 *	  inner and outer paths.
 */
static Plan *
create_join_plan(PlannerInfo *root, JoinPath *best_path)
{
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	Plan	   *plan;
	Relids		saveOuterRels = root->curOuterRels;

	outer_plan = create_plan_recurse(root, best_path->outerjoinpath);

	/* For a nestloop, include outer relids in curOuterRels for inner side */
	if (best_path->path.pathtype == T_NestLoop)
		root->curOuterRels = bms_union(root->curOuterRels,
								   best_path->outerjoinpath->parent->relids);

	inner_plan = create_plan_recurse(root, best_path->innerjoinpath);

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Plan *) create_mergejoin_plan(root,
												  (MergePath *) best_path,
												  outer_plan,
												  inner_plan);
			break;
		case T_HashJoin:
			plan = (Plan *) create_hashjoin_plan(root,
												 (HashPath *) best_path,
												 outer_plan,
												 inner_plan);
			break;
		case T_NestLoop:
			/* Restore curOuterRels */
			bms_free(root->curOuterRels);
			root->curOuterRels = saveOuterRels;

			plan = (Plan *) create_nestloop_plan(root,
												 (NestPath *) best_path,
												 outer_plan,
												 inner_plan);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->path.pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * If there are any pseudoconstant clauses attached to this node, insert a
	 * gating Result node that evaluates the pseudoconstants as one-time
	 * quals.
	 */
	if (root->hasPseudoConstantQuals)
		plan = create_gating_plan(root, plan, best_path->joinrestrictinfo);

#ifdef NOT_USED

	/*
	 * * Expensive function pullups may have pulled local predicates * into
	 * this path node.	Put them in the qpqual of the plan node. * JMH,
	 * 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   list_concat(get_qpqual((Plan) plan),
					   get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

#ifdef PGXC
#ifndef XCP
	/*
	 * Check if this join can be reduced to an equiv. remote scan node
	 * This can only be executed on a remote Coordinator
	 */
	if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
		plan = create_remotejoin_plan(root, best_path, plan, outer_plan, inner_plan);
#endif /* XCP */
#endif /* PGXC */

	return plan;
}


#ifdef PGXC
#ifndef XCP
/*
 * create_remotejoin_plan
 * 	check if the children plans involve remote entities from the same remote
 * 	node. If so, this join can be reduced to an equivalent remote scan plan
 * 	node
 *
 * 	RULES:
 *
 * * provide unique aliases to both inner and outer nodes to represent their
 *   corresponding subqueries
 *
 * * identify target entries from both inner and outer that appear in the join
 *   targetlist, only those need to be selected from these aliased subqueries
 *
 * * a join node has a joinqual list which represents the join condition. E.g.
 *   SELECT * from emp e LEFT JOIN emp2 d ON e.x = d.x
 *   Here the joinqual contains "e.x = d.x". If the joinqual itself has a local
 *   dependency, e.g "e.x = localfunc(d.x)", then this join cannot be reduced
 *
 * * other than the joinqual, the join node can contain additional quals. Even
 *   if they have any local dependencies, we can reduce the join and just
 *   append these quals into the reduced remote scan node. We DO do a pass to
 *   identify remote quals and ship those in the squery though
 *
 * * these quals (both joinqual and normal quals with no local dependencies)
 *   need to be converted into expressions referring to the aliases assigned to
 *   the nodes. These expressions will eventually become part of the squery of
 *   the reduced remote scan node
 *
 * * the children remote scan nodes themselves can have local dependencies in
 *   their quals (the remote ones are already part of the squery). We can still
 *   reduce the join and just append these quals into the reduced remote scan
 *   node
 *
 * * if we reached successfully so far, generate a new remote scan node with
 *   this new squery generated using the aliased references
 *
 * One important point to note here about targetlists is that this function
 * does not set any DUMMY var references in the Var nodes appearing in it. It
 * follows the standard mechanism as is followed by other nodes. Similar to the
 * existing nodes, the references which point to DUMMY vars is done in
 * set_remote_references() function in set_plan_references phase at the fag
 * end. Avoiding such DUMMY references manipulations till the end also makes
 * this code a lot much readable and easier.
 */
static Plan *
create_remotejoin_plan(PlannerInfo *root, JoinPath *best_path, Plan *parent, Plan *outer_plan, Plan *inner_plan)
{
	NestLoop   *nest_parent;
	JoinReduceInfo  join_info;
	RemoteQuery	*outer = NULL;
	RemoteQuery	*inner = NULL;

	if (!enable_remotejoin)
		return parent;

	/* meh, what are these for :( */
	if (root->hasPseudoConstantQuals)
		return parent;

	/* do not optimize CURSOR based select statements */
	if (root->parse->rowMarks != NIL)
		return parent;

	/*
	 * optimize only simple NestLoop joins for now. Other joins like Merge and
	 * Hash can be reduced too. But they involve additional intermediate nodes
	 * and we need to understand them a bit more as yet
	 */
	if (!IsA(parent, NestLoop))
		return parent;
	else
		nest_parent = (NestLoop *)parent;

	/*
	 * Now RemoteQuery subnode is behind Matherial but this may be changed later
	 */
	if (IsA(outer_plan, Material) && IsA(outer_plan->lefttree, RemoteQuery))
		outer = (RemoteQuery *) outer_plan->lefttree;
	else if (IsA(outer_plan, RemoteQuery))
		outer = (RemoteQuery *) outer_plan;

	if (IsA(inner_plan, Material) && IsA(inner_plan->lefttree, RemoteQuery))
		inner = (RemoteQuery *) inner_plan->lefttree;
	else if (IsA(inner_plan, RemoteQuery))
		inner = (RemoteQuery *) inner_plan;


	/* check if both the nodes qualify for reduction */
	if (outer && inner)
	{
		int i;
		List *rtable_list = NIL;

		/*
		 * Check if both these plans are from the same remote node. If yes,
		 * replace this JOIN along with it's two children with one equivalent
		 * remote node
		 */

		/*
		 * Build up rtable for XC Walker
		 * (was not sure I could trust this, but it seems to work in various cases)
		 */
		for (i = 0; i < root->simple_rel_array_size; i++)
		{
			RangeTblEntry *rte = root->simple_rte_array[i];

			/* Check for NULL first, sometimes it is NULL at position 0 */
			if (rte)
				rtable_list = lappend(rtable_list, root->simple_rte_array[i]);
		}

		/* XXX Check if the join optimization is possible */
		if (IsJoinReducible(inner, outer, rtable_list, best_path, &join_info))
		{
			RemoteQuery	   *result;
			Plan		   *result_plan;
			StringInfoData 	targets, clauses, scan_clauses, fromlist, join_condition;
			StringInfoData 	squery;
			List		   *parent_vars, *out_tlist = NIL, *in_tlist = NIL, *base_tlist;
			ListCell	   *l;
			char		    in_alias[15], out_alias[15];
			Relids			out_relids = NULL, in_relids = NULL;
			bool			use_where = false;
			Index			dummy_rtindex;
			RangeTblEntry  *dummy_rte;
			List	 	   *local_scan_clauses = NIL, *remote_scan_clauses = NIL;
			char		   *pname;


			/* KISS! As long as distinct aliases are provided for all the objects in
			 * involved in query, remote server should not crib!  */
			sprintf(in_alias,  "out_%d", root->rs_alias_index);
			sprintf(out_alias, "in_%d",  root->rs_alias_index);

			/*
			 * Walk the left, right trees and identify which vars appear in the
			 * parent targetlist, only those need to be selected. Note that
			 * depending on whether the parent targetlist is top-level or
			 * intermediate, the children vars may or may not be referenced
			 * multiple times in it.
			 */
			parent_vars = pull_var_clause((Node *)parent->targetlist, PVC_REJECT_PLACEHOLDERS);

			findReferencedVars(parent_vars, outer_plan, &out_tlist, &out_relids);
			findReferencedVars(parent_vars, inner_plan, &in_tlist, &in_relids);

			/*
			 * If the JOIN ON clause has a local dependency then we cannot ship
			 * the join to the remote side at all, bail out immediately.
			 */
			if (!is_foreign_expr((Node *)nest_parent->join.joinqual, NULL))
			{
				elog(DEBUG1, "cannot reduce: local dependencies in the joinqual");
				return parent;
			}

			/*
			 * If the normal plan qual has local dependencies, the join can
			 * still be shipped. Try harder to ship remote clauses out of the
			 * entire list. These local quals will become part of the quals
			 * list of the reduced remote scan node down later.
			 */
			if (!is_foreign_expr((Node *)nest_parent->join.plan.qual, NULL))
			{
				elog(DEBUG1, "local dependencies in the join plan qual");

				/*
				 * trawl through each entry and come up with remote and local
				 * clauses... sigh
				 */
				foreach(l, nest_parent->join.plan.qual)
				{
					Node *clause = lfirst(l);

					/*
					 * if the currentof in the above call to
					 * clause_is_local_bound is set, somewhere in the list there
					 * is currentof clause, so keep that information intact and
					 * pass a dummy argument here.
					 */
					if (!is_foreign_expr((Node *)clause, NULL))
						local_scan_clauses	= lappend(local_scan_clauses, clause);
					else
						remote_scan_clauses = lappend(remote_scan_clauses, clause);
				}
			}
			else
			{
				/*
				 * there is no local bound clause, all the clauses are remote
				 * scan clauses
				 */
				remote_scan_clauses = nest_parent->join.plan.qual;
			}

			/* generate the tlist for the new RemoteScan node using out_tlist, in_tlist */
			initStringInfo(&targets);
			create_remote_target_list(root, &targets, out_tlist, in_tlist,
						 out_alias, outer->reduce_level, in_alias, inner->reduce_level);

			/*
			 * generate the fromlist now. The code has to appropriately mention
			 * the JOIN type in the string being generated.
			 */
			initStringInfo(&fromlist);
			appendStringInfo(&fromlist, " (%s) %s ",
							 outer->sql_statement, quote_identifier(out_alias));

			use_where = false;
			switch (nest_parent->join.jointype)
			{
				case JOIN_INNER:
					pname	  = ", ";
					use_where = true;
					break;
				case JOIN_LEFT:
					pname	  = "LEFT JOIN";
					break;
				case JOIN_FULL:
					pname	  = "FULL JOIN";
					break;
				case JOIN_RIGHT:
					pname	  = "RIGHT JOIN";
					break;
				case JOIN_SEMI:
				case JOIN_ANTI:
				default:
					return parent;
			}

			/*
			 * splendid! we can actually replace this join hierarchy with a
			 * single RemoteScan node now. Start off by constructing the
			 * appropriate new tlist and tupdescriptor
			 */
			result = makeNode(RemoteQuery);

			/*
			 * Save various information about the inner and the outer plans. We
			 * may need this information later if more entries are added to it
			 * as part of the remote expression optimization
			 */
			result->remotejoin		   = true;
			result->inner_alias		   = pstrdup(in_alias);
			result->outer_alias		   = pstrdup(out_alias);
			result->inner_reduce_level = inner->reduce_level;
			result->outer_reduce_level = outer->reduce_level;
			result->inner_relids       = in_relids;
			result->outer_relids       = out_relids;
			result->inner_statement	   = pstrdup(inner->sql_statement);
			result->outer_statement	   = pstrdup(outer->sql_statement);
			result->join_condition	   = NULL;
			result->exec_nodes         = copyObject(join_info.exec_nodes);

			appendStringInfo(&fromlist, " %s (%s) %s",
							 pname, inner->sql_statement, quote_identifier(in_alias));

			/* generate join.joinqual remote clause string representation  */
			initStringInfo(&clauses);
			if (nest_parent->join.joinqual != NIL)
			{
				create_remote_clause_expr(root, parent, &clauses,
						nest_parent->join.joinqual, result);
			}

			/* generate join.plan.qual remote clause string representation  */
			initStringInfo(&scan_clauses);
			if (remote_scan_clauses != NIL)
			{
				create_remote_clause_expr(root, parent, &scan_clauses,
						remote_scan_clauses, result);
			}

			/*
			 * set the base tlist of the involved base relations, useful in
			 * set_plan_refs later. Additionally the tupledescs should be
			 * generated using this base_tlist and not the parent targetlist.
			 * This is because we want to take into account any additional
			 * column references from the scan clauses too
			 */
			base_tlist = add_to_flat_tlist(NIL, list_concat(out_tlist, in_tlist));

			/* cook up the reltupdesc using this base_tlist */
			dummy_rte = makeNode(RangeTblEntry);
			dummy_rte->reltupdesc = ExecTypeFromTL(base_tlist, false);
			dummy_rte->rtekind = RTE_RELATION;

			/* use a dummy relname... */
			dummy_rte->relname	   = "__FOREIGN_QUERY__";
			dummy_rte->eref		   = makeAlias("__FOREIGN_QUERY__", NIL);
			/* not sure if we need to set the below explicitly.. */
			dummy_rte->inh			 = false;
			dummy_rte->inFromCl		 = false;
			dummy_rte->requiredPerms = 0;
			dummy_rte->checkAsUser   = 0;
			dummy_rte->selectedCols  = NULL;
			dummy_rte->modifiedCols  = NULL;

			/*
			 * Append the dummy range table entry to the range table.
			 * Note that this modifies the master copy the caller passed us, otherwise
			 * e.g EXPLAIN VERBOSE will fail to find the rte the Vars built below refer
			 * to.
			 */
			root->parse->rtable = lappend(root->parse->rtable, dummy_rte);
			dummy_rtindex = list_length(root->parse->rtable);

			result_plan = &result->scan.plan;

			/* the join targetlist becomes this node's tlist */
			result_plan->targetlist = parent->targetlist;
			result_plan->lefttree 	= NULL;
			result_plan->righttree 	= NULL;
			result->scan.scanrelid 	= dummy_rtindex;

			/* generate the squery for this node */

			/* NOTE: it's assumed that the remote_paramNums array is
			 * filled in the same order as we create the query here.
			 *
			 * TODO: we need some way to ensure that the remote_paramNums
			 * is filled in the same order as the order in which the clauses
			 * are added in the query below.
			 */
			initStringInfo(&squery);
			appendStringInfo(&squery, "SELECT %s FROM %s", targets.data, fromlist.data);

			initStringInfo(&join_condition);
			if (clauses.data[0] != '\0')
				appendStringInfo(&join_condition, " %s %s", use_where? " WHERE " : " ON ", clauses.data);

			if (scan_clauses.data[0] != '\0')
				appendStringInfo(&join_condition, " %s %s", use_where? " AND " : " WHERE ", scan_clauses.data);

			if (join_condition.data[0] != '\0')
				appendStringInfoString(&squery, join_condition.data);

			result->sql_statement = squery.data;
			result->join_condition = join_condition.data;
			/* don't forget to increment the index for the next time around! */
			result->reduce_level = root->rs_alias_index++;


			/* set_plan_refs needs this later */
			result->base_tlist		= base_tlist;
			result->relname			= "__FOREIGN_QUERY__";
			result->partitioned_replicated = join_info.partitioned_replicated;

			/*
			 * if there were any local scan clauses stick them up here. They
			 * can come from the join node or from remote scan node themselves.
			 * Because of the processing being done earlier in
			 * create_remotescan_plan, all of the clauses if present will be
			 * local ones and hence can be stuck without checking for
			 * remoteness again here into result_plan->qual
			 */
			result_plan->qual = list_concat(result_plan->qual, outer_plan->qual);
			result_plan->qual = list_concat(result_plan->qual, inner_plan->qual);
			result_plan->qual = list_concat(result_plan->qual, local_scan_clauses);

			/* we actually need not worry about costs since this is the final plan */
			result_plan->startup_cost = outer_plan->startup_cost;
			result_plan->total_cost   = outer_plan->total_cost;
			result_plan->plan_rows 	  = outer_plan->plan_rows;
			result_plan->plan_width   = outer_plan->plan_width;

			return (Plan *) make_material(result_plan);
		}
	}

	return parent;
}

/*
 * Generate aliases for columns of remote tables using the
 * colname_varno_varattno_reduce_level nomenclature
 */
static Alias *
generate_remote_rte_alias(RangeTblEntry *rte, int varno, char *aliasname, int reduce_level)
{
	TupleDesc 	tupdesc;
	int			maxattrs;
	int			varattno;
	List	   *colnames = NIL;
	StringInfo	attr = makeStringInfo();

	if (rte->rtekind != RTE_RELATION)
		elog(ERROR, "called in improper context");

	if (reduce_level == 0)
		return makeAlias(aliasname, NIL);

	tupdesc  = rte->reltupdesc;
	maxattrs = tupdesc->natts;

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute  att = tupdesc->attrs[varattno];
		Value			  *attrname;

		resetStringInfo(attr);
		appendStringInfo(attr, "%s_%d_%d_%d",
						 NameStr(att->attname), varno, varattno + 1, reduce_level);

		attrname = makeString(pstrdup(attr->data));

		colnames = lappend(colnames, attrname);
	}

	return makeAlias(aliasname, colnames);
}

/* create_remote_target_list
 *  generate a targetlist using out_alias and in_alias appropriately. It is
 *  possible that in case of multiple-hierarchy reduction, both sides can have
 *  columns with the same name. E.g. consider the following:
 *
 *  select * from emp e join emp f on e.x = f.x, emp g;
 *
 *  So if we just use new_alias.columnname it can
 *  very easily clash with other columnname from the same side of an already
 *  reduced join. To avoid this, we generate unique column aliases using the
 *  following convention:
 *  	colname_varno_varattno_reduce_level_index
 *
 *  Each RemoteScan node carries it's reduce_level index to indicate the
 *  convention that should be adopted while referring to it's columns. If the
 *  level is 0, then normal column names can be used because they will never
 *  clash at the join level
 */
static void
create_remote_target_list(PlannerInfo *root, StringInfo targets, List *out_tlist, List *in_tlist,
				  char *out_alias, int out_index, char *in_alias, int in_index)
{
	int 	     i = 0;
	ListCell  	*l;
	StringInfo 	 attrname = makeStringInfo();
	bool		 add_null_target = true;

	foreach(l, out_tlist)
	{
		Var			  *var = (Var *) lfirst(l);
		RangeTblEntry *rte = planner_rt_fetch(var->varno, root);
		char		  *attname;


		if (i++ > 0)
			appendStringInfo(targets, ", ");

		attname = get_rte_attribute_name(rte, var->varattno);

		if (out_index)
		{
			resetStringInfo(attrname);
			/* varattno can be negative for sys attributes, hence the abs! */
			appendStringInfo(attrname, "%s_%d_%d_%d",
							 attname, var->varno, abs(var->varattno), out_index);
			appendStringInfo(targets, "%s.%s",
							 quote_identifier(out_alias), quote_identifier(attrname->data));
		}
		else
			appendStringInfo(targets, "%s.%s",
							 quote_identifier(out_alias), quote_identifier(attname));

		/* generate the new alias now using root->rs_alias_index */
		resetStringInfo(attrname);
		appendStringInfo(attrname, "%s_%d_%d_%d",
						 attname, var->varno, abs(var->varattno), root->rs_alias_index);
		appendStringInfo(targets, " AS %s", quote_identifier(attrname->data));
		add_null_target = false;
	}

	foreach(l, in_tlist)
	{
		Var			  *var = (Var *) lfirst(l);
		RangeTblEntry *rte = planner_rt_fetch(var->varno, root);
		char		  *attname;

		if (i++ > 0)
			appendStringInfo(targets, ", ");

		attname = get_rte_attribute_name(rte, var->varattno);

		if (in_index)
		{
			resetStringInfo(attrname);
			/* varattno can be negative for sys attributes, hence the abs! */
			appendStringInfo(attrname, "%s_%d_%d_%d",
							 attname, var->varno, abs(var->varattno), in_index);
			appendStringInfo(targets, "%s.%s",
							 quote_identifier(in_alias), quote_identifier(attrname->data));
		}
		else
			appendStringInfo(targets, "%s.%s",
							 quote_identifier(in_alias), quote_identifier(attname));

		/* generate the new alias now using root->rs_alias_index */
		resetStringInfo(attrname);
		appendStringInfo(attrname, "%s_%d_%d_%d",
						 attname, var->varno, abs(var->varattno), root->rs_alias_index);
		appendStringInfo(targets, " AS %s", quote_identifier(attrname->data));
		add_null_target = false;
	}

	/*
	 * It's possible that in some cases, the targetlist might not refer to any
	 * vars from the joined relations, eg.
	 * select count(*) from t1, t2; select const from t1, t2; etc
	 * For such cases just add a NULL selection into this targetlist
	 */
	if (add_null_target)
		appendStringInfo(targets, " NULL ");
}

/*
 * create_remote_clause_expr
 *  generate a string to represent the clause list expression using out_alias
 *  and in_alias references. This function does a cute hack by temporarily
 *  modifying the rte->eref entries of the involved relations to point to
 *  out_alias and in_alias appropriately. The deparse_expression call then
 *  generates a string using these erefs which is exactly what is desired here.
 *
 *  Additionally it creates aliases for the column references based on the
 *  reduce_level values too. This handles the case when both sides have same
 *  named columns..
 *
 *  Obviously this function restores the eref, alias values to their former selves
 *  appropriately too, after use
 */
static void
create_remote_clause_expr(PlannerInfo *root, Plan *parent, StringInfo clauses,
	  List *qual, RemoteQuery *scan)
{
	Node *node = (Node *) make_ands_explicit(qual);

	return create_remote_expr(root, parent, clauses, node, scan);
}

static void
create_remote_expr(PlannerInfo *root, Plan *parent, StringInfo expr,
	  Node *node, RemoteQuery *scan)
{
	List	 *context;
	List     *leref = NIL;
	ListCell *cell;
	char	 *exprstr;
	int 	  rtindex;
	Relids	  tmprelids, relids;

	relids = pull_varnos((Node *)node);

	tmprelids = bms_copy(relids);

	while ((rtindex = bms_first_member(tmprelids)) >= 0)
	{
		RangeTblEntry	*rte = planner_rt_fetch(rtindex, root);

		/*
		 * This rtindex should be a member of either out_relids or
		 * in_relids and never both
		 */
		if (bms_is_member(rtindex, scan->outer_relids) &&
			bms_is_member(rtindex, scan->inner_relids))
			elog(ERROR, "improper relid references in the join clause list");

		/*
		 * save the current rte->eref and rte->alias values and stick in a new
		 * one in the rte with the proper inner or outer alias
		 */
		leref = lappend(leref, rte->eref);
		leref = lappend(leref, rte->alias);

		if (bms_is_member(rtindex, scan->outer_relids))
		{
			rte->eref  = makeAlias(scan->outer_alias, NIL);

			/* attach proper column aliases.. */
			rte->alias = generate_remote_rte_alias(rte, rtindex,
							scan->outer_alias, scan->outer_reduce_level);
		}
		if (bms_is_member(rtindex, scan->inner_relids))
		{
			rte->eref  = makeAlias(scan->inner_alias, NIL);

			/* attach proper column aliases.. */
			rte->alias = generate_remote_rte_alias(rte, rtindex,
					scan->inner_alias, scan->inner_reduce_level);
		}
	}
	bms_free(tmprelids);

	/* Set up deparsing context */
	context = deparse_context_for_planstate((Node *) parent,
			NULL,
			root->parse->rtable);

	exprstr = deparse_expression(node, context, true, false);

	/* revert back the saved eref entries in the same order now!  */
	cell = list_head(leref);
	tmprelids = bms_copy(relids);
	while ((rtindex = bms_first_member(tmprelids)) >= 0)
	{
		RangeTblEntry	*rte = planner_rt_fetch(rtindex, root);

		Assert(cell != NULL);

		rte->eref  = lfirst(cell);
		cell = lnext(cell);

		rte->alias = lfirst(cell);
		cell = lnext(cell);
	}
	bms_free(tmprelids);

	appendStringInfo(expr, " %s", exprstr);
	return;
}
#endif /* XCP */
#endif /* PGXC */

/*
 * create_append_plan
 *	  Create an Append plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_append_plan(PlannerInfo *root, AppendPath *best_path)
{
	Append	   *plan;
	List	   *tlist = build_relation_tlist(best_path->path.parent);
	List	   *subplans = NIL;
	ListCell   *subpaths;

	/*
	 * It is possible for the subplans list to contain only one entry, or even
	 * no entries.	Handle these cases specially.
	 *
	 * XXX ideally, if there's just one entry, we'd not bother to generate an
	 * Append node but just return the single child.  At the moment this does
	 * not work because the varno of the child scan plan won't match the
	 * parent-rel Vars it'll be asked to emit.
	 */
	if (best_path->subpaths == NIL)
	{
		/* Generate a Result plan with constant-FALSE gating qual */
		return (Plan *) make_result(root,
									tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);
	}

	/* Normal case with multiple subpaths */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);

		subplans = lappend(subplans, create_plan_recurse(root, subpath));
	}

	plan = make_append(subplans, tlist);

	return (Plan *) plan;
}

/*
 * create_merge_append_plan
 *	  Create a MergeAppend plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path)
{
	MergeAppend *node = makeNode(MergeAppend);
	Plan	   *plan = &node->plan;
	List	   *tlist = build_relation_tlist(best_path->path.parent);
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;

	/*
	 * We don't have the actual creation of the MergeAppend node split out
	 * into a separate make_xxx function.  This is because we want to run
	 * prepare_sort_from_pathkeys on it before we do so on the individual
	 * child plans, to make cross-checking the sort info easier.
	 */
	copy_path_costsize(plan, (Path *) best_path);
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;

	/* Compute sort column info, and adjust MergeAppend's tlist as needed */
	(void) prepare_sort_from_pathkeys(root, plan, pathkeys,
									  true,
									  &node->numCols,
									  &node->sortColIdx,
									  &node->sortOperators,
									  &node->collations,
									  &node->nullsFirst);

	/*
	 * Now prepare the child plans.  We must apply prepare_sort_from_pathkeys
	 * even to subplans that don't need an explicit sort, to make sure they
	 * are returning the same sort key columns the MergeAppend expects.
	 */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;
		int			numsortkeys;
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *collations;
		bool	   *nullsFirst;

		/* Build the child plan */
		subplan = create_plan_recurse(root, subpath);

		/* Compute sort column info, and adjust subplan's tlist as needed */
		subplan = prepare_sort_from_pathkeys(root, subplan, pathkeys,
											 false,
											 &numsortkeys,
											 &sortColIdx,
											 &sortOperators,
											 &collations,
											 &nullsFirst);

		/*
		 * Check that we got the same sort key information.  We just Assert
		 * that the sortops match, since those depend only on the pathkeys;
		 * but it seems like a good idea to check the sort column numbers
		 * explicitly, to ensure the tlists really do match up.
		 */
		Assert(numsortkeys == node->numCols);
		if (memcmp(sortColIdx, node->sortColIdx,
				   numsortkeys * sizeof(AttrNumber)) != 0)
			elog(ERROR, "MergeAppend child's targetlist doesn't match MergeAppend");
		Assert(memcmp(sortOperators, node->sortOperators,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(collations, node->collations,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(nullsFirst, node->nullsFirst,
					  numsortkeys * sizeof(bool)) == 0);

		/* Now, insert a Sort node if subplan isn't sufficiently ordered */
		if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
			subplan = (Plan *) make_sort(root, subplan, numsortkeys,
										 sortColIdx, sortOperators,
										 collations, nullsFirst,
										 best_path->limit_tuples);

		subplans = lappend(subplans, subplan);
	}

	node->mergeplans = subplans;

	return (Plan *) node;
}

/*
 * create_result_plan
 *	  Create a Result plan for 'best_path'.
 *	  This is only used for the case of a query with an empty jointree.
 *
 *	  Returns a Plan node.
 */
static Result *
create_result_plan(PlannerInfo *root, ResultPath *best_path)
{
	List	   *tlist;
	List	   *quals;

	/* The tlist will be installed later, since we have no RelOptInfo */
	Assert(best_path->path.parent == NULL);
	tlist = NIL;

	/* best_path->quals is just bare clauses */

	quals = order_qual_clauses(root, best_path->quals);

	return make_result(root, tlist, (Node *) quals, NULL);
}

/*
 * create_material_plan
 *	  Create a Material plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Material *
create_material_plan(PlannerInfo *root, MaterialPath *best_path)
{
	Material   *plan;
	Plan	   *subplan;

	subplan = create_plan_recurse(root, best_path->subpath);

	/* We don't want any excess columns in the materialized tuples */
	disuse_physical_tlist(subplan, best_path->subpath);

	plan = make_material(subplan);

	copy_path_costsize(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_unique_plan
 *	  Create a Unique plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_unique_plan(PlannerInfo *root, UniquePath *best_path)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *in_operators;
	List	   *uniq_exprs;
	List	   *newtlist;
	int			nextresno;
	bool		newitems;
	int			numGroupCols;
	AttrNumber *groupColIdx;
	int			groupColPos;
	ListCell   *l;

	subplan = create_plan_recurse(root, best_path->subpath);

	/* Done if we don't need to do any actual unique-ifying */
	if (best_path->umethod == UNIQUE_PATH_NOOP)
		return subplan;

	/*
	 * As constructed, the subplan has a "flat" tlist containing just the Vars
	 * needed here and at upper levels.  The values we are supposed to
	 * unique-ify may be expressions in these variables.  We have to add any
	 * such expressions to the subplan's tlist.
	 *
	 * The subplan may have a "physical" tlist if it is a simple scan plan. If
	 * we're going to sort, this should be reduced to the regular tlist, so
	 * that we don't sort more data than we need to.  For hashing, the tlist
	 * should be left as-is if we don't need to add any expressions; but if we
	 * do have to add expressions, then a projection step will be needed at
	 * runtime anyway, so we may as well remove unneeded items. Therefore
	 * newtlist starts from build_relation_tlist() not just a copy of the
	 * subplan's tlist; and we don't install it into the subplan unless we are
	 * sorting or stuff has to be added.
	 */
	in_operators = best_path->in_operators;
	uniq_exprs = best_path->uniq_exprs;

	/* initialize modified subplan tlist as just the "required" vars */
	newtlist = build_relation_tlist(best_path->path.parent);
	nextresno = list_length(newtlist) + 1;
	newitems = false;

	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)
		{
			tle = makeTargetEntry((Expr *) uniqexpr,
								  nextresno,
								  NULL,
								  false);
			newtlist = lappend(newtlist, tle);
			nextresno++;
			newitems = true;
		}
	}

	if (newitems || best_path->umethod == UNIQUE_PATH_SORT)
	{
		/*
		 * If the top plan node can't do projections, we need to add a Result
		 * node to help it along.
		 */
		if (!is_projection_capable_plan(subplan))
			subplan = (Plan *) make_result(root, newtlist, NULL, subplan);
		else
			subplan->targetlist = newtlist;
#ifdef XCP
		/*
		 * RemoteSubplan is conditionally projection capable - it is pushing
		 * projection to the data nodes
		 */
		if (IsA(subplan, RemoteSubplan))
			subplan->lefttree->targetlist = newtlist;
#endif
	}

	/*
	 * Build control information showing which subplan output columns are to
	 * be examined by the grouping step.  Unfortunately we can't merge this
	 * with the previous loop, since we didn't then know which version of the
	 * subplan tlist we'd end up using.
	 */
	newtlist = subplan->targetlist;
	numGroupCols = list_length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));

	groupColPos = 0;
	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)				/* shouldn't happen */
			elog(ERROR, "failed to find unique expression in subplan tlist");
		groupColIdx[groupColPos++] = tle->resno;
	}

	if (best_path->umethod == UNIQUE_PATH_HASH)
	{
		long		numGroups;
		Oid		   *groupOperators;

		numGroups = (long) Min(best_path->rows, (double) LONG_MAX);

		/*
		 * Get the hashable equality operators for the Agg node to use.
		 * Normally these are the same as the IN clause operators, but if
		 * those are cross-type operators then the equality operators are the
		 * ones for the IN clause operators' RHS datatype.
		 */
		groupOperators = (Oid *) palloc(numGroupCols * sizeof(Oid));
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			eq_oper;

			if (!get_compatible_hash_operators(in_oper, NULL, &eq_oper))
				elog(ERROR, "could not find compatible hash operator for operator %u",
					 in_oper);
			groupOperators[groupColPos++] = eq_oper;
		}

		/*
		 * Since the Agg node is going to project anyway, we can give it the
		 * minimum output tlist, without any stuff we might have added to the
		 * subplan tlist.
		 */
		plan = (Plan *) make_agg(root,
								 build_relation_tlist(best_path->path.parent),
								 NIL,
								 AGG_HASHED,
								 NULL,
								 numGroupCols,
								 groupColIdx,
								 groupOperators,
								 numGroups,
								 subplan);
	}
	else
	{
		List	   *sortList = NIL;

		/* Create an ORDER BY list to sort the input compatibly */
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			sortop;
			Oid			eqop;
			TargetEntry *tle;
			SortGroupClause *sortcl;

			sortop = get_ordering_op_for_equality_op(in_oper, false);
			if (!OidIsValid(sortop))	/* shouldn't happen */
				elog(ERROR, "could not find ordering operator for equality operator %u",
					 in_oper);

			/*
			 * The Unique node will need equality operators.  Normally these
			 * are the same as the IN clause operators, but if those are
			 * cross-type operators then the equality operators are the ones
			 * for the IN clause operators' RHS datatype.
			 */
			eqop = get_equality_op_for_ordering_op(sortop, NULL);
			if (!OidIsValid(eqop))		/* shouldn't happen */
				elog(ERROR, "could not find equality operator for ordering operator %u",
					 sortop);

			tle = get_tle_by_resno(subplan->targetlist,
								   groupColIdx[groupColPos]);
			Assert(tle != NULL);

			sortcl = makeNode(SortGroupClause);
			sortcl->tleSortGroupRef = assignSortGroupRef(tle,
														 subplan->targetlist);
			sortcl->eqop = eqop;
			sortcl->sortop = sortop;
			sortcl->nulls_first = false;
			sortcl->hashable = false;	/* no need to make this accurate */
			sortList = lappend(sortList, sortcl);
			groupColPos++;
		}
		plan = (Plan *) make_sort_from_sortclauses(root, sortList, subplan);
		plan = (Plan *) make_unique(plan, sortList);
	}

	/* Adjust output size estimate (other fields should be OK already) */
	plan->plan_rows = best_path->rows;

	return plan;
}


#ifdef XCP
/*
 * create_remotescan_plan
 *	  Create a RemoteSubquery plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static RemoteSubplan *
create_remotescan_plan(PlannerInfo *root,
					   RemoteSubPath *best_path)
{
	RemoteSubplan  *plan;
	Plan	   	   *subplan;

	subplan = create_plan(root, best_path->subpath);

	/* We don't want any excess columns in the remote tuples */
	disuse_physical_tlist(subplan, best_path->subpath);

	plan = make_remotesubplan(root, subplan,
							  best_path->path.distribution,
							  best_path->subpath->distribution,
							  best_path->path.pathkeys);

	copy_path_costsize(&plan->scan.plan, (Path *) best_path);

	return plan;
}


RemoteSubplan *
find_push_down_plan(Plan *plan, bool force)
{
	if (IsA(plan, RemoteSubplan) &&
			(force || (list_length(((RemoteSubplan *) plan)->nodeList) > 1 &&
					   ((RemoteSubplan *) plan)->execOnAll)))
		return (RemoteSubplan *) plan;
	if (IsA(plan, Hash) ||
			IsA(plan, Material) ||
			IsA(plan, Unique) ||
			IsA(plan, Limit))
		return find_push_down_plan(plan->lefttree, force);
	return NULL;
}
#endif


/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS
 *
 *****************************************************************************/


/*
 * create_seqscan_plan
 *	 Returns a seqscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SeqScan *
create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_path_costsize(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_indexscan_plan
 *	  Returns an indexscan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 *
 * The indexquals list of the path contains implicitly-ANDed qual conditions.
 * The list can be empty --- then no index restrictions will be applied during
 * the scan.
 */
static IndexScan *
create_indexscan_plan(PlannerInfo *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses)
{
	List	   *indexquals = best_path->indexquals;
	List	   *indexorderbys = best_path->indexorderbys;
	Index		baserelid = best_path->path.parent->relid;
	Oid			indexoid = best_path->indexinfo->indexoid;
	List	   *qpqual;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	List	   *fixed_indexorderbys;
	ListCell   *l;
	IndexScan  *scan_plan;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * Build "stripped" indexquals structure (no RestrictInfos) to pass to
	 * executor as indexqualorig
	 */
	stripped_indexquals = get_actual_clauses(indexquals);

	/*
	 * The executor needs a copy with the indexkey on the left of each clause
	 * and with index attr numbers substituted for table ones.
	 */
	fixed_indexquals = fix_indexqual_references(root, best_path, indexquals);

	/*
	 * Likewise fix up index attr references in the ORDER BY expressions.
	 */
	fixed_indexorderbys = fix_indexorderby_references(root, best_path, indexorderbys);

	/*
	 * If this is an innerjoin scan, the indexclauses will contain join
	 * clauses that are not present in scan_clauses (since the passed-in value
	 * is just the rel's baserestrictinfo list).  We must add these clauses to
	 * scan_clauses to ensure they get checked.  In most cases we will remove
	 * the join clauses again below, but if a join clause contains a special
	 * operator, we need to make sure it gets into the scan_clauses.
	 *
	 * Note: pointer comparison should be enough to determine RestrictInfo
	 * matches.
	 */
	if (best_path->isjoininner)
		scan_clauses = list_union_ptr(scan_clauses, best_path->indexclauses);

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index.  All the predicates in the indexquals will be checked
	 * (either by the index itself, or by nodeIndexscan.c), but if there are
	 * any "special" operators involved then they must be included in qpqual.
	 * The upshot is that qpqual must contain scan_clauses minus whatever
	 * appears in indexquals.
	 *
	 * In normal cases simple pointer equality checks will be enough to spot
	 * duplicate RestrictInfos, so we try that first.  In some situations
	 * (particularly with OR'd index conditions) we may have scan_clauses that
	 * are not equal to, but are logically implied by, the index quals; so we
	 * also try a predicate_implied_by() check to see if we can discard quals
	 * that way.  (predicate_implied_by assumes its first input contains only
	 * immutable functions, so we have to check that.)
	 *
	 * We can also discard quals that are implied by a partial index's
	 * predicate, but only in a plain SELECT; when scanning a target relation
	 * of UPDATE/DELETE/SELECT FOR UPDATE, we must leave such quals in the
	 * plan so that they'll be properly rechecked by EvalPlanQual testing.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member_ptr(indexquals, rinfo))
			continue;
		if (!contain_mutable_functions((Node *) rinfo->clause))
		{
			List	   *clausel = list_make1(rinfo->clause);

			if (predicate_implied_by(clausel, indexquals))
				continue;
			if (best_path->indexinfo->indpred)
			{
				if (baserelid != root->parse->resultRelation &&
					get_parse_rowmark(root->parse, baserelid) == NULL)
					if (predicate_implied_by(clausel,
											 best_path->indexinfo->indpred))
						continue;
			}
		}
		qpqual = lappend(qpqual, rinfo);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * We have to replace any outer-relation variables with nestloop params in
	 * the indexqualorig, qpqual, and indexorderbyorig expressions.  A bit
	 * annoying to have to do this separately from the processing in
	 * fix_indexqual_references --- rethink this when generalizing the inner
	 * indexscan support.  But note we can't really do this earlier because
	 * it'd break the comparisons to predicates above ... (or would it?  Those
	 * wouldn't have outer refs)
	 */
	if (best_path->isjoininner)
	{
		stripped_indexquals = (List *)
			replace_nestloop_params(root, (Node *) stripped_indexquals);
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		indexorderbys = (List *)
			replace_nestloop_params(root, (Node *) indexorderbys);
	}

	/* Finally ready to build the plan node */
	scan_plan = make_indexscan(tlist,
							   qpqual,
							   baserelid,
							   indexoid,
							   fixed_indexquals,
							   stripped_indexquals,
							   fixed_indexorderbys,
							   indexorderbys,
							   best_path->indexscandir);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);
	/* use the indexscan-specific rows estimate, not the parent rel's */
	scan_plan->scan.plan.plan_rows = best_path->rows;

	return scan_plan;
}

/*
 * create_bitmap_scan_plan
 *	  Returns a bitmap scan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static BitmapHeapScan *
create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist,
						List *scan_clauses)
{
	Index		baserelid = best_path->path.parent->relid;
	Plan	   *bitmapqualplan;
	List	   *bitmapqualorig;
	List	   *indexquals;
	List	   *qpqual;
	ListCell   *l;
	BitmapHeapScan *scan_plan;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Process the bitmapqual tree into a Plan tree and qual lists */
	bitmapqualplan = create_bitmap_subplan(root, best_path->bitmapqual,
										   &bitmapqualorig, &indexquals);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * If this is a innerjoin scan, the indexclauses will contain join clauses
	 * that are not present in scan_clauses (since the passed-in value is just
	 * the rel's baserestrictinfo list).  We must add these clauses to
	 * scan_clauses to ensure they get checked.  In most cases we will remove
	 * the join clauses again below, but if a join clause contains a special
	 * operator, we need to make sure it gets into the scan_clauses.
	 */
	if (best_path->isjoininner)
	{
		scan_clauses = list_concat_unique(scan_clauses, bitmapqualorig);
	}

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index.  All the predicates in the indexquals will be checked
	 * (either by the index itself, or by nodeBitmapHeapscan.c), but if there
	 * are any "special" operators involved then they must be added to qpqual.
	 * The upshot is that qpqual must contain scan_clauses minus whatever
	 * appears in indexquals.
	 *
	 * In normal cases simple equal() checks will be enough to spot duplicate
	 * clauses, so we try that first.  In some situations (particularly with
	 * OR'd index conditions) we may have scan_clauses that are not equal to,
	 * but are logically implied by, the index quals; so we also try a
	 * predicate_implied_by() check to see if we can discard quals that way.
	 * (predicate_implied_by assumes its first input contains only immutable
	 * functions, so we have to check that.)
	 *
	 * Unlike create_indexscan_plan(), we need take no special thought here
	 * for partial index predicates; this is because the predicate conditions
	 * are already listed in bitmapqualorig and indexquals.  Bitmap scans have
	 * to do it that way because predicate conditions need to be rechecked if
	 * the scan becomes lossy.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		Node	   *clause = (Node *) lfirst(l);

		if (list_member(indexquals, clause))
			continue;
		if (!contain_mutable_functions(clause))
		{
			List	   *clausel = list_make1(clause);

			if (predicate_implied_by(clausel, indexquals))
				continue;
		}
		qpqual = lappend(qpqual, clause);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/*
	 * When dealing with special operators, we will at this point have
	 * duplicate clauses in qpqual and bitmapqualorig.	We may as well drop
	 * 'em from bitmapqualorig, since there's no point in making the tests
	 * twice.
	 */
	bitmapqualorig = list_difference_ptr(bitmapqualorig, qpqual);

	/* Finally ready to build the plan node */
	scan_plan = make_bitmap_heapscan(tlist,
									 qpqual,
									 bitmapqualplan,
									 bitmapqualorig,
									 baserelid);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);
	/* use the indexscan-specific rows estimate, not the parent rel's */
	scan_plan->scan.plan.plan_rows = best_path->rows;

	return scan_plan;
}

/*
 * Given a bitmapqual tree, generate the Plan tree that implements it
 *
 * As byproducts, we also return in *qual and *indexqual the qual lists
 * (in implicit-AND form, without RestrictInfos) describing the original index
 * conditions and the generated indexqual conditions.  (These are the same in
 * simple cases, but when special index operators are involved, the former
 * list includes the special conditions while the latter includes the actual
 * indexable conditions derived from them.)  Both lists include partial-index
 * predicates, because we have to recheck predicates as well as index
 * conditions if the bitmap scan becomes lossy.
 *
 * Note: if you find yourself changing this, you probably need to change
 * make_restrictinfo_from_bitmapqual too.
 */
static Plan *
create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual)
{
	Plan	   *plan;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		ListCell   *l;

		/*
		 * There may well be redundant quals among the subplans, since a
		 * top-level WHERE qual might have gotten used to form several
		 * different index quals.  We don't try exceedingly hard to eliminate
		 * redundancies, but we do eliminate obvious duplicates by using
		 * list_concat_unique.
		 */
		foreach(l, apath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual);
			subplans = lappend(subplans, subplan);
			subquals = list_concat_unique(subquals, subqual);
			subindexquals = list_concat_unique(subindexquals, subindexqual);
		}
		plan = (Plan *) make_bitmap_and(subplans);
		plan->startup_cost = apath->path.startup_cost;
		plan->total_cost = apath->path.total_cost;
		plan->plan_rows =
			clamp_row_est(apath->bitmapselectivity * apath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		*qual = subquals;
		*indexqual = subindexquals;
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		bool		const_true_subqual = false;
		bool		const_true_subindexqual = false;
		ListCell   *l;

		/*
		 * Here, we only detect qual-free subplans.  A qual-free subplan would
		 * cause us to generate "... OR true ..."  which we may as well reduce
		 * to just "true".	We do not try to eliminate redundant subclauses
		 * because (a) it's not as likely as in the AND case, and (b) we might
		 * well be working with hundreds or even thousands of OR conditions,
		 * perhaps from a long IN list.  The performance of list_append_unique
		 * would be unacceptable.
		 */
		foreach(l, opath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual);
			subplans = lappend(subplans, subplan);
			if (subqual == NIL)
				const_true_subqual = true;
			else if (!const_true_subqual)
				subquals = lappend(subquals,
								   make_ands_explicit(subqual));
			if (subindexqual == NIL)
				const_true_subindexqual = true;
			else if (!const_true_subindexqual)
				subindexquals = lappend(subindexquals,
										make_ands_explicit(subindexqual));
		}

		/*
		 * In the presence of ScalarArrayOpExpr quals, we might have built
		 * BitmapOrPaths with just one subpath; don't add an OR step.
		 */
		if (list_length(subplans) == 1)
		{
			plan = (Plan *) linitial(subplans);
		}
		else
		{
			plan = (Plan *) make_bitmap_or(subplans);
			plan->startup_cost = opath->path.startup_cost;
			plan->total_cost = opath->path.total_cost;
			plan->plan_rows =
				clamp_row_est(opath->bitmapselectivity * opath->path.parent->tuples);
			plan->plan_width = 0;		/* meaningless */
		}

		/*
		 * If there were constant-TRUE subquals, the OR reduces to constant
		 * TRUE.  Also, avoid generating one-element ORs, which could happen
		 * due to redundancy elimination or ScalarArrayOpExpr quals.
		 */
		if (const_true_subqual)
			*qual = NIL;
		else if (list_length(subquals) <= 1)
			*qual = subquals;
		else
			*qual = list_make1(make_orclause(subquals));
		if (const_true_subindexqual)
			*indexqual = NIL;
		else if (list_length(subindexquals) <= 1)
			*indexqual = subindexquals;
		else
			*indexqual = list_make1(make_orclause(subindexquals));
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		IndexScan  *iscan;
		ListCell   *l;

		/* Use the regular indexscan plan build machinery... */
		iscan = create_indexscan_plan(root, ipath, NIL, NIL);
		/* then convert to a bitmap indexscan */
		plan = (Plan *) make_bitmap_indexscan(iscan->scan.scanrelid,
											  iscan->indexid,
											  iscan->indexqual,
											  iscan->indexqualorig);
		plan->startup_cost = 0.0;
		plan->total_cost = ipath->indextotalcost;
		plan->plan_rows =
			clamp_row_est(ipath->indexselectivity * ipath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		*qual = get_actual_clauses(ipath->indexclauses);
		*indexqual = get_actual_clauses(ipath->indexquals);
		foreach(l, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(l);

			/*
			 * We know that the index predicate must have been implied by the
			 * query condition as a whole, but it may or may not be implied by
			 * the conditions that got pushed into the bitmapqual.	Avoid
			 * generating redundant conditions.
			 */
			if (!predicate_implied_by(list_make1(pred), ipath->indexclauses))
			{
				*qual = lappend(*qual, pred);
				*indexqual = lappend(*indexqual, pred);
			}
		}

		/*
		 * Replace outer-relation variables with nestloop params, but only
		 * after doing the above comparisons to index predicates.
		 */
		if (ipath->isjoininner)
		{
			*qual = (List *)
				replace_nestloop_params(root, (Node *) *qual);
			*indexqual = (List *)
				replace_nestloop_params(root, (Node *) *indexqual);
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
		plan = NULL;			/* keep compiler quiet */
	}

	return plan;
}

/*
 * create_tidscan_plan
 *	 Returns a tidscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *ortidquals;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * Remove any clauses that are TID quals.  This is a bit tricky since the
	 * tidquals list has implicit OR semantics.
	 */
	ortidquals = best_path->tidquals;
	if (list_length(ortidquals) > 1)
		ortidquals = list_make1(make_orclause(ortidquals));
	scan_clauses = list_difference(scan_clauses, ortidquals);

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 best_path->tidquals);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 Returns a subqueryscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SubqueryScan *
create_subqueryscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a subquery base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_SUBQUERY);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  best_path->parent->subplan,
								  best_path->parent->subrtable,
								  best_path->parent->subrowmark);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 Returns a functionscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static FunctionScan *
create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid,
								  rte->funcexpr,
								  rte->eref->colnames,
								  rte->funccoltypes,
								  rte->funccoltypmods,
								  rte->funccolcollations);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_valuesscan_plan
 *	 Returns a valuesscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ValuesScan *
create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	ValuesScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;

	/* it should be a values base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_VALUES);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	scan_plan = make_valuesscan(tlist, scan_clauses, scan_relid,
								rte->values_lists);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_ctescan_plan
 *	 Returns a ctescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static CteScan *
create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	CteScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	SubPlan    *ctesplan = NULL;
	int			plan_id;
	int			cte_param_id;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(!rte->self_reference);

	/*
	 * Find the referenced CTE, and locate the SubPlan previously made for it.
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * Note: cte_plan_ids can be shorter than cteList, if we are still working
	 * on planning the CTEs (ie, this is a side-reference from another CTE).
	 * So we mustn't use forboth here.
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	if (ndx >= list_length(cteroot->cte_plan_ids))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
	plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
	Assert(plan_id > 0);
	foreach(lc, cteroot->init_plans)
	{
		ctesplan = (SubPlan *) lfirst(lc);
		if (ctesplan->plan_id == plan_id)
			break;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);

	/*
	 * We need the CTE param ID, which is the sole member of the SubPlan's
	 * setParam list.
	 */
	cte_param_id = linitial_int(ctesplan->setParam);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	scan_plan = make_ctescan(tlist, scan_clauses, scan_relid,
							 plan_id, cte_param_id);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_worktablescan_plan
 *	 Returns a worktablescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static WorkTableScan *
create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	WorkTableScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	Index		levelsup;
	PlannerInfo *cteroot;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(rte->self_reference);

	/*
	 * We need to find the worktable param ID, which is in the plan level
	 * that's processing the recursive UNION, which is one level *below* where
	 * the CTE comes from.
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* shouldn't happen */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	if (cteroot->wt_param_id < 0)		/* shouldn't happen */
		elog(ERROR, "could not find param ID for CTE \"%s\"", rte->ctename);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	scan_plan = make_worktablescan(tlist, scan_clauses, scan_relid,
								   cteroot->wt_param_id);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}


#ifdef PGXC
#ifndef XCP
/*
 * create_remotequery_plan
 *	 Returns a remotequery plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static RemoteQuery *
create_remotequery_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	RemoteQuery *scan_plan;
	bool			prefix;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	char	   *wherestr			= NULL;
	List	   *remote_scan_clauses = NIL;
	List	   *local_scan_clauses  = NIL;
	Oid				nspid;
	char		   *nspname;
	char		   *relname;
	const char	   *nspname_q;
	const char	   *relname_q;
	const char	   *aliasname_q;
	ListCell	   *lc;
	List 		   *deparse_context;
	bool			first;
	StringInfoData	sql;
	RelationLocInfo *rel_loc_info;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(best_path->parent->rtekind == RTE_RELATION);
	Assert(rte->rtekind == RTE_RELATION);

	deparse_context = deparse_context_for_remotequery(rte->eref, rte->relid);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	if (scan_clauses)
	{
		ListCell	  *l;

		foreach(l, (List *)scan_clauses)
	    {
			Node *clause = lfirst(l);

			if (is_foreign_expr(clause, NULL))
				remote_scan_clauses = lappend(remote_scan_clauses, clause);
			else
				local_scan_clauses = lappend(local_scan_clauses, clause);
		}
	}

	/*
	 * Incorporate any remote_scan_clauses into the WHERE clause that
	 * we intend to push to the remote server.
	 */
	if (remote_scan_clauses)
	{
		char 		   *sep = "";
		ListCell	   *l;
		StringInfoData	buf;

		initStringInfo(&buf);

		/*
		 * remote_scan_clauses is a list of scan clauses (restrictions) that we
		 * can push to the remote server. We want to deparse each of those
		 * expressions (that is, each member of the List) and AND them together
		 * into a WHERE clause.
		 */

		foreach(l, (List *)remote_scan_clauses)
		{
			Node *clause = lfirst(l);

			appendStringInfo(&buf, "%s", sep );
			appendStringInfo(&buf, "%s", deparse_expression(clause, deparse_context, false, false));
			sep = " AND ";
		}

		wherestr = buf.data;
	}

	/*
	 * Scanning multiple relations in a RemoteQuery node is not supported.
	 */
	prefix = false;
#if 0
	prefix = list_length(estate->es_range_table) > 1;
#endif

	/* Get quoted names of schema, table and alias */
	nspid = get_rel_namespace(rte->relid);
	nspname = get_namespace_name(nspid);
	relname = get_rel_name(rte->relid);
	nspname_q = quote_identifier(nspname);
	relname_q = quote_identifier(relname);
	aliasname_q = quote_identifier(rte->eref->aliasname);

	initStringInfo(&sql);

	/* deparse SELECT clause */
	appendStringInfo(&sql, "SELECT ");

	/*
	 * TODO: omit (deparse to "NULL") columns which are not used in the
	 * original SQL.
	 *
	 * We must parse nodes parents of this RemoteQuery node to determine unused
	 * columns because some columns may be used only in parent Sort/Agg/Limit
	 * nodes.
	 */
	first = true;
	foreach (lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (!first)
			appendStringInfoString(&sql, ", ");

		appendStringInfo(&sql, "%s", deparse_expression((Node *) tle->expr,
														deparse_context,
														false,
														false));
		first = false;
	}

	/* if target list is composed only of system attributes, add dummy column */
	if (first)
		appendStringInfo(&sql, "NULL");

	/* deparse FROM clause */
	appendStringInfo(&sql, " FROM ");
	/*
	 * XXX: should use GENERIC OPTIONS like 'foreign_relname' or something for
	 * the foreign table name instead of the local name ?
	 *
	 * A temporary table does not use namespace as it may not be
	 * consistent among nodes cluster. Relation name is sufficient.
	 */
	if (IsTempTable(rte->relid))
		appendStringInfo(&sql, "%s %s", relname_q, aliasname_q);
	else
		appendStringInfo(&sql, "%s.%s %s", nspname_q, relname_q, aliasname_q);

	pfree(nspname);
	pfree(relname);
	if (nspname_q != nspname_q)
		pfree((char *) nspname_q);
	if (relname_q != relname_q)
		pfree((char *) relname_q);
	if (aliasname_q != rte->eref->aliasname)
		pfree((char *) aliasname_q);

	if (wherestr)
	{
		appendStringInfo(&sql, " WHERE %s", wherestr);
		pfree(wherestr);
	}

	scan_plan = make_remotequery(tlist,
							 rte,
							 local_scan_clauses,
							 scan_relid);

	scan_plan->sql_statement = sql.data;

	/*
	 * Populate what nodes we execute on.
	 * This is still basic, and was done to make sure we do not select
	 * a replicated table from all nodes.
	 * It does not take into account conditions on partitioned relations
	 * that could reduce to one node. To do that, we need to move general
	 * planning earlier.
	 */
	rel_loc_info = GetRelationLocInfo(rte->relid);
	scan_plan->exec_nodes = makeNode(ExecNodes);
	scan_plan->exec_nodes->tableusagetype = TABLE_USAGE_TYPE_USER;
	if (rel_loc_info)
		scan_plan->exec_nodes->baselocatortype = rel_loc_info->locatorType;
	else
		scan_plan->exec_nodes->baselocatortype = '\0';
	scan_plan->exec_nodes = GetRelationNodes(rel_loc_info, 0, UNKNOWNOID, RELATION_ACCESS_READ);
	copy_path_costsize(&scan_plan->scan.plan, best_path);

	/* PGXCTODO - get better estimates */
 	scan_plan->scan.plan.plan_rows = 1000;

	return scan_plan;
}
#endif /* XCP */
#endif /* PGXC */

/*
 * create_foreignscan_plan
 *	 Returns a foreignscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ForeignScan *
create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses)
{
	ForeignScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	RangeTblEntry *rte;
	bool		fsSystemCol;
	int			i;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(rel->rtekind == RTE_RELATION);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Detect whether any system columns are requested from rel */
	fsSystemCol = false;
	for (i = rel->min_attr; i < 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
		{
			fsSystemCol = true;
			break;
		}
	}

	scan_plan = make_foreignscan(tlist,
								 scan_clauses,
								 scan_relid,
								 fsSystemCol,
								 best_path->fdwplan);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*****************************************************************************
 *
 *	JOIN METHODS
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(PlannerInfo *root,
					 NestPath *best_path,
					 Plan *outer_plan,
					 Plan *inner_plan)
{
    NestLoop   *join_plan;
    List       *tlist = build_relation_tlist(best_path->path.parent);
    List       *joinrestrictclauses = best_path->joinrestrictinfo;
    List       *joinclauses;
    List       *otherclauses;
    Relids      outerrelids;
    List       *nestParams;
    ListCell   *cell;
    ListCell   *prev;
    ListCell   *next;

	/*
	 * If the inner path is a nestloop inner indexscan, it might be using some
	 * of the join quals as index quals, in which case we don't have to check
	 * them again at the join node.  Remove any join quals that are redundant.
	 */
	joinrestrictclauses =
		select_nonredundant_join_clauses(root,
										 joinrestrictclauses,
										 best_path->innerjoinpath);

	/* Sort join qual clauses into best execution order */
	joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Identify any nestloop parameters that should be supplied by this join
	 * node, and move them from root->curOuterParams to the nestParams list.
	 */
	outerrelids = best_path->outerjoinpath->parent->relids;
	nestParams = NIL;
	prev = NULL;
	for (cell = list_head(root->curOuterParams); cell; cell = next)
	{
		NestLoopParam *nlp = (NestLoopParam *) lfirst(cell);

		next = lnext(cell);
		if (bms_is_member(nlp->paramval->varno, outerrelids))
		{
			root->curOuterParams = list_delete_cell(root->curOuterParams,
													cell, prev);
			nestParams = lappend(nestParams, nlp);
		}
		else
			prev = cell;
	}
#ifdef XCP
	/*
	 * While NestLoop is executed it rescans inner plan. We do not want to
	 * rescan RemoteSubplan and do not support it.
	 * So if inner_plan is a RemoteSubplan, materialize it.
	 */
	if (IsA(inner_plan, RemoteSubplan))
	{
		Plan	   *matplan = (Plan *) make_material(inner_plan);

		/*
		 * We assume the materialize will not spill to disk, and therefore
		 * charge just cpu_operator_cost per tuple.  (Keep this estimate in
		 * sync with cost_mergejoin.)
		 */
		copy_plan_costsize(matplan, inner_plan);
		matplan->total_cost += cpu_operator_cost * matplan->plan_rows;

		inner_plan = matplan;
	}
#endif

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  nestParams,
							  outer_plan,
							  inner_plan,
							  best_path->jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->path);

	return join_plan;
}

static MergeJoin *
create_mergejoin_plan(PlannerInfo *root,
					  MergePath *best_path,
					  Plan *outer_plan,
					  Plan *inner_plan)
{
	List	   *tlist = build_relation_tlist(best_path->jpath.path.parent);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *mergeclauses;
	List	   *outerpathkeys;
	List	   *innerpathkeys;
	int			nClauses;
	Oid		   *mergefamilies;
	Oid		   *mergecollations;
	int		   *mergestrategies;
	bool	   *mergenullsfirst;
	MergeJoin  *join_plan;
	int			i;
	ListCell   *lc;
	ListCell   *lop;
	ListCell   *lip;

	/* Sort join qual clauses into best execution order */
	/* NB: do NOT reorder the mergeclauses */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the mergeclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	joinclauses = list_difference(joinclauses, mergeclauses);

	/*
	 * Rearrange mergeclauses, if needed, so that the outer variable is always
	 * on the left; mark the mergeclause restrictinfos with correct
	 * outer_is_left status.
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
							 best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * Create explicit sort nodes for the outer and inner paths if necessary.
	 * Make sure there are no excess columns in the inputs if sorting.
	 */
	if (best_path->outersortkeys)
	{
		disuse_physical_tlist(outer_plan, best_path->jpath.outerjoinpath);
		outer_plan = (Plan *)
			make_sort_from_pathkeys(root,
									outer_plan,
									best_path->outersortkeys,
									-1.0);
		outerpathkeys = best_path->outersortkeys;
	}
	else
		outerpathkeys = best_path->jpath.outerjoinpath->pathkeys;

	if (best_path->innersortkeys)
	{
		disuse_physical_tlist(inner_plan, best_path->jpath.innerjoinpath);
		inner_plan = (Plan *)
			make_sort_from_pathkeys(root,
									inner_plan,
									best_path->innersortkeys,
									-1.0);
		innerpathkeys = best_path->innersortkeys;
	}
	else
		innerpathkeys = best_path->jpath.innerjoinpath->pathkeys;

	/*
	 * If specified, add a materialize node to shield the inner plan from the
	 * need to handle mark/restore.
	 */
	if (best_path->materialize_inner)
	{
		Plan	   *matplan = (Plan *) make_material(inner_plan);

		/*
		 * We assume the materialize will not spill to disk, and therefore
		 * charge just cpu_operator_cost per tuple.  (Keep this estimate in
		 * sync with cost_mergejoin.)
		 */
		copy_plan_costsize(matplan, inner_plan);
		matplan->total_cost += cpu_operator_cost * matplan->plan_rows;

		inner_plan = matplan;
	}

	/*
	 * Compute the opfamily/collation/strategy/nullsfirst arrays needed by the
	 * executor.  The information is in the pathkeys for the two inputs, but
	 * we need to be careful about the possibility of mergeclauses sharing a
	 * pathkey (compare find_mergeclauses_for_pathkeys()).
	 */
	nClauses = list_length(mergeclauses);
	Assert(nClauses == list_length(best_path->path_mergeclauses));
	mergefamilies = (Oid *) palloc(nClauses * sizeof(Oid));
	mergecollations = (Oid *) palloc(nClauses * sizeof(Oid));
	mergestrategies = (int *) palloc(nClauses * sizeof(int));
	mergenullsfirst = (bool *) palloc(nClauses * sizeof(bool));

	lop = list_head(outerpathkeys);
	lip = list_head(innerpathkeys);
	i = 0;
	foreach(lc, best_path->path_mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *opathkey;
		PathKey    *ipathkey;
		EquivalenceClass *opeclass;
		EquivalenceClass *ipeclass;
		ListCell   *l2;

		/* fetch outer/inner eclass from mergeclause */
		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}
		Assert(oeclass != NULL);
		Assert(ieclass != NULL);

		/*
		 * For debugging purposes, we check that the eclasses match the paths'
		 * pathkeys.  In typical cases the merge clauses are one-to-one with
		 * the pathkeys, but when dealing with partially redundant query
		 * conditions, we might have clauses that re-reference earlier path
		 * keys.  The case that we need to reject is where a pathkey is
		 * entirely skipped over.
		 *
		 * lop and lip reference the first as-yet-unused pathkey elements;
		 * it's okay to match them, or any element before them.  If they're
		 * NULL then we have found all pathkey elements to be used.
		 */
		if (lop)
		{
			opathkey = (PathKey *) lfirst(lop);
			opeclass = opathkey->pk_eclass;
			if (oeclass == opeclass)
			{
				/* fast path for typical case */
				lop = lnext(lop);
			}
			else
			{
				/* redundant clauses ... must match something before lop */
				foreach(l2, outerpathkeys)
				{
					if (l2 == lop)
						break;
					opathkey = (PathKey *) lfirst(l2);
					opeclass = opathkey->pk_eclass;
					if (oeclass == opeclass)
						break;
				}
				if (oeclass != opeclass)
					elog(ERROR, "outer pathkeys do not match mergeclauses");
			}
		}
		else
		{
			/* redundant clauses ... must match some already-used pathkey */
			opathkey = NULL;
			opeclass = NULL;
			foreach(l2, outerpathkeys)
			{
				opathkey = (PathKey *) lfirst(l2);
				opeclass = opathkey->pk_eclass;
				if (oeclass == opeclass)
					break;
			}
			if (l2 == NULL)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
		}

		if (lip)
		{
			ipathkey = (PathKey *) lfirst(lip);
			ipeclass = ipathkey->pk_eclass;
			if (ieclass == ipeclass)
			{
				/* fast path for typical case */
				lip = lnext(lip);
			}
			else
			{
				/* redundant clauses ... must match something before lip */
				foreach(l2, innerpathkeys)
				{
					if (l2 == lip)
						break;
					ipathkey = (PathKey *) lfirst(l2);
					ipeclass = ipathkey->pk_eclass;
					if (ieclass == ipeclass)
						break;
				}
				if (ieclass != ipeclass)
					elog(ERROR, "inner pathkeys do not match mergeclauses");
			}
		}
		else
		{
			/* redundant clauses ... must match some already-used pathkey */
			ipathkey = NULL;
			ipeclass = NULL;
			foreach(l2, innerpathkeys)
			{
				ipathkey = (PathKey *) lfirst(l2);
				ipeclass = ipathkey->pk_eclass;
				if (ieclass == ipeclass)
					break;
			}
			if (l2 == NULL)
				elog(ERROR, "inner pathkeys do not match mergeclauses");
		}

		/* pathkeys should match each other too (more debugging) */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation ||
			opathkey->pk_strategy != ipathkey->pk_strategy ||
			opathkey->pk_nulls_first != ipathkey->pk_nulls_first)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* OK, save info for executor */
		mergefamilies[i] = opathkey->pk_opfamily;
		mergecollations[i] = opathkey->pk_eclass->ec_collation;
		mergestrategies[i] = opathkey->pk_strategy;
		mergenullsfirst[i] = opathkey->pk_nulls_first;
		i++;
	}

	/*
	 * Note: it is not an error if we have additional pathkey elements (i.e.,
	 * lop or lip isn't NULL here).  The input paths might be better-sorted
	 * than we need for the current mergejoin.
	 */

	/*
	 * Now we can build the mergejoin node.
	 */
	join_plan = make_mergejoin(tlist,
							   joinclauses,
							   otherclauses,
							   mergeclauses,
							   mergefamilies,
							   mergecollations,
							   mergestrategies,
							   mergenullsfirst,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype);

	/* Costs of sort and material steps are included in path cost already */
	copy_path_costsize(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static HashJoin *
create_hashjoin_plan(PlannerInfo *root,
					 HashPath *best_path,
					 Plan *outer_plan,
					 Plan *inner_plan)
{
	List	   *tlist = build_relation_tlist(best_path->jpath.path.parent);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses;
	Oid			skewTable = InvalidOid;
	AttrNumber	skewColumn = InvalidAttrNumber;
	bool		skewInherit = false;
	Oid			skewColType = InvalidOid;
	int32		skewColTypmod = -1;
	HashJoin   *join_plan;
	Hash	   *hash_plan;

	/* Sort join qual clauses into best execution order */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);
	/* There's no point in sorting the hash clauses ... */

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = list_difference(joinclauses, hashclauses);

	/*
	 * Rearrange hashclauses, if needed, so that the outer variable is always
	 * on the left.
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
							 best_path->jpath.outerjoinpath->parent->relids);

	/* We don't want any excess columns in the hashed tuples */
	disuse_physical_tlist(inner_plan, best_path->jpath.innerjoinpath);

	/* If we expect batching, suppress excess columns in outer tuples too */
	if (best_path->num_batches > 1)
		disuse_physical_tlist(outer_plan, best_path->jpath.outerjoinpath);

	/*
	 * If there is a single join clause and we can identify the outer variable
	 * as a simple column reference, supply its identity for possible use in
	 * skew optimization.  (Note: in principle we could do skew optimization
	 * with multiple join clauses, but we'd have to be able to determine the
	 * most common combinations of outer values, which we don't currently have
	 * enough stats for.)
	 */
	if (list_length(hashclauses) == 1)
	{
		OpExpr	   *clause = (OpExpr *) linitial(hashclauses);
		Node	   *node;

		Assert(is_opclause(clause));
		node = (Node *) linitial(clause->args);
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RangeTblEntry *rte;

			rte = root->simple_rte_array[var->varno];
			if (rte->rtekind == RTE_RELATION)
			{
				skewTable = rte->relid;
				skewColumn = var->varattno;
				skewInherit = rte->inh;
				skewColType = var->vartype;
				skewColTypmod = var->vartypmod;
			}
		}
	}

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan,
						  skewTable,
						  skewColumn,
						  skewInherit,
						  skewColType,
						  skewColTypmod);
	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES
 *
 *****************************************************************************/

/*
 * replace_nestloop_params
 *	  Replace outer-relation Vars in the given expression with nestloop Params
 *
 * All Vars belonging to the relation(s) identified by root->curOuterRels
 * are replaced by Params, and entries are added to root->curOuterParams if
 * not already present.
 */
static Node *
replace_nestloop_params(PlannerInfo *root, Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return replace_nestloop_params_mutator(expr, root);
}

static Node *
replace_nestloop_params_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Param	   *param;
		NestLoopParam *nlp;
		ListCell   *lc;

		/* Upper-level Vars should be long gone at this point */
		Assert(var->varlevelsup == 0);
		/* If not to be replaced, we can just return the Var unmodified */
		if (!bms_is_member(var->varno, root->curOuterRels))
			return node;
		/* Create a Param representing the Var */
		param = assign_nestloop_param(root, var);
		/* Is this param already listed in root->curOuterParams? */
		foreach(lc, root->curOuterParams)
		{
			nlp = (NestLoopParam *) lfirst(lc);
			if (nlp->paramno == param->paramid)
			{
				Assert(equal(var, nlp->paramval));
				/* Present, so we can just return the Param */
				return (Node *) param;
			}
		}
		/* No, so add it */
		nlp = makeNode(NestLoopParam);
		nlp->paramno = param->paramid;
		nlp->paramval = var;
		root->curOuterParams = lappend(root->curOuterParams, nlp);
		/* And return the replacement Param */
		return (Node *) param;
	}
	return expression_tree_mutator(node,
								   replace_nestloop_params_mutator,
								   (void *) root);
}

/*
 * fix_indexqual_references
 *	  Adjust indexqual clauses to the form the executor's indexqual
 *	  machinery needs.
 *
 * We have four tasks here:
 *	* Remove RestrictInfo nodes from the input clauses.
 *	* Replace any outer-relation Var nodes with nestloop Params.
 *	  (XXX eventually, that responsibility should go elsewhere?)
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.
 *
 * The result is a modified copy of the indexquals list --- the
 * original is not changed.  Note also that the copy shares no substructure
 * with the original; this is needed in case there is a subplan in it (we need
 * two separate copies of the subplan tree, or things will go awry).
 */
static List *
fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
						 List *indexquals)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexquals;
	ListCell   *l;

	fixed_indexquals = NIL;

	foreach(l, indexquals)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		Node	   *clause;

		Assert(IsA(rinfo, RestrictInfo));

		/*
		 * Replace any outer-relation variables with nestloop params.
		 *
		 * This also makes a copy of the clause, so it's safe to modify it
		 * in-place below.
		 */
		clause = replace_nestloop_params(root, (Node *) rinfo->clause);

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			if (list_length(op->args) != 2)
				elog(ERROR, "indexqual clause is not binary opclause");

			/*
			 * Check to see if the indexkey is on the right; if so, commute
			 * the clause. The indexkey should be the side that refers to
			 * (only) the base relation.
			 */
			if (!bms_equal(rinfo->left_relids, index->rel->relids))
				CommuteOpExpr(op);

			/*
			 * Now, determine which index attribute this is and change the
			 * indexkey operand as needed.
			 */
			linitial(op->args) = fix_indexqual_operand(linitial(op->args),
													   index);
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;
			ListCell   *lc;

			/*
			 * Check to see if the indexkey is on the right; if so, commute
			 * the clause. The indexkey should be the side that refers to
			 * (only) the base relation.
			 */
			if (!bms_overlap(pull_varnos(linitial(rc->largs)),
							 index->rel->relids))
				CommuteRowCompareExpr(rc);

			/*
			 * For each column in the row comparison, determine which index
			 * attribute this is and change the indexkey operand as needed.
			 */
			foreach(lc, rc->largs)
			{
				lfirst(lc) = fix_indexqual_operand(lfirst(lc),
												   index);
			}
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			/* Never need to commute... */

			/*
			 * Determine which index attribute this is and change the indexkey
			 * operand as needed.
			 */
			linitial(saop->args) = fix_indexqual_operand(linitial(saop->args),
														 index);
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;

			nt->arg = (Expr *) fix_indexqual_operand((Node *) nt->arg,
													 index);
		}
		else
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));

		fixed_indexquals = lappend(fixed_indexquals, clause);
	}

	return fixed_indexquals;
}

/*
 * fix_indexorderby_references
 *	  Adjust indexorderby clauses to the form the executor's index
 *	  machinery needs.
 *
 * This is a simplified version of fix_indexqual_references.  The input does
 * not have RestrictInfo nodes, and we assume that indxqual.c already
 * commuted the clauses to put the index keys on the left.	Also, we don't
 * bother to support any cases except simple OpExprs, since nothing else
 * is allowed for ordering operators.
 */
static List *
fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path,
							List *indexorderbys)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexorderbys;
	ListCell   *l;

	fixed_indexorderbys = NIL;

	foreach(l, indexorderbys)
	{
		Node	   *clause = (Node *) lfirst(l);

		/*
		 * Replace any outer-relation variables with nestloop params.
		 *
		 * This also makes a copy of the clause, so it's safe to modify it
		 * in-place below.
		 */
		clause = replace_nestloop_params(root, clause);

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			if (list_length(op->args) != 2)
				elog(ERROR, "indexorderby clause is not binary opclause");

			/*
			 * Now, determine which index attribute this is and change the
			 * indexkey operand as needed.
			 */
			linitial(op->args) = fix_indexqual_operand(linitial(op->args),
													   index);
		}
		else
			elog(ERROR, "unsupported indexorderby type: %d",
				 (int) nodeTag(clause));

		fixed_indexorderbys = lappend(fixed_indexorderbys, clause);
	}

	return fixed_indexorderbys;
}

/*
 * fix_indexqual_operand
 *	  Convert an indexqual expression to a Var referencing the index column.
 */
static Node *
fix_indexqual_operand(Node *node, IndexOptInfo *index)
{
	/*
	 * We represent index keys by Var nodes having the varno of the base table
	 * but varattno equal to the index's attribute number (index column
	 * position).  This is a bit hokey ... would be cleaner to use a
	 * special-purpose node type that could not be mistaken for a regular Var.
	 * But it will do for now.
	 */
	Var		   *result;
	int			pos;
	ListCell   *indexpr_item;

	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	if (IsA(node, Var) &&
		((Var *) node)->varno == index->rel->relid)
	{
		/* Try to match against simple index columns */
		int			varatt = ((Var *) node)->varattno;

		if (varatt != 0)
		{
			for (pos = 0; pos < index->ncolumns; pos++)
			{
				if (index->indexkeys[pos] == varatt)
				{
					result = (Var *) copyObject(node);
					result->varattno = pos + 1;
					return (Node *) result;
				}
			}
		}
	}

	/* Try to match against index expressions */
	indexpr_item = list_head(index->indexprs);
	for (pos = 0; pos < index->ncolumns; pos++)
	{
		if (index->indexkeys[pos] == 0)
		{
			Node	   *indexkey;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			if (indexkey && IsA(indexkey, RelabelType))
				indexkey = (Node *) ((RelabelType *) indexkey)->arg;
			if (equal(node, indexkey))
			{
				/* Found a match */
				result = makeVar(index->rel->relid, pos + 1,
								 exprType(lfirst(indexpr_item)), -1,
								 exprCollation(lfirst(indexpr_item)),
								 0);
				return (Node *) result;
			}
			indexpr_item = lnext(indexpr_item);
		}
	}

	/* Ooops... */
	elog(ERROR, "node is not an index attribute");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_switched_clauses
 *	  Given a list of merge or hash joinclauses (as RestrictInfo nodes),
 *	  extract the bare clauses, and rearrange the elements within the
 *	  clauses, if needed, so the outer join variable is on the left and
 *	  the inner is on the right.  The original clause data structure is not
 *	  touched; a modified list is returned.  We do, however, set the transient
 *	  outer_is_left field in each RestrictInfo to show which side was which.
 */
static List *
get_switched_clauses(List *clauses, Relids outerrelids)
{
	List	   *t_list = NIL;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);
		OpExpr	   *clause = (OpExpr *) restrictinfo->clause;

		Assert(is_opclause(clause));
		if (bms_is_subset(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * Duplicate just enough of the structure to allow commuting the
			 * clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->opcollid = clause->opcollid;
			temp->inputcollid = clause->inputcollid;
			temp->args = list_copy(clause->args);
			temp->location = clause->location;
			/* Commute it --- note this modifies the temp node in-place. */
			CommuteOpExpr(temp);
			t_list = lappend(t_list, temp);
			restrictinfo->outer_is_left = false;
		}
		else
		{
			Assert(bms_is_subset(restrictinfo->left_relids, outerrelids));
			t_list = lappend(t_list, clause);
			restrictinfo->outer_is_left = true;
		}
	}
	return t_list;
}

/*
 * order_qual_clauses
 *		Given a list of qual clauses that will all be evaluated at the same
 *		plan node, sort the list into the order we want to check the quals
 *		in at runtime.
 *
 * Ideally the order should be driven by a combination of execution cost and
 * selectivity, but it's not immediately clear how to account for both,
 * and given the uncertainty of the estimates the reliability of the decisions
 * would be doubtful anyway.  So we just order by estimated per-tuple cost,
 * being careful not to change the order when (as is often the case) the
 * estimates are identical.
 *
 * Although this will work on either bare clauses or RestrictInfos, it's
 * much faster to apply it to RestrictInfos, since it can re-use cost
 * information that is cached in RestrictInfos.
 *
 * Note: some callers pass lists that contain entries that will later be
 * removed; this is the easiest way to let this routine see RestrictInfos
 * instead of bare clauses.  It's OK because we only sort by cost, but
 * a cost/selectivity combination would likely do the wrong thing.
 */
static List *
order_qual_clauses(PlannerInfo *root, List *clauses)
{
	typedef struct
	{
		Node	   *clause;
		Cost		cost;
	} QualItem;
	int			nitems = list_length(clauses);
	QualItem   *items;
	ListCell   *lc;
	int			i;
	List	   *result;

	/* No need to work hard for 0 or 1 clause */
	if (nitems <= 1)
		return clauses;

	/*
	 * Collect the items and costs into an array.  This is to avoid repeated
	 * cost_qual_eval work if the inputs aren't RestrictInfos.
	 */
	items = (QualItem *) palloc(nitems * sizeof(QualItem));
	i = 0;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		QualCost	qcost;

		cost_qual_eval_node(&qcost, clause, root);
		items[i].clause = clause;
		items[i].cost = qcost.per_tuple;
		i++;
	}

	/*
	 * Sort.  We don't use qsort() because it's not guaranteed stable for
	 * equal keys.	The expected number of entries is small enough that a
	 * simple insertion sort should be good enough.
	 */
	for (i = 1; i < nitems; i++)
	{
		QualItem	newitem = items[i];
		int			j;

		/* insert newitem into the already-sorted subarray */
		for (j = i; j > 0; j--)
		{
			if (newitem.cost >= items[j - 1].cost)
				break;
			items[j] = items[j - 1];
		}
		items[j] = newitem;
	}

	/* Convert back to a list */
	result = NIL;
	for (i = 0; i < nitems; i++)
		result = lappend(result, items[i].clause);

	return result;
}

/*
 * Copy cost and size info from a Path node to the Plan node created from it.
 * The executor usually won't use this info, but it's needed by EXPLAIN.
 */
static void
copy_path_costsize(Plan *dest, Path *src)
{
	if (src)
	{
		dest->startup_cost = src->startup_cost;
		dest->total_cost = src->total_cost;
		dest->plan_rows = src->parent->rows;
		dest->plan_width = src->parent->width;
	}
	else
	{
		dest->startup_cost = 0;
		dest->total_cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * (Most callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	if (src)
	{
		dest->startup_cost = src->startup_cost;
		dest->total_cost = src->total_cost;
		dest->plan_rows = src->plan_rows;
		dest->plan_width = src->plan_width;
	}
	else
	{
		dest->startup_cost = 0;
		dest->total_cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}


/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES
 *
 * Some of these are exported because they are called to build plan nodes
 * in contexts where we're not deriving the plan node from a path node.
 *
 *****************************************************************************/

static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   Oid indexid,
			   List *indexqual,
			   List *indexqualorig,
			   List *indexorderby,
			   List *indexorderbyorig,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indexorderbyorig = indexorderbyorig;
	node->indexorderdir = indexscandir;

	return node;
}

static BitmapIndexScan *
make_bitmap_indexscan(Index scanrelid,
					  Oid indexid,
					  List *indexqual,
					  List *indexqualorig)
{
	BitmapIndexScan *node = makeNode(BitmapIndexScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = NIL;		/* not used */
	plan->qual = NIL;			/* not used */
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;

	return node;
}

static BitmapHeapScan *
make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid)
{
	BitmapHeapScan *node = makeNode(BitmapHeapScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->bitmapqualorig = bitmapqualorig;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tidquals)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidquals = tidquals;

	return node;
}

SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan,
				  List *subrtable,
				  List *subrowmark)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	/*
	 * Cost is figured here for the convenience of prepunion.c.  Note this is
	 * only correct for the case where qpqual is empty; otherwise caller
	 * should overwrite cost with a better estimate.
	 */
	copy_plan_costsize(plan, subplan);
	plan->total_cost += cpu_tuple_cost * subplan->plan_rows;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->subplan = subplan;
	node->subrtable = subrtable;
	node->subrowmark = subrowmark;

	return node;
}

static FunctionScan *
make_functionscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Node *funcexpr,
				  List *funccolnames,
				  List *funccoltypes,
				  List *funccoltypmods,
				  List *funccolcollations)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->funcexpr = funcexpr;
	node->funccolnames = funccolnames;
	node->funccoltypes = funccoltypes;
	node->funccoltypmods = funccoltypmods;
	node->funccolcollations = funccolcollations;

	return node;
}

static ValuesScan *
make_valuesscan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				List *values_lists)
{
	ValuesScan *node = makeNode(ValuesScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->values_lists = values_lists;

	return node;
}

static CteScan *
make_ctescan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 int ctePlanId,
			 int cteParam)
{
	CteScan    *node = makeNode(CteScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->ctePlanId = ctePlanId;
	node->cteParam = cteParam;

	return node;
}

static WorkTableScan *
make_worktablescan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   int wtParam)
{
	WorkTableScan *node = makeNode(WorkTableScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->wtParam = wtParam;

	return node;
}


#ifdef PGXC
#ifndef XCP
static RemoteQuery *
make_remotequery(List *qptlist,
				 RangeTblEntry *rte,
				 List *qpqual,
				 Index scanrelid)
{
	RemoteQuery *node = makeNode(RemoteQuery);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->read_only = true;

	return node;
}
#endif /* XCP */
#endif /* PGXC */


#ifdef XCP
/*
 * make_remotesubplan
 * 	Create a RemoteSubplan node to execute subplan on remote nodes.
 *  leftree - the subplan which we want to push down to remote node.
 *  resultDistribution - the distribution of the remote result. May be NULL -
 * results are coming to the invoking node
 *  execDistribution - determines how source data of the subplan are
 * distributed, where we should send the subplan and how combine results.
 *	pathkeys - the remote subplan is sorted according to these keys, executor
 * 		should perform merge sort of incoming tuples
 */
RemoteSubplan *
make_remotesubplan(PlannerInfo *root,
				   Plan *lefttree,
				   Distribution *resultDistribution,
				   Distribution *execDistribution,
				   List *pathkeys)
{
	RemoteSubplan *node = makeNode(RemoteSubplan);
	Plan	   *plan = &node->scan.plan;
	Bitmapset  *tmpset;
	int			nodenum;

	/* Sanity checks */
	Assert(!equal(resultDistribution, execDistribution));
	Assert(!IsA(lefttree, RemoteSubplan));

	if (resultDistribution)
	{
		node->distributionType = resultDistribution->distributionType;
		node->distributionKey = InvalidAttrNumber;
		if (resultDistribution->distributionExpr)
		{
			ListCell   *lc;

			/* Find distribution expression in the target list */
			foreach(lc, lefttree->targetlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				if (equal(tle->expr, resultDistribution->distributionExpr))
				{
					node->distributionKey = tle->resno;
					break;
				}
			}

			if (node->distributionKey == InvalidAttrNumber)
			{
				TargetEntry *newtle;

				/* The expression is not found, need to add junk */
				newtle = makeTargetEntry((Expr *) resultDistribution->distributionExpr,
										 list_length(lefttree->targetlist) + 1,
									     NULL,
										 true);

				if (is_projection_capable_plan(lefttree))
				{
					/* Ok to modify subplan's target list */
					lefttree->targetlist = lappend(lefttree->targetlist, newtle);
				}
				else
				{
					/* Use Result node to calculate expression */
					List *newtlist = list_copy(lefttree->targetlist);
					newtlist = lappend(newtlist, newtle);
					lefttree = (Plan *) make_result(root, newtlist, NULL, lefttree);
				}
			}
		}
		tmpset = bms_copy(resultDistribution->nodes);
		node->distributionNodes = NIL;
		while ((nodenum = bms_first_member(tmpset)) >= 0)
			node->distributionNodes = lappend_int(node->distributionNodes,
												  nodenum);
		bms_free(tmpset);
	}
	else
	{
		/*
		 * Return results to the caller only
		 * NB: we do not use LOCATOR_TYPE_COORDINATOR here since the value \0
		 * causes problem when plan is decoded on remote data node. Reader treat
		 * is as the end of the input.
		 */
		node->distributionType = LOCATOR_TYPE_SINGLE;
		node->distributionKey = InvalidAttrNumber;
		node->distributionNodes = NIL;
	}
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	copy_plan_costsize(plan, lefttree);
	/* determine where subplan will be executed */
	if (execDistribution)
	{
		if (execDistribution->restrictNodes)
			tmpset = bms_copy(execDistribution->restrictNodes);
		else
			tmpset = bms_copy(execDistribution->nodes);
		node->nodeList = NIL;
		while ((nodenum = bms_first_member(tmpset)) >= 0)
			node->nodeList = lappend_int(node->nodeList, nodenum);
		bms_free(tmpset);
		node->execOnAll = !IsReplicated(execDistribution->distributionType);
	}
	else
	{
		/* execute on local datanode only */
		node->nodeList = NIL;
		node->execOnAll = false;
	}
	plan->targetlist = lefttree->targetlist;
	/* We do not need to merge sort if only one node is yielding tuples */
	if (pathkeys && node->execOnAll && list_length(node->nodeList) > 1)
	{
		List	   *tlist = lefttree->targetlist;
		ListCell   *i;
		int			numsortkeys;
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *collations;
		bool	   *nullsFirst;

		/*
		 * We will need at most list_length(pathkeys) sort columns; possibly less
		 */
		numsortkeys = list_length(pathkeys);
		sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
		sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
		collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
		nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

		numsortkeys = 0;

		foreach(i, pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(i);
			EquivalenceClass *ec = pathkey->pk_eclass;
			TargetEntry *tle = NULL;
			Oid			pk_datatype = InvalidOid;
			Oid			sortop;
			ListCell   *j;

			if (ec->ec_has_volatile)
			{
				/*
				 * If the pathkey's EquivalenceClass is volatile, then it must
				 * have come from an ORDER BY clause, and we have to match it to
				 * that same targetlist entry.
				 */
				if (ec->ec_sortref == 0)	/* can't happen */
					elog(ERROR, "volatile EquivalenceClass has no sortref");
				tle = get_sortgroupref_tle(ec->ec_sortref, tlist);
				Assert(tle);
				Assert(list_length(ec->ec_members) == 1);
				pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
			}
			else
			{
				/*
				 * Otherwise, we can sort by any non-constant expression listed in
				 * the pathkey's EquivalenceClass.  For now, we take the first one
				 * that corresponds to an available item in the tlist.	If there
				 * isn't any, use the first one that is an expression in the
				 * input's vars.  (The non-const restriction only matters if the
				 * EC is below_outer_join; but if it isn't, it won't contain
				 * consts anyway, else we'd have discarded the pathkey as
				 * redundant.)
				 *
				 * XXX if we have a choice, is there any way of figuring out which
				 * might be cheapest to execute?  (For example, int4lt is likely
				 * much cheaper to execute than numericlt, but both might appear
				 * in the same equivalence class...)  Not clear that we ever will
				 * have an interesting choice in practice, so it may not matter.
				 */
				foreach(j, ec->ec_members)
				{
					EquivalenceMember *em = (EquivalenceMember *) lfirst(j);

					if (em->em_is_const || em->em_is_child)
						continue;

					tle = tlist_member((Node *) em->em_expr, tlist);
					if (tle)
					{
						pk_datatype = em->em_datatype;
						break;		/* found expr already in tlist */
					}

					/*
					 * We can also use it if the pathkey expression is a relabel
					 * of the tlist entry, or vice versa.  This is needed for
					 * binary-compatible cases (cf. make_pathkey_from_sortinfo).
					 * We prefer an exact match, though, so we do the basic search
					 * first.
					 */
					tle = tlist_member_ignore_relabel((Node *) em->em_expr, tlist);
					if (tle)
					{
						pk_datatype = em->em_datatype;
						break;		/* found expr already in tlist */
					}
				}

				if (!tle)
				{
					/* No matching tlist item; look for a computable expression */
					Expr	   *sortexpr = NULL;

					foreach(j, ec->ec_members)
					{
						EquivalenceMember *em = (EquivalenceMember *) lfirst(j);
						List	   *exprvars;
						ListCell   *k;

						if (em->em_is_const || em->em_is_child)
							continue;
						sortexpr = em->em_expr;
						exprvars = pull_var_clause((Node *) sortexpr,
												   PVC_INCLUDE_PLACEHOLDERS);
						foreach(k, exprvars)
						{
							if (!tlist_member_ignore_relabel(lfirst(k), tlist))
								break;
						}
						list_free(exprvars);
						if (!k)
						{
							pk_datatype = em->em_datatype;
							break;	/* found usable expression */
						}
					}
					if (!j)
						elog(ERROR, "could not find pathkey item to sort");

					/*
					 * Do we need to insert a Result node?
					 */
					if (!is_projection_capable_plan(lefttree))
					{
						/* copy needed so we don't modify input's tlist below */
						tlist = copyObject(tlist);
						lefttree = (Plan *) make_result(root, tlist, NULL,
														lefttree);
					}

					/*
					 * Add resjunk entry to input's tlist
					 */
					tle = makeTargetEntry(sortexpr,
										  list_length(tlist) + 1,
										  NULL,
										  true);
					tlist = lappend(tlist, tle);
					lefttree->targetlist = tlist;	/* just in case NIL before */
				}
			}

			/*
			 * Look up the correct sort operator from the PathKey's slightly
			 * abstracted representation.
			 */
			sortop = get_opfamily_member(pathkey->pk_opfamily,
										 pk_datatype,
										 pk_datatype,
										 pathkey->pk_strategy);
			if (!OidIsValid(sortop))	/* should not happen */
				elog(ERROR, "could not find member %d(%u,%u) of opfamily %u",
					 pathkey->pk_strategy, pk_datatype, pk_datatype,
					 pathkey->pk_opfamily);

			/*
			 * The column might already be selected as a sort key, if the pathkeys
			 * contain duplicate entries.  (This can happen in scenarios where
			 * multiple mergejoinable clauses mention the same var, for example.)
			 * So enter it only once in the sort arrays.
			 */
			numsortkeys = add_sort_column(tle->resno,
										  sortop,
										  pathkey->pk_eclass->ec_collation,
										  pathkey->pk_nulls_first,
										  numsortkeys,
										  sortColIdx, sortOperators,
										  collations, nullsFirst);
		}
		Assert(numsortkeys > 0);

		node->sort = makeNode(SimpleSort);
		node->sort->numCols = numsortkeys;
		node->sort->sortColIdx = sortColIdx;
		node->sort->sortOperators = sortOperators;
		node->sort->collations = collations;
		node->sort->nullsFirst = nullsFirst;
	}
	node->cursor = NULL;
	return node;
}
#endif /* XCP */


static ForeignScan *
make_foreignscan(List *qptlist,
				 List *qpqual,
				 Index scanrelid,
				 bool fsSystemCol,
				 FdwPlan *fdwplan)
{
	ForeignScan *node = makeNode(ForeignScan);

	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->fsSystemCol = fsSystemCol;
	node->fdwplan = fdwplan;

	return node;
}


Append *
make_append(List *appendplans, List *tlist)
{
	Append	   *node = makeNode(Append);
	Plan	   *plan = &node->plan;
	double		total_size;
	ListCell   *subnode;

	/*
	 * Compute cost as sum of subplan costs.  We charge nothing extra for the
	 * Append itself, which perhaps is too optimistic, but since it doesn't do
	 * any selection or projection, it is a pretty cheap node.
	 *
	 * If you change this, see also create_append_path().  Also, the size
	 * calculations should match set_append_rel_pathlist().  It'd be better
	 * not to duplicate all this logic, but some callers of this function
	 * aren't working from an appendrel or AppendPath, so there's noplace to
	 * copy the data from.
	 */
	plan->startup_cost = 0;
	plan->total_cost = 0;
	plan->plan_rows = 0;
	total_size = 0;
	foreach(subnode, appendplans)
	{
		Plan	   *subplan = (Plan *) lfirst(subnode);

		if (subnode == list_head(appendplans))	/* first node? */
			plan->startup_cost = subplan->startup_cost;
		plan->total_cost += subplan->total_cost;
		plan->plan_rows += subplan->plan_rows;
		total_size += subplan->plan_width * subplan->plan_rows;
	}
	if (plan->plan_rows > 0)
		plan->plan_width = rint(total_size / plan->plan_rows);
	else
		plan->plan_width = 0;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->appendplans = appendplans;

	return node;
}

RecursiveUnion *
make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups)
{
	RecursiveUnion *node = makeNode(RecursiveUnion);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);

	cost_recursive_union(plan, lefttree, righttree);

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->wtParam = wtParam;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	node->numCols = numCols;
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *dupColIdx;
		Oid		   *dupOperators;
		ListCell   *slitem;

		dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

		foreach(slitem, distinctList)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl,
													   plan->targetlist);

			dupColIdx[keyno] = tle->resno;
			dupOperators[keyno] = sortcl->eqop;
			Assert(OidIsValid(dupOperators[keyno]));
			keyno++;
		}
		node->dupColIdx = dupColIdx;
		node->dupOperators = dupOperators;
	}
	node->numGroups = numGroups;

	return node;
}

static BitmapAnd *
make_bitmap_and(List *bitmapplans)
{
	BitmapAnd  *node = makeNode(BitmapAnd);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static BitmapOr *
make_bitmap_or(List *bitmapplans)
{
	BitmapOr   *node = makeNode(BitmapOr);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *nestParams,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;
	node->nestParams = nestParams;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

static Hash *
make_hash(Plan *lefttree,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit,
		  Oid skewColType,
		  int32 skewColTypmod)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * For plausibility, make startup & total costs equal total cost of input
	 * plan; this only affects EXPLAIN display not decisions.
	 */
	plan->startup_cost = plan->total_cost;
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->skewTable = skewTable;
	node->skewColumn = skewColumn;
	node->skewInherit = skewInherit;
	node->skewColType = skewColType;
	node->skewColTypmod = skewColTypmod;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->mergeclauses = mergeclauses;
	node->mergeFamilies = mergefamilies;
	node->mergeCollations = mergecollations;
	node->mergeStrategies = mergestrategies;
	node->mergeNullsFirst = mergenullsfirst;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * make_sort --- basic routine to build a Sort plan node
 *
 * Caller must have built the sortColIdx, sortOperators, collations, and
 * nullsFirst arrays already.
 * limit_tuples is as for cost_sort (in particular, pass -1 if no limit)
 */
static Sort *
make_sort(PlannerInfo *root, Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst,
		  double limit_tuples)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;
	Path		sort_path;		/* dummy for result of cost_sort */
#ifdef XCP
	RemoteSubplan *pushdown;
#endif

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width,
			  0.0,
			  work_mem,
			  limit_tuples);
	plan->startup_cost = sort_path.startup_cost;
	plan->total_cost = sort_path.total_cost;
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->numCols = numCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->collations = collations;
	node->nullsFirst = nullsFirst;

#ifdef XCP
	/*
	 * It does not makes sence to sort on one data node and then perform
	 * one-tape merge sort. So do not push sort down if there is single
	 * remote data node
	 */
	pushdown = find_push_down_plan(lefttree, false);
	if (pushdown)
	{
		/* If we already sort results, need to prepend new keys to existing */
		/*
		 * It is not safe to share colum information.
		 * If another node will be pushed down the same RemoteSubplan column
		 * indexes may be modified and this would affect the Sort node
		 */
		AttrNumber *newSortColIdx;
		Oid 	   *newSortOperators;
		Oid 	   *newCollations;
		bool 	   *newNullsFirst;
		int 		newNumCols;
		int 		i, j;

		/*
		 * Insert new sort node immediately below the pushdown plan
		 */
		plan->lefttree = pushdown->scan.plan.lefttree;
		pushdown->scan.plan.lefttree = plan;

		newNumCols = numCols + (pushdown->sort ? pushdown->sort->numCols : 0);
		newSortColIdx = (AttrNumber *) palloc(newNumCols * sizeof(AttrNumber));
		newSortOperators = (Oid *) palloc(newNumCols * sizeof(Oid));
		newCollations = (Oid *) palloc(newNumCols * sizeof(Oid));
		newNullsFirst = (bool *) palloc(newNumCols * sizeof(bool));

		/* Copy sort columns */
		for (i = 0; i < numCols; i++)
		{
			newSortColIdx[i] = sortColIdx[i];
			newSortOperators[i] = sortOperators[i];
			newCollations[i] = collations[i];
			newNullsFirst[i] = nullsFirst[i];
		}

		newNumCols = numCols;
		if (pushdown->sort)
		{
			/* Continue and copy old keys of the subplan which is now under the
			 * sort */
			for (j = 0; j < pushdown->sort->numCols; j++)
				newNumCols = add_sort_column(pushdown->sort->sortColIdx[j],
											 pushdown->sort->sortOperators[j],
											 pushdown->sort->collations[j],
											 pushdown->sort->nullsFirst[j],
											 newNumCols,
											 newSortColIdx,
											 newSortOperators,
											 newCollations,
											 newNullsFirst);
		}
		else
		{
			/* Create simple sort object if does not exist */
			pushdown->sort = makeNode(SimpleSort);
		}

		pushdown->sort->numCols = newNumCols;
		pushdown->sort->sortColIdx = newSortColIdx;
		pushdown->sort->sortOperators = newSortOperators;
		pushdown->sort->collations = newCollations;
		pushdown->sort->nullsFirst = newNullsFirst;

		/*
		 * lefttree is not actually a Sort, but we hope it is not important and
		 * the result will be used as a generic Plan node.
		 */
		return (Sort *) lefttree;
	}
#endif
	return node;
}

/*
 * add_sort_column --- utility subroutine for building sort info arrays
 *
 * We need this routine because the same column might be selected more than
 * once as a sort key column; if so, the extra mentions are redundant.
 *
 * Caller is assumed to have allocated the arrays large enough for the
 * max possible number of columns.	Return value is the new column count.
 */
static int
add_sort_column(AttrNumber colIdx, Oid sortOp, Oid coll, bool nulls_first,
				int numCols, AttrNumber *sortColIdx,
				Oid *sortOperators, Oid *collations, bool *nullsFirst)
{
	int			i;

	Assert(OidIsValid(sortOp));

	for (i = 0; i < numCols; i++)
	{
		/*
		 * Note: we check sortOp because it's conceivable that "ORDER BY foo
		 * USING <, foo USING <<<" is not redundant, if <<< distinguishes
		 * values that < considers equal.  We need not check nulls_first
		 * however because a lower-order column with the same sortop but
		 * opposite nulls direction is redundant.
		 *
		 * We could probably consider sort keys with the same sortop and
		 * different collations to be redundant too, but for the moment treat
		 * them as not redundant.  This will be needed if we ever support
		 * collations with different notions of equality.
		 */
		if (sortColIdx[i] == colIdx &&
			sortOperators[numCols] == sortOp &&
			collations[numCols] == coll)
		{
			/* Already sorting by this col, so extra sort key is useless */
			return numCols;
		}
	}

	/* Add the column */
	sortColIdx[numCols] = colIdx;
	sortOperators[numCols] = sortOp;
	collations[numCols] = coll;
	nullsFirst[numCols] = nulls_first;
	return numCols + 1;
}


/*
 * prepare_sort_from_pathkeys
 *	  Prepare to sort according to given pathkeys
 *
 * This is used to set up for both Sort and MergeAppend nodes.	It calculates
 * the executor's representation of the sort key information, and adjusts the
 * plan targetlist if needed to add resjunk sort columns.
 *
 * Input parameters:
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'adjust_tlist_in_place' is TRUE if lefttree must be modified in-place
 *
 * We must convert the pathkey information into arrays of sort key column
 * numbers, sort operator OIDs, collation OIDs, and nulls-first flags,
 * which is the representation the executor wants.	These are returned into
 * the output parameters *p_numsortkeys etc.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions, since the Sort/MergeAppend node itself won't
 * do any such calculations.  If the input plan type isn't one that can do
 * projections, this means adding a Result node just to do the projection.
 * However, the caller can pass adjust_tlist_in_place = TRUE to force the
 * lefttree tlist to be modified in-place regardless of whether the node type
 * can project --- we use this for fixing the tlist of MergeAppend itself.
 *
 * Returns the node which is to be the input to the Sort (either lefttree,
 * or a Result stacked atop lefttree).
 */
static Plan *
prepare_sort_from_pathkeys(PlannerInfo *root, Plan *lefttree, List *pathkeys,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst)
{
	List	   *tlist = lefttree->targetlist;
	ListCell   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * We will need at most list_length(pathkeys) sort columns; possibly less
	 */
	numsortkeys = list_length(pathkeys);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *ec = pathkey->pk_eclass;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			sortop;
		ListCell   *j;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, tlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else
		{
			/*
			 * Otherwise, we can sort by any non-constant expression listed in
			 * the pathkey's EquivalenceClass.  For now, we take the first one
			 * that corresponds to an available item in the tlist.	If there
			 * isn't any, use the first one that is an expression in the
			 * input's vars.  (The non-const restriction only matters if the
			 * EC is below_outer_join; but if it isn't, it won't contain
			 * consts anyway, else we'd have discarded the pathkey as
			 * redundant.)
			 *
			 * XXX if we have a choice, is there any way of figuring out which
			 * might be cheapest to execute?  (For example, int4lt is likely
			 * much cheaper to execute than numericlt, but both might appear
			 * in the same equivalence class...)  Not clear that we ever will
			 * have an interesting choice in practice, so it may not matter.
			 */
			foreach(j, ec->ec_members)
			{
				EquivalenceMember *em = (EquivalenceMember *) lfirst(j);

				/*
				 * We shouldn't be trying to sort by an equivalence class that
				 * contains a constant, so no need to consider such cases any
				 * further.
				 */
				if (em->em_is_const)
					continue;

				tle = tlist_member((Node *) em->em_expr, tlist);
				if (tle)
				{
					pk_datatype = em->em_datatype;
					break;		/* found expr already in tlist */
				}

				/*
				 * We can also use it if the pathkey expression is a relabel
				 * of the tlist entry, or vice versa.  This is needed for
				 * binary-compatible cases (cf. make_pathkey_from_sortinfo).
				 * We prefer an exact match, though, so we do the basic search
				 * first.
				 */
				tle = tlist_member_ignore_relabel((Node *) em->em_expr, tlist);
				if (tle)
				{
					pk_datatype = em->em_datatype;
					break;		/* found expr already in tlist */
				}
			}

			if (!tle)
			{
				/* No matching tlist item; look for a computable expression */
				Expr	   *sortexpr = NULL;

				foreach(j, ec->ec_members)
				{
					EquivalenceMember *em = (EquivalenceMember *) lfirst(j);
					List	   *exprvars;
					ListCell   *k;

					if (em->em_is_const)
						continue;
					sortexpr = em->em_expr;
					exprvars = pull_var_clause((Node *) sortexpr,
											   PVC_INCLUDE_PLACEHOLDERS);
					foreach(k, exprvars)
					{
						if (!tlist_member_ignore_relabel(lfirst(k), tlist))
							break;
					}
					list_free(exprvars);
					if (!k)
					{
						pk_datatype = em->em_datatype;
						break;	/* found usable expression */
					}
				}
				if (!j)
					elog(ERROR, "could not find pathkey item to sort");

				/*
				 * Do we need to insert a Result node?
				 */
				if (!adjust_tlist_in_place &&
					!is_projection_capable_plan(lefttree))
				{
					/* copy needed so we don't modify input's tlist below */
					tlist = copyObject(tlist);
					lefttree = (Plan *) make_result(root, tlist, NULL,
													lefttree);
				}

				/* Don't bother testing is_projection_capable_plan again */
				adjust_tlist_in_place = true;

				/*
				 * Add resjunk entry to input's tlist
				 */
				tle = makeTargetEntry(sortexpr,
									  list_length(tlist) + 1,
									  NULL,
									  true);
				tlist = lappend(tlist, tle);
				lefttree->targetlist = tlist;	/* just in case NIL before */
#ifdef XCP
				/*
				 * RemoteSubplan is conditionally projection capable - it is
				 * pushing projection to the data nodes
				 */
				if (IsA(lefttree, RemoteSubplan))
					lefttree->lefttree->targetlist = tlist;
#endif
			}
		}

		/*
		 * Look up the correct sort operator from the PathKey's slightly
		 * abstracted representation.
		 */
		sortop = get_opfamily_member(pathkey->pk_opfamily,
									 pk_datatype,
									 pk_datatype,
									 pathkey->pk_strategy);
		if (!OidIsValid(sortop))	/* should not happen */
			elog(ERROR, "could not find member %d(%u,%u) of opfamily %u",
				 pathkey->pk_strategy, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		/*
		 * The column might already be selected as a sort key, if the pathkeys
		 * contain duplicate entries.  (This can happen in scenarios where
		 * multiple mergejoinable clauses mention the same var, for example.)
		 * So enter it only once in the sort arrays.
		 */
		numsortkeys = add_sort_column(tle->resno,
									  sortop,
									  pathkey->pk_eclass->ec_collation,
									  pathkey->pk_nulls_first,
									  numsortkeys,
									  sortColIdx, sortOperators,
									  collations, nullsFirst);
	}

	Assert(numsortkeys > 0);

	/* Return results */
	*p_numsortkeys = numsortkeys;
	*p_sortColIdx = sortColIdx;
	*p_sortOperators = sortOperators;
	*p_collations = collations;
	*p_nullsFirst = nullsFirst;

	return lefttree;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'limit_tuples' is the bound on the number of output tuples;
 *				-1 if no bound
 */
Sort *
make_sort_from_pathkeys(PlannerInfo *root, Plan *lefttree, List *pathkeys,
						double limit_tuples)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Compute sort column info, and adjust lefttree as needed */
	lefttree = prepare_sort_from_pathkeys(root, lefttree, pathkeys,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst);

	/* Now build the Sort node */
	return make_sort(root, lefttree, numsortkeys,
					 sortColIdx, sortOperators, collations,
					 nullsFirst, limit_tuples);
}

/*
 * make_sort_from_sortclauses
 *	  Create sort plan to sort according to given sortclauses
 *
 *	  'sortcls' is a list of SortGroupClauses
 *	  'lefttree' is the node which yields input tuples
 */
Sort *
make_sort_from_sortclauses(PlannerInfo *root, List *sortcls, Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * We will need at most list_length(sortcls) sort columns; possibly less
	 */
	numsortkeys = list_length(sortcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(l, sortcls)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, sub_tlist);

		/*
		 * Check for the possibility of duplicate order-by clauses --- the
		 * parser should have removed 'em, but no point in sorting
		 * redundantly.
		 */
		numsortkeys = add_sort_column(tle->resno, sortcl->sortop,
									  exprCollation((Node *) tle->expr),
									  sortcl->nulls_first,
									  numsortkeys,
									  sortColIdx, sortOperators,
									  collations, nullsFirst);
	}

	Assert(numsortkeys > 0);

	return make_sort(root, lefttree, numsortkeys,
					 sortColIdx, sortOperators, collations,
					 nullsFirst, -1.0);
}

/*
 * make_sort_from_groupcols
 *	  Create sort plan to sort based on grouping columns
 *
 * 'groupcls' is the list of SortGroupClauses
 * 'grpColIdx' gives the column numbers to use
 *
 * This might look like it could be merged with make_sort_from_sortclauses,
 * but presently we *must* use the grpColIdx[] array to locate sort columns,
 * because the child plan's tlist is not marked with ressortgroupref info
 * appropriate to the grouping node.  So, only the sort ordering info
 * is used from the SortGroupClause entries.
 */
Sort *
make_sort_from_groupcols(PlannerInfo *root,
						 List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	int			grpno = 0;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * We will need at most list_length(groupcls) sort columns; possibly less
	 */
	numsortkeys = list_length(groupcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(l, groupcls)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_tle_by_resno(sub_tlist, grpColIdx[grpno]);

		/*
		 * Check for the possibility of duplicate group-by clauses --- the
		 * parser should have removed 'em, but no point in sorting
		 * redundantly.
		 */
		numsortkeys = add_sort_column(tle->resno, grpcl->sortop,
									  exprCollation((Node *) tle->expr),
									  grpcl->nulls_first,
									  numsortkeys,
									  sortColIdx, sortOperators,
									  collations, nullsFirst);
		grpno++;
	}

	Assert(numsortkeys > 0);

	return make_sort(root, lefttree, numsortkeys,
					 sortColIdx, sortOperators, collations,
					 nullsFirst, -1.0);
}

static Material *
make_material(Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * materialize_finished_plan: stick a Material node atop a completed plan
 *
 * There are a couple of places where we want to attach a Material node
 * after completion of subquery_planner().	This currently requires hackery.
 * Since subquery_planner has already run SS_finalize_plan on the subplan
 * tree, we have to kluge up parameter lists for the Material node.
 * Possibly this could be fixed by postponing SS_finalize_plan processing
 * until setrefs.c is run?
 */
Plan *
materialize_finished_plan(Plan *subplan)
{
	Plan	   *matplan;
	Path		matpath;		/* dummy for result of cost_material */

	matplan = (Plan *) make_material(subplan);

	/* Set cost data */
	cost_material(&matpath,
				  subplan->startup_cost,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->startup_cost = matpath.startup_cost;
	matplan->total_cost = matpath.total_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;

	/* parameter kluge --- see comments above */
	matplan->extParam = bms_copy(subplan->extParam);
	matplan->allParam = bms_copy(subplan->allParam);

	return matplan;
}


#ifdef XCP
typedef struct
{
	List	   *subtlist;
	List	   *newtlist;
} find_referenced_cols_context;

static bool
find_referenced_cols_walker(Node *node, find_referenced_cols_context *context)
{
	TargetEntry *tle;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		/*
		 * We can not push down aggregates with DISTINCT.
		 */
		if (((Aggref *) node)->aggdistinct)
			return true;

		/*
		 * We need to add aggregate reference to the new tlist if it
		 * is not already there. Phase 1 aggregate is actually returns values
		 * of transition data type, so we should change the data type of the
		 * expression.
		 */
		if (!tlist_member(node, context->newtlist))
		{
			Aggref *aggref = (Aggref *) node;
			Aggref *newagg;
			TargetEntry *newtle;
			HeapTuple	aggTuple;
			Form_pg_aggregate aggform;
			Oid 	aggtranstype;

			aggTuple = SearchSysCache1(AGGFNOID,
									   ObjectIdGetDatum(aggref->aggfnoid));
			if (!HeapTupleIsValid(aggTuple))
				elog(ERROR, "cache lookup failed for aggregate %u",
					 aggref->aggfnoid);
			aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
			aggtranstype = aggform->aggtranstype;
			ReleaseSysCache(aggTuple);
			if (IsPolymorphicType(aggtranstype))
			{
				Oid 	   *inputTypes;
				Oid		   *declaredArgTypes;
				int			agg_nargs;
				int			numArgs;
				ListCell   *l;

				inputTypes = (Oid *) palloc(sizeof(Oid) * list_length(aggref->args));
				numArgs = 0;
				foreach(l, aggref->args)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(l);

					if (!tle->resjunk)
						inputTypes[numArgs++] = exprType((Node *) tle->expr);
				}

				/* have to fetch the agg's declared input types... */
				(void) get_func_signature(aggref->aggfnoid,
										  &declaredArgTypes, &agg_nargs);
				Assert(agg_nargs == numArgs);


				aggtranstype = enforce_generic_type_consistency(inputTypes,
																declaredArgTypes,
																agg_nargs,
																aggtranstype,
																false);
				pfree(inputTypes);
				pfree(declaredArgTypes);
			}
			newagg = copyObject(aggref);
			newagg->aggtype = aggtranstype;

			newtle = makeTargetEntry((Expr *) newagg,
									 list_length(context->newtlist) + 1,
									 NULL,
									 false);
			context->newtlist = lappend(context->newtlist, newtle);
		}

		return false;
	}
	/*
	 * If expression is in the subtlist copy it into new tlist
	 */
	tle = tlist_member(node, context->subtlist);
	if (tle && !tlist_member((Node *) tle->expr, context->newtlist))
	{
		TargetEntry *newtle;
		newtle = makeTargetEntry((Expr *) copyObject(node),
								 list_length(context->newtlist) + 1,
								 tle->resname,
								 false);
		context->newtlist = lappend(context->newtlist, newtle);
		return false;
	}
	if (IsA(node, Var))
	{
		/*
		 * Referenced Var is not a member of subtlist.
		 * Go ahead and add junk one.
		 */
		TargetEntry *newtle;
		newtle = makeTargetEntry((Expr *) copyObject(node),
								 list_length(context->newtlist) + 1,
								 NULL,
								 true);
		context->newtlist = lappend(context->newtlist, newtle);
		return false;
	}
	return expression_tree_walker(node, find_referenced_cols_walker,
								  (void *) context);
}
#endif


Agg *
make_agg(PlannerInfo *root, List *tlist, List *qual,
		 AggStrategy aggstrategy, const AggClauseCosts *aggcosts,
		 int numGroupCols, AttrNumber *grpColIdx, Oid *grpOperators,
		 long numGroups,
		 Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	Path		agg_path;		/* dummy for result of cost_agg */
	QualCost	qual_cost;
#ifdef XCP
	RemoteSubplan *pushdown;
#endif

	node->aggstrategy = aggstrategy;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->numGroups = numGroups;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_agg(&agg_path, root,
			 aggstrategy, aggcosts,
			 numGroupCols, numGroups,
			 lefttree->startup_cost,
			 lefttree->total_cost,
			 lefttree->plan_rows);
	plan->startup_cost = agg_path.startup_cost;
	plan->total_cost = agg_path.total_cost;

	/*
	 * We will produce a single output tuple if not grouping, and a tuple per
	 * group otherwise.
	 */
	if (aggstrategy == AGG_PLAIN)
		plan->plan_rows = 1;
	else
		plan->plan_rows = numGroups;

	/*
	 * We also need to account for the cost of evaluation of the qual (ie, the
	 * HAVING clause) and the tlist.  Note that cost_qual_eval doesn't charge
	 * anything for Aggref nodes; this is okay since they are really
	 * comparable to Vars.
	 *
	 * See notes in grouping_planner about why only make_agg, make_windowagg
	 * and make_group worry about tlist eval cost.
	 */
	if (qual)
	{
		cost_qual_eval(&qual_cost, qual, root);
		plan->startup_cost += qual_cost.startup;
		plan->total_cost += qual_cost.startup;
		plan->total_cost += qual_cost.per_tuple * plan->plan_rows;
	}
	cost_qual_eval(&qual_cost, tlist, root);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

#ifdef XCP
	/*
	 * If lefttree is a distributed subplan we may optimize aggregates by
	 * pushing down transition phase to remote data notes, and therefore reduce
	 * traffic and distribute evaluation load.
	 * We need to find all Var and Aggref expressions in tlist and qual and make
	 * up a new tlist from these expressions. Update original Vars.
	 * Create new Agg node with the new tlist and aggdistribution AGG_SLAVE.
	 * Set new Agg node as a lefttree of the distributed subplan, moving
	 * existing lefttree down under the new Agg node. Set new tlist to the
	 * distributed subplan - it should be matching to the subquery.
	 * Set node's aggdistribution to AGG_MASTER and continue node initialization
	 */
	pushdown = find_push_down_plan(lefttree, true);
	if (pushdown)
	{
		find_referenced_cols_context context;

		context.subtlist = pushdown->scan.plan.targetlist;
		context.newtlist = NIL;
		if (find_referenced_cols_walker((Node *) tlist, &context) ||
				find_referenced_cols_walker((Node *) qual, &context))
		{
			/*
			 * We found we can not push down this aggregate, clean up and
			 * fallback to default procedure
			 */
			node->aggdistribution = AGG_ONENODE;
		}
		else
		{
			Agg		   *phase1 = makeNode(Agg);
			Plan	   *plan1 = &phase1->plan;
			int			i;

			phase1->aggdistribution = AGG_SLAVE;
			phase1->aggstrategy = aggstrategy;
			phase1->numCols = numGroupCols;
			phase1->grpColIdx = grpColIdx;
			phase1->grpOperators = grpOperators;
			phase1->numGroups = numGroups;

			/*
			 * If we perform grouping we should make sure the grouping
			 * expressions are in the new tlist, and we should update indexes
			 * for the Phase2 aggregation node
			 */
			if (numGroupCols > 0)
			{
				AttrNumber *newGrpColIdx;
				newGrpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber)
														 * numGroupCols);
				for (i = 0; i < numGroupCols; i++)
				{
					TargetEntry *tle;
					TargetEntry *newtle;

					tle = (TargetEntry *) list_nth(context.subtlist,
												   grpColIdx[i] - 1);
					newtle = tlist_member((Node *) tle->expr, context.newtlist);
					if (newtle == NULL)
					{
						newtle = makeTargetEntry((Expr *) copyObject(tle->expr),
											 list_length(context.newtlist) + 1,
												 tle->resname,
												 false);
						context.newtlist = lappend(context.newtlist, newtle);
					}
					newGrpColIdx[i] = newtle->resno;
				}
				node->grpColIdx = newGrpColIdx;
			}

			/*
			 * If the pushdown plan is sorting update sort column indexes
			 */
			if (pushdown->sort)
			{
				SimpleSort *ssort = pushdown->sort;
				for (i = 0; i < ssort->numCols; i++)
				{
					TargetEntry *tle;
					TargetEntry *newtle;

					tle = (TargetEntry *) list_nth(context.subtlist,
												   grpColIdx[i] - 1);
					newtle = tlist_member((Node *) tle->expr, context.newtlist);
					if (newtle == NULL)
					{
						/* XXX maybe we should just remove the sort key ? */
						newtle = makeTargetEntry((Expr *) copyObject(tle->expr),
											 list_length(context.newtlist) + 1,
												 tle->resname,
												 false);
						context.newtlist = lappend(context.newtlist, newtle);
					}
					ssort->sortColIdx[i] = newtle->resno;
				}
			}

			copy_plan_costsize(plan1, (Plan *) pushdown); // ???

			/*
			 * We will produce a single output tuple if not grouping, and a tuple per
			 * group otherwise.
			 */
			if (aggstrategy == AGG_PLAIN)
				plan1->plan_rows = 1;
			else
				plan1->plan_rows = numGroups;

			plan1->targetlist = context.newtlist;
			plan1->qual = NIL;
			plan1->lefttree = pushdown->scan.plan.lefttree;
			pushdown->scan.plan.lefttree = plan1;
			plan1->righttree = NULL;

			/*
			 * Update target lists of all plans from lefttree till phase1.
			 * All they should be the same if the tree is transparent for push
			 * down modification.
			 */
			while (lefttree != plan1)
			{
				lefttree->targetlist = context.newtlist;
				lefttree = lefttree->lefttree;
			}

			node->aggdistribution = AGG_MASTER;
		}
	}
	else
		node->aggdistribution = AGG_ONENODE;
#endif

	return node;
}

WindowAgg *
make_windowagg(PlannerInfo *root, List *tlist,
			   List *windowFuncs, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Plan *lefttree)
{
	WindowAgg  *node = makeNode(WindowAgg);
	Plan	   *plan = &node->plan;
	Path		windowagg_path; /* dummy for result of cost_windowagg */
	QualCost	qual_cost;

	node->winref = winref;
	node->partNumCols = partNumCols;
	node->partColIdx = partColIdx;
	node->partOperators = partOperators;
	node->ordNumCols = ordNumCols;
	node->ordColIdx = ordColIdx;
	node->ordOperators = ordOperators;
	node->frameOptions = frameOptions;
	node->startOffset = startOffset;
	node->endOffset = endOffset;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_windowagg(&windowagg_path, root,
				   windowFuncs, partNumCols, ordNumCols,
				   lefttree->startup_cost,
				   lefttree->total_cost,
				   lefttree->plan_rows);
	plan->startup_cost = windowagg_path.startup_cost;
	plan->total_cost = windowagg_path.total_cost;

	/*
	 * We also need to account for the cost of evaluation of the tlist.
	 *
	 * See notes in grouping_planner about why only make_agg, make_windowagg
	 * and make_group worry about tlist eval cost.
	 */
	cost_qual_eval(&qual_cost, tlist, root);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	/* WindowAgg nodes never have a qual clause */
	plan->qual = NIL;

	return node;
}

Group *
make_group(PlannerInfo *root,
		   List *tlist,
		   List *qual,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   Oid *grpOperators,
		   double numGroups,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;
	Path		group_path;		/* dummy for result of cost_group */
	QualCost	qual_cost;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_group(&group_path, root,
			   numGroupCols, numGroups,
			   lefttree->startup_cost,
			   lefttree->total_cost,
			   lefttree->plan_rows);
	plan->startup_cost = group_path.startup_cost;
	plan->total_cost = group_path.total_cost;

	/* One output tuple per estimated result group */
	plan->plan_rows = numGroups;

	/*
	 * We also need to account for the cost of evaluation of the qual (ie, the
	 * HAVING clause) and the tlist.
	 *
	 * XXX this double-counts the cost of evaluation of any expressions used
	 * for grouping, since in reality those will have been evaluated at a
	 * lower plan level and will only be copied by the Group node. Worth
	 * fixing?
	 *
	 * See notes in grouping_planner about why only make_agg, make_windowagg
	 * and make_group worry about tlist eval cost.
	 */
	if (qual)
	{
		cost_qual_eval(&qual_cost, qual, root);
		plan->startup_cost += qual_cost.startup;
		plan->total_cost += qual_cost.startup;
		plan->total_cost += qual_cost.per_tuple * plan->plan_rows;
	}
	cost_qual_eval(&qual_cost, tlist, root);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist items
 * that should be considered by the Unique filter.	The input path must
 * already be sorted accordingly.
 */
Unique *
make_unique(Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	ListCell   *slitem;
#ifdef XCP
	RemoteSubplan *pushdown;
#endif

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.	(XXX probably this is
	 * an overestimate.)
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * numCols;

	/*
	 * plan->plan_rows is left as a copy of the input subplan's plan_rows; ie,
	 * we assume the filter removes nothing.  The caller must alter this if he
	 * has a better idea.
	 */

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = sortcl->eqop;
		Assert(OidIsValid(uniqOperators[keyno]));
		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;

#ifdef XCP
	/*
	 * We want to filter out duplicates on nodes to reduce amount of data sent
	 * over network and reduce coordinator load.
	 */
	pushdown = find_push_down_plan(lefttree, true);
	if (pushdown)
	{
		Unique	   *node1 = makeNode(Unique);
		Plan	   *plan1 = &node1->plan;

		copy_plan_costsize(plan1, pushdown->scan.plan.lefttree);
		plan1->targetlist = pushdown->scan.plan.lefttree->targetlist;
		plan1->qual = NIL;
		plan1->lefttree = pushdown->scan.plan.lefttree;
		pushdown->scan.plan.lefttree = plan1;
		plan1->righttree = NULL;

		node1->numCols = numCols;
		node1->uniqColIdx = uniqColIdx;
		node1->uniqOperators = uniqOperators;
	}
#endif

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist
 * items that should be considered by the SetOp filter.  The input path must
 * already be sorted accordingly.
 */
SetOp *
make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups, double outputRows)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	Oid		   *dupOperators;
	ListCell   *slitem;

	copy_plan_costsize(plan, lefttree);
	plan->plan_rows = outputRows;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.
	 */
	plan->total_cost += cpu_operator_cost * lefttree->plan_rows * numCols;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		dupColIdx[keyno] = tle->resno;
		dupOperators[keyno] = sortcl->eqop;
		Assert(OidIsValid(dupOperators[keyno]));
		keyno++;
	}

	node->cmd = cmd;
	node->strategy = strategy;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->dupOperators = dupOperators;
	node->flagColIdx = flagColIdx;
	node->firstFlag = firstFlag;
	node->numGroups = numGroups;

	return node;
}

/*
 * make_lockrows
 *	  Build a LockRows plan node
 */
LockRows *
make_lockrows(Plan *lefttree, List *rowMarks, int epqParam)
{
	LockRows   *node = makeNode(LockRows);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/* charge cpu_tuple_cost to reflect locking costs (underestimate?) */
	plan->total_cost += cpu_tuple_cost * plan->plan_rows;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	return node;
}

/*
 * Note: offset_est and count_est are passed in to save having to repeat
 * work already done to estimate the values of the limitOffset and limitCount
 * expressions.  Their values are as returned by preprocess_limit (0 means
 * "not relevant", -1 means "couldn't estimate").  Keep the code below in sync
 * with that function!
 */
Limit *
make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount,
		   int64 offset_est, int64 count_est)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;
#ifdef XCP
	RemoteSubplan *pushdown;
#endif

	copy_plan_costsize(plan, lefttree);

	/*
	 * Adjust the output rows count and costs according to the offset/limit.
	 * This is only a cosmetic issue if we are at top level, but if we are
	 * building a subquery then it's important to report correct info to the
	 * outer planner.
	 *
	 * When the offset or count couldn't be estimated, use 10% of the
	 * estimated number of rows emitted from the subplan.
	 */
	if (offset_est != 0)
	{
		double		offset_rows;

		if (offset_est > 0)
			offset_rows = (double) offset_est;
		else
			offset_rows = clamp_row_est(lefttree->plan_rows * 0.10);
		if (offset_rows > plan->plan_rows)
			offset_rows = plan->plan_rows;
		if (plan->plan_rows > 0)
			plan->startup_cost +=
				(plan->total_cost - plan->startup_cost)
				* offset_rows / plan->plan_rows;
		plan->plan_rows -= offset_rows;
		if (plan->plan_rows < 1)
			plan->plan_rows = 1;
	}

	if (count_est != 0)
	{
		double		count_rows;

		if (count_est > 0)
			count_rows = (double) count_est;
		else
			count_rows = clamp_row_est(lefttree->plan_rows * 0.10);
		if (count_rows > plan->plan_rows)
			count_rows = plan->plan_rows;
		if (plan->plan_rows > 0)
			plan->total_cost = plan->startup_cost +
				(plan->total_cost - plan->startup_cost)
				* count_rows / plan->plan_rows;
		plan->plan_rows = count_rows;
		if (plan->plan_rows < 1)
			plan->plan_rows = 1;
	}

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;

#ifdef XCP
	if ((limitOffset == NULL || offset_est > 0) &&
			(limitCount == NULL || count_est > 0))
	{
		/*
		 * We may reduce amount of rows sent over the network and do not send more
		 * rows then necessary
		 */
		pushdown = find_push_down_plan(lefttree, true);
		if (pushdown)
		{
			Limit	   *node1 = makeNode(Limit);
			Plan	   *plan1 = &node1->plan;

			copy_plan_costsize(plan1, pushdown->scan.plan.lefttree);
			plan1->targetlist = pushdown->scan.plan.lefttree->targetlist;
			plan1->qual = NIL;
			plan1->lefttree = pushdown->scan.plan.lefttree;
			pushdown->scan.plan.lefttree = plan1;
			plan1->righttree = NULL;

			node1->limitOffset = NULL;
			node1->limitCount = (Node *) makeConst(INT8OID, -1,
												   InvalidOid,
												   sizeof(int64),
									   Int64GetDatum(offset_est + count_est),
												   false, FLOAT8PASSBYVAL);
		}
	}
#endif

	return node;
}

/*
 * make_result
 *	  Build a Result plan node
 *
 * If we have a subplan, assume that any evaluation costs for the gating qual
 * were already factored into the subplan's startup cost, and just copy the
 * subplan cost.  If there's no subplan, we should include the qual eval
 * cost.  In either case, tlist eval cost is not to be included here.
 */
Result *
make_result(PlannerInfo *root,
			List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	if (subplan)
		copy_plan_costsize(plan, subplan);
	else
	{
		plan->startup_cost = 0;
		plan->total_cost = cpu_tuple_cost;
		plan->plan_rows = 1;	/* wrong if we have a set-valued function? */
		plan->plan_width = 0;	/* XXX is it worth being smarter? */
		if (resconstantqual)
		{
			QualCost	qual_cost;

			cost_qual_eval(&qual_cost, (List *) resconstantqual, root);
			/* resconstantqual is evaluated once at startup */
			plan->startup_cost += qual_cost.startup + qual_cost.per_tuple;
			plan->total_cost += qual_cost.startup + qual_cost.per_tuple;
		}
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

#ifdef XCP
	if (subplan)
	{
		/*
		 * We do not gain performance when pushing down Result, but Result on
		 * top of RemoteSubplan would not allow to push down other plan nodes
		 */
		RemoteSubplan *pushdown;
		pushdown = find_push_down_plan(subplan, true);
		if (pushdown)
		{
			/*
			 * Avoid pushing down results if the RemoteSubplan performs merge
			 * sort.
			 */
			if (pushdown->sort)
				return node;

			/*
			 * If remote subplan is generating distribution we should keep it
			 * correct. Set valid expression as a distribution key.
			 */
			if (pushdown->distributionKey != InvalidAttrNumber)
			{
				ListCell	   *lc;
				TargetEntry    *key;

				key = list_nth(pushdown->scan.plan.targetlist,
							   pushdown->distributionKey);
				pushdown->distributionKey = InvalidAttrNumber;
				foreach(lc, tlist)
				{
					TargetEntry    *tle = (TargetEntry *) lfirst(lc);
					if (equal(tle->expr, key->expr))
					{
						pushdown->distributionKey = tle->resno;
						break;
					}
				}

				if (pushdown->distributionKey != InvalidAttrNumber)
				{
					/* Not found, adding */
					TargetEntry    *newtle;
					/*
					 * The target entry is *NOT* junk to ensure it is not
					 * filtered out before sending from the data node.
					 */
					newtle = makeTargetEntry(copyObject(key->expr),
											 list_length(tlist) + 1,
											 key->resname,
											 false);
					tlist = lappend(tlist, newtle);
					/* just in case if it was NIL */
					plan->targetlist = tlist;
					pushdown->distributionKey = newtle->resno;
				}
			}
			/* This will be set as lefttree of the Result plan */
			plan->lefttree = pushdown->scan.plan.lefttree;
			pushdown->scan.plan.lefttree = plan;
			/* Now RemoteSubplan returns different values */
			pushdown->scan.plan.targetlist = tlist;
			return (Result *) subplan;
		}
	}
#endif /* XCP */
	return node;
}

/*
 * make_modifytable
 *	  Build a ModifyTable plan node
 *
 * Currently, we don't charge anything extra for the actual table modification
 * work, nor for the RETURNING expressions if any.	It would only be window
 * dressing, since these are always top-level nodes and there is no way for
 * the costs to change any higher-level planning choices.  But we might want
 * to make it look better sometime.
 */
ModifyTable *
make_modifytable(CmdType operation, bool canSetTag,
				 List *resultRelations,
				 List *subplans, List *returningLists,
				 List *rowMarks, int epqParam)
{
	ModifyTable *node = makeNode(ModifyTable);
	Plan	   *plan = &node->plan;
	double		total_size;
	ListCell   *subnode;

	Assert(list_length(resultRelations) == list_length(subplans));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	/*
	 * Compute cost as sum of subplan costs.
	 */
	plan->startup_cost = 0;
	plan->total_cost = 0;
	plan->plan_rows = 0;
	total_size = 0;
	foreach(subnode, subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(subnode);

		if (subnode == list_head(subplans))		/* first node? */
			plan->startup_cost = subplan->startup_cost;
		plan->total_cost += subplan->total_cost;
		plan->plan_rows += subplan->plan_rows;
		total_size += subplan->plan_width * subplan->plan_rows;
	}
	if (plan->plan_rows > 0)
		plan->plan_width = rint(total_size / plan->plan_rows);
	else
		plan->plan_width = 0;

	node->plan.lefttree = NULL;
	node->plan.righttree = NULL;
	node->plan.qual = NIL;

	/*
	 * Set up the visible plan targetlist as being the same as the first
	 * RETURNING list.	This is for the use of EXPLAIN; the executor won't pay
	 * any attention to the targetlist.
	 */
	if (returningLists)
		node->plan.targetlist = copyObject(linitial(returningLists));
	else
		node->plan.targetlist = NIL;

	node->operation = operation;
	node->canSetTag = canSetTag;
	node->resultRelations = resultRelations;
	node->resultRelIndex = -1;	/* will be set correctly in setrefs.c */
	node->plans = subplans;
	node->returningLists = returningLists;
	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	return node;
}

/*
 * is_projection_capable_plan
 *		Check whether a given Plan node is able to do projection.
 */
bool
is_projection_capable_plan(Plan *plan)
{
	/* Most plan types can project, so just list the ones that can't */
	switch (nodeTag(plan))
	{
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_Append:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
#ifdef XCP
		/*
		 * Remote subplan may push down projection to the data nodes if do not
		 * performs merge sort
		 */
		case T_RemoteSubplan:
			return ((RemoteSubplan *) plan)->sort == NULL &&
					is_projection_capable_plan(plan->lefttree);
#endif
		default:
			break;
	}
	return true;
}


#ifdef XCP
#define CNAME_MAXLEN 32
static int cursor_id = 0;


char *
get_internal_cursor(void)
{
	char *cursor;

	cursor = (char *) palloc(CNAME_MAXLEN);
	if (cursor_id++ == INT_MAX)
		cursor_id = 0;

	snprintf(cursor, CNAME_MAXLEN - 1, "p_%x", cursor_id);
	return cursor;
}
#endif


#ifdef PGXC
#ifndef XCP
/*
 * findReferencedVars()
 *
 *	Constructs a list of those Vars in targetlist which are found in
 *  parent_vars (in other words, the intersection of targetlist and
 *  parent_vars).  Returns a new list in *out_tlist and a bitmap of
 *  those relids found in the result.
 *
 *  Additionally do look at the qual references to other vars! They
 *  also need to be selected..
 */
static void
findReferencedVars(List *parent_vars, Plan *plan, List **out_tlist, Relids *out_relids)
{
	List	 *vars;
	Relids	  relids = NULL;
	List     *tlist  = NIL;
	ListCell *l;

	/* Pull vars from both the targetlist and the clauses attached to this plan */
	vars = pull_var_clause((Node *)plan->targetlist, PVC_REJECT_PLACEHOLDERS);

	foreach(l, vars)
	{
		Var	*var = lfirst(l);

		if (search_tlist_for_var(var, parent_vars))
			tlist = lappend(tlist, var);

		if (!bms_is_member(var->varno, relids))
			relids = bms_add_member(relids, var->varno);
	}

	/* Now consider the local quals */
	vars = pull_var_clause((Node *)plan->qual, PVC_REJECT_PLACEHOLDERS);

	foreach(l, vars)
	{
		Var	*var = lfirst(l);

		if (search_tlist_for_var(var, tlist) == NULL)
			tlist = lappend(tlist, var);

		if (!bms_is_member(var->varno, relids))
			relids = bms_add_member(relids, var->varno);
	}

	*out_tlist	= tlist;
	*out_relids = relids;
}

/*
 * create_remoteinsert_plan()
 *
 * Dummy
 */
Plan *
create_remoteinsert_plan(PlannerInfo *root, Plan *topplan)
{
	return topplan;
}


/*
 * create_remoteupdate_plan()
 *
 * Dummy
 */
Plan *
create_remoteupdate_plan(PlannerInfo *root, Plan *topplan)
{
	return topplan;
}

/*
 * create_remotedelete_plan()
 *
 * Builds up a final node of the plan executing DELETE command.
 *
 * If target table is on coordinator (like catalog tables) the plan is left
 * unchanged and delete will be handled using standard postgres procedure.
 *
 * If topmost node of the plan is a RemoteQuery the step query looks like
 * SELECT ctid FROM target_table WHERE condition, and we should convert it to
 * DELETE FROM target_table WHERE condition.
 *
 * In correlated case the step query looks like
 * SELECT target_table.ctid FROM target_table, other_tables WHERE condition, and
 * we should convert it to DELETE FROM target_table USING other_tables WHERE condition.
 *
 * XXX Is it ever possible if the topmost node is not a RemoteQuery?
 */
Plan *
create_remotedelete_plan(PlannerInfo *root, Plan *topplan)
{
	Query 		   *parse = root->parse;
	RangeTblEntry  *ttab;
	RelationLocInfo *rel_loc_info;
	RemoteQuery	   *fstep;
	StringInfo		buf;
	Oid				nspid;
	char		   *nspname;
	Var 		   *ctid;


	/* Get target table */
	ttab = (RangeTblEntry *) list_nth(parse->rtable, parse->resultRelation - 1);
	/* Bad relation ? */
	if (ttab == NULL || ttab->rtekind != RTE_RELATION)
		return topplan;

	/* Get location info of the target table */
	rel_loc_info = GetRelationLocInfo(ttab->relid);
	if (rel_loc_info == NULL)
		return topplan;

	buf = makeStringInfo();

	/* Compose DELETE FROM target_table */
	nspid = get_rel_namespace(ttab->relid);
	nspname = get_namespace_name(nspid);

	appendStringInfo(buf, "DELETE FROM %s.%s", quote_identifier(nspname),
					 quote_identifier(ttab->relname));

	/* See if we can push down DELETE */
	if (IsA(topplan, RemoteQuery))
	{
		char *query;

		fstep = (RemoteQuery *) topplan;
		query = fstep->sql_statement;

		if (strncmp(query, "SELECT ctid", 11) == 0)
		{
			/*
			 * Single table case
			 * We need to find position of the WHERE keyword in the string and
			 * append to the buffer part of original string starting from the
			 * position found. It is possible WHERE clause is absent (DELETE ALL)
			 * In this case buffer already has new step query
			 */
			char *where = strstr(query, " WHERE ");
			if (where)
				appendStringInfoString(buf, where);
		}
		else
		{
			/*
			 * Multi table case
			 * Assuming the RemoteQuery is created in create_remotejoin_plan().
			 * If the final RemoteQuery is for correlated delete outer_statement
			 * is just a SELECT FROM target_table, outer_statement is correlated
			 * part and we can put it into USING clause.
			 * Join type should be plain jon (comma-separated list) and all
			 * conditions are in WHERE clause.
			 * No GROUP BY or ORDER BY clauses expected.
			 * If create_remotejoin_plan is modified the code below should be
			 * revisited.
			 */
			/*
			 * In expressions target table is referenced as outer_alias, append
			 * alias name before USING clause
			 */
			appendStringInfo(buf, " %s USING ", fstep->outer_alias);

			/* Make up USING clause */
			appendStringInfo(buf, "(%s) %s ", fstep->inner_statement, fstep->inner_alias);

			/* Append WHERE clause */
			appendStringInfoString(buf, fstep->join_condition);
		}

		/* Replace step query */
		pfree(fstep->sql_statement);
		fstep->sql_statement = pstrdup(buf->data);
		/* set combine_type, it is COMBINE_TYPE_NONE for SELECT */
		fstep->combine_type = rel_loc_info->locatorType == LOCATOR_TYPE_REPLICATED ?
								  COMBINE_TYPE_SAME : COMBINE_TYPE_SUM;
		fstep->read_only = false;

		pfree(buf->data);
		pfree(buf);

		return topplan;
	}

	/*
	 * Top plan will return CTIDs and we should delete tuples with these CTIDs
	 * on the nodes. To determine target node
	 */
	fstep = make_remotequery(NIL, ttab, NIL, ttab->relid);

	if (rel_loc_info->locatorType == LOCATOR_TYPE_REPLICATED)
	{
		/*
		 * For replicated case we need two extra steps. One is to determine
		 * all values by CTID on the node from which the tuple has come, next
		 * is to remove all rows with these values on all nodes
		 */
		RemoteQuery	   *xstep;
		List 		   *xtlist = NIL;
		StringInfo 		xbuf = makeStringInfo();
		int 			natts = get_relnatts(ttab->relid);
		int 			att;

		appendStringInfoString(xbuf, "SELECT ");
		appendStringInfoString(buf, " WHERE");

		/*
		 * Populate projections of the extra SELECT step and WHERE clause of
		 * the final DELETE step
		 */
		for (att = 1; att <= natts; att++)
		{
			TargetEntry *tle;
			Var *expr;
			HeapTuple tp;

			tp = SearchSysCache(ATTNUM,
								ObjectIdGetDatum(ttab->relid),
								Int16GetDatum(att),
								0, 0);
			if (HeapTupleIsValid(tp))
			{
				Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);

				/* Add comma before all except first attributes */
				if (att > 1)
				{
					appendStringInfoString(xbuf, ", ");
					appendStringInfoString(buf, " AND");
				}
				appendStringInfoString(xbuf, NameStr(att_tup->attname));
				appendStringInfo(buf, " %s = $%d", NameStr(att_tup->attname), att);

				expr = makeVar(att, att, att_tup->atttypid,
							   att_tup->atttypmod, InvalidOid, 0);
				tle = makeTargetEntry((Expr *) expr, att,
									  NameStr(att_tup->attname), false);
				xtlist = lappend(xtlist, tle);
				ReleaseSysCache(tp);
			}
			else
				elog(ERROR, "cache lookup failed for attribute %d of relation %u",
					 att, ttab->relid);
		}

		/* Complete SELECT command */
		appendStringInfo(xbuf, " FROM %s.%s WHERE ctid = $1",
						 quote_identifier(nspname),
						 quote_identifier(ttab->relname));

		/* Build up the extra select step */
		xstep = make_remotequery(xtlist, ttab, NIL, ttab->relid);
		innerPlan(xstep) = topplan;
		xstep->sql_statement = pstrdup(xbuf->data);
		xstep->read_only = true;
		xstep->exec_nodes = makeNode(ExecNodes);
		xstep->exec_nodes->baselocatortype = rel_loc_info->locatorType;
		xstep->exec_nodes->tableusagetype = TABLE_USAGE_TYPE_USER;
		xstep->exec_nodes->primarynodelist = NULL;
		xstep->exec_nodes->nodelist = NULL;
		xstep->exec_nodes->en_relid = ttab->relid;
		xstep->exec_nodes->accesstype = RELATION_ACCESS_READ;

		/* First and only target entry of topplan is ctid, reference it */
		ctid = makeVar(INNER, 1, TIDOID, -1, InvalidOid, 0);
		xstep->exec_nodes->en_expr = (Expr *) ctid;

		pfree(xbuf->data);
		pfree(xbuf);

		/* Build up the final delete step */
		innerPlan(fstep) = (Plan *) xstep;
		fstep->sql_statement = pstrdup(buf->data);
		fstep->combine_type = COMBINE_TYPE_SAME;
		fstep->read_only = false;
		fstep->exec_nodes = GetRelationNodes(rel_loc_info, 0, UNKNOWNOID, RELATION_ACCESS_UPDATE);
	}
	else
	{
		/* Build up the final delete step */
		innerPlan(fstep) = topplan;
		appendStringInfoString(buf, " WHERE ctid = $1");
		fstep->sql_statement = pstrdup(buf->data);
		fstep->combine_type = COMBINE_TYPE_SUM;
		fstep->read_only = false;
		fstep->exec_nodes = makeNode(ExecNodes);
		fstep->exec_nodes->baselocatortype = rel_loc_info->locatorType;
		fstep->exec_nodes->tableusagetype = TABLE_USAGE_TYPE_USER;
		fstep->exec_nodes->primarynodelist = NULL;
		fstep->exec_nodes->nodelist = NULL;
		fstep->exec_nodes->en_relid = ttab->relid;
		fstep->exec_nodes->accesstype = RELATION_ACCESS_UPDATE;

		/* First and only target entry of topplan is ctid, reference it */
		ctid = makeVar(INNER, 1, TIDOID, -1, InvalidOid, 0);
		fstep->exec_nodes->en_expr = (Expr *) ctid;
	}

	pfree(buf->data);
	pfree(buf);

	return (Plan *) fstep;
}


/*
 * create_remotegrouping_plan
 * Check if the grouping and aggregates can be pushed down to the
 * datanodes.
 * Right now we can push with following restrictions
 * 1. there are plain aggregates (no expressions involving aggregates) and/or
 *    expressions in group by clauses
 * 2. No distinct or order by clauses
 * 3. No windowing clause
 * 4. No having clause
 *
 * Inputs
 * root - planerInfo root for this query
 * agg_plan - local grouping plan produced by grouping_planner()
 *
 * PGXCTODO: work on reducing these restrictions as much or document the reasons
 * why we need the restrictions, in these comments themselves. In case of
 * replicated tables, we should be able to push the whole query to the data
 * node in case there are no local clauses.
 */
Plan *
create_remotegrouping_plan(PlannerInfo *root, Plan *local_plan)
{
	Query		*query = root->parse;
	Sort		*sort_plan;
	RemoteQuery	*remote_scan;	/* remote query in the passed in plan */
	RemoteQuery	*remote_group;	/* remote query after optimization */
	Plan		*remote_group_plan;	/* plan portion of remote_group */
	Plan		*temp_plan;
	List		*temp_vars;			/* temporarily hold the VARs */
	List		*temp_vartlist;		/* temporarity hold tlist of VARs */
	ListCell	*temp;
	StringInfo	remote_targetlist;/* SELECT clause of remote query */
	StringInfo	remote_sql_stmt;
	StringInfo	groupby_clause;	/* remote query GROUP BY */
	StringInfo	orderby_clause;	/* remote query ORDER BY */
	StringInfo	remote_fromlist;	/* remote query FROM */
	StringInfo	in_alias;
	StringInfo	having_clause;	/* remote query HAVING clause */
	Relids		in_relids;			/* the list of Relids referenced by lefttree */
	Index		dummy_rtindex;
	List		*base_tlist;
	RangeTblEntry	*dummy_rte;
	int			numGroupCols;
	AttrNumber	*grpColIdx;
	bool		reduce_plan;
	List		*remote_qual;
	List 		*local_qual;

	/*
	 * We don't push aggregation and grouping to datanodes, in case there are
	 * windowing aggregates, distinct, having clause or sort clauses.
	 */
	if (query->hasWindowFuncs ||
		query->distinctClause ||
		query->sortClause)
		return local_plan;

	/* for now only Agg/Group plans */
	if (local_plan && IsA(local_plan, Agg))
	{
		numGroupCols = ((Agg *)local_plan)->numCols;
		grpColIdx = ((Agg *)local_plan)->grpColIdx;
	}
	else if (local_plan && IsA(local_plan, Group))
	{
		numGroupCols = ((Group *)local_plan)->numCols;
		grpColIdx = ((Group *)local_plan)->grpColIdx;
	}
	else
		return local_plan;

	/*
	 * We expect plan tree as Group/Agg->Sort->Result->Material->RemoteQuery,
	 * Result, Material nodes are optional. Sort is compulsory for Group but not
	 * for Agg.
	 * anything else is not handled right now.
	 */
	temp_plan = local_plan->lefttree;
	remote_scan = NULL;
	sort_plan = NULL;
	if (temp_plan && IsA(temp_plan, Sort))
	{
		sort_plan = (Sort *)temp_plan;
		temp_plan = temp_plan->lefttree;
	}
	if (temp_plan && IsA(temp_plan, Result))
		temp_plan = temp_plan->lefttree;
	if (temp_plan && IsA(temp_plan, Material))
		temp_plan = temp_plan->lefttree;
	if (temp_plan && IsA(temp_plan, RemoteQuery))
		remote_scan = (RemoteQuery *)temp_plan;

	if (!remote_scan)
		return local_plan;
	/*
	 * for Group plan we expect Sort under the Group, which is always the case,
	 * the condition below is really for some possibly non-existent case.
	 */
	if (IsA(local_plan, Group) && !sort_plan)
		return local_plan;
	/*
	 * If the remote_scan has any quals on it, those need to be executed before
	 * doing anything. Hence we won't be able to push any aggregates or grouping
	 * to the data node.
	 * If it has any SimpleSort in it, then sorting is intended to be applied
	 * before doing anything. Hence can not push any aggregates or grouping to
	 * the data node.
	 */
	if (remote_scan->scan.plan.qual || remote_scan->sort)
		return local_plan;

	/*
	 * Grouping_planner may add Sort node to sort the rows
	 * based on the columns in GROUP BY clause. Hence the columns in Sort and
	 * those in Group node in should be same. The columns are usually in the
	 * same order in both nodes, hence check the equality in order. If this
	 * condition fails, we can not handle this plan for now.
	 */
	if (sort_plan)
	{
		int cntCols;
		if (sort_plan->numCols != numGroupCols)
			return local_plan;
		for (cntCols = 0; cntCols < numGroupCols; cntCols++)
		{
			if (sort_plan->sortColIdx[cntCols] != grpColIdx[cntCols])
				return local_plan;
		}
	}

	/*
	 * At last we find the plan underneath is reducible into a single
	 * RemoteQuery node.
	 */

	/* find all the relations referenced by targetlist of Grouping node */
	temp_vars = pull_var_clause((Node *)local_plan->targetlist,
									PVC_REJECT_PLACEHOLDERS);
	findReferencedVars(temp_vars, (Plan *)remote_scan, &temp_vartlist, &in_relids);

	/*
	 * process the targetlist of the grouping plan, also construct the
	 * targetlist of the query to be shipped to the remote side
	 */
	base_tlist = pgxc_process_grouping_targetlist(root, &(local_plan->targetlist));
	/*
	 * If can not construct a targetlist shippable to the datanode. Resort to
	 * the plan created by grouping_planner()
	 */
	if (!base_tlist)
		return local_plan;

	base_tlist = pgxc_process_having_clause(root, base_tlist, query->havingQual,
												&local_qual, &remote_qual, &reduce_plan);
	/*
	 * Because of HAVING clause, we can not push the aggregates and GROUP BY
	 * clause to the data node. Resort to the plan created by grouping planner.
	 */
	if (!reduce_plan)
		return local_plan;
	Assert(base_tlist);

	/*
	 * We are now ready to create the RemoteQuery node to push the query to
	 * datanode.
	 * 1. Create a remote query node reflecting the query to be pushed to the
	 *    datanode.
	 * 2. Modify the Grouping node passed in, to accept the results sent by the
	 *    Datanodes, then group and aggregate them, if needed.
	 */
	remote_targetlist = makeStringInfo();
	remote_sql_stmt = makeStringInfo();
	groupby_clause = makeStringInfo();
	orderby_clause = makeStringInfo();
	remote_fromlist = makeStringInfo();
	in_alias = makeStringInfo();
	having_clause = makeStringInfo();

	appendStringInfo(in_alias, "%s_%d", "group", root->rs_alias_index);

	/*
	 * Build partial RemoteQuery node to be used for creating the Select clause
	 * to be sent to the remote node. Rest of the node will be built later
	 */
	remote_group = makeNode(RemoteQuery);

	/*
	 * Save information about the plan we are reducing.
	 * We may need this information later if more entries are added to it
	 * as part of the remote expression optimization.
	 */
	remote_group->remotejoin			= false;
	remote_group->inner_alias			= pstrdup(in_alias->data);
	remote_group->inner_reduce_level	= remote_scan->reduce_level;
	remote_group->inner_relids      	= in_relids;
	remote_group->inner_statement		= pstrdup(remote_scan->sql_statement);
	remote_group->exec_nodes			= remote_scan->exec_nodes;
	/* Don't forget to increment the index for the next time around! */
	remote_group->reduce_level			= root->rs_alias_index++;

	/* Generate the select clause of the remote query */
	appendStringInfoString(remote_targetlist, "SELECT");
	foreach (temp, base_tlist)
	{
		TargetEntry *tle = lfirst(temp);
		Node		*expr = (Node *)tle->expr;

		create_remote_expr(root, local_plan, remote_targetlist, expr, remote_group);

		/* If this is not last target entry, add a comma */
		if (lnext(temp))
			appendStringInfoString(remote_targetlist, ",");
	}

	/* Generate the from clause of the remote query */
	appendStringInfo(remote_fromlist, " FROM (%s) %s",
							remote_group->inner_statement, remote_group->inner_alias);

	/*
	 * Generate group by clause for the remote query and recompute the group by
	 * column locations. We want the tuples from remote node to be ordered by
	 * the grouping columns so that ExecGroup can work without any modification,
	 * hence create a SimpleSort structure to be added to RemoteQuery (which
	 * will merge the sorted results and present to Group node in sorted
	 * manner).
	 */
	if (query->groupClause)
	{
		int cntCols;
		char *sep;

		/*
		 * recompute the column ids of the grouping columns,
		 * the group column indexes computed earlier point in the
		 * targetlists of the scan plans under this node. But now the grouping
		 * column indexes will be pointing in the targetlist of the new
		 * RemoteQuery, hence those need to be recomputed
		 */
		pgxc_locate_grouping_columns(root, base_tlist, grpColIdx);

		appendStringInfoString(groupby_clause, "GROUP BY ");
		sep = "";
		for (cntCols = 0; cntCols < numGroupCols; cntCols++)
		{
			appendStringInfo(groupby_clause, "%s%d", sep, grpColIdx[cntCols]);
			sep = ", ";
		}
		if (sort_plan)
		{
			SimpleSort		*remote_sort = makeNode(SimpleSort);
			/*
			 * reuse the arrays allocated in sort_plan to create SimpleSort
			 * structure. sort_plan is useless henceforth.
			 */
			remote_sort->numCols = sort_plan->numCols;
			remote_sort->sortColIdx = sort_plan->sortColIdx;
			remote_sort->sortOperators = sort_plan->sortOperators;
			remote_sort->nullsFirst = sort_plan->nullsFirst;
			appendStringInfoString(orderby_clause, "ORDER BY ");
			sep = "";
			for (cntCols = 0; cntCols < remote_sort->numCols; cntCols++)
			{
				remote_sort->sortColIdx[cntCols] = grpColIdx[cntCols];
				appendStringInfo(orderby_clause, "%s%d", sep,
								remote_sort->sortColIdx[cntCols]);
				sep = ", ";
			}
			remote_group->sort = remote_sort;
		}
	}

	if (remote_qual)
	{
		appendStringInfoString(having_clause, "HAVING ");
		create_remote_clause_expr(root, local_plan, having_clause, remote_qual,
							remote_group);
	}

	/* Generate the remote sql statement from the pieces */
	appendStringInfo(remote_sql_stmt, "%s %s %s %s %s", remote_targetlist->data,
						remote_fromlist->data, groupby_clause->data,
						orderby_clause->data, having_clause->data);
	/*
	 * Create a dummy RTE for the remote query being created. Append the dummy
	 * range table entry to the range table. Note that this modifies the master
	 * copy the caller passed us, otherwise e.g EXPLAIN VERBOSE will fail to
	 * find the rte the Vars built below refer to. Also create the tuple
	 * descriptor for the result of this query from the base_tlist (targetlist
	 * we used to generate the remote node query).
	 */
	dummy_rte = makeNode(RangeTblEntry);
	dummy_rte->reltupdesc = ExecTypeFromTL(base_tlist, false);
	dummy_rte->rtekind = RTE_RELATION;

	/* Use a dummy relname... */
	dummy_rte->relname	   = "__FOREIGN_QUERY__";
	dummy_rte->eref		   = makeAlias("__FOREIGN_QUERY__", NIL);

	/* Rest will be zeroed out in makeNode() */
	root->parse->rtable = lappend(root->parse->rtable, dummy_rte);
	dummy_rtindex = list_length(root->parse->rtable);

	/* Build rest of the RemoteQuery node and the plan there */
	remote_group_plan = &remote_group->scan.plan;

	/* The join targetlist becomes this node's tlist */
	remote_group_plan->targetlist = base_tlist;
	remote_group_plan->lefttree 	= NULL;
	remote_group_plan->righttree 	= NULL;
	remote_group->scan.scanrelid 	= dummy_rtindex;
	remote_group->sql_statement = remote_sql_stmt->data;

	/* set_plan_refs needs this later */
	remote_group->base_tlist		= base_tlist;
	remote_group->relname			= "__FOREIGN_QUERY__";
	remote_group->partitioned_replicated = remote_scan->partitioned_replicated;
	remote_group->read_only = query->commandType == CMD_SELECT;

	/* we actually need not worry about costs since this is the final plan */
	remote_group_plan->startup_cost	= remote_scan->scan.plan.startup_cost;
	remote_group_plan->total_cost	= remote_scan->scan.plan.total_cost;
	remote_group_plan->plan_rows	= remote_scan->scan.plan.plan_rows;
	remote_group_plan->plan_width	= remote_scan->scan.plan.plan_width;

	/*
	 * Modify the passed in grouping plan according to the remote query we built
	 * Materialization is always needed for RemoteQuery in case we need to restart
	 * the scan.
	 */
	local_plan->lefttree = (Plan *) make_material(remote_group_plan);
	local_plan->qual = local_qual;
	/* indicate that we should apply collection function directly */
	if (IsA(local_plan, Agg))
		((Agg *)local_plan)->skip_trans = true;

	return local_plan;
}

/*
 * pgxc_locate_grouping_columns
 * Locates the grouping clauses in the given target list. This is very similar
 * to locate_grouping_columns except that there is only one target list to
 * search into.
 * PGXCTODO: Can we reuse locate_grouping_columns() instead of writing this
 * function? But this function is optimized to search in the same target list.
 */
static void
pgxc_locate_grouping_columns(PlannerInfo *root, List *tlist,
								AttrNumber *groupColIdx)
{
	int			keyno = 0;
	ListCell   *gl;

	/*
	 * No work unless grouping.
	 */
	if (!root->parse->groupClause)
	{
		Assert(groupColIdx == NULL);
		return;
	}
	Assert(groupColIdx != NULL);

	foreach(gl, root->parse->groupClause)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(gl);
		TargetEntry *te = get_sortgroupclause_tle(grpcl, tlist);
		if (!te)
			elog(ERROR, "failed to locate grouping columns");
		groupColIdx[keyno++] = te->resno;
	}
}

/*
 * pgxc_add_node_to_grouping_tlist
 * Add the given node to the target list to be sent to the datanode. If it's
 * Aggref node, also change the passed in node to point to the Aggref node in
 * the data node's target list
 */
static List *
pgxc_add_node_to_grouping_tlist(List *remote_tlist, Node *expr, Index ressortgroupref)
{
	TargetEntry *remote_tle;
	Oid			saved_aggtype;

	/*
	 * When we add an aggregate to the remote targetlist the aggtype of such
	 * Aggref node is changed to aggtrantype. Hence while searching a given
	 * Aggref in remote targetlist, we need to change the aggtype accordingly
	 * and then switch it back.
	 */
	if (IsA(expr, Aggref))
	{
		Aggref *aggref = (Aggref *)expr;
		saved_aggtype = aggref->aggtype;
		aggref->aggtype = aggref->aggtrantype;
	}
	remote_tle = tlist_member(expr, remote_tlist);
	if (IsA(expr, Aggref))
		((Aggref *)expr)->aggtype = saved_aggtype;

	if (!remote_tle)
	{
		remote_tle = makeTargetEntry(copyObject(expr),
							  list_length(remote_tlist) + 1,
							  NULL,
							  false);
		/* Copy GROUP BY/SORT BY reference for the locating group by columns */
		remote_tle->ressortgroupref = ressortgroupref;
		remote_tlist = lappend(remote_tlist, remote_tle);
	}
	else
	{

		if (remote_tle->ressortgroupref == 0)
			remote_tle->ressortgroupref = ressortgroupref;
		else if (ressortgroupref == 0)
		{
			/* do nothing remote_tle->ressortgroupref has the right value */
		}
		else
		{
			/*
			 * if the expression's TLE already has a Sorting/Grouping reference,
			 * and caller has passed a non-zero one as well, better both of them
			 * be same
			 */
			Assert(remote_tle->ressortgroupref == ressortgroupref);
		}
	}

	/*
	 * Replace the args of the local Aggref with Aggref node to be
	 * included in RemoteQuery node, so that set_plan_refs can convert
	 * the args into VAR pointing to the appropriate result in the tuple
	 * coming from RemoteQuery node
	 * PGXCTODO: should we push this change in targetlists of plans
	 * above?
	 */
	if (IsA(expr, Aggref))
	{
		Aggref	*local_aggref = (Aggref *)expr;
		Aggref	*remote_aggref = (Aggref *)remote_tle->expr;
		Assert(IsA(remote_tle->expr, Aggref));
		remote_aggref->aggtype = remote_aggref->aggtrantype;
		/* Is copyObject() needed here? probably yes */
		local_aggref->args = list_make1(makeTargetEntry(copyObject(remote_tle->expr),
																1, NULL,
																false));
	}
	return remote_tlist;
}
/*
 * pgxc_process_grouping_targetlist
 * The function scans the targetlist to check if the we can push anything
 * from the targetlist to the datanode. Following rules govern the choice
 * 1. Either all of the aggregates are pushed to the datanode or none is pushed
 * 2. If there are no aggregates, the targetlist is good to be shipped as is
 * 3. If aggregates are involved in expressions, we push the aggregates to the
 *    datanodes but not the involving expressions.
 *
 * The function constructs the targetlist for the query to be pushed to the
 * datanode. It modifies the local targetlist to point to the expressions in
 * remote targetlist wherever necessary (e.g. aggregates)
 *
 * PGXCTODO: we should be careful while pushing the function expressions, it's
 * better to push functions like strlen() which can be evaluated at the
 * datanode, but we should avoid pushing functions which can only be evaluated
 * at coordinator.
 */
static List *
pgxc_process_grouping_targetlist(PlannerInfo *root, List **local_tlist)
{
	bool	shippable_remote_tlist = true;
	List	*remote_tlist = NIL;
	List	*orig_local_tlist = NIL;/* Copy original local_tlist, in case it changes */
	ListCell	*temp;

	/*
	 * Walk through the target list and find out whether we can push the
	 * aggregates and grouping to datanodes. Also while doing so, create the
	 * targetlist for the query to be shipped to the datanode. Adjust the local
	 * targetlist accordingly.
	 */
	foreach(temp, *local_tlist)
	{
		TargetEntry				*local_tle = lfirst(temp);
		Node					*expr = (Node *)local_tle->expr;
		foreign_qual_context	context;

		pgxc_foreign_qual_context_init(&context);
		/*
		 * If the expression is not Aggref but involves aggregates (has Aggref
		 * nodes in the expression tree, we can not push the entire expression
		 * to the datanode, but push those aggregates to the data node, if those
		 * aggregates can be evaluated at the data nodes (if is_foreign_expr
		 * returns true for entire expression). To evaluate the rest of the
		 * expression, we need to fetch the values of VARs participating in the
		 * expression. But, if we include the VARs under the aggregate nodes,
		 * they may not be part of GROUP BY clause, thus generating an invalid
		 * query. Hence, is_foreign_expr() wouldn't collect VARs under the
		 * expression tree rooted under Aggref node.
		 * For example, the original query is
		 * SELECT sum(val) * val2 FROM tab1 GROUP BY val2;
		 * the query pushed to the data node is
		 * SELECT sum(val), val2 FROM tab1 GROUP BY val2;
		 * Notice that, if we include val in the query, it will become invalid.
		 */
		context.collect_vars = true;

		if (!is_foreign_expr(expr, &context))
		{
				shippable_remote_tlist = false;
				break;
		}

		/*
		 * We are about to change the local_tlist, check if we have already
		 * copied original local_tlist, if not take a copy
		 */
		if (!orig_local_tlist && (IsA(expr, Aggref) || context.aggs))
				orig_local_tlist = copyObject(*local_tlist);

		/*
		 * if there are aggregates involved in the expression, whole expression
		 * can not be pushed to the data node. Pick up the aggregates and the
		 * VAR nodes not covered by aggregates.
		 */
		if (context.aggs)
		{
			ListCell				*lcell;
			/*
			 * if the target list expression is an Aggref, then the context should
			 * have only one Aggref in the list and no VARs.
			 */
			Assert(!IsA(expr, Aggref) ||
						(list_length(context.aggs) == 1 &&
						linitial(context.aggs) == expr &&
						!context.vars));
			/*
			 * this expression is not going to be pushed as whole, thus other
			 * clauses won't be able to find out this TLE in the results
			 * obtained from data node. Hence can't optimize this query.
			 */
			if (local_tle->ressortgroupref > 0)
			{
				shippable_remote_tlist = false;
				break;
			}
			/* copy the aggregates into the remote target list */
			foreach (lcell, context.aggs)
			{
				Assert(IsA(lfirst(lcell), Aggref));
				remote_tlist = pgxc_add_node_to_grouping_tlist(remote_tlist, lfirst(lcell),
																0);
			}
			/* copy the vars into the remote target list */
			foreach (lcell, context.vars)
			{
				Assert(IsA(lfirst(lcell), Var));
				remote_tlist = pgxc_add_node_to_grouping_tlist(remote_tlist, lfirst(lcell),
																0);
			}
		}
		/* Expression doesn't contain any aggregate */
		else
			remote_tlist = pgxc_add_node_to_grouping_tlist(remote_tlist, expr,
													local_tle->ressortgroupref);

		pgxc_foreign_qual_context_free(&context);
	}

	if (!shippable_remote_tlist)
	{
		/*
		 * If local_tlist has changed but we didn't find anything shippable to
		 * datanode, we need to restore the local_tlist to original state,
		 */
		if (orig_local_tlist)
			*local_tlist = orig_local_tlist;
		if (remote_tlist)
			list_free_deep(remote_tlist);
		remote_tlist = NIL;
	}
	else if (orig_local_tlist)
	{
		/*
		 * If we have changed the targetlist passed, we need to pass back the
		 * changed targetlist. Free the copy that has been created.
		 */
		list_free_deep(orig_local_tlist);
	}

	return remote_tlist;
}

/*
 * pgxc_process_having_clause
 * For every expression in the havingQual take following action
 * 1. If it has aggregates, which can be evaluated at the data nodes, add those
 *    aggregates to the targetlist and modify the local aggregate expressions to
 *    point to the aggregate expressions being pushed to the data node. Add this
 *    expression to the local qual to be evaluated locally.
 * 2. If the expression does not have aggregates and the whole expression can be
 *    evaluated at the data node, add the expression to the remote qual to be
 *    evaluated at the data node.
 * 3. If qual contains an expression which can not be evaluated at the data
 *    node, the parent group plan can not be reduced to a remote_query.
 */
static List *
pgxc_process_having_clause(PlannerInfo *root, List *remote_tlist, Node *havingQual,
												List **local_qual, List **remote_qual,
												bool *reduce_plan)
{
	foreign_qual_context	context;
	List		*qual;
	ListCell	*temp;

	*reduce_plan = true;
	*remote_qual = NIL;
	*local_qual = NIL;

	if (!havingQual)
		return remote_tlist;
	/*
	 * PGXCTODO: we expect the quals in the form of List only. Is there a
	 * possibility that the quals will be another form?
	 */
	if (!IsA(havingQual, List))
	{
		*reduce_plan = false;
		return remote_tlist;
	}
	/*
	 * Copy the havingQual so that the copy can be modified later. In case we
	 * back out in between, the original expression remains intact.
	 */
	qual = copyObject(havingQual);
	foreach(temp, qual)
	{
		Node *expr = lfirst(temp);
		pgxc_foreign_qual_context_init(&context);
		if (!is_foreign_expr(expr, &context))
		{
			*reduce_plan = false;
			break;
		}

		if (context.aggs)
		{
				ListCell				*lcell;
				/*
				 * if the target list havingQual is an Aggref, then the context should
				 * have only one Aggref in the list and no VARs.
				 */
				Assert(!IsA(expr, Aggref) ||
							(list_length(context.aggs) == 1 &&
							linitial(context.aggs) == expr &&
							!context.vars));
				/* copy the aggregates into the remote target list */
				foreach (lcell, context.aggs)
				{
					Assert(IsA(lfirst(lcell), Aggref));
					remote_tlist = pgxc_add_node_to_grouping_tlist(remote_tlist, lfirst(lcell),
																	0);
				}
				/* copy the vars into the remote target list */
				foreach (lcell, context.vars)
				{
					Assert(IsA(lfirst(lcell), Var));
					remote_tlist = pgxc_add_node_to_grouping_tlist(remote_tlist, lfirst(lcell),
																	0);
				}
				*local_qual = lappend(*local_qual, expr);
		}
		else
			*remote_qual = lappend(*remote_qual, expr);

		pgxc_foreign_qual_context_free(&context);
	}

	if (!(*reduce_plan))
		list_free_deep(qual);

	return remote_tlist;
}
#endif /* XCP */
#endif /* PGXC */
