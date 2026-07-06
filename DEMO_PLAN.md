# Demo Plan: PostgreSQL + pg_mooncake with Amazon All Beauty Data

## Goal

Learn PostgreSQL basics and pg_mooncake columnstore analytics using the Amazon All Beauty dataset (~112K products, ~701K reviews).

## Data Sources

| Dataset  | File | Rows | Key Fields |
|----------|------|------|------------|
| Metadata | `~/amazon_data/raw/meta_categories/meta_All_Beauty.jsonl` | 112,590 | parent_asin, title, store, price, average_rating, rating_number, features, description |
| Reviews  | `~/amazon_data/raw/review_categories/All_Beauty.jsonl` | 701,528 | asin, parent_asin, user_id, rating, title, text, timestamp, helpful_vote, verified_purchase |
| Parquet  | `~/amazon_data/raw_meta_All_Beauty/full-00000-of-00001.parquet` | 112,590 | Same as metadata (alternative format) |

## Steps

### Step 1: Start PostgreSQL

```bash
export PATH=/Users/peipeiwang/post_db/pgsql/bin:$PATH
pg_ctl -D /Users/peipeiwang/post_db/pgdata -l /Users/peipeiwang/post_db/pgdata/logfile start
createdb beauty_demo
```

Server is already initialized and configured with `shared_preload_libraries = 'pg_duckdb,pg_mooncake'`.

### Step 2: Convert JSONL to CSV

Run the converter script (already created):
```bash
cd /Users/peipeiwang/post_db
python3 jsonl_to_csv.py
```

This produces:
- `products.csv` — normalized product metadata (price cleaned, description/features truncated)
- `reviews.csv` — review data with renamed fields to avoid SQL keyword conflicts

### Step 3: Create Regular PostgreSQL Tables and Load Data

```sql
-- Connect
psql -d beauty_demo

-- Create tables
CREATE TABLE products (
    parent_asin    TEXT PRIMARY KEY,
    title          TEXT,
    main_category  TEXT,
    store          TEXT,
    average_rating DOUBLE PRECISION,
    rating_number  INTEGER,
    price          DOUBLE PRECISION,
    description    TEXT,
    features       TEXT
);

CREATE TABLE reviews (
    asin              TEXT,
    parent_asin       TEXT,
    user_id           TEXT,
    rating            DOUBLE PRECISION,
    review_title      TEXT,
    review_text       TEXT,
    review_timestamp  BIGINT,
    helpful_vote      INTEGER,
    verified_purchase BOOLEAN
);

-- Load data
\copy products FROM '/Users/peipeiwang/post_db/products.csv' WITH (FORMAT csv, HEADER true);
\copy reviews FROM '/Users/peipeiwang/post_db/reviews.csv' WITH (FORMAT csv, HEADER true);

-- Add indexes
CREATE INDEX idx_reviews_parent_asin ON reviews(parent_asin);
CREATE INDEX idx_products_store ON products(store);
CREATE INDEX idx_products_rating ON products(average_rating);

-- Verify
SELECT 'products' AS tbl, count(*) FROM products
UNION ALL
SELECT 'reviews', count(*) FROM reviews;
```

### Step 4: Enable Extensions and Create Mooncake Columnstore Tables

```sql
-- Enable extensions
CREATE EXTENSION pg_duckdb;
CREATE EXTENSION pg_mooncake;

-- Create columnstore copies using the 'mooncake' access method
CREATE TABLE products_mooncake (
    parent_asin    TEXT,
    title          TEXT,
    main_category  TEXT,
    store          TEXT,
    average_rating DOUBLE PRECISION,
    rating_number  INTEGER,
    price          DOUBLE PRECISION,
    description    TEXT,
    features       TEXT
) USING mooncake;

INSERT INTO products_mooncake SELECT * FROM products;

CREATE TABLE reviews_mooncake (
    asin              TEXT,
    parent_asin       TEXT,
    user_id           TEXT,
    rating            DOUBLE PRECISION,
    review_title      TEXT,
    review_text       TEXT,
    review_timestamp  BIGINT,
    helpful_vote      INTEGER,
    verified_purchase BOOLEAN
) USING mooncake;

INSERT INTO reviews_mooncake SELECT * FROM reviews;
```

### Step 5: Example Queries — PostgreSQL Basics

These queries work on both regular tables and `_mooncake` tables.

```sql
-- 1. Basic counts
SELECT count(*) AS total_products,
       count(DISTINCT store) AS unique_stores
FROM products;

-- 2. Top 10 highest-rated products (≥50 reviews)
SELECT title, store, average_rating, rating_number, price
FROM products
WHERE rating_number >= 50
ORDER BY average_rating DESC, rating_number DESC
LIMIT 10;

-- 3. Price distribution
SELECT
    CASE
        WHEN price IS NULL THEN 'Unknown'
        WHEN price < 10    THEN '< $10'
        WHEN price < 25    THEN '$10–$25'
        WHEN price < 50    THEN '$25–$50'
        WHEN price < 100   THEN '$50–$100'
        ELSE '$100+'
    END AS price_bucket,
    count(*) AS products,
    round(avg(price)::numeric, 2) AS avg_price
FROM products
GROUP BY price_bucket
ORDER BY min(price) NULLS LAST;

-- 4. Top 10 stores by product count
SELECT store, count(*) AS num_products,
       round(avg(average_rating)::numeric, 2) AS avg_rating
FROM products
WHERE store != ''
GROUP BY store
ORDER BY num_products DESC
LIMIT 10;

-- 5. Keyword search: 'moisturizer'
SELECT title, store, average_rating, price
FROM products
WHERE lower(title) LIKE '%moisturizer%'
ORDER BY average_rating DESC NULLS LAST
LIMIT 10;

-- 6. Rating vs price correlation
SELECT round(corr(average_rating, price)::numeric, 4) AS rating_price_corr
FROM products
WHERE average_rating IS NOT NULL AND price IS NOT NULL;

-- 7. Join: top reviewed products with review stats
SELECT p.title, p.store, p.average_rating,
       count(r.*) AS review_count,
       round(avg(r.rating)::numeric, 2) AS avg_review_rating
FROM products p
JOIN reviews r ON p.parent_asin = r.parent_asin
GROUP BY p.parent_asin, p.title, p.store, p.average_rating
ORDER BY review_count DESC
LIMIT 10;

-- 8. Verified vs unverified purchase rating comparison
SELECT verified_purchase,
       count(*) AS num_reviews,
       round(avg(rating)::numeric, 2) AS avg_rating
FROM reviews
GROUP BY verified_purchase;
```

### Step 6: Compare Regular PG vs Mooncake Columnstore

```sql
-- Time an aggregation on regular table
\timing on

SELECT store, count(*), round(avg(price)::numeric, 2)
FROM products
WHERE price IS NOT NULL
GROUP BY store
ORDER BY count(*) DESC
LIMIT 10;

-- Same query on columnstore
SELECT store, count(*), round(avg(price)::numeric, 2)
FROM products_mooncake
WHERE price IS NOT NULL
GROUP BY store
ORDER BY count(*) DESC
LIMIT 10;

-- Heavy aggregation on reviews (701K rows)
SELECT date_trunc('month', to_timestamp(review_timestamp / 1000)) AS month,
       count(*) AS reviews,
       round(avg(rating)::numeric, 2) AS avg_rating
FROM reviews
GROUP BY month
ORDER BY month;

-- Same on columnstore
SELECT date_trunc('month', to_timestamp(review_timestamp / 1000)) AS month,
       count(*) AS reviews,
       round(avg(rating)::numeric, 2) AS avg_rating
FROM reviews_mooncake
GROUP BY month
ORDER BY month;

\timing off
```

### Step 7: pg_mooncake-Specific Features

```sql
-- List mooncake-managed tables
SELECT * FROM mooncake.list_tables();

-- Optimize columnstore (compact small files)
CALL mooncake.optimize_table('products_mooncake', 'compact');
```

## Verification Checklist

- [ ] `pg_isready` shows server accepting connections
- [ ] `products` table has ~112K rows
- [ ] `reviews` table has ~701K rows
- [ ] `products_mooncake` and `reviews_mooncake` have matching row counts
- [ ] Analytics queries return identical results on heap vs columnstore tables
- [ ] `\timing` shows performance difference on aggregations
