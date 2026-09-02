// Repro harness for the deferred-callback crash at module init.
// Usage: node idle-test.js <path-to-mscore.js> [idle-ms] [path-to-input.mscz]
//
// Loads the module like the browser does, then keeps the event loop alive so
// any timer scheduled during main() (emscripten_async_call / safeSetTimeout)
// gets a chance to fire. The browser surfaces these as
// "Uncaught RuntimeError: null function" at converter page load.
'use strict';
const fs = require('fs');
const path = require('path');

const jsPath = process.argv[2];
const idleMs = Number(process.argv[3] || 30000);
const msczPath = process.argv[4];

if (!jsPath) {
    console.error('Usage: node idle-test.js <mscore.js> [idle-ms] [input.mscz]');
    process.exit(2);
}

let sawError = false;
process.on('uncaughtException', (e) => {
    sawError = true;
    console.error('UNCAUGHT EXCEPTION:', e && (e.stack || e.message || e));
});
process.on('unhandledRejection', (e) => {
    sawError = true;
    console.error('UNHANDLED REJECTION:', e && (e.stack || e.message || e));
});

const factory = require(path.resolve(jsPath));

(async () => {
    const mod = await factory({
        print: (t) => console.log('[wasm]', t),
        printErr: (t) => {
            console.error('[wasm:err]', t);
            // A fatal abort is reported here; it does not reject the init promise
            // and does not raise an uncaught exception. Without this the harness
            // reports success for a module that has already died.
            if (String(t).includes('Aborted(')) {
                sawError = true;
            }
        },
    });
    console.log(`Module initialized. Idling ${idleMs} ms to let deferred callbacks fire...`);

    if (msczPath) {
        const bytes = new Uint8Array(fs.readFileSync(msczPath));
        const res = mod.pmConvert(bytes);
        if (res && res.ok) {
            console.log('pmConvert: OK');
        } else {
            // A refused conversion has to fail the run, or CI reports success on it.
            sawError = true;
            console.error(`pmConvert: FAILED: ${res && res.error}`);
        }
    }

    setTimeout(() => {
        console.log(sawError ? 'RESULT: FAILED' : 'RESULT: clean — module initialized, no deferred-callback error');
        process.exit(sawError ? 1 : 0);
    }, idleMs);
})().catch((e) => {
    console.error('INIT EXCEPTION:', e && (e.stack || e.message || e));
    process.exit(1);
});
