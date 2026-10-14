#include "videovault/app/sync_protocol.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
using namespace videovault::app::sync;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
 (void)argc; (void)argv;
 try {
  check(validServer(QUrl("https://example.com:8766")), "HTTPS URL accepted");
  check(!validServer(QUrl("http://example.com")), "remote HTTP rejected");
  check(validServer(QUrl("http://127.0.0.1:8766")), "literal loopback development permitted");
  check(!validServer(QUrl("https://user:pass@example.com")), "credentials in URL rejected");
  check(!validServer(QUrl("https://example.com/path?key=x")), "URL paths/query rejected");
  check(!validServer(QUrl("file:///secret")), "non-network URL rejected");
  check(isHex(QString(64,'a'),64) && !isHex(QString(64,'A'),64), "strict lowercase hex");
  QJsonObject record{{"id",QString(32,'a')},{"revision",1},{"sequence",1},{"blob",QString(64,'b')},{"size",123},{"deleted",false}};
  check(validRecord(record), "valid manifest record");
  record["revision"]=0; check(!validRecord(record), "zero revision rejected");
  record["revision"]=1.5; check(!validRecord(record), "fractional revision rejected");
  record["revision"]=1; record["size"]=-1; check(!validRecord(record), "negative size rejected");
  record["deleted"]=true; record["blob"]=""; record["size"]=0; check(validRecord(record), "valid tombstone");
  check(!acceptRevision(record,0,2), "rollback below locally acknowledged commit rejected");
  check(!acceptRevision(record,2,0), "rollback below observed manifest rejected");
  check(acceptRevision(record,1,1), "same acknowledged revision accepted");
  check(retryDelay(0)>=1000 && retryDelay(50)<=61000, "bounded backoff");
  QJsonObject pending{{"id",QString(32,'a')},{"operation",QString(32,'b')},{"base",8},{"blob",QString(64,'c')},{"path","stable.mvpk"}};
  auto copy=conflictCopy(pending,QString(32,'d'),QString(32,'e'));
  check(copy["base"].toInt()==0 && copy["id"].toString()==QString(32,'d') && copy["path"]==pending["path"] && copy["blob"]==pending["blob"], "conflict preserves durable bytes and uses fresh identity");
  check(pending["base"].toInt()==8, "conflict does not mutate original");
  std::cout << "sync protocol validation and conflict/backoff passed\n";
 } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
