/*-------------------------------------------------------------------------
 *
 * partition_creation.c
 *		Various functions for partition creation.
 *
 * Copyright (c) 2016, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "init.h"
#include "partition_creation.h"
#include "partition_filter.h"
#include "pathman.h"
#include "pathman_workers.h"
#include "xact_handling.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "commands/event_trigger.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_utilcmd.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Oid spawn_partitions_val(Oid parent_relid,
								const Bound *range_bound_min,
								const Bound *range_bound_max,
								Oid range_bound_type,
								Datum interval_binary,
								Oid interval_type,
								Datum value,
								Oid value_type,
								Oid collid);

static void create_single_partition_common(Oid parent_relid,
										   Oid partition_relid,
										   Constraint *check_constraint,
										   init_callback_params *callback_params,
										   List *trigger_columns);

static Oid create_single_partition_internal(Oid parent_relid,
											RangeVar *partition_rv,
											char *tablespace);

static char *choose_range_partition_name(Oid parent_relid, Oid parent_nsp);
static char *choose_hash_partition_name(Oid parent_relid, uint32 part_idx);

static ObjectAddress create_table_using_stmt(CreateStmt *create_stmt,
											 Oid relowner);

static void copy_foreign_keys(Oid parent_relid, Oid partition_oid);
static void postprocess_child_table_and_atts(Oid parent_relid, Oid partition_relid);

static Oid text_to_regprocedure(text *proname_args);

static Constraint *make_constraint_common(char *name, Node *raw_expr);
static Value make_string_value_struct(char *str);
static Value make_int_value_struct(int int_val);

static Node *build_partitioning_expression(Oid parent_relid,
										   Oid *expr_type,
										   List **columns);

/*
 * ---------------------------------------
 *  Public interface (partition creation)
 * ---------------------------------------
 */

/* Create one RANGE partition [start_value, end_value) */
Oid
create_single_range_partition_internal(Oid parent_relid,
									   const Bound *start_value,
									   const Bound *end_value,
									   Oid value_type,
									   RangeVar *partition_rv,
									   char *tablespace)
{
	Oid						partition_relid;
	Constraint			   *check_constr;
	init_callback_params	callback_params;
	List				   *trigger_columns = NIL;
	Node				   *expr;

	/* Generate a name if asked to */
	if (!partition_rv)
	{
		Oid		parent_nsp = get_rel_namespace(parent_relid);
		char   *parent_nsp_name = get_namespace_name(parent_nsp);
		char   *partition_name;

		partition_name = choose_range_partition_name(parent_relid, parent_nsp);

		partition_rv = makeRangeVar(parent_nsp_name, partition_name, -1);
	}

	/* Check pathman config anld fill variables */
	expr = build_partitioning_expression(parent_relid, NULL, &trigger_columns);

	/* Create a partition & get 'partitioning expression' */
	partition_relid = create_single_partition_internal(parent_relid,
													   partition_rv,
													   tablespace);

	/* Build check constraint for RANGE partition */
	check_constr = build_range_check_constraint(partition_relid,
												expr,
												start_value,
												end_value,
												value_type);

	/* Cook args for init_callback */
	MakeInitCallbackRangeParams(&callback_params,
								DEFAULT_INIT_CALLBACK,
								parent_relid, partition_relid,
								*start_value, *end_value, value_type);

	/* Add constraint & execute init_callback */
	create_single_partition_common(parent_relid,
								   partition_relid,
								   check_constr,
								   &callback_params,
								   trigger_columns);

	/* Return the Oid */
	return partition_relid;
}

/* Create one HASH partition */
Oid
create_single_hash_partition_internal(Oid parent_relid,
									  uint32 part_idx,
									  uint32 part_count,
									  RangeVar *partition_rv,
									  char *tablespace)
{
	Oid						partition_relid,
							expr_type;
	Constraint			   *check_constr;
	init_callback_params	callback_params;
	List				   *trigger_columns = NIL;
	Node				   *expr;

	/* Generate a name if asked to */
	if (!partition_rv)
	{
		Oid		parent_nsp = get_rel_namespace(parent_relid);
		char   *parent_nsp_name = get_namespace_name(parent_nsp);
		char   *partition_name;

		partition_name = choose_hash_partition_name(parent_relid, part_idx);

		partition_rv = makeRangeVar(parent_nsp_name, partition_name, -1);
	}

	/* Create a partition & get 'partitionining expression' */
	partition_relid = create_single_partition_internal(parent_relid,
													   partition_rv,
													   tablespace);

	/* check pathman config and fill variables */
	expr = build_partitioning_expression(parent_relid, &expr_type, &trigger_columns);

	/* Build check constraint for HASH partition */
	check_constr = build_hash_check_constraint(partition_relid,
											   expr,
											   part_idx,
											   part_count,
											   expr_type);

	/* Cook args for init_callback */
	MakeInitCallbackHashParams(&callback_params,
							   DEFAULT_INIT_CALLBACK,
							   parent_relid, partition_relid);

	/* Add constraint & execute init_callback */
	create_single_partition_common(parent_relid,
								   partition_relid,
								   check_constr,
								   &callback_params,
								   trigger_columns);

	/* Return the Oid */
	return partition_relid;
}

/* Add constraint & execute init_callback */
void
create_single_partition_common(Oid parent_relid,
							   Oid partition_relid,
							   Constraint *check_constraint,
							   init_callback_params *callback_params,
							   List *trigger_columns)
{
	Relation	child_relation;

	/* Open the relation and add new check constraint & fkeys */
	child_relation = heap_open(partition_relid, AccessExclusiveLock);
	AddRelationNewConstraints(child_relation, NIL,
							  list_make1(check_constraint),
							  false, true, true);
	heap_close(child_relation, NoLock);

	/* Make constraint visible */
	CommandCounterIncrement();

	/* Create trigger if needed */
	if (has_update_trigger_internal(parent_relid))
	{
		const char *trigger_name;

		trigger_name = build_update_trigger_name_internal(parent_relid);
		create_single_update_trigger_internal(partition_relid,
											  trigger_name,
											  trigger_columns);
	}

	/* Make trigger visible */
	CommandCounterIncrement();

	/* Finally invoke 'init_callback' */
	invoke_part_callback(callback_params);

	/* Make possible changes visible */
	CommandCounterIncrement();
}

/*
 * Create RANGE partitions (if needed) using either BGW or current backend.
 *
 * Returns Oid of the partition to store 'value'.
 */
Oid
create_partitions_for_value(Oid relid, Datum value, Oid value_type)
{
	TransactionId	rel_xmin;
	Oid				last_partition = InvalidOid;

	/* Check that table is partitioned and fetch xmin */
	if (pathman_config_contains_relation(relid, NULL, NULL, &rel_xmin, NULL))
	{
		/* Take default values */
		bool	spawn_using_bgw	= DEFAULT_SPAWN_USING_BGW,
				enable_auto		= DEFAULT_AUTO;

		/* Values to be extracted from PATHMAN_CONFIG_PARAMS */
		Datum	values[Natts_pathman_config_params];
		bool	isnull[Natts_pathman_config_params];

		/* Try fetching options from PATHMAN_CONFIG_PARAMS */
		if (read_pathman_params(relid, values, isnull))
		{
			enable_auto = values[Anum_pathman_config_params_auto - 1];
			spawn_using_bgw = values[Anum_pathman_config_params_spawn_using_bgw - 1];
		}

		/* Emit ERROR if automatic partition creation is disabled */
		if (!enable_auto || !IsAutoPartitionEnabled())
			elog(ERROR, ERR_PART_ATTR_NO_PART, datum_to_cstring(value, value_type));

		/*
		 * If table has been partitioned in some previous xact AND
		 * we don't hold any conflicting locks, run BGWorker.
		 */
		if (spawn_using_bgw &&
			xact_object_is_visible(rel_xmin) &&
			!xact_bgw_conflicting_lock_exists(relid))
		{
			elog(DEBUG2, "create_partitions(): chose BGWorker [%u]", MyProcPid);
			last_partition = create_partitions_for_value_bg_worker(relid,
																   value,
																   value_type);
		}
		/* Else it'd be better for the current backend to create partitions */
		else
		{
			elog(DEBUG2, "create_partitions(): chose backend [%u]", MyProcPid);
			last_partition = create_partitions_for_value_internal(relid,
																  value,
																  value_type,
																  false); /* backend */
		}
	}
	else
		elog(ERROR, "table \"%s\" is not partitioned",
			 get_rel_name_or_relid(relid));

	/* Check that 'last_partition' is valid */
	if (last_partition == InvalidOid)
		elog(ERROR, "could not create new partitions for relation \"%s\"",
			 get_rel_name_or_relid(relid));

	return last_partition;
}


/*
 * --------------------
 *  Partition creation
 * --------------------
 */

/*
 * Create partitions (if needed) and return Oid of the partition to store 'value'.
 *
 * NB: This function should not be called directly,
 * use create_partitions_for_value() instead.
 */
Oid
create_partitions_for_value_internal(Oid relid, Datum value, Oid value_type,
									 bool is_background_worker)
{
	MemoryContext	old_mcxt = CurrentMemoryContext;
	Oid				partid = InvalidOid; /* last created partition (or InvalidOid) */

	PG_TRY();
	{
		const PartRelationInfo *prel;
		LockAcquireResult		lock_result; /* could we lock the parent? */
		Datum					values[Natts_pathman_config];
		bool					isnull[Natts_pathman_config];

		/* Get both PartRelationInfo & PATHMAN_CONFIG contents for this relation */
		if (pathman_config_contains_relation(relid, values, isnull, NULL, NULL))
		{
			Oid			base_bound_type;	/* base type of prel->atttype */
			Oid			base_value_type;	/* base type of value_type */

			/* Fetch PartRelationInfo by 'relid' */
			prel = get_pathman_relation_info_after_lock(relid, true, &lock_result);
			shout_if_prel_is_invalid(relid, prel, PT_RANGE);

			/* Fetch base types of prel->atttype & value_type */
			base_bound_type = getBaseType(prel->ev_type);
			base_value_type = getBaseType(value_type);

			/* Search for a suitable partition if we didn't hold it */
			Assert(lock_result != LOCKACQUIRE_NOT_AVAIL);
			if (lock_result == LOCKACQUIRE_OK)
			{
				Oid	   *parts;
				int		nparts;

				/* Search for matching partitions */
				parts = find_partitions_for_value(value, value_type, prel, &nparts);

				/* Shout if there's more than one */
				if (nparts > 1)
					elog(ERROR, ERR_PART_ATTR_MULTIPLE);

				/* It seems that we got a partition! */
				else if (nparts == 1)
				{
					/* Unlock the parent (we're not going to spawn) */
					xact_unlock_partitioned_rel(relid);

					/* Simply return the suitable partition */
					partid = parts[0];
				}

				/* Don't forget to free */
				pfree(parts);
			}

			/* Else spawn a new one (we hold a lock on the parent) */
			if (partid == InvalidOid)
			{
				RangeEntry *ranges = PrelGetRangesArray(prel);
				Bound		bound_min,			/* absolute MIN */
							bound_max;			/* absolute MAX */

				Oid			interval_type = InvalidOid;
				Datum		interval_binary, /* assigned 'width' of one partition */
							interval_text;

				/* Copy datums in order to protect them from cache invalidation */
				bound_min = CopyBound(&ranges[0].min,
									  prel->ev_byval,
									  prel->ev_len);

				bound_max = CopyBound(&ranges[PrelLastChild(prel)].max,
									  prel->ev_byval,
									  prel->ev_len);

				/* Check if interval is set */
				if (isnull[Anum_pathman_config_range_interval - 1])
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot spawn new partition for key '%s'",
									datum_to_cstring(value, value_type)),
							 errdetail("default range interval is NULL")));
				}

				/* Retrieve interval as TEXT from tuple */
				interval_text = values[Anum_pathman_config_range_interval - 1];

				/* Convert interval to binary representation */
				interval_binary = extract_binary_interval_from_text(interval_text,
																	base_bound_type,
																	&interval_type);

				/* At last, spawn partitions to store the value */
				partid = spawn_partitions_val(PrelParentRelid(prel),
											  &bound_min, &bound_max, base_bound_type,
											  interval_binary, interval_type,
											  value, base_value_type,
											  prel->ev_collid);
			}
		}
		else
			elog(ERROR, "pg_pathman's config does not contain relation \"%s\"",
				 get_rel_name_or_relid(relid));
	}
	PG_CATCH();
	{
		ErrorData *edata;

		/* Simply rethrow ERROR if we're in backend */
		if (!is_background_worker)
			PG_RE_THROW();

		/* Switch to the original context & copy edata */
		MemoryContextSwitchTo(old_mcxt);
		edata = CopyErrorData();
		FlushErrorState();

		/* Produce log message if we're in BGW */
		ereport(LOG,
				(errmsg(CppAsString(create_partitions_for_value_internal) ": %s [%u]",
						edata->message, MyProcPid),
				(edata->detail) ? errdetail("%s", edata->detail) : 0));

		FreeErrorData(edata);

		/* Reset 'partid' in case of error */
		partid = InvalidOid;
	}
	PG_END_TRY();

	return partid;
}

/*
 * Append\prepend partitions if there's no partition to store 'value'.
 *
 * Used by create_partitions_for_value_internal().
 *
 * NB: 'value' type is not needed since we've already taken
 * it into account while searching for the 'cmp_proc'.
 */
static Oid
spawn_partitions_val(Oid parent_relid,				/* parent's Oid */
					 const Bound *range_bound_min,	/* parent's MIN boundary */
					 const Bound *range_bound_max,	/* parent's MAX boundary */
					 Oid range_bound_type,			/* type of boundary's value */
					 Datum interval_binary,			/* interval in binary form */
					 Oid interval_type,				/* INTERVALOID or prel->atttype */
					 Datum value,					/* value to be INSERTed */
					 Oid value_type,				/* type of value */
					 Oid collid)					/* collation id */
{
	bool		should_append;				/* append or prepend? */

	Oid			move_bound_op_func,			/* operator's function */
				move_bound_op_ret_type;		/* operator's ret type */

	FmgrInfo	cmp_value_bound_finfo,		/* exec 'value (>=|<) bound' */
				move_bound_finfo;			/* exec 'bound + interval' */

	Datum		cur_leading_bound,			/* boundaries of a new partition */
				cur_following_bound;

	Bound		value_bound = MakeBound(value);

	Oid			last_partition = InvalidOid;


	fill_type_cmp_fmgr_info(&cmp_value_bound_finfo, value_type, range_bound_type);

	/* Is it possible to append\prepend a partition? */
	if (IsInfinite(range_bound_min) && IsInfinite(range_bound_max))
		ereport(ERROR, (errmsg("cannot spawn a partition"),
						errdetail("both bounds are infinite")));

	/* value >= MAX_BOUNDARY */
	else if (cmp_bounds(&cmp_value_bound_finfo, collid,
						&value_bound, range_bound_max) >= 0)
	{
		should_append = true;
		cur_leading_bound = BoundGetValue(range_bound_max);
	}

	/* value < MIN_BOUNDARY */
	else if (cmp_bounds(&cmp_value_bound_finfo, collid,
						&value_bound, range_bound_min) < 0)
	{
		should_append = false;
		cur_leading_bound = BoundGetValue(range_bound_min);
	}

	/* There's a gap, halt and emit ERROR */
	else ereport(ERROR, (errmsg("cannot spawn a partition"),
						 errdetail("there is a gap")));

	/* Fetch operator's underlying function and ret type */
	extract_op_func_and_ret_type(should_append ? "+" : "-",
								 range_bound_type,
								 interval_type,
								 &move_bound_op_func,
								 &move_bound_op_ret_type);

	/* Perform casts if types don't match (e.g. date + interval = timestamp) */
	if (move_bound_op_ret_type != range_bound_type)
	{
		/* Cast 'cur_leading_bound' to 'move_bound_op_ret_type' */
		cur_leading_bound = perform_type_cast(cur_leading_bound,
											  range_bound_type,
											  move_bound_op_ret_type,
											  NULL); /* might emit ERROR */

		/* Update 'range_bound_type' */
		range_bound_type = move_bound_op_ret_type;

		/* Fetch new comparison function */
		fill_type_cmp_fmgr_info(&cmp_value_bound_finfo,
								value_type,
								range_bound_type);

		/* Since type has changed, fetch another operator */
		extract_op_func_and_ret_type(should_append ? "+" : "-",
									 range_bound_type,
									 interval_type,
									 &move_bound_op_func,
									 &move_bound_op_ret_type);

		/* What, again? Don't want to deal with this nightmare */
		if (move_bound_op_ret_type != range_bound_type)
			elog(ERROR, "error in function " CppAsString(spawn_partitions_val));
	}

	/* Get operator's underlying function */
	fmgr_info(move_bound_op_func, &move_bound_finfo);

	/* Execute comparison function cmp(value, cur_leading_bound) */
	while (should_append ?
				check_ge(&cmp_value_bound_finfo, value, cur_leading_bound) :
				check_lt(&cmp_value_bound_finfo, value, cur_leading_bound))
	{
		Bound bounds[2];

		/* Assign the 'following' boundary to current 'leading' value */
		cur_following_bound = cur_leading_bound;

		/* Move leading bound by interval (exec 'leading (+|-) INTERVAL') */
		cur_leading_bound = FunctionCall2(&move_bound_finfo,
										  cur_leading_bound,
										  interval_binary);

		bounds[0] = MakeBound(should_append ? cur_following_bound : cur_leading_bound);
		bounds[1] = MakeBound(should_append ? cur_leading_bound : cur_following_bound);

		last_partition = create_single_range_partition_internal(parent_relid,
																&bounds[0], &bounds[1],
																range_bound_type,
																NULL, NULL);

#ifdef USE_ASSERT_CHECKING
		elog(DEBUG2, "%s partition with following='%s' & leading='%s' [%u]",
			 (should_append ? "Appending" : "Prepending"),
			 DebugPrintDatum(cur_following_bound, range_bound_type),
			 DebugPrintDatum(cur_leading_bound, range_bound_type),
			 MyProcPid);
#endif
	}

	return last_partition;
}

/* Choose a good name for a RANGE partition */
static char *
choose_range_partition_name(Oid parent_relid, Oid parent_nsp)
{
	Datum	part_num;
	Oid		part_seq_relid;
	char   *part_seq_relname;
	Oid		save_userid;
	int		save_sec_context;
	bool	need_priv_escalation = !superuser(); /* we might be a SU */
	char   *relname;
	int		attempts_cnt = 1000;

	part_seq_relname = build_sequence_name_internal(parent_relid);
	part_seq_relid = get_relname_relid(part_seq_relname, parent_nsp);

	/* Could not find part number generating sequence */
	if (!OidIsValid(part_seq_relid))
		elog(ERROR, "auto naming sequence \"%s\" does not exist", part_seq_relname);

	/* Do we have to escalate privileges? */
	if (need_priv_escalation)
	{
		/* Get current user's Oid and security context */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);

		/* Become superuser in order to bypass sequence ACL checks */
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	}

	/* Generate unique name */
	while (true)
	{
		/* Get next integer for partition name */
		part_num = DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(part_seq_relid));

		relname = psprintf("%s_" UINT64_FORMAT,
						   get_rel_name(parent_relid),
						   (uint64) DatumGetInt64(part_num)); /* can't use UInt64 on 9.5 */

		/*
		 * If we found a unique name or attemps number exceeds some reasonable
		 * value then we quit
		 *
		 * XXX Should we throw an exception if max attempts number is reached?
		 */
		if (get_relname_relid(relname, parent_nsp) == InvalidOid || attempts_cnt < 0)
			break;

		pfree(relname);
		attempts_cnt--;
	}

	/* Restore user's privileges */
	if (need_priv_escalation)
		SetUserIdAndSecContext(save_userid, save_sec_context);

	return relname;
}

/* Choose a good name for a HASH partition */
static char *
choose_hash_partition_name(Oid parent_relid, uint32 part_idx)
{
	return psprintf("%s_%u", get_rel_name(parent_relid), part_idx);
}

/* Create a partition-like table (no constraints yet) */
static Oid
create_single_partition_internal(Oid parent_relid,
								 RangeVar *partition_rv,
								 char *tablespace)
{
	/* Value to be returned */
	Oid					partition_relid = InvalidOid; /* safety */

	/* Parent's namespace and name */
	Oid					parent_nsp;
	char			   *parent_name,
					   *parent_nsp_name;

	/* Elements of the "CREATE TABLE" query tree */
	RangeVar		   *parent_rv;
	TableLikeClause		like_clause;
	CreateStmt			create_stmt;
	List			   *create_stmts;
	ListCell		   *lc;

	/* Current user and security context */
	Oid					save_userid;
	int					save_sec_context;
	bool				need_priv_escalation = !superuser(); /* we might be a SU */

	/* Lock parent and check if it exists */
	LockRelationOid(parent_relid, ShareUpdateExclusiveLock);
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(parent_relid)))
		elog(ERROR, "relation %u does not exist", parent_relid);

	/* Check that table is registered in PATHMAN_CONFIG */
	if (!pathman_config_contains_relation(parent_relid, NULL, NULL, NULL, NULL))
		elog(ERROR, "table \"%s\" is not partitioned",
			 get_rel_name_or_relid(parent_relid));

	/* Cache parent's namespace and name */
	parent_name = get_rel_name(parent_relid);
	parent_nsp = get_rel_namespace(parent_relid);
	parent_nsp_name = get_namespace_name(parent_nsp);

	/* Make up parent's RangeVar */
	parent_rv = makeRangeVar(parent_nsp_name, parent_name, -1);

	Assert(partition_rv);

	/* If no 'tablespace' is provided, get parent's tablespace */
	if (!tablespace)
		tablespace = get_tablespace_name(get_rel_tablespace(parent_relid));

	/* Initialize TableLikeClause structure */
	NodeSetTag(&like_clause, T_TableLikeClause);
	like_clause.relation		= copyObject(parent_rv);
	like_clause.options			= CREATE_TABLE_LIKE_DEFAULTS |
								  CREATE_TABLE_LIKE_INDEXES |
								  CREATE_TABLE_LIKE_STORAGE;

	/* Initialize CreateStmt structure */
	NodeSetTag(&create_stmt, T_CreateStmt);
	create_stmt.relation		= copyObject(partition_rv);
	create_stmt.tableElts		= list_make1(copyObject(&like_clause));
	create_stmt.inhRelations	= list_make1(copyObject(parent_rv));
	create_stmt.ofTypename		= NULL;
	create_stmt.constraints		= NIL;
	create_stmt.options			= NIL;
	create_stmt.oncommit		= ONCOMMIT_NOOP;
	create_stmt.tablespacename	= tablespace;
	create_stmt.if_not_exists	= false;

#if defined(PGPRO_EE) && PG_VERSION_NUM >= 90600
	create_stmt.partition_info	= NULL;
#endif

	/* Do we have to escalate privileges? */
	if (need_priv_escalation)
	{
		/* Get current user's Oid and security context */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);

		/* Check that user's allowed to spawn partitions */
		if (ACLCHECK_OK != pg_class_aclcheck(parent_relid, save_userid,
											 ACL_SPAWN_PARTITIONS))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for parent relation \"%s\"",
							get_rel_name_or_relid(parent_relid)),
					 errdetail("user is not allowed to create new partitions"),
					 errhint("consider granting INSERT privilege")));

		/* Become superuser in order to bypass various ACL checks */
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	}

	/* Generate columns using the parent table */
	create_stmts = transformCreateStmt(&create_stmt, NULL);

	/* Create the partition and all required relations */
	foreach (lc, create_stmts)
	{
		Node *cur_stmt;

		/* Fetch current CreateStmt */
		cur_stmt = (Node *) lfirst(lc);

		if (IsA(cur_stmt, CreateStmt))
		{
			Oid child_relowner;

			/* Partition should have the same owner as the parent */
			child_relowner = get_rel_owner(parent_relid);

			/* Create a partition and save its Oid */
			partition_relid = create_table_using_stmt((CreateStmt *) cur_stmt,
													  child_relowner).objectId;

			/* Copy FOREIGN KEYS of the parent table */
			copy_foreign_keys(parent_relid, partition_relid);

			/* Make changes visible */
			CommandCounterIncrement();

			/* Copy ACL privileges of the parent table and set "attislocal" */
			postprocess_child_table_and_atts(parent_relid, partition_relid);
		}
		else if (IsA(cur_stmt, CreateForeignTableStmt))
		{
			elog(ERROR, "FDW partition creation is not implemented yet");
		}
		else
		{
			/*
			 * Recurse for anything else.  Note the recursive
			 * call will stash the objects so created into our
			 * event trigger context.
			 */
			ProcessUtility(cur_stmt,
						   "we have to provide a query string",
						   PROCESS_UTILITY_SUBCOMMAND,
						   NULL,
						   None_Receiver,
						   NULL);
		}

		/* Update config one more time */
		CommandCounterIncrement();
	}

	/* Restore user's privileges */
	if (need_priv_escalation)
		SetUserIdAndSecContext(save_userid, save_sec_context);

	return partition_relid;
}

/* Create a new table using cooked CreateStmt */
static ObjectAddress
create_table_using_stmt(CreateStmt *create_stmt, Oid relowner)
{
	ObjectAddress	table_addr;
	Datum			toast_options;
	static char	   *validnsps[] = HEAP_RELOPT_NAMESPACES;
	int				guc_level;

	/* Create new GUC level... */
	guc_level = NewGUCNestLevel();

	/* ... and set client_min_messages = warning */
	(void) set_config_option("client_min_messages", "WARNING",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/* Create new partition owned by parent's posessor */
	table_addr = DefineRelation(create_stmt, RELKIND_RELATION, relowner, NULL);

	/* Save data about a simple DDL command that was just executed */
	EventTriggerCollectSimpleCommand(table_addr,
									 InvalidObjectAddress,
									 (Node *) create_stmt);

	/*
	 * Let NewRelationCreateToastTable decide if this
	 * one needs a secondary relation too.
	 */
	CommandCounterIncrement();

	/* Parse and validate reloptions for the toast table */
	toast_options = transformRelOptions((Datum) 0, create_stmt->options,
										"toast", validnsps, true, false);

	/* Parse options for a new toast table */
	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	/* Now create the toast table if needed */
	NewRelationCreateToastTable(table_addr.objectId, toast_options);

	/* Restore original GUC values */
	AtEOXact_GUC(true, guc_level);

	/* Return the address */
	return table_addr;
}

/* Copy ACL privileges of parent table and set "attislocal" = true */
static void
postprocess_child_table_and_atts(Oid parent_relid, Oid partition_relid)
{
	Relation		parent_rel,
					partition_rel,
					pg_class_rel,
					pg_attribute_rel;

	TupleDesc		pg_class_desc,
					pg_attribute_desc;

	List		   *translated_vars;

	HeapTuple		htup;
	ScanKeyData		skey[2];
	SysScanDesc		scan;

	Datum			acl_datum;
	bool			acl_null;

	Snapshot		snapshot;

	/* Both parent & partition have already been locked */
	parent_rel = heap_open(parent_relid, NoLock);
	partition_rel = heap_open(partition_relid, NoLock);

	make_inh_translation_list(parent_rel, partition_rel, 0, &translated_vars);

	heap_close(parent_rel, NoLock);
	heap_close(partition_rel, NoLock);

	/* Open catalog's relations */
	pg_class_rel = heap_open(RelationRelationId, RowExclusiveLock);
	pg_attribute_rel = heap_open(AttributeRelationId, RowExclusiveLock);

	/* Get most recent snapshot */
	snapshot = RegisterSnapshot(GetLatestSnapshot());

	pg_class_desc = RelationGetDescr(pg_class_rel);
	pg_attribute_desc = RelationGetDescr(pg_attribute_rel);

	htup = SearchSysCache1(RELOID, ObjectIdGetDatum(parent_relid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for relation %u", parent_relid);

	/* Get parent's ACL */
	acl_datum = heap_getattr(htup, Anum_pg_class_relacl, pg_class_desc, &acl_null);

	/* Copy datum if it's not NULL */
	if (!acl_null)
	{
		Form_pg_attribute acl_column;

		acl_column = pg_class_desc->attrs[Anum_pg_class_relacl - 1];

		acl_datum = datumCopy(acl_datum, acl_column->attbyval, acl_column->attlen);
	}

	/* Release 'htup' */
	ReleaseSysCache(htup);

	/* Search for 'partition_relid' */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partition_relid));

	scan = systable_beginscan(pg_class_rel, ClassOidIndexId,
							  true, snapshot, 1, skey);

	/* There should be exactly one tuple (our child) */
	if (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		ItemPointerData		iptr;
		Datum				values[Natts_pg_class]		= { (Datum) 0 };
		bool				nulls[Natts_pg_class]		= { false };
		bool				replaces[Natts_pg_class]	= { false };

		/* Copy ItemPointer of this tuple */
		iptr = htup->t_self;

		values[Anum_pg_class_relacl - 1] = acl_datum;	/* ACL array */
		nulls[Anum_pg_class_relacl - 1] = acl_null;		/* do we have ACL? */
		replaces[Anum_pg_class_relacl - 1] = true;

		/* Build new tuple with parent's ACL */
		htup = heap_modify_tuple(htup, pg_class_desc, values, nulls, replaces);

		/* Update child's tuple */
		simple_heap_update(pg_class_rel, &iptr, htup);

		/* Don't forget to update indexes */
		CatalogUpdateIndexes(pg_class_rel, htup);
	}

	systable_endscan(scan);


	/* Search for 'parent_relid's columns */
	ScanKeyInit(&skey[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_relid));

	/* Consider only user-defined columns (>0) */
	ScanKeyInit(&skey[1],
				Anum_pg_attribute_attnum,
				BTEqualStrategyNumber, F_INT2GT,
				Int16GetDatum(InvalidAttrNumber));

	scan = systable_beginscan(pg_attribute_rel, AttributeRelidNumIndexId,
							  true, snapshot, lengthof(skey), skey);

	/* Go through the list of parent's columns */
	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		ScanKeyData		subskey[2];
		SysScanDesc		subscan;
		HeapTuple		subhtup;

		AttrNumber		cur_attnum;
		bool			cur_attnum_null;
		Var			   *cur_var;

		/* Get parent column's ACL */
		acl_datum = heap_getattr(htup, Anum_pg_attribute_attacl,
								 pg_attribute_desc, &acl_null);

		/* Copy datum if it's not NULL */
		if (!acl_null)
		{
			Form_pg_attribute acl_column;

			acl_column = pg_attribute_desc->attrs[Anum_pg_attribute_attacl - 1];

			acl_datum = datumCopy(acl_datum,
								  acl_column->attbyval,
								  acl_column->attlen);
		}

		/* Fetch number of current column (parent) */
		cur_attnum = DatumGetInt16(heap_getattr(htup, Anum_pg_attribute_attnum,
												pg_attribute_desc, &cur_attnum_null));
		Assert(cur_attnum_null == false); /* must not be NULL! */

		/* Fetch Var of partition's corresponding column */
		cur_var = (Var *) list_nth(translated_vars, cur_attnum - 1);
		if (!cur_var)
			continue; /* column is dropped */

		Assert(cur_var->varattno != InvalidAttrNumber);

		/* Search for 'partition_relid' */
		ScanKeyInit(&subskey[0],
					Anum_pg_attribute_attrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(partition_relid));

		/* Search for 'partition_relid's columns */
		ScanKeyInit(&subskey[1],
					Anum_pg_attribute_attnum,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(cur_var->varattno)); /* partition's column */

		subscan = systable_beginscan(pg_attribute_rel, AttributeRelidNumIndexId,
									 true, snapshot, lengthof(subskey), subskey);

		/* There should be exactly one tuple (our child's column) */
		if (HeapTupleIsValid(subhtup = systable_getnext(subscan)))
		{
			ItemPointerData		iptr;
			Datum				values[Natts_pg_attribute]		= { (Datum) 0 };
			bool				nulls[Natts_pg_attribute]		= { false };
			bool				replaces[Natts_pg_attribute]	= { false };

			/* Copy ItemPointer of this tuple */
			iptr = subhtup->t_self;

			/* Change ACL of this column */
			values[Anum_pg_attribute_attacl - 1] = acl_datum;	/* ACL array */
			nulls[Anum_pg_attribute_attacl - 1] = acl_null;		/* do we have ACL? */
			replaces[Anum_pg_attribute_attacl - 1] = true;

			/* Change 'attislocal' for DROP COLUMN */
			values[Anum_pg_attribute_attislocal - 1] = false;	/* should not be local */
			nulls[Anum_pg_attribute_attislocal - 1] = false;	/* NOT NULL */
			replaces[Anum_pg_attribute_attislocal - 1] = true;

			/* Build new tuple with parent's ACL */
			subhtup = heap_modify_tuple(subhtup, pg_attribute_desc,
										values, nulls, replaces);

			/* Update child's tuple */
			simple_heap_update(pg_attribute_rel, &iptr, subhtup);

			/* Don't forget to update indexes */
			CatalogUpdateIndexes(pg_attribute_rel, subhtup);
		}

		systable_endscan(subscan);
	}

	systable_endscan(scan);

	/* Don't forget to free snapshot */
	UnregisterSnapshot(snapshot);

	heap_close(pg_class_rel, RowExclusiveLock);
	heap_close(pg_attribute_rel, RowExclusiveLock);
}

/* Copy foreign keys of parent table */
static void
copy_foreign_keys(Oid parent_relid, Oid partition_oid)
{
	Oid						copy_fkeys_proc_args[] = { REGCLASSOID, REGCLASSOID };
	List				   *copy_fkeys_proc_name;
	FmgrInfo				copy_fkeys_proc_flinfo;
	FunctionCallInfoData	copy_fkeys_proc_fcinfo;
	char					*pathman_schema;

	/* Fetch pg_pathman's schema */
	pathman_schema = get_namespace_name(get_pathman_schema());

	/* Build function's name */
	copy_fkeys_proc_name = list_make2(makeString(pathman_schema),
									  makeString(CppAsString(copy_foreign_keys)));

	/* Lookup function's Oid and get FmgrInfo */
	fmgr_info(LookupFuncName(copy_fkeys_proc_name, 2,
							 copy_fkeys_proc_args, false),
			  &copy_fkeys_proc_flinfo);

	InitFunctionCallInfoData(copy_fkeys_proc_fcinfo, &copy_fkeys_proc_flinfo,
							 2, InvalidOid, NULL, NULL);
	copy_fkeys_proc_fcinfo.arg[0] = ObjectIdGetDatum(parent_relid);
	copy_fkeys_proc_fcinfo.argnull[0] = false;
	copy_fkeys_proc_fcinfo.arg[1] = ObjectIdGetDatum(partition_oid);
	copy_fkeys_proc_fcinfo.argnull[1] = false;

	/* Invoke the callback */
	FunctionCallInvoke(&copy_fkeys_proc_fcinfo);
}


/*
 * -----------------------------
 *  Check constraint generation
 * -----------------------------
 */

/* Drop pg_pathman's check constraint by 'relid' */
void
drop_check_constraint(Oid relid)
{
	char		   *constr_name;
	AlterTableStmt *stmt;
	AlterTableCmd  *cmd;

	/* Build a correct name for this constraint */
	constr_name = build_check_constraint_name_relid_internal(relid);

	stmt = makeNode(AlterTableStmt);
	stmt->relation	= makeRangeVarFromRelid(relid);
	stmt->relkind	= OBJECT_TABLE;

	cmd = makeNode(AlterTableCmd);
	cmd->subtype	= AT_DropConstraint;
	cmd->name		= constr_name;
	cmd->behavior	= DROP_RESTRICT;
	cmd->missing_ok	= true;

	stmt->cmds = list_make1(cmd);

	AlterTable(relid, ShareUpdateExclusiveLock, stmt);
}

/* Build RANGE check constraint expression tree */
Node *
build_raw_range_check_tree(Node *raw_expression,
						   const Bound *start_value,
						   const Bound *end_value,
						   Oid value_type)
{
	BoolExpr   *and_oper	= makeNode(BoolExpr);
	A_Expr	   *left_arg	= makeNode(A_Expr),
			   *right_arg	= makeNode(A_Expr);
	A_Const	   *left_const	= makeNode(A_Const),
			   *right_const	= makeNode(A_Const);

	and_oper->boolop	= AND_EXPR;
	and_oper->args		= NIL;
	and_oper->location	= -1;

	/* Left comparison (VAR >= start_value) */
	if (!IsInfinite(start_value))
	{
		/* Left boundary */
		left_const->val = make_string_value_struct(
			datum_to_cstring(BoundGetValue(start_value), value_type));
		left_const->location = -1;

		left_arg->name		= list_make1(makeString(">="));
		left_arg->kind		= AEXPR_OP;
		left_arg->lexpr		= raw_expression;
		left_arg->rexpr		= (Node *) left_const;
		left_arg->location	= -1;

		and_oper->args = lappend(and_oper->args, left_arg);
	}

	/* Right comparision (VAR < end_value) */
	if (!IsInfinite(end_value))
	{
		/* Right boundary */
		right_const->val = make_string_value_struct(
			datum_to_cstring(BoundGetValue(end_value), value_type));
		right_const->location = -1;

		right_arg->name		= list_make1(makeString("<"));
		right_arg->kind		= AEXPR_OP;
		right_arg->lexpr	= raw_expression;
		right_arg->rexpr	= (Node *) right_const;
		right_arg->location	= -1;

		and_oper->args = lappend(and_oper->args, right_arg);
	}

	/* (-inf, +inf) */
	if (and_oper->args == NIL)
		elog(ERROR, "cannot create partition with range (-inf, +inf)");

	return (Node *) and_oper;
}

/* Build complete RANGE check constraint */
Constraint *
build_range_check_constraint(Oid child_relid,
							 Node *raw_expression,
							 const Bound *start_value,
							 const Bound *end_value,
							 Oid value_type)
{
	Constraint	   *range_constr;
	char		   *range_constr_name;

	/* Build a correct name for this constraint */
	range_constr_name = build_check_constraint_name_relid_internal(child_relid);

	/* Initialize basic properties of a CHECK constraint */
	range_constr = make_constraint_common(range_constr_name,
										  build_raw_range_check_tree(raw_expression,
																	 start_value,
																	 end_value,
																	 value_type));
	/* Everything seems to be fine */
	return range_constr;
}

/* Check if range overlaps with any partitions */
bool
check_range_available(Oid parent_relid,
					  const Bound *start,
					  const Bound *end,
					  Oid value_type,
					  bool raise_error)
{
	const PartRelationInfo	   *prel;
	RangeEntry				   *ranges;
	FmgrInfo					cmp_func;
	uint32						i;

	/* Try fetching the PartRelationInfo structure */
	prel = get_pathman_relation_info(parent_relid);

	/* If there's no prel, return TRUE (overlap is not possible) */
	if (!prel)
	{
		ereport(WARNING, (errmsg("table \"%s\" is not partitioned",
								 get_rel_name_or_relid(parent_relid))));
		return true;
	}

	/* Emit an error if it is not partitioned by RANGE */
	shout_if_prel_is_invalid(parent_relid, prel, PT_RANGE);

	/* Fetch comparison function */
	fill_type_cmp_fmgr_info(&cmp_func,
							getBaseType(value_type),
							getBaseType(prel->ev_type));

	ranges = PrelGetRangesArray(prel);
	for (i = 0; i < PrelChildrenCount(prel); i++)
	{
		int c1, c2;

		c1 = cmp_bounds(&cmp_func, prel->ev_collid, start, &ranges[i].max);
		c2 = cmp_bounds(&cmp_func, prel->ev_collid, end, &ranges[i].min);

		/* There's something! */
		if (c1 < 0 && c2 > 0)
		{
			if (raise_error)
				elog(ERROR, "specified range [%s, %s) overlaps "
							"with existing partitions",
					 !IsInfinite(start) ?
						 datum_to_cstring(BoundGetValue(start), value_type) :
						 "NULL",
					 !IsInfinite(end) ?
						 datum_to_cstring(BoundGetValue(end), value_type) :
						 "NULL");

			else return false;
		}
	}

	return true;
}

/* Build HASH check constraint expression tree */
Node *
build_raw_hash_check_tree(Node *raw_expression,
						  uint32 part_idx,
						  uint32 part_count,
						  Oid relid,
						  Oid value_type)
{
	A_Expr		   *eq_oper			= makeNode(A_Expr);
	FuncCall	   *part_idx_call	= makeNode(FuncCall),
				   *hash_call		= makeNode(FuncCall);
	A_Const		   *part_idx_c		= makeNode(A_Const),
				   *part_count_c	= makeNode(A_Const);

	List		   *get_hash_part_idx_proc;

	Oid				hash_proc;
	TypeCacheEntry *tce;

	tce = lookup_type_cache(value_type, TYPECACHE_HASH_PROC);
	hash_proc = tce->hash_proc;

	/* Total amount of partitions */
	part_count_c->val = make_int_value_struct(part_count);
	part_count_c->location = -1;

	/* Index of this partition (hash % total amount) */
	part_idx_c->val = make_int_value_struct(part_idx);
	part_idx_c->location = -1;

	/* Call hash_proc() */
	hash_call->funcname			= list_make1(makeString(get_func_name(hash_proc)));
	hash_call->args				= list_make1(raw_expression);
	hash_call->agg_order		= NIL;
	hash_call->agg_filter		= NULL;
	hash_call->agg_within_group	= false;
	hash_call->agg_star			= false;
	hash_call->agg_distinct		= false;
	hash_call->func_variadic	= false;
	hash_call->over				= NULL;
	hash_call->location			= -1;

	/* Build schema-qualified name of function get_hash_part_idx() */
	get_hash_part_idx_proc =
			list_make2(makeString(get_namespace_name(get_pathman_schema())),
					   makeString("get_hash_part_idx"));

	/* Call get_hash_part_idx() */
	part_idx_call->funcname			= get_hash_part_idx_proc;
	part_idx_call->args				= list_make2(hash_call, part_count_c);
	part_idx_call->agg_order		= NIL;
	part_idx_call->agg_filter		= NULL;
	part_idx_call->agg_within_group	= false;
	part_idx_call->agg_star			= false;
	part_idx_call->agg_distinct		= false;
	part_idx_call->func_variadic	= false;
	part_idx_call->over				= NULL;
	part_idx_call->location			= -1;

	/* Construct equality operator */
	eq_oper->kind = AEXPR_OP;
	eq_oper->name = list_make1(makeString("="));
	eq_oper->lexpr = (Node *) part_idx_call;
	eq_oper->rexpr = (Node *) part_idx_c;
	eq_oper->location = -1;

	return (Node *) eq_oper;
}

/* Build complete HASH check constraint */
Constraint *
build_hash_check_constraint(Oid child_relid,
							Node *raw_expression,
							uint32 part_idx,
							uint32 part_count,
							Oid value_type)
{
	Constraint	   *hash_constr;
	char		   *hash_constr_name;

	/* Build a correct name for this constraint */
	hash_constr_name = build_check_constraint_name_relid_internal(child_relid);

	/* Initialize basic properties of a CHECK constraint */
	hash_constr = make_constraint_common(hash_constr_name,
										 build_raw_hash_check_tree(raw_expression,
																   part_idx,
																   part_count,
																   child_relid,
																   value_type));
	/* Everything seems to be fine */
	return hash_constr;
}

static Constraint *
make_constraint_common(char *name, Node *raw_expr)
{
	Constraint *constraint;

	/* Initialize basic properties of a CHECK constraint */
	constraint = makeNode(Constraint);
	constraint->conname			= name;
	constraint->deferrable		= false;
	constraint->initdeferred	= false;
	constraint->location		= -1;
	constraint->contype			= CONSTR_CHECK;
	constraint->is_no_inherit	= false;

	/* Validate existing data using this constraint */
	constraint->skip_validation	= false;
	constraint->initially_valid	= true;

	/* Finally we should build an expression tree */
	constraint->raw_expr		= raw_expr;

	return constraint;
}

static Value
make_string_value_struct(char *str)
{
	Value val;

	val.type = T_String;
	val.val.str = str;

	return val;
}

static Value
make_int_value_struct(int int_val)
{
	Value val;

	val.type = T_Integer;
	val.val.ival = int_val;

	return val;
}


/*
 * ---------------------
 *  Callback invocation
 * ---------------------
 */

/* Invoke 'init_callback' for a partition */
static void
invoke_init_callback_internal(init_callback_params *cb_params)
{
#define JSB_INIT_VAL(value, val_type, val_cstring) \
	do { \
		if ((val_cstring) != NULL) \
		{ \
			(value)->type = jbvString; \
			(value)->val.string.len = strlen(val_cstring); \
			(value)->val.string.val = val_cstring; \
		} \
		else \
		{ \
			(value)->type = jbvNull; \
			Assert((val_type) != WJB_KEY); \
		} \
		\
		pushJsonbValue(&jsonb_state, val_type, (value)); \
	} while (0)

	Oid						parent_oid = cb_params->parent_relid;
	Oid						partition_oid = cb_params->partition_relid;

	FmgrInfo				cb_flinfo;
	FunctionCallInfoData	cb_fcinfo;

	JsonbParseState		   *jsonb_state = NULL;
	JsonbValue			   *result,
							key,
							val;

	char				   *parent_name,
						   *parent_namespace,
						   *partition_name,
						   *partition_namespace;


	/* Fetch & cache callback's Oid if needed */
	if (!cb_params->callback_is_cached)
	{
		Datum	param_values[Natts_pathman_config_params];
		bool	param_isnull[Natts_pathman_config_params];

		/* Search for init_callback entry in PATHMAN_CONFIG_PARAMS */
		if (read_pathman_params(parent_oid, param_values, param_isnull))
		{
			Datum		init_cb_datum; /* signature of init_callback */
			AttrNumber	init_cb_attno = Anum_pathman_config_params_init_callback;

			/* Extract Datum storing callback's signature */
			init_cb_datum = param_values[init_cb_attno - 1];

			/* Cache init_callback's Oid */
			if (init_cb_datum)
			{
				/* Try fetching callback's Oid */
				cb_params->callback = text_to_regprocedure(DatumGetTextP(init_cb_datum));

				if (!RegProcedureIsValid(cb_params->callback))
					ereport(ERROR,
							(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
							 errmsg("callback function \"%s\" does not exist",
									TextDatumGetCString(init_cb_datum))));
			}
			/* There's no callback */
			else cb_params->callback = InvalidOid;

			/* We've made a lookup */
			cb_params->callback_is_cached = true;
		}
	}

	/* No callback is set, exit */
	if (!OidIsValid(cb_params->callback))
		return;

	/* Validate the callback's signature */
	validate_part_callback(cb_params->callback, true);

	parent_name = get_rel_name(parent_oid);
	parent_namespace = get_namespace_name(get_rel_namespace(parent_oid));

	partition_name = get_rel_name(partition_oid);
	partition_namespace = get_namespace_name(get_rel_namespace(partition_oid));

	/* Generate JSONB we're going to pass to callback */
	switch (cb_params->parttype)
	{
		case PT_HASH:
			{
				pushJsonbValue(&jsonb_state, WJB_BEGIN_OBJECT, NULL);

				JSB_INIT_VAL(&key, WJB_KEY, "parent");
				JSB_INIT_VAL(&val, WJB_VALUE, parent_name);
				JSB_INIT_VAL(&key, WJB_KEY, "parent_schema");
				JSB_INIT_VAL(&val, WJB_VALUE, parent_namespace);
				JSB_INIT_VAL(&key, WJB_KEY, "partition");
				JSB_INIT_VAL(&val, WJB_VALUE, partition_name);
				JSB_INIT_VAL(&key, WJB_KEY, "partition_schema");
				JSB_INIT_VAL(&val, WJB_VALUE, partition_namespace);
				JSB_INIT_VAL(&key, WJB_KEY, "parttype");
				JSB_INIT_VAL(&val, WJB_VALUE, PartTypeToCString(PT_HASH));

				result = pushJsonbValue(&jsonb_state, WJB_END_OBJECT, NULL);
			}
			break;

		case PT_RANGE:
			{
				char   *start_value	= NULL,
					   *end_value	= NULL;
				Bound	sv_datum	= cb_params->params.range_params.start_value,
						ev_datum	= cb_params->params.range_params.end_value;
				Oid		type		= cb_params->params.range_params.value_type;

				/* Convert min to CSTRING */
				if (!IsInfinite(&sv_datum))
					start_value = datum_to_cstring(BoundGetValue(&sv_datum), type);

				/* Convert max to CSTRING */
				if (!IsInfinite(&ev_datum))
					end_value = datum_to_cstring(BoundGetValue(&ev_datum), type);

				pushJsonbValue(&jsonb_state, WJB_BEGIN_OBJECT, NULL);

				JSB_INIT_VAL(&key, WJB_KEY, "parent");
				JSB_INIT_VAL(&val, WJB_VALUE, parent_name);
				JSB_INIT_VAL(&key, WJB_KEY, "parent_schema");
				JSB_INIT_VAL(&val, WJB_VALUE, parent_namespace);
				JSB_INIT_VAL(&key, WJB_KEY, "partition");
				JSB_INIT_VAL(&val, WJB_VALUE, partition_name);
				JSB_INIT_VAL(&key, WJB_KEY, "partition_schema");
				JSB_INIT_VAL(&val, WJB_VALUE, partition_namespace);
				JSB_INIT_VAL(&key, WJB_KEY, "parttype");
				JSB_INIT_VAL(&val, WJB_VALUE, PartTypeToCString(PT_RANGE));

				/* Lower bound */
				JSB_INIT_VAL(&key, WJB_KEY, "range_min");
				JSB_INIT_VAL(&val, WJB_VALUE, start_value);

				/* Upper bound */
				JSB_INIT_VAL(&key, WJB_KEY, "range_max");
				JSB_INIT_VAL(&val, WJB_VALUE, end_value);

				result = pushJsonbValue(&jsonb_state, WJB_END_OBJECT, NULL);
			}
			break;

		default:
			WrongPartType(cb_params->parttype);
	}

	/* Fetch function call data */
	fmgr_info(cb_params->callback, &cb_flinfo);

	InitFunctionCallInfoData(cb_fcinfo, &cb_flinfo, 1, InvalidOid, NULL, NULL);
	cb_fcinfo.arg[0] = PointerGetDatum(JsonbValueToJsonb(result));
	cb_fcinfo.argnull[0] = false;

	/* Invoke the callback */
	FunctionCallInvoke(&cb_fcinfo);
}

/* Invoke a callback of a specified type */
void
invoke_part_callback(init_callback_params *cb_params)
{
	switch (cb_params->cb_type)
	{
		case PT_INIT_CALLBACK:
			invoke_init_callback_internal(cb_params);
			break;

		default:
			elog(ERROR, "Unknown callback type: %u", cb_params->cb_type);
	}
}

/*
 * Checks that callback function meets specific requirements.
 * It must have the only JSONB argument and BOOL return type.
 */
bool
validate_part_callback(Oid procid, bool emit_error)
{
	HeapTuple		tp;
	Form_pg_proc	functup;
	bool			is_ok = true;

	if (procid == DEFAULT_INIT_CALLBACK)
		return true;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(procid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "callback function %u does not exist", procid);

	functup = (Form_pg_proc) GETSTRUCT(tp);

	if (functup->pronargs != 1 ||
		functup->proargtypes.values[0] != JSONBOID ||
		functup->prorettype != VOIDOID)
		is_ok = false;

	ReleaseSysCache(tp);

	if (emit_error && !is_ok)
		elog(ERROR,
			 "callback function must have the following signature: "
			 "callback(arg JSONB) RETURNS VOID");

	return is_ok;
}

/*
 * Utility function that converts signature of procedure into regprocedure.
 *
 * Precondition: proc_signature != NULL.
 *
 * Returns InvalidOid if proname_args is not found.
 * Raise error if it's incorrect.
 */
static Oid
text_to_regprocedure(text *proc_signature)
{
	FunctionCallInfoData	fcinfo;
	Datum					result;

	InitFunctionCallInfoData(fcinfo, NULL, 1, InvalidOid, NULL, NULL);

#if PG_VERSION_NUM >= 90600
	fcinfo.arg[0] = PointerGetDatum(proc_signature);
#else
	fcinfo.arg[0] = CStringGetDatum(text_to_cstring(proc_signature));
#endif
	fcinfo.argnull[0] = false;

	result = to_regprocedure(&fcinfo);

	return DatumGetObjectId(result);
}

/* Extract column names from raw expression */
static bool
extract_column_names(Node *node, List **columns)
{
	if (node == NULL)
		return false;

	if (IsA(node, ColumnRef))
	{
		ListCell *lc;

		foreach(lc, ((ColumnRef *) node)->fields)
			if (IsA(lfirst(lc), String))
				*columns = lappend(*columns, lfirst(lc));
	}

	return raw_expression_tree_walker(node, extract_column_names, columns);
}

/* Returns raw partitioning expression + expr_type + columns */
static Node *
build_partitioning_expression(Oid parent_relid,
							  Oid *expr_type,		/* ret val #1 */
							  List **columns)		/* ret val #2 */
{
	/* Values extracted from PATHMAN_CONFIG */
	Datum		values[Natts_pathman_config];
	bool		isnull[Natts_pathman_config];
	char	   *expr_cstr;
	Node	   *expr;

	/* Check that table is registered in PATHMAN_CONFIG */
	if (!pathman_config_contains_relation(parent_relid, values, isnull, NULL, NULL))
		elog(ERROR, "table \"%s\" is not partitioned",
			 get_rel_name_or_relid(parent_relid));

	expr_cstr = TextDatumGetCString(values[Anum_pathman_config_expression - 1]);
	expr = parse_partitioning_expression(parent_relid, expr_cstr, NULL, NULL);
	pfree(expr_cstr);

	/* We need expression type for hash functions */
	if (expr_type)
	{
		char *expr_p_cstr;

		/* We can safely assume that this field will always remain not null */
		Assert(!isnull[Anum_pathman_config_expression_p - 1]);
		expr_p_cstr =
				TextDatumGetCString(values[Anum_pathman_config_expression_p - 1]);

		/* Finally return expression type */
		*expr_type = exprType(stringToNode(expr_p_cstr));
	}

	if (columns)
	{
		/* Column list should be empty */
		AssertArg(*columns == NIL);
		extract_column_names(expr, columns);
	}

	return expr;
}

/*
 * -------------------------
 *  Update trigger creation
 * -------------------------
 */

/* Create trigger for partition */
void
create_single_update_trigger_internal(Oid partition_relid,
									  const char *trigname,
									  List *columns)
{
	CreateTrigStmt	   *stmt;
	List			   *func;

	func = list_make2(makeString(get_namespace_name(get_pathman_schema())),
					  makeString(CppAsString(pathman_update_trigger_func)));

	stmt = makeNode(CreateTrigStmt);
	stmt->trigname		= (char *) trigname;
	stmt->relation		= makeRangeVarFromRelid(partition_relid);
	stmt->funcname		= func;
	stmt->args			= NIL;
	stmt->row			= true;
	stmt->timing		= TRIGGER_TYPE_BEFORE;
	stmt->events		= TRIGGER_TYPE_UPDATE;
	stmt->columns		= columns;
	stmt->whenClause	= NULL;
	stmt->isconstraint	= false;
	stmt->deferrable	= false;
	stmt->initdeferred	= false;
	stmt->constrrel		= NULL;

	(void) CreateTrigger(stmt, NULL, InvalidOid, InvalidOid,
						 InvalidOid, InvalidOid, false);
}

/* Check if relation has pg_pathman's update trigger */
bool
has_update_trigger_internal(Oid parent_relid)
{
	bool			res = false;
	Relation		tgrel;
	SysScanDesc		scan;
	ScanKeyData		key[1];
	HeapTuple		tuple;
	const char	   *trigname;

	/* Build update trigger's name */
	trigname = build_update_trigger_name_internal(parent_relid);

	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_relid));

	scan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
							  true, NULL, lengthof(key), key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_trigger trigger = (Form_pg_trigger) GETSTRUCT(tuple);

		if (namestrcmp(&(trigger->tgname), trigname) == 0)
		{
			res = true;
			break;
		}
	}

	systable_endscan(scan);
	heap_close(tgrel, RowExclusiveLock);

	return res;
}
