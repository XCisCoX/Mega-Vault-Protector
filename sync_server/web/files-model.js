/* Shared, dependency-free file model. Browser global + Node test entry. */
(function (root, factory) {
  const model = factory();
  if (typeof module === 'object' && module.exports) module.exports = model;
  else root.FilesModel = model;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  function parts(path) { return String(path || '').replace(/\\/g, '/').split('/').filter(p => p && p !== '.' && p !== '..'); }
  function parent(record) { return parts(record.path).join('/'); }
  function folderEntries(records, folder) {
    const result = [], folders = new Set(), prefix = folder ? folder + '/' : '';
    for (const record of records) {
      if (record.deleted) continue;
      const dir = parent(record);
      if (dir === folder) result.push({...record, kind:'file'});
      else if (dir.startsWith(prefix)) {
        const name = dir.slice(prefix.length).split('/')[0];
        if (name && !folders.has(name)) { folders.add(name); result.push({kind:'folder', name, path:prefix + name}); }
      }
    }
    return result;
  }
  function safeName(name) {
    let result = String(name || '').replace(/\\/g, '/').split('/').pop().replace(/[<>:"|?*\u0000-\u001f\u007f]/g, '_').replace(/^ +|[. ]+$/g, '');
    if (!result || result === '.' || result === '..') result = 'Untitled';
    if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$)/i.test(result)) result = '_' + result;
    // Native package path segments permit 120 UTF-8 bytes, not 255 JS code units.
    const encoder=new TextEncoder();
    if(encoder.encode(result).length>120){const match=result.match(/\.[a-z0-9]{1,12}$/i),ext=match?match[0]:'';let stem=ext?result.slice(0,-ext.length):result;while(encoder.encode(stem+ext).length>120)stem=Array.from(stem).slice(0,-1).join('');result=stem+ext;}
    return result;
  }
  function safePath(path) {
    const value = String(path || '').replace(/\\/g, '/').trim();
    if (value.length > 1024 || value.startsWith('/') || /[<>:"|?*\u0000-\u001f]/.test(value) || value.split('/').some(p => p === '.' || p === '..')) throw new Error('Use a relative folder path, such as Trips/Paris.');
    return value.split('/').filter(Boolean).join('/');
  }
  const media = {png:'image/png',jpg:'image/jpeg',jpeg:'image/jpeg',webp:'image/webp',gif:'image/gif',avif:'image/avif',mp3:'audio/mpeg',wav:'audio/wav',ogg:'audio/ogg',m4a:'audio/mp4',flac:'audio/flac',mp4:'video/mp4',webm:'video/webm',mov:'video/quicktime',pdf:'application/pdf'};
  function previewType(name) { return media[String(name).split('.').pop().toLowerCase()] || null; }
  function visibleEntries(records, options) {
    const {view='files',folder='',query='',sort='name'} = options;
    const q = query.trim().toLocaleLowerCase();
    let items;
    if (view === 'files' && !q) items = folderEntries(records, folder);
    else items = records.filter(r => (view === 'trash' ? r.deleted : !r.deleted))
      .filter(r => view !== 'photos' || String(previewType(r.name)).startsWith('image/'))
      .filter(r => view !== 'music' || String(previewType(r.name)).startsWith('audio/'))
      .filter(r => !q || (r.name + ' ' + r.path + ' ' + (r.tags || []).join(' ')).toLocaleLowerCase().includes(q))
      .map(r => ({...r,kind:'file'}));
    return items.sort((a,b) => {
      if (a.kind !== b.kind) return a.kind === 'folder' ? -1 : 1;
      if (sort === 'size') return (Number(b.size)||0) - (Number(a.size)||0) || a.name.localeCompare(b.name);
      if (sort === 'updated') return (Number(b.updated)||0) - (Number(a.updated)||0) || a.name.localeCompare(b.name);
      return (sort === 'name-desc' ? -1 : 1) * a.name.localeCompare(b.name, undefined, {numeric:true});
    });
  }
  async function loadRecords(fetchPage, limit=500) {
    const found = new Map();
    let cursor=0;
    for (;;) {
      const page=await fetchPage(cursor,limit);
      if (!Array.isArray(page.records)) throw new Error('The file list could not be read.');
      for (const record of page.records) found.set(record.id,record);
      if (page.records.length < limit) break;
      if (!Number.isSafeInteger(page.cursor) || page.cursor <= cursor) throw new Error('The file list cursor did not advance.');
      cursor=page.cursor;
    }
    return Array.from(found.values());
  }
  async function resumeUpload(bytes,{inspect,put,signal,chunkBytes=1024*1024,onProgress=()=>{}}) {
    const total=bytes.length;
    if(!Number.isSafeInteger(chunkBytes)||chunkBytes<1||chunkBytes>1024*1024)throw new Error('Invalid upload chunk limit.');
    const check=()=>{if(signal?.aborted)throw new DOMException('Cancelled','AbortError');};
    const position=async()=>{check();const info=await inspect();check();if(!Number.isSafeInteger(info.offset)||info.offset<0||info.offset>total)throw new Error('Invalid upload resume offset.');return info;};
    let offset=(await position()).offset,retries=0;onProgress(offset,total);
    while(offset<total){check();const end=Math.min(offset+chunkBytes,total);try{await put(offset,bytes.subarray(offset,end));check();offset=end;retries=0;onProgress(offset,total);}catch(error){if(error.name==='AbortError'||(error.status&&error.status!==409)||++retries>2)throw error;offset=(await position()).offset;onProgress(offset,total);}}
    const final=await position();if(!final.complete||final.offset!==total)throw new Error('The encrypted upload is not complete. Retry to resume it.');
  }
  return {folderEntries, parent, safeName, safePath, previewType, visibleEntries, loadRecords, resumeUpload};
});
