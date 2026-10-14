#include "videovault/app/sync_protocol.hpp"
#include <cmath>
#include <QRandomGenerator>
namespace videovault::app::sync {
static bool integer(const QJsonValue& v, double minimum) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=minimum && v.toDouble()<=9007199254740991.0 && std::floor(v.toDouble())==v.toDouble(); }
bool validRecord(const QJsonObject& r) {
 if(!isHex(r["id"].toString(),32) || !integer(r["revision"],1) || !integer(r["sequence"],1) || !integer(r["size"],0) || !r["deleted"].isBool()) return false;
 return r["deleted"].toBool() ? r["blob"].toString().isEmpty() && r["size"].toDouble()==0 : isHex(r["blob"].toString(),64) && r["size"].toDouble()>0;
}
bool acceptRevision(const QJsonObject& r,double observed,double acknowledged) { return validRecord(r) && r["revision"].toDouble()>=qMax(observed,acknowledged); }
int retryDelay(int attempts) { return (1000 << qBound(0,attempts,5)) + QRandomGenerator::global()->bounded(1001) + (attempts>5 ? 28000 : 0); }
QJsonObject conflictCopy(const QJsonObject& p,const QString& id,const QString& op) { auto c=p; c["conflict_of"]=p["id"]; c["id"]=id; c["operation"]=op; c["base"]=0; return c; }

bool isHex(const QString& value,int length) {
 if(value.size()!=length) return false;
 for(auto c:value) if(!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
 return true;
}
bool validServer(const QUrl& u) {
 if(!u.isValid() || u.host().isEmpty() || !u.userInfo().isEmpty() || u.hasQuery() || u.hasFragment() || (!u.path().isEmpty() && u.path()!="/")) return false;
 return u.scheme()=="https" || (u.scheme()=="http" && (u.host()=="127.0.0.1" || u.host()=="::1"));
}
}
