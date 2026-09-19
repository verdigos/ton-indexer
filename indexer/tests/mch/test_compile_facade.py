from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.mch import compile_dir, compile_file, compile_text
from indexer.events.mch.registry import Registries


def _basic_registries() -> Registries:
    return Registries()


def test_compile_text_returns_one_compiled_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock

        op Foo @t
        out { v: 1 }
    }
    """
    out = compile_text(src, "test.mch", _basic_registries())
    assert len(out) == 1
    assert out[0].name == "m"


def test_compile_file_reads_from_disk(tmp_path: Path):
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock

        op Foo @t
        out { v: 1 }
    }
    """
    p = tmp_path / "x.mch"
    p.write_text(src, encoding="utf-8")
    out = compile_file(p, _basic_registries())
    assert len(out) == 1


def test_compile_dir_concatenates(tmp_path: Path):
    (tmp_path / "a.mch").write_text(
        """
        opcode A = 0x1
        matcher a_m {
            produces FooBlock

            op A @t
            out { v: 1 }
        }
        """,
        encoding="utf-8",
    )
    (tmp_path / "b.mch").write_text(
        """
        opcode B = 0x2
        matcher b_m {
            produces FooBlock

            op B @t
            out { v: 1 }
        }
        """,
        encoding="utf-8",
    )
    reg = _basic_registries()
    out = compile_dir(tmp_path, reg)
    names = {c.name for c in out}
    assert names == {"a_m", "b_m"}
