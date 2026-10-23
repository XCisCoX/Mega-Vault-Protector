#pragma once

#include "videovault/app/sync_state.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>
#include <vector>

class QTimer;

namespace videovault::core {
class Vault;
struct FolderInfo;
struct VideoInfo;
}

namespace videovault::app {

namespace sync {
class Client;
}

class Job;

// The protocol 4 client: accounts, one vault, versioned records and lazy,
// resumable transfers of content-addressed packages.
//
// Signing in uses the account password (POST /auth/login then POST /auth/token)
// and stores the device token encrypted under the key derived from the *vault*
// password, so a stolen state file is useless without the vault. The vault
// password itself is never sent anywhere: the server only ever sees a bearer
// token and opaque bytes.
//
// A pass is optimistic and never destructive. Local items without a record are
// exported (blob first, then the record with base_version and an operation id),
// remote records that are not local are queued for a lazy download, and a
// version conflict never overwrites: the loser is re-published as a new record
// named "<name> (conflict copy)" so both items survive. Deletes are tombstones,
// which stay listed as debris the user can restore or purge.
class SyncEngine final : public QObject {
    Q_OBJECT

public:
    SyncEngine(std::shared_ptr<core::Vault> vault, sync::Keys keys,
               const QString& privateDirectory = {}, QObject* parent = nullptr);
    ~SyncEngine() override;

    // Where this vault's sync state lives, derived from the vault folder.
    static QString stateDirectoryFor(const QString& vaultRoot);

    // -- sign in ----------------------------------------------------------
    // Runs on a worker: checks the protocol, logs in, issues one device token
    // and loads the vault list. The password is wiped as soon as it is used.
    void signIn(const QUrl& server, const QString& email, const QString& password,
                const QString& deviceLabel = {});
    void refreshVaults();
    void signOut();

    // -- choose -----------------------------------------------------------
    void chooseVault(const QString& vaultId, const QString& name);
    // "Choose folder": the local vault this remote vault mirrors. Only the
    // vault this engine was created for is accepted, because a different folder
    // is a different local database with a different key.
    bool setLocalFolder(const QString& path);
    QString localFolder() const { return state_.folder; }

    // -- work -------------------------------------------------------------
    void syncNow();                              // publish, pull, then receive
    void receive(const QString& recordId);       // download one queued item now
    void tombstone(const QString& recordId);     // reversible delete
    void restore(const QString& recordId);       // undelete
    void purge(const QString& recordId);         // delete permanently

    // The same operations over a selection, in one pass: the page's list acts
    // on everything the user ticked, not one row at a time.
    void receiveMany(const QStringList& recordIds);    // keep on this device
    void tombstoneMany(const QStringList& recordIds);  // recycle everywhere
    void restoreMany(const QStringList& recordIds);    // undo a recycle
    void purgeMany(const QStringList& recordIds);      // delete permanently
    // Drop the local copy only: the record and its bytes stay in the vault and
    // on the other devices, and later passes will not pull it back down.
    void removeFromDevice(const QStringList& recordIds);

    void stop();

    // The server's page size. 500 is the default; a smaller value makes the
    // cursor walk visible (and is what the paging test uses).
    void setPageLimit(int limit);

    bool busy() const { return busy_; }
    bool failed() const { return failed_; }
    QString status() const { return status_; }
    bool signedIn() const { return state_.signedIn(); }
    QString server() const { return state_.server; }
    QString email() const { return state_.email; }
    QString vaultId() const { return state_.vaultId; }
    QString vaultName() const { return state_.vaultName; }

    QJsonArray vaults() const { return vaults_; }
    QJsonArray items() const { return items_; }          // records + local items, with state
    QJsonArray transfers() const;                        // live transfer rows
    QJsonObject stats() const { return stats_; }
    QString privateDirectory() const { return directory_; }
    QString stagingDirectory() const { return directory_ + QStringLiteral("/staging"); }
    int pendingCount() const;

    // The relative path an item carries inside its package: its folder path and
    // its name, with '/' separators.
    static QString relativePath(const core::VideoInfo& video, const std::vector<core::FolderInfo>& folders);

    // What the operation needs before it may start.
    enum class Launch { SignedOut, Token, Vault };

signals:
    void changed();
    void finished();
    void libraryChanged();

private:
    void launch(const QString& label, Launch requirement,
                const std::function<void(Job&, sync::Client&)>& task);
    void reject(const QString& message);
    void save();
    void load();
    void refreshItems();

    std::shared_ptr<core::Vault> vault_;
    sync::Keys keys_;
    QString directory_;
    sync::SyncState state_;

    std::shared_ptr<Job> job_;
    QTimer* pulse_{nullptr};
    QJsonArray vaults_;
    QJsonArray items_;
    QJsonArray transfers_;
    QJsonObject stats_;
    QString status_;
    int pageLimit_{500};
    bool busy_{false};
    bool failed_{false};
};

}
