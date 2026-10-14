#include "videovault/app/sync_package.hpp"
#include "videovault/app/sync_protocol.hpp"
#include <QFile>
#include <QCryptographicHash>
#include <QtEndian>
#include <sodium.h>
#include <zlib.h>
#include <stdexcept>
namespace videovault::app::sync {
static void bad() { throw std::runtime_error("MVPK package is invalid, unsupported, not one-file, or recovery key is incorrect. Local originals preserved."); }
static QByteArray exact(QFile& f,int n) { auto b=f.read(n); if(b.size()!=n) bad(); return b; }
PackageIdentity inspectPackage(const QString& path,const QString& recovery) {
 if(sodium_init()<0 || !isHex(recovery,64)) bad(); QFile f(path); if(!f.open(QIODevice::ReadOnly)) bad();
 auto h=exact(f,40); if(h.left(8)!=QByteArray::fromHex("4d56504ba701c300") || h.at(8)!=1) bad();
 auto mem=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(h.constData()+12)); auto ops=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(h.constData()+16)); auto lanes=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(h.constData()+20));
 // This client emits the current fixed core export profile, not arbitrary KDF requests.
 if(mem!=65536 || ops!=3 || lanes!=1) bad();
 unsigned char key[32]; auto password=recovery.toUtf8();
 if(crypto_pwhash(key,32,password.constData(),password.size(),reinterpret_cast<const unsigned char*>(h.constData()+24),ops,mem*1024ULL,crypto_pwhash_ALG_ARGON2ID13)!=0) bad();
 sodium_memzero(password.data(),password.size()); auto header=exact(f,crypto_secretstream_xchacha20poly1305_HEADERBYTES); crypto_secretstream_xchacha20poly1305_state stream;
 int rc=crypto_secretstream_xchacha20poly1305_init_pull(&stream,reinterpret_cast<const unsigned char*>(header.constData()),key); sodium_memzero(key,sizeof key); if(rc) bad();
 PackageIdentity result; QCryptographicHash hash(QCryptographicHash::Sha256); bool file=false,ended=false; quint64 written=0;
 while(!ended) {
  auto length=exact(f,4); auto n=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(length.constData())); if(n<crypto_secretstream_xchacha20poly1305_ABYTES || n>compressBound(1024*1024)+64+crypto_secretstream_xchacha20poly1305_ABYTES) bad();
  auto c=exact(f,int(n)); QByteArray p(int(n),0); unsigned long long plen=0; unsigned char tag=0;
  if(crypto_secretstream_xchacha20poly1305_pull(&stream,reinterpret_cast<unsigned char*>(p.data()),&plen,&tag,reinterpret_cast<const unsigned char*>(c.constData()),c.size(),nullptr,0)!=0) bad(); p.resize(int(plen)); if(p.isEmpty()) bad();
  auto kind=uchar(p[0]);
  if(kind==1) {
   if(file || p.size()<11) bad(); int len=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(p.constData()+1)); if(len<1 || len>1024 || p.size()!=3+len+8) bad();
   result.relativePath=QString::fromUtf8(p.constData()+3,len); result.size=qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(p.constData()+3+len)); file=true;
  } else if(kind==2) {
   if(!file || p.size()<6) bad(); auto raw=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(p.constData()+2)); if(raw>1024*1024 || written+raw>result.size) bad();
   if(p.at(1)==0) { if(p.size()!=int(raw)+6) bad(); hash.addData(p.constData()+6,int(raw)); }
   else if(p.at(1)==1) { QByteArray plain(int(raw),0); uLongf out=raw; if(uncompress(reinterpret_cast<Bytef*>(plain.data()),&out,reinterpret_cast<const Bytef*>(p.constData()+6),p.size()-6)!=Z_OK || out!=raw) bad(); hash.addData(plain); sodium_memzero(plain.data(),plain.size()); }
   else bad(); written+=raw;
  } else if(kind==3) { if(p.size()<3) bad(); }
  else if(kind==4) { if(p.size()!=1 || tag!=crypto_secretstream_xchacha20poly1305_TAG_FINAL || !f.atEnd() || !file || written!=result.size) bad(); ended=true; }
  else bad();
  if(!ended && tag==crypto_secretstream_xchacha20poly1305_TAG_FINAL) bad(); sodium_memzero(p.data(),p.size());
 }
 sodium_memzero(&stream,sizeof stream); result.plaintextHash=QString::fromLatin1(hash.result().toHex()); return result;
}
}
