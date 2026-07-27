# Windows-build patches for dirty submodules

Local UNCOMMITTED changes living in submodule working trees that make the C++ tree
build on Windows/MSVC. Captured 2026-07-27; re-based onto the v1.2 pins during that merge (rocksdb patch was discovered mid-merge — nested sub-submodule missed by the first capture). Re-verified against the v1.3 ton pin 0b0b7bae — all three patches still apply unchanged. If a `git submodule update` ever clobbers the working trees,
re-apply with `git apply` inside each submodule.

| patch | submodule | base commit (see *-base-commit.txt) | content |
|---|---|---|---|
| ton-windows-build.patch | external/ton | 0b0b7bae | CMakeLists: `/bigobj` for MSVC; RocksDbSecondary.cpp: <algorithm> include; BuildSodium/BuildLZ4.cmake: PlatformToolset v143→v145 (this machine = VS18, only v145 installed) |
| clickhouse-cpp-windows-build.patch | external/clickhouse-cpp | 1024efe8 (v2.6.1) | contrib/zstd CMakeLists: 3-line fix |
| rocksdb-windows-build.patch | external/ton/third-party/rocksdb | 4595a5e9 | unique_id_gen.cc: MSVC rdtsc guard x2 (NESTED sub-submodule — easy to miss) |

Re-apply after a pin move (from the submodule dir):

    git apply ../../win-patches/<patch>

If a hunk fails on a new pin, the fix is small enough to re-derive by hand from the
patch content. Keep this dir updated when the working-tree patches change.

## v1.2-pin configure recipe (2026-07-27, post upstream/v1.2 merge)

The v1.2 ton pin builds several third-party deps via msbuild AT CMAKE CONFIGURE TIME
and needs extra sub-submodules. Full local-configure procedure (plain env, then build
under vcvars as usual):

1. `git submodule update --init` inside external/ton for: third-party/{sodium,lz4,
   ngtcp2,openssl,libbacktrace,libmicrohttpd,zlib} (new at this pin).
2. Apply the patches from this dir (ton patch includes the v145 toolset fix).
3. PREBUILD the msbuild deps ONCE under vcvars64 (VS18) so the configure-time
   `if (NOT EXISTS lib)` guards skip — libs land in <build>/third-party/<dep>/lib/:
   - sodium: `msbuild libsodium.vcxproj /p:Configuration=ReleaseLIB /p:Platform=x64 /p:PlatformToolset=v145 /p:OutDir=<build>/third-party/sodium/lib/ /p:IntDir=.../obj/` (from external/ton/third-party/sodium/builds/msvc/vs2022/libsodium)
   - lz4: `msbuild liblz4.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 ...` (from .../lz4/build/VS2022/liblz4)
   - libmicrohttpd: `msbuild libmicrohttpd.vcxproj /p:Configuration=Release-static /p:platform=x64 /p:PlatformToolset=v145 ...` (from .../libmicrohttpd/w32/VS2022)
   - zlib: `msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:Platform=x64 /p:PlatformToolset=v145 ...` (from .../zlib/contrib/vstudio/vc14)
4. Configure with:
   `cmake -B <build> -DUSE_QUIC=OFF -DOPENSSL_CRYPTO_LIBRARY="C:/Program Files/OpenSSL-Win64/lib/VC/x64/MD/libcrypto.lib" -DOPENSSL_SSL_LIBRARY=".../libssl.lib" -DOPENSSL_INCLUDE_DIR="C:/Program Files/OpenSSL-Win64/include"`
   - USE_QUIC=OFF because ngtcp2 needs a QUIC-patched OpenSSL (system one lacks
     SSL_set_quic_tls_cbs) and the bundled-OpenSSL build needs perl+nmake at configure.
     The indexer does not use the QUIC transport.
   - If ninja later demands `third-party/openssl/lib/libssl.lib`: stale QUIC-era cache —
     reconfigure once with `-UOPENSSL_LIBRARIES -UOPENSSL_ROOT_DIR -UOPENSSL_FOUND`.
5. Known API drift at this pin: `td::actor::SchedulerContext::get()` returns a
   REFERENCE now (`.stop()`, not `->stop()`).
