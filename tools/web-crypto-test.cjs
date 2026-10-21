'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const web = path.resolve(__dirname, '../sync_server/web');
// Browser UMD path, not the CommonJS dependency resolver. No eval/Function allowed.
const browser = vm.createContext({console, crypto: require('node:crypto').webcrypto,
  TextEncoder, TextDecoder, Uint8Array, ArrayBuffer, DataView, Blob, File,
  DecompressionStream, ReadableStream, Response, setTimeout, clearTimeout, WebAssembly},
  {codeGeneration: {strings: false, wasm: true}});
browser.window = browser; browser.self = browser;
for (const file of ['vendor/sodium.js', 'vendor/sodium-wrappers.js'])
  vm.runInContext(fs.readFileSync(path.join(web, file), 'utf8'), browser, {filename:file});
const ready = browser.sodium.ready;
if (fs.existsSync(path.join(web, 'package-crypto.js')))
  vm.runInContext(fs.readFileSync(path.join(web, 'package-crypto.js'), 'utf8'), browser);

test('browser writer emits native format readable by reference sodium', async () => {
  await ready;
  assert.ok(browser.MvpPackage, 'window.MvpPackage must exist');
  const progress = [];
  const content = new Uint8Array(1048579).fill(42);
  const bytes = await browser.MvpPackage.encrypt(new File([content], 'sample.bin'), 'test-password', n => progress.push(n));
  assert.ok(bytes instanceof Uint8Array);
  assert.deepEqual([...bytes.slice(0, 12)], [77,86,80,75,167,1,195,0,1,0,0,0]);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  assert.deepEqual([dv.getUint32(12,true),dv.getUint32(16,true),dv.getUint32(20,true)], [65536,3,1]);
  const s = browser.sodium;
  const key = s.crypto_pwhash(32, 'test-password', bytes.slice(24,40), 3, 67108864, s.crypto_pwhash_ALG_ARGON2ID13);
  const state = s.crypto_secretstream_xchacha20poly1305_init_pull(bytes.slice(40,64),key);
  const records=[]; let p=64;
  while(p<bytes.length) { const n=dv.getUint32(p,true); p+=4; records.push(s.crypto_secretstream_xchacha20poly1305_pull(state,bytes.slice(p,p+n))); p+=n; }
  assert.equal(p,bytes.length);
  assert.deepEqual(records.map(r=>r.message[0]), [1,2,2,4]);
  assert.equal(records.at(-1).tag,s.crypto_secretstream_xchacha20poly1305_TAG_FINAL);
  assert.deepEqual(Buffer.concat(records.slice(1,-1).map(r=>Buffer.from(r.message.slice(6)))), Buffer.from(content));
  assert.equal(progress[0],0); assert.equal(progress.at(-1),1);
  assert.ok(progress.every((n,i)=>n>=0&&n<=1&&(!i||n>=progress[i-1])));
});
function u32(n) { const b=Buffer.alloc(4); b.writeUInt32LE(n); return b; }
function entry(name, size, kind=1) {
  const n=Buffer.from(name), b=Buffer.alloc(3+n.length+(kind===1?8:0));
  b[0]=kind; b.writeUInt16LE(n.length,1); n.copy(b,3);
  if(kind===1) b.writeBigUInt64LE(BigInt(size),3+n.length);
  return b;
}
function chunk(bytes, compress=false, declared=bytes.length) {
  return Buffer.concat([Buffer.from([2,compress?1:0]),u32(declared),compress?require('node:zlib').deflateSync(bytes):Buffer.from(bytes)]);
}
async function fixture(records, tags, password='test-password') {
  await ready; const s=browser.sodium;
  const salt=s.randombytes_buf(16), key=s.crypto_pwhash(32,password,salt,3,67108864,s.crypto_pwhash_ALG_ARGON2ID13);
  const stream=s.crypto_secretstream_xchacha20poly1305_init_push(key), header=Buffer.alloc(64);
  Buffer.from([77,86,80,75,167,1,195,0,1,1,0,0]).copy(header);
  header.writeUInt32LE(65536,12);header.writeUInt32LE(3,16);header.writeUInt32LE(1,20);
  Buffer.from(salt).copy(header,24);Buffer.from(stream.header).copy(header,40);
  const parts=[header];
  records.forEach((r,i)=>{ const c=s.crypto_secretstream_xchacha20poly1305_push(stream.state,new Uint8Array(r),null,tags?tags[i]:(i===records.length-1?3:0));parts.push(u32(c.length),Buffer.from(c)); });
  s.memzero(key); s.libsodium.HEAPU8.fill(0,stream.state,stream.state+52);s.libsodium._free(stream.state);
  return new Uint8Array(Buffer.concat(parts));
}
test('reader accepts reference compressed multi-file package only after final validation', async()=>{
  const payload=Buffer.alloc(1048576,65);
  const archive=await fixture([entry('folder',0,3),entry('folder/a.txt',payload.length),chunk(payload,true),entry('empty.txt',0),Buffer.from([4])]);
  const progress=[];
  assert.equal(typeof browser.MvpPackage.decrypt,'function');
  const output=await browser.MvpPackage.decrypt(archive.buffer,'test-password',n=>progress.push(n));
  assert.deepEqual(Array.from(output,r=>r.name),['folder/a.txt','empty.txt']);
  assert.deepEqual(Buffer.from(output[0].bytes),payload); assert.equal(output[1].bytes.length,0);
  assert.equal(progress.at(-1),1);
  const corrupt=archive.slice();corrupt[corrupt.length-1]^=1;
  const failedProgress=[];
  await assert.rejects(browser.MvpPackage.decrypt(corrupt,'test-password',n=>failedProgress.push(n)),/authentic|password|damaged/i);
  assert.ok(!failedProgress.includes(1),'cannot announce completion before authenticating final record');
});
test('rejects wrong passwords, truncation, trailing bytes, forged KDF and oversized frames', async()=>{
  const a=await fixture([entry('a',1),chunk(Buffer.from('x')),Buffer.from([4])]);
  await assert.rejects(browser.MvpPackage.decrypt(a,'wrong-password'),/authentic|password/i);
  for(const b of [a.slice(0,-1),a.slice(0,66),new Uint8Array(Buffer.concat([Buffer.from(a),Buffer.from([0])]))])
    await assert.rejects(browser.MvpPackage.decrypt(b,'test-password'),/truncat|incomplete|trailing/i);
  for(const n of [0,1,17,0xffffffff]) {
    const b=a.slice();new DataView(b.buffer).setUint32(64,n,true);
    await assert.rejects(browser.MvpPackage.decrypt(b,'test-password'),/frame/i);
  }
  const s=browser.sodium, original=s.crypto_pwhash; let calls=0;
  s.crypto_pwhash=(...args)=>{calls++;return original(...args);};
  try {
    for(const [offset,n] of [[12,0xffffffff],[12,8192],[16,20],[20,2],[20,0]]) {
      const b=a.slice(); new DataView(b.buffer).setUint32(offset,n,true);
      await assert.rejects(browser.MvpPackage.decrypt(b,'test-password'),/KDF/i);
    }
    assert.equal(calls,0,'untrusted KDF dimensions must be rejected before any Argon2 work');
  } finally {s.crypto_pwhash=original;}
});
test('rejects authenticated malformed record structure, unsafe paths, limits and compressed streams', async()=>{
  const end=Buffer.from([4]);
  const zipped=chunk(Buffer.alloc(100,65),true);
  const cases=[
    [entry('../escape',0),end], [entry('/root',0),end], [entry('C:evil',0),end],
    [entry('x\\y',0),end], [entry('x//y',0),end], [entry('x/..',0),end],
    [entry('a',0),entry('A',0),end], [entry('a',0),entry('a/b',0),end],
    [entry('a',134217729),end], [entry('a',18446744073709551615n),end],
    [entry('a',1),end], [chunk(Buffer.from('x')),end],
    [entry('a',1),chunk(Buffer.from('xx')),end],
    [entry('a',1),chunk(Buffer.from('x'),false,1048577),end],
    [entry('a',100),Buffer.concat([zipped,Buffer.from([0])]),end],
    [entry('a',100),zipped.slice(0,-1),end],
    [entry('a',100),chunk(Buffer.alloc(200,65),true,100),end],
    [entry('a',100),chunk(Buffer.alloc(99,65),true,100),end],
    [entry('a',0),Buffer.from([99]),end],
    [entry('a',0),Buffer.from([4,0])],
    [...Array.from({length:257},(_,i)=>entry('file'+i,0)),end],
    [...Array.from({length:257},(_,i)=>entry('dir'+i,0,3)),end]
  ];
  for(let i=0;i<cases.length;i++) {
    const a=await fixture(cases[i]);
    await assert.rejects(browser.MvpPackage.decrypt(a,'test-password'),undefined,'malformed case '+i);
  }
  for(const tags of [[3,3],[0,0],[2,3]]) {
    const a=await fixture([entry('a',0),end],tags);
    await assert.rejects(browser.MvpPackage.decrypt(a,'test-password'),/tag|final/i);
  }
});
test('writer fallback, empty file, password and size limits', async()=>{
  const a=await browser.MvpPackage.encrypt(new Blob([]),'🔐رمز');
  const output=await browser.MvpPackage.decrypt(a,'🔐رمز');
  assert.equal(output[0].name,'file.bin');assert.equal(output[0].bytes.length,0);
  await assert.rejects(browser.MvpPackage.encrypt({size:134217729,slice(){throw Error('must not read');}},'x'),/limit/i);
  for(const p of ['', 'x'.repeat(4097)]) await assert.rejects(browser.MvpPackage.encrypt(new Blob([]),p),/password/i);
  await assert.rejects(browser.MvpPackage.encrypt({size:0,slice(){return new Blob([]);}},'x'),/Blob/i);
  await assert.rejects(browser.MvpPackage.encrypt(new File([], '../bad'),'p'),/path/i);
});
test('native Vault export decrypts in browser and browser export imports in native Vault', {skip: !process.env.MVP_CRYPTO_NATIVE}, async()=>{
  const {spawnSync}=require('node:child_process');
  const root=fs.mkdtempSync(path.join(process.env.MVP_CRYPTO_FIXTURES || path.dirname(process.env.MVP_CRYPTO_NATIVE),'mvpk-native-'));
  const run=mode=>{
    const r=spawnSync(process.env.MVP_CRYPTO_NATIVE,[mode,root],{encoding:'utf8'});
    assert.equal(r.status,0,r.stdout+'\n'+r.stderr+'\n'+String(r.error||''));
    console.log(r.stdout.trim());
  };
  try {
    run('export');
    for(const name of ['plain.mvpk','compressed.mvpk']) {
      const files=await browser.MvpPackage.decrypt(new Uint8Array(fs.readFileSync(path.join(root,name))),'test-password');
      assert.deepEqual(Array.from(files,r=>r.name),['folder/sample.bin','empty.bin']);
      assert.deepEqual(Buffer.from(files[0].bytes),Buffer.alloc(1048579,42));
      assert.equal(files[1].bytes.length,0);
    }
    const bytes=await browser.MvpPackage.encrypt(new File([Buffer.alloc(1048579,42)],'sample.bin'),'test-password');
    fs.writeFileSync(path.join(root,'browser.mvpk'),bytes);
    run('import');
  } finally {fs.rmSync(root,{recursive:true,force:true});}
});
test('decompression feeds small bounded input segments before rejecting an expansion bomb', async()=>{
  const source=await fixture([entry('a',1048576),chunk(Buffer.alloc(2097152,65),true,1048576),Buffer.from([4])]);
  const Original=browser.DecompressionStream; let maximum=0;
  browser.DecompressionStream=class {
    constructor(format) {
      const ds=new DecompressionStream(format);
      const tap=new TransformStream({transform(bytes,controller){maximum=Math.max(maximum,bytes.length);controller.enqueue(bytes);}});
      return {writable:tap.writable,readable:tap.readable.pipeThrough(ds)};
    }
  };
  try {
    await assert.rejects(browser.MvpPackage.decrypt(source,'test-password'),/decompressed|compressed/i);
    assert.ok(maximum>0&&maximum<=256,'bounded decompressor input, observed '+maximum+' bytes');
  } finally {browser.DecompressionStream=Original;}
});
test('enforces a bounded count of otherwise valid tiny records', async()=>{
  const records=[entry('tiny.bin',4095),...Array.from({length:4095},()=>chunk(Buffer.from([1]))),Buffer.from([4])];
  const a=await fixture(records);
  await assert.rejects(browser.MvpPackage.decrypt(a,'test-password'),/record count/i);
});
test('vendored upstream JavaScript hashes remain pinned',()=>{
  const {createHash}=require('node:crypto');
  for(const [name,hash] of Object.entries({
    'sodium.js':'a3d0b7eb2c24ba6b5401d88717e65bc6a92786d211bc8b91ed746d24226180f3',
    'sodium-wrappers.js':'4e2c5442899321d7d9aea69bef71a3c24a6eab5911b74a5c1ad8bc8c733b0a15'
  })) assert.equal(createHash('sha256').update(fs.readFileSync(path.join(web,'vendor',name))).digest('hex'),hash);
});
test('reader owns its input even when Uint8Array is a Buffer subclass', async()=>{
  const archive=await fixture([entry('a',1),chunk(Buffer.from('x')),Buffer.from([4])]);
  const input=Buffer.from(archive);
  const pending=browser.MvpPackage.decrypt(input,'test-password');
  input.fill(0);
  const result=await pending;
  assert.equal(result[0].name,'a');assert.equal(result[0].bytes[0],120);
});
test('preserves a UTF-8 BOM codepoint that belongs to a native filename', async()=>{
  const archive=await fixture([entry('\ufeffname.txt',0),Buffer.from([4])]);
  const result=await browser.MvpPackage.decrypt(archive,'test-password');
  assert.equal(result[0].name,'\ufeffname.txt');
});
module.exports = {browser, ready,fixture,entry,chunk,u32};
