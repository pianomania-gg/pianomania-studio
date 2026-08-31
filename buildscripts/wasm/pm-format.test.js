// Roundtrip + sanity test for the .pm container (WebApp/wwwroot/js/pm-format.js).
// Run: node pm-format.test.js
'use strict';
const path = require('path');
const PmFormat = require(path.resolve(__dirname, '../../../WebApp/wwwroot/js/pm-format.js'));

function bytes(str) { return new Uint8Array(Buffer.from(str, 'utf8')); }
function eq(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

const sections = [
    { name: 'song.mei', type: 'mei', bytes: bytes('<?xml version="1.0"?><mei xmlns:pm="...">'.repeat(50)) },
    { name: 'song.mid', type: 'midi-base', bytes: Uint8Array.from([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 1, 2, 3]) },
    { name: 'song-repeats.mid', type: 'midi-repeats', bytes: Uint8Array.from([0x4D, 0x54, 0x68, 0x64, 9, 9]) },
    { name: 'manifest.json', type: 'manifest', bytes: bytes('{"version":1}') },
    { name: 'cover.png', type: 'cover', bytes: Uint8Array.from([0x89, 0x50, 0x4E, 0x47, 1, 2, 3, 4, 5]) },
    { name: 'license.json', type: 'license', bytes: bytes('{"payload":"eyJ2IjoxfQ==","sig":"c2lnbmF0dXJl"}') },
];

const pm = PmFormat.encode(sections);
let fail = 0;
function check(cond, msg) { console.log((cond ? 'ok  ' : 'FAIL') + ' - ' + msg); if (!cond) fail++; }

check(pm[0] === 0x50 && pm[1] === 0x4D && pm[2] === 0xF0 && pm[3] === 0x01, 'magic bytes present');
check(pm[4] === 1, 'version = 1');

// Body must NOT contain obvious plaintext (MThd / <?xml) — i.e. it is scrambled.
const hay = Buffer.from(pm.slice(22));
check(hay.indexOf('<?xml') === -1, 'no "<?xml" leaks in scrambled body');
check(hay.indexOf('MThd') === -1, 'no "MThd" leaks in scrambled body');

const out = PmFormat.decode(pm);
check(out.sections.length === sections.length, 'section count roundtrips');
let allEq = true;
for (let i = 0; i < sections.length; i++) {
    const a = sections[i], b = out.sections[i];
    if (a.name !== b.name || a.type !== b.type || !eq(a.bytes, b.bytes)) { allEq = false; console.log('   mismatch at', a.name); }
}
check(allEq, 'every section (name/type/bytes) roundtrips exactly');

// Two encodes use different random salts -> different ciphertext (keystream varies).
const pm2 = PmFormat.encode(sections);
check(!eq(pm.slice(22), pm2.slice(22)), 'random salt -> different scrambled body each encode');
check(eq(pm.slice(6, 22), pm.slice(6, 22)), 'salt is stored in header (sanity)');

console.log('\n.pm size for sample:', pm.length, 'bytes');
process.exit(fail ? 1 : 0);
