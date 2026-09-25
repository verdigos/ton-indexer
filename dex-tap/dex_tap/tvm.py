"""Run pool get methods locally in the TVM emulator on the state the tap sent.

Needs libemulator from a current TON release (the one bundled with pytvm is too old):

    curl -L -o ~/libemulator-linux-x86_64.so \\
        https://github.com/ton-blockchain/ton/releases/latest/download/libemulator-linux-x86_64.so

TVM_EMULATOR_LIB overrides the path.
"""
from __future__ import annotations

import base64
import os
from pathlib import Path
from typing import Callable, Sequence

from pytoniq_core import Cell, HashMap, Slice, VmStack

_ENGINE = None


def _engine():
    from pytvm.engine import EmulatorEngineC

    global _ENGINE
    if _ENGINE is None:
        library = Path(os.environ.get("TVM_EMULATOR_LIB", "~/libemulator-linux-x86_64.so")).expanduser()
        if not library.exists():
            raise RuntimeError(f"TVM emulator library not found at {library}; see dex_tap/tvm.py")
        _ENGINE = EmulatorEngineC(str(library))
        _ENGINE.emulator_set_verbosity_level(0)
    return _ENGINE


def _b64(value: bytes) -> str:
    return base64.b64encode(value).decode()


def library_hashes(code: Cell) -> list[bytes]:
    """Hashes of library cells referenced from the code tree."""
    found, pending = [], [code]
    while pending:
        cell = pending.pop()
        if cell.type_ == 2:
            found.append(cell.bits[8:].tobytes())
        pending.extend(cell.refs)
    return found


class Contract:
    """One account state, ready to run get methods on."""

    def __init__(self, address: str, code_boc: str, data_boc: str, balance: int, utime: int,
                 load_library: Callable[[bytes], Cell]):
        self.address = address
        self.code_boc = code_boc
        self.data_boc = data_boc
        self.balance = balance
        self.utime = utime
        self.libraries = None
        libs: dict[int, Cell] = {}
        pending = [Cell.one_from_boc(base64.b64decode(code_boc))]
        while pending:
            for lib_hash in library_hashes(pending.pop()):
                key = int.from_bytes(lib_hash, "big")
                if key not in libs:
                    libs[key] = load_library(lib_hash)
                    pending.append(libs[key])
        if libs:
            self.libraries = HashMap(256, value_serializer=lambda value, builder: builder.store_ref(value),
                                     map_=libs).serialize()

    def run(self, method: str, stack: Sequence = ()) -> list | None:
        from pytvm.utils import get_method_id

        engine = _engine()
        emulator = engine.tvm_emulator_create(self.code_boc.encode(), self.data_boc.encode(), 0)
        try:
            if self.libraries is not None:
                engine.tvm_emulator_set_libraries(emulator, _b64(self.libraries.to_boc()).encode())
            engine.tvm_emulator_set_c7(emulator, self.address.encode(), self.utime, self.balance, b"0" * 64, None)
            stack_boc = VmStack.serialize(list(stack))
            result = engine.tvm_emulator_run_get_method(emulator, get_method_id(method), _b64(stack_boc.to_boc()).encode())
        finally:
            engine.tvm_emulator_destroy(emulator)
        if not result.get("success") or int(result.get("vm_exit_code", -1)) not in (0, 1):
            return None
        return list(VmStack.deserialize(Slice.one_from_boc(result["stack"])))
