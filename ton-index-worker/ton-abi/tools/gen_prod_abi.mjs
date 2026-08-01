// Compiles the PRODUCTION ABI declarations in ton-abi/abi/*.tolk into their
// committed *.abi.json siblings. These are hand-authored declaration files
// (plan §0.11) describing real on-chain message families -- not test fixtures,
// and not copied from the reference repo -- so this script needs only the Tolk
// compiler (@ton/tolk-js). Unlike tools/gen_vectors.mjs it does NOT need the
// reference-repo checkout or the @ton/tolk-abi-to-typescript oracle.
//
// The committed .abi.json is what ton-abi-gen consumes into the committed
// generated/<name>_gen.{h,cpp} pair; `ton-abi-gen --check` (ctest
// ton_abi_gen_check) proves the pair still regenerates byte-identically from
// it, so this script only has to reproduce the .abi.json itself.
//
// The set of declarations is the explicit PROD_DECLS list below (NOT a glob),
// so scaffolding files like abi/_TEMPLATE.tolk are never compiled.
//
//   node gen_prod_abi.mjs            # recompile every PROD_DECLS entry
//   node gen_prod_abi.mjs --check    # fail if any committed .abi.json is stale

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { runTolkCompiler } from '@ton/tolk-js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ABI_DIR = path.join(path.resolve(__dirname, '..'), 'abi');

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

// The PRODUCTION declaration files, by stem. Adding protocol #2 = ONE entry
// here. An explicit list (rather than a glob of abi/*.tolk) keeps scaffolding
// files such as _TEMPLATE.tolk out of the compile+--check set.
const PROD_DECLS = [
    'jetton',
    'multisig',
    'pton',
    'stonfi',
    'dedust',
    'dedust_v2',
    'coffee',
    'coffee_staking_withdraw3',
    'evaa',
    'evaa_supply_forward',
    'jvault',
    'jvault_payload',
    'subscriptions',
    'tonco',
    'teleitem',
    'nft_sale',
    'tonstakers',
    'cocoon',
];

async function main() {
    const check = process.argv.includes('--check');
    if (PROD_DECLS.length === 0) {
        throw new Error('PROD_DECLS is empty -- nothing to compile');
    }

    let stale = 0;
    for (const stem of PROD_DECLS) {
        const declFile = `${stem}.tolk`;
        const declPath = path.join(ABI_DIR, declFile);
        if (!fs.existsSync(declPath)) {
            throw new Error(`PROD_DECLS lists '${stem}' but ${declPath} does not exist`);
        }
        const abi = await compileTolk(declPath);
        // Locked decision 15 pins the schema version exactly.
        if (abi.abi_schema_version !== '1.0') {
            throw new Error(`unexpected abi_schema_version '${abi.abi_schema_version}' for ${stem}`);
        }
        const text = JSON.stringify(abi, null, 2) + '\n';
        const outPath = path.join(ABI_DIR, `${stem}.abi.json`);

        if (check) {
            const committed = fs.existsSync(outPath) ? fs.readFileSync(outPath, 'utf-8') : null;
            if (committed !== text) {
                console.error(`STALE: ${stem}.abi.json does not match a fresh compile of ${declFile}`);
                stale++;
            } else {
                console.log(`ok: ${stem}.abi.json (contract_name=${abi.contract_name})`);
            }
            continue;
        }

        fs.writeFileSync(outPath, text);
        console.log(`compiled ${declFile} -> ${stem}.abi.json (contract_name=${abi.contract_name})`);
    }

    if (stale > 0) {
        process.exitCode = 1;
    }
}

main().catch(e => {
    console.error(e);
    process.exit(1);
});
