#pragma once
#include <QDialog>
#include <memory>
class QLabel; class QLineEdit; class QPushButton; class QListWidget; class QSpinBox;
namespace videovault::core { class Vault; }
namespace videovault::app {
class SyncEngine;
class SyncDialog final: public QDialog {
public:
 SyncDialog(SyncEngine* engine,std::shared_ptr<core::Vault> vault,QWidget* parent=nullptr);
private:
 void refresh(); void saveSelection(); void rebuildLists();
 SyncEngine* engine_; std::shared_ptr<core::Vault> vault_;
 QLineEdit *url_,*pin_,*name_,*code_,*recovery_;
 QPushButton *pair_,*retry_,*refresh_,*load_,*generate_,*folder_,*history_,*devices_,*deletes_;
 QLabel *status_,*counts_;
 QListWidget *local_,*remote_;
 QSpinBox* bandwidth_;
 bool loaded_=false,rebuilding_=false;
};
}
