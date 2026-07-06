# PostgreSQL Instrumentation

Instrumenting PostgreSQL 18.4 vector search: a per-query trace for pgvector's HNSW index scans, exercised against the Amazon Reviews 2023 "All Beauty" dataset (112K products / 701K reviews) with pg_mooncake columnstore analytics for hybrid search.

## What's here

| Path | Contents |
|------|----------|
| `pgvector/` | Vendored pgvector 0.8.3 with the HNSW trace instrumentation applied — builds directly with `make && make install` |
| `patches/` | The same instrumentation as three standalone git patches against upstream pgvector |
| `INSTALL_SUMMARY.md` | Full build/install notes: PostgreSQL from source, pgvector, pg_duckdb, pg_mooncake; trace field reference; hybrid search recipe with real trace outputs |
| `DEMO_PLAN.md` | Step-by-step walkthrough: load the Amazon All Beauty dataset, create mooncake columnstore mirrors, run analytics queries |
| `setup_beauty.sql` | Table creation + data load + mooncake mirror setup |
| `jsonl_to_csv.py` | Converts the raw Amazon JSONL to CSV for `\copy` |
| `generate_embeddings.py` | Generates 384-dim sentence embeddings (all-MiniLM-L6-v2) for products |

## The HNSW trace

`SET hnsw.trace = on;` makes every HNSW index scan emit a JSON line via `elog(INFO)`:

```json
{"query_id": 1, "latency_ms": 4.311, "topk": 5,
 "hnsw_search_ms": 3.950, "heap_fetch_ms": 0.361,
 "distance_compute_count": 172, "visited_nodes": 172, "heap_fetch_count": 5,
 "index_element_loads": 172, "index_element_page_runs": 167, "index_element_distinct_pages": 39,
 "index_neighbor_loads": 44, "index_neighbor_page_runs": 41, "index_neighbor_distinct_pages": 27,
 "blks_hit_before": 1133, "blks_hit_after": 1313, "blks_read_before": 12, "blks_read_after": 54,
 "idx_blks_hit": 179, "idx_blks_read": 39, "heap_blks_hit": 1, "heap_blks_read": 3,
 "topk_ids": ["(2,83)","(2,84)","(4,147)","(5,144)","(4,81)"]}
```

Covered dimensions:

- **Timing split** — end-to-end latency, exact in-index time (`hnsw_search_ms`), executor-side remainder (`heap_fetch_ms`)
- **Graph work** — nodes visited, distance function calls
- **Page access patterns** — element/neighbor tuple loads, page runs (randomness), distinct pages
- **Buffer cache** — exact index block hit/read, derived heap block hit/read, raw before/after counter snapshots
- **Results** — ordered returned heap TIDs (`topk_ids`) for recall@k comparison against an exact-scan baseline

See `INSTALL_SUMMARY.md` for the complete field reference, measurement semantics, and worked examples (including a cold-vs-warm cache pair and filtered hybrid-search traces).

## Applying the patches to stock pgvector

```bash
git clone https://github.com/pgvector/pgvector.git
cd pgvector
git am ../patches/*.patch
PG_CONFIG=/path/to/pg_config make clean && make && make install
```

Note: after modifying `src/hnsw.h`, always `make clean` first — PGXS does not track header dependencies, and a stale partial rebuild will crash the server.

## Hybrid search (pgvector + pg_mooncake)

The repo documents a working pattern for combining mooncake's columnstore (fast analytical filtering over 700K+ rows) with HNSW semantic ranking, including the three planner pitfalls that silently bypass the HNSW index (joined query vectors, `IN (subquery)` filters, and the btree + sort plan). See "Hybrid Search" in `INSTALL_SUMMARY.md`.
