#include "videovault/app/sync_engine.hpp"
#include "videovault/app/sync_package.hpp"
#include "videovault/app/sync_state.hpp"
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSslCertificate>
#include <QSslError>
#include "videovault/core/vault.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <iostream>
#include <stdexcept>
using namespace videovault;
static QByteArray network(const QUrl& base,const QString& token,const QByteArray& method,const QString& path,const QByteArray& body={},const QList<QPair<QByteArray,QByteArray>>& headers={}) {
 QNetworkAccessManager manager; QUrl url=base; url.setPath(path.section('?',0,0)); url.setQuery(path.section('?',1)); QNetworkRequest req(url); req.setRawHeader("Authorization","Bearer "+token.toLatin1()); for(auto h:headers) req.setRawHeader(h.first,h.second);
 req.setHeader(QNetworkRequest::ContentTypeHeader,method=="PUT"?"application/octet-stream":"application/json");
 auto* reply=method=="PUT"?manager.put(req,body):(method=="POST"?manager.post(req,body):manager.get(req)); QEventLoop loop;
 QObject::connect(reply,QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),&loop,[&](const QList<QSslError>&){auto pin=qgetenv("MV_SYNC_TEST_PIN"); if(!pin.isEmpty() && reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256).toHex()==pin) reply->ignoreSslErrors();});
 QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit); QTimer::singleShot(20000,&loop,&QEventLoop::quit); loop.exec(); if(!reply->isFinished() || reply->error()!=QNetworkReply::NoError) throw std::runtime_error("live test fixture request failed"); return reply->readAll();
}
// Optional live relay integration: sync_engine_tests URL PAIR_CODE RECOVERY_KEY.
// Server must be empty and accepts literal-loopback HTTP or trusted HTTPS.
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv); QTemporaryDir temp;
 try {
  core::VideoInfo video; video.id=1; video.display_name="note.txt"; video.folder_id=2;
  std::vector<core::FolderInfo> folders{{2,0,"Work",0}};
  auto a=app::SyncEngine::fingerprint(video,folders);
  folders[0].name="Renamed";
  if(a==app::SyncEngine::fingerprint(video,folders)) throw std::runtime_error("folder rename not detected");
  video.tags={"private"}; auto b=app::SyncEngine::fingerprint(video,folders);
  video.tags={"other"}; if(b==app::SyncEngine::fingerprint(video,folders)) throw std::runtime_error("tags not tracked");
  {
   auto created=core::Vault::create(std::filesystem::path(temp.path().toStdString())/"lock-vault","local",{8192,1,1}); if(!created) throw std::runtime_error("lock test vault");
   auto shared=std::make_shared<core::Vault>(std::move(created.value()));
   auto owner=std::make_unique<app::SyncEngine>(shared,temp.path()+"/lock-state");
   app::SyncEngine contender(shared,temp.path()+"/lock-state");
   if(contender.unlock(QString(64,'a'))) throw std::runtime_error("two clients acquired same private ledger");
   owner.reset();
   if(!contender.unlock(QString(64,'a'))) throw std::runtime_error("cannot retry ledger lock after previous client exits");
  }
  if(argc>=4) {
   auto result=core::Vault::create(std::filesystem::path(temp.path().toStdString())/"vault","local-only",{8192,1,1});
   if(!result) throw std::runtime_error("create vault");
   auto vault=std::make_shared<core::Vault>(std::move(result.value()));
   std::vector<unsigned char> bytes{'h','e','l','l','o'};
   auto local=vault->import_bytes(bytes,"note.txt"); if(!local) throw std::runtime_error("import bytes");
   auto videos=vault->list_videos(); auto folders=vault->list_folders(); if(!videos || !folders) throw std::runtime_error("list local metadata");
   auto exported=temp.path()+"/one.mvpk";
   if(!vault->export_package(std::filesystem::path(exported.toStdString()),QString::fromUtf8(argv[3]).toStdString(),{{local.value(),"Work/note.txt"}},{"Work"},false)) throw std::runtime_error("export MVPK");
   auto inspected=app::sync::inspectPackage(exported,QString::fromUtf8(argv[3]));
   if(inspected.relativePath!="Work/note.txt" || inspected.size!=5 || inspected.plaintextHash!=QString::fromLatin1(QCryptographicHash::hash("hello",QCryptographicHash::Sha256).toHex())) throw std::runtime_error("MVPK inspection did not hash actual encrypted content");
   app::SyncEngine engine(vault,temp.path()+"/private-state");
   if(!engine.unlock(QString::fromUtf8(argv[3]))) throw std::runtime_error(engine.status().toStdString());
   auto wait=[&](bool expectedFailure=false) { QEventLoop loop; QObject::connect(&engine,&app::SyncEngine::finished,&loop,&QEventLoop::quit); QTimer::singleShot(60000,&loop,&QEventLoop::quit); loop.exec(); if(engine.busy() || engine.failed()!=expectedFailure) throw std::runtime_error(engine.status().toStdString()); };
   auto pin=QString::fromLatin1(qgetenv("MV_SYNC_TEST_PIN"));
   if(!pin.isEmpty()) {
    engine.pair(QUrl(argv[1]),{},"native-test",QString::fromUtf8(argv[2])); wait(true);
    engine.pair(QUrl(argv[1]),QString(64,'0'),"native-test",QString::fromUtf8(argv[2])); wait(true);
    if(!engine.status().contains("pin mismatch",Qt::CaseInsensitive)) throw std::runtime_error("wrong certificate pin lacks explicit recovery guidance");
   }
   engine.pair(QUrl(argv[1]),pin,QStringLiteral("native-test"),QString::fromUtf8(argv[2])); wait();
   engine.select({QString::number(local.value())},{});
   engine.syncNow(); wait();
   if(engine.state()["queue"].toArray().size()!=0 || engine.state()["maps"].toObject().size()!=1) throw std::runtime_error("upload not acknowledged");
   QJsonValue before=engine.state()["maps"];
   engine.syncNow(); wait(); if(engine.state()["maps"]!=before) throw std::runtime_error("echo upload");
   if(argc>=5) {
    auto durable=engine.state(); auto objectId=durable["maps"].toObject().keys().first(); auto acknowledged=durable["maps"].toObject()[objectId].toObject();
    auto operation=app::sync::randomHex(); auto staged=temp.path()+"/private-state/"+operation+".mvpk";
    if(!vault->export_package(std::filesystem::path(staged.toStdString()),QString::fromUtf8(argv[3]).toStdString(),{{local.value(),"note.txt"}},{},false)) throw std::runtime_error("durable resume staging");
    auto hash=app::sync::fileHash(staged); auto size=QFileInfo(staged).size(); QFile package(staged); package.open(QIODevice::ReadOnly); auto first=package.read(32); package.close();
    auto offset=QJsonDocument::fromJson(network(QUrl(argv[1]),durable["token"].toString(),"PUT","/v2/blobs/"+hash+"?offset=0",first,{{"X-Blob-Size",QByteArray::number(size)}})).object()["offset"].toInt(); if(offset!=32) throw std::runtime_error("fixture partial offset");
    QJsonObject pending{{"id",objectId},{"local",QString::number(local.value())},{"fingerprint",acknowledged["fingerprint"]},{"base",1},{"operation",operation},{"path",operation+".mvpk"},{"blob",hash},{"size",double(size)}};
    durable["queue"]=QJsonArray{pending}; app::sync::saveState(temp.path()+"/private-state/state.enc",QString::fromUtf8(argv[3]),durable);
    if(!engine.unlock(QString::fromUtf8(argv[3])) || engine.state()["queue"].toArray().first().toObject()["operation"].toString()!=operation) throw std::runtime_error("durable operation ID did not survive reload");
    engine.syncNow(); wait(); if(engine.state()["maps"].toObject()[objectId].toObject()["revision"].toInt()!=2 || !engine.state()["queue"].toArray().isEmpty()) throw std::runtime_error("partial upload did not resume and commit");
    auto second=core::Vault::create(std::filesystem::path(temp.path().toStdString())/"second","different-local-password",{8192,1,1});
    if(!second) throw std::runtime_error("second independent vault");
    auto vaultB=std::make_shared<core::Vault>(std::move(second.value()));
    auto existing=vaultB->import_bytes(bytes,"existing-local-name.txt"); if(!existing) throw std::runtime_error("dedup seed");
    app::SyncEngine peer(vaultB,temp.path()+"/peer-state");
    if(!peer.unlock(QString::fromUtf8(argv[3]))) throw std::runtime_error("peer recovery load");
    auto waitPeer=[&] { QEventLoop loop; QObject::connect(&peer,&app::SyncEngine::finished,&loop,&QEventLoop::quit); QTimer::singleShot(60000,&loop,&QEventLoop::quit); loop.exec(); if(peer.busy() || peer.failed()) throw std::runtime_error(peer.status().toStdString()); };
    peer.pair(QUrl(argv[1]),pin,"second-native-test",QString::fromUtf8(argv[4])); waitPeer();
    auto id=engine.state()["maps"].toObject().keys().first();
    auto partialDownload=network(QUrl(argv[1]),durable["token"].toString(),"GET","/v2/blobs/"+hash,{},{{"Range","bytes=0-31"}}); QFile cached(temp.path()+"/peer-state/download-"+hash+".mvpk"); if(!cached.open(QIODevice::WriteOnly) || cached.write(partialDownload)!=32) throw std::runtime_error("fixture ranged partial cache"); cached.close();
    peer.select({}, {id}); peer.syncNow(); waitPeer();
    auto map=peer.state()["maps"].toObject()[id].toObject();
    if(map["local"].toString()!=QString::number(existing.value()) || map["revision"].toInt()!=2) throw std::runtime_error("resumed dedup import did not resolve exact local ID");
    peer.syncNow(); waitPeer(); if(peer.catalog().size()!=1 || peer.catalog().first().toObject()["revision"].toInt()!=2) throw std::runtime_error("dedup echoed upload");
    std::vector<unsigned char> editA{'A','-','e','d','i','t'}, editB{'B','-','e','d','i','t'};
    if(!vault->replace_video_content(local.value(),editA,"note.txt") || !vaultB->replace_video_content(existing.value(),editB,"existing-local-name.txt")) throw std::runtime_error("independent offline edits");
    engine.syncNow(); wait(); peer.syncNow(); waitPeer();
    if(peer.state()["conflicts"].toArray().size()!=1 || peer.catalog().size()!=2 || peer.state()["maps"].toObject().size()!=2) throw std::runtime_error("conflict copy not preserved");
    auto kept=vaultB->read_video_bytes(existing.value()); if(!kept || kept.value()!=editB) throw std::runtime_error("conflict overwrote local original");
    auto imported=vaultB->list_videos(); if(!imported || imported.value().size()!=2) throw std::runtime_error("remote edit did not preserve both local contents");
    QJsonValue ledger=peer.state()["maps"]; peer.syncNow(); waitPeer(); if(peer.state()["maps"]!=ledger) throw std::runtime_error("imported conflict echoed");
    auto peerOriginal=peer.state()["maps"].toObject()[id].toObject()["local"].toString().toLongLong(); std::vector<unsigned char> nextRemote{'n','e','x','t','-','p','e','e','r'};
    if(!vaultB->replace_video_content(peerOriginal,nextRemote,"note.txt")) throw std::runtime_error("next mapped peer edit"); peer.syncNow(); waitPeer(); engine.syncNow(); wait();
    auto receivedId=engine.state()["maps"].toObject()[id].toObject()["local"].toString().toLongLong(); auto received=vault->read_video_bytes(receivedId);
    if(receivedId==local.value() || !received || received.value()!=nextRemote || vault->read_video_bytes(local.value()).value()!=editA) throw std::runtime_error("selected local file is not bidirectional or local original was overwritten");
    QJsonObject tombstone{{"base_revision",4},{"blob",""},{"size",0},{"deleted",true},{"operation_id",app::sync::randomHex()}};
    auto deleted=QJsonDocument::fromJson(network(QUrl(argv[1]),peer.state()["token"].toString(),"POST","/v2/objects/"+id,QJsonDocument(tombstone).toJson(QJsonDocument::Compact))).object(); if(!deleted["deleted"].toBool() || deleted["revision"].toInt()!=5) throw std::runtime_error("fixture tombstone");
    peer.syncNow(); waitPeer(); if(peer.state()["deletions"].toObject().size()!=1 || vaultB->list_videos().value().size()!=2) throw std::runtime_error("remote delete removed local original or lacked notice");
    peer.acknowledgeDeletes(); peer.syncNow(); waitPeer(); if(!peer.state()["deletions"].toObject().isEmpty() || vaultB->list_videos().value().size()!=2) throw std::runtime_error("delete acknowledgement removed local copies");
    auto authorized=peer.state(); auto invalid=authorized; invalid["token"]=QString(64,'0'); app::sync::saveState(temp.path()+"/peer-state/state.enc",QString::fromUtf8(argv[3]),invalid);
    if(!peer.unlock(QString::fromUtf8(argv[3]))) throw std::runtime_error("reload invalid token fixture");
    peer.syncNow(); { QEventLoop loop; QObject::connect(&peer,&app::SyncEngine::finished,&loop,&QEventLoop::quit); QTimer::singleShot(10000,&loop,&QEventLoop::quit); loop.exec(); }
    if(!peer.failed() || !peer.status().contains("re-pair",Qt::CaseInsensitive)) throw std::runtime_error("401 did not stop sync with re-pair guidance");
    app::sync::saveState(temp.path()+"/peer-state/state.enc",QString::fromUtf8(argv[3]),authorized); if(!peer.unlock(QString::fromUtf8(argv[3]))) throw std::runtime_error("recover original token fixture"); peer.syncNow(); waitPeer();
    peer.history(id); waitPeer();
    peer.devices(); waitPeer();
    peer.stop(); if(!peer.status().contains("paused",Qt::CaseInsensitive)) throw std::runtime_error("pause status not reported"); peer.syncNow(); waitPeer();
    std::cout<<"durable queue reload, upload/download resume, dedup/no-echo, conflicts, conservative deletion, 401 recovery, history, devices and pause/retry passed\n";
   }
  }
  std::cout<<"native engine fingerprints and optional live sync passed\n";
 }catch(const std::exception& e){ std::cerr<<e.what()<<'\n'; return 1; }
}
