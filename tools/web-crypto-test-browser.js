/* Executed in a real browser under a strict CSP with no unsafe-eval. */
(async function () {
  'use strict';
  const violations=[];
  addEventListener('securitypolicyviolation',e=>violations.push(e.violatedDirective));
  const assert=(condition,message)=>{if(!condition)throw Error(message);};
  const rejected=async action=>{let failed=false;try{await action();}catch(_){failed=true;}assert(failed,'invalid input accepted');};
  try {
    await sodium.ready;
    const payload=new Uint8Array(1048579).fill(42), progress=[];
    const encrypted=await MvpPackage.encrypt(new File([payload],'sample.bin'),'test-password',n=>progress.push(n));
    const out=await MvpPackage.decrypt(encrypted,'test-password');
    assert(out.length===1&&out[0].name==='sample.bin'&&out[0].bytes.length===payload.length&&out[0].bytes.every(x=>x===42),'browser roundtrip failed');
    assert(progress[0]===0&&progress.at(-1)===1,'progress failed');
    await rejected(()=>MvpPackage.decrypt(encrypted,'wrong'));
    const bad=encrypted.slice();bad[bad.length-1]^=1;
    await rejected(()=>MvpPackage.decrypt(bad,'test-password'));
    const native=[];
    let compressed;
    for(const name of ['plain.mvpk','compressed.mvpk']) {
      const response=await fetch('/fixture/'+name);
      assert(response.ok,'native fixture missing: '+name);
      const bytes=new Uint8Array(await response.arrayBuffer());
      const files=await MvpPackage.decrypt(bytes,'test-password');
      assert(files.length===2&&files[0].name==='folder/sample.bin'&&files[0].bytes.length===1048579&&files[0].bytes.every(x=>x===42)&&files[1].bytes.length===0,'native browser fixture failed');
      native.push(name);if(name==='compressed.mvpk')compressed=bytes;
    }
    // Re-authenticate a native compressed record with a trailing zlib byte.
    // This tests strict decompressor EOF in the browser, not just Node's implementation.
    const key=sodium.crypto_pwhash(32,'test-password',compressed.slice(24,40),3,67108864,sodium.crypto_pwhash_ALG_ARGON2ID13);
    const pull=sodium.crypto_secretstream_xchacha20poly1305_init_pull(compressed.slice(40,64),key);
    const push=sodium.crypto_secretstream_xchacha20poly1305_init_push(key);
    const header=compressed.slice(0,64);header.set(push.header,40);const parts=[header];
    const dv=new DataView(compressed.buffer);let p=64,changed=false;
    while(p<compressed.length) {
      const n=dv.getUint32(p,true);p+=4;
      const record=sodium.crypto_secretstream_xchacha20poly1305_pull(pull,compressed.slice(p,p+n));p+=n;
      let plain=record.message;
      if(!changed&&plain[0]===2&&plain[1]===1) {const appended=new Uint8Array(plain.length+1);appended.set(plain);plain=appended;changed=true;}
      const cipher=sodium.crypto_secretstream_xchacha20poly1305_push(push.state,plain,null,record.tag),size=new Uint8Array(4);new DataView(size.buffer).setUint32(0,cipher.length,true);parts.push(size,cipher);
    }
    assert(changed,'compressed fixture did not contain compressed chunks');
    const corrupt=new Uint8Array(parts.reduce((n,a)=>n+a.length,0));let offset=0;for(const part of parts){corrupt.set(part,offset);offset+=part.length;}
    await rejected(()=>MvpPackage.decrypt(corrupt,'test-password'));
    sodium.memzero(key);for(const state of [pull,push.state]){sodium.libsodium.HEAPU8.fill(0,state,state+52);sodium.libsodium._free(state);}
    assert(!violations.length,'CSP violations: '+violations.join(','));
    window.testResult={ok:true,libsodium:sodium.sodium_version_string(),native,strictCsp:true,compressedTrailingRejected:true,bytes:payload.length};
    document.getElementById('result').textContent=JSON.stringify(window.testResult,null,2);
  } catch(error) {
    window.testResult={ok:false,error:String(error),stack:error.stack,violations};
    document.getElementById('result').textContent=JSON.stringify(window.testResult,null,2);
  }
})();
