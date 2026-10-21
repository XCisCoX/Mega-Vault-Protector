/* MegaVault Files. Secrets and decrypted content stay in page memory only. */
'use strict';
const $ = id => document.getElementById(id);
const M = window.FilesModel;
const MAX_FILE_BYTES = 128 * 1024 * 1024;
const CHUNK_BYTES = 1024 * 1024;
const MAX_PACKAGE_BYTES = MAX_FILE_BYTES + 2 * CHUNK_BYTES;
const state = {
  account:null, csrf:'', registration:'closed', vaults:[], vault:null, quota:{},
  records:[], view:'files', folder:'', query:'', sort:'name', layout:'list', selected:null,
  password:null, checkedPackage:null, loading:false, loadError:'', generation:0,
  controllers:new Set(), transfers:[], active:0, urls:new Set(), devices:[],
  capabilities:{chunk_bytes:CHUNK_BYTES,max_file_bytes:MAX_FILE_BYTES}, busy:false
};
const FILE_VIEWS = ['files','photos','music','trash'];
const titles = {files:'Files',photos:'Photos',music:'Music',trash:'Trash',devices:'Devices',settings:'Settings'};
function el(tag,cls,text) {const n=document.createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n;}
function button(label, handler, cls='button small') {const b=el('button',cls,label);b.type='button';b.addEventListener('click',()=>run(handler));return b;}
function fmtBytes(n) {n=Number(n)||0;if(n<1024)return n+' B';const units=['KiB','MiB','GiB','TiB'];let v=n/1024,i=0;while(v>=1024&&i<3){v/=1024;i++;}return v.toFixed(v>=100?0:1)+' '+units[i];}
function fmtDate(n) {const d=new Date(Number(n));return n&&!Number.isNaN(d.valueOf())?d.toLocaleDateString(undefined,{month:'short',day:'numeric',year:'numeric'}):'—';}
function toast(message,error=false) {$('toast').textContent=message;$('toast').className='toast'+(error?' error':'');$('toast').hidden=false;clearTimeout(toast.timer);toast.timer=setTimeout(()=>$('toast').hidden=true,error?9000:4500);}
function newController() {const c=new AbortController();state.controllers.add(c);return c;}
function checkGeneration(generation,signal) {if(generation!==state.generation||signal?.aborted)throw new DOMException('Cancelled','AbortError');}
async function request(path,{method='GET',body,signal,headers={}}={}) {
  const h={Accept:'application/json',...headers};
  if(!['GET','HEAD'].includes(method))h['X-CSRF-Token']=state.csrf;
  if(body!==undefined&&!ArrayBuffer.isView(body)&&!(body instanceof ArrayBuffer)&&!(body instanceof Blob)){h['Content-Type']='application/json';body=JSON.stringify(body);}
  else if(!['GET','HEAD'].includes(method)&&!h['Content-Type'])h['Content-Type']='application/json';
  const response=await fetch(path,{method,body,signal,credentials:'same-origin',headers:h});
  if(!response.ok){let data={};try{data=await response.json();}catch(_){}const err=new Error(response.status===409?'This file changed on another device. Refresh and review it; nothing was overwritten.':data.error||'Request failed ('+response.status+').');err.status=response.status;err.data=data;throw err;}
  return response;
}
async function api(method,path,body,signal) {const r=await request(path,{method,body,signal});return r.status===204?{}:r.json();}
const libraryPath=(id=state.vault?.id)=>'/browser/vaults/'+encodeURIComponent(id);
function recordPath(id,vault=state.vault?.id) {return libraryPath(vault)+'/records/'+encodeURIComponent(id);}
async function run(task) {try{return await task();}catch(e){if(e.name==='AbortError')return;if(e.status===401){clearSession();toast('Your session ended. Sign in again.',true);return;}toast(e.message||'Something went wrong. Please try again.',true);}}
async function verifySignedOut() {try{await api('GET','/auth/session');}catch(error){if(error.status===401)return;throw error;}throw new Error('The server still reports an active session. Please sign out again.');}
async function copy(text) {try{await navigator.clipboard.writeText(text);toast('Copied to clipboard');}catch(_){toast('Could not copy. Select the address and copy it manually.',true);}}

/* Native dialogs provide focus containment and Escape. Restore the opener explicitly. */
function ask({title,text='',label='',value='',password=false,confirm='Continue',required=true,validate}) {
  if($('modal').open)return Promise.resolve(null);
  const dialog=$('modal'),input=$('modal-input'),opener=document.activeElement;
  $('modal-title').textContent=title;$('modal-text').textContent=text;$('modal-label').textContent=label;
  $('modal-field').hidden=!label;input.type=password?'password':'text';input.autocomplete='off';input.value=value;input.required=Boolean(label)&&required;input.maxLength=password?1024:1024;
  $('modal-ok').textContent=confirm;$('modal-error').hidden=true;
  return new Promise(resolve=>{
    let answer=null;
    const close=()=>{dialog.removeEventListener('close',close);$('modal-form').removeEventListener('submit',submit);$('modal-cancel').removeEventListener('click',cancel);input.value='';if(opener?.isConnected)opener.focus();resolve(answer);};
    const cancel=()=>dialog.close();
    const submit=e=>{e.preventDefault();const result=label?(password?input.value:input.value.trim()):'';try{if(validate)validate(result);answer=result;dialog.close();}catch(error){$('modal-error').textContent=error.message;$('modal-error').hidden=false;input.focus();}};
    dialog.addEventListener('close',close);$('modal-form').addEventListener('submit',submit);$('modal-cancel').addEventListener('click',cancel);dialog.showModal();(label?input:$('modal-ok')).focus();if(label&&!password)input.select();
  });
}
function closePreview() {const d=$('preview-dialog');if(d.open)d.close();$('preview-content').replaceChildren();for(const url of state.urls)URL.revokeObjectURL(url);state.urls.clear();}
function lockFiles(announce=true) {
  state.generation++;state.password=null;state.checkedPackage=null;
  for(const c of state.controllers)c.abort();state.controllers.clear();
  state.loading=false;
  for(const t of state.transfers){if(!['Complete','Cancelled'].includes(t.status))t.status='Cancelled';t.file=null;t.bytes=null;t.payload=null;}
  if($('modal').open)$('modal').close();closePreview();renderLock();renderTransfers();renderFiles();if(announce)toast('Files locked. Transfers stopped and previews closed.');
}
function clearSession() {lockFiles(false);state.account=null;state.csrf='';state.vault=null;state.vaults=[];state.records=[];state.devices=[];state.transfers=[];state.selected=null;state.folder='';state.query='';$('files-search').value='';$('token-value').textContent='';$('new-token').hidden=true;$('copy-token').onclick=null;for(const id of ['password-form','login-form','register-form','token-form'])$(id).reset();$('app').hidden=true;showGate('login');renderTransfers();}
function showGate(mode='login') {const login=mode!=='register'||state.registration!=='open';$('auth').hidden=false;$('app').hidden=true;$('login-form').hidden=!login;$('register-form').hidden=login;for(const [id,on] of [['tab-login',login],['tab-register',!login]]){$(id).classList.toggle('is-active',on);$(id).setAttribute('aria-selected',String(on));}$(login?'login-form':'register-form').querySelector('input').focus();}
function renderLock() {const unlocked=state.password!==null;$('lock-status').textContent=unlocked?'Unlocked for this visit':'Files locked';$('lock-status').classList.toggle('ok',unlocked);$('lock-library').textContent=unlocked?'Lock files':'Unlock files';$('lock-library').disabled=!state.vault||state.loading;$('settings-lock').disabled=!state.vault;}
function renderLibraries() {
  $('library-select').replaceChildren();
  if(!state.vaults.length){const o=el('option',null,'No libraries yet');o.value='';$('library-select').append(o);}
  for(const vault of state.vaults){const o=el('option',null,vault.name);o.value=vault.id;o.selected=vault.id===state.vault?.id;$('library-select').append(o);}
  $('library-select').disabled=!state.vaults.length;$('rename-library').disabled=!state.vault;$('delete-library').disabled=!state.vault;
  $('library-name').textContent=state.vault?.name||'Your space';
  const used=Number(state.quota.bytes)||0,limit=Number(state.quota.limit)||0;
  $('usage-meter').value=limit?Math.min(100,used/limit*100):0;
  $('usage-label').textContent=fmtBytes(used)+(limit?' of '+fmtBytes(limit)+' used':' used');
  $('who').textContent=state.account?.email||'';$('account-email').textContent=state.account?.email||'';$('account-created').textContent='Member since '+fmtDate(state.account?.created);$('server-url').textContent=location.origin;renderLock();
}
async function refreshLibraries() {const result=await api('GET','/v1/vaults');state.vaults=result.vaults||[];state.quota=result.quota||{};state.vault=state.vaults.find(v=>v.id===state.vault?.id)||state.vaults[0]||null;renderLibraries();}
async function loadSession() {
  const session=await api('GET','/auth/session');state.account=session.account;state.csrf=session.csrf||'';
  await refreshLibraries();
  try{state.capabilities=await api('GET','/browser/capabilities');}catch(e){if(e.status===401)throw e;toast('Browser file access is unavailable on this server. Account settings still work.',true);}
  $('auth').hidden=true;$('app').hidden=false;setView('files');await loadRecords();
}
async function selectLibrary(id) {lockFiles(false);state.vault=state.vaults.find(v=>v.id===id)||null;state.records=[];state.folder='';state.selected=null;state.query='';state.loadError='';$('files-search').value='';renderLibraries();setView('files');await loadRecords();}
async function loadRecords() {
  if(!state.vault){state.records=[];state.loading=false;renderFiles();return;}
  const generation=state.generation,vault=state.vault.id,c=newController();state.loading=true;state.loadError='';renderLock();renderFiles();
  try{const records=await M.loadRecords((since,limit)=>api('GET',libraryPath(vault)+'/records?since='+since+'&limit='+limit,undefined,c.signal));checkGeneration(generation,c.signal);state.records=records;if(state.selected&&!records.some(r=>r.id===state.selected))state.selected=null;}
  catch(e){if(e.name!=='AbortError'&&generation===state.generation){state.loadError=e.message;throw e;}}
  finally{state.controllers.delete(c);if(generation===state.generation){state.loading=false;renderLock();renderFiles();}}
}
function setView(view) {state.view=view;state.selected=null;for(const b of document.querySelectorAll('.nav-item')){b.classList.toggle('is-active',b.dataset.view===view);if(b.dataset.view===view)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');}$('view-title').textContent=titles[view];$('view-files').hidden=!FILE_VIEWS.includes(view);$('view-devices').hidden=view!=='devices';$('view-settings').hidden=view!=='settings';$('add-files').hidden=view==='trash';$('lock-status').hidden=!FILE_VIEWS.includes(view);$('lock-library').hidden=!FILE_VIEWS.includes(view);if(view==='devices')run(loadDevices);renderFiles();}
function renderBreadcrumbs() {const host=$('breadcrumbs');host.replaceChildren();host.append(button(state.view==='files'?'All files':titles[state.view],()=>{state.folder='';state.query='';$('files-search').value='';renderFiles();},'breadcrumb'));if(state.view==='files'&&state.folder&&!state.query){const parts=state.folder.split('/');parts.forEach((part,i)=>{host.append(el('span','crumb-separator','/'));host.append(button(part,()=>{state.folder=parts.slice(0,i+1).join('/');state.selected=null;renderFiles();},'breadcrumb'));});}if(state.query)host.append(el('span','hint','Search results'));}
function empty(title,text,action,handler) {$('files-empty').hidden=false;$('empty-title').textContent=title;$('empty-text').textContent=text;$('empty-action').hidden=!action;$('empty-action').textContent=action||'';$('empty-action').onclick=handler?()=>run(handler):null;}
function renderFiles() {
  if(!state.account)return;renderBreadcrumbs();renderSelection();
  const host=$('file-list');host.replaceChildren();host.className='file-list '+state.layout;
  $('files-empty').hidden=true;$('list-heading').hidden=state.layout==='grid';$('add-files').disabled=!state.vault||state.loading||Boolean(state.loadError);
  $('file-count').textContent='';
  if(!state.vault){$('list-heading').hidden=true;empty('A home for your files','Create your first library to start adding the things you want to keep.','Create library',createLibrary);return;}
  if(state.loading){$('list-heading').hidden=true;empty('Loading your files…','Getting the latest file list from your library.');return;}
  if(state.loadError){$('list-heading').hidden=true;empty('Couldn’t load your files',state.loadError,'Try again',loadRecords);return;}
  const items=M.visibleEntries(state.records,{view:state.view,folder:state.folder,query:state.query,sort:state.sort});
  if(!items.length){$('list-heading').hidden=true;if(state.query)empty('No matching files','Try a different name, folder or tag.');else if(state.view==='trash')empty('Your trash is empty','Files you move to Trash can be restored here.');else if(state.view==='photos')empty('Your photos, together','Images in this library will appear here. Add a photo to get started.','Choose files',()=>$('file-picker').click());else if(state.view==='music')empty('Make room for your music','Audio files in this library will appear here.','Choose files',()=>$('file-picker').click());else empty(state.password===null?'Your library is ready':'Your next favourite thing belongs here',state.password===null?'Add files to choose a file password, or unlock existing packages. Browser uploads are buffered in memory, up to 128 MiB per file.':'Choose files or drop them here. Folders appear automatically when you move files into a folder path.','Choose files',()=>$('file-picker').click());}
  for(const item of items){const row=el('div','file-row'+(state.selected===item.id?' selected':''));row.setAttribute('role','listitem');if(item.id)row.dataset.recordId=item.id;
    const open=button('',()=>{if(item.kind==='folder'){state.folder=item.path;state.query='';$('files-search').value='';state.selected=null;renderFiles();}else{state.selected=item.id;renderFiles();}},'file-name');open.setAttribute('aria-label',(item.kind==='folder'?'Open folder ':'Select ')+item.name);
    const icon=el('span','file-icon '+(item.kind==='folder'?'folder':mediaClass(item.name)),item.kind==='folder'?'▰':mediaIcon(item.name));icon.setAttribute('aria-hidden','true');open.append(icon);const info=el('span','file-info');info.append(el('span','name',item.name));if(state.query||state.view!=='files')info.append(el('span','file-path',M.parent(item)||'All files'));open.append(info);row.append(open);
    if(item.kind==='folder'){row.append(el('span','file-size','Folder'),el('span','file-date','—'),el('span','row-action','›'));}
    else{row.append(el('span','file-size',fmtBytes(item.size)),el('span','file-date',fmtDate(item.updated)));const action=button('•••',()=>{state.selected=item.id;renderFiles();$('selection-actions').querySelector('button')?.focus();},'icon-button row-action');action.setAttribute('aria-label','Actions for '+item.name);row.append(action);}
    if(item.id)row.addEventListener('click',e=>{if(!e.target.closest('button')){state.selected=item.id;renderFiles();}});host.append(row);
  }
  $('file-count').textContent=items.length+' '+(items.length===1?'item':'items')+(state.password===null?' · Files locked':'');
}
function mediaClass(name) {const type=M.previewType(name);return type?.startsWith('image/')?'photo':type?.startsWith('audio/')?'audio':type?.startsWith('video/')?'video':'document';}
function mediaIcon(name) {return {photo:'▧',audio:'♫',video:'▷',document:'▤'}[mediaClass(name)];}
function renderSelection() {const record=state.records.find(r=>r.id===state.selected);$('selection-bar').hidden=!record;const actions=$('selection-actions');actions.replaceChildren();if(!record)return;$('selected-name').textContent=record.name;if(record.deleted){actions.append(button('Restore',()=>restoreRecord(record)));return;}actions.append(button('Download',()=>queueDownload(record,false)),button('Rename',()=>renameRecord(record)),button('Move',()=>moveRecord(record)),button('Move to Trash',()=>trashRecord(record)));if(M.previewType(record.name))actions.prepend(button('Preview',()=>queueDownload(record,true)));}
async function createLibrary() {const name=await ask({title:'Create library',label:'Library name',text:'Give this space a name. You’ll choose a file password when you add files.',confirm:'Create library'});if(!name)return;const result=await api('POST','/v1/vaults',{name});await refreshLibraries();await selectLibrary(result.vault.id);toast('Library created');}
async function renameLibrary() {if(!state.vault)return;const name=await ask({title:'Rename library',label:'Library name',value:state.vault.name,confirm:'Rename'});if(!name)return;const id=state.vault.id;await api('PATCH','/v1/vaults/'+encodeURIComponent(id),{name});await refreshLibraries();toast('Library renamed');}
async function updateRecord(record,changes) {const vault=state.vault.id;const result=await api('PUT',recordPath(record.id,vault),{base_version:record.version,operation:crypto.randomUUID(),name:record.name,path:record.path||'',tags:record.tags||[],package:record.package,size:record.size,...changes});const verified=await api('GET',recordPath(record.id,vault));if(verified.record.version<result.record.version)throw new Error('Could not verify the file change. Refresh before trying again.');await loadRecords();toast('File updated');}
async function renameRecord(record) {const name=await ask({title:'Rename file',label:'File name',value:record.name,confirm:'Rename',validate:v=>{if(M.safeName(v)!==v)throw new Error('Use a file name without slashes or reserved characters.');}});if(!name||name===record.name)return;try{await updateRecord(record,{name});}catch(e){if(e.status===409)await loadRecords();throw e;}}
async function moveRecord(record) {const path=await ask({title:'Move file',label:'Folder path',value:M.parent(record),required:false,text:'Enter a relative path such as Trips/Paris. Leave it empty for All files. A folder exists while it contains files; empty folders aren’t stored.',confirm:'Move',validate:M.safePath});if(path===null)return;try{await updateRecord(record,{path:M.safePath(path)});}catch(e){if(e.status===409)await loadRecords();throw e;}}
async function trashRecord(record) {const answer=await ask({title:'Move “'+record.name+'” to Trash?',text:'You can restore it from Trash. Its encrypted package stays in your storage.',confirm:'Move to Trash'});if(answer===null)return;const target=recordPath(record.id);await api('DELETE',target,{});const read=await api('GET',target);if(!read.record.deleted)throw new Error('Could not verify Trash. Refresh before trying again.');state.selected=null;await loadRecords();toast('Moved to Trash');}
async function restoreRecord(record) {const target=recordPath(record.id);await api('POST',target+'/restore',{});const read=await api('GET',target);if(read.record.deleted)throw new Error('Could not verify restore. Refresh before trying again.');state.selected=null;await loadRecords();toast('File restored');}

/* Password proof is a full authenticated native package decrypt, never a guessed account key. */
async function ensureUnlocked() {
  if(!state.vault)throw new Error('Create a library first.');
  if(!window.MvpPackage||!crypto.subtle)throw new Error('File encryption is unavailable. Use HTTPS and check that this server includes its encryption scripts.');
  if(state.loading)throw new Error('Wait for your file list to finish loading.');
  if(state.loadError)throw new Error('Refresh your file list before unlocking or adding files. An existing encrypted package must be checked first.');
  const first=state.records.find(r=>r.package);
  if(state.password!==null&&(!first||state.checkedPackage===first.package))return true;
  const password=state.password!==null?state.password:await ask({title:first?'Unlock files':'Choose your file password',label:'File password',password:true,confirm:'Unlock',text:first?'Use the password that encrypted this library’s existing packages. Your account password cannot recover it. We’ll check an existing encrypted file before allowing uploads.':'This password will encrypt new files in this library. Keep it safe: your account password cannot recover it. It stays in this page’s memory until you lock or sign out.'});
  if(password===null)return false;
  if(!password)throw new Error('Enter a file password.');
  const generation=state.generation,c=newController();
  $('lock-status').textContent=first?'Checking file password…':'Unlocking…';
  try{if(first){const bytes=await fetchCiphertext(first,state.vault.id,c.signal);checkGeneration(generation,c.signal);const entries=await window.MvpPackage.decrypt(bytes,password);for(const entry of entries)entry.bytes?.fill?.(0);checkGeneration(generation,c.signal);state.checkedPackage=first.package;}state.password=password;renderLock();renderFiles();toast('Files unlocked for this visit');return true;}
  catch(e){state.password=null;state.checkedPackage=null;renderLock();if(e.name==='AbortError')throw e;if(e.status)throw e;throw new Error('Could not unlock this library. Check the file password, package format and file size. Nothing was uploaded.');}
  finally{state.controllers.delete(c);}
}
async function fetchCiphertext(record,vault,signal,onProgress=()=>{}) {
  if(!/^[a-f0-9]{64}$/.test(record.package||''))throw new Error('This file does not have an encrypted package.');
  const max=Math.min(MAX_PACKAGE_BYTES,Number(state.capabilities.max_file_bytes)||MAX_PACKAGE_BYTES);
  if(record.size>max)throw new Error('This package exceeds the browser’s 128 MiB buffering limit. Use the native app.');
  const r=await request(libraryPath(vault)+'/blobs/'+record.package,{signal,headers:{Accept:'application/octet-stream'}});
  const total=Number(r.headers.get('Content-Length'))||0;if(total>max){await r.body?.cancel();throw new Error('This encrypted package is too large for browser buffering (128 MiB maximum). Use the native app.');}
  const reader=r.body?.getReader();if(!reader){const b=new Uint8Array(await r.arrayBuffer());if(b.length>max)throw new Error('Encrypted package exceeds the browser limit.');return b;}
  const chunks=[];let done=0;
  try{for(;;){const result=await reader.read();if(result.done)break;done+=result.value.length;if(done>max){await reader.cancel();throw new Error('Encrypted package exceeds the browser’s 128 MiB limit.');}chunks.push(result.value);onProgress(done,total||record.size);}}finally{reader.releaseLock();}
  if(total&&done!==total)throw new Error('The download was interrupted. Please retry.');
  const bytes=new Uint8Array(done);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.length;}return bytes;
}

/* Two bounded transfer slots. Each failure is isolated; retry reuses ciphertext and operation. */
function makeTransfer(name,kind,extra) {const t={id:crypto.randomUUID(),name,kind,status:'Queued',done:0,total:0,error:'',controller:null,generation:state.generation,...extra};state.transfers.push(t);renderTransfers();return t;}
async function addFiles(fileList) {
  const files=Array.from(fileList||[]);if(!files.length)return;
  if(!state.vault){toast('Create a library before adding files.',true);return;}
  await loadRecords();if(!await ensureUnlocked())return;
  const path=state.folder,vault=state.vault.id;
  for(const file of files){const t=makeTransfer(M.safeName(file.name),'upload',{file,path,vault});if(file.size>MAX_FILE_BYTES){t.status='Failed';t.error='This file exceeds the 128 MiB browser buffering limit. Use the native app.';t.file=null;}else{t.total=file.size;}}
  renderTransfers();pump();
}
async function queueDownload(record,preview) {if(!await ensureUnlocked())return;makeTransfer(record.name,preview?'preview':'download',{record:{...record},vault:state.vault.id});pump();}
function pump() {
  while(state.active<2){const t=state.transfers.find(x=>x.status==='Queued');if(!t)break;state.active++;t.controller=newController();t.status=t.kind==='upload'?'Preparing':'Downloading';renderTransfers();
    executeTransfer(t).then(()=>{checkGeneration(t.generation,t.controller.signal);t.status='Complete';t.error='';t.file=null;t.bytes=null;t.payload=null;}).catch(e=>{t.status=e.name==='AbortError'?'Cancelled':'Failed';t.error=e.name==='AbortError'?'':e.message;if(e.name==='AbortError'){t.file=null;t.bytes=null;t.payload=null;}if(e.status===401){clearSession();toast('Your session ended. Sign in again.',true);}}).finally(()=>{state.controllers.delete(t.controller);state.active--;t.controller=null;renderTransfers();pump();});
  }
}
function transferProgress(t,done,total) {if(t.generation!==state.generation)return;t.done=done;t.total=total;renderTransfers();}
async function executeTransfer(t) {
  const signal=t.controller.signal;checkGeneration(t.generation,signal);
  if(t.kind!=='upload')return downloadTransfer(t,signal);
  if(!t.payload){
    if(!t.bytes){t.status='Encrypting';renderTransfers();const file=new File([t.file],t.name,{type:t.file.type});t.bytes=await window.MvpPackage.encrypt(file,state.password,fraction=>{if(typeof fraction==='number')transferProgress(t,Math.round(Math.min(1,Math.max(0,fraction))*file.size),file.size);});checkGeneration(t.generation,signal);t.file=null;
      const max=Math.min(MAX_PACKAGE_BYTES,Number(state.capabilities.max_file_bytes)||MAX_PACKAGE_BYTES);if(t.bytes.length>max){t.bytes=null;throw new Error('The encrypted package exceeds this browser/server buffering limit. Try a smaller file or the native app.');}}
    if(!t.hash){t.status='Checking encrypted file';renderTransfers();const hash=await crypto.subtle.digest('SHA-256',t.bytes);checkGeneration(t.generation,signal);t.hash=Array.from(new Uint8Array(hash),b=>b.toString(16).padStart(2,'0')).join('');}
    await uploadCiphertext(t,signal);checkGeneration(t.generation,signal);
    t.recordId=t.recordId||crypto.randomUUID();t.payload={base_version:0,operation:crypto.randomUUID(),name:t.name,path:t.path,tags:[],package:t.hash,size:t.bytes.length};t.bytes=null;
  }
  t.status='Saving file';renderTransfers();const target=recordPath(t.recordId,t.vault);
  await api('PUT',target,t.payload,signal);const verified=await api('GET',target,undefined,signal);checkGeneration(t.generation,signal);
  if(verified.record.package!==t.hash||verified.record.deleted)throw new Error('The file commit could not be verified. Retry saving; the encrypted file will not be uploaded again.');
  const index=state.records.findIndex(r=>r.id===verified.record.id);if(index===-1)state.records.push(verified.record);else state.records[index]=verified.record;
  if(!state.checkedPackage)state.checkedPackage=t.hash;t.done=t.total;renderFiles();await refreshLibraries();
}
async function uploadCiphertext(t,signal) {
  const url=libraryPath(t.vault)+'/blobs/'+t.hash,total=t.bytes.length;
  const inspect=async()=>{try{const head=await request(url,{method:'HEAD',signal});const n=Number(head.headers.get('X-Offset'));if(!Number.isSafeInteger(n)||n<0||n>total||Number(head.headers.get('Content-Length'))!==total)throw new Error('The upload resume information is invalid.');return {offset:n,complete:head.headers.get('X-Complete')==='true'};}catch(e){if(e.status===404)return {offset:0,complete:false};throw e;}};
  t.status='Uploading';
  await M.resumeUpload(t.bytes,{inspect,signal,chunkBytes:Math.min(CHUNK_BYTES,Number(state.capabilities.chunk_bytes)||CHUNK_BYTES),put:(offset,chunk)=>apiBinary(url+'?offset='+offset,chunk,total,signal),onProgress:(done,size)=>transferProgress(t,done,size)});
}
async function apiBinary(path,body,total,signal) {return request(path,{method:'PUT',body,signal,headers:{'Content-Type':'application/octet-stream','X-Total-Size':String(total)}});}
async function downloadTransfer(t,signal) {
  const bytes=await fetchCiphertext(t.record,t.vault,signal,(n,total)=>transferProgress(t,n,total));checkGeneration(t.generation,signal);
  t.status='Authenticating and decrypting';renderTransfers();const entries=await window.MvpPackage.decrypt(bytes,state.password);
  if(!entries.length)throw new Error('The authenticated package contains no files.');
  try{checkGeneration(t.generation,signal);if(t.kind==='preview'){
    if(entries.length!==1)throw new Error('This package contains multiple files. Use Download to save its contents.');
    const type=M.previewType(t.record.name);if(!type)throw new Error('This file type cannot be previewed safely. Use Download.');
    const url=URL.createObjectURL(new Blob([entries[0].bytes],{type}));closePreview();state.urls.add(url);$('preview-title').textContent=t.record.name;const content=$('preview-content');let node;
    if(type.startsWith('image/')){node=el('img');node.alt=t.record.name;}
    else if(type.startsWith('audio/')){node=el('audio');node.controls=true;}
    else if(type.startsWith('video/')){node=el('video');node.controls=true;node.preload='metadata';}
    else{node=el('iframe');node.title='PDF preview: '+t.record.name;node.setAttribute('sandbox','');}
    node.src=url;content.replaceChildren(node);$('preview-dialog').showModal();$('close-preview').focus();
  }else{
    for(const entry of entries){checkGeneration(t.generation,signal);const name=M.safeName(entries.length===1?t.record.name:entry.name);const url=URL.createObjectURL(new Blob([entry.bytes],{type:'application/octet-stream'}));state.urls.add(url);const link=el('a');link.href=url;link.download=name;document.body.append(link);link.click();link.remove();setTimeout(()=>{URL.revokeObjectURL(url);state.urls.delete(url);},60000);}
  }}finally{for(const entry of entries)entry.bytes?.fill?.(0);}
  t.done=t.total;
}
function renderTransfers() {
  const host=$('transfer-list');host.replaceChildren();$('transfer-tray').hidden=!state.transfers.length;
  const running=state.transfers.filter(t=>!['Complete','Failed','Cancelled'].includes(t.status));$('transfer-summary').textContent=running.length?running.length+' in progress':state.transfers.some(t=>t.status==='Failed')?'Some transfers need attention':'Transfers finished';
  for(const t of state.transfers){const row=el('div','transfer-row');const info=el('div','transfer-info');info.append(el('strong',null,t.name));const text=t.status+(t.total?' · '+fmtBytes(t.done)+' / '+fmtBytes(t.total):'');info.append(el('span',t.status==='Failed'?'error-text':'hint',text));if(t.error)info.append(el('p','error-text',t.error));row.append(info);const progress=el('progress');progress.max=t.total||1;progress.value=t.status==='Complete'?progress.max:t.done;progress.setAttribute('aria-label',t.name+' transfer progress');row.append(progress);
    if(!['Complete','Failed','Cancelled'].includes(t.status))row.append(button('Cancel',()=>{t.controller?.abort();t.status='Cancelled';t.file=null;t.bytes=null;t.payload=null;renderTransfers();}));
    if(t.status==='Failed'&&t.generation===state.generation&&(t.kind!=='upload'||t.bytes||t.payload||t.file))row.append(button(t.payload?'Retry save':'Retry',()=>{if(state.password===null)throw new Error('Unlock files before retrying.');t.status='Queued';t.error='';renderTransfers();pump();}));host.append(row);
  }
}
async function loadDevices() {const data=await api('GET','/auth/token');state.devices=data.tokens||[];renderDevices();}
function renderDevices() {const host=$('device-list');host.replaceChildren();if(!state.devices.length){host.append(el('h2',null,'No app connections yet'),el('p','hint','Connect your phone or computer using the setup steps below.'));return;}for(const device of state.devices){const row=el('div','device-row');const info=el('div');info.append(el('strong',null,device.label||'Unnamed device'),el('p','hint',(device.revoked?'Revoked':'Connected')+' · added '+fmtDate(device.created)));row.append(info);if(!device.revoked){row.append(button('Rename',async()=>{const label=await ask({title:'Rename device',label:'Device name',value:device.label,confirm:'Rename'});if(!label)return;await api('PATCH','/auth/token/'+encodeURIComponent(device.id),{label});await loadDevices();toast('Device renamed');}),button('Revoke',async()=>{if(await ask({title:'Revoke “'+(device.label||'this device')+'”?',text:'This stops the app’s access. It needs a new connection to use this server again.',confirm:'Revoke'})===null)return;await api('DELETE','/auth/token/'+encodeURIComponent(device.id),{});await loadDevices();toast('Device revoked');}));}host.append(row);}}
function bind() {
  $('tab-login').addEventListener('click',()=>showGate());$('tab-register').addEventListener('click',()=>showGate('register'));$('switch-button').addEventListener('click',()=>showGate('register'));
  for(const mode of ['login','register'])$(mode+'-form').addEventListener('submit',e=>{e.preventDefault();const form=e.currentTarget;if(mode==='register'&&form.elements.password.value!==form.elements.confirm.value){toast('The passwords do not match.',true);return;}const submit=form.querySelector('button[type=submit]');submit.disabled=true;run(async()=>{const body={email:form.elements.email.value.trim(),password:form.elements.password.value};try{if(mode==='register')await api('POST','/auth/register',body);await api('POST','/auth/login',body);form.reset();await loadSession();}finally{body.password='';}}).finally(()=>submit.disabled=false);});
  $('signout').addEventListener('click',()=>run(async()=>{lockFiles(false);try{await api('POST','/auth/logout',{});await verifySignedOut();}finally{clearSession();}toast('Signed out');}));
  for(const b of document.querySelectorAll('.nav-item'))b.addEventListener('click',()=>setView(b.dataset.view));
  $('library-select').addEventListener('change',e=>run(()=>selectLibrary(e.target.value)));$('create-library').addEventListener('click',()=>run(createLibrary));$('rename-library').addEventListener('click',()=>run(renameLibrary));
  $('lock-library').addEventListener('click',()=>run(async()=>{if(state.password!==null)lockFiles();else await ensureUnlocked();}));$('settings-lock').addEventListener('click',()=>lockFiles());
  $('add-files').addEventListener('click',()=>$('file-picker').click());$('file-picker').addEventListener('change',e=>{const files=Array.from(e.target.files);e.target.value='';run(()=>addFiles(files));});
  $('files-search').addEventListener('input',e=>{state.query=e.target.value;state.selected=null;renderFiles();});$('files-sort').addEventListener('change',e=>{state.sort=e.target.value;renderFiles();});
  for(const layout of ['list','grid'])$(layout+'-view').addEventListener('click',()=>{state.layout=layout;for(const l of ['list','grid']){$(l+'-view').classList.toggle('is-active',layout===l);$(l+'-view').setAttribute('aria-pressed',String(layout===l));}renderFiles();});
  $('clear-selection').addEventListener('click',()=>{state.selected=null;renderFiles();});$('refresh-files').addEventListener('click',()=>run(loadRecords));
  let depth=0;const drop=$('drop-zone');drop.addEventListener('dragenter',e=>{e.preventDefault();depth++;drop.classList.add('dragging');});drop.addEventListener('dragover',e=>{e.preventDefault();e.dataTransfer.dropEffect='copy';});drop.addEventListener('dragleave',()=>{if(--depth<=0){depth=0;drop.classList.remove('dragging');}});drop.addEventListener('drop',e=>{e.preventDefault();depth=0;drop.classList.remove('dragging');if(state.view==='trash'){toast('Switch to Files before adding files.',true);return;}run(()=>addFiles(e.dataTransfer.files));});
  window.addEventListener('dragover',e=>{if(e.dataTransfer.types.includes('Files'))e.preventDefault();});window.addEventListener('drop',e=>e.preventDefault());
  $('close-preview').addEventListener('click',closePreview);$('preview-dialog').addEventListener('close',()=>{if(!$('preview-dialog').open)closePreview();});
  $('clear-transfers').addEventListener('click',()=>{state.transfers=state.transfers.filter(t=>!['Complete','Failed','Cancelled'].includes(t.status));renderTransfers();});
  $('copy-url').addEventListener('click',()=>copy(location.origin));$('dismiss-token').addEventListener('click',()=>{$('token-value').textContent='';$('new-token').hidden=true;$('copy-token').onclick=null;});
  $('token-form').addEventListener('submit',e=>{e.preventDefault();const form=e.currentTarget;run(async()=>{const issued=await api('POST','/auth/token',{label:form.elements.label.value.trim()});form.reset();$('token-value').textContent=issued.token;$('new-token').hidden=false;$('copy-token').onclick=()=>copy(issued.token);await loadDevices();});});
  $('password-form').addEventListener('submit',e=>{e.preventDefault();const form=e.currentTarget;run(async()=>{await api('POST','/auth/password',{current_password:form.elements.current.value,new_password:form.elements.next.value});form.reset();const session=await api('GET','/auth/session');state.csrf=session.csrf||state.csrf;toast('Account password updated. Other browsers have been signed out.');});});
  $('signout-all').addEventListener('click',()=>run(async()=>{if(await ask({title:'Sign out everywhere?',text:'Every browser session ends, including this one.',confirm:'Sign out everywhere'})===null)return;await api('DELETE','/auth/sessions',{});await verifySignedOut();clearSession();toast('Signed out everywhere');}));
  $('delete-account').addEventListener('click',()=>run(async()=>{const password=await ask({title:'Permanently delete this account?',text:'All libraries and their files on this server will be removed. This cannot be undone. Enter your account password to confirm.',label:'Account password',password:true,confirm:'Delete everything'});if(password===null)return;await api('DELETE','/auth/account',{password});await verifySignedOut();clearSession();toast('Account deleted');}));
  $('delete-library').addEventListener('click',()=>run(async()=>{if(!state.vault)return;const vault=state.vault;if(await ask({title:'Delete “'+vault.name+'” permanently?',text:'Every file in this server library will be removed. This cannot be undone.',confirm:'Delete library'})===null)return;lockFiles(false);await api('DELETE','/v1/vaults/'+encodeURIComponent(vault.id),{});await refreshLibraries();if(state.vaults.some(v=>v.id===vault.id))throw new Error('Could not verify library deletion. Refresh before trying again.');await selectLibrary(state.vault?.id);toast('Library deleted');}));
  $('theme-select').addEventListener('change',e=>{document.documentElement.dataset.theme=e.target.value;try{localStorage.setItem('mvp-theme',e.target.value);}catch(_){}});
  const network=()=>{$('connection').hidden=navigator.onLine;$('auth-status').textContent=navigator.onLine?'Your account password and file password are separate.':'You’re offline. Reconnect to sign in.';};window.addEventListener('online',network);window.addEventListener('offline',network);network();
  window.addEventListener('pagehide',()=>lockFiles(false));
}
async function start() {
  try{const theme=localStorage.getItem('mvp-theme');if(['system','light','dark'].includes(theme)){document.documentElement.dataset.theme=theme;$('theme-select').value=theme;}}catch(_){}
  bind();try{const health=await api('GET','/healthz');state.registration=health.registration||'closed';$('tab-register').hidden=state.registration!=='open';$('signup-hint').hidden=state.registration!=='open';$('tab-register').parentElement.classList.toggle('is-single',state.registration!=='open');await loadSession();}catch(e){if(!state.account)showGate();if(e.status!==401)toast(e.message||'Could not connect to your server.',true);}
}
start();
