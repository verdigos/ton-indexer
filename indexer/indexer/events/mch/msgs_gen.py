"""M1 message-schema compiler (program doc §3.2).

Compiles ``ton-mch-engine/mch-msgs.schema`` into a committed C++ TU of per-message
body parsers plus a boundary-coverage vectors JSON. The parsers call ONLY the
existing PSlice leaf helpers (zero new leaf semantics); each reproduces its hand
predecessor byte-for-byte, guarded by the parse-dump A/B twin gate.

Determinism: output order = schema order; regeneration is byte-stable, guarded by
tests/mch/test_msgs_gen.py (repro guard, the A1 discipline).

Usage (from the indexer/ dir, venv python):
    python -m indexer.events.mch.msgs_gen            # write cpp + vectors
    python -m indexer.events.mch.msgs_gen --check    # write to temp, diff committed
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- paths ------------------------------------------------------------------
_MCH = Path(__file__).resolve()
REPO = _MCH.parents[4]  # S:\Dev\ton-indexer-clear
ENG = REPO / "ton-index-worker" / "ton-mch-engine"
SCHEMA = ENG / "mch-msgs.schema"
CPP_OUT = ENG / "src" / "MsgParsersGenerated.cpp"
VEC_OUT = ENG / "ir" / "msg_vectors.json"

# --- schema model -----------------------------------------------------------
LEAVES = {
    "bool", "u32", "u64", "uint128", "uint256", "int24", "coins", "addr_py",
    "asset2", "asset4", "ref_cell", "maybe_ref_cell", "dict_keys267",
    "fwd_cell_bits", "fwd_cell_refs",
}

# A field whose name starts with "_" is PARSED (cursor advanced, failure still
# fails the whole parse) but NOT emitted — the §3.2 parsed-and-dropped convention.


@dataclass
class Field:
    name: str
    type: str  # a leaf name, "bits", or "ref"
    sub: list["Field"] = field(default_factory=list)  # ref subfields
    bitn: int = 0  # width for "bits N" (fixed-width skip)

    @property
    def drop(self) -> bool:
        return self.name.startswith("_")


@dataclass
class Message:
    name: str
    opcode: int
    fields: list[Field]


# --- schema parser ----------------------------------------------------------
def parse_schema(text: str) -> list[Message]:
    # Tokenize into words / braces / colons, dropping comments.
    toks: list[str] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0]
        for m in re.finditer(r"\{|\}|:|[^\s:{}]+", line):
            toks.append(m.group(0))
    pos = 0

    def peek() -> str | None:
        return toks[pos] if pos < len(toks) else None

    def eat(expect: str | None = None) -> str:
        nonlocal pos
        if pos >= len(toks):
            raise ValueError(f"unexpected end of schema (wanted {expect})")
        t = toks[pos]
        pos += 1
        if expect is not None and t != expect:
            raise ValueError(f"expected {expect!r}, got {t!r}")
        return t

    def parse_fields() -> list[Field]:
        eat("{")
        out: list[Field] = []
        while peek() != "}":
            fname = eat()
            eat(":")
            ftype = eat()
            if ftype == "ref":
                out.append(Field(fname, "ref", parse_fields()))
            elif ftype == "bits":
                out.append(Field(fname, "bits", bitn=int(eat())))
            elif ftype in LEAVES:
                out.append(Field(fname, ftype))
            else:
                raise ValueError(f"unknown field type {ftype!r} for {fname!r}")
        eat("}")
        return out

    msgs: list[Message] = []
    while peek() is not None:
        eat("message")
        name = eat()
        eat("opcode")
        opcode = int(eat(), 16)
        # optional policy word(s) before the brace (currently only skip-opcode)
        while peek() not in ("{", None):
            eat()
        msgs.append(Message(name, opcode, parse_fields()))
    return msgs


def snake(camel: str) -> str:
    s = re.sub(r"(?<!^)(?=[A-Z])", "_", camel).lower()
    return s


# --- C++ emit ---------------------------------------------------------------
ASSET2_HELPER = """\
// messages/coffee.py load_coffee_asset: 2-bit tag (0 TON, 1 jetton wc:HEX upper).
static td::Result<Value> gen_load_asset2(vm::CellSlice &cs) {
  if (!cs.have(2)) {
    return td::Status::Error("asset2: tag underflow");
  }
  auto tag = cs.fetch_ulong(2);
  if (tag == 0) {
    return Value::make_asset_ton();
  }
  if (tag == 1) {
    if (!cs.have(8)) {
      return td::Status::Error("asset2: chain underflow");
    }
    int chain = static_cast<int>(cs.fetch_ulong(8));
    if (!cs.have(256)) {
      return td::Status::Error("asset2: hash underflow");
    }
    unsigned char buf[32];
    if (!cs.fetch_bytes(buf, 32)) {
      return td::Status::Error("asset2: hash fetch failed");
    }
    return Value::make_asset_jetton(std::to_string(chain) + ":" + hex_upper(buf, 32));
  }
  return td::Status::Error("asset2: unsupported tag");
}
"""

ASSET4_HELPER = """\
// messages/swaps.py load_asset: 4-bit kind (0 TON, else wc:uint8 + 32 bytes).
static td::Result<Value> gen_load_asset4(vm::CellSlice &cs) {
  if (!cs.have(4)) {
    return td::Status::Error("asset4: kind underflow");
  }
  auto kind = cs.fetch_ulong(4);
  if (kind == 0) {
    return Value::make_asset_ton();
  }
  if (!cs.have(8 + 256)) {
    return td::Status::Error("asset4: jetton underflow");
  }
  int wc = static_cast<int>(cs.fetch_ulong(8));
  unsigned char buf[32];
  if (!cs.fetch_bytes(buf, 32)) {
    return td::Status::Error("asset4: hash fetch failed");
  }
  return Value::make_asset_jetton(std::to_string(wc) + ":" + hex_upper(buf, 32));
}
"""

DICT_HELPER = """\
// messages/jvault.py load_address_list: HashmapE(267) key walk (MsgAddressStd
// keys, values ignored, ascending). Constructs the Dictionary from an explicit
// root cell so a malformed node only THROWS VmError (caught by MsgParse) rather
// than CHECK/abort() (see the hand predecessor for the rationale).
static td::Status gen_load_addr_dict267(vm::CellSlice &cs, std::vector<Value> &out) {
  if (!cs.have(1)) {
    return td::Status::Error("dict267: maybe underflow");
  }
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() < 1) {
      return td::Status::Error("dict267: root ref missing");
    }
    td::Status inner = td::Status::OK();
    vm::Dictionary dict{cs.fetch_ref(), 267};
    bool ok = dict.check_for_each(
        [&](td::Ref<vm::CellSlice>, td::ConstBitPtr key, int n) -> bool {
          vm::CellBuilder cb;
          if (!cb.store_bits_bool(key, n)) {
            inner = td::Status::Error("dict267: key store failed");
            return false;
          }
          vm::CellSlice keycs = vm::load_cell_slice(cb.finalize());
          auto addr = load_address_py(keycs);
          if (addr.is_error()) {
            inner = addr.move_as_error();
            return false;
          }
          out.push_back(addr.move_as_ok());
          return true;
        });
    if (!ok) {
      return inner.is_error() ? std::move(inner)
                              : td::Status::Error("dict267: address list parse failed");
    }
  }
  return td::Status::OK();
}
"""


def emit_field(msg: str, cur: str, f: Field, indent: str) -> list[str]:
    """C++ lines reading one field from cursor `cur`. A field is READ (cursor
    advanced, failure fails the whole parse) always; it is emplaced into
    `Value::Fields f` only when not dropped (name not starting with `_`)."""
    n = f.name
    err = lambda why: f'return td::Status::Error("{msg}: {n} {why}");'
    L: list[str] = []
    ind = indent
    if f.type == "ref":
        # A ref group is always descended; its subfields flatten into the parent.
        L.append(f"{ind}{{")
        L.append(f"{ind}  if ({cur}.size_refs() == 0) {{ {err('ref missing')} }}")
        L.append(f"{ind}  bool sp_{n.strip('_')} = false;")
        L.append(f"{ind}  vm::CellSlice rc_{n.strip('_')};")
        L.append(f"{ind}  try {{ rc_{n.strip('_')} = vm::load_cell_slice_special({cur}.fetch_ref(), sp_{n.strip('_')}); }}")
        L.append(f'{ind}  catch (...) {{ {err("ref cell")} }}')
        for sub in f.sub:
            L += emit_field(msg, f"rc_{n.strip('_')}", sub, ind + "  ")
        L.append(f"{ind}}}")
        return L

    L.append(f"{ind}{{")
    b = ind + "  "
    emit = None  # C++ expression for the field value, or None for a pure skip
    if f.type == "bits":
        L.append(f"{b}if (!{cur}.have({f.bitn}) || !{cur}.advance({f.bitn})) {{ {err('underflow')} }}")
    elif f.type == "bool":
        L.append(f"{b}if (!{cur}.have(1)) {{ {err('underflow')} }}")
        L.append(f"{b}bool bv = {cur}.fetch_ulong(1) != 0;")
        emit = "Value::make_bool(bv)"
    elif f.type == "u32":
        L.append(f"{b}if (!{cur}.have(32)) {{ {err('underflow')} }}")
        L.append(f"{b}auto q = {cur}.fetch_ulong(32);")
        emit = "Value::make_int(refint_u64(q))"
    elif f.type == "u64":
        L.append(f"{b}if (!{cur}.have(64)) {{ {err('underflow')} }}")
        L.append(f"{b}auto q = {cur}.fetch_ulong(64);")
        emit = "Value::make_int(refint_u64(q))"
    elif f.type in ("uint128", "uint256", "int24"):
        # Widths that overflow / are signed, so fetch_ulong is not an option.
        # int24 is the only SIGNED leaf: pytoniq's load_int(24) yields a negative
        # Python int for a negative tick, and fetch_int256(24, true) matches it.
        width, signed = {"uint128": (128, False), "uint256": (256, False),
                         "int24": (24, True)}[f.type]
        L.append(f"{b}if (!{cur}.have({width})) {{ {err('underflow')} }}")
        L.append(f"{b}auto iv = {cur}.fetch_int256({width}, {'true' if signed else 'false'});")
        L.append(f'{b}if (iv.is_null()) {{ {err("fetch failed")} }}')
        emit = "Value::make_int(std::move(iv))"
    elif f.type == "coins":
        L.append(f"{b}auto rc = load_coins_py({cur});")
        L.append(f"{b}if (rc.is_error()) {{ return rc.move_as_error(); }}")
        emit = "Value::make_int(rc.move_as_ok())"
    elif f.type == "addr_py":
        L.append(f"{b}auto ra = load_address_py({cur});")
        L.append(f"{b}if (ra.is_error()) {{ return ra.move_as_error(); }}")
        emit = "ra.move_as_ok()"
    elif f.type in ("asset2", "asset4"):
        helper = "gen_load_asset2" if f.type == "asset2" else "gen_load_asset4"
        L.append(f"{b}auto rs = {helper}({cur});")
        L.append(f"{b}if (rs.is_error()) {{ return rs.move_as_error(); }}")
        emit = "rs.move_as_ok()"
    elif f.type == "ref_cell":
        L.append(f"{b}if ({cur}.size_refs() == 0) {{ {err('ref missing')} }}")
        L.append(f"{b}auto rf = {cur}.fetch_ref();")  # consume even when dropped
        emit = "Value::make_cell(std::move(rf))"
    elif f.type == "maybe_ref_cell":
        # Maybe ^Cell: bit=1 -> require+consume ref (Cell); bit=0 -> Null. No
        # remainder fallback (distinct from fwd_cell_*).
        L.append(f"{b}if (!{cur}.have(1)) {{ {err('bit missing')} }}")
        L.append(f"{b}Value mc = Value::null();")
        L.append(f"{b}if ({cur}.fetch_ulong(1)) {{")
        L.append(f"{b}  if ({cur}.size_refs() == 0) {{ {err('ref missing')} }}")
        L.append(f"{b}  mc = Value::make_cell({cur}.fetch_ref());")
        L.append(f"{b}}}")
        emit = "std::move(mc)"
    elif f.type == "dict_keys267":
        L.append(f"{b}std::vector<Value> keys;")
        L.append(f"{b}auto ds = gen_load_addr_dict267({cur}, keys);")
        L.append(f"{b}if (ds.is_error()) {{ return ds; }}")
        emit = "Value::make_list(std::move(keys))"
    elif f.type in ("fwd_cell_bits", "fwd_cell_refs"):
        rest = f"{cur}.size() > 0" if f.type == "fwd_cell_bits" else f"{cur}.size_refs() > 0"
        L.append(f"{b}if (!{cur}.have(1)) {{ {err('bit missing')} }}")
        L.append(f"{b}Value fc = Value::null();")
        L.append(f"{b}if ({cur}.fetch_ulong(1)) {{")
        L.append(f"{b}  if ({cur}.size_refs() == 0) {{ {err('ref missing')} }}")
        L.append(f"{b}  fc = Value::make_cell({cur}.fetch_ref());")
        L.append(f"{b}}} else if ({rest}) {{")
        L.append(f"{b}  vm::CellBuilder cb;")
        L.append(f"{b}  cb.store_bits({cur}.data_bits(), {cur}.size());")
        L.append(f"{b}  for (unsigned i = 0; i < {cur}.size_refs(); i++) {{ cb.store_ref({cur}.prefetch_ref(i)); }}")
        L.append(f"{b}  try {{ fc = Value::make_cell(cb.finalize()); }}")
        L.append(f'{b}  catch (...) {{ {err("to_cell failed")} }}')
        L.append(f"{b}}}")
        emit = "std::move(fc)"
    else:  # pragma: no cover
        raise ValueError(f"unhandled leaf {f.type}")
    if emit is not None and not f.drop:
        L.append(f'{b}f.emplace_back("{n}", {emit});')
    L.append(f"{ind}}}")
    return L


def used_helpers(msgs: list[Message]) -> set[str]:
    used: set[str] = set()

    def walk(fs: list[Field]) -> None:
        for f in fs:
            if f.type in ("asset2", "asset4", "dict_keys267"):
                used.add(f.type)
            if f.type == "ref":
                walk(f.sub)

    for m in msgs:
        walk(m.fields)
    return used


def emit_cpp(msgs: list[Message]) -> str:
    used = used_helpers(msgs)
    out: list[str] = []
    out.append("// GENERATED by indexer/events/mch/msgs_gen.py from mch-msgs.schema.")
    out.append("// DO NOT EDIT — regenerate via `python -m indexer.events.mch.msgs_gen`;")
    out.append("// byte-repro guarded by tests/mch/test_msgs_gen.py. See MsgParse.cpp for the")
    out.append("// pytoniq-parity catalogue and parse/PSlice.h for the leaf helpers.")
    out.append('#include "MsgParsersGenerated.h"')
    out.append("")
    out.append('#include "MsgParse.h"')
    out.append('#include "parse/PSlice.h"')
    out.append("")
    out.append('#include "common/refint.h"')
    out.append('#include "vm/cellslice.h"')
    out.append('#include "vm/cells/CellBuilder.h"')
    if "dict_keys267" in used:
        out.append('#include "vm/dict.h"')
    out.append("")
    out.append("#include <string>")
    out.append("#include <utility>")
    out.append("#include <vector>")
    out.append("")
    out.append("namespace mch {")
    out.append("")
    out.append("namespace {")
    out.append("")
    if "asset2" in used:
        out.append(ASSET2_HELPER)
    if "asset4" in used:
        out.append(ASSET4_HELPER)
    if "dict_keys267" in used:
        out.append(DICT_HELPER)
    for m in msgs:
        fn = f"gen_parse_{snake(m.name)}"
        out.append(f"// {m.name} (opcode 0x{m.opcode:08x}); leading opcode skipped unchecked.")
        out.append(f"td::Result<Value> {fn}(const td::Ref<vm::Cell> &body) {{")
        out.append("  TRY_RESULT(ctx, open_body(body));")
        out.append("  auto &cs = ctx.cs;")
        out.append('  if (!cs.have(32) || !cs.advance(32)) {')
        out.append(f'    return td::Status::Error("{m.name}: opcode underflow");')
        out.append("  }")
        out.append("  Value::Fields f;")
        for fld in m.fields:
            out += emit_field(m.name, "cs", fld, "  ")
        out.append("  return Value::make_obj(std::move(f));")
        out.append("}")
        out.append("")
    out.append("}  // namespace")
    out.append("")
    out.append("const std::vector<std::pair<std::string, MsgParserFn>> &generated_message_parsers() {")
    out.append("  static const std::vector<std::pair<std::string, MsgParserFn>> rows = {")
    for m in msgs:
        out.append(f'      {{"{m.name}", gen_parse_{snake(m.name)}}},')
    out.append("  };")
    out.append("  return rows;")
    out.append("}")
    out.append("")
    out.append("}  // namespace mch")
    return "\n".join(out) + "\n"


# --- vectors (boundary coverage) --------------------------------------------
def emit_vectors(msgs: list[Message]) -> str:
    """Per-message boundary vectors: a minimal-valid body (expect ok) and the
    opcode-only truncation (expect fail). Bodies are built with pytoniq_core."""
    import json

    from pytoniq_core import begin_cell  # type: ignore

    def b64(cell) -> str:
        import base64
        return base64.b64encode(cell.to_boc()).decode()

    def build_min(m: Message):
        b = begin_cell().store_uint(m.opcode, 32)
        _build_fields(b, m.fields)
        return b.end_cell()

    def _build_fields(b, fields):
        for f in fields:  # dropped (_) fields still consume bits, so build them too
            if f.type == "bits":
                b.store_uint(0, f.bitn)
            elif f.type == "bool":
                b.store_uint(0, 1)
            elif f.type == "u32":
                b.store_uint(0, 32)
            elif f.type == "u64":
                b.store_uint(0, 64)
            elif f.type == "uint128":
                b.store_uint(0, 128)
            elif f.type == "uint256":
                b.store_uint(0, 256)
            elif f.type == "int24":
                b.store_int(0, 24)
            elif f.type == "coins":
                b.store_coins(0)
            elif f.type == "addr_py":
                b.store_uint(0, 2)  # addr_none
            elif f.type == "asset2":
                b.store_uint(0, 2)  # TON
            elif f.type == "asset4":
                b.store_uint(0, 4)  # TON
            elif f.type == "ref_cell":
                b.store_ref(begin_cell().end_cell())
            elif f.type == "maybe_ref_cell":
                b.store_uint(0, 1)  # absent
            elif f.type == "dict_keys267":
                b.store_uint(0, 1)  # empty HashmapE
            elif f.type in ("fwd_cell_bits", "fwd_cell_refs"):
                b.store_uint(0, 1)  # no payload
            elif f.type == "ref":
                rb = begin_cell()
                _build_fields(rb, f.sub)
                b.store_ref(rb.end_cell())

    cases = []
    for m in msgs:
        cases.append({"type": m.name, "body": b64(build_min(m)), "expect": "ok"})
        # opcode-only truncation: fails unless the message has no fields.
        if m.fields:
            trunc = begin_cell().store_uint(m.opcode, 32).end_cell()
            cases.append({"type": m.name, "body": b64(trunc), "expect": "fail"})
    doc = {"note": "M1 boundary-coverage vectors (msgs_gen.py). ok=parses, fail=parse error.",
           "cases": cases}
    return json.dumps(doc, indent=2, sort_keys=False) + "\n"


# --- driver -----------------------------------------------------------------
def generate() -> tuple[str, str]:
    msgs = parse_schema(SCHEMA.read_text(encoding="utf-8"))
    return emit_cpp(msgs), emit_vectors(msgs)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify committed outputs are byte-identical (no write)")
    args = ap.parse_args(argv)
    cpp, vec = generate()
    if args.check:
        ok = True
        for path, gen in ((CPP_OUT, cpp), (VEC_OUT, vec)):
            cur = path.read_text(encoding="utf-8") if path.is_file() else None
            if cur != gen:
                print(f"DRIFT: {path}")
                ok = False
        print("msgs_gen: OK" if ok else "msgs_gen: DRIFT")
        return 0 if ok else 1
    CPP_OUT.write_text(cpp, encoding="utf-8", newline="\n")
    VEC_OUT.write_text(vec, encoding="utf-8", newline="\n")
    print(f"wrote {CPP_OUT}")
    print(f"wrote {VEC_OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
