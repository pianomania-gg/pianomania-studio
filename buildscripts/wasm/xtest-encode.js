// Cross-language parity helper: encode a deterministic .pm with pm-format.js and
// print a per-section summary (name|type|len|bytesum) that the C# decoder must
// reproduce. Usage: node xtest-encode.js <out.pm>
'use strict';
const fs = require('fs');
const path = require('path');
const PmFormat = require(path.resolve(__dirname, '../../../WebApp/wwwroot/js/pm-format.js'));

function mk(len, seed) { const a = new Uint8Array(len); for (let i = 0; i < len; i++) a[i] = (i * 37 + seed) & 0xff; return a; }
function sum(a) { let s = 0; for (let i = 0; i < a.length; i++) s = (s + a[i]) >>> 0; return s; }

const sections = [
    { name: 'song.mei', type: 'mei', bytes: mk(5000, 3) },
    { name: 'song.mid', type: 'midi-base', bytes: mk(241, 7) },
    { name: 'song-repeats.mid', type: 'midi-repeats', bytes: mk(900, 9) },
    { name: 'manifest.json', type: 'manifest', bytes: mk(120, 1) },
    { name: 'cover.png', type: 'cover', bytes: mk(2048, 5) },
];

fs.writeFileSync(process.argv[2], Buffer.from(PmFormat.encode(sections)));
sections.forEach(s => console.log(`${s.name}|${s.type}|${s.bytes.length}|${sum(s.bytes)}`));
