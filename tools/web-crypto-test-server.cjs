'use strict';
const http=require('node:http'), fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const files=new Map([
  ['/',[path.join(__dirname,'web-crypto-test-browser.html'),'text/html']],
  ['/test.js',[path.join(__dirname,'web-crypto-test-browser.js'),'application/javascript']],
  ['/package-crypto.js',[path.join(root,'sync_server/web/package-crypto.js'),'application/javascript']],
  ['/vendor/sodium.js',[path.join(root,'sync_server/web/vendor/sodium.js'),'application/javascript']],
  ['/vendor/sodium-wrappers.js',[path.join(root,'sync_server/web/vendor/sodium-wrappers.js'),'application/javascript']]
]);
if(process.env.MVP_CRYPTO_FIXTURES) for(const name of ['plain.mvpk','compressed.mvpk'])
  files.set('/fixture/'+name,[path.join(process.env.MVP_CRYPTO_FIXTURES,name),'application/octet-stream']);
http.createServer((req,res)=>{
  const target=files.get(req.url);
  res.setHeader('Content-Security-Policy',"default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  res.setHeader('X-Content-Type-Options','nosniff');
  if(!target) {res.writeHead(404);res.end();return;}
  res.setHeader('Content-Type',target[1]);res.end(fs.readFileSync(target[0]));
}).listen(18973,'127.0.0.1',()=>console.log('Crypto CSP test: http://127.0.0.1:18973'));
