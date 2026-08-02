#!/usr/bin/env node
// code-atlas renderer. Zero dependencies. See SCHEMA.md.
//   node gen.mjs build <project> | rehash <project> | hash <file> <s> <e>
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname, resolve, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = dirname(fileURLToPath(import.meta.url));
const NUL = String.fromCharCode(0);

function sha8(text) {
  return createHash('sha256').update(text, 'utf8').digest('hex').slice(0, 8);
}
function spanText(lines, s, e) {
  return lines.slice(s - 1, e).join('\n');
}
function esc(t) {
  return t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
function slug(t) {
  return t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
}

// --- minimal syntax highlight (escape first, then wrap) ---------------------
const KW = {
  cpp: /\b(auto|bool|break|case|catch|class|const|constexpr|continue|default|delete|do|double|else|enum|explicit|extern|false|float|for|friend|if|inline|int|long|mutable|namespace|new|noexcept|nullptr|operator|private|public|return|short|signed|sizeof|static|struct|switch|template|this|throw|true|try|typedef|typename|union|unsigned|using|virtual|void|while)\b/g,
  py: /\b(and|as|assert|async|await|break|class|continue|def|del|elif|else|except|False|finally|for|from|global|if|import|in|is|lambda|None|not|or|pass|raise|return|True|try|while|with|yield)\b/g,
  js: /\b(async|await|break|case|catch|class|const|continue|default|delete|do|else|export|extends|false|finally|for|from|function|if|import|in|instanceof|let|new|null|of|return|static|switch|this|throw|true|try|typeof|undefined|var|void|while|yield)\b/g,
};
function hl(code, lang) {
  let h = esc(code);
  // strings + comments first via placeholders so keywords don't fire inside;
  // placeholder = NUL t <idx> NUL — the 't' keeps the digit pass (\b\d+\b) off it.
  const toks = [];
  const put = (cls, m) => { toks.push(`<span class="tk-${cls}">${m}</span>`); return NUL + 't' + (toks.length - 1) + NUL; };
  if (lang === 'py') {
    h = h.replace(/(#[^\n]*)/g, (_, m) => put('c', m));
    h = h.replace(/(&quot;{3}[\s\S]*?&quot;{3}|&#39;{3}[\s\S]*?&#39;{3})/g, (_, m) => put('s', m));
  } else {
    h = h.replace(/(\/\/[^\n]*)/g, (_, m) => put('c', m));
    h = h.replace(/(\/\*[\s\S]*?\*\/)/g, (_, m) => put('c', m));
  }
  h = h.replace(/(&quot;(?:[^&\\]|\\.|&(?!quot;))*?&quot;|&#39;(?:[^&\\]|\\.|&(?!#39;))*?&#39;)/g, (_, m) => put('s', m));
  const kw = KW[lang] || KW.cpp;
  h = h.replace(kw, '<span class="tk-k">$&</span>');
  h = h.replace(/\b(0x[0-9a-fA-F]+|\d+)\b/g, '<span class="tk-n">$1</span>');
  h = h.replace(new RegExp(NUL + 't(\\d+)' + NUL, 'g'), (_, i) => toks[+i]);
  return h;
}

// --- prose: escape, `code`, [[crosslink]], paragraphs ------------------------
function prose(text, scopeIds) {
  if (!text) return '';
  return text.split(/\n\s*\n/).map(p => {
    let h = esc(p.trim());
    h = h.replace(/`([^`]+)`/g, '<code>$1</code>');
    h = h.replace(/\[\[([a-z0-9-]+)(#[a-z0-9-]+)?\]\]/g, (m, id, anchor) =>
      scopeIds.has(id)
        ? `<a class="xl" href="${id}.html${anchor || ''}">${id}${anchor || ''}</a>`
        : `<span class="xl-missing" title="unknown scope">${id}</span>`);
    return `<p>${h}</p>`;
  }).join('\n');
}
function inline(text, scopeIds) {
  return prose(text || '', scopeIds).replace(/^<p>|<\/p>$/g, '').replace(/<\/p>\s*<p>/g, '<br>');
}
function mermaidBlock(d) {
  return `<div class="diagram"><div class="label">${esc(d.title || '')}</div><pre class="mermaid">${esc(d.mermaid)}</pre></div>`;
}
// Captured command output (tutorial "run this, see this" blocks). NOT live —
// authored snapshots; the label says so.
function runBlock(r, scopeIds) {
  return `<div class="run"><div class="label">captured output</div><div class="run-cmd">$ ${esc(r.cmd)}</div><pre class="run-out">${esc(r.output)}</pre>${r.note ? `<div class="run-note">${inline(r.note, scopeIds)}</div>` : ''}</div>`;
}

// --- excerpt renderer: two-pane (code left, aligned margin notes right) ------
function renderExcerpt(ex, repoRoot, warnings, scopeIds) {
  const abs = isAbsolute(ex.file) ? ex.file : join(repoRoot, ex.file);
  let fileText;
  try { fileText = readFileSync(abs, 'utf8'); }
  catch { warnings.push(`MISSING FILE: ${ex.file}`); return `<div class="drift">missing file: ${esc(ex.file)}</div>`; }
  const lines = fileText.split(/\r?\n/);
  const [s, e] = ex.lines;
  if (s < 1 || e > lines.length || s > e) {
    warnings.push(`BAD SPAN: ${ex.file}:${s}-${e} (file has ${lines.length})`);
    return `<div class="drift">bad span ${esc(ex.file)}:${s}-${e}</div>`;
  }
  const span = spanText(lines, s, e);
  const cur = sha8(span);
  let drift = '';
  if (ex.sha8 && ex.sha8 !== cur) {
    warnings.push(`DRIFT: ${ex.file}:${s}-${e} sha8 ${ex.sha8} -> ${cur}`);
    drift = `<div class="drift">⚠ code changed since annotation (sha8 ${esc(ex.sha8)} → ${cur}) — notes may be stale</div>`;
  }
  const anns = ex.annotations || [];
  const marks = new Map(); // anchor line -> [note index]
  const bandOf = new Map(); // line -> note index (first wins, for hover pairing)
  anns.forEach((a, i) => {
    const [as, ae] = a.lines;
    for (let l = as; l <= ae; l++) if (!bandOf.has(l)) bandOf.set(l, i);
    if (!marks.has(as)) marks.set(as, []);
    marks.get(as).push(i);
  });
  const lang = ex.lang || 'cpp';
  const rows = [];
  for (let l = s; l <= e; l++) {
    const codeHtml = hl(lines[l - 1] ?? '', lang);
    const ni = bandOf.has(l) ? bandOf.get(l) : -1;
    const band = ni >= 0 ? ` band nb-${ni}` : '';
    const mk = marks.has(l) ? marks.get(l).map(n => `<span class="chip">${n + 1}</span>`).join('') : '';
    rows.push(`<tr class="cl${band}" data-l="${l}"${ni >= 0 ? ` data-n="${ni}"` : ''}><td class="ln">${l}</td><td class="mk">${mk}</td><td class="cd">${codeHtml || '&nbsp;'}</td></tr>`);
  }
  const notes = anns.map((a, i) =>
    `<div class="note" data-a="${a.lines[0]}" data-n="${i}"><div class="nt"><span class="chip">${i + 1}</span><span class="nl">L${a.lines[0]}${a.lines[1] !== a.lines[0] ? '–' + a.lines[1] : ''}</span>${inline(a.text, scopeIds)}</div></div>`).join('\n');
  return `<div class="excerpt${ex.continues ? ' cont' : ''}${anns.length ? '' : ' bare'}">
${ex.continues ? '' : `<div class="ex-head"><span class="path">${esc(ex.file)}<span class="plines">:${s}–${e}</span></span>${ex.title ? `<span class="ex-title">${esc(ex.title)}</span>` : ''}<span class="lang">${esc(lang)}</span><span class="sha" title="span hash at render">${cur}</span></div>`}
${drift}
<div class="ex-grid">
<div class="ex-code"><table class="code">${rows.join('\n')}</table></div>
${anns.length ? `<div class="ex-rail">${notes}</div>` : ''}
</div>
</div>`;
}

// Colour tokens live in one object per theme and are emitted into all three
// selector blocks below, so the media query and both data-theme overrides can
// never drift apart. Contrast of every fg/bg pair here is checked in both themes.
const THEME = {
  light: {
    bg: '#ffffff', fg: '#1b2026', mut: '#5c6570',
    line: '#dfe4ea', 'line-soft': '#edf0f4', surface: '#f8f9fb',
    accent: '#0b6bcb',
    band: '#f2f4f7', 'band-edge': '#b6c0cc',      // "has a note" = neutral grey
    hot: '#cfe4f9', 'hot-edge': '#0b6bcb',         // hover pairing = blue
    'code-bg': '#f6f8fa', 'note-bg': '#fbfcfe',
    drift: '#b3261e', 'drift-bg': '#fdecea',
    'chip-bg': '#0b6bcb', 'chip-fg': '#ffffff',
    'tk-k': '#b02a37', 'tk-s': '#1a5f3c', 'tk-n': '#0550ae', 'tk-c': '#5c6570',
  },
  dark: {
    bg: '#0e1116', fg: '#dde3ea', mut: '#949daa',
    line: '#2a313a', 'line-soft': '#20262e', surface: '#151b23',
    accent: '#57a6ea',
    band: '#191f27', 'band-edge': '#3a4655',
    hot: '#17304a', 'hot-edge': '#57a6ea',
    'code-bg': '#141920', 'note-bg': '#12171e',
    drift: '#ef6a5a', 'drift-bg': '#3a1a15',
    'chip-bg': '#57a6ea', 'chip-fg': '#08101a',
    'tk-k': '#ff9c92', 'tk-s': '#7ed8a8', 'tk-n': '#79c0ff', 'tk-c': '#949daa',
  },
};
const vars = t => Object.entries(THEME[t]).map(([k, v]) => `--${k}:${v}`).join(';');

const CSS = `
:root{${vars('light')}}
@media (prefers-color-scheme: dark){:root{${vars('dark')}}}
:root[data-theme=light]{${vars('light')}}
:root[data-theme=dark]{${vars('dark')}}
:root{
  --ff-sans:system-ui,-apple-system,'Segoe UI',Roboto,'Helvetica Neue',Arial,'Noto Sans',sans-serif;
  --ff-mono:ui-monospace,'SF Mono',SFMono-Regular,'Cascadia Mono','Segoe UI Mono',Menlo,Consolas,'Liberation Mono',monospace;
}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%;scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--fg);font:400 16px/1.65 var(--ff-sans);-webkit-font-smoothing:antialiased}
.wrap{max-width:1360px;margin:0 auto;padding:0 28px 96px}

/* --- chrome ------------------------------------------------------------- */
.topbar{position:sticky;top:0;z-index:5;background:var(--bg);border-bottom:1px solid var(--line);margin:0 -28px 24px;padding:10px 28px;display:flex;gap:9px;align-items:baseline}
.topbar .crumb{color:var(--mut);font-size:13px;flex:none}
.topbar .here{font-weight:600;font-size:14px;letter-spacing:-.005em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline;text-underline-offset:2px}
a:focus-visible,.card:focus-within{outline:2px solid var(--accent);outline-offset:3px;border-radius:4px}

/* --- type scale --------------------------------------------------------- */
h1{font-size:26px;line-height:1.25;font-weight:700;letter-spacing:-.015em;margin:22px 0 10px;text-wrap:balance}
h2{font-size:19px;line-height:1.35;font-weight:650;letter-spacing:-.01em;margin:46px 0 12px;padding-left:12px;border-left:3px solid var(--accent);scroll-margin-top:66px;text-wrap:balance}
p{margin:0 0 .9em}
.wrap>p,section>p{max-width:72ch}
.meta{color:var(--mut);font-size:13px;line-height:1.55}
.meta+p{margin-top:14px}
.label{margin:0 0 9px;font:650 12px/1.4 var(--ff-sans);color:var(--mut);text-transform:uppercase;letter-spacing:.07em}
.summary{border:1px solid var(--line);border-left:3px solid var(--accent);border-radius:0 9px 9px 0;padding:13px 18px;margin:16px 0 24px;background:var(--surface);max-width:78ch;font-size:16.5px;line-height:1.62}
.summary p:last-child{margin-bottom:0}
.rule{border:none;border-top:1px solid var(--line);margin:48px 0 14px}

/* --- on-this-page ------------------------------------------------------- */
.toc{margin:0 0 28px;padding:13px 18px;border:1px solid var(--line);border-radius:10px;background:var(--surface);max-width:78ch}
.toc ol{margin:0;padding:0;list-style:none}
.toc li{display:flex;gap:11px;font-size:14.5px;line-height:1.5;margin:5px 0}
.toc .tn{flex:none;min-width:1.3em;color:var(--mut);font:650 12px/1.7 var(--ff-mono);font-variant-numeric:tabular-nums}

/* --- excerpt shell ------------------------------------------------------ */
.excerpt{margin:26px 0;border:1px solid var(--line);border-radius:10px;background:var(--bg)}
.excerpt.cont{margin-top:-27px;border-top:none;border-top-left-radius:0;border-top-right-radius:0}
.ex-head{display:flex;gap:12px;align-items:baseline;padding:9px 14px;background:var(--surface);border-bottom:1px solid var(--line);border-radius:9px 9px 0 0;font:500 12.5px/1.5 var(--ff-mono);min-width:0}
.path{font-weight:650;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.plines{color:var(--mut);font-weight:400}
.ex-title{color:var(--mut);font:400 13px/1.5 var(--ff-sans);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lang{margin-left:auto;flex:none;color:var(--mut);border:1px solid var(--line);border-radius:10px;padding:1px 8px;font-size:11px;letter-spacing:.05em;text-transform:uppercase}
.sha{flex:none;color:var(--mut);font-size:11px}

/* --- two-pane: code left, note rail right -------------------------------- */
.ex-grid{display:grid;grid-template-columns:minmax(0,1fr) clamp(320px,30%,420px);align-items:start}
.excerpt.bare .ex-grid{grid-template-columns:minmax(0,1fr)}
.ex-code{overflow-x:auto;overscroll-behavior-x:contain;padding:10px 0 12px}
table.code{border-collapse:collapse;width:100%;font:400 13.5px/1.55 var(--ff-mono);letter-spacing:0;font-variant-ligatures:none;tab-size:4}
/* keep these at the same specificity as the td reset above, or the reset wins */
table.code td{padding:0;white-space:pre;vertical-align:baseline}
table.code td.ln{width:1%;padding:0 8px 0 14px;color:var(--mut);text-align:right;user-select:none;font-size:12.5px;font-variant-numeric:tabular-nums}
table.code td.mk{width:26px;padding:0 4px;text-align:center;vertical-align:middle;user-select:none}
.excerpt.bare table.code td.mk{width:0;padding:0}
table.code td.cd{width:100%;padding:0 12px 0 6px}
tr.band td{background:var(--band)}
tr.band td.mk{box-shadow:inset 3px 0 0 var(--band-edge)}
tr.hot td{background:var(--hot)}
tr.hot td.mk{box-shadow:inset 3px 0 0 var(--hot-edge)}
/* text-indent:0 is load-bearing: the note body sets a negative text-indent for
   its hanging first line, which would otherwise be inherited by the chip's
   inner text box and fling the digit outside the card. */
.chip{display:inline-flex;align-items:center;justify-content:center;min-width:18px;height:18px;padding:0 5px;border-radius:9px;background:var(--chip-bg);color:var(--chip-fg);font:700 11px/1 var(--ff-sans);font-variant-numeric:tabular-nums;text-indent:0;flex:none}
.ex-rail{position:relative;align-self:stretch;min-width:0;border-left:1px solid var(--line)}
.note{position:absolute;left:14px;right:14px;padding:9px 13px 10px;border:1px solid var(--line);border-radius:8px;background:var(--note-bg);font-size:14px;line-height:1.55;transition:background .12s,border-color .12s,box-shadow .12s}
.note .nt{padding-left:26px;text-indent:-26px}
.note .chip{margin-right:7px}
.note .nl{margin-right:7px;white-space:nowrap;color:var(--mut);font:650 12px/1 var(--ff-mono);letter-spacing:.01em}
.note .nt code{font-size:12.5px}
.note.hot{background:var(--hot);border-color:var(--hot-edge);box-shadow:0 0 0 1px var(--hot-edge)}
@media (max-width: 979px){
  .ex-grid{grid-template-columns:minmax(0,1fr)}
  .ex-rail{border-left:none;border-top:1px solid var(--line);padding:12px 0 2px}
  .note{position:static;margin:0 14px 10px}
}

/* --- syntax ------------------------------------------------------------- */
.tk-k{color:var(--tk-k)}.tk-s{color:var(--tk-s)}.tk-n{color:var(--tk-n)}
.tk-c{color:var(--tk-c);font-style:italic}
.drift{background:var(--drift-bg);color:var(--drift);border-bottom:1px solid var(--line);padding:8px 14px;font-size:13px;line-height:1.5}
.run{margin:16px 0;border:1px solid var(--line);border-radius:8px;background:var(--code-bg);overflow:hidden}
.run .label{padding:6px 14px 0}
.run-cmd{padding:6px 14px 2px;font:600 12.5px/1.6 ui-monospace,'Cascadia Mono',Consolas,monospace;color:var(--accent);white-space:pre-wrap;word-break:break-all}
.run-out{margin:0;padding:2px 14px 10px;font:12.5px/1.55 ui-monospace,'Cascadia Mono',Consolas,monospace;overflow-x:auto;color:var(--fg)}
.run-note{padding:7px 14px 9px;border-top:1px solid var(--line-soft);font-size:13.5px;line-height:1.55;color:var(--mut)}

/* --- blocks ------------------------------------------------------------- */
.diagram{margin:22px 0;padding:14px 16px;border:1px solid var(--line);border-radius:10px;overflow-x:auto;background:var(--surface)}
pre.mermaid{margin:0;font:13px/1.5 var(--ff-mono)}
.edges,.cover{margin:26px 0;padding:14px 18px;border:1px solid var(--line);border-radius:10px;background:var(--surface)}
.edges ul,.cover ul{margin:0;padding-left:22px}
.edges li,.cover li{margin:6px 0;font-size:14.5px;line-height:1.55}
.cover .why{color:var(--mut)}
.cover .label+ul+.label{margin-top:18px}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:14px;margin:20px 0}
.card{border:1px solid var(--line);border-radius:10px;padding:14px 16px;background:var(--surface);transition:border-color .12s}
.card:hover{border-color:var(--accent)}
.card b{font-size:15.5px;font-weight:650;line-height:1.35}
.card .num{margin-bottom:7px;color:var(--mut);font:650 11px/1 var(--ff-mono);letter-spacing:.06em}
.card .meta{margin-top:5px}
.card.planned{opacity:.55}
.card.planned:hover{border-color:var(--line)}
code{background:var(--code-bg);border:1px solid var(--line-soft);padding:.5px 5px;border-radius:5px;font-family:var(--ff-mono);font-size:.875em}
.xl{font-weight:600}.xl-missing{color:var(--drift)}
@media (prefers-reduced-motion: reduce){html{scroll-behavior:auto}*{transition:none!important}}
`;

// Aligns each note card to its anchor line's vertical position (two-pane mode),
// pushing later notes down on overlap; pairs hover states both directions.
// Pass 2 pulls the stack back up so it never extends past the bottom of the code
// it annotates: when the notes are collectively taller than the excerpt, the
// leftover is split above and below the anchors instead of piling up entirely
// below the last one (which used to drift a note up to ~275px from its line).
const JS = `
(function(){
  var GAP = 10;
  function layout(){
    var wide = window.matchMedia('(min-width: 980px)').matches;
    document.querySelectorAll('.excerpt').forEach(function(ex){
      var rail = ex.querySelector('.ex-rail');
      if(!rail) return;
      var grid = ex.querySelector('.ex-grid');
      var notes = rail.querySelectorAll('.note');
      if(!wide){ notes.forEach(function(n){ n.style.top=''; }); rail.style.minHeight=''; return; }
      var g = grid.getBoundingClientRect(), y = 0, items = [];
      notes.forEach(function(n){
        var row = ex.querySelector('tr[data-l="'+n.dataset.a+'"]');
        var t = Math.max(row ? (row.getBoundingClientRect().top - g.top) : y, y);
        items.push({ n: n, t: t, h: n.offsetHeight });
        y = t + n.offsetHeight + GAP;
      });
      if(!items.length) return;
      var limit = ex.querySelector('.ex-code').getBoundingClientRect().height;
      for(var i = items.length - 1; i >= 0; i--){
        items[i].t = Math.min(items[i].t, limit - items[i].h);
        limit = items[i].t - GAP;
      }
      var shift = Math.max(0, -items[0].t), maxB = 0;
      items.forEach(function(it){
        var t = it.t + shift;
        it.n.style.top = t + 'px';
        maxB = Math.max(maxB, t + it.h);
      });
      rail.style.minHeight = (maxB + 12) + 'px';
    });
  }
  function hover(){
    document.querySelectorAll('.excerpt').forEach(function(ex){
      ex.querySelectorAll('.note').forEach(function(n){
        var i = n.dataset.n;
        var rows = ex.querySelectorAll('tr[data-n="'+i+'"]');
        n.addEventListener('mouseenter', function(){ rows.forEach(function(r){ r.classList.add('hot'); }); });
        n.addEventListener('mouseleave', function(){ rows.forEach(function(r){ r.classList.remove('hot'); }); });
        rows.forEach(function(r){
          r.addEventListener('mouseenter', function(){ n.classList.add('hot'); });
          r.addEventListener('mouseleave', function(){ n.classList.remove('hot'); });
        });
      });
    });
  }
  var t; function relayout(){ clearTimeout(t); t = setTimeout(layout, 60); }
  window.addEventListener('resize', relayout);
  window.addEventListener('load', relayout);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(layout);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', function(){ layout(); hover(); });
  else { layout(); hover(); }
})();
`;

function page(title, body, meta, crumb) {
  return `<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)}</title>
<style>${CSS}</style>
<div class="wrap">
<div class="topbar">${crumb ? `<a class="crumb" href="index.html">atlas</a><span class="crumb">/</span>` : `<span class="crumb">atlas</span>`}<span class="here">${esc(title)}</span></div>
${body}
<hr class="rule">
<div class="meta">${esc(meta)}</div>
</div>
<script>${JS}</script>`;
}

function buildProject(proj, opts = {}) {
  const pdir = join(ROOT, 'projects', proj);
  const atlas = JSON.parse(readFileSync(join(pdir, 'atlas.json'), 'utf8'));
  const repoRoot = atlas.repo_root;
  const outDir = join(ROOT, 'out', proj);
  mkdirSync(outDir, { recursive: true });
  const scopeIds = new Set(atlas.scopes.map(s => s.id));
  const warnings = [];
  const singleParts = [];
  const meta = `${atlas.title} — commit ${atlas.commit} — generated ${new Date().toISOString().slice(0, 10)} — code pulled live from repo at render time`;

  const coverage = [];
  for (const sc of atlas.scopes) {
    if (sc.status === 'planned') continue;
    let data;
    try { data = JSON.parse(readFileSync(join(pdir, 'scopes', `${sc.id}.json`), 'utf8')); }
    catch (err) {
      // status says done but the artifact is absent/broken: manifest/disk mismatch is a
      // HARD error (it mis-signals completion to anything trusting the manifest).
      console.error(`ERROR: scope '${sc.id}' marked done but unreadable: ${err.message.split('\n')[0]}`);
      process.exitCode = 1;
      continue;
    }
    const parts = [];
    parts.push(`<h1>${esc(data.title)}</h1>`);
    parts.push(`<div class="summary">${prose(data.summary, scopeIds)}</div>`);
    const heads = (data.sections || []).filter(s => s.heading);
    if (heads.length >= 3) {
      parts.push(`<nav class="toc"><div class="label">On this page</div><ol>` + heads.map((s, i) =>
        `<li><span class="tn">${i + 1}</span><a href="#${slug(s.heading)}">${inline(s.heading, scopeIds)}</a></li>`).join('') + `</ol></nav>`);
    }
    for (const d of data.diagrams || []) parts.push(mermaidBlock(d));
    for (const sec of data.sections || []) {
      parts.push(`<h2 id="${slug(sec.heading)}">${inline(sec.heading, scopeIds)}</h2>`);
      if (sec.prose) parts.push(`<section>${prose(sec.prose, scopeIds)}</section>`);
      for (const ex of sec.excerpts || []) parts.push(renderExcerpt(ex, repoRoot, warnings, scopeIds));
      for (const r of sec.runs || []) parts.push(runBlock(r, scopeIds));
    }
    if (data.edges?.length) {
      parts.push(`<div class="edges"><div class="label">Connected scopes</div><ul>` + data.edges.map(e =>
        `<li><a class="xl" href="${e.to}.html">${e.to}</a> — ${inline(e.why, scopeIds)}</li>`).join('') + `</ul></div>`);
    }
    if (data.not_annotated?.length) {
      const na = data.not_annotated.filter(x => !x.partial), pa = data.not_annotated.filter(x => x.partial);
      let cov = '<div class="cover">';
      if (na.length) cov += '<div class="label">Not annotated in this scope — read directly</div><ul>' + na.map(x =>
        `<li><code>${esc(x.path)}</code> <span class="why">— ${inline(x.reason, scopeIds)}</span></li>`).join('') + '</ul>';
      if (pa.length) cov += '<div class="label">Partially annotated — remainder is yours</div><ul>' + pa.map(x =>
        `<li><code>${esc(x.path)}</code> <span class="why">— ${inline(x.reason, scopeIds)}</span></li>`).join('') + '</ul>';
      cov += '</div>';
      parts.push(cov);
      coverage.push({ scope: sc, items: data.not_annotated });
    }
    writeFileSync(join(outDir, `${sc.id}.html`), page(data.title, parts.join('\n'), meta, true));
    singleParts.push(`<section class="chap" id="sc-${sc.id}">${parts.join('\n')}</section>`);
  }

  // index
  const ix = [];
  ix.push(`<h1>${esc(atlas.title)}</h1>`);
  ix.push(`<div class="meta">commit ${esc(atlas.commit)} · excerpts pulled from the live tree at render time · drift-checked per excerpt</div>`);
  if (atlas.intro) ix.push(prose(atlas.intro, scopeIds));
  for (const d of atlas.arch_diagrams || []) ix.push(mermaidBlock(d));
  ix.push(`<h2>Reading order</h2><div class="cards">` + atlas.scopes.map((s, i) =>
    s.status === 'planned'
      ? `<div class="card planned"><div class="num">${i + 1}</div><b>${esc(s.title)}</b><div class="meta">planned</div></div>`
      : `<div class="card"><div class="num">${i + 1}</div><a class="xl" href="${s.id}.html"><b>${esc(s.title)}</b></a></div>`).join('') + `</div>`);
  if (coverage.length) {
    ix.push(`<h2>Global "read yourself" list</h2><div class="cover"><ul>` + coverage.flatMap(c =>
      c.items.map(n => `<li><code>${esc(n.path)}</code> <span class="why">— ${n.partial ? '<b>partial</b> · ' : ''}${inline(n.reason, scopeIds)} <i>(${esc(c.scope.id)})</i></span></li>`)).join('') + `</ul></div>`);
  }
  writeFileSync(join(outDir, 'index.html'), page(atlas.title, ix.join('\n'), meta, false));

  if (opts.single) {
    // One self-contained file (for artifact publishing: cross-page hrefs would be
    // dead across separate artifacts). Rewrites scope-page links to #sc-<id>
    // anchors; deep section anchors collapse to the chapter top.
    const rewrite = h => h.replace(/href="([a-z0-9-]+)\.html(#[a-z0-9-]+)?"/g, (m, id) =>
      scopeIds.has(id) ? `href="#sc-${id}"` : (id === 'index' ? 'href="#sc-top"' : m));
    const body = `<div id="sc-top"></div>` + rewrite(ix.join('\n')) + '\n' + rewrite(singleParts.join('\n'));
    writeFileSync(join(outDir, `${proj}-tutorial.html`), page(atlas.title, body, meta, false));
  }

  if (warnings.length) {
    console.error(`WARNINGS (${warnings.length}):`);
    for (const w of warnings) console.error('  ' + w);
  }
  console.log(`built ${atlas.scopes.filter(s => s.status !== 'planned').length + 1} pages -> ${outDir}${warnings.length ? ` (${warnings.length} warnings)` : ''}`);
}

// --- cli ---------------------------------------------------------------------
const [cmd, ...args] = process.argv.slice(2);
if (cmd === 'hash') {
  const [file, s, e] = args;
  const abs = isAbsolute(file) ? file : resolve(process.cwd(), file);
  const lines = readFileSync(abs, 'utf8').split(/\r?\n/);
  console.log(sha8(spanText(lines, +s, +e)));
} else if (cmd === 'rehash') {
  const proj = args[0];
  const pdir = join(ROOT, 'projects', proj);
  const atlas = JSON.parse(readFileSync(join(pdir, 'atlas.json'), 'utf8'));
  for (const sc of atlas.scopes) {
    if (sc.status === 'planned') continue;
    const p = join(pdir, 'scopes', sc.id + '.json');
    let data;
    try { data = JSON.parse(readFileSync(p, 'utf8')); } catch { continue; }
    let changed = 0;
    for (const sec of data.sections || []) for (const ex of sec.excerpts || []) {
      const abs = isAbsolute(ex.file) ? ex.file : join(atlas.repo_root, ex.file);
      const lines = readFileSync(abs, 'utf8').split(/\r?\n/);
      const h = sha8(spanText(lines, ex.lines[0], ex.lines[1]));
      if (ex.sha8 !== h) { ex.sha8 = h; changed++; }
    }
    if (changed) { writeFileSync(p, JSON.stringify(data, null, 2) + '\n'); console.log(sc.id + ': ' + changed + ' sha8 updated'); }
  }
} else if (cmd === 'build') {
  buildProject(args[0], { single: args.includes('--single') });
} else {
  console.log('usage: node gen.mjs build <project> | rehash <project> | hash <file> <start> <end>');
  process.exit(1);
}
