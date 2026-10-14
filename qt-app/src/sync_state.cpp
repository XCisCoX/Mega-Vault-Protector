#include "videovault/app/sync_state.hpp"
#include "videovault/app/sync_protocol.hpp"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <sodium.h>
#include <stdexcept>
namespace videovault::app::sync {
static void fail(const char* s) { throw std::runtime_error(s); }
static QByteArray key(const QString& secret) {
 if(sodium_init()<0 || !isHex(secret,64)) fail("Enter the original 64 lowercase hex recovery key. It is never sent to the server.");
 return QCryptographicHash::hash(QByteArray("MegaVault-sync-state-v2\0",24)+QByteArray::fromHex(secret.toLatin1()),QCryptographicHash::Sha256);
}
QString randomHex(int n) { if(sodium_init()<0) fail("Crypto initialization failed"); QByteArray b(n,0); randombytes_buf(b.data(),n); return QString::fromLatin1(b.toHex()); }
QString fileHash(const QString& p) { QFile f(p); if(!f.open(QIODevice::ReadOnly)) fail("Encrypted package is missing or unreadable; queue preserved."); QCryptographicHash h(QCryptographicHash::Sha256); if(!h.addData(&f)) fail("Cannot hash encrypted package"); return QString::fromLatin1(h.result().toHex()); }
void saveState(const QString& path,const QString& recovery,const QJsonObject& state) {
 auto k=key(recovery); auto plain=QJsonDocument(state).toJson(QJsonDocument::Compact);
 QByteArray nonce(crypto_secretbox_NONCEBYTES,0); randombytes_buf(nonce.data(),nonce.size());
 QByteArray cipher(plain.size()+crypto_secretbox_MACBYTES,0);
 crypto_secretbox_easy(reinterpret_cast<unsigned char*>(cipher.data()),reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),reinterpret_cast<const unsigned char*>(nonce.constData()),reinterpret_cast<const unsigned char*>(k.constData()));
 sodium_memzero(k.data(),k.size()); sodium_memzero(plain.data(),plain.size());
 QSaveFile out(path); out.setDirectWriteFallback(false);
 if(!out.open(QIODevice::WriteOnly)) fail("Cannot save private sync state; no network commit attempted.");
 out.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
 auto bytes=QByteArray("MVS2")+nonce+cipher;
 if(out.write(bytes)!=bytes.size() || !out.commit()) fail("Cannot durably save private sync state; check disk space and permissions.");
}
QJsonObject loadState(const QString& path,const QString& recovery) {
 auto k=key(recovery);
 QFile in(path); if(!in.exists()) { sodium_memzero(k.data(),k.size()); return QJsonObject{{"version",2},{"queue",QJsonArray{}},{"maps",QJsonObject{}},{"selected_local",QJsonArray{}},{"selected_remote",QJsonArray{}}}; }
 if(!in.open(QIODevice::ReadOnly) || in.size()>16*1024*1024) fail("Private sync state is unreadable or too large; restore its backup.");
 auto bytes=in.readAll();
 if(!bytes.startsWith("MVS2") || bytes.size()<4+crypto_secretbox_NONCEBYTES+crypto_secretbox_MACBYTES) fail("Private sync state is corrupt; do not reset it while pending uploads exist.");
 auto nonce=bytes.mid(4,crypto_secretbox_NONCEBYTES); auto c=bytes.mid(4+crypto_secretbox_NONCEBYTES); QByteArray p(c.size()-crypto_secretbox_MACBYTES,0);
 int rc=crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(p.data()),reinterpret_cast<const unsigned char*>(c.constData()),c.size(),reinterpret_cast<const unsigned char*>(nonce.constData()),reinterpret_cast<const unsigned char*>(k.constData()));
 sodium_memzero(k.data(),k.size()); if(rc) fail("Recovery key is incorrect or private state is damaged. Pending packages were not discarded.");
 QJsonParseError error; auto doc=QJsonDocument::fromJson(p,&error); sodium_memzero(p.data(),p.size());
 if(error.error!=QJsonParseError::NoError || !doc.isObject() || doc.object()["version"].toInt()!=2) fail("Unsupported or corrupt private state.");
 return doc.object();
}
}
