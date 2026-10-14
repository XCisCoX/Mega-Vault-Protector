#include "videovault/app/sync_dialog.hpp"
#include "videovault/app/sync_engine.hpp"
#include "videovault/app/sync_state.hpp"
#include "videovault/core/vault.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QSpinBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QJsonDocument>

#include <QTabWidget>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
namespace videovault::app {
namespace { QStringList ids(QListWidget* list) { QStringList out; for(int i=0;i<list->count();++i) if(list->item(i)->checkState()==Qt::Checked) out.append(list->item(i)->data(Qt::UserRole).toString()); return out; } bool contains(const QJsonArray& a,const QString& id) { for(auto v:a) if(v.toString()==id) return true; return false; } }
SyncDialog::SyncDialog(SyncEngine* engine,std::shared_ptr<core::Vault> vault,QWidget* parent):QDialog(parent),engine_(engine),vault_(std::move(vault)) {
 setWindowTitle("Self-hosted encrypted sync v2"); resize(850,730); setAttribute(Qt::WA_DeleteOnClose);
 auto* root=new QVBoxLayout(this);
 auto* explanation=new QLabel("Your local vault stays the library and playback source. The relay stores opaque encrypted MVPK packages, not your vault password or recovery key. Sync runs only while this local vault is unlocked.",this); explanation->setWordWrap(true); root->addWidget(explanation);
 auto* tabs=new QTabWidget(this); root->addWidget(tabs,1);
 auto* setup=new QWidget(tabs); auto* form=new QFormLayout(setup);
 url_=new QLineEdit(setup); url_->setPlaceholderText("https://your-relay.example:8766");
 pin_=new QLineEdit(setup); pin_->setPlaceholderText("Optional certificate SHA-256, 64 lowercase hex");
 name_=new QLineEdit(setup); name_->setText("Desktop"); name_->setPlaceholderText("Non-sensitive device label");
 code_=new QLineEdit(setup); code_->setEchoMode(QLineEdit::Password); code_->setPlaceholderText("One-use code from server pair-code CLI");
 recovery_=new QLineEdit(setup); recovery_->setObjectName("syncRecovery"); recovery_->setEchoMode(QLineEdit::Password); recovery_->setPlaceholderText("Original shared 64 lowercase hex recovery key — local only");
 form->addRow("Relay origin",url_); form->addRow("Certificate pin",pin_); form->addRow("Device label",name_); form->addRow("Pairing code",code_); form->addRow("Recovery key",recovery_);
 auto* keys=new QHBoxLayout; load_=new QPushButton("Load existing key / resume",setup); load_->setObjectName("syncLoad"); generate_=new QPushButton("Generate new recovery key…",setup); auto* reveal=new QPushButton("Show / hide",setup); keys->addWidget(load_); keys->addWidget(generate_); keys->addWidget(reveal); form->addRow(keys);
 pair_=new QPushButton("Pair / re-pair this device",setup); pair_->setObjectName("syncPair"); form->addRow(pair_);
 auto* caveats=new QLabel("Back up the recovery key out of band and enter that same key on every device. It is not saved by this client and is never sent with a pairing code. Losing it makes shared packages unrecoverable. Device tokens and the durable queue ledger are encrypted locally with that key.\n\nHTTPS uses normal certificate validation. An explicitly supplied certificate pin can trust a self-signed certificate and must match exactly. HTTP is accepted only for literal 127.0.0.1 / ::1 development origins.\n\nThe sync cache contains encrypted packages only. The existing core importer uses transient plaintext staging during import; cleanup is best effort, not secure erasure. Names and non-empty folder paths travel inside MVPK. Identical-content dedup keeps the existing local name/folder; name collisions retain copies rather than overwrite metadata. Tags and empty folders are not synchronized. Remote history and local originals are preserved; deleting locally does not automatically delete remotely.\n\nThe server can see device labels, IDs, sizes, timing and revision relationships. E2EE does not prevent withholding; locally observed decreasing revisions are rejected. Revoke devices with the server CLI. After compromise, rotate the recovery key and re-encrypt packages using a separate migration — re-pairing alone does not rotate it.",setup); caveats->setWordWrap(true); form->addRow(caveats);
 auto* setupScroll=new QScrollArea(tabs); setupScroll->setWidgetResizable(true); setupScroll->setWidget(setup);
 tabs->addTab(setupScroll,"Setup & recovery");
 auto* selection=new QWidget(tabs); auto* choose=new QVBoxLayout(selection);
 auto* note=new QLabel("Check individual local files or select a folder below. Local selection is bidirectional for its known object. Refresh the catalog and explicitly check unknown remote IDs to receive them; names are encrypted until import. To stop an object, uncheck its local file and any explicit remote selection. Durable uploads already queued still finish. No background download-all.",selection); note->setWordWrap(true); choose->addWidget(note);
 auto* row=new QHBoxLayout; auto* l=new QVBoxLayout; auto* r=new QVBoxLayout;
 l->addWidget(new QLabel("Local files (names remain on this device)",selection)); local_=new QListWidget(selection); l->addWidget(local_);
 r->addWidget(new QLabel("Remote object IDs / revisions",selection)); remote_=new QListWidget(selection); r->addWidget(remote_); row->addLayout(l); row->addLayout(r); choose->addLayout(row,1);
 auto* actions=new QHBoxLayout; folder_=new QPushButton("Select local folder…",selection); refresh_=new QPushButton("Refresh catalog",selection); history_=new QPushButton("History of selected object",selection); actions->addWidget(folder_); actions->addWidget(refresh_); actions->addWidget(history_); choose->addLayout(actions);
 bandwidth_=new QSpinBox(selection); bandwidth_->setRange(0,1048576); bandwidth_->setSuffix(" KiB/s (0 = unlimited)"); choose->addWidget(bandwidth_);
 tabs->addTab(selection,"Selective sync");
 auto* maintenance=new QWidget(tabs); auto* maintenanceLayout=new QVBoxLayout(maintenance);
 counts_=new QLabel(maintenance); counts_->setWordWrap(true); maintenanceLayout->addWidget(counts_);
 devices_=new QPushButton("View paired devices",maintenance); deletes_=new QPushButton("Acknowledge remote deletion notices (keep local copies)",maintenance); maintenanceLayout->addWidget(devices_); maintenanceLayout->addWidget(deletes_);
 auto* recoveryNote=new QLabel("Offline work is staged once as encrypted MVPK and stable operation IDs. Retry resumes server offsets and range downloads without re-encrypting packages. HTTP 401 stops automatic retries and requires re-pairing. Conflicts keep original queued bytes and publish the edit as a fresh object. Old local content is retained rather than overwritten.\n\nPrivate sync state and queued packages live in the application-private sync-v2 directory, bound to this local vault path. Back up the entire directory alongside your vault and recovery key. Never delete/reset that directory while work is queued. Restore missing queued bytes from backup, then Retry. Changing relay origin with an existing ledger is deliberately rejected.",maintenance); recoveryNote->setWordWrap(true); maintenanceLayout->addWidget(recoveryNote); maintenanceLayout->addStretch(); tabs->addTab(maintenance,"Status & recovery");
 status_=new QLabel(this); status_->setWordWrap(true); status_->setTextInteractionFlags(Qt::TextSelectableByMouse); root->addWidget(status_);
 auto* footer=new QHBoxLayout; retry_=new QPushButton("Sync now / Retry",this); retry_->setObjectName("syncRetry"); auto* pause=new QPushButton("Pause",this); auto* close=new QPushButton("Close (sync continues while unlocked)",this); footer->addWidget(retry_); footer->addWidget(pause); footer->addStretch(); footer->addWidget(close); root->addLayout(footer);
 connect(close,&QPushButton::clicked,this,&QDialog::close);
 connect(reveal,&QPushButton::clicked,this,[this]{recovery_->setEchoMode(recovery_->echoMode()==QLineEdit::Password?QLineEdit::Normal:QLineEdit::Password);});
 connect(generate_,&QPushButton::clicked,this,[this] {
  if(QMessageBox::warning(this,"Generate only for a new shared vault","Do not generate a new key for existing shared packages. Save the new key offline and share it directly with your own devices. Continue?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
  try { recovery_->setText(sync::randomHex(32)); recovery_->setEchoMode(QLineEdit::Normal); } catch(const std::exception& e){QMessageBox::warning(this,"Recovery key",QString::fromUtf8(e.what()));}
 });
 connect(load_,&QPushButton::clicked,this,[this] {
  loaded_=engine_->unlock(recovery_->text());
  if(loaded_) { auto s=engine_->state(); if(!s["url"].toString().isEmpty()) url_->setText(s["url"].toString()); pin_->setText(s["pin"].toString()); if(!s["device_name"].toString().isEmpty()) name_->setText(s["device_name"].toString()); bandwidth_->setValue(s["bandwidth"].toInt()); recovery_->clear(); rebuildLists(); }
  refresh();
 });
 connect(pair_,&QPushButton::clicked,this,[this] {
  engine_->pair(QUrl(url_->text().trimmed()),pin_->text().trimmed(),name_->text().trimmed(),code_->text().trimmed()); code_->clear();
 });
 connect(retry_,&QPushButton::clicked,this,[this]{saveSelection(); engine_->syncNow();});
 connect(pause,&QPushButton::clicked,this,[this]{engine_->stop(); status_->setText("Sync paused. Local library is unchanged. Sync now / Retry resumes it.");});
 connect(refresh_,&QPushButton::clicked,engine_,&SyncEngine::refreshRemote);
 connect(devices_,&QPushButton::clicked,engine_,&SyncEngine::devices);
 connect(deletes_,&QPushButton::clicked,engine_,&SyncEngine::acknowledgeDeletes);
 connect(history_,&QPushButton::clicked,this,[this]{if(remote_->currentItem()) engine_->history(remote_->currentItem()->data(Qt::UserRole).toString());});
 connect(local_,&QListWidget::itemChanged,this,[this]{saveSelection();}); connect(remote_,&QListWidget::itemChanged,this,[this]{saveSelection();});
 connect(bandwidth_,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){if(loaded_&&!rebuilding_) engine_->setBandwidth(v);});
 connect(folder_,&QPushButton::clicked,this,[this] {
  QStringList paths; for(int i=0;i<local_->count();++i){auto p=local_->item(i)->text().section('/',0,-2); if(!paths.contains(p)) paths.append(p);} bool ok=false; auto path=QInputDialog::getItem(this,"Select folder","Existing files in this folder and descendants",paths,0,false,&ok); if(!ok) return;
  rebuilding_=true; for(int i=0;i<local_->count();++i) if(path.isEmpty() || local_->item(i)->text().startsWith(path+"/")) local_->item(i)->setCheckState(Qt::Checked); rebuilding_=false; saveSelection();
 });
 connect(engine_,&SyncEngine::changed,this,&SyncDialog::refresh);
 connect(engine_,&SyncEngine::finished,this,[this]{if(loaded_) rebuildLists();});
 connect(engine_,&SyncEngine::information,this,[this](const QString& title,const QString& body){auto* d=new QDialog(this); d->setAttribute(Qt::WA_DeleteOnClose); d->setWindowTitle(title); d->resize(700,450); auto* layout=new QVBoxLayout(d); auto* text=new QPlainTextEdit(d); text->setReadOnly(true); text->setPlainText(body); layout->addWidget(text); d->show();});
 loaded_=!engine_->state().isEmpty(); if(loaded_) {auto s=engine_->state(); url_->setText(s["url"].toString()); pin_->setText(s["pin"].toString()); name_->setText(s["device_name"].toString()); bandwidth_->setValue(s["bandwidth"].toInt());} rebuildLists(); refresh();
}
void SyncDialog::refresh() {
 bool ready=loaded_&&!engine_->busy(); bool paired=!engine_->state()["token"].toString().isEmpty();
 for(auto* b:{pair_,folder_}) b->setEnabled(ready);
 for(auto* b:{retry_,refresh_,history_,devices_,deletes_}) b->setEnabled(ready&&paired);
 local_->setEnabled(ready); remote_->setEnabled(ready); bandwidth_->setEnabled(ready); load_->setEnabled(!engine_->busy()); generate_->setEnabled(!engine_->busy());
 status_->setText(engine_->status()); status_->setStyleSheet(engine_->failed()?"color:#f28b82;":"");
 auto s=engine_->state(); counts_->setText(QString("Queued encrypted packages: %1\nAcknowledged object mappings: %2\nPreserved conflict requests: %3\nUnacknowledged remote deletions: %4\nDevice ID: %5\nPrivate encrypted state and queue: %6").arg(s["queue"].toArray().size()).arg(s["maps"].toObject().size()).arg(s["conflicts"].toArray().size()).arg(s["deletions"].toObject().size()).arg(s["device"].toString()).arg(engine_->privateDirectory()));
}
void SyncDialog::saveSelection() { if(rebuilding_ || !loaded_ || engine_->busy()) return; engine_->select(ids(local_),ids(remote_)); }
void SyncDialog::rebuildLists() {
 if(engine_->busy() || !vault_->is_unlocked()) return; rebuilding_=true;
 auto videos=vault_->list_videos(); auto folders=vault_->list_folders(); if(videos && folders) { local_->clear(); for(const auto& v:videos.value()) {auto* item=new QListWidgetItem(SyncEngine::relativePath(v,folders.value()),local_); auto id=QString::number(v.id); item->setData(Qt::UserRole,id); item->setFlags(item->flags()|Qt::ItemIsUserCheckable); item->setCheckState(contains(engine_->state()["selected_local"].toArray(),id)?Qt::Checked:Qt::Unchecked);} }
 remote_->clear(); for(auto value:engine_->catalog()) {auto r=value.toObject(); auto id=r["id"].toString(); auto* item=new QListWidgetItem(QString("%1 · r%2 · %3 bytes%4").arg(id).arg(r["revision"].toDouble(),0,'f',0).arg(r["size"].toDouble(),0,'f',0).arg(r["deleted"].toBool()?" · deleted":""),remote_); item->setData(Qt::UserRole,id); item->setFlags(item->flags()|Qt::ItemIsUserCheckable); item->setCheckState(contains(engine_->state()["selected_remote"].toArray(),id)?Qt::Checked:Qt::Unchecked); }
 rebuilding_=false;
}
}
