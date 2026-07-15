// Dev-only oracle (§0.15). Compiles Tolk fixtures via tolk-js, computes ABI JSON
// + golden pack/unpack vectors via the reference @ton/tolk-abi-to-typescript
// dynamic-serialization path, and dumps everything byte-stably for the C++
// VectorRunner (T0.3) to consume later (value/hex vectors enable at wave W8;
// nothing is enabled yet at W0 — see enabled_from below).
//
// SCOPE (W0): 3 fixtures (tolk_counter, lots-of-messages, lots-of-wrappers),
// scalar-only struct vectors (intN/uintN/coins fields). No unions, addresses,
// cells, arrays, generics or custom serializers are exercised here — those
// need the full value-JSON codec + registry seam, both out of scope until
// W1 (loader), W2 (kernel/generics) and W8/W9 (interpreter + custom registry).
//
// Value-JSON schema: §0.14 of docs/ton-abi-plan.md. This script implements the
// SUBSET of that schema needed for intN/uintN/coins/bool + plain structs.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

import { runTolkCompiler } from '@ton/tolk-js';
import { DynamicCtx, packToBuilderDynamic, unpackFromSliceDynamic } from '@ton/tolk-abi-to-typescript';
import { beginCell } from '@ton/core';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const FIXTURES_SRC = 'S:/Dev/tolk-abi-to-typescript/input';
const FIXTURES_DIR = path.join(ROOT, 'testdata', 'fixtures');
const VECTORS_FILE = path.join(ROOT, 'testdata', 'abi_vectors.json');

const tolkAbiPkg = require('@ton/tolk-abi-to-typescript/package.json');
const tolkJsPkg = require('@ton/tolk-js/package.json');
const corePkg = require('@ton/core/package.json');

async function compileTolk(absFileName) {
    const r = await runTolkCompiler({
        entrypointFileName: absFileName,
        fsReadCallback: p => fs.readFileSync(p, 'utf-8'),
    });
    if (r.status === 'error') {
        throw new Error(`tolk-js compile failed for ${absFileName}: ${r.message}`);
    }
    const abiJson = r.abiJson;
    abiJson.code_boc64 = r.codeBoc64;
    return abiJson;
}

// ---- canonical value-JSON codec: SUBSET of §0.14 (intN/uintN/coins/bool + struct) ----

function findStructDecl(abi, structName) {
    const decl = abi.declarations.find(d => d.kind === 'struct' && d.name === structName);
    if (!decl) throw new Error(`struct '${structName}' not found in declarations`);
    return decl;
}

function toValueJson(abi, tyIdx, v) {
    const ty = abi.unique_types[tyIdx];
    switch (ty.kind) {
        case 'intN': case 'uintN': case 'coins': case 'varintN': case 'varuintN':
            return v.toString();
        case 'bool':
            return v;
        case 'StructRef': {
            const decl = findStructDecl(abi, ty.struct_name);
            const out = { $: decl.name };
            for (const f of decl.fields) {
                out[f.name] = toValueJson(abi, f.ty_idx, v[f.name]);
            }
            return out;
        }
        default:
            throw new Error(`toValueJson: unsupported kind '${ty.kind}' (out of W0 scope, see file header)`);
    }
}

function fromValueJson(abi, tyIdx, j) {
    const ty = abi.unique_types[tyIdx];
    switch (ty.kind) {
        case 'intN': case 'uintN': case 'coins': case 'varintN': case 'varuintN':
            return BigInt(j);
        case 'bool':
            return j;
        case 'StructRef': {
            const decl = findStructDecl(abi, ty.struct_name);
            const out = { $: decl.name };
            for (const f of decl.fields) {
                out[f.name] = fromValueJson(abi, f.ty_idx, j[f.name]);
            }
            return out;
        }
        default:
            throw new Error(`fromValueJson: unsupported kind '${ty.kind}'`);
    }
}

function packHex(ctx, structTyIdx, obj) {
    const b = beginCell();
    packToBuilderDynamic(ctx, structTyIdx, obj, b);
    const cell = b.endCell();
    return cell.beginParse().toString();
}

function roundTripCheckOrThrow(ctx, abi, structTyIdx, obj, expectHex) {
    // (a) direct pack must equal expected golden hex
    const hex1 = packHex(ctx, structTyIdx, obj);
    if (hex1 !== expectHex) {
        throw new Error(`golden hex mismatch: got ${hex1} want ${expectHex}`);
    }
    // (a) value-JSON must be a lossless round-trip: json -> object -> pack -> same hex
    const json = toValueJson(abi, structTyIdx, obj);
    const reparsed = fromValueJson(abi, structTyIdx, json);
    const hex2 = packHex(ctx, structTyIdx, reparsed);
    if (hex2 !== expectHex) {
        throw new Error(`round-trip hex mismatch: got ${hex2} want ${expectHex} (value-JSON not lossless: ${JSON.stringify(json)})`);
    }
    // dynamic unpack must agree too (sanity against the reference lib itself)
    const s = beginCell().store(bb => packToBuilderDynamic(ctx, structTyIdx, obj, bb)).endCell().beginParse();
    const unpacked = unpackFromSliceDynamic(ctx, structTyIdx, s);
    if (s.remainingBits !== 0 || s.remainingRefs !== 0) {
        throw new Error(`unpack did not consume slice fully for ${JSON.stringify(json)}`);
    }
    return json;
}

async function main() {
    fs.mkdirSync(FIXTURES_DIR, { recursive: true });

    const fixtureNames = ['tolk_counter', 'lots-of-messages', 'lots-of-wrappers'];
    const fixtureFiles = {
        'tolk_counter': 'tolk_counter.tolk',
        'lots-of-messages': 'lots-of-messages.tolk',
        'lots-of-wrappers': 'lots-of-wrappers.tolk',
    };

    const vectors = [];
    const abiByFixture = {};

    for (const fixture of fixtureNames) {
        const srcFile = path.join(FIXTURES_SRC, fixtureFiles[fixture]);
        const dstFile = path.join(FIXTURES_DIR, fixtureFiles[fixture]);
        fs.copyFileSync(srcFile, dstFile);

        const abi = await compileTolk(srcFile);
        abiByFixture[fixture] = abi;
        if (abi.abi_schema_version !== '1.0') {
            throw new Error(`unexpected abi_schema_version '${abi.abi_schema_version}' for ${fixture} (locked decision 15 pins exactly "1.0")`);
        }
        fs.writeFileSync(
            path.join(FIXTURES_DIR, `${fixture}.abi.json`),
            JSON.stringify(abi, null, 2) + '\n',
        );

        const ctx = new DynamicCtx(abi);

        if (fixture === 'tolk_counter') {
            const tyIdx = ctx.symbols.getStruct('IncreaseCounter').ty_idx;
            for (const val of [
                { queryId: 0n, increaseBy: 1n },
                { queryId: 123456789n, increaseBy: 4294967295n },
            ]) {
                const hex = packHex(ctx, tyIdx, val);
                const json = roundTripCheckOrThrow(ctx, abi, tyIdx, val, hex);
                vectors.push({ fixture, struct: 'IncreaseCounter', enabled_from: 'W8', value: json, golden_hex: hex });
            }
            const tyIdxReset = ctx.symbols.getStruct('ResetCounter').ty_idx;
            for (const val of [{ queryId: 0n }, { queryId: 18446744073709551615n }]) {
                const hex = packHex(ctx, tyIdxReset, val);
                const json = roundTripCheckOrThrow(ctx, abi, tyIdxReset, val, hex);
                vectors.push({ fixture, struct: 'ResetCounter', enabled_from: 'W8', value: json, golden_hex: hex });
            }
        }

        if (fixture === 'lots-of-messages') {
            const tyIdx = ctx.symbols.getStruct('IncreaseBy').ty_idx;
            for (const val of [{ counter_id: 0n, inc_by: 1n }, { counter_id: -5n, inc_by: 100n }]) {
                const hex = packHex(ctx, tyIdx, val);
                const json = roundTripCheckOrThrow(ctx, abi, tyIdx, val, hex);
                vectors.push({ fixture, struct: 'IncreaseBy', enabled_from: 'W8', value: json, golden_hex: hex });
            }
            const tyIdxEmpty = ctx.symbols.getStruct('EmptyMsg').ty_idx;
            const hexEmpty = packHex(ctx, tyIdxEmpty, {});
            const jsonEmpty = roundTripCheckOrThrow(ctx, abi, tyIdxEmpty, {}, hexEmpty);
            vectors.push({ fixture, struct: 'EmptyMsg', enabled_from: 'W8', value: jsonEmpty, golden_hex: hexEmpty });
        }

        if (fixture === 'lots-of-wrappers') {
            // literal golds transcribed from tests/LotsOfWrappers.spec.ts (T0.2 self-validation (b))
            const literalCases = [
                { struct: 'MsgSinglePrefix32', val: { amount1: 80n, amount2: 800000000n }, hex: 'x{8765432115042FAF0800}' },
                { struct: 'CounterIncrement', val: { counter_id: 123n, inc_by: 78n }, hex: 'x{123456787B0000004E}' },
                { struct: 'CounterDecrement', val: { counter_id: 0n, dec_by: -38n }, hex: 'x{2345678900FFFFFFDA}' },
                { struct: 'CounterReset0', val: { counter_id: 0n }, hex: 'x{3456789000}' },
                { struct: 'CounterResetTo', val: { counter_id: 0n, initial_value: 29874329774732n }, hex: 'x{001843000000001B2BA8D06A8C}' },
            ];
            for (const { struct, val, hex } of literalCases) {
                const tyIdx = ctx.symbols.getStruct(struct).ty_idx;
                const json = roundTripCheckOrThrow(ctx, abi, tyIdx, val, hex);
                vectors.push({ fixture, struct, enabled_from: 'W8', value: json, golden_hex: hex, source: 'LotsOfWrappers.spec.ts literal gold' });
            }
        }
    }

    const output = {
        header: {
            pkg_version: tolkAbiPkg.version,
            tolk_js_version: tolkJsPkg.version,
            core_version: corePkg.version,
            compiler_version: abiByFixture['tolk_counter'].compiler_version,
            abi_schema_version: '1.0',
        },
        vectors,
    };

    fs.writeFileSync(VECTORS_FILE, JSON.stringify(output, null, 2) + '\n');
    console.log(`wrote ${vectors.length} vectors to ${VECTORS_FILE}`);
}

main().catch(e => { console.error(e); process.exit(1); });
