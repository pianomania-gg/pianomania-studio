// Headless node smoke test for the wasm converter.
// Usage: node smoketest.js <path-to-mscore.js> <path-to-input.mscz>
//
// Loads the MODULARIZE'd module, calls the Embind pmConvert() on the .mscz bytes,
// and validates the complete coordinated bundle. Exits non-zero on conversion,
// schema, file-role, MIDI, or locator failure.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const {
    readMuseScoreExportManifest,
} = require('../../../scripts/pianomania-export-manifest.cjs');

const jsPath = process.argv[2];
const msczPath = process.argv[3];
if (!jsPath || !msczPath) {
    console.error('Usage: node smoketest.js <mscore.js> <input.mscz>');
    process.exit(2);
}

const absoluteJsPath = path.resolve(jsPath);
const jsDirectory = path.dirname(absoluteJsPath);
const adjacentWasmPath = path.join(
    jsDirectory,
    `${path.basename(absoluteJsPath, path.extname(absoluteJsPath))}.wasm`
);
const deployedWasmPath = path.join(jsDirectory, 'pm-converter.wasm');
const factory = require(absoluteJsPath);

(async () => {
    const mod = await factory({
        // Quiet, and surface aborts loudly.
        print: (t) => console.log('[wasm]', t),
        printErr: (t) => console.error('[wasm:err]', t),
        locateFile: (requested) => {
            if (!requested.endsWith('.wasm')) {
                return path.join(jsDirectory, requested);
            }
            return fs.existsSync(adjacentWasmPath) ? adjacentWasmPath : deployedWasmPath;
        },
    });

    const bytes = new Uint8Array(fs.readFileSync(msczPath));
    console.log(`Input: ${msczPath} (${bytes.length} bytes)`);

    const res = mod.pmConvert(bytes);
    if (!res || !res.ok) {
        console.error('CONVERT FAILED:', res && res.error);
        process.exit(1);
    }

    const requestedOutDir = process.argv[4];
    const outDir = requestedOutDir
        ? path.resolve(requestedOutDir)
        : fs.mkdtempSync(path.join(os.tmpdir(), 'pmwasm-v2-'));
    fs.mkdirSync(outDir, {recursive: true});
    const outputNames = new Set();
    console.log('CONVERT OK. Output files:');
    for (const f of res.files) {
        if (!f || typeof f.name !== 'string' || path.basename(f.name) !== f.name
            || outputNames.has(f.name)) {
            throw new Error(`Invalid or duplicate converter output name: ${f && f.name}`);
        }
        outputNames.add(f.name);
        console.log(`  ${f.name}  ${f.bytes.length} bytes`);
        fs.writeFileSync(path.join(outDir, f.name), Buffer.from(f.bytes));
    }

    if (!outputNames.has('manifest.json')) {
        throw new Error('Converter output is missing manifest.json.');
    }
    const validated = await readMuseScoreExportManifest(
        path.join(outDir, 'manifest.json'),
        outDir
    );
    const declaredNames = new Set([
        path.basename(validated.meiPath),
        ...validated.files.map((file) => path.basename(file.localPath)),
    ]);
    for (const name of declaredNames) {
        if (!outputNames.has(name)) {
            throw new Error(`Manifest declares an output that pmConvert did not return: ${name}`);
        }
    }
    console.log(
        `BUNDLE VALID: ${validated.schemaVersion}; ${validated.files.length} MIDI variant(s); all locators replayed.`
    );
    if (!requestedOutDir) {
        fs.rmSync(outDir, {recursive: true, force: true});
    }
    process.exit(0);
})().catch((e) => {
    console.error('EXCEPTION:', e && (e.stack || e.message || e));
    process.exit(1);
});
