/*-------------------------------------------------------------------------
 *
 * nodeGatherMerge.c
 *		Scan a plan in multiple workers, and do order-preserving merge.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGatherMerge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/execdebug.h"
#include "executor/execParallel.h"
#include "executor/nodeGatherMerge.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * When we read tuples from workers, it's a good idea to read several at once
 * for efficiency when possible: this minimizes context-switching overhead.
 * But reading too many at a time wastes memory without improving performance.
 * We'll read up to MAX_TUPLE_STORE tuples (in addition to the first one).
 */
#define MAX_TUPLE_STORE 10

/*
 * Pending-tuple array for each worker.  This holds additional tuples that
 * we were able to fetch from the worker, but can't process yet.  In addition,
 * this struct holds the "done" flag indicating the worker is known to have
 * no more tuples.  (We do not use this struct for the leader; we don't keep
 * any pending tuples for the leader, and the need_to_scan_locally flag serves
 * as its "done" indicator.)
 */
typedef struct GMReaderTupleBuffer
{
	HeapTuple  *tuple;			/* array of length MAX_TUPLE_STORE */
	int			nTuples;		/* number of tuples currently stored */
	int			readCounter;	/* index of next tuple to extract */
	bool		done;			/* true if reader is known exhausted */
} GMReaderTupleBuffer;

static TupleTableSlot *ExecGatherMerge(PlanState *pstate);
static int32 heap_compare_slots(Datum a, Datum b, void *arg);
static TupleTableSlot *gather_merge_getnext(GatherMergeState *gm_state);
static HeapTuple gm_readnext_tuple(GatherMergeState *gm_state, int nreader,
				  bool nowait, bool *done);
static void gather_merge_init(GatherMergeState *gm_state);
static void ExecShutdownGatherMergeWorkers(GatherMergeState *node);
static bool gather_merge_readnext(GatherMergeState *gm_state, int reader,
					  bool nowait);
static void load_tuple_array(GatherMergeState *gm_state, int reader);

/* ----------------------------------------------------------------
 *		ExecInitGather
 * ----------------------------------------------------------------
 */
GatherMergeState *
ExecInitGatherMerge(GatherMerge *node, EState *estate, int eflags)
{
	GatherMergeState *gm_state;
	Plan	   *outerNode;
	bool		hasoid;
	TupleDesc	tupDesc;

	/* Gather merge node doesn't have innerPlan node. */
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	gm_state = makeNode(GatherMergeState);
	gm_state->ps.plan = (Plan *) node;
	gm_state->ps.state = estate;
	gm_state->ps.ExecProcNode = ExecGatherMerge;

	gm_state->initialized = false;
	gm_state->gm_initialized = false;
	gm_state->tuples_needed = -1;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &gm_state->ps);

	/*
	 * GatherMerge doesn't support checking a qual (it's always more efficient
	 * to do it in the child node).
	 */
	Assert(!node->plan.qual);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &gm_state->ps);

	/*
	 * now initialize outer plan
	 */
	outerNode = outerPlan(node);
	outerPlanState(gm_state) = ExecInitNode(outerNode, estate, eflags);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&gm_state->ps);
	ExecAssignProjectionInfo(&gm_state->ps, NULL);

	/*
	 * initialize sort-key information
	 */
	if (node->numCols)
	{
		int			i;

		gm_state->gm_nkeys = node->numCols;
		gm_state->gm_sortkeys =
			palloc0(sizeof(SortSupportData) * node->numCols);

		for (i = 0; i < node->numCols; i++)
		{
			SortSupport sortKey = gm_state->gm_sortkeys + i;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = node->collations[i];
			sortKey->ssup_nulls_first = node->nullsFirst[i];
			sortKey->ssup_attno = node->sortColIdx[i];

			/*
			 * We don't perform abbreviated key conversion here, for the same
			 * reasons that it isn't used in MergeAppend
			 */
			sortKey->abbreviate = false;

			PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
		}
	}

	/*
	 * store the tuple descriptor into gather merge state, so we can use it
	 * later while initializing the gather merge slots.
	 */
	if (!ExecContextForcesOids(&gm_state->ps, &hasoid))
		hasoid = false;
	tupDesc = ExecTypeFromTL(outerNode->targetlist, hasoid);
	gm_state->tupDesc = tupDesc;

	return gm_state;
}

/* ----------------------------------------------------------------
 *		ExecGatherMerge(node)
 *
 *		Scans the relation via multiple workers and returns
 *		the next qualifying tuple.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGatherMerge(PlanState *pstate)
{
	GatherMergeState *node = castNode(GatherMergeState, pstate);
	TupleTableSlot *slot;
	ExprContext *econtext;
	int			i;

	CHECK_FOR_INTERRUPTS();

	/*
	 * As with Gather, we don't launch workers until this node is actually
	 * executed.
	 */
	if (!node->initialized)
	{
		EState	   *estate = node->ps.state;
		GatherMerge *gm = castNode(GatherMerge, node->ps.plan);

		/*
		 * Sometimes we might have to run without parallelism; but if parallel
		 * mode is active then we can try to fire up some workers.
		 */
		if (gm->num_workers > 0 && IsInParallelMode())
		{
			ParallelContext *pcxt;

			/* Initialize, or re-initialize, shared state needed by workers. */
			if (!node->pei)
				node->pei = ExecInitParallelPlan(node->ps.lefttree,
												 estate,
												 gm->num_workers,
												 node->tuples_needed);
			else
				ExecParallelReinitialize(node->ps.lefttree,
										 node->pei);

			/* Try to launch workers. */
			pcxt = node->pei->pcxt;
			LaunchParallelWorkers(pcxt);
			/* We save # workers launched for the benefit of EXPLAIN */
			node->nworkers_launched = pcxt->nworkers_launched;
			node->nreaders = 0;

			/* Set up tuple queue readers to read the results. */
			if (pcxt->nworkers_launched > 0)
			{
				node->reader = palloc(pcxt->nworkers_launched *
									  sizeof(TupleQueueReader *));

				for (i = 0; i < pcxt->nworkers_launched; ++i)
				{
					shm_mq_set_handle(node->pei->tqueue[i],
									  pcxt->worker[i].bgwhandle);
					node->reader[node->nreaders++] =
						CreateTupleQueueReader(node->pei->tqueue[i],
											   node->tupDesc);
				}
			}
			else
			{
				/* No workers?	Then never mind. */
				ExecShutdownGatherMergeWorkers(node);
			}
		}

		/* always allow leader to participate */
		node->need_to_scan_locally = true;
		node->initialized = true;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/*
	 * Get next tuple, either from one of our workers, or by running the plan
	 * ourselves.
	 */
	slot = gather_merge_getnext(node);
	if (TupIsNull(slot))
		return NULL;

	/*
	 * Form the result tuple using ExecProject(), and return it.
	 */
	econtext->ecxt_outertuple = slot;
	return ExecProject(node->ps.ps_ProjInfo);
}

/* ----------------------------------------------------------------
 *		ExecEndGatherMerge
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndGatherMerge(GatherMergeState *node)
{
	ExecEndNode(outerPlanState(node));	/* let children clean up first */
	ExecShutdownGatherMerge(node);
	ExecFreeExprContext(&node->ps);
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherMerge
 *
 *		Destroy the setup for parallel workers including parallel context.
 *		Collect all the stats after workers are stopped, else some work
 *		done by workers won't be accounted.
 * ----------------------------------------------------------------
 */
void
ExecShutdownGatherMerge(GatherMergeState *node)
{
	ExecShutdownGatherMergeWorkers(node);

	/* Now destroy the parallel context. */
	if (node->pei != NULL)
	{
		ExecParallelCleanup(node->pei);
		node->pei = NULL;
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherMergeWorkers
 *
 *		Destroy the parallel workers.  Collect all the stats after
 *		workers are stopped, else some work done by workers won't be
 *		accounted.
 * ----------------------------------------------------------------
 */
static void
ExecShutdownGatherMergeWorkers(GatherMergeState *node)
{
	/* Shut down tuple queue readers before shutting down workers. */
	if (node->reader != NULL)
	{
		int			i;

		for (i = 0; i < node->nreaders; ++i)
			if (node->reader[i])
				DestroyTupleQueueReader(node->reader[i]);

		pfree(node->reader);
		node->reader = NULL;
	}

	/* Now shut down the workers. */
	if (node->pei != NULL)
		ExecParallelFinish(node->pei);
}

/* ----------------------------------------------------------------
 *		ExecReScanGatherMerge
 *
 *		Prepare to re-scan the result of a GatherMerge.
 * ----------------------------------------------------------------
 */
void
ExecReScanGatherMerge(GatherMergeState *node)
{
	GatherMerge *gm = (GatherMerge *) node->ps.plan;
	PlanState  *outerPlan = outerPlanState(node);

	/* Make sure any existing workers are gracefully shut down */
	ExecShutdownGatherMergeWorkers(node);

	/* Mark node so that shared state will be rebuilt at next call */
	node->initialized = false;
	node->gm_initialized = false;

	/*
	 * Set child node's chgParam to tell it that the next scan might deliver a
	 * different set of rows within the leader process.  (The overall rowset
	 * shouldn't change, but the leader process's subset might; hence nodes
	 * between here and the parallel table scan node mustn't optimize on the
	 * assumption of an unchanging rowset.)
	 */
	if (gm->rescan_param >= 0)
		outerPlan->chgParam = bms_add_member(outerPlan->chgParam,
											 gm->rescan_param);

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Note: because this does nothing if we have a
	 * rescan_param, it's currently guaranteed that parallel-aware child nodes
	 * will not see a ReScan call until after they get a ReInitializeDSM call.
	 * That ordering might not be something to rely on, though.  A good rule
	 * of thumb is that ReInitializeDSM should reset only shared state, ReScan
	 * should reset only local state, and anything that depends on both of
	 * those steps being finished must wait until the first ExecProcNode call.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/*
 * Initialize the Gather merge tuple read.
 *
 * Pull at least a single tuple from each worker + leader and set up the heap.
 */
static void
gather_merge_init(GatherMergeState *gm_state)
{
	int			nreaders = gm_state->nreaders;
	bool		nowait = true;
	int			i;

	/*
	 * Allocate gm_slots for the number of workers + one more slot for leader.
	 * Last slot is always for leader. Leader always calls ExecProcNode() to
	 * read the tuple which will return the TupleTableSlot. Later it will
	 * directly get assigned to gm_slot. So just initialize leader gm_slot
	 * with NULL. For other slots, code below will call
	 * ExecInitExtraTupleSlot() to create a slot for the worker's results.
	 */
	gm_state->gm_slots =
		palloc((gm_state->nreaders + 1) * sizeof(TupleTableSlot *));
	gm_state->gm_slots[gm_state->nreaders] = NULL;

	/* Initialize the tuple slot and tuple array for each worker */
	gm_state->gm_tuple_buffers =
		(GMReaderTupleBuffer *) palloc0(sizeof(GMReaderTupleBuffer) *
										gm_state->nreaders);
	for (i = 0; i < gm_state->nreaders; i++)
	{
		/* Allocate the tuple array with length MAX_TUPLE_STORE */
		gm_state->gm_tuple_buffers[i].tuple =
			(HeapTuple *) palloc0(sizeof(HeapTuple) * MAX_TUPLE_STORE);

		/* Initialize slot for worker */
		gm_state->gm_slots[i] = ExecInitExtraTupleSlot(gm_state->ps.state);
		ExecSetSlotDescriptor(gm_state->gm_slots[i],
							  gm_state->tupDesc);
	}

	/* Allocate the resources for the merge */
	gm_state->gm_heap = binaryheap_allocate(gm_state->nreaders + 1,
											heap_compare_slots,
											gm_state);

	/*
	 * First, try to read a tuple from each worker (including leader) in
	 * nowait mode.  After this, if not all workers were able to produce a
	 * tuple (or a "done" indication), then re-read from remaining workers,
	 * this time using wait mode.  Add all live readers (those producing at
	 * least one tuple) to the heap.
	 */
reread:
	for (i = 0; i < nreaders + 1; i++)
	{
		CHECK_FOR_INTERRUPTS();

		/* ignore this source if already known done */
		if ((i < nreaders) ?
			!gm_state->gm_tuple_buffers[i].done :
			gm_state->need_to_scan_locally)
		{
			if (TupIsNull(gm_state->gm_slots[i]))
			{
				/* Don't have a tuple yet, try to get one */
				if (gather_merge_readnext(gm_state, i, nowait))
					binaryheap_add_unordered(gm_state->gm_heap,
											 Int32GetDatum(i));
			}
			else
			{
				/*
				 * We already got at least one tuple from this worker, but
				 * might as well see if it has any more ready by now.
				 */
				load_tuple_array(gm_state, i);
			}
		}
	}

	/* need not recheck leader, since nowait doesn't matter for it */
	for (i = 0; i < nreaders; i++)
	{
		if (!gm_state->gm_tuple_buffers[i].done &&
			TupIsNull(gm_state->gm_slots[i]))
		{
			nowait = false;
			goto reread;
		}
	}

	/* Now heapify the heap. */
	binaryheap_build(gm_state->gm_heap);

	gm_state->gm_initialized = true;
}

/*
 * Clear out the tuple table slots for each gather merge input.
 */
static void
gather_merge_clear_slots(GatherMergeState *gm_state)
{
	int			i;

	for (i = 0; i < gm_state->nreaders; i++)
	{
		pfree(gm_state->gm_tuple_buffers[i].tuple);
		ExecClearTuple(gm_state->gm_slots[i]);
	}

	/* Free tuple array as we don't need it any more */
	pfree(gm_state->gm_tuple_buffers);
	/* Free the binaryheap, which was created for sort */
	binaryheap_free(gm_state->gm_heap);
}

/*
 * Read the next tuple for gather merge.
 *
 * Fetch the sorted tuple out of the heap.
 */
static TupleTableSlot *
gather_merge_getnext(GatherMergeState *gm_state)
{
	int			i;

	if (!gm_state->gm_initialized)
	{
		/*
		 * First time through: pull the first tuple from each participant, and
		 * set up the heap.
		 */
		gather_merge_init(gm_state);
	}
	else
	{
		/*
		 * Otherwise, pull the next tuple from whichever participant we
		 * returned from last time, and reinsert that participant's index into
		 * the heap, because it might now compare differently against the
		 * other elements of the heap.
		 */
		i = DatumGetInt32(binaryheap_first(gm_state->gm_heap));

		if (gather_merge_readnext(gm_state, i, false))
			binaryheap_replace_first(gm_state->gm_heap, Int32GetDatum(i));
		else
		{
			/* reader exhausted, remove it from heap */
			(void) binaryheap_remove_first(gm_state->gm_heap);
		}
	}

	if (binaryheap_empty(gm_state->gm_heap))
	{
		/* All the queues are exhausted, and so is the heap */
		gather_merge_clear_slots(gm_state);
		return NULL;
	}
	else
	{
		/* Return next tuple from whichever participant has the leading one */
		i = DatumGetInt32(binaryheap_first(gm_state->gm_heap));
		return gm_state->gm_slots[i];
	}
}

/*
 * Read tuple(s) for given reader in nowait mode, and load into its tuple
 * array, until we have MAX_TUPLE_STORE of them or would have to block.
 */
static void
load_tuple_array(GatherMergeState *gm_state, int reader)
{
	GMReaderTupleBuffer *tuple_buffer;
	int			i;

	/* Don't do anything if this is the leader. */
	if (reader == gm_state->nreaders)
		return;

	tuple_buffer = &gm_state->gm_tuple_buffers[reader];

	/* If there's nothing in the array, reset the counters to zero. */
	if (tuple_buffer->nTuples == tuple_buffer->readCounter)
		tuple_buffer->nTuples = tuple_buffer->readCounter = 0;

	/* Try to fill additional slots in the array. */
	for (i = tuple_buffer->nTuples; i < MAX_TUPLE_STORE; i++)
	{
		HeapTuple	tuple;

		tuple = gm_readnext_tuple(gm_state,
								  reader,
								  true,
								  &tuple_buffer->done);
		if (!HeapTupleIsValid(tuple))
			break;
		tuple_buffer->tuple[i] = heap_copytuple(tuple);
		tuple_buffer->nTuples++;
	}
}

/*
 * Store the next tuple for a given reader into the appropriate slot.
 *
 * Returns true if successful, false if not (either reader is exhausted,
 * or we didn't want to wait for a tuple).  Sets done flag if reader
 * is found to be exhausted.
 */
static bool
gather_merge_readnext(GatherMergeState *gm_state, int reader, bool nowait)
{
	GMReaderTupleBuffer *tuple_buffer;
	HeapTuple	tup;

	/*
	 * If we're being asked to generate a tuple from the leader, then we just
	 * call ExecProcNode as normal to produce one.
	 */
	if (gm_state->nreaders == reader)
	{
		if (gm_state->need_to_scan_locally)
		{
			PlanState  *outerPlan = outerPlanState(gm_state);
			TupleTableSlot *outerTupleSlot;

			outerTupleSlot = ExecProcNode(outerPlan);

			if (!TupIsNull(outerTupleSlot))
			{
				gm_state->gm_slots[reader] = outerTupleSlot;
				return true;
			}
			/* need_to_scan_locally serves as "done" flag for leader */
			gm_state->need_to_scan_locally = false;
		}
		return false;
	}

	/* Otherwise, check the state of the relevant tuple buffer. */
	tuple_buffer = &gm_state->gm_tuple_buffers[reader];

	if (tuple_buffer->nTuples > tuple_buffer->readCounter)
	{
		/* Return any tuple previously read that is still buffered. */
		tup = tuple_buffer->tuple[tuple_buffer->readCounter++];
	}
	else if (tuple_buffer->done)
	{
		/* Reader is known to be exhausted. */
		DestroyTupleQueueReader(gm_state->reader[reader]);
		gm_state->reader[reader] = NULL;
		return false;
	}
	else
	{
		/* Read and buffer next tuple. */
		tup = gm_readnext_tuple(gm_state,
								reader,
								nowait,
								&tuple_buffer->done);
		if (!HeapTupleIsValid(tup))
			return false;
		tup = heap_copytuple(tup);

		/*
		 * Attempt to read more tuples in nowait mode and store them in the
		 * pending-tuple array for the reader.
		 */
		load_tuple_array(gm_state, reader);
	}

	Assert(HeapTupleIsValid(tup));

	/* Build the TupleTableSlot for the given tuple */
	ExecStoreTuple(tup,			/* tuple to store */
				   gm_state->gm_slots[reader],	/* slot in which to store the
												 * tuple */
				   InvalidBuffer,	/* buffer associated with this tuple */
				   true);		/* pfree this pointer if not from heap */

	return true;
}

/*
 * Attempt to read a tuple from given reader.
 */
static HeapTuple
gm_readnext_tuple(GatherMergeState *gm_state, int nreader, bool nowait,
				  bool *done)
{
	TupleQueueReader *reader;
	HeapTuple	tup;
	MemoryContext oldContext;
	MemoryContext tupleContext;

	/* Check for async events, particularly messages from workers. */
	CHECK_FOR_INTERRUPTS();

	/* Attempt to read a tuple. */
	reader = gm_state->reader[nreader];

	/* Run TupleQueueReaders in per-tuple context */
	tupleContext = gm_state->ps.ps_ExprContext->ecxt_per_tuple_memory;
	oldContext = MemoryContextSwitchTo(tupleContext);
	tup = TupleQueueReaderNext(reader, nowait, done);
	MemoryContextSwitchTo(oldContext);

	return tup;
}

/*
 * We have one slot for each item in the heap array.  We use SlotNumber
 * to store slot indexes.  This doesn't actually provide any formal
 * type-safety, but it makes the code more self-documenting.
 */
typedef int32 SlotNumber;

/*
 * Compare the tuples in the two given slots.
 */
static int32
heap_compare_slots(Datum a, Datum b, void *arg)
{
	GatherMergeState *node = (GatherMergeState *) arg;
	SlotNumber	slot1 = DatumGetInt32(a);
	SlotNumber	slot2 = DatumGetInt32(b);

	TupleTableSlot *s1 = node->gm_slots[slot1];
	TupleTableSlot *s2 = node->gm_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->gm_nkeys; nkey++)
	{
		SortSupport sortKey = node->gm_sortkeys + nkey;
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		int			compare;

		datum1 = slot_getattr(s1, attno, &isNull1);
		datum2 = slot_getattr(s2, attno, &isNull2);

		compare = ApplySortComparator(datum1, isNull1,
									  datum2, isNull2,
									  sortKey);
		if (compare != 0)
			return -compare;
	}
	return 0;
}
