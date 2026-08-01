"""Opcode-dominance gate for schema-backed message parser call sites.

ABI ``from_slice`` checks its prefix while the legacy generated parser skips it.
Every parse use therefore needs an independent, matching opcode constraint.  IR
uses are proved from matcher topology.  Host uses are a reviewed call-site
manifest: exact source locations make source drift fail closed, and each entry
records the traced guard rather than guessing from a nearby opcode literal.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
MCH = REPO / "ton-index-worker/ton-mch-engine"
SCHEMA = MCH / "mch-msgs.schema"
IR = MCH / "ir/mch_ir.json"
HOST = MCH / "src/host"


@dataclass(frozen=True)
class HostCall:
    file: str
    line: int
    message: str
    guard_opcodes: tuple[int, ...]
    evidence: str


@dataclass(frozen=True)
class DominanceFailure:
    message: str
    detail: str


KNOWN_NON_DOMINATED = {
    "JettonMint": (
        "IR jetton_mint: mint capture shared with MinterJettonMint under a 2-opcode "
        "anchor {0x15, 0x642b7d07}; not individually opcode-dominated"
    ),
    "CoffeeCreateVault": (
        "schema opcode 0x00000000, wire truth 0xc0ffee06 at IR coffee_create_vault node 27"
    ),
    "DedustPayoutFromPool": (
        "schema opcode 0x474f86cf, wire truth 0xad4eb6f5 at IR dedust_withdraw nodes "
        "122/126 + HostDedust.cpp:281"
    ),
    "DedustV2PayNative": (
        "schema opcode 0xa5a7cbf8; 5 IR uses (dedust_v2_swap / failed_swap / deposit_final / "
        "deposit_rejected / deposit_partial) whose anchors also admit other ops/a "
        "jetton_transfer arm, not single-opcode-dominated"
    ),
    "JettonInternalTransfer": (
        "schema opcode 0x178d4519; the ethena_withdrawal_request.ts_usde_transfer capture is "
        "guarded by 0xb2583ed5 instead"
    ),
    "CocoonOwnerClientRequestRefund": (
        "schema opcode 0xfafa6cc1; cocoon_client_request_refund capture is reachable under a "
        "2-opcode anchor (parent-wallet 0x9c69f376, refund 0xfafa6cc1)"
    ),
    "PTonTransfer": (
        "schema opcode 0x01f3835d; HostStonfi.cpp:519 (bare pton_self_transfer) never checks "
        "the parsed body's opcode"
    ),
    "JVaultStakePeriodPayload": (
        "schema opcode 0x00000000; HostJvault.cpp:46 decodes an arbitrary jetton "
        "forward_payload with no opcode check"
    ),
}


# Each entry was traced through its enclosing function and, where necessary,
# through every helper caller / matcher capture. Empty guard_opcodes means no
# dominating check was found; a non-matching value records an explicit mismatch.
HOST_CALLS = (
    HostCall("HostEvaa.cpp", 352, "EvaaSupplyMaster", (0x00000001,),
             "evaa_supply_anchor accepts the call arm only through is_call_op(kSupplyMaster)"),
    HostCall("HostEvaa.cpp", 383, "EvaaSupplyJettonForward", (0x00000001,),
             "evaa_supply_anchor opens this same forward-payload cell and checks kSupplyMaster"),
    HostCall("HostEvaa.cpp", 434, "EvaaSupplySuccess", (0x0000011A,),
             "success is assigned only by is_call_op(kSupplySuccess)"),
    HostCall("HostStonfi.cpp", 72, "StonfiPaymentRequest", (0xF93BB43F,),
             "both helper callers use the StonfiPaymentRequest op node/is_call_op filter"),
    HostCall("HostStonfi.cpp", 97, "StonfiSwapMessage", (0x25938561,),
             "the helper input is the matcher\'s kStonfiSwap contract capture"),
    HostCall("HostStonfi.cpp", 124, "StonfiV2PayTo", (0x657B54F5,),
             "the helper input is selected from kV2PayTo contract calls"),
    HostCall("HostStonfi.cpp", 519, "PTonTransfer", (),
             "bare pton_self_transfer pred checks payload opcode, but never the parsed body opcode"),
    HostCall("HostStonfi.cpp", 744, "PTonTransfer", (0x01F3835D,),
             "same branch checks im->opcode32() == kPTonTransfer"),
    HostCall("HostStonfi.cpp", 866, "PTonTransfer", (0x01F3835D,),
             "pton_transfer is assigned only by is_call_op(kPTonTransfer)"),
    HostCall("HostTonco.cpp", 58, "ToncoRouterV3SwapSourceWallet", (0xA7FB58F8,),
             "all helper inputs come from derive_tonco_parts kV3Swap filtering"),
    HostCall("HostTonco.cpp", 287, "JettonNotify", (0x7362D09C,),
             "jetton_notify_block is selected only by find_call/is_call_op(kJettonNotify)"),
    HostCall("HostTonco.cpp", 380, "PTonTransfer", (0x01F3835D,),
             "pton is returned by find_call(..., kPTonTransfer)"),
    HostCall("HostTonco.cpp", 496, "PTonTransfer", (0x01F3835D,),
             "pton is returned by find_call(..., kPTonTransfer)"),
    HostCall("HostTonco.cpp", 704, "PTonTransfer", (0x01F3835D,),
             "same branch checks m->opcode32() == kPTonTransfer"),
    HostCall("HostDedustDeposit.cpp", 100, "DedustDepositLiquidityToPool", (0xB56B9598,),
             "all helper inputs are the matcher\'s pool opcode anchor"),
    HostCall("HostDedustDeposit.cpp", 209, "JettonInternalTransfer", (0x178D4519,),
             "lp_transfer is assigned only by is_call_op(kJettonInternalTransfer)"),
    HostCall("HostDedustDeposit.cpp", 254, "JettonNotify", (0x7362D09C,),
             "resolve_leg obtains the jetton deposit through find_call(kJettonNotify)"),
    HostCall("HostDedustDeposit.cpp", 458, "JettonNotify", (0x7362D09C,),
             "the non-TON matcher leg is the exact deposit_notify contract branch"),
    HostCall("HostDedust.cpp", 159, "DedustSwapNotification", (0x9C610DE3,),
             "notif_blocks contains only is_call_op(kDedustSwapNotification)"),
    HostCall("HostDedust.cpp", 281, "DedustPayoutFromPool", (0xAD4EB6F5,),
             "payout_from_pool is opcode-filtered, but schema declares 0x474f86cf"),
    HostCall("HostCoffee.cpp", 110, "CoffeeSwapEvent", (0xC0FFEE30,),
             "event is returned by first_next_call(kCoffeeSwapSuccessfulEvent)"),
    HostCall("HostNft.cpp", 130, "NftOwnershipAssignedPrevOwner", (0x05138D91,),
             "the sole helper caller receives the exact assigned contract capture"),
    HostCall("HostJvault.cpp", 46, "JVaultStakePeriodPayload", (),
             "arbitrary jetton forward_payload is decoded; no payload opcode check exists"),
)


def _schema_opcodes() -> dict[str, int]:
    pattern = re.compile(r"^message\s+(\w+)\s+opcode\s+(0x[0-9a-fA-F]+)", re.MULTILINE)
    return {name: int(opcode, 16) for name, opcode in pattern.findall(SCHEMA.read_text())}


def _walk_statements(value):
    if isinstance(value, dict):
        if value.get("s") == "parse":
            yield value
        for child in value.values():
            yield from _walk_statements(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk_statements(child)


def _reachable(nodes: list[dict], root: int) -> set[int]:
    result: set[int] = set()
    pending = [root]
    while pending:
        index = pending.pop()
        if index in result:
            continue
        result.add(index)
        node = nodes[index]
        for key in ("child", "children", "branches", "step", "exit", "parent"):
            value = node.get(key)
            if isinstance(value, int):
                pending.append(value)
            elif isinstance(value, list):
                pending.extend(item for item in value if isinstance(item, int))
    return result


def _ir_failures(opcodes: dict[str, int]) -> list[DominanceFailure]:
    artifact = json.loads(IR.read_text())
    nodes = artifact["nodes"]
    failures = []
    for matcher in artifact["matchers"]:
        reachable = _reachable(nodes, matcher["root"])
        for statement in _walk_statements(matcher.get("build_program", [])):
            schema_types = [name for name in statement.get("types", []) if name in opcodes]
            if not schema_types:
                continue
            captures = [
                (index, nodes[index])
                for index in reachable
                if nodes[index].get("capture") == statement["target"]
            ]
            type_opcodes = {opcodes[name] for name in schema_types}
            anchored_set = (
                len(captures) == 1
                and captures[0][0] == matcher["root"]
                and captures[0][1].get("kind") == "any"
                and matcher.get("anchor", {}).get("kind") == "opcode_set"
                and set(matcher["anchor"].get("values", [])) == type_opcodes
            )
            for name in schema_types:
                expected = opcodes[name]
                explicit = bool(captures) and all(
                    node.get("kind") == "contract" and node.get("opcode") == expected
                    for _, node in captures
                )
                if explicit or anchored_set:
                    continue
                shapes = ", ".join(
                    f"node {index} {node.get('kind')} opcode={node.get('opcode')}"
                    for index, node in captures
                ) or "no matching capture"
                failures.append(
                    DominanceFailure(
                        name,
                        f"IR {matcher['name']} parse {statement['target']}:{name} wants "
                        f"0x{expected:08x}; {shapes}; anchor={matcher.get('anchor')}",
                    )
                )
    return failures


def _host_failures(opcodes: dict[str, int]) -> list[DominanceFailure]:
    call_re = re.compile(r'parse_message_body\(\s*"([^"]+)"')
    actual = set()
    for path in sorted(HOST.glob("*.cpp")):
        for line_no, line in enumerate(path.read_text().splitlines(), 1):
            for message in call_re.findall(line):
                if message in opcodes:
                    actual.add((path.name, line_no, message))
    reviewed = {(call.file, call.line, call.message) for call in HOST_CALLS}
    assert actual == reviewed, (
        f"host parse call inventory drift; unreviewed={sorted(actual - reviewed)}, "
        f"missing={sorted(reviewed - actual)}"
    )
    failures = []
    for call in HOST_CALLS:
        expected = opcodes[call.message]
        if call.guard_opcodes == (expected,):
            continue
        guards = ",".join(f"0x{op:08x}" for op in call.guard_opcodes) or "none"
        failures.append(
            DominanceFailure(
                call.message,
                f"HOST {call.file}:{call.line} {call.message} wants 0x{expected:08x}; "
                f"dominating guard={guards}; {call.evidence}",
            )
        )
    return failures


def test_schema_parser_opcode_dominance():
    opcodes = _schema_opcodes()
    assert len(opcodes) == 70
    failures = _ir_failures(opcodes) + _host_failures(opcodes)
    failing_names = {failure.message for failure in failures}
    ledger_names = set(KNOWN_NON_DOMINATED)
    unexpected = failing_names - ledger_names
    stale = ledger_names - failing_names

    unexpected_details = [
        failure.detail for failure in failures if failure.message in unexpected
    ]
    stale_details = [f"{name}: {KNOWN_NON_DOMINATED[name]}" for name in sorted(stale)]
    sections = []
    if unexpected_details:
        sections.append("new unledgered failures:\n" + "\n".join(unexpected_details))
    if stale_details:
        sections.append("stale ledger entries:\n" + "\n".join(stale_details))
    assert not unexpected and not stale, (
        "opcode-dominance ratchet drift:\n" + "\n".join(sections)
    )
