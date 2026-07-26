# code-atlas content schema (v1)

Atlas = one renderer (`gen.mjs`, zero-dep Node) + per-project content dirs.
Code is NEVER stored in content JSON — the renderer pulls it from the repo at
render time by `file` + `lines`, so excerpts are always the real current code.
Authors pin a `sha8` per excerpt (hash of the span at authoring time); if the
code later changes, the page renders a DRIFT warning on that excerpt instead of
silently showing re-annotated-looking stale notes.

## Layout

```
code-atlas/
  gen.mjs                     renderer + span-hash utility
  SCHEMA.md                   this file
  projects/<proj>/atlas.json  project manifest
  projects/<proj>/scopes/<id>.json
  out/<proj>/*.html           rendered (index.html + one page per scope)
```

## atlas.json

```json
{
  "project": "mch",
  "title": "mch C++ engine — code atlas",
  "repo_root": "S:/Dev/ton-indexer-clear",
  "commit": "ff4135bc",
  "arch_diagrams": [ { "title": "...", "mermaid": "flowchart TD; ..." } ],
  "intro": "markdown-ish plain text shown on index",
  "scopes": [ { "id": "parse-layer", "title": "Parse layer", "status": "done|planned" } ]
}
```

`scopes` order = recommended reading order. `status:"planned"` renders a stub
card (workflow fills them later).

## scopes/<id>.json

```json
{
  "id": "parse-layer",
  "title": "Parse layer — three parser sources, two value universes",
  "summary": "3-6 sentence orientation: what this subsystem is, why it exists.",
  "diagrams": [ { "title": "...", "mermaid": "..." } ],
  "sections": [
    {
      "heading": "The registry",
      "prose": "Short narrative BEFORE the code. Plain text; blank line = paragraph. Inline `code` backticks ok. [[scope-id]] renders a crosslink; [[scope-id#anchor]] deep-links a section (anchor = slugified heading).",
      "excerpts": [
        {
          "file": "ton-index-worker/ton-mch-engine/src/MsgParse.cpp",
          "lines": [49, 78],
          "sha8": "a1b2c3d4",
          "lang": "cpp",
          "title": "optional caption",
          "annotations": [
            { "lines": [49, 51], "text": "Meyer's static — built once on first call." },
            { "lines": [61, 66], "text": "Second source: schema-generated rows." }
          ]
        }
      ]
    }
  ],
  "edges": [
    { "to": "ir-loader", "why": "parse.types[] names in the artifact resolve against this registry" }
  ],
  "not_annotated": [
    { "path": "ton-index-worker/ton-mch-engine/src/parse/PVars.h", "reason": "trivial var-bag helper, read directly (~40 LOC)" }
  ]
}
```

Rules:
- `lines` are 1-based inclusive. Keep excerpts ≤ ~45 lines (hard ceiling 60 when
  a semantic unit genuinely can't split — say why in `title`). An excerpt with
  `"continues": true` renders visually joined to the PREVIOUS excerpt (no
  header) — use for one unit that must span two blocks.
- `annotations[].lines` must lie inside the excerpt span. Range gets a tinted
  band + [n] marker; note text renders under the excerpt.
- Crosslinks `[[scope-id]]` and `` `backticks` `` are legal in: summary, prose,
  annotation text, edges[].why, not_annotated[].reason. NOT in headings' ids
  (heading text itself may use backticks).
- `sha8`: omit while authoring, then run `node gen.mjs rehash <project>` once —
  it fills/updates every excerpt's sha8 in place. (Manual: `node gen.mjs hash
  <file> <s> <e>`; path resolves against CWD.)
- `not_annotated` is REQUIRED — every in-scope file either appears in an excerpt
  or is listed here with a reason (+ rough LOC). Files excerpted only in part
  get `"partial": true` (rendered as a separate "remainder is yours" group).
  The index aggregates everything into the global coverage table.
- Diagrams: mermaid source; rendered natively when published as a Claude
  artifact (`<pre class="mermaid">`); local out/ copies embed no JS.
- `sections[].runs` (optional, tutorial "run this, see this"): array of
  `{ "cmd": "...", "output": "...", "note": "..." }` — a CAPTURED command +
  output snapshot (authored, not live; rendered with a "captured output"
  label after the section's excerpts). Keep outputs trimmed to the
  demonstrative lines (≤ ~20 lines; elide with "…").

## Rendering

```
node gen.mjs build <project>          # -> out/<project>/*.html (+ drift warnings)
node gen.mjs rehash <project>         # fill/refresh sha8 in all scope JSONs
node gen.mjs hash <file> <s> <e>      # single span sha8 (CWD-relative path)
```
