# PostgreSQL 18.4 Installation Summary

## Overview

PostgreSQL 18.4 built from source on macOS ARM64, with three extensions: pgvector, pg_duckdb, and pg_mooncake.

## Paths

| Component        | Path                                          |
|------------------|-----------------------------------------------|
| Install prefix   | `/Users/peipeiwang/post_db/pgsql/`            |
| Binaries         | `/Users/peipeiwang/post_db/pgsql/bin/`        |
| Libraries        | `/Users/peipeiwang/post_db/pgsql/lib/`        |
| Data directory   | `/Users/peipeiwang/post_db/pgdata/`           |
| Server log       | `/Users/peipeiwang/post_db/pgdata/logfile`    |
| Source code      | `/Users/peipeiwang/post_db/postgresql-18.4/`  |

## PostgreSQL Build Configuration

- **Version**: 18.4
- **Source**: https://ftp.postgresql.org/pub/source/v18.4/postgresql-18.4.tar.bz2
- **Configure flags**: `--with-openssl --with-readline --with-lz4 --with-icu`
- **Homebrew dependencies**: openssl@3, readline, icu4c@78, lz4

## Installed Extensions

### pgvector (0.8.3)
- **Source**: https://github.com/pgvector/pgvector (cloned to `/Users/peipeiwang/post_db/pgvector/`)
- **Purpose**: Vector similarity search — supports IVFFlat and HNSW indexes for embeddings
- **Build**: `PG_CONFIG=.../pg_config make && make install`

### pg_duckdb (1.0.0)
- **Source**: Bundled as a submodule inside pg_mooncake (`/Users/peipeiwang/post_db/pg_mooncake/pg_duckdb/`)
- **Purpose**: Embeds DuckDB inside PostgreSQL for accelerated analytical queries
- **Build**: `make pg_duckdb` from the pg_mooncake Makefile

### pg_mooncake (0.2.0)
- **Source**: https://github.com/Mooncake-Labs/pg_mooncake (cloned to `/Users/peipeiwang/post_db/pg_mooncake/`)
- **Purpose**: Columnstore analytics on Postgres tables via Iceberg format, powered by DuckDB
- **Build**: `cargo pgrx install --release` (required Rust 1.90.0, cargo-pgrx 0.16.1)
- **Dependencies built**: duckdb_mooncake (DuckDB engine), croaring (Homebrew)

## Server Configuration

`postgresql.conf` has been configured with:
```
shared_preload_libraries = 'pg_duckdb,pg_mooncake'
wal_level = logical
duckdb.allow_unsigned_extensions = true
```

Key notes:
- pg_duckdb must be listed before pg_mooncake (pg_mooncake depends on symbols from pg_duckdb)
- `wal_level = logical` is required for pg_mooncake's replication-based columnstore mirrors
- `duckdb.allow_unsigned_extensions = true` is required for the locally-built mooncake DuckDB extension
- Source tables must have a PRIMARY KEY for pg_mooncake to mirror them

## Quick Start

```bash
# Add binaries to PATH
export PATH=/Users/peipeiwang/post_db/pgsql/bin:$PATH

# Start the server (already initialized)
pg_ctl -D /Users/peipeiwang/post_db/pgdata -l /Users/peipeiwang/post_db/pgdata/logfile start

# Check server status
pg_isready

# Connect
psql -d beauty_demo

# Enable extensions in a database
CREATE EXTENSION vector;
CREATE EXTENSION pg_duckdb;
CREATE EXTENSION pg_mooncake;

# Stop the server
pg_ctl -D /Users/peipeiwang/post_db/pgdata stop
```

## HNSW Trace Instrumentation

pgvector has been instrumented with per-query trace output for HNSW index scans (local commits in `/Users/peipeiwang/post_db/pgvector/`: `aa72d37` core trace, `e1408a5` buffer cache metrics). Enable with:

```sql
SET hnsw.trace = on;
```

**Rebuild warning**: PGXS does not track header dependencies. After changing `src/hnsw.h` (e.g. the `HnswTraceStats` struct), a plain `make` only recompiles `.c` files whose own timestamps changed, leaving other objects with the old struct layout — this segfaulted the server once. Always rebuild with:

```bash
PG_CONFIG=/Users/peipeiwang/post_db/pgsql/bin/pg_config make clean
PG_CONFIG=/Users/peipeiwang/post_db/pgsql/bin/pg_config make -j$(sysctl -n hw.ncpu)
PG_CONFIG=/Users/peipeiwang/post_db/pgsql/bin/pg_config make install
# then: pg_ctl -D /Users/peipeiwang/post_db/pgdata restart
```

### Example: Vector Similarity Search

```sql
SELECT id, embedding <-> '[0.5, 0.5, 0.5]'::vector AS distance
FROM vector_test
ORDER BY embedding <-> '[0.5, 0.5, 0.5]'::vector
LIMIT 5;
```

**Trace output — same query on cold cache (right after restart), then warm:**
```json
{"query_id": 1, "latency_ms": 2.943, "topk": 5, "hnsw_search_ms": 2.943,
 "distance_compute_count": 172, "visited_nodes": 172, "heap_fetch_count": 5,
 "index_element_loads": 172, "index_element_page_runs": 167, "index_element_distinct_pages": 39,
 "index_neighbor_loads": 44, "index_neighbor_page_runs": 41, "index_neighbor_distinct_pages": 27,
 "blks_hit_before": 1129, "blks_hit_after": 1309, "blks_read_before": 12, "blks_read_after": 54,
 "idx_blks_hit": 179, "idx_blks_read": 39, "heap_blks_hit": 1, "heap_blks_read": 3}

{"query_id": 2, "latency_ms": 0.352, "topk": 5, "hnsw_search_ms": 0.352,
 "distance_compute_count": 172, "visited_nodes": 172, "heap_fetch_count": 5,
 "index_element_loads": 172, "index_element_page_runs": 167, "index_element_distinct_pages": 39,
 "index_neighbor_loads": 44, "index_neighbor_page_runs": 41, "index_neighbor_distinct_pages": 27,
 "blks_hit_before": 1310, "blks_hit_after": 1532, "blks_read_before": 54, "blks_read_after": 54,
 "idx_blks_hit": 218, "idx_blks_read": 0, "heap_blks_hit": 4, "heap_blks_read": 0}
```

The pair shows cache warming directly: identical traversal (172 nodes, 218 total page accesses in both runs), but the cold run paid 39 storage reads — exactly its 39 distinct element pages — and ran 8x slower (2.9ms vs 0.35ms). `idx_blks_hit + idx_blks_read = 218` in both runs, cross-validating the counters.

**Query results:**
```
 id  |      distance
-----+---------------------
 397 | 0.05276385560569426
 398 | 0.08704193224317819
 775 | 0.09097442062627956
 929 |  0.0984670643839238
 709 | 0.10107437999915193
```

### Trace field reference

| Field | Description |
|-------|-------------|
| `query_id` | Monotonic query counter for paired comparison |
| `latency_ms` | End-to-end query time |
| `topk` | Number of TIDs the scan returned (with a filter above the scan, this is the over-fetch count, not the final LIMIT) |
| `hnsw_search_ms` | Time inside index AM calls only: HNSW traversal + distance computation (per-call `instr_time` deltas, same bracketing as `idx_blks_*`) |
| `heap_fetch_ms` | Derived: `latency_ms - hnsw_search_ms`. Executor-side time between index calls: heap fetches, filter quals, other plan nodes |
| `topk_ids` | Ordered heap TIDs (`ctid` strings) returned by the scan, capped at 1000 — for recall/baseline comparison against an exact scan |
| `distance_compute_count` | Number of distance function calls |
| `visited_nodes` | HNSW graph nodes expanded during search |
| `heap_fetch_count` | Heap tuple fetches (result rows) |
| `index_element_loads` | HNSW element tuple reads (contain vector data + heap TID) |
| `index_element_page_runs` | Element page runs (closer to loads = more random I/O) |
| `index_element_distinct_pages` | Distinct index pages accessed for elements |
| `index_neighbor_loads` | HNSW neighbor tuple reads (contain adjacency lists) |
| `index_neighbor_page_runs` | Neighbor page runs |
| `index_neighbor_distinct_pages` | Distinct index pages accessed for neighbors |
| `blks_hit_before` / `blks_hit_after` | Backend-global `pgBufferUsage.shared_blks_hit` snapshot at scan start / end |
| `blks_read_before` / `blks_read_after` | Backend-global `pgBufferUsage.shared_blks_read` snapshot at scan start / end |
| `idx_blks_hit` | Index blocks found in buffer cache (exact: accumulated inside index AM calls) |
| `idx_blks_read` | Index blocks read from storage (exact) |
| `heap_blks_hit` | Derived: `(after - before) - idx`. Heap fetches plus any other plan nodes that ran between index AM calls |
| `heap_blks_read` | Derived, same caveat |

Buffer-metric semantics: there is no per-relation counter readable in-process, so `idx_blks_*` are measured by snapshotting `pgBufferUsage` on entry/exit of every `hnswgettuple()` call — inside the index AM all buffer access is index pages, making these exact. The `heap_blks_*` remainder is attributed to heap fetches but also absorbs other plan nodes (e.g. a join) executing in the same window. A `read` means "not in shared_buffers"; the OS page cache may still serve it from RAM.

### Save trace to file

```bash
psql -d beauty_demo -c "SET hnsw.trace=on; SELECT ..." 2>&1 \
  | grep 'HNSW_TRACE' | sed 's/^INFO:  HNSW_TRACE: //' > trace_output.json
```

## Hybrid Search: pgvector + pg_mooncake

Hybrid search combines **mooncake columnstore for fast analytical filtering** with **pgvector HNSW for semantic ranking**.

### Schema constraint: keep vectors out of mirrored tables

**moonlink cannot replicate `vector` columns.** Adding an `embedding vector(384)` column to `products` (which has a mooncake mirror) crashed the moonlink background worker and corrupted its metadata store, requiring a wipe of `pgdata/pg_mooncake/` and a rebuild of both mirrors. The working layout keeps embeddings in a separate table that mooncake never sees:

```sql
CREATE TABLE product_embeddings (
    parent_asin TEXT PRIMARY KEY,
    embedding vector(384)
);
CREATE INDEX idx_embeddings_hnsw ON product_embeddings
    USING hnsw (embedding vector_cosine_ops) WITH (m = 16, ef_construction = 64);
```

Embeddings: all-MiniLM-L6-v2 (384-dim, normalized) over title + store + features, generated by `generate_embeddings.py`.

### Making the planner actually use HNSW

Three things silently push the planner off the HNSW index in filtered queries:

1. **The query vector must be a literal**, not a value from a join/subquery. Capture it with `\gset` and inline it as `:'qv'::vector`. With a joined vector the planner falls back to PK lookups + sort. For reproducible benchmarks, **pin the vector to a specific `parent_asin`** — selecting it with `LIKE ... LIMIT 1` (no ORDER BY) is non-deterministic, and a different query vector changes every trace metric (one run visited 3,550 nodes and over-fetched 235 candidates; another vector needed 2,558 and 85).
2. **The filter must be a plain qual, not a join.** `IN (subquery)` becomes a join and HNSW can't be a join inner. `= ANY((SELECT array_agg(...))::text[])` works — but the PK btree will grab it as an index condition unless sorting is disabled.
3. **Force the HNSW path** with `SET hnsw.iterative_scan = relaxed_order;` (over-fetch + post-filter) and `SET enable_sort = off;` (blocks the btree + sort plan).

Note: for these dataset sizes the btree + sort plan the planner prefers is genuinely competitive (~440ms vs ~402ms on Example 1). Forcing HNSW is what makes the trace observable; measure both before choosing.

### Recipe

```sql
SET hnsw.trace = on;
SET hnsw.iterative_scan = relaxed_order;
SET enable_sort = off;

-- Step 1: mooncake columnstore does the analytical filter/aggregation
CREATE TEMP TABLE filtered_asins AS
SELECT parent_asin FROM products_mooncake
WHERE price IS NOT NULL AND price < 30 AND average_rating >= 4.0;

-- Step 2: capture the query vector as a psql variable (becomes a literal).
-- Pin it to a specific parent_asin so every run uses the same vector —
-- LIKE ... LIMIT 1 without ORDER BY picks an arbitrary matching row and
-- makes runs incomparable.
SELECT embedding AS qv FROM product_embeddings
WHERE parent_asin = 'B004K4IYMS'   -- "Ultimate Organic Moisturizer Body Gloss"
\gset

-- Step 3: HNSW iterative scan with the filter as a post-filter qual
SELECT e.parent_asin, p.title, p.price, p.average_rating,
       1 - (e.embedding <=> :'qv'::vector) AS similarity
FROM product_embeddings e
JOIN products p ON p.parent_asin = e.parent_asin
WHERE e.parent_asin = ANY ((SELECT array_agg(parent_asin) FROM filtered_asins)::text[])
ORDER BY e.embedding <=> :'qv'::vector
LIMIT 10;

DROP TABLE filtered_asins;
```

### Pinned vector = reproducible traces

Running the pinned recipe twice in one session (vector `B004K4IYMS`, warm cache):

| Metric | Run 1 | Run 2 |
|---|---|---|
| `distance_compute_count` / `visited_nodes` | 2558 | 2558 |
| `topk` (over-fetch) | 85 | 85 |
| `index_element_loads` / distinct pages | 2558 / 2448 | 2558 / 2448 |
| `idx_blks_read` / `heap_blks_read` | 0 / 0 | 0 / 0 |
| `topk_ids` | identical | identical |
| `latency_ms` | 26.9 | 24.1 |
| `hnsw_search_ms` / `heap_fetch_ms` | 8.4 / 18.5 | 6.3 / 17.8 |

All work metrics and the returned TID list are deterministic; only wall-clock timing jitters. This is the paired-comparison baseline `query_id` exists for — change one variable (ef_search, index params, cache state) and any metric difference is attributable to it. Note also: on a warm index the executor side dominates (`heap_fetch_ms` ≈ 3x `hnsw_search_ms`) — that's the per-row `= ANY(9,690-element array)` filter cost, which cold-cache runs hide under I/O.

### Example 1: "organic moisturizer" + price < $30, rating >= 4.0

Mooncake filters 112,590 products to 9,690 candidates in ~15ms; HNSW ranks in ~449ms (cold cache).

```json
{"query_id": 1, "latency_ms": 448.670, "topk": 235, "hnsw_search_ms": 448.670,
 "distance_compute_count": 3550, "visited_nodes": 3550, "heap_fetch_count": 235,
 "index_element_loads": 3550, "index_element_page_runs": 3550, "index_element_distinct_pages": 3358,
 "index_neighbor_loads": 251, "index_neighbor_page_runs": 247, "index_neighbor_distinct_pages": 243,
 "blks_hit_before": 2430, "blks_hit_after": 3138, "blks_read_before": 2099, "blks_read_after": 5480,
 "idx_blks_hit": 682, "idx_blks_read": 3121, "heap_blks_hit": 26, "heap_blks_read": 260}
```

How the buffer fields relate — the before/after snapshots define the total, and idx/heap decompose it exactly:

```
total hits  = blks_hit_after  − blks_hit_before  = 3138 − 2430 = 708  = 682 idx + 26 heap
total reads = blks_read_after − blks_read_before = 5480 − 2099 = 3381 = 3121 idx + 260 heap
```

82% of index page accesses missed the cache (3,121 of 3,803) — this run was I/O-bound, not graph-bound. The `before` values are not part of this query; they are the session's earlier activity (only the deltas describe the scan).

```
 parent_asin |                          title                          | price | average_rating | similarity
-------------+---------------------------------------------------------+-------+----------------+------------
 B08FCV18Q4  | Face Scrub by Disco for Men, Exfoliating and Cleansing, |  7.99 |            4.7 |     0.6889
 B07WGN8F8C  | StBotanica Moroccan Argan Oil Creamy Face Wash - Soothe | 28.44 |              4 |     0.6735
 B0711TG2WP  | One With Nature Eucalyptus Soap (Pack of 2) With Dead S | 16.61 |              5 |     0.6687
```

### Example 2: "hair serum" + stores with >= 50 products

Mooncake aggregates to 16,277 candidate ASINs in ~53ms; HNSW ranks in ~413ms.

```json
{"query_id": 1, "latency_ms": 413.461, "topk": 265, "hnsw_search_ms": 413.461,
 "distance_compute_count": 3889, "visited_nodes": 3889, "heap_fetch_count": 265,
 "index_element_loads": 3889, "index_element_page_runs": 3889, "index_element_distinct_pages": 3641,
 "index_neighbor_loads": 290, "index_neighbor_page_runs": 286, "index_neighbor_distinct_pages": 282,
 "idx_blks_hit": 0, "idx_blks_read": 0, "heap_blks_hit": 0, "heap_blks_read": 0}
```

```
 parent_asin |                          title                          |     store     | average_rating | similarity
-------------+---------------------------------------------------------+---------------+----------------+------------
 B0B7TTVH5G  | Pantene Pro-V Gentle Cleansing with Aloe Vera Extract S | Pantene       |              4 |     0.6347
 B00B4A42TA  | L'Oreal Hair Expertise EverCreme Nourishing Shampoo 8.5 | L'Oréal Paris |              1 |     0.5668
 B010818EFI  | OGX Shampoo with Smooth Hydration Argan Oil and Shea Bu | OGX           |            3.7 |     0.5610
```

### Example 3: "vitamin c serum" + products with >= 20 verified reviews

Mooncake aggregates 701,528 reviews down to 5,377 popular ASINs in ~23ms; HNSW ranks in ~71ms warm / ~145ms cold. Cold-cache trace (with buffer metrics):

```json
{"query_id": 1, "latency_ms": 144.940, "topk": 97, "hnsw_search_ms": 144.940,
 "distance_compute_count": 1147, "visited_nodes": 1147, "heap_fetch_count": 97,
 "index_element_loads": 1147, "index_element_page_runs": 1147, "index_element_distinct_pages": 1116,
 "index_neighbor_loads": 137, "index_neighbor_page_runs": 133, "index_neighbor_distinct_pages": 132,
 "blks_hit_before": 2091, "blks_hit_after": 2287, "blks_read_before": 4502, "blks_read_after": 5746,
 "idx_blks_hit": 169, "idx_blks_read": 1117, "heap_blks_hit": 27, "heap_blks_read": 127}
```

`idx_blks_read` (1,117) ≈ `index_element_distinct_pages` (1,116): on a cold cache, virtually every distinct element page in the 384-dim index is a storage read — the random-access cost the page-run metrics predict. (Example 2 above was traced before the buffer metrics were wired up, hence its zeros.)

```
 parent_asin |                          title                          |             store              | average_rating | similarity
-------------+---------------------------------------------------------+--------------------------------+----------------+------------
 B016MNB4HQ  | Vitamin C Serum for Face - C Booster With Hyaluronic Ac |                                |            3.5 |     0.7727
 B002OSEMYG  | Hyaluronic Acid Serum with Vitamin C : 100 % pure 1 oz  | Natureful                      |            3.4 |     0.7599
 B00OS9YWJY  | Pure Hyaluronic Acid Serum with Vitamin C for Face - Or | Derma-nu Miracle Skin Remedies |              4 |     0.7559
```

### Reading the traces

- **`heap_fetch_count` is the over-fetch cost of filtered vector search**: the iterative HNSW scan returned 235 / 265 / 69 candidates before 10 survived the filter. Example 3 has the most selective filter (4.8% of products) yet ran fastest — its query neighborhood is dense with qualifying products.
- **Access is fully random**: `index_element_page_runs == index_element_loads` in all three runs — every consecutive element load hit a different page (3,300–3,600 distinct pages on the large scans). At 384 dims only ~2 element tuples fit per 8KB page, so graph traversal costs roughly one page read per visited node.
- **~14 distance computations per returned candidate** (e.g. 3550/235) — the graph-walking overhead behind each result.

## Rust Toolchain (for pg_mooncake development)

- Rust 1.90.0 installed via rustup (project specifies 1.88.0 in `rust-toolchain.toml` but dependency `roaring@0.11.4` requires 1.90.0)
- cargo-pgrx 0.16.1 installed
- pgrx initialized with PG 18 at `~/.pgrx/`
