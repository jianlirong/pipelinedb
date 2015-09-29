/*-------------------------------------------------------------------------
 *
 * cont_adhoc.c
 *
 * Copyright (c) 2013-2015, PipelineDB
 *
 * IDENTIFICATION
 *    src/include/pipeline/cont_adhoc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "pipeline/cont_adhoc.h"
#include "pipeline/tuplebuf.h"
#include "catalog/pipeline_query_fn.h"
#include "tcop/dest.h"
#include "executor/execdesc.h"
#include "pgstat.h"
#include "executor/executor.h"
#include "utils/memutils.h"
#include "executor/tupletableReceiver.h"
#include "utils/snapmgr.h"
#include "catalog/pipeline_query.h"
#include "access/htup_details.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "miscadmin.h"
#include "commands/pipelinecmds.h"
#include "nodes/makefuncs.h"
#include "access/xact.h"
#include "pipeline/cont_plan.h"
#include "utils/rel.h"
#include "executor/tstoreReceiver.h"
#include "nodes/print.h"
#include "tcop/utility.h"

#include "pipeline/cont_adhoc_sender.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "tcop/tcopprot.h"
#include "pipeline/cont_analyze.h"
#include "utils/syscache.h"
#include "tcop/pquery.h"

#include <unistd.h>

void
dump_tuplestore(Tuplestorestate *batch, TupleTableSlot *slot);

typedef struct {
	TupleBufferBatchReader  *reader;
	Oid                     view_id;
	ContinuousView          *view;
	DestReceiver            *dest;
	QueryDesc               *query_desc;
	CQStatEntry             stats;
	TimestampTz 			last_processed;
	uint32 					num_processed;

} AdhocWorkerState;

// exec_adhoc_worker
// init_query_state

typedef struct {
	QueryDesc               *query_desc;

	TupleDesc               tup_desc;
	TupleTableSlot          *slot;

	Tuplestorestate *batch;      /* to hold pre combine rows */
	TupleHashTable existing; 	 /* only needed if is_agg is true */
	Tuplestorestate *result; 	 /* holds the combined result */

	DestReceiver            *dest;

	bool is_agg;

} AdhocCombinerState;

typedef struct {

	Tuplestorestate *batch;
	QueryDesc *query_desc;
	DestReceiver *dest;
	TupleTableSlot          *slot;

} AdhocViewState;

static TupleHashTable
create_agg_hash(int num_cols, Oid *grp_ops, AttrNumber *group_atts)
{
	FmgrInfo *eq_funcs = 0;
	FmgrInfo *hash_funcs = 0;
	MemoryContext hash_tmp_cxt;

	execTuplesHashPrepare(num_cols, grp_ops,
						  &eq_funcs, &hash_funcs);

	/* this tmp cxt is reqd by hash table, it will reset it internally */

	hash_tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
			"AdhocCombinerQueryHashTableTempCxt",
			ALLOCSET_DEFAULT_MINSIZE,
			ALLOCSET_DEFAULT_INITSIZE,
			ALLOCSET_DEFAULT_MAXSIZE);

	return BuildTupleHashTable(num_cols, group_atts,
							   eq_funcs, hash_funcs, 1000,
							   sizeof(HeapTupleEntryData),
							   CurrentMemoryContext, hash_tmp_cxt);
}

/* read everything */
static bool
should_read_fn(TupleBufferReader *reader, TupleBufferSlot *slot)
{
	return true;
}

static void
init_cont_query_batch_context()
{
	ContQueryBatchContext = AllocSetContextCreate(CurrentMemoryContext,
			"ContQueryBatchContext",
			ALLOCSET_DEFAULT_MINSIZE,
			ALLOCSET_DEFAULT_INITSIZE,
			ALLOCSET_DEFAULT_MAXSIZE);
}

static void
set_reader(PlanState *planstate, TupleBufferBatchReader *reader)
{
	if (planstate == NULL)
		return;

	if (IsA(planstate, StreamScanState))
	{
		StreamScanState *scan = (StreamScanState *) planstate;
		scan->reader = reader;
		return;
	}
	else if (IsA(planstate, SubqueryScanState))
	{
		set_reader(((SubqueryScanState *) planstate)->subplan, reader);
		return;
	}

	set_reader(planstate->lefttree, reader);
	set_reader(planstate->righttree, reader);
}

//static void
//set_reader_on_view_plan(PlanState *planstate,
//						TupleBufferBatchReader *reader)
//{
//	if (planstate == NULL)
//		return;
//
//	// TransformSelectStmtForContProcess
//	if (IsA(planstate, SeqScanState))
//	{
//		// StreamScanState
//		// SeqScanState
//		// S*scan = (StreamScanState *) planstate;
//		// scan->reader = reader;
//		return;
//	}
//	else if (IsA(planstate, SubqueryScanState))
//	{
//		set_reader(((SubqueryScanState *) planstate)->subplan, reader);
//		return;
//	}
//
//	set_reader(planstate->lefttree, reader);
//	set_reader(planstate->righttree, reader);
//}

static EState *
create_estate(QueryDesc *query_desc)
{
	EState *estate;

	estate = CreateExecutorState();
	estate->es_param_list_info = query_desc->params;
	estate->es_snapshot = RegisterSnapshot(query_desc->snapshot);
	estate->es_crosscheck_snapshot = RegisterSnapshot(query_desc->crosscheck_snapshot);
	estate->es_instrument = query_desc->instrument_options;
	estate->es_range_table = query_desc->plannedstmt->rtable;
	estate->es_continuous = query_desc->plannedstmt->is_continuous;
	estate->es_lastoid = InvalidOid;
	estate->es_processed = estate->es_filtered = 0;

	if (query_desc->plannedstmt->nParamExec > 0)
		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(query_desc->plannedstmt->nParamExec * sizeof(ParamExecData));

	estate->es_top_eflags |= EXEC_FLAG_SKIP_TRIGGERS;

	return estate;
}

static void
set_snapshot(EState *estate, ResourceOwner owner)
{
	estate->es_snapshot = GetTransactionSnapshot();
	estate->es_snapshot->active_count++;
	estate->es_snapshot->copied = true;
	PushActiveSnapshot(estate->es_snapshot);
}

static void
unset_snapshot(EState *estate, ResourceOwner owner)
{
	PopActiveSnapshot();
	estate->es_snapshot = NULL;
}

static Oid
get_cont_view_id(RangeVar *name)
{
	HeapTuple tuple = GetPipelineQueryTuple(name);
	Form_pipeline_query row;
	Oid row_id;

	Assert(HeapTupleIsValid(tuple));
	row = (Form_pipeline_query) GETSTRUCT(tuple);

	row_id = row->id;
	ReleaseSysCache(tuple);

	return row_id;
}

static ContQueryProc *
get_cont_query_proc()
{
	Oid key = MyDatabaseId;
	ContQueryProc *proc = ContQueryGetAdhoc(key);

	Assert(proc);

	/* TODO - manage cq procs for adhoc queries correctly.
	 * cont_scheduler.c:start_group is currently doing the rest of this.
	 * id can't be too high - if the bms tries to resize it will die
	 */
	
	proc->id = 23; /* HACK */
	proc->latch = &MyProc->procLatch;
	proc->active = true;

	return proc;
}

static char*
get_unique_adhoc_view_name()
{
	char *relname = palloc0(NAMEDATALEN);
	RangeVar var = {T_RangeVar,0x0,0x0,relname,INH_DEFAULT,'p', 0x0, 0x0};

	int i = 0;

	while (true)
	{
		sprintf(relname, "adhoc%d", i++);

		if (!IsAContinuousView(&var))
		{
			break;
		}
	}

	return relname;
}

typedef struct{
	Oid view_id;
	ContinuousView *view;
} ContinuousViewData;

static ContinuousViewData
init_cont_view(SelectStmt *stmt, const char* querystring)
{
	ContinuousViewData view_data;
	RangeVar* view_name = makeRangeVar(0, get_unique_adhoc_view_name(), -1);

	CreateContViewStmt *create_stmt	= makeNode(CreateContViewStmt);

	create_stmt->into = makeNode(IntoClause);
	create_stmt->into->rel = view_name;
	create_stmt->query = (Node*) stmt;

	stmt->forContinuousView = true;

	PushActiveSnapshot(GetTransactionSnapshot());
	ExecCreateContViewStmt(create_stmt, querystring);
	PopActiveSnapshot();

	view_data.view_id = get_cont_view_id(view_name);
	view_data.view = GetContinuousView(view_data.view_id);

	return view_data;
}

void
dump_tuplestore(Tuplestorestate *batch, TupleTableSlot *slot)
{
	size_t ctr = 0;
	elog(LOG, "******************************");

	tuplestore_rescan(batch);

	foreach_tuple(slot, batch)
	{
		elog(LOG, "dump store %zu", ctr++);
		print_slot(slot);
	}
}

static AdhocWorkerState*
init_adhoc_worker(ContinuousViewData data, DestReceiver *receiver)
{
	PlannedStmt *pstmt = 0;
	AdhocWorkerState *state = palloc0(sizeof(AdhocWorkerState));
	ResourceOwner owner = CurrentResourceOwner;

	init_cont_query_batch_context();

	state->reader = 
		TupleBufferOpenBatchReader(WorkerTupleBuffer, &should_read_fn);

	state->view_id = data.view_id;
	state->view = data.view;

	Assert(state->view);
	state->dest = receiver;

	// GetContPlan
	PushActiveSnapshot(GetTransactionSnapshot());
	pstmt = GetContPlan(state->view, Worker);
	PopActiveSnapshot();

	state->query_desc = CreateQueryDesc(pstmt, NULL, InvalidSnapshot,
										InvalidSnapshot, state->dest, NULL, 0);

	state->query_desc->snapshot = GetTransactionSnapshot();
	state->query_desc->snapshot->copied = true;

	RegisterSnapshotOnOwner(state->query_desc->snapshot, owner);

	ExecutorStart(state->query_desc, 0);

	state->query_desc->snapshot->active_count++;
	UnregisterSnapshotFromOwner(state->query_desc->snapshot, owner);
	UnregisterSnapshotFromOwner(state->query_desc->estate->es_snapshot, owner);
	state->query_desc->snapshot = NULL;

	set_reader(state->query_desc->planstate, state->reader);

	state->query_desc->estate->es_lastoid = InvalidOid;

	(*state->dest->rStartup) (state->dest, 
			state->query_desc->operation, state->query_desc->tupDesc);

	cq_stat_init(&state->stats, state->view->id, 0);

	/*
	 * The main loop initializes and ends plans across plan executions, so 
	 * it expects the plan to be uninitialized
	 */
	ExecEndNode(state->query_desc->planstate);
	FreeExecutorState(state->query_desc->estate);

	// CommitTransactionCommand

	state->last_processed = GetCurrentTimestamp();

	return state;
}

static TupleDesc
prepare_combine_plan(RangeVar *matrel, PlannedStmt *plan,
					 Tuplestorestate *batch)
{
	TuplestoreScan *scan;

	Relation rel = heap_openrv(matrel, AccessShareLock);

	plan->is_continuous = false;

	scan = SetCombinerPlanTuplestorestate(plan, batch);
	scan->desc = CreateTupleDescCopy(RelationGetDescr(rel));

	heap_close(rel, AccessShareLock);

	return scan->desc;

}

static AdhocCombinerState*
init_adhoc_combiner(ContinuousViewData data,
					Tuplestorestate *batch)
{
	AdhocCombinerState *state = palloc0(sizeof(AdhocWorkerState));
	PlannedStmt *pstmt = 0;

	PushActiveSnapshot(GetTransactionSnapshot());
	pstmt = GetContPlan(data.view, Combiner);
	PopActiveSnapshot();

	state->tup_desc = prepare_combine_plan(data.view->matrel, pstmt, batch);
	state->dest = CreateDestReceiver(DestTuplestore);
	 
	state->query_desc = CreateQueryDesc(pstmt, NULL, InvalidSnapshot,
										InvalidSnapshot, state->dest,
										NULL, 0);

	state->slot = MakeSingleTupleTableSlot(state->tup_desc);

	state->batch = batch;

	state->result = tuplestore_begin_heap(true, true,
							continuous_query_combiner_work_mem);

	SetTuplestoreDestReceiverParams(state->dest, state->result,
									CurrentMemoryContext, true);

	// required by combiner group stuff.

	if (IsA(pstmt->planTree, Agg))
	{
		Agg *agg = (Agg *) pstmt->planTree;
		state->is_agg = true;

		state->existing = create_agg_hash(agg->numCols, agg->grpOperators,
										  agg->grpColIdx);
	}

	ExecutorStart(state->query_desc, 0);
	state->query_desc->estate->es_lastoid = InvalidOid;

	(*state->dest->rStartup) (state->dest, 
			state->query_desc->operation, state->query_desc->tupDesc);

	ExecEndNode(state->query_desc->planstate);
	FreeExecutorState(state->query_desc->estate);

	return state;
}

static bool
exec_adhoc_worker(AdhocWorkerState* state)
{
	struct Plan *plan = 0;
	bool has_tup = false;
	EState *estate = 0;
	ResourceOwner owner = CurrentResourceOwner;

	TupleBufferBatchReaderTrySleep(state->reader, state->last_processed);

	has_tup = 
		TupleBufferBatchReaderHasTuplesForCQId(state->reader, state->view_id);

	if (!has_tup)
	{
		TupleBufferBatchReaderRewind(state->reader);
		return false;
	}

	state->query_desc->estate = create_estate(state->query_desc);
	estate = state->query_desc->estate;

	TupleBufferBatchReaderSetCQId(state->reader, state->view_id);

	set_snapshot(estate, owner);

	plan = state->query_desc->plannedstmt->planTree;

	state->query_desc->planstate = 
		ExecInitNode(plan, state->query_desc->estate, 0);

	set_reader(state->query_desc->planstate, state->reader);

	ExecutePlan(estate, state->query_desc->planstate, 
				state->query_desc->operation,
				true, 0, 0, ForwardScanDirection, state->dest);

	ExecEndNode(state->query_desc->planstate);
	state->query_desc->planstate = NULL;
	state->num_processed = estate->es_processed + estate->es_filtered;

	if (state->num_processed)
	{
		state->last_processed = GetCurrentTimestamp();
	}

	unset_snapshot(estate, owner);

	state->query_desc->estate = NULL;
	estate = state->query_desc->estate;

	TupleBufferBatchReaderRewind(state->reader);
	TupleBufferBatchReaderReset(state->reader);

	MemoryContextResetAndDeleteChildren(ContQueryBatchContext);

	return state->num_processed != 0;
}

static void
adhoc_sync_combine(AdhocCombinerState* state);

static void
exec_adhoc_combiner(AdhocCombinerState* state)
{
	ResourceOwner owner = CurrentResourceOwner;
	struct Plan *plan = 0;
	EState *estate = 0;

	/* put all existing groups in with batch. */

	if (state->is_agg)
	{
		foreach_tuple(state->slot, state->batch)
		{
			HeapTupleEntry entry = (HeapTupleEntry) 
				LookupTupleHashEntry(state->existing, state->slot, NULL);

			if (entry)
			{
				/* if the group exists, copy it into 'result'
				 * we are using 'result' as scratch space here. */

				tuplestore_puttuple(state->result, entry->tuple);
			}
			else
			{
			}
		}

		foreach_tuple(state->slot, state->result)
		{
//			print_slot(state->slot);
//			fflush(stdout);

			tuplestore_puttupleslot(state->batch, state->slot);
		}

		tuplestore_clear(state->result);
		tuplestore_rescan(state->batch);
	}

	state->query_desc->estate = create_estate(state->query_desc);
	estate = state->query_desc->estate;

	set_snapshot(estate, owner);

	plan = state->query_desc->plannedstmt->planTree;

	state->query_desc->planstate = 
		ExecInitNode(plan, state->query_desc->estate, 0);

	ExecutePlan(estate, state->query_desc->planstate, 
				state->query_desc->operation,
				true, 0, 0, ForwardScanDirection, state->dest);

	ExecEndNode(state->query_desc->planstate);
	state->query_desc->planstate = NULL;

	unset_snapshot(estate, owner);

	state->query_desc->estate = NULL;
	estate = state->query_desc->estate;

	MemoryContextResetAndDeleteChildren(ContQueryBatchContext);

	adhoc_sync_combine(state);
}

static void
adhoc_sync_combine(AdhocCombinerState* state)
{
	tuplestore_clear(state->batch);
	tuplestore_rescan(state->result);

	MemoryContext old_cxt;

	// lame

	foreach_tuple(state->slot, state->result)
	{
		if (state->is_agg)
		{
			bool isnew = false;

			HeapTupleEntry entry = (HeapTupleEntry) 
				LookupTupleHashEntry(state->existing, state->slot, &isnew);

			if (!isnew)
			{
				heap_freetuple(entry->tuple);

				old_cxt = MemoryContextSwitchTo(state->existing->tablecxt);
				entry->tuple = ExecCopySlotTuple(state->slot);
				MemoryContextSwitchTo(old_cxt);
			}
			else
			{
				old_cxt = MemoryContextSwitchTo(state->existing->tablecxt);
				entry->tuple = ExecCopySlotTuple(state->slot);
				MemoryContextSwitchTo(old_cxt);
			}
		}
		else
		{
//			elog(LOG, "asc derp");
//			print_slot(state->slot);
//			sender_insert(sender, state->slot);
		}
	}

//	tuplestore_clear(state->result);
}

static AdhocViewState*
init_adhoc_view(ContinuousViewData data,
				Tuplestorestate *batch,
				DestReceiver *receiver)
{
	AdhocViewState *state = palloc0(sizeof(AdhocViewState));
	PlannedStmt *pstmt = 0;
	TupleDesc desc;

	PushActiveSnapshot(GetTransactionSnapshot());
	pstmt = get_view_plan(data.view);
	PopActiveSnapshot();

	desc = prepare_combine_plan(data.view->matrel, pstmt, batch);
	(void) desc;

	state->batch = batch;
	state->dest = receiver;

	state->query_desc = CreateQueryDesc(pstmt, NULL, InvalidSnapshot,
										InvalidSnapshot, state->dest,
										NULL, 0);

	ExecutorStart(state->query_desc, 0);
	state->query_desc->estate->es_lastoid = InvalidOid;

	(*state->dest->rStartup) (state->dest, 
			state->query_desc->operation, state->query_desc->tupDesc);

	ExecEndNode(state->query_desc->planstate);
	FreeExecutorState(state->query_desc->estate);

	return state;
}

static void
exec_adhoc_view(AdhocViewState* state)
{
	ResourceOwner owner = CurrentResourceOwner;
	struct Plan *plan = 0;
	EState *estate = 0;

	tuplestore_rescan(state->batch);

	state->query_desc->estate = create_estate(state->query_desc);
	estate = state->query_desc->estate;
	set_snapshot(estate, owner);

	plan = state->query_desc->plannedstmt->planTree;

	state->query_desc->planstate = 
		ExecInitNode(plan, state->query_desc->estate, 0);

	ExecutePlan(estate, state->query_desc->planstate, 
				state->query_desc->operation,
				true, 0, 0, ForwardScanDirection, state->dest);

	ExecEndNode(state->query_desc->planstate);
	state->query_desc->planstate = NULL;

	unset_snapshot(estate, owner);

	state->query_desc->estate = NULL;
	estate = state->query_desc->estate;

	tuplestore_clear(state->batch);
}

typedef struct AdaptReceiver
{
	DestReceiver pub;
	struct AdhocSender* sender; 

	bool is_agg;
	AttrNumber *keyColIdx;
	int numCols;

} AdaptReceiver;

static void view_receiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
	AdaptReceiver* receiver = (AdaptReceiver*)(self);
	sender_insert(receiver->sender, slot);
}

static void view_rStartup(DestReceiver *self, int operation,
						  TupleDesc typeinfo)
{
	struct AdaptReceiver *r = (AdaptReceiver*)(self);

//	elog(LOG, "wtf %d", r->numCols);
//sender_startup(struct AdhocSender *sender, TupleDesc tup_desc,
//		AttrNumber *keyColIdx,
//		int	numCols)
//{

//	raise(SIGSTOP);
	sender_startup(r->sender, typeinfo, r->is_agg, r->keyColIdx, r->numCols);
}

static void view_rShutdown(DestReceiver *self)
{

}

static void view_rDestroy(DestReceiver *self)
{

}

static AdaptReceiver*
create_view_receiver()
{
	AdaptReceiver *self = (AdaptReceiver*) palloc0(sizeof(AdaptReceiver));

	self->pub.receiveSlot = view_receiveSlot;
	self->pub.rStartup = view_rStartup;
	self->pub.rShutdown = view_rShutdown;
	self->pub.rDestroy = view_rDestroy;
	self->sender = sender_create();

	return self;
}

static void cleanup_cont_view(ContinuousViewData *data)
{
	DestReceiver* receiver = 0;
	DropStmt *stmt = makeNode(DropStmt);
	List *querytree_list;
	Node *plan;
	Portal portal;

	stmt->objects = list_make1(list_make1(makeString(data->view->name.data)));
	stmt->removeType = OBJECT_CONTINUOUS_VIEW;

	querytree_list = pg_analyze_and_rewrite((Node *) stmt, "DROP",
			NULL, 0);

	// pg_plan_queries
	plan = ((Query*) linitial(querytree_list))->utilityStmt;
	PushActiveSnapshot(GetTransactionSnapshot());

	portal = CreatePortal("__cleanup_cont_view__", true, true);
	portal->visible = false;

	PortalDefineQuery(portal,
			NULL,
			"DROP",
			"DROP",
			list_make1(plan),
			NULL);

	receiver = CreateDestReceiver(DestNone);

	PortalStart(portal, NULL, 0, GetActiveSnapshot());

	(void) PortalRun(portal,
			FETCH_ALL,
			true,
			receiver,
			receiver,
			NULL);

	(*receiver->rDestroy) (receiver);
	PortalDrop(portal, false);
	PopActiveSnapshot();
}

static void
cleanup(int code, Datum arg)
{
	AbortCurrentTransaction();

	StartTransactionCommand();
	cleanup_cont_view((ContinuousViewData*)(arg));
	CommitTransactionCommand();
}

// exec_adhoc_query

void
ExecAdhocQuery(SelectStmt* stmt, const char *s)
{
	int numCols = 1;
	AttrNumber one = 1;
	AttrNumber *keyColIdx = &one;

	AdhocWorkerState *worker_state = 0;
	AdhocCombinerState *combiner_state = 0;
	AdhocViewState *view_state = 0;
	MemoryContext run_cxt = CurrentMemoryContext;

	ResourceOwner owner = 
			ResourceOwnerCreate(CurrentResourceOwner, "WorkerResourceOwner");

	StartTransactionCommand();
	CurrentResourceOwner = owner;
	MemoryContextSwitchTo(run_cxt);
	ContinuousViewData data = init_cont_view(stmt, s);
	CommitTransactionCommand();

	StartTransactionCommand();
	CurrentResourceOwner = owner;
	MemoryContextSwitchTo(run_cxt);
	AdaptReceiver *view_receiver = 0;
	DestReceiver *worker_receiver = CreateDestReceiver(DestTuplestore);

	Tuplestorestate *batch = tuplestore_begin_heap(true, true,
			continuous_query_combiner_work_mem);

	(void) (keyColIdx);
	(void) (exec_adhoc_view);
	(void) (numCols);

	SetTuplestoreDestReceiverParams(worker_receiver, batch,
			CurrentMemoryContext, true);

	MyContQueryProc = get_cont_query_proc();

	worker_state = init_adhoc_worker(data, worker_receiver);
	combiner_state = init_adhoc_combiner(data, batch);

	if (combiner_state->existing)
	{
		keyColIdx = combiner_state->existing->keyColIdx;
		numCols = combiner_state->existing->numCols;
	}

	view_receiver = create_view_receiver();
	view_receiver->is_agg = combiner_state->is_agg;
	view_receiver->keyColIdx = keyColIdx;
	view_receiver->numCols = numCols;

	view_state = init_adhoc_view(data, combiner_state->result,
				 (DestReceiver*) view_receiver);
	view_state->slot = combiner_state->slot;

	CommitTransactionCommand();

	PG_ENSURE_ERROR_CLEANUP(cleanup, PointerGetDatum(&data));
	{
		while (true)
		{
			StartTransactionCommand();
			CurrentResourceOwner = owner;

			// in this case, we want the transaction to cleanup memory

			bool new_rows = exec_adhoc_worker(worker_state);

			if (new_rows)
			{
				exec_adhoc_combiner(combiner_state);
				exec_adhoc_view(view_state);
			}
			else
			{
				sender_heartbeat(view_receiver->sender);
			}

			CommitTransactionCommand();
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(cleanup, PointerGetDatum(&data));
}