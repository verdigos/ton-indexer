# Windows-build patches for dirty submodules

Local UNCOMMITTED changes living in submodule working trees that make the C++ tree
build on Windows/MSVC. Captured 2026-07-27; re-based onto the v1.2 pins during the merge (rocksdb patch was discovered mid-merge — nested sub-submodule missed by the first capture). If a `git submodule update` ever clobbers the working trees,
re-apply with `git apply` inside each submodule.

| patch | submodule | base commit (see *-base-commit.txt) | content |
|---|---|---|---|
| ton-windows-build.patch | external/ton | 52e367f4 | CMakeLists: `/bigobj` for MSVC; RocksDbSecondary.cpp: <algorithm> include |
| clickhouse-cpp-windows-build.patch | external/clickhouse-cpp | 1024efe8 (v2.6.1) | contrib/zstd CMakeLists: 3-line fix |
| rocksdb-windows-build.patch | external/ton/third-party/rocksdb | 4595a5e9 | unique_id_gen.cc: MSVC rdtsc guard x2 (NESTED sub-submodule — easy to miss) |

Re-apply after a pin move (from the submodule dir):

    git apply ../../win-patches/<patch>

If a hunk fails on a new pin, the fix is small enough to re-derive by hand from the
patch content. Keep this dir updated when the working-tree patches change.
