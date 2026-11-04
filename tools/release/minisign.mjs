#!/usr/bin/env node
// minisign-compatible keys and signatures for 3re's update manifests, with nothing but Node's built-in
// crypto (Ed25519, BLAKE2b-512): no package to install in CI. Format: https://jedisct1.github.io/minisign/
// Signatures made here verify with the official `minisign -Vm FILE -p tools/release/3re-update.pub`.
//
//   node tools/release/minisign.mjs keygen --pub OUT.pub --secret OUT.secret
//   node tools/release/minisign.mjs sign   --file F [--trusted-comment TEXT]   (secret from env THREERE_UPDATE_SIGNING_KEY)
//   node tools/release/minisign.mjs verify --file F --pub tools/release/3re-update.pub
//
// The secret is "3RE-MINISIGN-SECRET-V1:" + base64(key id (8) | Ed25519 seed (32)). It belongs only in the
// repository's Actions secret THREERE_UPDATE_SIGNING_KEY and in an offline backup, never in a file in the repo.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

const SECRET_PREFIX = '3RE-MINISIGN-SECRET-V1:';
const PKCS8_ED25519 = Buffer.from('302e020100300506032b657004220420', 'hex');
const SPKI_ED25519 = Buffer.from('302a300506032b6570032100', 'hex');

function args(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) out[argv[i].slice(2)] = argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[++i] : true;
    else out._.push(argv[i]);
  }
  return out;
}

function keyIdHex(id) {
  return Buffer.from(id).reverse().toString('hex').toUpperCase();
}

function privateKey(seed) {
  return crypto.createPrivateKey({ key: Buffer.concat([PKCS8_ED25519, seed]), format: 'der', type: 'pkcs8' });
}

function publicRaw(priv) {
  const der = crypto.createPublicKey(priv).export({ format: 'der', type: 'spki' });
  return der.subarray(der.length - 32);
}

function readSecret(text) {
  const t = String(text || '').trim();
  if (!t.startsWith(SECRET_PREFIX)) throw new Error('the signing secret is missing or not a 3RE-MINISIGN-SECRET-V1 value');
  const raw = Buffer.from(t.slice(SECRET_PREFIX.length), 'base64');
  if (raw.length !== 40) throw new Error('the signing secret has the wrong length');
  return { keyId: raw.subarray(0, 8), seed: raw.subarray(8, 40) };
}

function parsePub(text) {
  const lines = text.split(/\r?\n/).filter((l) => l.length);
  const raw = Buffer.from(lines[lines.length - 1].trim(), 'base64');
  if (raw.length !== 42 || raw.subarray(0, 2).toString() !== 'Ed') throw new Error('not a minisign public key');
  return { keyId: raw.subarray(2, 10), key: raw.subarray(10, 42) };
}

function sign({ keyId, seed }, data, trusted, untrusted = 'signature from the 3re release key') {
  if (/[\r\n]/.test(trusted) || /[\r\n]/.test(untrusted)) throw new Error('comments must be one line');
  const priv = privateKey(seed);
  const digest = crypto.createHash('blake2b512').update(data).digest();
  const sig = crypto.sign(null, digest, priv);
  const global = crypto.sign(null, Buffer.concat([sig, Buffer.from(trusted, 'utf8')]), priv);
  const line = Buffer.concat([Buffer.from('ED'), keyId, sig]).toString('base64');
  return `untrusted comment: ${untrusted}\n${line}\ntrusted comment: ${trusted}\n${global.toString('base64')}\n`;
}

function verify(pub, data, sigText) {
  const lines = sigText.split(/\r?\n/).filter((l) => l.length);
  if (lines.length !== 4 || !lines[0].startsWith('untrusted comment: ') || !lines[2].startsWith('trusted comment: '))
    return 'not a minisign signature file';
  const raw = Buffer.from(lines[1], 'base64');
  const global = Buffer.from(lines[3], 'base64');
  if (raw.length !== 74 || global.length !== 64) return 'malformed signature';
  const alg = raw.subarray(0, 2).toString();
  if (!raw.subarray(2, 10).equals(pub.keyId)) return `signed with another key (${keyIdHex(raw.subarray(2, 10))})`;
  const key = crypto.createPublicKey({ key: Buffer.concat([SPKI_ED25519, pub.key]), format: 'der', type: 'spki' });
  const message = alg === 'ED' ? crypto.createHash('blake2b512').update(data).digest() : alg === 'Ed' ? data : null;
  if (!message) return 'unknown signature algorithm';
  const sig = raw.subarray(10);
  if (!crypto.verify(null, message, key, sig)) return 'the signature does not match the file';
  const trusted = lines[2].slice('trusted comment: '.length);
  if (!crypto.verify(null, Buffer.concat([sig, Buffer.from(trusted, 'utf8')]), key, global)) return 'the trusted comment was changed';
  return '';
}

const a = args(process.argv.slice(2));
try {
  switch (a._[0]) {
    case 'keygen': {
      if (!a.pub || !a.secret) throw new Error('keygen needs --pub and --secret');
      if (fs.existsSync(a.secret)) throw new Error(`${a.secret} exists; refusing to overwrite a secret`);
      const keyId = crypto.randomBytes(8);
      const seed = crypto.randomBytes(32);
      const pub = publicRaw(privateKey(seed));
      const pubText = `untrusted comment: minisign public key ${keyIdHex(keyId)}\n${Buffer.concat([Buffer.from('Ed'), keyId, pub]).toString('base64')}\n`;
      fs.writeFileSync(a.pub, pubText);
      fs.writeFileSync(a.secret, SECRET_PREFIX + Buffer.concat([keyId, seed]).toString('base64') + '\n', { mode: 0o600 });
      console.log(`key id ${keyIdHex(keyId)}; public key written to ${a.pub}`);
      break;
    }
    case 'sign': {
      if (!a.file) throw new Error('sign needs --file');
      const secret = readSecret(process.env.THREERE_UPDATE_SIGNING_KEY);
      const data = fs.readFileSync(a.file);
      const trusted = a['trusted-comment'] || `timestamp:${Math.floor(Date.now() / 1000)}\tfile:${path.basename(a.file)}\thashed`;
      fs.writeFileSync(a.file + '.minisig', sign(secret, data, trusted));
      console.log(`signed ${a.file} with key ${keyIdHex(secret.keyId)}`);
      break;
    }
    case 'verify': {
      if (!a.file || !a.pub) throw new Error('verify needs --file and --pub');
      const why = verify(parsePub(fs.readFileSync(a.pub, 'utf8')), fs.readFileSync(a.file), fs.readFileSync(a.sig || a.file + '.minisig', 'utf8'));
      if (why) { console.error(`SIGNATURE NOT VALID: ${why}`); process.exit(1); }
      console.log(`signature valid: ${a.file}`);
      break;
    }
    default:
      console.error('usage: minisign.mjs keygen|sign|verify (see the header of this file)');
      process.exit(2);
  }
} catch (e) {
  console.error(`minisign.mjs: ${e.message}`);
  process.exit(1);
}
