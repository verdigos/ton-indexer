# ir/ — version-controlled MCH IR contract copies

These JSON files are the cross-language contract consumed by the C++ build
(mch-codegen reads them; no Python is required to build ton-mch-engine).

- `expr_vectors.json` — expression conformance vectors. Codegen input for the
  `--vectors-gen` path (generates src/generated/vectors_generated.cpp).
- `mch_ir.json` — the compiled matcher IR artifact (32 matchers), used by
  `--match`.

Source of truth lives in the indexer frontend:
- expr_vectors.json  <- indexer/tests/mch_ir/vectors/expr_vectors.json
- mch_ir.json        <- `python -m indexer.events.mch.ir_emit <specs> -o ...`

Syncing is MANUAL for now: re-copy when the frontend changes, then regenerate
(the `mch_codegen_vectors` CMake target rebuilds vectors_generated.cpp).
