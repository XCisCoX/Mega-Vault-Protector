'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const M = require('./files-model.js');
test('relative record paths derive folders without inventing folder records', () => {
  assert.deepEqual(M.folderEntries([
    {id:'a',name:'A.jpg',path:'Trips/Paris',deleted:false},
    {id:'b',name:'B.jpg',path:'Trips/Rome/B.jpg',deleted:false},
    {id:'c',name:'Gone.jpg',path:'Trash/Gone.jpg',deleted:true}
  ], ''), [{kind:'folder',name:'Trips',path:'Trips'}]);
  assert.deepEqual(M.folderEntries([{id:'a',name:'A.jpg',path:'Trips/Paris'}], 'Trips/Paris').map(x=>x.id), ['a']);
});
test('filenames, move destinations and preview media are constrained', () => {
  assert.equal(M.safeName('../../<bad>\\\\name.html'), 'name.html');
  assert.equal(M.safeName(''), 'Untitled');
  assert.equal(M.safePath('Trips/Paris'), 'Trips/Paris');
  assert.throws(() => M.safePath('../escape'));
  assert.throws(() => M.safePath('C:/escape'));
  assert.equal(M.previewType('photo.SVG'), null);
  assert.equal(M.previewType('web.html'), null);
  assert.equal(M.previewType('image.png'), 'image/png');
  assert.equal(M.safeName('  lead.txt'), 'lead.txt');
  assert.ok(new TextEncoder().encode(M.safeName('é'.repeat(150)+'.txt')).length<=120);
  assert.ok(M.safeName('é'.repeat(150)+'.txt').endsWith('.txt'));
});
test('search traverses folders, media filters and trash never leak live records', () => {
  const records = [
    {id:'1',name:'Z.jpg',path:'Trips',size:5},
    {id:'2',name:'A.mp3',path:'Music',size:20},
    {id:'3',name:'Deleted.jpg',path:'',deleted:true,size:9}
  ];
  assert.deepEqual(M.visibleEntries(records,{view:'files',folder:'',query:'z',sort:'name'}).map(r=>r.id), ['1']);
  assert.deepEqual(M.visibleEntries(records,{view:'photos',folder:'',query:'',sort:'name'}).map(r=>r.id), ['1']);
  assert.deepEqual(M.visibleEntries(records,{view:'trash',folder:'',query:'',sort:'name'}).map(r=>r.id), ['3']);
  assert.deepEqual(M.visibleEntries(records,{view:'files',folder:'',query:'.',sort:'size'}).map(r=>r.id), ['2','1']);
});
test('record pagination walks cursor until a short batch and rejects a stuck cursor', async () => {
  const calls=[];
  const records=await M.loadRecords(async (cursor,limit)=> {calls.push([cursor,limit]); return cursor===0?{records:[{id:'a'},{id:'b'}],cursor:2}:{records:[{id:'c'}],cursor:3};},2);
  assert.deepEqual(records.map(r=>r.id), ['a','b','c']);
  assert.deepEqual(calls, [[0,2],[2,2]]);
  await assert.rejects(()=>M.loadRecords(async()=>({records:[{id:'a'}],cursor:0}),1), /cursor/i);
});
test('ciphertext upload resumes durable bytes and verifies completion before returning', async () => {
  const source=Uint8Array.from([0,1,2,3,4,5,6,7,8]);
  let durable=2;const writes=[],progress=[];
  await M.resumeUpload(source,{
    inspect:async()=>({offset:durable,complete:durable===source.length}),
    put:async(offset,chunk)=>{writes.push([offset,...chunk]);assert.equal(offset,durable);durable+=chunk.length;},
    chunkBytes:4,onProgress:(n)=>progress.push(n)
  });
  assert.deepEqual(writes,[[2,2,3,4,5],[6,6,7,8]]);
  assert.deepEqual(progress,[2,6,9]);
  await assert.rejects(()=>M.resumeUpload(source,{inspect:async()=>({offset:9,complete:false}),put:async()=>{},chunkBytes:4}),/complete/i);
});
test('lost chunk response resumes at the durable offset; cancellation prevents writes', async () => {
  const source=new Uint8Array([1,2,3,4,5,6]);let durable=0,first=true;const offsets=[];
  await M.resumeUpload(source,{
    inspect:async()=>({offset:durable,complete:durable===source.length}),chunkBytes:3,
    put:async(offset,chunk)=>{offsets.push(offset);durable+=chunk.length;if(first){first=false;throw new TypeError('connection interrupted after durable write');}}
  });
  assert.deepEqual(offsets,[0,3]);
  const c=new AbortController();c.abort();let inspected=false;
  await assert.rejects(()=>M.resumeUpload(source,{signal:c.signal,inspect:async()=>{inspected=true;return {offset:0};},put:async()=>assert.fail('must not write')}),{name:'AbortError'});
  assert.equal(inspected,false);
});
