// Inspect a .pm file: decode the container, list its sections, and verify the
// embedded ownership license (signature + MEI hash binding + optional uid check).
// This mirrors what the game's PmLicense.cs does, so it's the fastest way to test
// the Composer -> .pm -> license pipeline without a game build.
//
// Usage:
//   node pm-inspect.js <file.pm> [--uid <firebase-uid>] [--pubkey <spki.pem>]
//
// Exit code: 0 if the container decodes and (when a license is present) the
// signature + hash verify; 1 otherwise.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const PmFormat = require(path.resolve(__dirname, '../../../WebApp/wwwroot/js/pm-format.js'));

// SPKI (base64 DER) of the license verification key. Must correspond to the
// WebApp's Composer:LicenseSigningKey and the game's PmLicense.cs PublicKeys.
// dev/beta key, generated 2026-06-09 — rotate before GA.
const LICENSE_SPKI_B64 =
    'MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA2wbDxLSupDdo8o6sVZbeaoSjuyp43cZ4CEdQqldEpLIWVzv4S4lqMqqekCqO8eSRCaa5e3AaSWugoKncF+itNK2hzH1yMxdh4zG2sCZuzOAZ14Mz1sFNzPwBOD6SGqowXQRXMVKyyK260fCfDrhMr1rXxzhifKaLuAI6flXS98a35zMaZPbCZUDRPD6Ta9HrBydNu4OG7ODxBPZZzMWhugJCkCdtMKI6kdmzc1cYqvE9zxV6rSHZYBIWtvQfcLQX7ZNBZYfC3+xbpQHXy/LIKDsrrglfP7fL1st9T650kQwReAKze+Uu08rYKUFZECaN3LY0BcBWGp/41I2+KTq83QIDAQAB';

function arg(flag) {
    const i = process.argv.indexOf(flag);
    return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : null;
}

const file = process.argv[2];
if (!file || file.startsWith('--')) {
    console.error('Usage: node pm-inspect.js <file.pm> [--uid <firebase-uid>] [--pubkey <spki.pem>]');
    process.exit(1);
}

let fail = 0;
function check(cond, msg) { console.log((cond ? 'ok  ' : 'FAIL') + ' - ' + msg); if (!cond) fail++; }

const pm = PmFormat.decode(new Uint8Array(fs.readFileSync(file)));
console.log(`version ${pm.version}, ${pm.sections.length} section(s):`);
for (const s of pm.sections) {
    console.log(`  ${s.type.padEnd(13)} ${s.name.padEnd(28)} ${s.bytes.length} bytes`);
}
console.log('');

const license = pm.sections.find(s => s.type === 'license');
const mei = pm.sections.find(s => s.type === 'mei');

if (!license) {
    console.log('No license section — this .pm will NOT play in the game.');
    process.exit(1);
}

const pubkeyPath = arg('--pubkey');
const publicKey = pubkeyPath
    ? crypto.createPublicKey(fs.readFileSync(pubkeyPath, 'utf8'))
    : crypto.createPublicKey({ key: Buffer.from(LICENSE_SPKI_B64, 'base64'), format: 'der', type: 'spki' });

let envelope = null;
try { envelope = JSON.parse(Buffer.from(license.bytes).toString('utf8')); } catch (_) { /* checked below */ }
check(!!(envelope && envelope.payload && envelope.sig), 'license parses as {payload, sig} envelope');
if (!envelope || !envelope.payload || !envelope.sig) process.exit(1);

const payloadBytes = Buffer.from(envelope.payload, 'base64');
const sigOk = crypto.createVerify('RSA-SHA256')
    .update(payloadBytes)
    .verify(publicKey, Buffer.from(envelope.sig, 'base64'));
check(sigOk, 'RSA-2048 PKCS#1 v1.5 SHA-256 signature verifies');

let payload = null;
try { payload = JSON.parse(payloadBytes.toString('utf8')); } catch (_) { /* checked below */ }
check(!!(payload && payload.v && Array.isArray(payload.uids) && payload.uids.length > 0 && payload.fileHash),
    'payload has {v, uids[], fileHash}');

if (payload) {
    console.log(`     v=${payload.v} issuedAt=${payload.issuedAt}`);
    console.log(`     uids: ${payload.uids.join(', ')}`);

    if (mei) {
        const meiHash = crypto.createHash('sha256').update(Buffer.from(mei.bytes)).digest('hex');
        check(meiHash === String(payload.fileHash).toLowerCase(), 'fileHash matches SHA-256 of the MEI section');
    } else {
        check(false, 'container has an MEI section to hash');
    }

    const uid = arg('--uid');
    if (uid) {
        check(payload.uids.includes(uid) || payload.uids.includes('*'), `uid '${uid}' is licensed to play this file`);
    }
}

process.exit(fail ? 1 : 0);
