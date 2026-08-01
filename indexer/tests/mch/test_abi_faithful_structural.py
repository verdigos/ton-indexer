"""F1 structural gate for the unregistered ABI-faithful message declarations."""
from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

from indexer.events.mch.msgs_gen import Field, Message, parse_schema


REPO = Path(__file__).resolve().parents[3]
ABI_DIR = REPO / "ton-index-worker/ton-abi/abi"
SCHEMA = REPO / "ton-index-worker/ton-mch-engine/mch-msgs.schema"
MSG_PARSE = REPO / "ton-index-worker/ton-mch-engine/src/MsgParse.cpp"
ABI_BRIDGE = REPO / "ton-index-worker/ton-mch-engine/src/AbiBridge.cpp"

# This is deliberately a name-to-owner manifest, not a raw struct census:
# JettonInternalTransfer is one logical migrated row with two concrete structs.
LOGICAL_ROWS_BY_STEM = {
    "jetton": (
        "JettonInternalTransfer", "JettonBurn", "JettonNotify", "JettonMint",
    ),
    "multisig": (
        "MultisigNewOrder", "MultisigInitOrder", "MultisigApprove",
        "MultisigApproveRejected", "MultisigExecute",
    ),
    "pton": ("PTonTransfer",),
    "stonfi": (
        "StonfiV2ProvideLiquidity", "StonfiPaymentRequest", "StonfiSwapMessage",
        "StonfiV2PayTo",
    ),
    "dedust": (
        "DedustPayoutFromPool", "DedustSwapNotification",
        "DedustDepositLiquidityToPool",
    ),
    "dedust_v2": (
        "DedustV2PayNative", "DedustV2SwapEvent", "DedustV2PayoutPositionFees",
        "DedustV2Withdraw", "DedustV2WithdrawalEvent", "DedustV2CreditAsset",
        "DedustV2JoinLiquidity", "DedustV2DepositEvent",
    ),
    "coffee": (
        "CoffeeCreateVault", "CoffeeSwapEvent",
        "CoffeeCreateLiquidityDepositoryRequest",
        "CoffeeDepositLiquiditySuccessfulEvent", "CoffeeLiquidityWithdrawalEvent",
        "CoffeeCreatePoolCreatorRequest", "CoffeeCreatePoolRequest",
        "CoffeeStakingClaimRewards", "CoffeeStakingPositionWithdraw2",
    ),
    "coffee_staking_withdraw3": ("CoffeeStakingPositionWithdraw3",),
    "evaa": (
        "EvaaSupplyMaster", "EvaaSupplySuccess", "EvaaWithdrawMaster",
        "EvaaWithdrawCollateralized",
    ),
    "evaa_supply_forward": ("EvaaSupplyJettonForward",),
    "jvault": ("JVaultUnstakeJettons", "JVaultUnstakeRequest", "JVaultClaim"),
    "jvault_payload": ("JVaultStakePeriodPayload",),
    "subscriptions": ("SubscriptionPaymentRequest",),
    "tonco": (
        "ToncoRouterV3SwapSourceWallet", "ToncoPoolV3StartBurn",
        "ToncoPositionNftV3PositionBurn", "ToncoPoolV3Burn",
        "ToncoRouterV3CreatePool", "ToncoPoolV3Init",
    ),
    "teleitem": ("TeleitemStartAuction",),
    "nft_sale": (
        "NftOwnershipAssignedPrevOwner", "SaleUpdateMessage", "NftReportStaticData",
    ),
    "tonstakers": ("TONStakersNftBurnNotification",),
    "cocoon": (
        "CocoonPayoutPayload", "CocoonLastPayoutPayload", "CocoonWorkerProxyRequest",
        "CocoonExtProxyPayoutRequest", "CocoonChargePayload",
        "CocoonGrantRefundPayload", "CocoonExtClientTopUp", "CocoonRegisterProxy",
        "CocoonUnregisterProxy", "CocoonOwnerClientRegister",
        "CocoonOwnerClientChangeSecretHash", "CocoonOwnerClientRequestRefund",
        "CocoonOwnerClientIncreaseStake", "CocoonOwnerClientWithdraw",
    ),
}

ETHENA_ARM = "JettonInternalTransferEthena"
EXTRA_STRUCTS = {"JettonInternalTransfer": (ETHENA_ARM,)}
OPCODE_OVERRIDES = {
    "CoffeeCreateVault": 0xC0FFEE06,
    "DedustPayoutFromPool": 0xAD4EB6F5,
}
ARM_NAMES = {
    "AssetTon", "AssetJetton",
    "JettonForwardPayloadRef", "JettonForwardPayloadInline",
    "PTonForwardPayloadRef", "PTonForwardPayloadInline",
}

SCALAR_TYPES = {
    "bool": "bool", "bytes32": "bits256", "u2": "uint2", "u8": "uint8",
    "u16": "uint16", "u32": "uint32", "u48": "uint48", "u64": "uint64",
    "uint128": "uint128", "uint160": "uint160", "uint256": "uint256",
    "int24": "int24", "coins": "coins", "addr_py": "any_address",
    "ref_cell": "cell", "maybe_ref_cell": "cell?", "asset2": "Asset",
    "asset4": "Asset", "dict_keys267": "map<address, JVaultClaimEntry>",
    "fwd_cell_bits": "JettonForwardTail", "fwd_cell_refs": "PTonForwardTail",
}

STRUCT_RE = re.compile(
    r"\bstruct\s*(?:\((?P<prefix>0x[0-9a-fA-F]+|0b[01]+)\)\s*)?"
    r"(?P<name>[A-Za-z_]\w*)\s*\{(?P<body>.*?)\}",
    re.DOTALL,
)
FIELD_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*:\s*([^;\n]+)\s*;", re.MULTILINE)


def _sources() -> dict[str, str]:
    return {
        stem: (ABI_DIR / f"{stem}.tolk").read_text(encoding="utf-8")
        for stem in LOGICAL_ROWS_BY_STEM
    }


def _messages() -> list[Message]:
    messages = parse_schema(SCHEMA.read_text(encoding="utf-8"))
    assert len(messages) == 70
    return messages


def _logical_to_stem() -> dict[str, str]:
    return {
        name: stem
        for stem, names in LOGICAL_ROWS_BY_STEM.items()
        for name in names
    }


def _structs(text: str) -> dict[str, tuple[str | None, str]]:
    found = {}
    for match in STRUCT_RE.finditer(text):
        name = match.group("name")
        assert name not in found, f"duplicate struct declaration {name}"
        found[name] = (match.group("prefix"), match.group("body"))
    return found


def _fields(body: str) -> list[tuple[str, str]]:
    return [(name, type_.strip()) for name, type_ in FIELD_RE.findall(body)]


def _allowed_arms(text: str) -> list[str]:
    match = re.search(
        r"\btype\s+AllowedMessage\s*=\s*(.*?)\n\s*\n\s*struct\s+Storage\b",
        text,
        re.DOTALL,
    )
    assert match, "missing AllowedMessage union"
    return re.findall(r"[A-Za-z_]\w*", match.group(1))


def _registry_names(path: Path, function: str) -> set[str]:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"\b{re.escape(function)}\(\).*?\brows\s*=\s*\{{(.*?)\n\s*\}};",
        text,
        re.DOTALL,
    )
    assert match, f"could not read {function} rows from {path.name}"
    return set(re.findall(r'\{\s*"([A-Za-z_]\w*)"\s*,', match.group(1)))


def _expected_type(message: Message, field: Field) -> str:
    if field.type == "bits":
        return f"bits{field.bitn}"
    if field.type == "ref":
        return r"Cell<[^>]+>"
    return re.escape(SCALAR_TYPES[field.type])


def test_exactly_70_logical_rows_are_owned_and_unregistered():
    sources = _sources()
    schema_names = {message.name for message in _messages()}
    ownership = _logical_to_stem()
    assert len(ownership) == 70
    assert set(ownership) == schema_names

    hand_names = _registry_names(MSG_PARSE, "hand_message_parsers")
    existing_abi_names = _registry_names(ABI_BRIDGE, "abi_message_parsers")
    assert len(hand_names) == 13
    assert len(existing_abi_names) == 3
    assert schema_names.isdisjoint(hand_names)
    assert schema_names.isdisjoint(existing_abi_names)

    incoming_rows = [
        name
        for stem, text in sources.items()
        for name in _allowed_arms(text)
        if stem in LOGICAL_ROWS_BY_STEM
    ]
    incoming = set(incoming_rows)
    assert incoming.isdisjoint(hand_names)
    migrated_incoming = incoming - existing_abi_names - {ETHENA_ARM}
    assert migrated_incoming == schema_names
    assert len(incoming - existing_abi_names) == 71
    migrated_counts = Counter(
        name for name in incoming_rows if name not in existing_abi_names and name != ETHENA_ARM
    )
    assert migrated_counts == Counter({name: 1 for name in schema_names})

    for name, stem in ownership.items():
        assert name in _allowed_arms(sources[stem])
        assert name in _structs(sources[stem])


def test_every_schema_field_is_visible_in_wire_order_and_has_faithful_type():
    sources = _sources()
    ownership = _logical_to_stem()
    for message in _messages():
        structs = _structs(sources[ownership[message.name]])
        _, body = structs[message.name]
        actual = _fields(body)
        if message.name == "JVaultStakePeriodPayload":
            assert actual == [("payload_opcode", "uint32"), ("stake_period", "uint32")]
            continue
        assert [name for name, _ in actual] == [field.name for field in message.fields]
        for (name, actual_type), field in zip(actual, message.fields):
            assert name == field.name
            assert re.fullmatch(_expected_type(message, field), actual_type), (
                f"{message.name}.{name}: {actual_type}"
            )

    assert "signers_hash: bits256;" in sources["multisig"]
    assert "type JVaultClaimEntry = RemainingBitsAndRefs" in sources["jvault"]
    assert "jettons_to_claim: map<address, JVaultClaimEntry>;" in sources["jvault"]
    jetton_structs = _structs(sources["jetton"])
    assert _fields(jetton_structs[ETHENA_ARM][1]) == _fields(
        jetton_structs["JettonInternalTransfer"][1]
    )


def test_opcode_census_includes_all_frozen_corrections():
    sources = _sources()
    ownership = _logical_to_stem()
    schema = {message.name: message for message in _messages()}

    for name, message in schema.items():
        structs = _structs(sources[ownership[name]])
        prefix, _ = structs[name]
        if name == "JVaultStakePeriodPayload":
            assert prefix is None
            continue
        expected = OPCODE_OVERRIDES.get(name, message.opcode)
        assert prefix is not None
        assert int(prefix, 0) == expected, name

    jetton_structs = _structs(sources["jetton"])
    prefixes = {
        int(jetton_structs[name][0], 0)
        for name in ("JettonInternalTransfer", ETHENA_ARM)
    }
    assert prefixes == {0x178D4519, 0xB2583ED5}
    assert int(_structs(sources["coffee"])["CoffeeCreateVault"][0], 0) == 0xC0FFEE06
    assert "struct (0x00000000) CoffeeCreateVault" not in sources["coffee"]
    assert int(_structs(sources["dedust"])["DedustPayoutFromPool"][0], 0) == 0xAD4EB6F5
    assert "struct (0x474f86cf) DedustPayoutFromPool" not in sources["dedust"]
    assert re.search(r"\bstruct\s+JVaultStakePeriodPayload\s*\{", sources["jvault_payload"])
    assert not re.search(r"\bstruct\s*\([^)]*\)\s+JVaultStakePeriodPayload", sources["jvault_payload"])
    assert "0x9c69f376" not in sources["cocoon"]


def test_b4_union_arm_names_and_shapes_are_exact():
    sources = _sources()
    all_text = "\n".join(sources.values())
    family_spellings = set(re.findall(
        r"\b(?:Asset(?:Ton|Jetton)\w*|(?:Jetton|PTon)ForwardPayload\w*)\b",
        all_text,
    ))
    assert family_spellings == ARM_NAMES
    assert "MchLegacy" not in all_text
    assert "Flatten" not in all_text

    for stem in ("coffee", "dedust"):
        structs = _structs(sources[stem])
        assert _fields(structs["AssetTon"][1]) == []
        assert _fields(structs["AssetJetton"][1]) == [
            ("workchain", "uint8"), ("hash", "bits256"),
        ]
        assert re.search(r"type\s+Asset\s*=\s*AssetTon\s*\|\s*AssetJetton", sources[stem])

    coffee_structs = _structs(sources["coffee"])
    dedust_structs = _structs(sources["dedust"])
    assert coffee_structs["AssetTon"][0] == "0b00"
    assert coffee_structs["AssetJetton"][0] == "0b01"
    assert dedust_structs["AssetTon"][0] == "0b0000"
    assert dedust_structs["AssetJetton"][0] == "0b0001"

    for stem, alias, ref_arm, inline_arm in (
        ("jetton", "JettonForwardTail", "JettonForwardPayloadRef", "JettonForwardPayloadInline"),
        ("pton", "PTonForwardTail", "PTonForwardPayloadRef", "PTonForwardPayloadInline"),
    ):
        structs = _structs(sources[stem])
        assert _fields(structs[ref_arm][1]) == [("value", "cell")]
        assert _fields(structs[inline_arm][1]) == [("value", "RemainingBitsAndRefs")]
        assert re.search(
            rf"type\s+{alias}\s*=\s*{ref_arm}\s*\|\s*{inline_arm}", sources[stem]
        )


def test_every_live_schema_ref_is_one_named_cell_hop():
    sources = _sources()
    ownership = _logical_to_stem()
    ref_count = 0
    for message in _messages():
        structs = _structs(sources[ownership[message.name]])
        parent_fields = dict(_fields(structs[message.name][1]))
        for field in message.fields:
            if field.type != "ref":
                continue
            ref_count += 1
            match = re.fullmatch(r"Cell<([A-Za-z_]\w*)>", parent_fields[field.name])
            assert match, f"{message.name}.{field.name} is not a named Cell<T>"
            inner_name = match.group(1)
            assert inner_name in structs
            inner_fields = _fields(structs[inner_name][1])
            assert [name for name, _ in inner_fields] == [child.name for child in field.sub]
            for (name, actual_type), child in zip(inner_fields, field.sub):
                assert name == child.name
                assert re.fullmatch(_expected_type(message, child), actual_type), (
                    f"{message.name}.{field.name}.{name}: {actual_type}"
                )
            assert all("Cell<" not in type_ for _, type_ in inner_fields)
    assert ref_count == 13


def test_message_opcodes_are_unique_within_each_protocol_file():
    sources = _sources()
    for stem, logical_names in LOGICAL_ROWS_BY_STEM.items():
        structs = _structs(sources[stem])
        concrete = list(logical_names)
        for logical_name in logical_names:
            concrete.extend(EXTRA_STRUCTS.get(logical_name, ()))
        prefixed = [structs[name][0] for name in concrete if structs[name][0] is not None]
        normalized = [int(prefix, 0) for prefix in prefixed]
        duplicates = {opcode for opcode, count in Counter(normalized).items() if count > 1}
        assert not duplicates, f"{stem}: duplicate opcodes {sorted(duplicates)}"

    assert _structs(sources["evaa"])["EvaaSupplyMaster"][0] == "0x00000001"
    assert _structs(sources["evaa_supply_forward"])["EvaaSupplyJettonForward"][0] == "0x00000001"
    assert _structs(sources["coffee"])["CoffeeStakingPositionWithdraw2"][0] == "0xcb03bfaf"
    assert _structs(sources["coffee_staking_withdraw3"])["CoffeeStakingPositionWithdraw3"][0] == "0xcb03bfaf"


def test_generated_abi_incoming_rows_when_artifacts_exist():
    sources = _sources()
    abi_paths = {stem: ABI_DIR / f"{stem}.abi.json" for stem in LOGICAL_ROWS_BY_STEM}
    if not all(path.is_file() for path in abi_paths.values()):
        return

    def body_struct_name(abi, body_ty_idx):
        body_type = abi["unique_types"][body_ty_idx]
        if body_type["kind"] == "StructRef":
            return body_type["struct_name"]

        alias = next(
            declaration
            for declaration in abi["declarations"]
            if declaration["kind"] == "alias"
            and declaration["name"] == body_type["alias_name"]
        )
        struct = next(
            declaration
            for declaration in abi["declarations"]
            if declaration["kind"] == "struct"
            and declaration["ty_idx"] == alias["target_ty_idx"]
        )
        return struct["name"]

    for stem, path in abi_paths.items():
        abi = json.loads(path.read_text(encoding="utf-8"))
        incoming_names = {
            body_struct_name(abi, row["body_ty_idx"])
            for row in abi["incoming_messages"]
        }
        assert incoming_names == set(_allowed_arms(sources[stem]))
