#include "videovault/app/sync_state.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <stdexcept>
#include <iostream>
using namespace videovault::app::sync;
int main() { try {
 QTemporaryDir dir; if(!dir.isValid()) throw std::runtime_error("temp directory");
 auto path=dir.path()+"/state";
 auto key=QString(64,'a');
 QJsonObject state{{"version",2},{"token",QString(64,'b')},{"queue",QJsonArray{QJsonObject{{"operation",QString(32,'c')},{"path","opaque.mvpk"}}}}};
 saveState(path,key,state);
 QFile f(path); f.open(QIODevice::ReadOnly); auto sealed=f.readAll();
 if(sealed.contains(QString(64,'b').toUtf8()) || sealed.contains("opaque.mvpk")) throw std::runtime_error("plaintext state leaked");
 if(loadState(path,key)!=state) throw std::runtime_error("durable queue roundtrip");
 bool rejected=false; try { loadState(path,QString(64,'d')); } catch(...) { rejected=true; }
 if(!rejected) throw std::runtime_error("wrong recovery key accepted");
 sealed[sealed.size()-1]=char(sealed.at(sealed.size()-1)^1); f.close(); f.open(QIODevice::WriteOnly); f.write(sealed); f.close();
 rejected=false; try { loadState(path,key); } catch(...) { rejected=true; }
 if(!rejected) throw std::runtime_error("tampered state accepted");
 std::cout<<"encrypted state, queue persistence, wrong key and corruption passed\n";
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
