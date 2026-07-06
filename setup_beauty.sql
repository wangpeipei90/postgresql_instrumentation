-- ============================================================
-- Step 1: Create regular PostgreSQL tables
-- ============================================================

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
    id                SERIAL PRIMARY KEY,
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

-- ============================================================
-- Step 2: Load data from CSV
-- ============================================================

\copy products FROM '/Users/peipeiwang/post_db/products.csv' WITH (FORMAT csv, HEADER true);
\copy reviews(asin, parent_asin, user_id, rating, review_title, review_text, review_timestamp, helpful_vote, verified_purchase) FROM '/Users/peipeiwang/post_db/reviews.csv' WITH (FORMAT csv, HEADER true);

-- ============================================================
-- Step 3: Create indexes
-- ============================================================

CREATE INDEX idx_reviews_parent_asin ON reviews(parent_asin);
CREATE INDEX idx_products_store ON products(store);
CREATE INDEX idx_products_rating ON products(average_rating);

-- ============================================================
-- Step 4: Verify data loaded
-- ============================================================

SELECT 'products' AS table_name, count(*) AS row_count FROM products
UNION ALL
SELECT 'reviews', count(*) FROM reviews;

-- ============================================================
-- Step 5: Enable extensions
-- ============================================================

CREATE EXTENSION IF NOT EXISTS pg_duckdb;
CREATE EXTENSION IF NOT EXISTS pg_mooncake;

-- ============================================================
-- Step 6: Create mooncake columnstore mirrors
-- pg_mooncake mirrors existing tables via logical replication.
-- Prerequisites:
--   postgresql.conf: wal_level = logical
--   postgresql.conf: shared_preload_libraries = 'pg_duckdb,pg_mooncake'
--   postgresql.conf: duckdb.allow_unsigned_extensions = true
--   Each source table must have a PRIMARY KEY.
-- ============================================================

CALL mooncake.create_table('products_mooncake', 'products');
CALL mooncake.create_table('reviews_mooncake', 'reviews');

-- ============================================================
-- Step 7: Verify columnstore tables
-- ============================================================

SELECT * FROM mooncake.list_tables();

SELECT 'products_mooncake' AS table_name, count(*) AS row_count FROM products_mooncake
UNION ALL
SELECT 'reviews_mooncake', count(*) FROM reviews_mooncake;
