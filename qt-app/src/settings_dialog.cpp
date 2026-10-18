#include "videovault/app/settings_dialog.hpp"
#include "videovault/core/vault.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEasingCurve>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QTabBar>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>

namespace videovault::app {
namespace {
// Retain QCheckBox keyboard/accessibility semantics while drawing only a pill.
class PillSwitch final : public QCheckBox {
public:
    explicit PillSwitch(QWidget* parent) : QCheckBox(parent) {
        setFixedSize(52, 34);
        setCursor(Qt::PointingHandCursor);
        animation_.setDuration(160);
        animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            position_ = value.toReal(); update();
        });
        connect(this, &QCheckBox::toggled, this, [this](bool checked) {
            animation_.stop();
            if (!isVisible()) { position_ = checked ? 1.0 : 0.0; update(); return; }
            animation_.setStartValue(position_);
            animation_.setEndValue(checked ? 1.0 : 0.0);
            animation_.start();
        });
    }
protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(isEnabled() ? 1.0 : 0.45);
        const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? palette().color(group, QPalette::Highlight)
                                    : palette().color(group, QPalette::Mid));
        painter.drawRoundedRect(QRectF(3, 5, 46, 24), 12, 12);
        painter.setBrush(palette().color(group, QPalette::Light));
        painter.drawEllipse(QRectF(6 + 22 * position_, 8, 18, 18));
        if (hasFocus()) {
            QStyleOptionFocusRect focus;
            focus.initFrom(this); focus.rect = rect().adjusted(0, 1, -1, -2);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }
private:
    QVariantAnimation animation_;
    qreal position_{0};
};
QLabel* textLabel(const QString& text, QWidget* parent, bool secondary = false) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (secondary) {
        label->setProperty("secondary", true);
        label->setForegroundRole(QPalette::PlaceholderText);
    }
    return label;
}
QVBoxLayout* card(QVBoxLayout* section) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("settingsCard"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(0);
    section->addWidget(frame);
    return layout;
}
void row(QVBoxLayout* group, const QString& title, const QString& detail, QWidget* control) {
    auto* widget = new QWidget;
    widget->setMinimumHeight(56);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 7, 0, 7);
    layout->setSpacing(14);
    auto* copy = new QVBoxLayout;
    copy->setSpacing(3);
    auto* label = textLabel(title, widget);
    label->setBuddy(control);
    copy->addWidget(label);
    if (!detail.isEmpty()) copy->addWidget(textLabel(detail, widget, true));
    layout->addLayout(copy, 1);
    layout->addWidget(control, 0, Qt::AlignVCenter);
    control->setAccessibleName(title);
    group->addWidget(widget);
}
QPushButton* action(const QString& title, const QString& name, QWidget* parent) {
    auto* button = new QPushButton(title, parent);
    button->setObjectName(name);
    button->setProperty("settingsAction", true);
    button->setMinimumHeight(44);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}
}
SettingsDialog::SettingsDialog(const std::shared_ptr<core::Vault>& vault, QWidget* parent)
    : QWidget(parent), vault_(vault) {
    setObjectName(QStringLiteral("settingsPage"));
    setWindowTitle(QStringLiteral("Settings"));
    setAttribute(Qt::WA_StyledBackground, true);
    // Keep local styling palette-based; refresh Qt 5's cached brushes on palette changes.
    setStyleSheet(QStringLiteral(
        "QWidget#settingsPage { background: palette(window); color: palette(window-text); }"
        "QFrame#settingsCard { background: palette(base); border: none; border-radius: 16px; }"
        "QLabel { background: transparent; }"
        "QLabel[secondary=\"true\"] { font-size: 12px; }"
        "QPushButton[settingsAction=\"true\"] { color: palette(highlight); background: transparent; border: none; border-radius: 8px; padding: 0 12px; font-weight: 600; }"
        "QPushButton[settingsAction=\"true\"]:hover { background: palette(alternate-base); }"
        "QPushButton[settingsAction=\"true\"]:focus { border: 1px solid palette(highlight); }"
        "QPushButton[settingsAction=\"true\"]:disabled { color: palette(mid); }"
        "QLineEdit, QComboBox { background: palette(base); color: palette(text); border: 1px solid palette(button); border-radius: 8px; padding: 6px 10px; min-height: 30px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: palette(highlight); }"
        "QComboBox QAbstractItemView { background: palette(base); color: palette(text); selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }"
        "QListWidget { background: palette(base); color: palette(text); border: none; }"
        "QListWidget::item { min-height: 46px; border-bottom: 1px solid palette(midlight); }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QTabBar::tab { color: palette(window-text); background: transparent; padding: 11px 9px; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: palette(highlight); border-bottom: 2px solid palette(highlight); }"
        "QTabBar::tab:hover { background: palette(alternate-base); }"
        "QPushButton[themeSegment=\"true\"] { background: palette(alternate-base); color: palette(text); border: 1px solid palette(midlight); border-radius: 9px; min-height: 48px; }"
        "QPushButton[themeSegment=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
        "QScrollArea#settingsSection { border: none; background: transparent; }"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 16, 20, 12);
    outer->setSpacing(12);
    auto* header = new QHBoxLayout;
    auto* back = action(QStringLiteral("‹ Back"), QStringLiteral("settingsBack"), this);
    connect(back, &QPushButton::clicked, this, &SettingsDialog::reject);
    header->addWidget(back);
    auto* title = new QLabel(QStringLiteral("Settings"), this);
    QFont titleFont = title->font(); titleFont.setPointSize(21); titleFont.setBold(true); title->setFont(titleFont);
    header->addWidget(title); header->addStretch(); outer->addLayout(header);
    auto* tabs = new QTabBar(this);
    tabs->setObjectName(QStringLiteral("settingsNavigation"));
    tabs->setExpanding(false); tabs->setDrawBase(false); tabs->setUsesScrollButtons(true);
    tabs->setAccessibleName(QStringLiteral("Settings sections"));
    outer->addWidget(tabs);
    auto* pages = new QStackedWidget(this);
    pages->setObjectName(QStringLiteral("settingsSections"));
    outer->addWidget(pages, 1);
    auto section = [tabs, pages](const QString& name, const QString& description) {
        tabs->addTab(name);
        auto* scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("settingsSection"));
        scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* content = new QWidget;
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 6, 4, 16); layout->setSpacing(14);
        layout->setAlignment(Qt::AlignTop);
        auto* heading = new QLabel(name, content);
        auto font = heading->font(); font.setPointSize(15); font.setBold(true); heading->setFont(font);
        layout->addWidget(heading);
        layout->addWidget(textLabel(description, content, true));
        scroll->setWidget(content); pages->addWidget(scroll);
        return layout;
    };
    auto* updates = section(QStringLiteral("Updates"), QStringLiteral("Keep Mega Vault up to date, on your terms."));
    auto* updateCard = card(updates);
    auto toggle = [this, updateCard](const QString& title, const QString& detail, const QString& key, bool fallback, const QString& name) {
        auto* button = new PillSwitch(this);
        button->setObjectName(name);
        button->setChecked(QSettings().value(key, fallback).toBool());
        row(updateCard, title, detail, button);
        connect(button, &QCheckBox::toggled, this, [this, key](bool checked) {
            QSettings().setValue(key, checked); emit updatePreferencesChanged();
        });
    };
    toggle(QStringLiteral("Check for updates automatically"), QString(), QStringLiteral("updates/checkAutomatically"), true, QStringLiteral("checkAutomatically"));
    toggle(QStringLiteral("Download updates automatically"), QString(), QStringLiteral("updates/downloadAutomatically"), false, QStringLiteral("downloadAutomatically"));
    toggle(QStringLiteral("Include beta releases"), QStringLiteral("Try previews; they may be less stable."), QStringLiteral("updates/includeBeta"), false, QStringLiteral("includeBeta"));
    updates->addWidget(textLabel(QStringLiteral("Automatic downloads never close or restart the app. You confirm when to install an update."), this, true));
    auto* manual = card(updates);
    checkUpdatesButton_ = action(QStringLiteral("Check now"), QStringLiteral("checkUpdatesNow"), this);
    row(manual, QStringLiteral("Check for a new version"), QStringLiteral("You can check manually at any time."), checkUpdatesButton_);
    updateStatus_ = textLabel(QStringLiteral("Ready to check for updates."), this, true);
    updateStatus_->setObjectName(QStringLiteral("updateStatus"));
    updateStatus_->setAccessibleName(QStringLiteral("Update status"));
    manual->addWidget(updateStatus_);
    connect(checkUpdatesButton_, &QPushButton::clicked, this, &SettingsDialog::updateCheckRequested);

    auto* appearance = section(QStringLiteral("Appearance"), QStringLiteral("Choose a look for the whole app."));
    auto* appearanceCard = card(appearance);
    auto* segments = new QHBoxLayout;
    segments->setSpacing(8);
    const QString savedTheme = QSettings().value(QStringLiteral("appearance/theme"), QStringLiteral("dark")).toString();
    const QStringList themes{QStringLiteral("system"), QStringLiteral("light"), QStringLiteral("dark")};
    const QStringList names{QStringLiteral("System"), QStringLiteral("Light"), QStringLiteral("Dark")};
    for (int i = 0; i < themes.size(); ++i) {
        auto* button = new QPushButton(names[i], this);
        button->setObjectName(QStringLiteral("theme_") + themes[i]);
        button->setProperty("themeSegment", true); button->setCheckable(true); button->setAutoExclusive(true);
        button->setChecked(savedTheme == themes[i] || (!themes.contains(savedTheme) && i == 2));
        button->setAccessibleName(names[i] + QStringLiteral(" appearance"));
        segments->addWidget(button, 1);
        connect(button, &QPushButton::clicked, this, [this, theme = themes[i]] {
            if (QSettings().value(QStringLiteral("appearance/theme"), QStringLiteral("dark")).toString() == theme) return;
            QSettings().setValue(QStringLiteral("appearance/theme"), theme); emit appearanceChanged();
        });
    }
    appearanceCard->addLayout(segments);
    appearance->addWidget(textLabel(QStringLiteral("System follows your device’s appearance. Your choice is saved for the next launch."), this, true));

    auto* plugins = section(QStringLiteral("Plugins"), QStringLiteral("A future home for external extensions."));
    auto* pluginCard = card(plugins);
    auto* empty = textLabel(QStringLiteral("External plugin loading is not supported yet"), this);
    empty->setObjectName(QStringLiteral("pluginsEmptyState")); pluginCard->addWidget(empty);
    pluginCard->addWidget(textLabel(QStringLiteral("There is no plugin loader or supported plugin API in this version. Files in this folder are not loaded or executed."), this, true));
    auto* openPlugins = action(QStringLiteral("Open plugins folder"), QStringLiteral("openPluginsFolder"), this);
    pluginCard->addWidget(openPlugins, 0, Qt::AlignLeft);
    connect(openPlugins, &QPushButton::clicked, this, [this] {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/plugins");
        if (!QDir().mkpath(path)) { setStatus(QStringLiteral("Could not create the plugins folder."), true); return; }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) setStatus(QStringLiteral("Could not open the plugins folder."), true);
    });

    auto* security = section(QStringLiteral("Security"), QStringLiteral("Protect your vault and control when it locks."));
    auto* lockCard = card(security);
    auto* lockAfter = new QComboBox(this);
    lockAfter->setObjectName(QStringLiteral("autoLockMinutes"));
    const int savedLock = QSettings().value(QStringLiteral("security/autoLockMinutes"), 5).toInt();
    const int minutes[]{0, 5, 15, 60, 240};
    const QStringList lockNames{QStringLiteral("Never"), QStringLiteral("5 minutes"), QStringLiteral("15 minutes"), QStringLiteral("1 hour"), QStringLiteral("4 hours")};
    for (int i = 0; i < 5; ++i) lockAfter->addItem(lockNames[i], minutes[i]);
    const int index = lockAfter->findData(savedLock); lockAfter->setCurrentIndex(index < 0 ? 1 : index);
    row(lockCard, QStringLiteral("Auto-lock"), QStringLiteral("After no clicks or key presses."), lockAfter);
    connect(lockAfter, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, lockAfter](int index) {
        if (index >= 0) { QSettings().setValue(QStringLiteral("security/autoLockMinutes"), lockAfter->itemData(index)); emit settingsChanged(); }
    });
    auto* passwordCard = card(security);
    passwordCard->addWidget(textLabel(QStringLiteral("Change vault password"), this));
    auto password = [this, passwordCard](const QString& placeholder, const QString& name) {
        auto* edit = new QLineEdit(this); edit->setObjectName(name);
        edit->setEchoMode(QLineEdit::Password); edit->setPlaceholderText(placeholder); edit->setAccessibleName(placeholder);
        passwordCard->addWidget(edit); return edit;
    };
    currentPassword_ = password(QStringLiteral("Current password"), QStringLiteral("currentPassword"));
    newPassword_ = password(QStringLiteral("New password"), QStringLiteral("newPassword"));
    confirmPassword_ = password(QStringLiteral("Confirm new password"), QStringLiteral("confirmPassword"));
    passwordCard->setSpacing(8);
    securityProfile_ = new QComboBox(this);
    securityProfile_->setObjectName(QStringLiteral("securityProfile"));
    securityProfile_->setAccessibleName(QStringLiteral("Argon2id security profile"));
    securityProfile_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    securityProfile_->setMinimumWidth(0);
    securityProfile_->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    securityProfile_->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    passwordCard->addWidget(securityProfile_);
    changePasswordButton_ = action(QStringLiteral("Change password"), QStringLiteral("changePassword"), this);
    passwordCard->addWidget(changePasswordButton_);
    connect(changePasswordButton_, &QPushButton::clicked, this, &SettingsDialog::beginPasswordChange);

    auto* library = section(QStringLiteral("Library"), QStringLiteral("Organize tags and maintain your media library."));
    auto* tags = card(library);
    tags->addWidget(textLabel(QStringLiteral("Tags"), this));
    tagList_ = new QListWidget(this); tagList_->setObjectName(QStringLiteral("tagList"));
    tagList_->setAccessibleName(QStringLiteral("Library tags")); tagList_->setMinimumHeight(150);
    tagList_->setMaximumHeight(240); tagList_->setSelectionMode(QAbstractItemView::SingleSelection);
    tagList_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tags->addWidget(tagList_);
    auto* tagActions = new QHBoxLayout;
    newTagEdit_ = new QLineEdit(this); newTagEdit_->setObjectName(QStringLiteral("newTag"));
    newTagEdit_->setPlaceholderText(QStringLiteral("New tag")); newTagEdit_->setAccessibleName(QStringLiteral("New tag"));
    tagActions->addWidget(newTagEdit_, 1);
    addTagButton_ = action(QStringLiteral("Add"), QStringLiteral("addTag"), this); tagActions->addWidget(addTagButton_); tags->addLayout(tagActions);
    auto* selectionActions = new QHBoxLayout;
    renameTagButton_ = action(QStringLiteral("Rename"), QStringLiteral("renameTag"), this);
    removeTagButton_ = action(QStringLiteral("Delete tag"), QStringLiteral("removeTag"), this);
    selectionActions->addWidget(renameTagButton_); selectionActions->addWidget(removeTagButton_); selectionActions->addStretch(); tags->addLayout(selectionActions);
    connect(newTagEdit_, &QLineEdit::returnPressed, this, &SettingsDialog::addTag);
    connect(addTagButton_, &QPushButton::clicked, this, &SettingsDialog::addTag);
    connect(renameTagButton_, &QPushButton::clicked, this, &SettingsDialog::renameSelectedTag);
    connect(removeTagButton_, &QPushButton::clicked, this, &SettingsDialog::removeSelectedTag);
    connect(tagList_, &QListWidget::itemSelectionChanged, this, [this] {
        const bool selected = tagList_->currentItem() != nullptr;
        renameTagButton_->setEnabled(selected); removeTagButton_->setEnabled(selected);
    });
    connect(tagList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!vault_ || !vault_->is_unlocked()) return;
        const auto outcome = vault_->rename_tag(item->data(Qt::UserRole).toLongLong(), item->text().trimmed().toStdString());
        if (!outcome) {
            setStatus(QString::fromUtf8(core::user_message(outcome.error().code).data()), true);
            const QSignalBlocker block(tagList_); item->setText(item->data(Qt::UserRole + 1).toString());
            return;
        }
        { const QSignalBlocker block(tagList_); item->setText(item->text().trimmed()); item->setData(Qt::UserRole + 1, item->text()); }
        emit settingsChanged();
    });
    auto* playback = card(library);
    cacheCombo_ = new QComboBox(this); cacheCombo_->setObjectName(QStringLiteral("streamCacheMib"));
    for (const int mib : {64, 128, 256, 512}) cacheCombo_->addItem(QStringLiteral("%1 MiB").arg(mib), mib);
    int cacheIndex = cacheCombo_->findData(QSettings().value(QStringLiteral("player/streamCacheMib"), 128).toInt());
    cacheCombo_->setCurrentIndex(cacheIndex < 0 ? 1 : cacheIndex);
    row(playback, QStringLiteral("Streaming cache"), QStringLiteral("Used when you next open a video."), cacheCombo_);
    connect(cacheCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyCacheSelection(); });
    regenerateButton_ = action(QStringLiteral("Regenerate missing"), QStringLiteral("regenerateMissing"), this);
    regenerateButton_->setToolTip(QStringLiteral("Regenerate missing thumbnails"));
    playback->addWidget(regenerateButton_);
    playback->addWidget(textLabel(QStringLiteral("Only items without a stored thumbnail are processed."), this, true));
    connect(regenerateButton_, &QPushButton::clicked, this, &SettingsDialog::beginRegenerateMissing);
    status_ = textLabel(QString(), this); status_->setObjectName(QStringLiteral("settingsStatus")); status_->hide();
    outer->addWidget(status_);
    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    const bool unlocked = vault_ && vault_->is_unlocked();
    changePasswordButton_->setEnabled(unlocked); regenerateButton_->setEnabled(unlocked); addTagButton_->setEnabled(unlocked);
    reloadTags();
    connect(qApp, &QApplication::paletteChanged, this, [this](const QPalette& palette) {
        // Qt 5 caches stylesheet palette() brushes; rebuild them for live theme changes.
        const QString rules = styleSheet();
        setStyleSheet(QString());
        setPalette(palette);
        setStyleSheet(rules);
        update();
    });
}
void SettingsDialog::reject() { emit closed(); }
void SettingsDialog::setUpdateStatus(const QString& message, bool busy) {
    updateStatus_->setText(message); checkUpdatesButton_->setEnabled(!busy);
}
QSize SettingsDialog::sizeHint() const { return QSize(1100, 760); }
QSize SettingsDialog::minimumSizeHint() const { return QSize(360, 320); }
void SettingsDialog::setStatus(const QString& message, bool error) {
    status_->setText(message); status_->setVisible(!message.isEmpty());
    status_->setAccessibleName(error ? QStringLiteral("Settings error") : QStringLiteral("Settings status"));
}
void SettingsDialog::reloadTags() {
    const QSignalBlocker block(tagList_);
    tagList_->clear(); renameTagButton_->setEnabled(false); removeTagButton_->setEnabled(false);
    if (!vault_ || !vault_->is_unlocked()) return;
    const auto tags = vault_->list_tags();
    if (!tags) { setStatus(QStringLiteral("Could not list tags."), true); return; }
    for (const auto& tag : tags.value()) {
        auto* item = new QListWidgetItem(QString::fromStdString(tag.name), tagList_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(tag.id));
        item->setData(Qt::UserRole + 1, item->text());
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setToolTip(QStringLiteral("%1 item(s). Double-click to rename.").arg(tag.video_count));
    }
}
void SettingsDialog::renameSelectedTag() {
    if (auto* item = tagList_->currentItem()) tagList_->editItem(item);
}

void SettingsDialog::beginPasswordChange() {
    if (!vault_ || !vault_->is_unlocked() || passwordWatcher_ != nullptr) {
        return;
    }
    setStatus({});
    const QString current_text = currentPassword_->text();
    const QString next_text = newPassword_->text();
    if (current_text.isEmpty() || next_text.isEmpty()) {
        setStatus(QStringLiteral("Enter the current and a new password."), true);
        return;
    }
    if (next_text != confirmPassword_->text()) {
        setStatus(QStringLiteral("The new password confirmation does not match."), true);
        return;
    }
    auto current = std::make_shared<std::string>(current_text.toUtf8().constData());
    auto next = std::make_shared<std::string>(next_text.toUtf8().constData());
    const auto parameters = securityProfile_->currentIndex() == 0
        ? core::Argon2Parameters{256U * 1024U, 3U, 1U}
        : core::Argon2Parameters{512U * 1024U, 4U, 1U};
    const auto vault = vault_;
    changePasswordButton_->setEnabled(false);
    setStatus(QStringLiteral("Changing password…"));
    passwordWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(passwordWatcher_, &QFutureWatcherBase::finished,
        this, [this] { finishPasswordChange(); });
    passwordWatcher_->setFuture(QtConcurrent::run([vault, current, next, parameters] {
        auto outcome = std::make_shared<core::Result<bool>>(
            vault->change_password(*current, *next, parameters));
        current->assign(current->size(), '\0');
        next->assign(next->size(), '\0');
        return outcome;
    }));
}

void SettingsDialog::finishPasswordChange() {
    auto* completed = passwordWatcher_;
    passwordWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    changePasswordButton_->setEnabled(true);
    if (!outcome) {
        setStatus(QString::fromUtf8(
            core::user_message(outcome.error().code).data()), true);
        return;
    }
    currentPassword_->clear();
    newPassword_->clear();
    confirmPassword_->clear();
    setStatus(QStringLiteral("Password changed."), false);
    emit settingsChanged();
}

void SettingsDialog::addTag() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const QString name = newTagEdit_->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    setStatus({});
    const auto result = vault_->create_tag(name.toStdString());
    if (!result) {
        setStatus(QString::fromUtf8(
            core::user_message(result.error().code).data()), true);
        return;
    }
    newTagEdit_->clear();
    reloadTags();
    emit settingsChanged();
}

void SettingsDialog::removeSelectedTag() {
    auto* item = tagList_->currentItem();
    if (item == nullptr || !vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto tag_id = static_cast<std::int64_t>(item->data(Qt::UserRole).toULongLong());
    const auto result = vault_->delete_tag(tag_id);
    if (!result) {
        setStatus(QString::fromUtf8(
            core::user_message(result.error().code).data()), true);
    }
    reloadTags();
    emit settingsChanged();
}

void SettingsDialog::beginRegenerateMissing() {
    if (!vault_ || !vault_->is_unlocked() || regenerateWatcher_ != nullptr) {
        return;
    }
    setStatus(QStringLiteral("Regenerating missing thumbnails…"), false);
    regenerateButton_->setEnabled(false);
    const auto vault = vault_;
    regenerateWatcher_ =
        new QFutureWatcher<std::shared_ptr<std::pair<int, int>>>(this);
    connect(regenerateWatcher_, &QFutureWatcherBase::finished,
        this, [this] { finishRegenerateMissing(); });
    regenerateWatcher_->setFuture(QtConcurrent::run([vault] {
        // first = how many had no thumbnail, second = how many were created.
        // first < 0 means the gallery could not be listed.
        auto result = std::make_shared<std::pair<int, int>>(0, 0);
        const auto videos = vault->list_videos();
        if (!videos) {
            result->first = -1;
            return result;
        }
        for (const auto& video : videos.value()) {
            const auto stored = vault->thumbnail(video.id);
            if (stored && !stored.value().bytes.empty()) {
                continue;
            }
            ++result->first;
            const auto generated = vault->generate_thumbnail(video.id, 320U);
            if (generated && !generated.value().bytes.empty()) {
                ++result->second;
            }
        }
        return result;
    }));
}

void SettingsDialog::finishRegenerateMissing() {
    auto* completed = regenerateWatcher_;
    regenerateWatcher_ = nullptr;
    const auto outcome = completed->result();
    completed->deleteLater();
    regenerateButton_->setEnabled(true);
    const int missing = outcome->first;
    const int regenerated = outcome->second;
    if (missing < 0) {
        setStatus(QStringLiteral("Could not list the vault."), true);
        return;
    }
    if (missing == 0) {
        setStatus(QStringLiteral("Every item already has a thumbnail."), false);
        return;
    }
    if (regenerated == missing) {
        setStatus(QStringLiteral("Regenerated %1 missing thumbnail(s).").arg(regenerated), false);
    } else {
        setStatus(QStringLiteral("Regenerated %1 of %2 missing thumbnail(s).")
            .arg(regenerated)
            .arg(missing),
            regenerated == 0);
    }
    emit settingsChanged();
}

void SettingsDialog::applyCacheSelection() {
    const int megabytes = cacheCombo_->currentData().toInt();
    if (megabytes > 0) {
        QSettings().setValue(QStringLiteral("player/streamCacheMib"), megabytes);
        emit settingsChanged();
    }
}

} // namespace videovault::app
