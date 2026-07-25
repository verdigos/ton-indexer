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
//   node gen_prod_abi.mjs            # recompile every ton-abi/abi/*.tolk
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

async function main() {
    const check = process.argv.includes('--check');
    const decls = fs.readdirSync(ABI_DIR).filter(f => f.endsWith('.tolk')).sort();
    if (decls.length === 0) {
        throw new Error(`no .tolk declarations found in ${ABI_DIR}`);
    }

    let stale = 0;
    for (const declFile of decls) {
        const stem = declFile.slice(0, -'.tolk'.length);
        const abi = await compileTolk(path.join(ABI_DIR, declFile));
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
