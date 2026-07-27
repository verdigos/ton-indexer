# Windows-build patches for dirty submodules

Local UNCOMMITTED changes living in submodule working trees that make the C++ tree
build on Windows/MSVC. Captured 2026-07-27 before the upstream/v1.2 merge (which moves
both submodule pins). If a `git submodule update` ever clobbers the working trees,
re-apply with `git apply` inside each submodule.

| patch | submodule | base commit (see *-base-commit.txt) | content |
|---|---|---|---|
| ton-windows-build.patch | external/ton | 32e23abf | CMakeLists: `/bigobj` for MSVC; RocksDbSecondary.cpp: 2-line fix; (diff also shows third-party/rocksdb sub-pin moved locally) |
| clickhouse-cpp-windows-build.patch | external/clickhouse-cpp | 418e219e | contrib/zstd CMakeLists: 3-line fix |

Re-apply after a pin move (from the submodule dir):

    git apply ../../win-patches/<patch>

If a hunk fails on a new pin, the fix is small enough to re-derive by hand from the
patch content. Keep this dir updated when the working-tree patches change.
