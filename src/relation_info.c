/* ------------------------------------------------------------------------
 *
 * relation_info.c
 *		Data structures describing partitioned relations
 *
 * Copyright (c) 2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "compat/pg_compat.h"

#include "relation_info.h"
#include "init.h"
#include "utils.h"
#include "xact_handling.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/parser.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#if PG_VERSION_NUM < 90600
#include "optimizer/planmain.h"
#endif

#if PG_VERSION_NUM >= 90600
#include "catalog/pg_constraint_fn.h"
#endif


/* Comparison function info */
typedef struct cmp_func_info
{
	FmgrInfo	flinfo;
	Oid			collid;
} cmp_func_info;

/*
 * For pg_pathman.enable_bounds_cache GUC.
 */
bool			pg_pathman_enable_bounds_cache = true;


/*
 * We delay all invalidation jobs received in relcache hook.
 */
static List	   *delayed_invalidation_parent_rels = NIL;
static List	   *delayed_invalidation_vague_rels = NIL;
static bool		delayed_shutdown = false; /* pathman was dropped */


/* Add unique Oid to list, allocate in TopPathmanContext */
#define list_add_unique(list, oid) \
	do { \
		MemoryContext old_mcxt = MemoryContextSwitchTo(TopPathmanContext); \
		list = list_append_unique_oid(list, ObjectIdGetDatum(oid)); \
		MemoryContextSwitchTo(old_mcxt); \
	} while (0)

#define free_invalidation_list(list) \
	do { \
		list_free(list); \
		list = NIL; \
	} while (0)


static bool try_perform_parent_refresh(Oid parent);
static Oid try_syscache_parent_search(Oid partition, PartParentSearch *status);
static Oid get_parent_of_partition_internal(Oid partition,
											PartParentSearch *status,
											HASHACTION action);

static Expr *get_partition_constraint_expr(Oid partition);

static void fill_prel_with_partitions(PartRelationInfo *prel,
									  const Oid *partitions,
									  const uint32 parts_count);

static void fill_pbin_with_bounds(PartBoundInfo *pbin,
								  const PartRelationInfo *prel,
								  const Expr *constraint_expr);

static int cmp_range_entries(const void *p1, const void *p2, void *arg);

static PartBoundInfo *get_bounds_of_partition(Oid partition,
											  const PartRelationInfo *prel);

void
init_relation_info_static_data(void)
{
	DefineCustomBoolVariable("pg_pathman.enable_bounds_cache",
							 "Make updates of partition dispatch cache faster",
							 NULL,
							 &pg_pathman_enable_bounds_cache,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
}

/*
 * refresh\invalidate\get\remove PartRelationInfo functions.
 */

const PartRelationInfo *
refresh_pathman_relation_info(Oid relid,
							  Datum *values,
							  bool allow_incomplete)
{
	const LOCKMODE			lockmode = AccessShareLock;
	const TypeCacheEntry   *typcache;
	Oid					   *prel_children;
	uint32					prel_children_count = 0,
							i;
	PartRelationInfo	   *prel;
	Datum					param_values[Natts_pathman_config_params];
	bool					param_isnull[Natts_pathman_config_params];
	char				   *expr;
	Relids					expr_varnos;
	HeapTuple				htup;
	MemoryContext			old_mcxt;

	AssertTemporaryContext();
	prel = invalidate_pathman_relation_info(relid, NULL);
	Assert(prel);

	/* Try locking parent, exit fast if 'allow_incomplete' */
	if (allow_incomplete)
	{
		if (!ConditionalLockRelationOid(relid, lockmode))
			return NULL; /* leave an invalid entry */
	}
	else LockRelationOid(relid, lockmode);

	/* Check if parent exists */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
	{
		/* Nope, it doesn't, remove this entry and exit */
		UnlockRelationOid(relid, lockmode);
		remove_pathman_relation_info(relid);
		return NULL; /* exit */
	}

	/* Make both arrays point to NULL */
	prel->children	= NULL;
	prel->ranges	= NULL;

	/* Set partitioning type */
	prel->parttype	= DatumGetPartType(values[Anum_pathman_config_parttype - 1]);

	/* Fetch cooked partitioning expression */
	expr = TextDatumGetCString(values[Anum_pathman_config_cooked_expr - 1]);

	/* Expression and attname should be saved in cache context */
	old_mcxt = MemoryContextSwitchTo(PathmanRelationCacheContext);

	/* Build partitioning expression tree */
	prel->expr_cstr = TextDatumGetCString(values[Anum_pathman_config_expr - 1]);
	prel->expr = (Node *) stringToNode(expr);
	fix_opfuncids(prel->expr);

	expr_varnos = pull_varnos(prel->expr);
	if (bms_singleton_member(expr_varnos) != PART_EXPR_VARNO)
		elog(ERROR, "partitioning expression may reference only one table");

	/* Extract Vars and varattnos of partitioning expression */
	prel->expr_vars = NIL;
	prel->expr_atts = NULL;
	prel->expr_vars = pull_var_clause_compat(prel->expr, 0, 0);
	pull_varattnos((Node *) prel->expr_vars, PART_EXPR_VARNO, &prel->expr_atts);

	MemoryContextSwitchTo(old_mcxt);

	/* First, fetch type of partitioning expression */
	prel->ev_type	= exprType(prel->expr);

	htup = SearchSysCache1(TYPEOID, prel->ev_type);
	if (HeapTupleIsValid(htup))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(htup);
		prel->ev_typmod = typtup->typtypmod;
		prel->ev_collid = typtup->typcollation;
		ReleaseSysCache(htup);
	}
	else elog(ERROR, "cache lookup failed for type %u", prel->ev_type);

	/* Fetch HASH & CMP fuctions and other stuff from type cache */
	typcache = lookup_type_cache(prel->ev_type,
								 TYPECACHE_CMP_PROC | TYPECACHE_HASH_PROC);

	prel->ev_byval	= typcache->typbyval;
	prel->ev_len	= typcache->typlen;
	prel->ev_align	= typcache->typalign;

	prel->cmp_proc	= typcache->cmp_proc;
	prel->hash_proc	= typcache->hash_proc;

	/* Try searching for children (don't wait if we can't lock) */
	switch (find_inheritance_children_array(relid, lockmode,
											allow_incomplete,
											&prel_children_count,
											&prel_children))
	{
		/* If there's no children at all, remove this entry */
		case FCS_NO_CHILDREN:
			elog(DEBUG2, "refresh: relation %u has no children [%u]",
						 relid, MyProcPid);

			UnlockRelationOid(relid, lockmode);
			remove_pathman_relation_info(relid);
			return NULL; /* exit */

		/* If can't lock children, leave an invalid entry */
		case FCS_COULD_NOT_LOCK:
			elog(DEBUG2, "refresh: cannot lock children of relation %u [%u]",
						 relid, MyProcPid);

			UnlockRelationOid(relid, lockmode);
			return NULL; /* exit */

		/* Found some children, just unlock parent */
		case FCS_FOUND:
			elog(DEBUG2, "refresh: found children of relation %u [%u]",
						 relid, MyProcPid);

			UnlockRelationOid(relid, lockmode);
			break; /* continue */

		/* Error: unknown result code */
		default:
			elog(ERROR, "error in function "
						CppAsString(find_inheritance_children_array));
	}

	/*
	 * Fill 'prel' with partition info, raise ERROR if anything is wrong.
	 * This way PartRelationInfo will remain 'invalid', and 'get' procedure
	 * will try to refresh it again (and again), until the error is fixed
	 * by user manually (i.e. invalid check constraints etc).
	 */
	PG_TRY();
	{
		fill_prel_with_partitions(prel, prel_children, prel_children_count);
	}
	PG_CATCH();
	{
		/* Free remaining resources */
		FreeChildrenArray(prel);
		FreeRangesArray(prel);
		FreeIfNotNull(prel->expr_cstr);
		FreeIfNotNull(prel->expr);

		/* Rethrow ERROR further */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Peform some actions for each child */
	for (i = 0; i < prel_children_count; i++)
	{
		/* Add "partition+parent" pair to cache */
		cache_parent_of_partition(prel_children[i], relid);

		/* Now it's time to unlock this child */
		UnlockRelationOid(prel_children[i], lockmode);
	}

	if (prel_children)
		pfree(prel_children);

	/* Read additional parameters ('enable_parent' at the moment) */
	if (read_pathman_params(relid, param_values, param_isnull))
	{
		prel->enable_parent = param_values[Anum_pathman_config_params_enable_parent - 1];
	}
	/* Else set default values if they cannot be found */
	else
	{
		prel->enable_parent = DEFAULT_ENABLE_PARENT;
	}

	/* We've successfully built a cache entry */
	prel->valid = true;

	return prel;
}

/* Invalidate PartRelationInfo cache entry. Create new entry if 'found' is NULL. */
PartRelationInfo *
invalidate_pathman_relation_info(Oid relid, bool *found)
{
	bool				prel_found;
	HASHACTION			action = found ? HASH_FIND : HASH_ENTER;
	PartRelationInfo   *prel;

	prel = pathman_cache_search_relid(partitioned_rels,
									  relid, action,
									  &prel_found);

	if ((action == HASH_FIND ||
		(action == HASH_ENTER && prel_found)) && PrelIsValid(prel))
	{
		FreeChildrenArray(prel);
		FreeRangesArray(prel);
		FreeIfNotNull(prel->expr_cstr);

		prel->valid = false; /* now cache entry is invalid */
	}
	/* Handle invalid PartRelationInfo */
	else if (prel)
	{
		prel->children = NULL;
		prel->ranges = NULL;

		prel->valid = false; /* now cache entry is invalid */
	}

	/* Set 'found' if necessary */
	if (found) *found = prel_found;

	elog(DEBUG2,
		 "Invalidating record for relation %u in pg_pathman's cache [%u]",
		 relid, MyProcPid);

	return prel;
}

/* Get PartRelationInfo from local cache. */
const PartRelationInfo *
get_pathman_relation_info(Oid relid)
{
	const PartRelationInfo *prel = pathman_cache_search_relid(partitioned_rels,
															  relid, HASH_FIND,
															  NULL);
	/* Refresh PartRelationInfo if needed */
	if (prel && !PrelIsValid(prel))
	{
		ItemPointerData		iptr;
		Datum				values[Natts_pathman_config];
		bool				isnull[Natts_pathman_config];

		/* Check that PATHMAN_CONFIG table contains this relation */
		if (pathman_config_contains_relation(relid, values, isnull, NULL, &iptr))
		{
			bool upd_expr = isnull[Anum_pathman_config_cooked_expr - 1];
			if (upd_expr)
				pathman_config_refresh_parsed_expression(relid, values, isnull, &iptr);

			/* Refresh partitioned table cache entry (might turn NULL) */
			prel = refresh_pathman_relation_info(relid, values, false);
		}

		/* Else clear remaining cache entry */
		else
		{
			remove_pathman_relation_info(relid);
			prel = NULL; /* don't forget to reset 'prel' */
		}
	}

	elog(DEBUG2,
		 "Fetching %s record for relation %u from pg_pathman's cache [%u]",
		 (prel ? "live" : "NULL"), relid, MyProcPid);

	/* Make sure that 'prel' is valid */
	Assert(!prel || PrelIsValid(prel));

	return prel;
}

/* Acquire lock on a table and try to get PartRelationInfo */
const PartRelationInfo *
get_pathman_relation_info_after_lock(Oid relid,
									 bool unlock_if_not_found,
									 LockAcquireResult *lock_result)
{
	const PartRelationInfo *prel;
	LockAcquireResult		acquire_result;

	/* Restrict concurrent partition creation (it's dangerous) */
	acquire_result = xact_lock_partitioned_rel(relid, false);

	/* Invalidate cache entry (see AcceptInvalidationMessages()) */
	invalidate_pathman_relation_info(relid, NULL);

	/* Set 'lock_result' if asked to */
	if (lock_result)
		*lock_result = acquire_result;

	prel = get_pathman_relation_info(relid);
	if (!prel && unlock_if_not_found)
		xact_unlock_partitioned_rel(relid);

	return prel;
}

/* Remove PartRelationInfo from local cache. */
void
remove_pathman_relation_info(Oid relid)
{
	bool found;

	/* Free resources */
	invalidate_pathman_relation_info(relid, &found);

	/* Now let's remove the entry completely */
	if (found)
		pathman_cache_search_relid(partitioned_rels, relid, HASH_REMOVE, NULL);

	elog(DEBUG2,
		 "Removing record for relation %u in pg_pathman's cache [%u]",
		 relid, MyProcPid);
}

/* Fill PartRelationInfo with partition-related info */
static void
fill_prel_with_partitions(PartRelationInfo *prel,
						  const Oid *partitions,
						  const uint32 parts_count)
{
	uint32			i;
	MemoryContext	cache_mcxt = PathmanRelationCacheContext,
					temp_mcxt,	/* reference temporary mcxt */
					old_mcxt;	/* reference current mcxt */

	AssertTemporaryContext();

	/* Allocate memory for 'prel->children' & 'prel->ranges' (if needed) */
	prel->children	= MemoryContextAllocZero(cache_mcxt, parts_count * sizeof(Oid));
	prel->ranges	= NULL;
	prel->has_null_partition = false;

	/* Set number of children */
	PrelChildrenCount(prel) = parts_count;

	/* Now we can use PrelRangePartitionsCount macros */
	if (prel->parttype == PT_RANGE)
		prel->ranges = MemoryContextAllocZero(cache_mcxt,
						PrelRangePartitionsCount(prel) * sizeof(RangeEntry));

	/* Create temporary memory context for loop */
	temp_mcxt = AllocSetContextCreate(CurrentMemoryContext,
									  CppAsString(fill_prel_with_partitions),
									  ALLOCSET_DEFAULT_SIZES);

	/* Initialize bounds of partitions */
	for (i = 0; i < PrelChildrenCount(prel); i++)
	{
		PartBoundInfo *bound_info;

		/* Clear all previous allocations */
		MemoryContextReset(temp_mcxt);

		/* Switch to the temporary memory context */
		old_mcxt = MemoryContextSwitchTo(temp_mcxt);
		{
			/* Fetch constraint's expression tree */
			bound_info = get_bounds_of_partition(partitions[i], prel);
		}
		MemoryContextSwitchTo(old_mcxt);

		/* Copy bounds from bound cache */
		switch (bound_info->parttype)
		{
			case PT_NULL:
				/* last item in children will contain NULL partition */
				prel->children[parts_count - 1] = bound_info->child_rel;
				prel->has_null_partition = true;
				break;
			case PT_HASH:
				Assert(bound_info->part_idx < PrelHashPartitionsCount(prel));
				prel->children[bound_info->part_idx] = bound_info->child_rel;
				break;

			case PT_RANGE:
				{
					if (i >= PrelRangePartitionsCount(prel))
						/* this shouldn't happen but we check anyway,
						 * also removes clang error */
						elog(ERROR, "range array overflow");

					/* Copy child's Oid */
					prel->ranges[i].child_oid = bound_info->child_rel;

					/* Copy all min & max Datums to the persistent mcxt */
					old_mcxt = MemoryContextSwitchTo(cache_mcxt);
					{
						prel->ranges[i].min = CopyBound(&bound_info->range_min,
														prel->ev_byval,
														prel->ev_len);

						prel->ranges[i].max = CopyBound(&bound_info->range_max,
														prel->ev_byval,
														prel->ev_len);
					}
					MemoryContextSwitchTo(old_mcxt);
				}
				break;

			default:
				{
					DisablePathman(); /* disable pg_pathman since config is broken */
					WrongPartType(prel->parttype);
				}
				break;
		}
	}

	/* Drop temporary memory context */
	MemoryContextDelete(temp_mcxt);

	/* Finalize 'prel' for a RANGE-partitioned table */
	if (prel->parttype == PT_RANGE)
	{
		cmp_func_info	cmp_info;

		/* Prepare function info */
		fmgr_info(prel->cmp_proc, &cmp_info.flinfo);
		cmp_info.collid = prel->ev_collid;

		/* Sort partitions by RangeEntry->min asc */
		qsort_arg((void *) prel->ranges, PrelRangePartitionsCount(prel),
				  sizeof(RangeEntry), cmp_range_entries,
				  (void *) &cmp_info);

		/* Initialize 'prel->children' array */
		for (i = 0; i < PrelRangePartitionsCount(prel); i++)
			prel->children[i] = prel->ranges[i].child_oid;
	}

#ifdef USE_ASSERT_CHECKING
	/* Check that each partition Oid has been assigned properly */
	if (prel->parttype == PT_HASH)
		for (i = 0; i < PrelChildrenCount(prel); i++)
		{
			if (!OidIsValid(prel->children[i]))
			{
				DisablePathman(); /* disable pg_pathman since config is broken */
				elog(ERROR, "pg_pathman's cache for relation \"%s\" "
							"has not been properly initialized",
					 get_rel_name_or_relid(PrelParentRelid(prel)));
			}
		}
#endif
}


/*
 * Partitioning expression routines.
 */

/* Wraps expression in SELECT query and returns parse tree */
Node *
parse_partitioning_expression(const Oid relid,
							  const char *exp_cstr,
							  char **query_string_out,	/* ret value #1 */
							  Node **parsetree_out)		/* ret value #2 */
{
	SelectStmt		   *select_stmt;
	List			   *parsetree_list;
	MemoryContext		old_mcxt;

	const char *sql = "SELECT (%s) FROM ONLY %s.%s";
	char	   *relname = get_rel_name(relid),
			   *nspname = get_namespace_name(get_rel_namespace(relid));
	char	   *query_string = psprintf(sql, exp_cstr,
										quote_identifier(nspname),
										quote_identifier(relname));

	old_mcxt = CurrentMemoryContext;

	PG_TRY();
	{
		parsetree_list = raw_parser(query_string);
	}
	PG_CATCH();
	{
		ErrorData  *error;

		/* Switch to the original context & copy edata */
		MemoryContextSwitchTo(old_mcxt);
		error = CopyErrorData();
		FlushErrorState();

		error->detail = error->message;
		error->message = "partitioning expression parse error";
		error->sqlerrcode = ERRCODE_INVALID_PARAMETER_VALUE;
		error->cursorpos = 0;
		error->internalpos = 0;

		ReThrowError(error);
	}
	PG_END_TRY();

	if (list_length(parsetree_list) != 1)
		elog(ERROR, "expression \"%s\" produced more than one query", exp_cstr);

	select_stmt = (SelectStmt *) linitial(parsetree_list);

	if (query_string_out)
		*query_string_out = query_string;

	if (parsetree_out)
		*parsetree_out = (Node *) select_stmt;

	return ((ResTarget *) linitial(select_stmt->targetList))->val;
}

/* Parse partitioning expression and return its type and nodeToString() as TEXT */
Datum
cook_partitioning_expression(const Oid relid,
							 const char *expr_cstr,
							 Oid *expr_type_out) /* ret value #1 */
{
	Node				   *parsetree;
	List				   *querytree_list;
	TargetEntry			   *target_entry;

	Query				   *expr_query;
	PlannedStmt			   *expr_plan;
	Node				   *expr;
	Datum					expr_datum;

	char				   *query_string,
						   *expr_serialized;

	MemoryContext			parse_mcxt,
							old_mcxt;

	AssertTemporaryContext();

	parse_mcxt = AllocSetContextCreate(CurrentMemoryContext,
									   "pathman parse context",
									   ALLOCSET_DEFAULT_SIZES);

	/* Keep raw expression */
	parse_partitioning_expression(relid, expr_cstr,
									&query_string, &parsetree);

	/* We don't need pathman activity initialization for this relation yet */
	pathman_hooks_enabled = false;

	/*
	 * We use separate memory context here, just to make sure we
	 * don't leave anything behind after analyze and planning.
	 * Parsed raw expression will stay in caller's context.
	 */
	old_mcxt = MemoryContextSwitchTo(parse_mcxt);

	PG_TRY();
	{
		/* This will fail with elog in case of wrong expression */
		querytree_list = pg_analyze_and_rewrite(parsetree, query_string, NULL, 0);
	}
	PG_CATCH();
	{
		ErrorData  *error;

		/* Switch to the original context & copy edata */
		MemoryContextSwitchTo(old_mcxt);
		error = CopyErrorData();
		FlushErrorState();

		error->detail = error->message;
		error->message = "partitioning expression analyze error";
		error->sqlerrcode = ERRCODE_INVALID_PARAMETER_VALUE;
		error->cursorpos = 0;
		error->internalpos = 0;

		/* Enable pathman hooks */
		pathman_hooks_enabled = true;
		ReThrowError(error);
	}
	PG_END_TRY();

	if (list_length(querytree_list) != 1)
		elog(ERROR, "partitioning expression produced more than 1 query");

	expr_query = (Query *) linitial(querytree_list);

	/* Plan this query. We reuse 'expr_node' here */
	expr_plan = pg_plan_query(expr_query, 0, NULL);

	target_entry = IsA(expr_plan->planTree, IndexOnlyScan) ?
					linitial(((IndexOnlyScan *) expr_plan->planTree)->indextlist) :
					linitial(expr_plan->planTree->targetlist);

	expr = eval_const_expressions(NULL, (Node *) target_entry->expr);
	if (contain_mutable_functions(expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("functions in partitioning expression must be marked IMMUTABLE")));

	Assert(expr);
	expr_serialized = nodeToString(expr);

	/* Switch to previous mcxt */
	MemoryContextSwitchTo(old_mcxt);

	/* Set 'expr_type_out' if needed */
	if (expr_type_out)
		*expr_type_out = exprType(expr);

	expr_datum = CStringGetTextDatum(expr_serialized);

	/* Free memory */
	MemoryContextDelete(parse_mcxt);

	/* Enable pathman hooks */
	pathman_hooks_enabled = true;

	return expr_datum;
}


/*
 * Functions for delayed invalidation.
 */

/* Add new delayed pathman shutdown job (DROP EXTENSION) */
void
delay_pathman_shutdown(void)
{
	delayed_shutdown = true;
}

/* Add new delayed invalidation job for a [ex-]parent relation */
void
delay_invalidation_parent_rel(Oid parent)
{
	list_add_unique(delayed_invalidation_parent_rels, parent);
}

/* Add new delayed invalidation job for a vague relation */
void
delay_invalidation_vague_rel(Oid vague_rel)
{
	list_add_unique(delayed_invalidation_vague_rels, vague_rel);
}

/* Finish all pending invalidation jobs if possible */
void
finish_delayed_invalidation(void)
{
	/* Exit early if there's nothing to do */
	if (delayed_invalidation_parent_rels == NIL &&
		delayed_invalidation_vague_rels == NIL &&
		delayed_shutdown == false)
	{
		return;
	}

	/* Check that current state is transactional */
	if (IsTransactionState())
	{
		ListCell   *lc;

		/* Handle the probable 'DROP EXTENSION' case */
		if (delayed_shutdown)
		{
			Oid		cur_pathman_config_relid;

			/* Unset 'shutdown' flag */
			delayed_shutdown = false;

			/* Get current PATHMAN_CONFIG relid */
			cur_pathman_config_relid = get_relname_relid(PATHMAN_CONFIG,
														 get_pathman_schema());

			/* Check that PATHMAN_CONFIG table has indeed been dropped */
			if (cur_pathman_config_relid == InvalidOid ||
				cur_pathman_config_relid != get_pathman_config_relid(true))
			{
				/* Ok, let's unload pg_pathman's config */
				unload_config();

				/* Disregard all remaining invalidation jobs */
				free_invalidation_list(delayed_invalidation_parent_rels);
				free_invalidation_list(delayed_invalidation_vague_rels);

				/* No need to continue, exit */
				return;
			}
		}

		/* Process relations that are (or were) definitely partitioned */
		foreach (lc, delayed_invalidation_parent_rels)
		{
			Oid		parent = lfirst_oid(lc);

			/* Skip if it's a TOAST table */
			if (IsToastNamespace(get_rel_namespace(parent)))
				continue;

			if (!pathman_config_contains_relation(parent, NULL, NULL, NULL, NULL))
				remove_pathman_relation_info(parent);
			else
				/* get_pathman_relation_info() will refresh this entry */
				invalidate_pathman_relation_info(parent, NULL);
		}

		/* Process all other vague cases */
		foreach (lc, delayed_invalidation_vague_rels)
		{
			Oid		vague_rel = lfirst_oid(lc);

			/* Skip if it's a TOAST table */
			if (IsToastNamespace(get_rel_namespace(vague_rel)))
				continue;

			/* It might be a partitioned table or a partition */
			if (!try_perform_parent_refresh(vague_rel))
			{
				PartParentSearch	search;
				Oid					parent;
				List			   *fresh_rels = delayed_invalidation_parent_rels;

				parent = get_parent_of_partition(vague_rel, &search);

				switch (search)
				{
					/* It's still parent */
					case PPS_ENTRY_PART_PARENT:
						{
							/* Skip if we've already refreshed this parent */
							if (!list_member_oid(fresh_rels, parent))
								try_perform_parent_refresh(parent);
						}
						break;

					/* It *might have been* parent before (not in PATHMAN_CONFIG) */
					case PPS_ENTRY_PARENT:
						{
							/* Skip if we've already refreshed this parent */
							if (!list_member_oid(fresh_rels, parent))
								try_perform_parent_refresh(parent);
						}
						break;

					/* How come we still don't know?? */
					case PPS_NOT_SURE:
						elog(ERROR, "Unknown table status, this should never happen");
						break;

					default:
						break;
				}
			}
		}

		free_invalidation_list(delayed_invalidation_parent_rels);
		free_invalidation_list(delayed_invalidation_vague_rels);
	}
}


/*
 * cache\forget\get PartParentInfo functions.
 */

/* Create "partition+parent" pair in local cache */
void
cache_parent_of_partition(Oid partition, Oid parent)
{
	bool			found;
	PartParentInfo *ppar;

	ppar = pathman_cache_search_relid(parent_cache,
									  partition,
									  HASH_ENTER,
									  &found);
	elog(DEBUG2,
		 found ?
			 "Refreshing record for child %u in pg_pathman's cache [%u]" :
			 "Creating new record for child %u in pg_pathman's cache [%u]",
		 partition, MyProcPid);

	ppar->child_rel = partition;
	ppar->parent_rel = parent;
}

/* Remove "partition+parent" pair from cache & return parent's Oid */
Oid
forget_parent_of_partition(Oid partition, PartParentSearch *status)
{
	return get_parent_of_partition_internal(partition, status, HASH_REMOVE);
}

/* Return partition parent's Oid */
Oid
get_parent_of_partition(Oid partition, PartParentSearch *status)
{
	return get_parent_of_partition_internal(partition, status, HASH_FIND);
}

/*
 * Get [and remove] "partition+parent" pair from cache,
 * also check syscache if 'status' is provided.
 *
 * "status == NULL" implies that we don't care about
 * neither syscache nor PATHMAN_CONFIG table contents.
 */
static Oid
get_parent_of_partition_internal(Oid partition,
								 PartParentSearch *status,
								 HASHACTION action)
{
	const char	   *action_str; /* "Fetching"\"Resetting" */
	Oid				parent;
	PartParentInfo *ppar = pathman_cache_search_relid(parent_cache,
													  partition,
													  HASH_FIND,
													  NULL);
	/* Set 'action_str' */
	switch (action)
	{
		case HASH_REMOVE:
			action_str = "Resetting";
			break;

		case HASH_FIND:
			action_str = "Fetching";
			break;

		default:
			elog(ERROR, "Unexpected HTAB action %u", action);
	}

	elog(DEBUG2,
		 "%s %s record for child %u from pg_pathman's cache [%u]",
		 action_str, (ppar ? "live" : "NULL"), partition, MyProcPid);

	if (ppar)
	{
		if (status) *status = PPS_ENTRY_PART_PARENT;
		parent = ppar->parent_rel;

		/* Remove entry if necessary */
		if (action == HASH_REMOVE)
			pathman_cache_search_relid(parent_cache, partition,
									   HASH_REMOVE, NULL);
	}
	/* Try fetching parent from syscache if 'status' is provided */
	else if (status)
		parent = try_syscache_parent_search(partition, status);
	else
		parent = InvalidOid; /* we don't have to set status */

	return parent;
}

/* Try to find parent of a partition using syscache & PATHMAN_CONFIG */
static Oid
try_syscache_parent_search(Oid partition, PartParentSearch *status)
{
	if (!IsTransactionState())
	{
		/* We could not perform search */
		if (status) *status = PPS_NOT_SURE;

		return InvalidOid;
	}
	else
	{
		Relation		relation;
		ScanKeyData		key[1];
		SysScanDesc		scan;
		HeapTuple		inheritsTuple;
		Oid				parent = InvalidOid;

		/* At first we assume parent does not exist (not a partition) */
		if (status) *status = PPS_ENTRY_NOT_FOUND;

		relation = heap_open(InheritsRelationId, AccessShareLock);

		ScanKeyInit(&key[0],
					Anum_pg_inherits_inhrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(partition));

		scan = systable_beginscan(relation, InheritsRelidSeqnoIndexId,
								  true, NULL, 1, key);

		while ((inheritsTuple = systable_getnext(scan)) != NULL)
		{
			parent = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhparent;

			/*
			 * NB: don't forget that 'inh' flag does not immediately
			 * mean that this is a pg_pathman's partition. It might
			 * be just a casual inheriting table.
			 */
			if (status) *status = PPS_ENTRY_PARENT;

			/* Check that PATHMAN_CONFIG contains this table */
			if (pathman_config_contains_relation(parent, NULL, NULL, NULL, NULL))
			{
				/* We've found the entry, update status */
				if (status) *status = PPS_ENTRY_PART_PARENT;
			}

			break; /* there should be no more rows */
		}

		systable_endscan(scan);
		heap_close(relation, AccessShareLock);

		return parent;
	}
}

/*
 * Try to refresh cache entry for relation 'parent'.
 *
 * Return true on success.
 */
static bool
try_perform_parent_refresh(Oid parent)
{
	ItemPointerData		iptr;
	Datum				values[Natts_pathman_config];
	bool				isnull[Natts_pathman_config];

	if (pathman_config_contains_relation(parent, values, isnull, NULL, &iptr))
	{
		bool should_update_expr = isnull[Anum_pathman_config_cooked_expr - 1];

		if (should_update_expr)
			pathman_config_refresh_parsed_expression(parent, values, isnull, &iptr);

		/* If anything went wrong, return false (actually, it might emit ERROR) */
		refresh_pathman_relation_info(parent,
									  values,
									  true); /* allow lazy */
	}
	/* Not a partitioned relation */
	else return false;

	return true;
}


/*
 * forget\get constraint functions.
 */

/* Remove partition's constraint from cache */
void
forget_bounds_of_partition(Oid partition)
{
	PartBoundInfo *pbin;

	/* Should we search in bounds cache? */
	pbin = pg_pathman_enable_bounds_cache ?
				pathman_cache_search_relid(bound_cache,
										   partition,
										   HASH_FIND,
										   NULL) :
				NULL; /* don't even bother */

	/* Free this entry */
	if (pbin)
	{
		/* Call pfree() if it's RANGE bounds */
		if (pbin->parttype == PT_RANGE)
		{
			FreeBound(&pbin->range_min, pbin->byval);
			FreeBound(&pbin->range_max, pbin->byval);
		}

		/* Finally remove this entry from cache */
		pathman_cache_search_relid(bound_cache,
								   partition,
								   HASH_REMOVE,
								   NULL);
	}
}

/* Return partition's constraint as expression tree */
static PartBoundInfo *
get_bounds_of_partition(Oid partition, const PartRelationInfo *prel)
{
	PartBoundInfo *pbin;

	/*
	 * We might end up building the constraint
	 * tree that we wouldn't want to keep.
	 */
	AssertTemporaryContext();

	/* Should we search in bounds cache? */
	pbin = pg_pathman_enable_bounds_cache ?
				pathman_cache_search_relid(bound_cache,
										   partition,
										   HASH_FIND,
										   NULL) :
				NULL; /* don't even bother */

	/* Build new entry */
	if (pbin == NULL)
	{
		PartBoundInfo	pbin_local;
		Expr		   *con_expr;

		/* Initialize other fields */
		pbin_local.child_rel = partition;
		pbin_local.byval = prel->ev_byval;

		/* Try to build constraint's expression tree (may emit ERROR) */
		con_expr = get_partition_constraint_expr(partition);

		/* Grab bounds/hash and fill in 'pbin_local' (may emit ERROR) */
		fill_pbin_with_bounds(&pbin_local, prel, con_expr);

		/* We strive to delay the creation of cache's entry */
		pbin = pg_pathman_enable_bounds_cache ?
					pathman_cache_search_relid(bound_cache,
											   partition,
											   HASH_ENTER,
											   NULL) :
					palloc(sizeof(PartBoundInfo));

		/* Copy data from 'pbin_local' */
		memcpy(pbin, &pbin_local, sizeof(PartBoundInfo));
	}

	return pbin;
}

/*
 * Get constraint expression tree of a partition.
 *
 * build_check_constraint_name_internal() is used to build conname.
 */
static Expr *
get_partition_constraint_expr(Oid partition)
{
	Oid			conid;			/* constraint Oid */
	char	   *conname;		/* constraint name */
	HeapTuple	con_tuple;
	Datum		conbin_datum;
	bool		conbin_isnull;
	Expr	   *expr;			/* expression tree for constraint */

	conname = build_check_constraint_name_relid_internal(partition);
	conid = get_relation_constraint_oid(partition, conname, true);

	if (!OidIsValid(conid))
	{
		DisablePathman(); /* disable pg_pathman since config is broken */
		ereport(ERROR,
				(errmsg("constraint \"%s\" of partition \"%s\" does not exist",
						conname, get_rel_name_or_relid(partition)),
				 errhint(INIT_ERROR_HINT)));
	}

	con_tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conid));
	conbin_datum = SysCacheGetAttr(CONSTROID, con_tuple,
								   Anum_pg_constraint_conbin,
								   &conbin_isnull);
	if (conbin_isnull)
	{
		DisablePathman(); /* disable pg_pathman since config is broken */
		ereport(WARNING,
				(errmsg("constraint \"%s\" of partition \"%s\" has NULL conbin",
						conname, get_rel_name_or_relid(partition)),
				 errhint(INIT_ERROR_HINT)));
		pfree(conname);

		return NULL; /* could not parse */
	}
	pfree(conname);

	/* Finally we get a constraint expression tree */
	expr = (Expr *) stringToNode(TextDatumGetCString(conbin_datum));

	/* Don't foreget to release syscache tuple */
	ReleaseSysCache(con_tuple);

	return expr;
}

/* Fill PartBoundInfo with bounds/hash */
static void
fill_pbin_with_bounds(PartBoundInfo *pbin,
					  const PartRelationInfo *prel,
					  const Expr *constraint_expr)
{
	AssertTemporaryContext();

	/* Copy partitioning type to 'pbin' */
	if (IsA(constraint_expr, NullTest))
	{
		pbin->parttype = PT_NULL;

		/* we're done here */
		return;
	}

	pbin->parttype = prel->parttype;

	/* Perform a partitioning_type-dependent task */
	switch (prel->parttype)
	{
		case PT_HASH:
			{
				if (!validate_hash_constraint(constraint_expr,
											  prel, &pbin->part_idx))
				{
					DisablePathman(); /* disable pg_pathman since config is broken */
					ereport(ERROR,
							(errmsg("wrong constraint format for HASH partition \"%s\"",
									get_rel_name_or_relid(pbin->child_rel)),
							 errhint(INIT_ERROR_HINT)));
				}
			}
			break;

		case PT_RANGE:
			{
				Datum	lower, upper;
				bool	lower_null, upper_null;

				if (validate_range_constraint(constraint_expr,
											  prel, &lower, &upper,
											  &lower_null, &upper_null))
				{
					MemoryContext old_mcxt;

					/* Switch to the persistent memory context */
					old_mcxt = MemoryContextSwitchTo(PathmanBoundCacheContext);

					pbin->range_min = lower_null ?
											MakeBoundInf(MINUS_INFINITY) :
											MakeBound(datumCopy(lower,
																prel->ev_byval,
																prel->ev_len));

					pbin->range_max = upper_null ?
											MakeBoundInf(PLUS_INFINITY) :
											MakeBound(datumCopy(upper,
																prel->ev_byval,
																prel->ev_len));

					/* Switch back */
					MemoryContextSwitchTo(old_mcxt);
				}
				else
				{
					DisablePathman(); /* disable pg_pathman since config is broken */
					ereport(ERROR,
							(errmsg("wrong constraint format for RANGE partition \"%s\"",
									get_rel_name_or_relid(pbin->child_rel)),
							 errhint(INIT_ERROR_HINT)));
				}
			}
			break;

		default:
			{
				DisablePathman(); /* disable pg_pathman since config is broken */
				WrongPartType(prel->parttype);
			}
			break;
	}
}

/* qsort comparison function for RangeEntries */
static int
cmp_range_entries(const void *p1, const void *p2, void *arg)
{
	const RangeEntry   *v1 = (const RangeEntry *) p1;
	const RangeEntry   *v2 = (const RangeEntry *) p2;
	cmp_func_info	   *info = (cmp_func_info *) arg;

	return cmp_bounds(&info->flinfo, info->collid, &v1->min, &v2->min);
}


/*
 * Common PartRelationInfo checks. Emit ERROR if anything is wrong.
 */
void
shout_if_prel_is_invalid(const Oid parent_oid,
						 const PartRelationInfo *prel,
						 const PartType expected_part_type)
{
	if (!prel)
		elog(ERROR, "relation \"%s\" has no partitions",
			 get_rel_name_or_relid(parent_oid));

	if (!PrelIsValid(prel))
		elog(ERROR, "pg_pathman's cache contains invalid entry "
					"for relation \"%s\" [%u]",
			 get_rel_name_or_relid(parent_oid),
			 MyProcPid);

	/* Check partitioning type unless it's "ANY" */
	if (expected_part_type != PT_ANY &&
		expected_part_type != prel->parttype)
	{
		char *expected_str;

		switch (expected_part_type)
		{
			case PT_HASH:
				expected_str = "HASH";
				break;

			case PT_RANGE:
				expected_str = "RANGE";
				break;

			default:
				WrongPartType(expected_part_type);
				expected_str = NULL; /* keep compiler happy */
		}

		elog(ERROR, "relation \"%s\" is not partitioned by %s",
			 get_rel_name_or_relid(parent_oid),
			 expected_str);
	}
}
