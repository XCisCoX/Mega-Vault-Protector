#include "videovault/app/sync_page.hpp"

#include "videovault/app/sync_engine.hpp"
#include "videovault/core/vault.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace videovault::app {

namespace {

// The app's secondary text tone: labels that explain, never compete.
const char* const kSecondary = "palette(placeholder-text)";

QString countText(const int count, const QString& singular, const QString& plural) {
    return QStringLiteral("%1 %2").arg(QString::number(count), count == 1 ? singular : plural);
}

// Sizes stay readable at a glance: bytes are shown in MB once they pass a
// kilobyte, which is what the transfer panel is actually about.
QString bytesText(const double bytes) {
    if (bytes < 1024.0) {
        return QStringLiteral("%1 B").arg(static_cast<qint64>(bytes));
    }
    if (bytes < 1024.0 * 1024.0) {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    }
    if (bytes < 1024.0 * 1024.0 * 1024.0) {
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

QString folderLeaf(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QString leaf = QDir(path).dirName();
    return leaf.isEmpty() ? path : leaf;
}

QString pathText(const std::filesystem::path& path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

QString elided(const QString& text, const QFont& font, const int width) {
    return QFontMetrics(font).elidedText(text, Qt::ElideRight, std::max(24, width));
}

QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;"
                                        " font: 600 12px \"Segoe UI\";")
                             .arg(QString::fromLatin1(kSecondary)));
    return label;
}

QLabel* quietLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;"
                                        " font: 12px \"Segoe UI\";")
                             .arg(QString::fromLatin1(kSecondary)));
    return label;
}

QWidget* hairline(QWidget* parent) {
    auto* line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAttribute(Qt::WA_StyledBackground, true);
    line->setStyleSheet(QStringLiteral("background: palette(midlight);"));
    return line;
}

QFont rowFont() {
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    return font;
}

QFont metaFont() {
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(12);
    return font;
}

// One list line: content on the left, metadata pinned right, a hairline under
// it. Every list in the page is built from this, so no list ever reads as a
// table with columns.
struct RowParts {
    QWidget* row{nullptr};
    QHBoxLayout* body{nullptr};
};

RowParts makeRow(QWidget* parent, const int height, const QString& name = {}) {
    RowParts parts;
    parts.row = new QWidget(parent);
    if (!name.isEmpty()) {
        parts.row->setObjectName(name);
    }
    auto* outer = new QVBoxLayout(parts.row);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* body = new QWidget(parts.row);
    body->setMinimumHeight(std::max(1, height - 1));
    parts.body = new QHBoxLayout(body);
    parts.body->setContentsMargins(14, 0, 10, 0);
    parts.body->setSpacing(10);
    outer->addWidget(body, 1);
    outer->addWidget(hairline(parts.row));
    return parts;
}

QString rowStyle(const char* name) {
    return QStringLiteral(
               "QListWidget#%1 { background: palette(base); border: 1px solid palette(midlight);"
               " border-radius: 14px; outline: none; }"
               "QListWidget#%1::item { border: none; background: transparent; }"
               "QListWidget#%1::item:hover { background: palette(midlight); }"
               "QListWidget#%1::item:selected { background: rgba(51, 144, 236, 70); }")
        .arg(QString::fromLatin1(name));
}

} // namespace

SyncPage::SyncPage(SyncEngine* engine, std::shared_ptr<core::Vault> vault, QWidget* parent)
    : QWidget(parent), engine_(engine), vault_(std::move(vault)) {
    setObjectName(QStringLiteral("syncPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("QWidget#syncPage { background: palette(window); }"));
    setMinimumWidth(360);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // -- header: Back, the page name, and who is signed in -----------------
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("syncBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(52);
    bar->setStyleSheet(QStringLiteral(
        "QWidget#syncBar { background: palette(alternate-base);"
        " border-bottom: 1px solid palette(midlight); }"));
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(16, 0, 16, 0);
    barLayout->setSpacing(10);

    back_ = new QPushButton(QStringLiteral("Back"), bar);
    back_->setObjectName(QStringLiteral("syncBack"));
    back_->setCursor(Qt::PointingHandCursor);
    back_->setFocusPolicy(Qt::NoFocus);
    back_->setStyleSheet(QStringLiteral(
        "QPushButton { color: #3390ec; background: transparent; border: none;"
        " padding: 2px; min-height: 28px; font: 600 13px \"Segoe UI\"; }"
        "QPushButton:hover { color: #5aa6f0; }"));
    auto* title = new QLabel(QStringLiteral("Sync"), bar);
    title->setStyleSheet(QStringLiteral(
        "color: white; background: transparent; font: 600 15px \"Segoe UI\";"));
    account_ = quietLabel(bar);
    account_->setObjectName(QStringLiteral("syncAccount"));
    account_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    signOutButton_ = new QPushButton(QStringLiteral("Sign out"), bar);
    signOutButton_->setObjectName(QStringLiteral("syncSignOut"));
    signOutButton_->setCursor(Qt::PointingHandCursor);
    signOutButton_->setFocusPolicy(Qt::NoFocus);
    signOutButton_->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; background: transparent; border: none;"
        " padding: 2px 8px; min-height: 28px; font: 12px \"Segoe UI\"; }"
        "QPushButton:hover { color: white; }"
        "QPushButton:disabled { color: palette(placeholder-text); }")
                                       .arg(QString::fromLatin1(kSecondary)));
    barLayout->addWidget(back_);
    barLayout->addSpacing(6);
    barLayout->addWidget(title);
    barLayout->addStretch(1);
    barLayout->addWidget(account_);
    barLayout->addWidget(signOutButton_);
    root->addWidget(bar);

    // -- one centred column, scrolled only when it has to be ---------------
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("syncScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#syncScroll { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: palette(mid);"
        " border-radius: 4px; min-height: 28px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"));
    scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));

    auto* host = new QWidget(scroll);
    host->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* hostLayout = new QHBoxLayout(host);
    hostLayout->setContentsMargins(24, 0, 24, 0);
    hostLayout->addStretch(1);
    auto* column = new QWidget(host);
    column->setMaximumWidth(560);
    column->setMinimumWidth(320);
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 28, 0, 28);
    columnLayout->setSpacing(16);
    hostLayout->addWidget(column);
    hostLayout->addStretch(1);
    scroll->setWidget(host);
    root->addWidget(scroll, 1);

    columnLayout->addStretch(1);

    // -- signed out: one card, three fields, one button --------------------
    signInCard_ = new QFrame(column);
    signInCard_->setObjectName(QStringLiteral("syncSignInCard"));
    signInCard_->setAttribute(Qt::WA_StyledBackground, true);
    signInCard_->setStyleSheet(QStringLiteral(
        "QFrame#syncSignInCard { background: palette(base);"
        " border: 1px solid palette(midlight); border-radius: 22px; }"
        "QFrame#syncSignInCard QLineEdit { min-height: 38px; color: white;"
        " background: palette(alternate-base); border: 1px solid palette(midlight);"
        " border-radius: 14px; padding: 0 12px; font-size: 13px; }"
        "QFrame#syncSignInCard QLineEdit:focus { border: 1px solid #3390ec; }"
        "QFrame#syncSignInCard QPushButton#syncSignIn { min-height: 40px; color: white;"
        " background: #3390ec; border: 1px solid #3390ec; border-radius: 16px;"
        " font: 600 13px \"Segoe UI\"; }"
        "QFrame#syncSignInCard QPushButton#syncSignIn:hover { background: #4ba0f5; }"
        "QFrame#syncSignInCard QPushButton#syncSignIn:disabled {"
        " color: palette(placeholder-text); background: palette(midlight); border-color: palette(midlight); }"));
    auto* cardLayout = new QVBoxLayout(signInCard_);
    cardLayout->setContentsMargins(28, 26, 28, 26);
    cardLayout->setSpacing(10);

    auto* cardTitle = new QLabel(QStringLiteral("Sign in to sync"), signInCard_);
    cardTitle->setStyleSheet(QStringLiteral(
        "color: white; background: transparent; font: 600 19px \"Segoe UI\";"));
    auto* cardText = new QLabel(
        QStringLiteral("Use the account that owns this vault. Your vault password is never "
                       "asked for here, and the account password is never stored."),
        signInCard_);
    cardText->setWordWrap(true);
    cardText->setStyleSheet(QStringLiteral("color: %1; background: transparent;"
                                           " font: 12px \"Segoe UI\";")
                                .arg(QString::fromLatin1(kSecondary)));

    url_ = new QLineEdit(engine_->server(), signInCard_);
    url_->setObjectName(QStringLiteral("syncServer"));
    url_->setPlaceholderText(QStringLiteral("Server URL"));
    url_->setMinimumHeight(38);
    email_ = new QLineEdit(engine_->email(), signInCard_);
    email_->setObjectName(QStringLiteral("syncEmail"));
    email_->setPlaceholderText(QStringLiteral("Account email"));
    email_->setMinimumHeight(38);
    password_ = new QLineEdit(signInCard_);
    password_->setObjectName(QStringLiteral("syncAccountPassword"));
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(QStringLiteral("Account password"));
    password_->setMinimumHeight(38);

    signInButton_ = new QPushButton(QStringLiteral("Sign in"), signInCard_);
    signInButton_->setObjectName(QStringLiteral("syncSignIn"));
    signInButton_->setCursor(Qt::PointingHandCursor);
    signInButton_->setAutoDefault(false);
    signInButton_->setDefault(false);
    signInButton_->setMinimumHeight(40);

    // One line, in place, under the button. Never a popup.
    signInStatus_ = new QLabel(signInCard_);
    signInStatus_->setObjectName(QStringLiteral("syncSignInStatus"));
    signInStatus_->setWordWrap(false);
    signInStatus_->setStyleSheet(QStringLiteral(
        "color: #ff6b6b; background: transparent; font: 12px \"Segoe UI\";"));
    signInStatus_->hide();

    cardLayout->addWidget(cardTitle);
    cardLayout->addWidget(cardText);
    cardLayout->addSpacing(6);
    cardLayout->addWidget(url_);
    cardLayout->addWidget(email_);
    cardLayout->addWidget(password_);
    cardLayout->addSpacing(4);
    cardLayout->addWidget(signInButton_);
    cardLayout->addWidget(signInStatus_);
    columnLayout->addWidget(signInCard_);

    // -- signed in: vault, folder, one button, progress, debris ------------
    work_ = new QWidget(column);
    work_->setObjectName(QStringLiteral("syncWork"));
    auto* workLayout = new QVBoxLayout(work_);
    workLayout->setContentsMargins(0, 0, 0, 0);
    workLayout->setSpacing(14);

    vaultBox_ = new QWidget(work_);
    auto* vaultLayout = new QVBoxLayout(vaultBox_);
    vaultLayout->setContentsMargins(0, 0, 0, 0);
    vaultLayout->setSpacing(6);
    vaultLayout->addWidget(sectionLabel(QStringLiteral("Vaults on this account"), vaultBox_));
    vaults_ = new QListWidget(vaultBox_);
    vaults_->setObjectName(QStringLiteral("syncVaults"));
    vaults_->setFrameShape(QFrame::NoFrame);
    vaults_->setSelectionMode(QAbstractItemView::SingleSelection);
    vaults_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    vaults_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    vaults_->setStyleSheet(rowStyle("syncVaults"));
    vaultLayout->addWidget(vaults_);
    vaultBox_->hide();
    workLayout->addWidget(vaultBox_);

    chooseFolder_ = new QPushButton(QStringLiteral("Choose folder"), work_);
    chooseFolder_->setObjectName(QStringLiteral("syncChooseFolder"));
    chooseFolder_->setCursor(Qt::PointingHandCursor);
    chooseFolder_->setMinimumHeight(44);
    chooseFolder_->setStyleSheet(QStringLiteral(
        "QPushButton#syncChooseFolder { text-align: left; padding: 0 14px;"
        " min-height: 44px; color: palette(text); background: palette(alternate-base);"
        " border: 1px solid palette(midlight); border-radius: 14px;"
        " font-size: 13px; }"
        "QPushButton#syncChooseFolder:hover { background: palette(midlight); }"
        "QPushButton#syncChooseFolder:disabled { color: palette(placeholder-text); }"));
    workLayout->addWidget(chooseFolder_);

    run_ = new QPushButton(QStringLiteral("Sync now"), work_);
    run_->setObjectName(QStringLiteral("syncRun"));
    run_->setCursor(Qt::PointingHandCursor);
    run_->setMinimumHeight(40);
    run_->setStyleSheet(QStringLiteral(
        "QPushButton#syncRun { min-height: 40px; color: white; background: #3390ec;"
        " border: 1px solid #3390ec; border-radius: 16px;"
        " font: 600 13px \"Segoe UI\"; }"
        "QPushButton#syncRun:hover { background: #4ba0f5; }"
        "QPushButton#syncRun:disabled { color: palette(placeholder-text); background: palette(midlight);"
        " border-color: palette(midlight); }"));
    workLayout->addWidget(run_);

    lastRun_ = quietLabel(work_);
    lastRun_->setObjectName(QStringLiteral("syncLastRun"));
    lastRun_->setWordWrap(false);
    workLayout->addWidget(lastRun_);

    progressRow_ = new QWidget(work_);
    auto* progressLayout = new QVBoxLayout(progressRow_);
    progressLayout->setContentsMargins(2, 2, 2, 0);
    progressLayout->setSpacing(6);
    progress_ = new QProgressBar(progressRow_);
    progress_->setObjectName(QStringLiteral("syncProgress"));
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setStyleSheet(QStringLiteral(
        "QProgressBar { background: palette(alternate-base); border: none; border-radius: 3px;"
        " min-height: 6px; max-height: 6px; }"
        "QProgressBar::chunk { background: #3390ec; border-radius: 3px; }"));
    auto* progressLine = new QHBoxLayout();
    progressLine->setContentsMargins(0, 0, 0, 0);
    progressLine->setSpacing(8);
    progressText_ = quietLabel(progressRow_);
    progressText_->setObjectName(QStringLiteral("syncProgressText"));
    stop_ = new QPushButton(QStringLiteral("Stop"), progressRow_);
    stop_->setObjectName(QStringLiteral("syncStop"));
    stop_->setCursor(Qt::PointingHandCursor);
    stop_->setFocusPolicy(Qt::NoFocus);
    stop_->setStyleSheet(QStringLiteral(
        "QPushButton { color: #3390ec; background: transparent; border: none;"
        " padding: 0 4px; min-height: 28px; font: 600 12px \"Segoe UI\"; }"
        "QPushButton:hover { color: #5aa6f0; }"));
    progressLine->addWidget(progressText_, 1);
    progressLine->addWidget(stop_);
    progressLayout->addWidget(progress_);
    progressLayout->addLayout(progressLine);
    progressRow_->hide();
    workLayout->addWidget(progressRow_);

    status_ = quietLabel(work_);
    status_->setObjectName(QStringLiteral("syncStatus"));
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->hide();
    workLayout->addWidget(status_);

    conflictBox_ = new QWidget(work_);
    auto* conflictLayout = new QVBoxLayout(conflictBox_);
    conflictLayout->setContentsMargins(0, 0, 0, 0);
    conflictLayout->setSpacing(6);
    conflictLayout->addWidget(
        sectionLabel(QStringLiteral("Conflict copies"), conflictBox_));
    conflicts_ = new QListWidget(conflictBox_);
    conflicts_->setObjectName(QStringLiteral("syncConflicts"));
    conflicts_->setFrameShape(QFrame::NoFrame);
    conflicts_->setSelectionMode(QAbstractItemView::NoSelection);
    conflicts_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conflicts_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    conflicts_->setStyleSheet(rowStyle("syncConflicts"));
    conflictLayout->addWidget(conflicts_);
    conflictBox_->hide();
    workLayout->addWidget(conflictBox_);

    debrisBox_ = new QWidget(work_);
    auto* debrisLayout = new QVBoxLayout(debrisBox_);
    debrisLayout->setContentsMargins(0, 0, 0, 0);
    debrisLayout->setSpacing(6);
    debrisLayout->addWidget(
        sectionLabel(QStringLiteral("Removed on another device"), debrisBox_));
    debris_ = new QListWidget(debrisBox_);
    debris_->setObjectName(QStringLiteral("syncDebris"));
    debris_->setFrameShape(QFrame::NoFrame);
    debris_->setSelectionMode(QAbstractItemView::NoSelection);
    debris_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    debris_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    debris_->setStyleSheet(rowStyle("syncDebris"));
    debrisLayout->addWidget(debris_);
    debrisBox_->hide();
    workLayout->addWidget(debrisBox_);

    work_->hide();
    columnLayout->addWidget(work_);
    columnLayout->addStretch(1);

    // -- wiring -----------------------------------------------------------
    connect(back_, &QPushButton::clicked, this, [this] { emit closed(); });
    connect(signInButton_, &QPushButton::clicked, this, &SyncPage::signIn);
    connect(password_, &QLineEdit::returnPressed, this, &SyncPage::signIn);
    connect(chooseFolder_, &QPushButton::clicked, this, &SyncPage::chooseFolder);
    connect(run_, &QPushButton::clicked, this, &SyncPage::startSync);
    connect(stop_, &QPushButton::clicked, this, &SyncPage::stopTransfers);
    connect(signOutButton_, &QPushButton::clicked, this, &SyncPage::signOut);
    connect(vaults_, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* current, QListWidgetItem*) {
            if (building_ || current == nullptr) {
                return;
            }
            const QString id = current->data(Qt::UserRole).toString();
            if (id.isEmpty() || id == engine_->vaultId()) {
                return;
            }
            engine_->chooseVault(id, current->data(Qt::UserRole + 1).toString());
            refresh();
        });
    connect(engine_, &SyncEngine::changed, this, &SyncPage::refresh);
    connect(engine_, &SyncEngine::finished, this, [this] {
        if (passStarted_) {
            passStarted_ = false;
            if (!engine_->failed()) {
                lastPassAt_ = QDateTime::currentDateTime();
            }
        }
        refresh();
    });
    connect(engine_, &SyncEngine::libraryChanged, this, [this] {
        rebuildDebris();
        rebuildConflicts();
    });

    refresh();
}

bool SyncPage::setLocalFolder(const QString& path) {
    if (!engine_->setLocalFolder(path)) {
        refresh();
        return false;
    }
    refresh();
    return true;
}

void SyncPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!firstShown_) {
        firstShown_ = true;
        // The rows are filled once the page has its real width, so the first
        // paint is not measured against the placeholder geometry.
        QTimer::singleShot(0, this, [this] { refresh(); });
    }
    if (engine_->signedIn() && engine_->vaults().isEmpty() && !engine_->busy()
        && !vaultsRequested_) {
        vaultsRequested_ = true;
        engine_->refreshVaults();
    }
    refresh();
}

void SyncPage::signIn() {
    if (engine_->busy()) {
        return;
    }
    const QString password = password_->text();
    engine_->signIn(QUrl(url_->text().trimmed()), email_->text().trimmed(), password);
    // The account password is never kept: the field is emptied as soon as the
    // engine has it, and the engine wipes its copy when the request ends.
    password_->clear();
    refresh();
}

void SyncPage::chooseFolder() {
    const QString start = engine_->localFolder().isEmpty()
        ? pathText(vault_ != nullptr ? vault_->root_path() : std::filesystem::path())
        : engine_->localFolder();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose the folder this vault syncs into"), start);
    if (chosen.isEmpty()) {
        return;
    }
    setLocalFolder(chosen);
}

void SyncPage::startSync() {
    if (engine_->busy()) {
        return;
    }
    passStarted_ = true;
    engine_->syncNow();
    refresh();
}

void SyncPage::stopTransfers() {
    engine_->stop();
    refresh();
}

void SyncPage::signOut() {
    if (engine_->busy()) {
        return;
    }
    engine_->signOut();
    password_->clear();
    lastPassAt_ = QDateTime();
    vaultSignature_.clear();
    debrisSignature_.clear();
    conflictSignature_.clear();
    confirmingRecord_.clear();
    refresh();
}

void SyncPage::refresh() {
    const bool signedIn = engine_->signedIn();
    const bool busy = engine_->busy();
    const bool failed = engine_->failed();

    signInCard_->setVisible(!signedIn);
    work_->setVisible(signedIn);
    account_->setVisible(signedIn);
    signOutButton_->setVisible(signedIn);
    account_->setText(signedIn ? engine_->email() : QString());
    account_->setToolTip(signedIn ? engine_->server() : QString());

    // The quiet line, the folder control and the bar belong to the signed-in
    // section, but they are kept current while it is hidden too: nothing is
    // stale on the frame the account lands.
    updateFolderControl();
    lastRun_->setText(lastRunLine());
    lastRun_->setToolTip(lastRun_->text());
    updateProgress();

    // Gating is shared by both states: the signed-in actions are simply never
    // enabled before there is an account, a vault and a folder.
    const bool haveVault = !engine_->vaultId().isEmpty();
    const bool haveFolder = !engine_->localFolder().isEmpty();
    chooseFolder_->setEnabled(signedIn && !busy);
    vaults_->setEnabled(!busy);
    signOutButton_->setEnabled(!busy);
    run_->setEnabled(signedIn && !busy && haveVault && haveFolder);
    run_->setText(busy ? QStringLiteral("Syncing…") : QStringLiteral("Sync now"));
    // Stop lives beside the bar and only while a pass is running.
    stop_->setVisible(signedIn && busy);

    if (!signedIn) {
        const QString text = engine_->status();
        const bool showError = failed && !text.isEmpty();
        signInStatus_->setText(showError ? text : QString());
        signInStatus_->setVisible(showError);
        signInStatus_->setToolTip(showError ? text : QString());
        url_->setEnabled(!busy);
        email_->setEnabled(!busy);
        password_->setEnabled(!busy);
        signInButton_->setEnabled(!busy);
        signInButton_->setText(busy ? QStringLiteral("Signing in…")
                                    : QStringLiteral("Sign in"));
        return;
    }

    const QString statusText = displayedStatus();
    const bool problem = failed;
    // A failure is always stated. So is a signed-in account that has nothing
    // to pick, which would otherwise be a dead end with a disabled button.
    const bool showStatus = !statusText.isEmpty()
        && (problem || (!busy && engine_->vaults().isEmpty()));
    status_->setText(showStatus ? statusText : QString());
    status_->setVisible(showStatus);
    status_->setStyleSheet(problem
        ? QStringLiteral("color: #ff6b6b; background: transparent; font: 12px \"Segoe UI\";")
        : QStringLiteral("color: %1; background: transparent; font: 12px \"Segoe UI\";")
              .arg(QString::fromLatin1(kSecondary)));

    rebuildVaults();
    rebuildConflicts();
    rebuildDebris();
}

QString SyncPage::lastRunLine() const {
    QStringList parts;
    if (lastPassAt_.isValid()) {
        const qint64 seconds = lastPassAt_.secsTo(QDateTime::currentDateTime());
        QString when;
        if (seconds < 60) {
            when = QStringLiteral("just now");
        } else if (seconds < 3600) {
            when = countText(int(seconds / 60), QStringLiteral("minute ago"),
                QStringLiteral("minutes ago"));
        } else if (seconds < 86400) {
            when = countText(int(seconds / 3600), QStringLiteral("hour ago"),
                QStringLiteral("hours ago"));
        } else {
            when = countText(int(seconds / 86400), QStringLiteral("day ago"),
                QStringLiteral("days ago"));
        }
        parts << QStringLiteral("Last sync %1").arg(when);

        const QJsonObject stats = engine_->stats();
        const int published = stats.value(QStringLiteral("uploaded")).toInt()
            + stats.value(QStringLiteral("updated")).toInt();
        const int received = stats.value(QStringLiteral("imported")).toInt();
        const int removed = stats.value(QStringLiteral("tombstoned")).toInt();
        QStringList detail;
        if (published > 0) {
            detail << QStringLiteral("%1 published").arg(published);
        }
        if (received > 0) {
            detail << QStringLiteral("%1 received").arg(received);
        }
        if (removed > 0) {
            detail << QStringLiteral("%1 removed").arg(removed);
        }
        if (!detail.isEmpty()) {
            parts << detail.join(QStringLiteral(", "));
        }
    } else {
        parts << QStringLiteral("No sync in this session yet");
    }

    const int pending = engine_->pendingCount();
    if (pending > 0) {
        parts << (pending == 1 ? QStringLiteral("1 item pending")
                               : QStringLiteral("%1 items pending").arg(pending));
    }
    if (pending == 0 && lastPassAt_.isValid()) {
        parts << QStringLiteral("Up to date");
    }
    return parts.join(QStringLiteral(" · "));
}

QString SyncPage::displayedStatus() const {
    QString text = engine_->status();
    const QString folder = engine_->localFolder();
    if (!folder.isEmpty()) {
        text.replace(folder, folderLeaf(folder));
    }
    const QString privateDirectory = engine_->privateDirectory();
    if (!privateDirectory.isEmpty()) {
        text.replace(privateDirectory, QStringLiteral("this device's sync data"));
    }
    return text;
}

void SyncPage::updateFolderControl() {
    const QString folder = engine_->localFolder();
    const QFont font = chooseFolder_->font();
    if (folder.isEmpty()) {
        chooseFolder_->setText(QStringLiteral("Choose folder"));
        chooseFolder_->setToolTip(
            QStringLiteral("Pick the folder this vault syncs into"));
        return;
    }
    // Only the folder's own name is chrome; the full path stays in the tooltip.
    chooseFolder_->setText(elided(folderLeaf(folder), font, 420));
    chooseFolder_->setToolTip(folder);
}

void SyncPage::updateProgress() {
    const QJsonArray rows = engine_->transfers();
    if (rows.isEmpty() || !engine_->busy()) {
        progressRow_->hide();
        return;
    }
    double done = 0.0;
    double total = 0.0;
    int finished = 0;
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        done += row.value(QStringLiteral("done")).toDouble();
        total += row.value(QStringLiteral("total")).toDouble();
        if (row.value(QStringLiteral("state")).toString() == QLatin1String("done")) {
            ++finished;
        }
    }
    progress_->setRange(0, 1000);
    progress_->setValue(total > 0.0
            ? static_cast<int>(std::min(1000.0, (done * 1000.0) / total))
            : 0);
    progressText_->setText(
        QStringLiteral("%1 of %2 items · %3 of %4")
            .arg(QString::number(finished), QString::number(rows.size()), bytesText(done),
                bytesText(total)));
    progressText_->setToolTip(progressText_->text());
    progressRow_->show();
}

void SyncPage::rebuildVaults() {
    const QJsonArray known = engine_->vaults();
    QStringList signature;
    signature.reserve(known.size());
    for (const auto& value : known) {
        const QJsonObject vault = value.toObject();
        signature << vault.value(QStringLiteral("id")).toString();
    }
    const QString joined = signature.join(QLatin1Char('\n'));
    if (joined == vaultSignature_ && vaults_->count() == known.size()) {
        vaultBox_->setVisible(!known.isEmpty());
        return;
    }
    vaultSignature_ = joined;

    const QString current = engine_->vaultId();
    building_ = true;
    vaults_->clear();
    for (const auto& value : known) {
        const QJsonObject vault = value.toObject();
        const QString id = vault.value(QStringLiteral("id")).toString();
        const QString name = vault.value(QStringLiteral("name")).toString();
        const int records = vault.value(QStringLiteral("records")).toInt();
        const double bytes = vault.value(QStringLiteral("bytes")).toDouble();

        auto* item = new QListWidgetItem(vaults_);
        item->setText(QString());
        item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole + 1, name);
        item->setSizeHint(QSize(0, 45));

        const RowParts parts = makeRow(vaults_, 45, QStringLiteral("syncVaultRow"));
        auto* nameLabel = new QLabel(name.isEmpty() ? QStringLiteral("Untitled vault") : name,
            parts.row);
        nameLabel->setFont(rowFont());
        nameLabel->setStyleSheet(QStringLiteral(
            "color: white; background: transparent;"));
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto* usage = quietLabel(parts.row);
        usage->setFont(metaFont());
        usage->setStyleSheet(QStringLiteral(
            "color: %1; background: transparent;")
                                 .arg(QString::fromLatin1(kSecondary)));
        usage->setText(QStringLiteral("%1 · %2")
                           .arg(countText(records, QStringLiteral("item"),
                               QStringLiteral("items")),
                               bytesText(bytes)));
        usage->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        parts.body->addWidget(nameLabel, 1);
        parts.body->addWidget(usage);
        vaults_->setItemWidget(item, parts.row);
        if (id == current) {
            vaults_->setCurrentItem(item);
        }
    }
    building_ = false;

    vaultBox_->setVisible(!known.isEmpty());
    const int visibleRows = std::min(int(known.size()), 3);
    vaults_->setFixedHeight(visibleRows * 44 + 2);
}

void SyncPage::rebuildConflicts() {
    QStringList names;
    for (const auto& value : engine_->items()) {
        const QJsonObject item = value.toObject();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (name.contains(QStringLiteral("(conflict copy)"))) {
            names << name;
        }
    }
    const QString joined = names.join(QLatin1Char('\n'));
    if (joined == conflictSignature_ && conflicts_->count() == names.size()) {
        conflictBox_->setVisible(!names.isEmpty());
        return;
    }
    conflictSignature_ = joined;

    building_ = true;
    conflicts_->clear();
    for (const auto& value : engine_->items()) {
        const QJsonObject item = value.toObject();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (!name.contains(QStringLiteral("(conflict copy)"))) {
            continue;
        }
        auto* entry = new QListWidgetItem(conflicts_);
        entry->setText(QString());
        entry->setSizeHint(QSize(0, 41));
        entry->setToolTip(item.value(QStringLiteral("path")).toString());

        const RowParts parts = makeRow(conflicts_, 41, QStringLiteral("syncConflictRow"));
        auto* nameLabel = new QLabel(elided(name, rowFont(), 320), parts.row);
        nameLabel->setFont(rowFont());
        nameLabel->setStyleSheet(QStringLiteral(
            "color: palette(text); background: transparent;"));
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto* meta = quietLabel(parts.row);
        meta->setFont(metaFont());
        meta->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                .arg(QString::fromLatin1(kSecondary)));
        meta->setText(QStringLiteral("Both versions kept"));
        meta->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        parts.body->addWidget(nameLabel, 1);
        parts.body->addWidget(meta);
        conflicts_->setItemWidget(entry, parts.row);
    }
    building_ = false;

    conflictBox_->setVisible(!names.isEmpty());
    const int visibleRows = std::min(int(names.size()), 3);
    conflicts_->setFixedHeight(visibleRows * 40 + 2);
}

QWidget* SyncPage::debrisRow(const QJsonObject& item) {
    const QString record = item.value(QStringLiteral("record")).toString();
    const QString name = item.value(QStringLiteral("name")).toString();
    const bool confirming = !record.isEmpty() && record == confirmingRecord_;

    const RowParts parts = makeRow(debris_, 45, QStringLiteral("syncDebrisRow"));
    auto* nameLabel = new QLabel(elided(name, rowFont(), 220), parts.row);
    nameLabel->setFont(rowFont());
    nameLabel->setStyleSheet(QStringLiteral(
        "color: palette(text); background: transparent;"));
    nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* meta = quietLabel(parts.row);
    meta->setFont(metaFont());
    meta->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                            .arg(QString::fromLatin1(kSecondary)));
    // The section heading already says where these came from, so the row only
    // speaks up when it is about to delete something.
    meta->setText(confirming ? QStringLiteral("Delete from the server and this device?")
                             : QString());
    meta->setVisible(confirming);
    meta->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    parts.body->addWidget(nameLabel, 1);
    parts.body->addWidget(meta);

    const QString actionStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none; color: palette(link);"
        " padding: 0 8px; min-height: 28px; font: 600 12px \"Segoe UI\"; }"
        "QPushButton:hover { color: white; }");
    const QString dangerStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none; color: #ff6b6b;"
        " padding: 0 8px; min-height: 28px; font: 600 12px \"Segoe UI\"; }"
        "QPushButton:hover { color: #ff8f85; }");

    if (confirming) {
        auto* cancel = new QPushButton(QStringLiteral("Cancel"), parts.row);
        cancel->setObjectName(QStringLiteral("syncDebrisCancel"));
        cancel->setCursor(Qt::PointingHandCursor);
        cancel->setFocusPolicy(Qt::NoFocus);
        cancel->setStyleSheet(actionStyle);
        auto* remove = new QPushButton(QStringLiteral("Delete"), parts.row);
        remove->setObjectName(QStringLiteral("syncDebrisConfirm"));
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setStyleSheet(dangerStyle);
        parts.body->addWidget(cancel);
        parts.body->addWidget(remove);
        connect(cancel, &QPushButton::clicked, this, [this] {
            confirmingRecord_.clear();
            rebuildDebris();
        });
        connect(remove, &QPushButton::clicked, this, [this, record] {
            confirmingRecord_.clear();
            engine_->purge(record);
            rebuildDebris();
        });
    } else {
        auto* restore = new QPushButton(QStringLiteral("Restore"), parts.row);
        restore->setObjectName(QStringLiteral("syncRestore"));
        restore->setCursor(Qt::PointingHandCursor);
        restore->setFocusPolicy(Qt::NoFocus);
        restore->setStyleSheet(actionStyle);
        auto* purge = new QPushButton(QStringLiteral("Delete permanently"), parts.row);
        purge->setObjectName(QStringLiteral("syncPurge"));
        purge->setCursor(Qt::PointingHandCursor);
        purge->setFocusPolicy(Qt::NoFocus);
        purge->setStyleSheet(dangerStyle);
        parts.body->addWidget(restore);
        parts.body->addWidget(purge);
        connect(restore, &QPushButton::clicked, this, [this, record] {
            confirmingRecord_.clear();
            engine_->restore(record);
        });
        connect(purge, &QPushButton::clicked, this, [this, record] {
            confirmingRecord_ = record;
            rebuildDebris();
        });
    }
    return parts.row;
}

void SyncPage::rebuildDebris() {
    QStringList records;
    for (const auto& value : engine_->items()) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("state")).toString() == QLatin1String("removed")) {
            records << item.value(QStringLiteral("record")).toString();
        }
    }
    const QString joined = records.join(QLatin1Char('\n')) + QLatin1Char('|') + confirmingRecord_;
    if (joined == debrisSignature_ && debris_->count() == records.size()) {
        debrisBox_->setVisible(!records.isEmpty());
        return;
    }
    debrisSignature_ = joined;

    building_ = true;
    debris_->clear();
    for (const auto& value : engine_->items()) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("state")).toString() != QLatin1String("removed")) {
            continue;
        }
        auto* entry = new QListWidgetItem(debris_);
        entry->setText(QString());
        entry->setSizeHint(QSize(0, 45));
        entry->setData(Qt::UserRole, item.value(QStringLiteral("record")).toString());
        entry->setToolTip(item.value(QStringLiteral("path")).toString());
        debris_->setItemWidget(entry, debrisRow(item));
    }
    building_ = false;

    if (records.isEmpty()) {
        confirmingRecord_.clear();
    }
    debrisBox_->setVisible(!records.isEmpty());
    const int visibleRows = std::min(int(records.size()), 3);
    debris_->setFixedHeight(visibleRows * 45 + 2);
}

} // namespace videovault::app
