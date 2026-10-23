#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <memory>

class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QShowEvent;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

class SyncEngine;

// Protocol 4 sync, as a page inside the main window's content stack rather
// than a dialog. One job per screen, in the order the account flow goes:
//
//   1. Signed out: a single centred card — server URL, account email and the
//      *account* password, entered through placeholders — and one primary
//      "Sign in" button with a single inline error line under it. The vault
//      password is not asked for here: it was already entered to unlock the
//      local database, and it is never sent.
//   2. Signed in: the account's vaults as rows (name, usage), one inline
//      "Choose folder" control that shows the chosen folder's name, one
//      primary "Sync now" button, a quiet last-sync line, transfer progress
//      as an inline bar with counts and bytes, and one list of everything in
//      the vault with a checkbox on every row. The list carries the bulk
//      actions: Select all, Keep on device, Remove from device, Restore,
//      Recycle and Delete permanently, applied to every ticked row in one
//      pass. A permanent delete asks inside the list before it runs.
//
// The page never blocks the window: a running pass only dims its own primary
// action and shows the bar, and the rest of the app stays usable.
class SyncPage final : public QWidget {
    Q_OBJECT

public:
    SyncPage(SyncEngine* engine, std::shared_ptr<core::Vault> vault, QWidget* parent = nullptr);

    // What the "Choose folder" control does once a folder has been picked. The
    // page only accepts the vault it was opened for.
    bool setLocalFolder(const QString& path);

signals:
    // The page asked to go back to the library.
    void closed();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void signIn();
    void chooseFolder();
    void startSync();
    void stopTransfers();
    void signOut();
    void refresh();
    void rebuildVaults();
    void rebuildItems();
    void updateSelection();
    void selectAll(bool select);
    QStringList checkedRecords(const QStringList& states) const;
    void requestPurge();
    void confirmPurge();
    void updateProgress();
    void updateFolderControl();
    QString lastRunLine() const;
    QString displayedStatus() const;

    SyncEngine* engine_;
    std::shared_ptr<core::Vault> vault_;

    QPushButton* back_{nullptr};
    QLabel* account_{nullptr};
    QPushButton* signOutButton_{nullptr};

    // Signed out.
    QFrame* signInCard_{nullptr};
    QLineEdit* url_{nullptr};
    QLineEdit* email_{nullptr};
    QLineEdit* password_{nullptr};
    QPushButton* signInButton_{nullptr};
    QLabel* signInStatus_{nullptr};

    // Signed in.
    QWidget* work_{nullptr};
    QWidget* vaultBox_{nullptr};
    QListWidget* vaults_{nullptr};
    QPushButton* chooseFolder_{nullptr};
    QPushButton* run_{nullptr};
    QLabel* lastRun_{nullptr};
    QWidget* progressRow_{nullptr};
    QProgressBar* progress_{nullptr};
    QLabel* progressText_{nullptr};
    QPushButton* stop_{nullptr};
    QLabel* status_{nullptr};
    QWidget* itemsBox_{nullptr};
    QListWidget* items_{nullptr};
    QPushButton* selectAll_{nullptr};
    QLabel* selectionLabel_{nullptr};
    QPushButton* keep_{nullptr};
    QPushButton* offload_{nullptr};
    QPushButton* restore_{nullptr};
    QPushButton* recycle_{nullptr};
    QPushButton* purgeButton_{nullptr};
    QWidget* confirmBar_{nullptr};
    QLabel* confirmText_{nullptr};
    QPushButton* confirmDelete_{nullptr};
    QPushButton* confirmCancel_{nullptr};

    QString vaultSignature_;
    QString itemsSignature_;
    QSet<QString> checked_;
    QStringList pendingPurge_;
    bool building_{false};
    bool firstShown_{false};
    bool vaultsRequested_{false};
    // Set when "Sync now" starts a pass, so a restore or a purge does not
    // overwrite the last-sync time.
    bool passStarted_{false};
    QDateTime lastPassAt_;
};

} // namespace videovault::app
