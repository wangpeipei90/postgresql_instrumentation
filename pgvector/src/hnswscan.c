#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/relscan.h"
#include "catalog/pg_class.h"
#include "executor/instrument.h"
#include "hnsw.h"
#include "lib/pairingheap.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "storage/lmgr.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

/*
 * Algorithm 5 from paper
 */
static List *
GetScanItems(IndexScanDesc scan, Datum value)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;
	HnswSupport *support = &so->support;
	List	   *ep;
	List	   *w;
	int			m;
	HnswElement entryPoint;
	char	   *base = NULL;
	HnswQuery  *q = &so->q;

	/* Get m and entry point */
	HnswGetMetaPageInfo(index, &m, &entryPoint);

	q->value = value;
	so->m = m;

	if (entryPoint == NULL)
		return NIL;

	ep = list_make1(HnswEntryCandidate(base, entryPoint, q, index, support, false));

	for (int lc = entryPoint->level; lc >= 1; lc--)
	{
		w = HnswSearchLayer(base, q, ep, 1, lc, index, support, m, false, NULL, NULL, NULL, true, NULL, hnsw_trace_enabled ? &so->trace : NULL);
		ep = w;
	}

	return HnswSearchLayer(base, q, ep, hnsw_ef_search, 0, index, support, m, false, NULL, &so->v, hnsw_iterative_scan != HNSW_ITERATIVE_SCAN_OFF ? &so->discarded : NULL, true, &so->tuples, hnsw_trace_enabled ? &so->trace : NULL);
}

/*
 * Resume scan at ground level with discarded candidates
 */
static List *
ResumeScanItems(IndexScanDesc scan)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;
	List	   *ep = NIL;
	char	   *base = NULL;
	int			batch_size = hnsw_ef_search;

	if (pairingheap_is_empty(so->discarded))
		return NIL;

	/* Get next batch of candidates */
	for (int i = 0; i < batch_size; i++)
	{
		HnswSearchCandidate *sc;

		if (pairingheap_is_empty(so->discarded))
			break;

		sc = HnswGetSearchCandidate(w_node, pairingheap_remove_first(so->discarded));

		ep = lappend(ep, sc);
	}

	return HnswSearchLayer(base, &so->q, ep, batch_size, 0, index, &so->support, so->m, false, NULL, &so->v, &so->discarded, false, &so->tuples, hnsw_trace_enabled ? &so->trace : NULL);
}

/*
 * Get scan value
 */
static Datum
GetScanValue(IndexScanDesc scan)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;
	Datum		value;

	if (scan->orderByData->sk_flags & SK_ISNULL)
		value = PointerGetDatum(NULL);
	else
	{
		value = scan->orderByData->sk_argument;

		/* Value should not be compressed or toasted */
		Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
		Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));

		/* Normalize if needed */
		if (so->support.normprocinfo != NULL)
			value = HnswNormValue(so->typeInfo, so->support.collation, value);
	}

	return value;
}

#if defined(HNSW_MEMORY)
/*
 * Show memory usage
 */
static void
ShowMemoryUsage(HnswScanOpaque so)
{
	elog(INFO, "memory: %zu KB, tuples: " INT64_FORMAT, MemoryContextMemAllocated(so->tmpCtx, false) / 1024, so->tuples);
}
#endif

/* Max distinct pages we track individually */
#define HNSW_TRACE_MAX_PAGES 8192

/* Max returned heap TIDs recorded for the topk_ids list */
#define HNSW_TRACE_MAX_TIDS 1000

static int64 hnsw_trace_query_counter = 0;

/*
 * Initialize trace stats for a new scan
 */
static void
HnswTraceInit(HnswScanOpaque so)
{
	HnswTraceStats *t = &so->trace;

	memset(t, 0, sizeof(HnswTraceStats));
	t->last_element_page = InvalidBlockNumber;
	t->last_neighbor_page = InvalidBlockNumber;

	t->element_pages_capacity = HNSW_TRACE_MAX_PAGES;
	t->element_pages_seen = (BlockNumber *) MemoryContextAlloc(so->tmpCtx, sizeof(BlockNumber) * HNSW_TRACE_MAX_PAGES);

	t->neighbor_pages_capacity = HNSW_TRACE_MAX_PAGES;
	t->neighbor_pages_seen = (BlockNumber *) MemoryContextAlloc(so->tmpCtx, sizeof(BlockNumber) * HNSW_TRACE_MAX_PAGES);

	t->returned_tids_capacity = HNSW_TRACE_MAX_TIDS;
	t->returned_tids = (ItemPointerData *) MemoryContextAlloc(so->tmpCtx, sizeof(ItemPointerData) * HNSW_TRACE_MAX_TIDS);

	so->query_id = ++hnsw_trace_query_counter;

	/* Snapshot backend-global buffer counters at scan start */
	t->blks_hit_before = pgBufferUsage.shared_blks_hit;
	t->blks_read_before = pgBufferUsage.shared_blks_read;

	INSTR_TIME_SET_CURRENT(t->scan_start);
}

/*
 * Emit trace stats as JSON via elog(INFO)
 */
static void
HnswTraceEmit(HnswScanOpaque so, int result_count)
{
	HnswTraceStats *t = &so->trace;
	instr_time	now;
	StringInfoData tids;
	int			ntids;

	INSTR_TIME_SET_CURRENT(now);
	INSTR_TIME_SUBTRACT(now, t->scan_start);
	t->latency_ms = INSTR_TIME_GET_MILLISEC(now);

	/* Time spent inside index AM calls vs the executor-side remainder */
	t->hnsw_search_ms = INSTR_TIME_GET_MILLISEC(t->in_index_time);
	t->heap_fetch_ms = t->latency_ms - t->hnsw_search_ms;
	if (t->heap_fetch_ms < 0)
		t->heap_fetch_ms = 0;

	/* Ordered heap TIDs returned by the scan, as ctid strings */
	initStringInfo(&tids);
	appendStringInfoChar(&tids, '[');
	ntids = Min(t->heap_fetch_count, (int64) t->returned_tids_capacity);
	for (int i = 0; i < ntids; i++)
	{
		ItemPointer tid = &t->returned_tids[i];

		if (i > 0)
			appendStringInfoChar(&tids, ',');
		appendStringInfo(&tids, "\"(%u,%u)\"",
						 ItemPointerGetBlockNumber(tid),
						 ItemPointerGetOffsetNumber(tid));
	}
	appendStringInfoChar(&tids, ']');

	/* Snapshot backend-global buffer counters at scan end */
	t->blks_hit_after = pgBufferUsage.shared_blks_hit;
	t->blks_read_after = pgBufferUsage.shared_blks_read;

	/*
	 * idx_blks_* are accumulated exactly inside hnswgettuple. The remainder
	 * of the scan-window delta is heap fetches plus any other plan nodes
	 * that ran between index AM calls.
	 */
	t->heap_blks_hit = (t->blks_hit_after - t->blks_hit_before) - t->idx_blks_hit;
	t->heap_blks_read = (t->blks_read_after - t->blks_read_before) - t->idx_blks_read;

	elog(INFO, "HNSW_TRACE: {"
		 "\"query_id\": " INT64_FORMAT ", "
		 "\"latency_ms\": %.3f, "
		 "\"topk\": %d, "
		 "\"hnsw_search_ms\": %.3f, "
		 "\"heap_fetch_ms\": %.3f, "
		 "\"distance_compute_count\": " INT64_FORMAT ", "
		 "\"visited_nodes\": " INT64_FORMAT ", "
		 "\"heap_fetch_count\": " INT64_FORMAT ", "
		 "\"index_element_loads\": " INT64_FORMAT ", "
		 "\"index_element_page_runs\": " INT64_FORMAT ", "
		 "\"index_element_distinct_pages\": " INT64_FORMAT ", "
		 "\"index_neighbor_loads\": " INT64_FORMAT ", "
		 "\"index_neighbor_page_runs\": " INT64_FORMAT ", "
		 "\"index_neighbor_distinct_pages\": " INT64_FORMAT ", "
		 "\"blks_hit_before\": " INT64_FORMAT ", "
		 "\"blks_hit_after\": " INT64_FORMAT ", "
		 "\"blks_read_before\": " INT64_FORMAT ", "
		 "\"blks_read_after\": " INT64_FORMAT ", "
		 "\"idx_blks_hit\": " INT64_FORMAT ", "
		 "\"idx_blks_read\": " INT64_FORMAT ", "
		 "\"heap_blks_hit\": " INT64_FORMAT ", "
		 "\"heap_blks_read\": " INT64_FORMAT ", "
		 "\"topk_ids\": %s"
		 "}",
		 so->query_id,
		 t->latency_ms,
		 result_count,
		 t->hnsw_search_ms,
		 t->heap_fetch_ms,
		 t->distance_compute_count,
		 t->visited_nodes,
		 t->heap_fetch_count,
		 t->index_element_loads,
		 t->index_element_page_runs,
		 t->index_element_distinct_pages,
		 t->index_neighbor_loads,
		 t->index_neighbor_page_runs,
		 t->index_neighbor_distinct_pages,
		 t->blks_hit_before,
		 t->blks_hit_after,
		 t->blks_read_before,
		 t->blks_read_after,
		 t->idx_blks_hit,
		 t->idx_blks_read,
		 t->heap_blks_hit,
		 t->heap_blks_read,
		 tids.data);

	pfree(tids.data);
}

/*
 * Prepare for an index scan
 */
IndexScanDesc
hnswbeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	HnswScanOpaque so;
	double		maxMemory;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	so = (HnswScanOpaque) palloc(sizeof(HnswScanOpaqueData));
	so->typeInfo = HnswGetTypeInfo(index);

	/* Set support functions */
	HnswInitSupport(&so->support, index);

	/*
	 * Use a lower max allocation size than default to allow scanning more
	 * tuples for iterative search before exceeding work_mem
	 */
	so->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Hnsw scan temporary context",
									   0, 8 * 1024, 256 * 1024);

	/* Calculate max memory */
	/* Add 256 extra bytes to fill last block when close */
	maxMemory = (double) work_mem * hnsw_scan_mem_multiplier * 1024.0 + 256;
	so->maxMemory = Min(maxMemory, (double) (SIZE_MAX / 2));

	scan->opaque = so;

	return scan;
}

/*
 * Start or restart an index scan
 */
void
hnswrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;

	so->first = true;
	/* v and discarded are allocated in tmpCtx */
	so->v.tids = NULL;
	so->discarded = NULL;
	so->tuples = 0;
	so->previousDistance = -get_float8_infinity();
	MemoryContextReset(so->tmpCtx);

	if (hnsw_trace_enabled)
		HnswTraceInit(so);

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));

	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
}

/*
 * Fetch the next tuple in the given scan
 */
bool
hnswgettuple(IndexScanDesc scan, ScanDirection dir)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;
	MemoryContext oldCtx = MemoryContextSwitchTo(so->tmpCtx);
	int64		entryBlksHit = 0;
	int64		entryBlksRead = 0;
	instr_time	entryTime;

	INSTR_TIME_SET_ZERO(entryTime);

	/*
	 * All buffer access inside the index AM is index pages, so the
	 * per-call pgBufferUsage delta gives exact idx_blks_* counts. The
	 * per-call time delta sums to hnsw_search_ms.
	 */
	if (hnsw_trace_enabled)
	{
		entryBlksHit = pgBufferUsage.shared_blks_hit;
		entryBlksRead = pgBufferUsage.shared_blks_read;
		INSTR_TIME_SET_CURRENT(entryTime);
	}

	/*
	 * Index can be used to scan backward, but Postgres doesn't support
	 * backward scan on operators
	 */
	Assert(ScanDirectionIsForward(dir));

	if (so->first)
	{
		Datum		value;

		/* Count index scan for stats */
		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		/* Safety check */
		if (scan->orderByData == NULL)
			elog(ERROR, "cannot scan hnsw index without order");

		/* Requires MVCC-compliant snapshot as not able to maintain a pin */
		/* https://www.postgresql.org/docs/current/index-locking.html */
		if (!IsMVCCSnapshot(scan->xs_snapshot))
			elog(ERROR, "non-MVCC snapshots are not supported with hnsw");

		/* Get scan value */
		value = GetScanValue(scan);

		/*
		 * Get a shared lock. This allows vacuum to ensure no in-flight scans
		 * before marking tuples as deleted.
		 */
		LockPage(scan->indexRelation, HNSW_SCAN_LOCK, ShareLock);

		so->w = GetScanItems(scan, value);

		/* Release shared lock */
		UnlockPage(scan->indexRelation, HNSW_SCAN_LOCK, ShareLock);

		so->first = false;

#if defined(HNSW_MEMORY)
		ShowMemoryUsage(so);
#endif
	}

	for (;;)
	{
		char	   *base = NULL;
		HnswSearchCandidate *sc;
		HnswElement element;
		ItemPointer heaptid;

		if (list_length(so->w) == 0)
		{
			if (hnsw_iterative_scan == HNSW_ITERATIVE_SCAN_OFF)
				break;

			/* Empty index */
			if (so->discarded == NULL)
				break;

			/* Reached max number of tuples or memory limit */
			if (so->tuples >= hnsw_max_scan_tuples || MemoryContextMemAllocated(so->tmpCtx, false) > so->maxMemory)
			{
				if (pairingheap_is_empty(so->discarded))
					break;

				/* Return remaining tuples */
				so->w = lappend(so->w, HnswGetSearchCandidate(w_node, pairingheap_remove_first(so->discarded)));
			}
			else
			{
				/*
				 * Locking ensures when neighbors are read, the elements they
				 * reference will not be deleted (and replaced) during the
				 * iteration.
				 *
				 * Elements loaded into memory on previous iterations may have
				 * been deleted (and replaced), so when reading neighbors, the
				 * element version must be checked.
				 */
				LockPage(scan->indexRelation, HNSW_SCAN_LOCK, ShareLock);

				so->w = ResumeScanItems(scan);

				UnlockPage(scan->indexRelation, HNSW_SCAN_LOCK, ShareLock);

#if defined(HNSW_MEMORY)
				ShowMemoryUsage(so);
#endif
			}

			if (list_length(so->w) == 0)
				break;
		}

		sc = llast(so->w);
		element = HnswPtrAccess(base, sc->element);

		/* Move to next element if no valid heap TIDs */
		if (element->heaptidsLength == 0)
		{
			so->w = list_delete_last(so->w);

			/* Mark memory as free for next iteration */
			if (hnsw_iterative_scan != HNSW_ITERATIVE_SCAN_OFF)
			{
				pfree(element);
				pfree(sc);
			}

			continue;
		}

		heaptid = &element->heaptids[--element->heaptidsLength];

		if (hnsw_iterative_scan == HNSW_ITERATIVE_SCAN_STRICT)
		{
			if (sc->distance < so->previousDistance)
				continue;

			so->previousDistance = sc->distance;
		}

		if (hnsw_trace_enabled)
		{
			instr_time	exitTime;

			if (so->trace.heap_fetch_count < so->trace.returned_tids_capacity)
				so->trace.returned_tids[so->trace.heap_fetch_count] = *heaptid;
			so->trace.heap_fetch_count++;
			so->trace.idx_blks_hit += pgBufferUsage.shared_blks_hit - entryBlksHit;
			so->trace.idx_blks_read += pgBufferUsage.shared_blks_read - entryBlksRead;
			INSTR_TIME_SET_CURRENT(exitTime);
			INSTR_TIME_ACCUM_DIFF(so->trace.in_index_time, exitTime, entryTime);
		}

		MemoryContextSwitchTo(oldCtx);

		scan->xs_heaptid = *heaptid;
		scan->xs_recheck = false;
		scan->xs_recheckorderby = false;
		return true;
	}

	if (hnsw_trace_enabled)
	{
		instr_time	exitTime;

		so->trace.idx_blks_hit += pgBufferUsage.shared_blks_hit - entryBlksHit;
		so->trace.idx_blks_read += pgBufferUsage.shared_blks_read - entryBlksRead;
		INSTR_TIME_SET_CURRENT(exitTime);
		INSTR_TIME_ACCUM_DIFF(so->trace.in_index_time, exitTime, entryTime);
	}

	MemoryContextSwitchTo(oldCtx);
	return false;
}

/*
 * End a scan and release resources
 */
void
hnswendscan(IndexScanDesc scan)
{
	HnswScanOpaque so = (HnswScanOpaque) scan->opaque;

	if (hnsw_trace_enabled && !so->first)
		HnswTraceEmit(so, (int) so->trace.heap_fetch_count);

	MemoryContextDelete(so->tmpCtx);

	pfree(so);
	scan->opaque = NULL;
}
