#include "videovault/app/sync_engine.hpp"
#include "videovault/app/sync_protocol.hpp"
#include "videovault/app/sync_state.hpp"
#include "videovault/app/sync_package.hpp"
#include "videovault/core/vault.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslError>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QLockFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QThread>
#include <QElapsedTimer>
#include <sodium.h>
#include <stdexcept>
#include <set>
#include <map>
namespace videovault::app {
using namespace sync;
namespace {
std::filesystem::path fsPath(const QString& p) {
#ifdef Q_OS_WIN
 return std::filesystem::path(p.toStdWString());
#else
 return std::filesystem::u8path(p.toUtf8().constData());
#endif
}
QString qtPath(const std::filesystem::path& p) {
#ifdef Q_OS_WIN
 return QString::fromStdWString(p.wstring());
#else
 auto utf8=p.u8string(); return QString::fromUtf8(reinterpret_cast<const char*>(utf8.c_str()));
#endif
}
void error(const QString& s) { throw std::runtime_error(s.toUtf8().constData()); }
template<class T> T required(core::Result<T> r) { if(!r) error(QString::fromUtf8(core::user_message(r.error().code).data(),int(core::user_message(r.error().code).size()))); return std::move(r.value()); }
QJsonArray array(const QStringList& list) { QJsonArray a; for(const auto& x:list) a.append(x); return a; }
QStringList strings(const QJsonArray& a) { QStringList s; for(const auto& v:a) s.append(v.toString()); return s; }
struct Response { int code{}; QByteArray bytes; QList<QNetworkReply::RawHeaderPair> headers; QJsonObject json() const { QJsonParseError e; auto d=QJsonDocument::fromJson(bytes,&e); if(e.error!=QJsonParseError::NoError || !d.isObject()) error("Server returned invalid JSON."); return d.object(); } QByteArray header(const QByteArray& k) const { for(auto h:headers) if(h.first.toLower()==k.toLower()) return h.second; return {}; } };
}
QString SyncEngine::relativePath(const core::VideoInfo& v,const std::vector<core::FolderInfo>& folders) {
 QStringList parts{QString::fromStdString(v.display_name)}; auto id=v.folder_id; std::set<std::int64_t> visited;
 while(id) { if(!visited.insert(id).second) error("Local folder cycle; sync stopped."); bool found=false; for(const auto& f:folders) if(f.id==id) { parts.prepend(QString::fromStdString(f.name)); id=f.parent_id; found=true; break; } if(!found) error("Local folder is missing; sync stopped."); }
 return parts.join('/');
}
QString SyncEngine::fingerprint(const core::VideoInfo& v,const std::vector<core::FolderInfo>& folders) {
 QJsonArray tags; for(const auto& t:v.tags) tags.append(QString::fromStdString(t));
 QJsonArray values{QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(v.package_sha256.data()),32).toHex()),relativePath(v,folders),tags};
 return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
class SyncSession {
public:
 std::shared_ptr<QLockFile> lock;
 ~SyncSession() { if(!recovery.isEmpty()) { recovery.detach(); sodium_memzero(recovery.data(),recovery.size()*sizeof(QChar)); } }
 std::shared_ptr<core::Vault> vault; QString directory,recovery,status,infoTitle,infoBody; QJsonObject state; QJsonArray catalog; std::shared_ptr<std::atomic_bool> stop; bool failed=false,auth=false,library=false;
 void check() { if(stop->load() || !vault->is_unlocked()) error("Sync paused: unlock the local vault and enter its recovery key to resume."); }
 void save() { check(); saveState(directory+"/state.enc",recovery,state); }
 Response request(const QByteArray& method,const QString& path,const QByteArray& body={},const QList<QPair<QByteArray,QByteArray>>& headers={},bool authorized=true) {
  check(); QUrl base(state["url"].toString()); if(!validServer(base)) error("Invalid server URL; use HTTPS (HTTP is permitted only for literal loopback).");
  base.setPath(path.section('?',0,0)); base.setQuery(path.contains('?')?path.section('?',1):QString());
  QNetworkAccessManager manager; QNetworkRequest req(base);
  // Never follow redirects, particularly with a device bearer token.
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
  req.setRawHeader("Content-Type",method=="PUT"?"application/octet-stream":"application/json");
  if(authorized) { auto token=state["token"].toString(); if(!isHex(token,64)) error("Pair this device first."); req.setRawHeader("Authorization","Bearer "+token.toLatin1()); }
  for(auto h:headers) req.setRawHeader(h.first,h.second);
  QNetworkReply* reply=nullptr;
  // QNAM must know HEAD semantics: sendCustomRequest("HEAD") may wait for a
  // Content-Length body that a valid HEAD response deliberately never sends.
  if(method=="HEAD") reply=manager.head(req);
  else if(method=="GET") reply=manager.get(req);
  else if(method=="POST") reply=manager.post(req,body);
  else if(method=="PUT") reply=manager.put(req,body);
  else reply=manager.sendCustomRequest(req,method,body);
  QEventLoop loop; QTimer deadline,cancel; deadline.setSingleShot(true); deadline.setInterval(20000); cancel.setInterval(100);
  QByteArray received; bool tooLarge=false,pinMismatch=false,timedOut=false; const int limit=path.startsWith("/v2/blobs/")?2*1024*1024:16*1024*1024;
  auto read=[&] { auto b=reply->readAll(); if(received.size()+b.size()>limit) { tooLarge=true; reply->abort(); } else received+=b; };
  QObject::connect(reply,&QIODevice::readyRead,&loop,read);
  const auto pin=state["pin"].toString();
  auto matches=[&] { return !pin.isEmpty() && QString::fromLatin1(reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256).toHex())==pin; };
  QObject::connect(reply,&QNetworkReply::encrypted,&loop,[&] { if(!pin.isEmpty() && !matches()) { pinMismatch=true; reply->abort(); } });
  QObject::connect(reply,QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),&loop,[&](const QList<QSslError>&) {
   if(matches()) reply->ignoreSslErrors();
   else if(!pin.isEmpty()) { pinMismatch=true; reply->abort(); }
  });
  QObject::connect(&deadline,&QTimer::timeout,&loop,[&] { timedOut=true; reply->abort(); });
  QObject::connect(&cancel,&QTimer::timeout,&loop,[&] { if(stop->load() || !vault->is_unlocked()) reply->abort(); });
  QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
  deadline.start(); cancel.start(); loop.exec(); read(); check();
  Response r; r.code=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); r.bytes=received; r.headers=reply->rawHeaderPairs();
  if(tooLarge) error("Server response exceeds the protocol limit.");
  if(pinMismatch) error("TLS certificate pin mismatch. Verify server identity before re-pairing.");
  if(timedOut) error("Server timed out; encrypted queue retained for retry.");
  if(r.code==401) { auth=true; error("Device token revoked or expired. Re-pair this device; recovery key and pending work stay local."); }
  if(r.code==409 || r.code==404) return r;
  if(reply->error()!=QNetworkReply::NoError || r.code<200 || r.code>=300) error(QString("Sync request failed (HTTP %1). Check TLS, connectivity, server quota and Retry.").arg(r.code));
  if(state["bandwidth"].toInt()>0 && path.startsWith("/v2/blobs/") && (method=="PUT" || method=="GET")) {
   // Pace encrypted blob traffic only. Control and pairing remain responsive.
   int ms=int((qint64(body.size()+r.bytes.size())*1000)/(state["bandwidth"].toInt()*1024LL));
   while(ms>0) { check(); QThread::msleep(qMin(ms,100)); ms-=100; }
  }
  return r;
 }
 void pair(const QUrl& url,const QString& pin,const QString& name,const QString& code) {
  if(!validServer(url) || (!pin.isEmpty() && (!isHex(pin,64) || url.scheme()!="https"))) error("Enter a valid HTTPS origin and optional 64 lowercase hex certificate SHA-256 pin.");
  if(code.trimmed().isEmpty() || name.trimmed().isEmpty()) error("Enter a one-use pairing code and non-sensitive device label.");
  if(!state["url"].toString().isEmpty() && QUrl(state["url"].toString())!=url) error("This vault is bound to another server. Do not switch a durable queue to another relay; use a separate vault/profile.");
  state["url"]=url.toString(); state["pin"]=pin;
  auto health=request("GET","/healthz",{},{},false).json(); if(health["protocol"].toInt()!=2 || !health["ok"].toBool()) error("This is not a sync v2 relay. Legacy vault-password cloud login is disabled.");
  auto paired=request("POST","/v2/pair",QJsonDocument(QJsonObject{{"code",code},{"name",name}}).toJson(QJsonDocument::Compact),{},false).json();
  if(!isHex(paired["device_id"].toString(),32) || !isHex(paired["token"].toString(),64)) error("Pairing response is invalid.");
  state["device"]=paired["device_id"]; state["token"]=paired["token"]; state["device_name"]=name; save(); status="Device paired. Select local files and remote object IDs to sync.";
 }
 void manifest() {
  auto doc=request("GET","/v2/objects").json(); if(!doc["objects"].isArray() || doc["objects"].toArray().size()>10000) error("Invalid or oversized remote manifest.");
  catalog=doc["objects"].toArray(); std::set<QString> ids; auto high=state["highest"].toObject(); auto acknowledged=state["maps"].toObject();
  for(auto v:catalog) { if(!v.isObject() || !validRecord(v.toObject())) error("Invalid remote object record."); auto r=v.toObject(); auto id=r["id"].toString(); if(!ids.insert(id).second) error("Duplicate remote object id."); if(!acceptRevision(r,high[id].toDouble(),acknowledged[id].toObject()["revision"].toDouble())) error("Remote revision rollback detected. Local copies and queue preserved."); high[id]=r["revision"]; }
  for(auto it=acknowledged.begin();it!=acknowledged.end();++it) if(!ids.count(it.key())) error("An acknowledged object is missing from the relay. Possible rollback; local originals preserved.");
  for(auto it=high.begin();it!=high.end();++it) if(!ids.count(it.key())) error("Previously observed remote object is missing. Possible rollback; local copies preserved.");
  state["highest"]=high; save();
 }
 void stage() {
  auto videos=required(vault->list_videos()); auto folders=required(vault->list_folders()); auto maps=state["maps"].toObject(); auto queue=state["queue"].toArray(); auto selected=strings(state["selected_local"].toArray()); auto suppressed=state["suppressed"].toObject();
  for(const auto& v:videos) {
   check(); auto local=QString::number(v.id); if(!selected.contains(local)) continue;
   bool pending=false; for(auto p:queue) if(p.toObject()["local"].toString()==local) pending=true; if(pending) continue;
   auto fp=SyncEngine::fingerprint(v,folders); if(suppressed[local].toString()==fp) continue;
   QString id; QJsonObject mapping; for(auto it=maps.begin();it!=maps.end();++it) if(it.value().toObject()["local"].toString()==local) { id=it.key(); mapping=it.value().toObject(); break; }
   if(mapping["fingerprint"].toString()==fp) continue;
   if(id.isEmpty()) id=randomHex(); auto op=randomHex(); auto name=op+".mvpk"; auto partial=directory+"/"+name+".staging";
   required(vault->export_package(fsPath(partial),recovery.toStdString(),{{v.id,SyncEngine::relativePath(v,folders).toStdString()}},{},false,[this](double){check();})); check();
   // Verify local metadata did not change during export; next retry snapshots it again.
   auto after=required(vault->list_videos()); bool same=false; auto afterFolders=required(vault->list_folders()); for(auto item:after) if(item.id==v.id) same=SyncEngine::fingerprint(item,afterFolders)==fp;
   if(!same) { QFile::remove(partial); error("Local item changed while staging; retry to snapshot its current version."); }
   QFile f(partial); if(!f.open(QIODevice::ReadOnly)) error("Cannot read staged encrypted package.");
   QSaveFile durable(directory+"/"+name); durable.setDirectWriteFallback(false); if(!durable.open(QIODevice::WriteOnly)) error("Cannot persist encrypted queue package."); durable.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
   while(!f.atEnd()) { check(); auto b=f.read(1024*1024); if(b.isEmpty() && f.error()!=QFile::NoError) error("Read failure staging queue"); if(durable.write(b)!=b.size()) error("Disk full staging encrypted queue"); }
   if(!durable.commit()) error("Cannot durably commit encrypted package."); f.close(); QFile::remove(partial);
   auto blob=fileHash(directory+"/"+name); auto size=QFileInfo(directory+"/"+name).size();
   queue.append(QJsonObject{{"id",id},{"local",local},{"fingerprint",fp},{"base",mapping["revision"].toDouble()},{"operation",op},{"path",name},{"blob",blob},{"size",double(size)}});
   state["queue"]=queue; save();
  }
 }
 void upload(QJsonObject p) {
  auto path=directory+"/"+p["path"].toString(); const auto blob=p["blob"].toString(); const auto size=qint64(p["size"].toDouble());
  if(!isHex(blob,64) || QFileInfo(path).size()!=size || fileHash(path)!=blob) error("Queued encrypted package is missing or corrupt. Restore its backup; do not discard pending local edits.");
  auto head=request("HEAD","/v2/blobs/"+blob); qint64 offset=0;
  if(head.code!=404) { bool ok=false; offset=head.header("X-Upload-Offset").toLongLong(&ok); if(!ok || offset<0 || offset>size) error("Invalid server upload offset."); if(head.header("X-Blob-Complete")=="true" && offset!=size) error("Invalid completed blob size."); }
  QFile f(path); if(!f.open(QIODevice::ReadOnly)) error("Cannot open queued encrypted package."); int mismatches=0;
  while(offset<size) {
   check(); if(!f.seek(offset)) error("Cannot seek queued package."); auto chunk=f.read(qMin<qint64>(1024*1024,size-offset)); if(chunk.isEmpty()) error("Encrypted queue read failed.");
   auto r=request("PUT",QString("/v2/blobs/%1?offset=%2").arg(blob).arg(offset),chunk,{{"X-Blob-Size",QByteArray::number(size)}}); auto json=r.json();
   bool ok=json["offset"].isDouble(); auto next=qint64(json["offset"].toDouble()); if(!ok || double(next)!=json["offset"].toDouble() || next<0 || next>size) error("Invalid upload acknowledgement.");
   if(r.code==409) { if(++mismatches>3) error("Repeated upload offset conflict; retry later."); offset=next; continue; }
   if(next!=offset+chunk.size() && !(next==size && json["complete"].toBool())) error("Upload acknowledgement skipped bytes."); offset=next;
   if(offset==size && !json["complete"].toBool()) error("Server did not finalize encrypted blob.");
  }
 }
 void publish() {
  while(!state["queue"].toArray().isEmpty()) {
   check(); auto queue=state["queue"].toArray(); auto p=queue.first().toObject(); upload(p);
   QJsonObject payload{{"base_revision",p["base"]},{"blob",p["blob"]},{"size",p["size"]},{"deleted",false},{"operation_id",p["operation"]}};
   auto r=request("POST","/v2/objects/"+p["id"].toString(),QJsonDocument(payload).toJson(QJsonDocument::Compact));
   if(r.code==409) {
    auto current=r.json()["current"].toObject(); if(!validRecord(current) || current["id"]!=p["id"]) error("Invalid conflict response; encrypted queue retained.");
    // Archive original request verbatim; only its edited copy gets a new identity.
    auto archive=state["conflicts"].toArray(); archive.append(p); state["conflicts"]=archive;
    auto copy=conflictCopy(p,randomHex(),randomHex()); queue[0]=copy; state["queue"]=queue;
    auto maps=state["maps"].toObject(); auto old=maps[p["id"].toString()].toObject(); old.remove("local"); maps[p["id"].toString()]=old; state["maps"]=maps;
    save(); status="Conflict copy preserved and published separately; remote history and local originals kept."; continue;
   }
   auto committed=r.json(); if(!validRecord(committed) || committed["id"]!=p["id"] || committed["blob"]!=p["blob"] || committed["size"]!=p["size"] || committed["revision"].toDouble()!=p["base"].toDouble()+1) error("Invalid object commit acknowledgement; retry uses the same operation ID.");
   auto maps=state["maps"].toObject(); maps[p["id"].toString()]=QJsonObject{{"local",p["local"]},{"fingerprint",p["fingerprint"]},{"revision",committed["revision"]},{"blob",p["blob"]}}; state["maps"]=maps;
   queue.removeAt(0); state["queue"]=queue; save();
   bool retained=false; for(auto c:state["conflicts"].toArray()) if(c.toObject()["path"]==p["path"]) retained=true;
   if(!retained) QFile::remove(directory+"/"+p["path"].toString());
  }
 }
 void download(const QJsonObject& r) {
  auto id=r["id"].toString(); auto blob=r["blob"].toString(); auto size=qint64(r["size"].toDouble()); auto path=directory+"/download-"+blob+".mvpk";
  QFile f(path); if(!f.open(QIODevice::ReadWrite)) error("Cannot open encrypted download staging file."); f.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
  if(f.size()>size) { f.close(); error("Encrypted download cache size is invalid; preserve it for diagnosis or remove this blob manually before retry."); }
  auto offset=f.size();
  while(offset<size) {
   check(); auto end=qMin<qint64>(offset+1024*1024-1,size-1);
   auto reply=request("GET","/v2/blobs/"+blob,{},{{"Range",QString("bytes=%1-%2").arg(offset).arg(end).toLatin1()}});
   if(reply.code!=206 || reply.header("Content-Range")!=QString("bytes %1-%2/%3").arg(offset).arg(end).arg(size).toLatin1() || reply.bytes.size()!=end-offset+1) error("Invalid ranged download response; encrypted partial retained.");
   if(!f.seek(offset) || f.write(reply.bytes)!=reply.bytes.size() || !f.flush()) error("Cannot persist encrypted download; check free space."); offset=end+1;
  }
  f.close(); if(fileHash(path)!=blob) { QFile::remove(path); error("Downloaded encrypted blob hash mismatch; local originals unchanged. Retry downloads it again."); }
  // Never advance revision until authentication/import and exact ID resolution succeed.
  auto identity=inspectPackage(path,recovery); check();
  auto before=required(vault->list_videos()); auto foldersBefore=required(vault->list_folders()); std::vector<std::int64_t> imported;
  library=true;
  required(vault->import_package(fsPath(path),recovery.toStdString(),0,[this](double){check();},[&](std::int64_t local,const std::string&,std::uint64_t,std::int64_t) { imported.push_back(local); })); check(); library=true;
  auto after=required(vault->list_videos()); auto folders=required(vault->list_folders());
  if(imported.empty()) {
   // Core's content dedup may return zero without callbacks. Authenticate/hash the
   // one-file MVPK, then compare actual local content in bounded memory; never infer
   // identity from a name, size alone, or unrelated gallery differences.
   for(const auto& v:after) {
    if(v.original_size!=identity.size) continue;
    QCryptographicHash hash(QCryptographicHash::Sha256); quint64 offset=0;
    while(offset<v.original_size) { check(); auto bytes=required(vault->read_video_range(v.id,offset,std::size_t(qMin<quint64>(1024*1024,v.original_size-offset)))); if(bytes.empty()) error("Cannot resolve deduplicated local content."); hash.addData(reinterpret_cast<const char*>(bytes.data()),int(bytes.size())); offset+=bytes.size(); sodium_memzero(bytes.data(),bytes.size()); }
    if(QString::fromLatin1(hash.result().toHex())==identity.plaintextHash) imported.push_back(v.id);
   }
   std::sort(imported.begin(),imported.end()); imported.erase(std::unique(imported.begin(),imported.end()),imported.end());
  }
  if(imported.size()!=1) error("Package imported/deduplicated but exact local ID cannot be resolved safely. Originals preserved; revision not acknowledged. Choose a known copy or recover from an independent vault.");
  auto maps=state["maps"].toObject(); auto previous=maps[id].toObject(); auto suppressed=state["suppressed"].toObject();
  if(!previous["local"].toString().isEmpty() && previous["local"].toString()!=QString::number(imported.front())) {
   // Old content remains in the library and is not echoed as a new object.
   for(auto v:before) if(QString::number(v.id)==previous["local"].toString()) suppressed[previous["local"].toString()]=SyncEngine::fingerprint(v,foldersBefore);
  }
  QString fp; for(auto v:after) if(v.id==imported.front()) fp=SyncEngine::fingerprint(v,folders); if(fp.isEmpty()) error("Imported local ID missing; revision not acknowledged.");
  maps[id]=QJsonObject{{"local",QString::number(imported.front())},{"fingerprint",fp},{"revision",r["revision"]},{"blob",blob}}; state["maps"]=maps; state["suppressed"]=suppressed;
  auto local=strings(state["selected_local"].toArray()); if(!local.contains(QString::number(imported.front()))) local.append(QString::number(imported.front())); state["selected_local"]=array(local); save(); QFile::remove(path);
 }
 void receive() {
  auto selected=strings(state["selected_remote"].toArray()); auto maps=state["maps"].toObject();
  // Choosing a local file selects its acknowledged object bidirectionally.
  // Unknown remote IDs still require explicit selection: never download-all.
  auto localSelected=strings(state["selected_local"].toArray());
  for(auto it=maps.begin();it!=maps.end();++it) if(localSelected.contains(it.value().toObject()["local"].toString()) && !selected.contains(it.key())) selected.append(it.key());
  for(auto v:catalog) { auto r=v.toObject(); auto id=r["id"].toString(); if(!selected.contains(id)) continue; auto previous=maps[id].toObject(); if(r["revision"].toDouble()<=previous["revision"].toDouble()) continue;
   if(r["deleted"].toBool()) { auto notices=state["deletions"].toObject(); if(state["delete_ack"].toObject()[id].toDouble()<r["revision"].toDouble()) notices[id]=r["revision"]; state["deletions"]=notices; save(); continue; }
   download(r); maps=state["maps"].toObject();
  }
 }
 void run() {
  check(); save(); stage(); publish(); manifest(); receive();
  if(!state["queue"].toArray().isEmpty()) error("Pending encrypted uploads remain.");
  if(!state["deletions"].toObject().isEmpty()) status="Remote deletion notice: local copies preserved. Acknowledge the notice explicitly (no local files will be removed).";
  else if(!state["conflicts"].toArray().isEmpty()) status="Selected files synced; conflict copies and original queued bytes preserved. Tags are local-only.";
  else status="Selected files synced. Names and folders included; tags are local-only.";
 }
};
SyncEngine::SyncEngine(std::shared_ptr<core::Vault> vault,const QString& directory,QObject* parent):QObject(parent),vault_(std::move(vault)),stop_(std::make_shared<std::atomic_bool>(false)) {
 auto canonical=QFileInfo(qtPath(vault_->root_path())).canonicalFilePath(); if(canonical.isEmpty()) canonical=qtPath(vault_->root_path());
 directory_=directory.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/sync-v2/"+QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(),QCryptographicHash::Sha256).toHex()):directory;
 if(!QDir().mkpath(directory_)) { status_="Cannot create private sync state directory."; failed_=true; }
 QFile::setPermissions(directory_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 lock_=std::make_shared<QLockFile>(directory_+"/session.lock"); lock_->setStaleLockTime(0); if(!lock_->tryLock(0)) { status_="Another process owns this vault's sync state. Close it before syncing."; failed_=true; }
 timer_.setSingleShot(true); connect(&timer_,&QTimer::timeout,this,&SyncEngine::syncNow);
}
SyncEngine::~SyncEngine() { stop_->store(true); timer_.stop(); if(!recovery_.isEmpty()) { recovery_.detach(); sodium_memzero(recovery_.data(),recovery_.size()*sizeof(QChar)); } }
bool SyncEngine::unlock(const QString& recovery) {
 if(busy_ || !vault_->is_unlocked()) return false;
 if(!lock_->isLocked() && !lock_->tryLock(0)) { status_="Another process still owns this vault's sync state. Retry loading the key after it closes."; failed_=true; emit changed(); return false; }
 try { state_=loadState(directory_+"/state.enc",recovery); recovery_=recovery; failed_=false; stop_->store(false); status_="Ready. Pair or Retry to resume the durable encrypted queue."; emit changed(); return true; }
 catch(const std::exception& e) { status_=QString::fromUtf8(e.what()); failed_=true; emit changed(); return false; }
}
void SyncEngine::launch(const std::function<void(SyncSession&)>& action) {
 if(busy_ || recovery_.isEmpty() || !vault_->is_unlocked() || !lock_->isLocked()) { status_="Unlock the local vault and load its recovery key first."; failed_=true; emit changed(); return; }
 busy_=true; failed_=false; timer_.stop(); status_="Sync working — encrypted queue remains durable."; emit changed();
 auto worker=std::make_shared<SyncSession>(); worker->lock=lock_; worker->vault=vault_; worker->directory=directory_; worker->recovery=recovery_; worker->state=state_; worker->catalog=catalog_; worker->stop=stop_;
 auto* watcher=new QFutureWatcher<void>(this);
 connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher,worker] {
  watcher->deleteLater(); busy_=false; state_=worker->state; catalog_=worker->catalog; failed_=worker->failed; status_=worker->status;
  if(worker->library) emit libraryChanged(); if(!worker->infoTitle.isEmpty()) emit information(worker->infoTitle,worker->infoBody);
  if(!stop_->load() && !worker->auth && vault_->is_unlocked() && !state_["token"].toString().isEmpty()) timer_.start(failed_?retryDelay(attempts_++):30000); if(!failed_) attempts_=0;
  emit changed(); emit finished();
 });
 watcher->setFuture(QtConcurrent::run([worker,action] { try { action(*worker); } catch(const std::exception& e) { worker->failed=true; worker->status=QString::fromUtf8(e.what()); } }));
}
void SyncEngine::pair(const QUrl& u,const QString& p,const QString& n,const QString& c) { launch([=](SyncSession& s){s.pair(u,p,n,c);}); }
void SyncEngine::syncNow() { timer_.stop(); if(!busy_) stop_->store(false); launch([](SyncSession& s){s.run();}); }
void SyncEngine::refreshRemote() { launch([](SyncSession& s){s.manifest(); s.status="Remote catalog refreshed. Select object IDs before downloading; names remain encrypted.";}); }
void SyncEngine::select(const QStringList& local,const QStringList& remote) {
 if(busy_ || recovery_.isEmpty()) return;
 try { for(auto id:remote) if(!isHex(id,32)) error("Invalid selected object ID."); state_["selected_local"]=array(local); state_["selected_remote"]=array(remote); saveState(directory_+"/state.enc",recovery_,state_); failed_=false; emit changed(); } catch(const std::exception& e) { failed_=true; status_=QString::fromUtf8(e.what()); emit changed(); }
}
void SyncEngine::setBandwidth(int speed) { if(busy_ || recovery_.isEmpty()) return; state_["bandwidth"]=qBound(0,speed,1048576); try { saveState(directory_+"/state.enc",recovery_,state_); } catch(const std::exception& e) { failed_=true; status_=QString::fromUtf8(e.what()); } emit changed(); }
void SyncEngine::acknowledgeDeletes() { if(busy_ || recovery_.isEmpty()) return; auto ack=state_["delete_ack"].toObject(); auto notices=state_["deletions"].toObject(); for(auto it=notices.begin();it!=notices.end();++it) ack[it.key()]=it.value(); state_["delete_ack"]=ack; state_["deletions"]=QJsonObject{}; try { saveState(directory_+"/state.enc",recovery_,state_); status_="Deletion notices acknowledged. Local copies remain untouched."; } catch(const std::exception& e) { failed_=true; status_=QString::fromUtf8(e.what()); } emit changed(); }
void SyncEngine::history(const QString& id) { if(!isHex(id,32)) return; launch([id](SyncSession& s){auto d=s.request("GET","/v2/objects/"+id+"/history").json(); if(!d["versions"].isArray()) error("Invalid remote history"); for(auto v:d["versions"].toArray()) if(!validRecord(v.toObject()) || v.toObject()["id"].toString()!=id) error("Invalid remote history record"); s.infoTitle="Immutable encrypted object history"; s.infoBody=QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Indented)); s.status="History loaded; local library unchanged.";}); }
void SyncEngine::devices() { launch([](SyncSession& s){auto d=s.request("GET","/v2/devices").json(); if(!d["devices"].isArray()) error("Invalid devices response"); s.infoTitle="Paired devices — revoke through the server CLI"; s.infoBody=QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Indented)); s.status="Devices loaded. Revocation cannot erase already copied data or secrets.";}); }
void SyncEngine::stop() { stop_->store(true); timer_.stop(); failed_=false; status_="Sync paused. Encrypted pending work and local originals are retained. Sync now / Retry resumes."; emit changed(); }
}
