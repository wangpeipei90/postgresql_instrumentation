# Removed Vendored Submodules

To keep the repo size reasonable, the two DuckDB source trees (~282MB each,
unmodified upstream) were removed from this vendored copy of pg_mooncake.
Restore them at the pinned commits before building:

| Path | Repo | Pinned commit |
|------|------|---------------|
| `duckdb_mooncake/duckdb` | https://github.com/duckdb/duckdb.git | `b390a7c3760bd95926fe8aefde20d04b349b472e` |
| `pg_duckdb/third_party/duckdb` | https://github.com/duckdb/duckdb.git | `b390a7c3760bd95926fe8aefde20d04b349b472e` |

```bash
git clone https://github.com/duckdb/duckdb.git duckdb_mooncake/duckdb
git -C duckdb_mooncake/duckdb checkout b390a7c3760bd95926fe8aefde20d04b349b472e
cp -R duckdb_mooncake/duckdb pg_duckdb/third_party/duckdb
```

Kept in-tree (small): `moonlink` @ `c1a99ef01af878a5ffa77d1019ef4833a7907410`,
`pg_duckdb` @ `7daa8e53a2d977e6312d937323ed2e7d907d0707`,
`duckdb_mooncake/extension-ci-tools` @ `c098325d7e622b52747a0df810a8146ab10a9ab5`.

Upstream: https://github.com/Mooncake-Labs/pg_mooncake (v0.2.0). Build notes,
including the Rust 1.90.0 requirement, are in the top-level INSTALL_SUMMARY.md.
