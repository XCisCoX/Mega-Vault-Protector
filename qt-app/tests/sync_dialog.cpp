#include "videovault/app/sync_dialog.hpp"
#include "videovault/app/sync_engine.hpp"
#include "videovault/core/vault.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <stdexcept>
int main(int argc,char** argv) {
 QApplication app(argc,argv); QTemporaryDir temp;
 auto r=videovault::core::Vault::create(std::filesystem::path(temp.path().toStdString())/"vault","local",{8192,1,1}); if(!r) return 1;
 auto vault=std::make_shared<videovault::core::Vault>(std::move(r.value()));
 videovault::app::SyncEngine engine(vault,temp.path()+"/state");
 videovault::app::SyncDialog dialog(&engine,vault);
 auto* secret=dialog.findChild<QLineEdit*>("syncRecovery");
 if(!secret || secret->echoMode()!=QLineEdit::Password) throw std::runtime_error("local recovery entry must be masked");
 if(!dialog.findChild<QPushButton*>("syncRetry")) throw std::runtime_error("explicit retry missing");
 if(!dialog.findChild<QPushButton*>("syncPair")) throw std::runtime_error("per-device pairing missing");
 auto* load=dialog.findChild<QPushButton*>("syncLoad"); if(!load) throw std::runtime_error("local recovery load action missing");
 secret->setText(QString(64,'a')); load->click();
 if(engine.state().isEmpty() || !secret->text().isEmpty() || !dialog.findChild<QPushButton*>("syncPair")->isEnabled() || dialog.findChild<QPushButton*>("syncRetry")->isEnabled()) throw std::runtime_error("setup did not load key locally and keep unpaired sync disabled");
 dialog.show(); QApplication::processEvents();
 if(!secret->isVisible()) throw std::runtime_error("recovery control is not reachable in native setup");
 return 0;
}
