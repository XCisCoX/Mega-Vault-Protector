// The sync page's surface for protocol 4: one centered sign-in card, then the
// vault rows, the inline Choose folder control, Sync now, the last-sync line,
// the transfer bar and the debris rows. Runs offscreen against a real temporary
// vault; no server is needed, so the sign-in failures shown here are the local
// refusals and the transport failure.
#include "videovault/app/sync_engine.hpp"
#include "videovault/app/sync_page.hpp"
#include "videovault/app/sync_state.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

std::filesystem::path fsPath(const QString& path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::u8path(path.toUtf8().constData());
#endif
}

QString nativePath(const std::filesystem::path& path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

void waitFor(videovault::app::SyncEngine& engine, int timeoutMs = 30000) {
    if (!engine.busy()) return;
    QEventLoop loop;
    bool finished = false;
    QObject::connect(&engine, &videovault::app::SyncEngine::finished, &loop, [&] {
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    check(finished, "the operation finishes");
}

// Every label the page shows, so a test can prove no full path leaks into the
// chrome (name only, path in the tooltip).
QString allLabelText(QWidget& page) {
    QString text;
    for (auto* label : page.findChildren<QLabel*>()) {
        text += label->text();
        text += QLatin1Char('\n');
    }
    for (auto* button : page.findChildren<QPushButton*>()) {
        text += button->text();
        text += QLatin1Char('\n');
    }
    return text;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using videovault::app::SyncEngine;
    using videovault::app::SyncPage;
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary directory");
        auto created = videovault::core::Vault::create(fsPath(temp.path() + "/vault"), "local", {8192, 1, 1});
        check(created.has_value(), "create a vault");
        auto vault = std::make_shared<videovault::core::Vault>(std::move(created.value()));
        const std::vector<unsigned char> bytes{'n', 'o', 't', 'e'};
        check(vault->import_bytes(bytes, "note.txt").has_value(), "import one item");

        const QString stateDirectory = temp.path() + "/state";
        SyncEngine engine(vault, videovault::app::sync::deriveKeys(stateDirectory, "local"), stateDirectory);
        SyncPage page(&engine, vault);

        auto* card = page.findChild<QFrame*>("syncSignInCard");
        auto* work = page.findChild<QWidget*>("syncWork");
        auto* url = page.findChild<QLineEdit*>("syncServer");
        auto* email = page.findChild<QLineEdit*>("syncEmail");
        auto* password = page.findChild<QLineEdit*>("syncAccountPassword");
        auto* signIn = page.findChild<QPushButton*>("syncSignIn");
        auto* back = page.findChild<QPushButton*>("syncBack");
        auto* vaults = page.findChild<QListWidget*>("syncVaults");
        auto* chooseFolder = page.findChild<QPushButton*>("syncChooseFolder");
        auto* run = page.findChild<QPushButton*>("syncRun");
        auto* stop = page.findChild<QPushButton*>("syncStop");
        auto* lastRun = page.findChild<QLabel*>("syncLastRun");
        auto* progress = page.findChild<QProgressBar*>("syncProgress");
        auto* progressText = page.findChild<QLabel*>("syncProgressText");
        auto* itemList = page.findChild<QListWidget*>("syncItems");
        auto* selectAll = page.findChild<QPushButton*>("syncSelectAll");
        auto* keep = page.findChild<QPushButton*>("syncKeep");
        auto* offload = page.findChild<QPushButton*>("syncOffload");
        auto* restore = page.findChild<QPushButton*>("syncRestore");
        auto* recycle = page.findChild<QPushButton*>("syncRecycle");
        auto* purge = page.findChild<QPushButton*>("syncPurge");
        auto* signInStatus = page.findChild<QLabel*>("syncSignInStatus");
        check(card && work && url && email && password && signIn && back && vaults && chooseFolder
                  && run && stop && lastRun && progress && progressText && itemList && selectAll
                  && keep && offload && restore && recycle && purge && signInStatus,
              "the sign-in card, the vault rows, Choose folder, Sync now, the progress bar and the item list with its bulk actions all exist");

        // -- it is a page, not a dialog ------------------------------------
        check(page.findChild<QDialog*>() == nullptr, "no dialog anywhere in the sync surface");
        check(page.findChild<QTabWidget*>() == nullptr, "no tabs");
        check(page.findChildren<QCheckBox*>().isEmpty(), "no checkboxes");
        check(page.parentWidget() == nullptr, "the page can be built without a window around it");
        check(back->text() == "Back", "the page carries a way back to the library");

        // -- signed out: one card, three placeholders ----------------------
        check(!card->isHidden(), "the page opens on the sign-in card");
        check(work->isHidden(), "the vault and transfer section waits for a signed-in account");
        check(url->placeholderText().contains("Server"), "the server field is labelled by its placeholder");
        check(email->placeholderText().contains("email"), "the email field is labelled by its placeholder");
        check(password->placeholderText().contains("password"), "the password field is labelled by its placeholder");
        check(password->placeholderText().contains("Account"),
              "the password placeholder asks for the account password, not the vault's");
        check(password->echoMode() == QLineEdit::Password, "the account password is masked");
        int passwordFields = 0;
        for (auto* field : page.findChildren<QLineEdit*>()) {
            if (field->echoMode() == QLineEdit::Password) ++passwordFields;
        }
        check(passwordFields == 1, "exactly one password field: the account password");
        check(!page.findChild<QLineEdit*>("syncVaultPassword")
                  && !page.findChild<QLineEdit*>("syncKey")
                  && !page.findChild<QLineEdit*>("syncLibraryKey"),
              "there is no vault-password or library-key field");
        check(signIn->text() == "Sign in", "one primary action on the card");
        check(signInStatus->text().isEmpty() && signInStatus->isHidden(),
              "no error line until something failed");
        check(url->minimumHeight() >= 38 && password->minimumHeight() >= 38,
              "the fields are at least 38px high");
        check(!run->isEnabled(), "Sync now waits for a signed-in account");
        check(stop->isHidden(), "there is nothing to stop before a transfer");
        check(work->isHidden() && itemList->count() == 0, "no item rows before a sync");
        check(!keep->isEnabled() && !offload->isEnabled() && !restore->isEnabled()
                  && !recycle->isEnabled() && !purge->isEnabled(),
              "the bulk actions stay disabled until something is selected");
        check(selectAll->text() == "Select all", "the list offers Select all");

        // A local item is listed as pending: already local, openable, and
        // waiting for the first pass to carry it up.
        check(engine.pendingCount() == 1, "one item is queued");
        check(lastRun->text().contains("pending"), "the quiet line says an item is pending");
        check(lastRun->text().contains("1 item pending"), "the pending count is stated, not a state name");

        // -- local refusals, one inline line ---------------------------------
        url->clear();
        email->setText("owner@example.com");
        password->setText("account-password");
        signIn->click();
        check(engine.failed(), "an empty URL is refused");
        check(!signInStatus->isHidden(), "the refusal appears in place");
        check(signInStatus->text().contains("server URL"), "the URL rule is stated");
        check(!engine.busy(), "a local refusal starts no network work");

        url->setText("http://127.0.0.1:1");
        email->setText("owner");
        signIn->click();
        check(engine.failed() && signInStatus->text().contains("email"),
              "an address without a domain is refused");

        email->setText("owner@example.com");
        password->clear();
        signIn->click();
        check(engine.failed() && signInStatus->text().contains("account password"),
              "the account password is required");
        check(!engine.busy(), "still no network work");

        // -- choose folder --------------------------------------------------
        check(!page.setLocalFolder(temp.path() + "/not-a-vault"), "a folder that is not the open vault is refused");
        check(engine.localFolder().isEmpty(), "the refused folder was not remembered");
        check(page.setLocalFolder(nativePath(vault->root_path())), "the open vault is accepted");
        check(engine.localFolder() == nativePath(vault->root_path()), "the chosen folder is remembered");
        const QString expectedName = QDir(nativePath(vault->root_path())).dirName();
        check(chooseFolder->text() == expectedName, "the control shows the folder's name");
        check(chooseFolder->toolTip() == nativePath(vault->root_path()),
              "the full path stays in the tooltip");
        check(!allLabelText(page).contains(nativePath(vault->root_path())),
              "no full path is painted in the chrome");

        // -- the reachable-but-wrong-server path ----------------------------
        password->setText("account-password");
        signIn->click();
        check(engine.busy(), "sign in runs on a worker");
        check(page.isEnabled() && back->isEnabled(),
              "the page stays usable while the request runs");
        check(!signIn->isEnabled(), "only the action that started it is busy");
        waitFor(engine);
        check(engine.failed(), "an unreachable server fails the sign-in");
        check(engine.status().contains("Cannot reach"), "the failure names the connection");
        check(!engine.signedIn(), "no account was signed in");
        check(!card->isHidden(), "the page stays on the sign-in card");
        check(signInStatus->text().contains("Cannot reach"), "the failure lands in the inline line");
        check(password->text().isEmpty(), "the account password is not kept in the field");

        // Sync now is still gated after the failure.
        check(!run->isEnabled(), "Sync now is still waiting for a signed-in account");
        engine.syncNow();
        check(engine.failed() && engine.status().contains("Sign in first"), "Sync without sign-in says to sign in");
        check(!engine.busy(), "no request was attempted");

        page.show();
        QApplication::processEvents();
        check(signIn->isVisible() && password->isVisible() && url->isVisible(),
              "the sign-in controls are reachable once shown");
        check(!signInStatus->text().isEmpty() && signInStatus->isVisible(),
              "the error line is visible in place");
        check(!work->isVisible(), "the signed-in section stays hidden");

        std::cout << "sync page: no dialog or tab, one masked account-password field and no vault-password "
                     "field, placeholders instead of labels, inline sign-in errors, inline Choose folder with the "
                     "name on screen and the path in the tooltip, gated Sync now, pending marker, an interactive "
                     "page during a request, and failure reporting passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
