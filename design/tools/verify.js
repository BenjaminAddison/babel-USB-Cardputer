// Cross-check the screen's BABEL-70 maths against the repo's own algorithms.
const B = require('../babel-screen.js');
const enc = B.ENCODINGS.b70;
// 1) file-to-path.js, verbatim logic (74-entry array, % 70)
const alphabet = ['A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z','0','1','2','3','4','5','6','7','8','9','!',' ','&','(',')','-','_','+','[',']','^','='];
function fileToPath(bytes) {
  let index = 0n;
  for (let i = 0n; i < BigInt(bytes.length); i++) index += (BigInt(bytes[i]) + 1n) * (256n ** i);
  let path = '';
  while (index) { index--; let handle = index % 4900n; for (let i = 0; i < 2; i++) { path = alphabet[Number(handle % 70n)] + path; handle /= 70n; } path = '/' + path; index /= 4900n; }
  return path;
}
// 2) usb_mtp_impl.c.h fs_append_output(), byte-for-byte port
function cAppend(out, len, value) {
  const inc = () => { for (let i = 0; i < len.v; i++) { let j = i; while (1) { if (out[j] === 255) out[j] = 0; else { out[j]++; break; } if (++j > len.v) { out[j] = 0; len.v = j; } } } };
  const mul = (v) => { let index = 0, carry = 0; while (index < len.v || carry) { const d = out[index] * v + carry; out[index] = d & 255; carry = d >>> 8; if (++index > len.v) { out[index] = 0; len.v = index; } } };
  const add = (v) => { let index = 0, carry = 0; while (v || carry) { const d = (v & 255) + out[index] + carry; out[index] = d & 255; carry = d >> 8; v >>>= 8; if (++index > len.v) { out[index] = 0; len.v = index; } } };
  const dec = () => { for (let i = 0; i < len.v - 1; i++) { let j = i; while (1) { if (out[j] === 0) out[j] = 255; else { out[j]--; break; } j++; } } if (out[len.v - 1] === 0) len.v--; else out[len.v - 1]--; };
  inc(); mul(4900); add(value); dec();
}
let ok = true;
for (const text of ['Hello', 'Help', 'BABEL', 'every file', 'A', '\u0000', 'The Library is unlimited and cyclical.']) {
  const bytes = B.textBytes(text);
  const p = enc.pathFor(bytes);
  const shown = '/' + p.map(enc.name).join('/');
  const ref = fileToPath(bytes);
  const out = new Array(4096).fill(0), len = { v: 0 };
  for (const h of p) cAppend(out, len, h);
  const cBytes = out.slice(0, len.v);
  const back = enc.content(p);
  const same = shown === ref && JSON.stringify(cBytes) === JSON.stringify(bytes) && JSON.stringify(back) === JSON.stringify(bytes);
  ok = ok && same;
  console.log(same ? 'OK ' : 'BAD', JSON.stringify(text).padEnd(42), shown, same ? '' : ' ref=' + ref + ' c=' + cBytes);
}
// HEX mode is the identity
console.log('hex Hello ->', B.ENCODINGS.hex.pathFor(B.textBytes('Hello')).map(B.ENCODINGS.hex.name).join('/'));
process.exit(ok ? 0 : 1);
