#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
class QLockFile;
namespace videovault::core { class Vault; struct VideoInfo; struct FolderInfo; }
namespace videovault::app {
class SyncEngine final : public QObject {
 Q_OBJECT
public:
 explicit SyncEngine(std::shared_ptr<core::Vault> vault,const QString& privateDirectory={},QObject* parent=nullptr);
 ~SyncEngine() override;
 bool unlock(const QString& recovery);
 void pair(const QUrl& url,const QString& pin,const QString& deviceName,const QString& code);
 void select(const QStringList& localIds,const QStringList& remoteIds);
 void syncNow();
 void refreshRemote();
 void history(const QString& object);
 void devices();
 void acknowledgeDeletes();
 void setBandwidth(int kibPerSecond);
 void stop();
 bool busy() const { return busy_; }
 bool failed() const { return failed_; }
 QString status() const { return status_; }
 QString privateDirectory() const { return directory_; }
 QJsonObject state() const { return state_; }
 QJsonArray catalog() const { return catalog_; }
 static QString relativePath(const core::VideoInfo&,const std::vector<core::FolderInfo>&);
 static QString fingerprint(const core::VideoInfo&,const std::vector<core::FolderInfo>&);
signals:
 void changed();
 void finished();
 void libraryChanged();
 void information(const QString& title,const QString& body);
private:
 void launch(const std::function<void(class SyncSession&)>& action);
 std::shared_ptr<core::Vault> vault_;
 QString directory_,recovery_,status_;
 QJsonObject state_;
 QJsonArray catalog_;
 std::shared_ptr<QLockFile> lock_;
 std::shared_ptr<std::atomic_bool> stop_;
 QTimer timer_;
 int attempts_{0}; bool busy_{false},failed_{false};
};
}
