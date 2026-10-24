#include "videovault/app/settings_dialog.hpp"

#include "videovault/app/glass.hpp"

#include "videovault/app/secure_input.hpp"
#include "videovault/app/in_window_sheet.hpp"
#include "videovault/app/plugin_host.hpp"
#include "videovault/app/proxy_settings.hpp"
#include "videovault/app/record_document.hpp"
#include <QDialog>
#include <QIntValidator>
#include <QResizeEvent>
#include <QUuid>
#include "videovault/app/sync_state.hpp"
#include "videovault/core/vault.hpp"
#include "videovault/core/version.hpp"
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
#include <QRadioButton>
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
SettingsDialog::SettingsDialog(const std::shared_ptr<core::Vault>& vault, QWidget* parent, PluginHost* pluginHost)
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
        "QLineEdit, QComboBox { background: palette(base); color: palette(text); border: 1px solid palette(midlight); border-radius: 8px; padding: 6px 10px; min-height: 30px; }"
        "QComboBox { padding-right: 30px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: palette(highlight); }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 26px; border: none; background: transparent; }"
        "QComboBox::down-arrow { image: url(:/icons/chevron_down.png); width: 12px; height: 12px; }"
        "QComboBox QAbstractItemView { background: palette(base); color: palette(text); selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }"
        "QComboBox QAbstractItemView::item { min-height: 30px; padding: 2px 8px; }"
        "QListWidget { background: palette(base); color: palette(text); border: none; }"
        "QListWidget::item { min-height: 46px; border-bottom: 1px solid palette(midlight); }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QListWidget#settingsNavigation { background: transparent; border: none; outline: none; padding: 2px; }"
        "QListWidget#settingsNavigation::item { min-height: 32px; padding: 0 10px; border-radius: 8px; border-bottom: none; color: palette(window-text); }"
        "QListWidget#settingsNavigation::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QListWidget#settingsNavigation::item:hover:!selected { background: palette(alternate-base); }"
        "QListWidget#settingsNavigation::item:disabled { color: palette(mid); }"
        "QTabBar::tab { color: palette(window-text); background: transparent; padding: 11px 9px; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: palette(highlight); border-bottom: 2px solid palette(highlight); }"
        "QTabBar::tab:hover { background: palette(alternate-base); }"
        "QPushButton[themeSegment=\"true\"] { background: palette(alternate-base); color: palette(text); border: 1px solid palette(midlight); border-radius: 9px; min-height: 48px; }"
        "QPushButton[themeSegment=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
        "QScrollArea#settingsSection { border: none; background: transparent; }"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 16, 20, 12);
    outer->setSpacing(12);
    // The header is its own widget so the frosted band can sit exactly behind
    // it: the same glass material as the library header.
    auto* headerHost = new QWidget(this);
    headerHost->setObjectName(QStringLiteral("settingsHeader"));
    headerHost->setAttribute(Qt::WA_StyledBackground, false);
    headerHost->setAutoFillBackground(false);
    auto* header = new QHBoxLayout(headerHost);
    header->setContentsMargins(0, 0, 0, 0);
    auto* back = action(QStringLiteral("‹ Back"), QStringLiteral("settingsBack"), this);
    connect(back, &QPushButton::clicked, this, &SettingsDialog::reject);
    header->addWidget(back);
    auto* title = new QLabel(QStringLiteral("Settings"), headerHost);
    QFont titleFont = title->font(); titleFont.setPointSize(21); titleFont.setBold(true); title->setFont(titleFont);
    header->addWidget(title); header->addStretch(); outer->addWidget(headerHost);
    auto* body = new QHBoxLayout;
    body->setSpacing(16);
    auto* nav = new QListWidget(this);
    nav->setObjectName(QStringLiteral("settingsNavigation"));
    nav->setAccessibleName(QStringLiteral("Settings sections"));
    nav->setFrameShape(QFrame::NoFrame);
    nav->setMinimumWidth(148);
    nav->setMaximumWidth(210);
    nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav->setSelectionMode(QAbstractItemView::SingleSelection);
    nav->setFocusPolicy(Qt::StrongFocus);
    body->addWidget(nav);
    auto* pages = new QStackedWidget(this);
    pages->setObjectName(QStringLiteral("settingsSections"));
    body->addWidget(pages, 1);
    // Compact mode: below ~620px the sidebar becomes a switcher above the page.
    // A 210px column on a 480px window left the content narrower than its cards.
    auto* navCombo = new QComboBox(this);
    navCombo->setObjectName(QStringLiteral("settingsNavigationCompact"));
    navCombo->setAccessibleName(QStringLiteral("Settings section"));
    navCombo->hide();
    outer->addWidget(navCombo);
    outer->addLayout(body, 1);
    // Behind the header: the frosted band, sampling whichever section scroll is
    // currently shown. Lowered so the Back button and title stay on top.
    settingsHeader_ = videovault::app::attachHeaderGlass(this, nullptr, 160);
    settingsHeader_->lower();
    headerHost->raise();
    connect(pages, &QStackedWidget::currentChanged, this, [this](int) {
        if (settingsHeader_ == nullptr || navigationPages_ == nullptr) {
            return;
        }
        settingsHeader_->follow(
            qobject_cast<QAbstractScrollArea*>(navigationPages_->currentWidget()));
        settingsHeader_->update();
    });
    if (auto* first = qobject_cast<QAbstractScrollArea*>(pages->currentWidget())) {
        settingsHeader_->follow(first);
    }
    // Each section is built in whatever order its controls need; the sidebar is
    // assembled afterwards from this list, so the navigation order is explicit
    // instead of an accident of construction order.
    QHash<QString, int> pageForSection;
    const QStringList navigationOrder{
        QStringLiteral("appearance"),
        QStringLiteral("|library"),
        QStringLiteral("tags"),
        QStringLiteral("playback"),
        QStringLiteral("thumbnails"),
        QStringLiteral("templates"),
        QStringLiteral("extensions"),
        QStringLiteral("security"),
        QStringLiteral("network"),
        QStringLiteral("updates")};
    auto section = [nav, pages, &pageForSection](const QString& key, const QString& name,
                         const QString& description) {
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
        pageForSection.insert(key, pages->count() - 1);
        Q_UNUSED(nav);
        return layout;
    };
    auto* updates = section(QStringLiteral("updates"), QStringLiteral("About & updates"),
        QStringLiteral("Version, update channel, and checking for a new release."));
    {
        // Product details first: who made this build and where the source lives.
        auto* aboutCard = card(updates);
        auto* aboutTitle = textLabel(QStringLiteral("Mega Vault Protect"), this);
        QFont aboutFont = aboutTitle->font();
        aboutFont.setPointSize(aboutFont.pointSize() + 2);
        aboutFont.setBold(true);
        aboutTitle->setFont(aboutFont);
        aboutTitle->setObjectName(QStringLiteral("aboutProductName"));
        aboutCard->addWidget(aboutTitle);
        // The version CMake stamped into the core, unless main() set an explicit
        // application version (the updater does that when it knows better).
        const std::string_view stamped = videovault::core::version();
        const QString reported = QCoreApplication::applicationVersion();
        const QString version = reported.isEmpty()
            ? QString::fromLatin1(stamped.data(), static_cast<int>(stamped.size()))
            : reported;
        auto* aboutVersion = textLabel(
            QStringLiteral("Version %1").arg(version), this, true);
        aboutVersion->setObjectName(QStringLiteral("aboutVersion"));
        aboutVersion->setAccessibleName(QStringLiteral("Application version"));
        aboutCard->addWidget(aboutVersion);
        auto* aboutCompany = textLabel(
            QStringLiteral("Creator %1")
                .arg( QStringLiteral("CISCO_X")), this, true);
        aboutCompany->setObjectName(QStringLiteral("aboutCompany"));
        aboutCompany->setAccessibleName(QStringLiteral("Company and producer"));
        aboutCard->addWidget(aboutCompany);
        auto* aboutLink = new QLabel(this);
        aboutLink->setObjectName(QStringLiteral("aboutGitHub"));
        aboutLink->setTextFormat(Qt::RichText);
        aboutLink->setWordWrap(true);
        aboutLink->setOpenExternalLinks(true);
        aboutLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
        aboutLink->setText(QStringLiteral(
            "<a href=\"https://github.com/XCisCoX/Mega-Vault-Protector\">"
            "github.com/XCisCoX/Mega-Vault-Protector</a>"));
        aboutLink->setToolTip(QStringLiteral("Open the project on GitHub"));
        aboutLink->setAccessibleName(QStringLiteral("Project address on GitHub"));
        aboutCard->addWidget(aboutLink);
        aboutCard->addWidget(textLabel(
            QStringLiteral("Encrypted local-first vault. Licensed under AGPL-3.0; bundled "
                           "Telegram player assets keep their GPL-3.0-or-later notices."),
            this, true));
    }
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

    auto* appearance = section(QStringLiteral("appearance"), QStringLiteral("Appearance"),
        QStringLiteral("Choose a look for the whole app."));
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

    auto* plugins = section(QStringLiteral("extensions"), QStringLiteral("Extensions"),
        QStringLiteral("Local sandboxed tools and permission-controlled providers."));
    if (!pluginHost) pluginHost = new PluginHost({}, this);
    plugins->addWidget(pluginHost->manager(vault_, this));

    auto* templates = section(QStringLiteral("templates"), QStringLiteral("Record templates"),
        QStringLiteral("Structures to start a record from. They are kept here rather than in "
                       "the library, so they never look like files."));
    auto* templateCard = card(templates);
    templateEmpty_ = textLabel(
        QStringLiteral("No templates yet. In a record, use the bookmark-plus action on a "
                       "record or on one of its objects to save one."),
        this, true);
    templateEmpty_->setObjectName(QStringLiteral("templatesEmptyState"));
    templateCard->addWidget(templateEmpty_);
    auto* templateHost = new QWidget(this);
    templateRows_ = new QVBoxLayout(templateHost);
    templateRows_->setContentsMargins(0, 0, 0, 0);
    templateRows_->setSpacing(6);
    templateCard->addWidget(templateHost);

    auto* security = section(QStringLiteral("security"), QStringLiteral("Security"),
        QStringLiteral("Protect your vault and control when it locks."));
    auto* lockCard = card(security);
    auto* lockAfter = new QComboBox(this);
    lockAfter->setObjectName(QStringLiteral("autoLockMinutes"));
    lockAfter->setMinimumWidth(150);
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

    auto* library = section(QStringLiteral("tags"), QStringLiteral("Tags"),
        QStringLiteral("Organize the tags you put on library items."));
    auto* tags = card(library);
    tags->addWidget(textLabel(QStringLiteral("Tags"), this));
    tagList_ = new QListWidget(this); tagList_->setObjectName(QStringLiteral("tagList"));
    tagList_->setAccessibleName(QStringLiteral("Library tags"));
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
    auto* playback = section(QStringLiteral("playback"), QStringLiteral("Playback"),
        QStringLiteral("Buffering for smooth playback. A change applies the next time you open a video."));
    auto* playbackCard = card(playback);
    cacheCombo_ = new QComboBox(this); cacheCombo_->setObjectName(QStringLiteral("streamCacheMib"));
    for (const int mib : {64, 128, 256, 512}) cacheCombo_->addItem(QStringLiteral("%1 MiB").arg(mib), mib);
    int cacheIndex = cacheCombo_->findData(QSettings().value(QStringLiteral("player/streamCacheMib"), 128).toInt());
    cacheCombo_->setCurrentIndex(cacheIndex < 0 ? 1 : cacheIndex);
    row(playbackCard, QStringLiteral("Streaming cache"), QStringLiteral("Memory reserved while a video plays."), cacheCombo_);
    connect(cacheCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyCacheSelection(); });

    auto* thumbnails = section(QStringLiteral("thumbnails"), QStringLiteral("Thumbnails"),
        QStringLiteral("Maintain the preview images the library shows. Nothing here touches your files' contents."));
    auto* thumbnailCard = card(thumbnails);
    row(thumbnailCard, QStringLiteral("Missing previews"),
        QStringLiteral("Only items without a stored thumbnail are processed."), new QWidget(this));
    regenerateButton_ = action(QStringLiteral("Regenerate missing thumbnails"),
        QStringLiteral("regenerateMissing"), this);
    regenerateButton_->setToolTip(QStringLiteral("Regenerate missing thumbnails"));
    thumbnailCard->addWidget(regenerateButton_);
    thumbnailCard->addWidget(textLabel(
        QStringLiteral("Items that already have a preview are left untouched, so this is safe to run again."),
        this, true));
    connect(regenerateButton_, &QPushButton::clicked, this, &SettingsDialog::beginRegenerateMissing);
    auto* proxies = section(QStringLiteral("network"), QStringLiteral("Network"),
        QStringLiteral("Route internet requests through HTTP or SOCKS5. SOCKS5 resolves destination names on the proxy. No MTProto."));
    auto* proxyCard = card(proxies);
    QString proxyError;
    auto proxyState = std::make_shared<ProxySettings>(loadProxySettings(&proxyError));
    auto* useProxy = new PillSwitch(this); useProxy->setObjectName("useProxy");
    useProxy->setChecked(proxyState->enabled);
    row(proxyCard, QStringLiteral("Use proxy"), QStringLiteral("Applies to new sync, update, provider and audio requests."), useProxy);
    auto* proxyStatus = textLabel(proxyError, this, true); proxyStatus->setObjectName("proxyStatus");
    proxyCard->addWidget(proxyStatus);
    auto* proxyList = new QListWidget(this); proxyList->setObjectName("proxyList");
    proxyList->setAccessibleName("Saved proxies"); proxyList->setMinimumHeight(120); proxyList->setMaximumHeight(240);
    proxyList->setTextElideMode(Qt::ElideMiddle); proxyCard->addWidget(proxyList);
    auto* proxyActions = new QHBoxLayout;
    auto* addProxy = action("Add proxy", "addProxy", this);
    auto* editProxy = action("Edit", "editProxy", this);
    auto* deleteProxy = action("Delete", "deleteProxy", this);
    proxyActions->addWidget(addProxy); proxyActions->addWidget(editProxy); proxyActions->addWidget(deleteProxy);
    proxyCard->addLayout(proxyActions);
    auto refreshProxies = [proxyState, proxyList, useProxy, editProxy, deleteProxy] {
        QSignalBlocker blocker(proxyList); QSignalBlocker switchBlock(useProxy);
        proxyList->clear(); useProxy->setChecked(proxyState->enabled);
        for (const auto& entry : proxyState->entries) {
            auto* item = new QListWidgetItem(QStringLiteral("%1%2:%3 · %4").arg(entry.id == proxyState->activeId ? QStringLiteral("Active · ") : QString(), entry.server).arg(entry.port).arg(entry.socks5 ? "SOCKS5" : "HTTP"), proxyList);
            item->setData(Qt::UserRole, entry.id);
            item->setToolTip(entry.server + ':' + QString::number(entry.port));
            if (entry.id == proxyState->activeId) proxyList->setCurrentItem(item);
        }
        editProxy->setEnabled(proxyList->currentItem()); deleteProxy->setEnabled(proxyList->currentItem());
    };
    refreshProxies();
    connect(useProxy, &QCheckBox::toggled, this, [proxyState, proxyStatus, refreshProxies](bool enabled) {
        auto next = *proxyState; next.enabled = enabled; QString error;
        if (saveProxySettings(next, &error)) *proxyState = next;
        proxyStatus->setText(error); refreshProxies();
    });
    connect(proxyList, &QListWidget::currentItemChanged, this, [proxyState, proxyList, proxyStatus, refreshProxies](QListWidgetItem* item) {
        if (!item) return;
        auto next = *proxyState; next.activeId = item->data(Qt::UserRole).toString(); QString error;
        if (saveProxySettings(next, &error)) *proxyState = next;
        proxyStatus->setText(error); refreshProxies();
    });
    auto openProxyEditor = [this, proxyState, proxyList, proxyStatus, refreshProxies](bool editing) {
        ProxyEntry entry; entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (editing) {
            if (!proxyList->currentItem()) return;
            for (const auto& saved : proxyState->entries) if (saved.id == proxyList->currentItem()->data(Qt::UserRole).toString()) entry = saved;
        }
        // An in-window sheet, like rename and the confirmations: it dims and
        // blurs the window behind it instead of opening a separate OS dialog.
        InWindowSheet sheet(this);
        sheet.setTitle(editing ? QStringLiteral("Edit proxy") : QStringLiteral("Add proxy"));
        sheet.setMessage(QStringLiteral(
            "HTTP or SOCKS5. SOCKS5 resolves the destination on the proxy. "
            "Credentials are sealed by the operating system, never plaintext."));
        auto rowOf = [](QWidget* parent) {
            auto* host = new QWidget(parent);
            auto* column = new QVBoxLayout(host);
            column->setContentsMargins(0, 0, 0, 0);
            column->setSpacing(4);
            return column;
        };
        auto* protocolHost = new QWidget;
        auto* protocols = new QHBoxLayout(protocolHost);
        protocols->setContentsMargins(0, 0, 0, 0);
        protocols->setSpacing(14);
        auto* socks = new QRadioButton("SOCKS5", protocolHost);
        auto* http = new QRadioButton("HTTP", protocolHost);
        socks->setObjectName("proxySocks5");
        http->setObjectName("proxyHttp");
        socks->setChecked(entry.socks5);
        http->setChecked(!entry.socks5);
        protocols->addWidget(socks);
        protocols->addWidget(http);
        protocols->addStretch(1);
        sheet.addRow(protocolHost);
        auto field = [&sheet, rowOf](const QString& title, const char* name, const QString& value) {
            auto* column = rowOf(&sheet);
            auto* label = textLabel(title, column->parentWidget());
            auto* input = new QLineEdit(value, column->parentWidget());
            input->setObjectName(name);
            input->setAccessibleName(title);
            input->setCursorPosition(0);
            column->addWidget(label);
            column->addWidget(input);
            sheet.addRow(column->parentWidget());
            return input;
        };
        auto* server = field("Server", "proxyServer", entry.server);
        auto* port = field("Port", "proxyPort", QString::number(entry.port));
        port->setValidator(new QIntValidator(1, 65535, port));
        auto* username = field("Username (optional)", "proxyUsername", entry.username);
        auto* password = field("Password (optional)", "proxyPassword", entry.password);
        password->setEchoMode(QLineEdit::Password);
        auto* cancelButton = sheet.addButton(QStringLiteral("Cancel"), 0, false);
        cancelButton->setObjectName(QStringLiteral("cancelProxy"));
        auto* saveButton = sheet.addButton(QStringLiteral("Save"), 1, true, QStringLiteral("check"));
        saveButton->setObjectName(QStringLiteral("saveProxy"));
        for (;;) {
            if (sheet.run() != 1) {
                password->clear();
                return;
            }
            auto next = *proxyState;
            auto changed = entry;
            changed.server = server->text().trimmed();
            changed.port = port->text().toInt();
            changed.username = username->text();
            changed.password = password->text();
            changed.socks5 = socks->isChecked();
            bool found = false;
            for (auto& saved : next.entries) {
                if (saved.id == changed.id) { saved = changed; found = true; }
            }
            if (!found) next.entries.append(changed);
            next.activeId = changed.id;
            QString error;
            if (!saveProxySettings(next, &error)) {
                // Keep the sheet open with the reason instead of dropping the edit.
                sheet.setMessage(error, true);
                continue;
            }
            *proxyState = next;
            proxyStatus->clear();
            refreshProxies();
            password->clear();
            return;
        }
    };
    connect(addProxy, &QPushButton::clicked, this, [openProxyEditor] { openProxyEditor(false); });
    connect(editProxy, &QPushButton::clicked, this, [openProxyEditor] { openProxyEditor(true); });
    connect(deleteProxy, &QPushButton::clicked, this, [proxyState, proxyList, proxyStatus, refreshProxies] {
        if (!proxyList->currentItem()) return;
        const auto id = proxyList->currentItem()->data(Qt::UserRole).toString(); auto next = *proxyState;
        for (int i = next.entries.size() - 1; i >= 0; --i) if (next.entries[i].id == id) next.entries.removeAt(i);
        if (next.activeId == id) { next.enabled = false; next.activeId = next.entries.isEmpty() ? QString() : next.entries.first().id; }
        QString error; if (saveProxySettings(next, &error)) *proxyState = next;
        proxyStatus->setText(error); refreshProxies();
    });
    proxies->addWidget(textLabel(QStringLiteral("Credentials are kept in your operating system’s protected credential store, not plaintext preferences. HTTP proxies and SOCKS5 authentication are not encrypted on the proxy hop; use a trusted local or secure-network proxy. Existing transfers finish on their original connection."), this, true));
    status_ = textLabel(QString(), this); status_->setObjectName(QStringLiteral("settingsStatus")); status_->hide();
    outer->addWidget(status_);
    // Build the sidebar from the explicit order above. Group headers are
    // unselectable labels, so keyboard and pointer navigation both skip them.
    nav->clear();
    for (const QString& key : navigationOrder) {
        if (key == QStringLiteral("|library")) {
            auto* group = new QListWidgetItem(QStringLiteral("LIBRARY & PLAYBACK"), nav);
            group->setFlags(Qt::NoItemFlags);
            group->setData(Qt::UserRole, -1);
            QFont groupFont = group->font();
            groupFont.setPointSize(8);
            groupFont.setBold(true);
            group->setFont(groupFont);
            group->setForeground(palette().color(QPalette::PlaceholderText));
            continue;
        }
        const auto found = pageForSection.constFind(key);
        if (found == pageForSection.constEnd()) {
            continue;
        }
        const int page = found.value();
        auto* item = new QListWidgetItem(nav);
        // The visible label is the page's own heading, so the sidebar can never
        // drift from what the page calls itself.
        if (auto* scroll = qobject_cast<QScrollArea*>(pages->widget(page))) {
            if (auto* headingLabel = scroll->widget() == nullptr
                    ? nullptr : scroll->widget()->findChild<QLabel*>()) {
                item->setText(headingLabel->text());
            }
        }
        item->setData(Qt::UserRole, page);
        item->setData(Qt::AccessibleTextRole, item->text());
        navCombo->addItem(item->text(), page);
    }
    auto showPage = [nav, pages](const QListWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        const int page = item->data(Qt::UserRole).toInt();
        if (page >= 0 && page < pages->count()) {
            pages->setCurrentIndex(page);
        }
    };
    connect(nav, &QListWidget::currentItemChanged, pages,
        [showPage, navCombo](QListWidgetItem* current, QListWidgetItem*) {
            showPage(current);
            if (current == nullptr || navCombo->isHidden()) {
                return;
            }
            const QSignalBlocker block(navCombo);
            navCombo->setCurrentIndex(navCombo->findData(current->data(Qt::UserRole).toInt()));
        });
    connect(navCombo, qOverload<int>(&QComboBox::currentIndexChanged), pages,
        [navCombo, nav, pages](const int index) {
            if (index < 0) {
                return;
            }
            const int page = navCombo->itemData(index).toInt();
            pages->setCurrentIndex(page);
            const QSignalBlocker block(nav);
            for (int row = 0; row < nav->count(); ++row) {
                if (nav->item(row)->data(Qt::UserRole).toInt() == page) {
                    nav->setCurrentRow(row);
                    break;
                }
            }
        });
    navigationList_ = nav;
    navigationCombo_ = navCombo;
    navigationPages_ = pages;
    // Appearance first: the old strip opened on Updates, which put a
    // rarely-used page in front of everything people actually change.
    nav->setCurrentRow(0);
    pages->setCurrentIndex(pageForSection.value(QStringLiteral("appearance"), 0));
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
void SettingsDialog::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (settingsHeader_ != nullptr) {
        const QWidget* host = findChild<QWidget*>(QStringLiteral("settingsHeader"));
        const int height = host != nullptr ? host->geometry().bottom() + 1 : 56;
        settingsHeader_->setGeometry(0, 0, width(), std::max(1, height));
        settingsHeader_->update();
    }
    if (navigationList_ == nullptr || navigationCombo_ == nullptr
        || navigationPages_ == nullptr) {
        return;
    }
    // One navigation control at a time: the sidebar needs ~210px, which a narrow
    // window does not have, and hiding it must not lose the current section.
    const bool compact = width() < 620;
    // Already in the matching mode: sidebar visible and wide, or hidden and narrow.
    if (navigationList_->isHidden() == compact) {
        return;
    }
    navigationList_->setVisible(!compact);
    navigationCombo_->setVisible(compact);
    if (compact) {
        const auto* current = navigationList_->currentItem();
        const QSignalBlocker block(navigationCombo_);
        navigationCombo_->setCurrentIndex(current == nullptr ? 0
            : navigationCombo_->findData(current->data(Qt::UserRole).toInt()));
    }
}

void SettingsDialog::showEvent(QShowEvent* event) {
    // The page outlives sessions of use, and a template can be saved from the record editor
    // while it is hidden, so the list is rebuilt every time it comes back.
    reloadTemplates();
    QWidget::showEvent(event);
}

void SettingsDialog::reloadTemplates() {
    if (templateRows_ == nullptr || vault_ == nullptr) {
        return;
    }
    while (auto* entry = templateRows_->takeAt(0)) {
        if (auto* widget = entry->widget()) {
            widget->deleteLater();
        }
        delete entry;
    }

    // The store folder, then its items. Nothing else counts as a template.
    std::int64_t folderId = 0;
    if (const auto folders = vault_->list_folders(); folders) {
        for (const auto& folder : folders.value()) {
            if (isTemplateFolderName(QString::fromStdString(folder.name))) {
                folderId = folder.id;
                break;
            }
        }
    }
    std::vector<core::VideoInfo> items;
    if (folderId != 0) {
        if (const auto listed = vault_->list_videos(); listed) {
            for (const auto& item : listed.value()) {
                if (item.folder_id == folderId) {
                    items.push_back(item);
                }
            }
        }
    }
    templateEmpty_->setVisible(items.empty());
    for (const auto& item : items) {
        auto* row = new QWidget(this);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        const QString name = QString::fromStdString(item.display_name);
        auto* label = new QLabel(name, row);
        label->setWordWrap(true);
        layout->addWidget(label, 1);
        auto* edit = action(QStringLiteral("Edit"), QStringLiteral("editTemplate"), row);
        auto* remove = action(QStringLiteral("Delete"), QStringLiteral("deleteTemplate"), row);
        layout->addWidget(edit);
        layout->addWidget(remove);
        const auto id = item.id;
        connect(edit, &QPushButton::clicked, this,
            [this, id, name] { emit openTemplateRequested(qlonglong(id), name); });
        connect(remove, &QPushButton::clicked, this,
            [this, id, name] { removeTemplate(id, name); });
        templateRows_->addWidget(row);
    }
}

void SettingsDialog::removeTemplate(const qint64 templateId, const QString& name) {
    if (vault_ == nullptr) {
        return;
    }
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("Delete template"));
    sheet.setMessage(QStringLiteral("\"%1\" is removed. Records already made from it keep "
                                    "their fields.")
                         .arg(name));
    sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    sheet.addButton(QStringLiteral("Delete"), 1, true, QStringLiteral("trash"));
    if (sheet.run() != 1) {
        return;
    }
    if (const auto removed = vault_->remove_video(templateId); !removed) {
        setStatus(QStringLiteral("The template could not be deleted."), true);
        return;
    }
    setStatus(QStringLiteral("\"%1\" deleted.").arg(name), false);
    reloadTemplates();
    emit settingsChanged();
}

void SettingsDialog::reloadTags() {
    const QSignalBlocker block(tagList_);
    tagList_->clear(); renameTagButton_->setEnabled(false); removeTagButton_->setEnabled(false);
    if (!vault_ || !vault_->is_unlocked()) { tagList_->setFixedHeight(52); return; }
    const auto tags = vault_->list_tags();
    if (!tags) { setStatus(QStringLiteral("Could not list tags."), true); return; }
    for (const auto& tag : tags.value()) {
        auto* item = new QListWidgetItem(QString::fromStdString(tag.name), tagList_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(tag.id));
        item->setData(Qt::UserRole + 1, item->text());
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setToolTip(QStringLiteral("%1 item(s). Double-click to rename.").arg(tag.video_count));
    }
    // A tag list is as tall as its tags: two tags used to sit under ~150px of
    // empty panel, which read as a broken table rather than a settings list.
    tagList_->setFixedHeight(std::min(240, std::max(52, tagList_->count() * 48 + 4)));
}
void SettingsDialog::renameSelectedTag() {
    if (auto* item = tagList_->currentItem()) tagList_->editItem(item);
}

void SettingsDialog::beginPasswordChange() {
    if (!vault_ || !vault_->is_unlocked() || passwordWatcher_ != nullptr) {
        return;
    }
    setStatus({});
    if (currentPassword_->text().isEmpty() || newPassword_->text().isEmpty()) {
        setStatus(QStringLiteral("Enter the current and a new password."), true);
        return;
    }
    if (newPassword_->text() != confirmPassword_->text()) {
        setStatus(QStringLiteral("The new password confirmation does not match."), true);
        return;
    }
    // Password bytes go straight into locked, self-wiping buffers and both
    // fields are emptied before the re-key starts.
    auto current = std::make_shared<core::Secret>();
    auto next = std::make_shared<core::Secret>();
    auto capturedCurrent = takeSecretFromLineEdit(currentPassword_);
    auto capturedNext = takeSecretFromLineEdit(newPassword_);
    confirmPassword_->clear();
    if (!capturedCurrent || !capturedNext) {
        setStatus(QStringLiteral("A password could not be held in protected memory."), true);
        return;
    }
    *current = std::move(*capturedCurrent);
    *next = std::move(*capturedNext);
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
        if (outcome->has_value()) {
            // The vault database is re-keyed; the package password this vault
            // shares must not be, or every package already uploaded would
            // become unreadable. Re-seal the sync state under the new password
            // while both passwords are still at hand.
            const auto root = vault->root_path();
            // u8string() is char8_t under C++20 on GCC/clang and char on MSVC,
            // so reinterpret through data()/size() rather than .c_str(): the
            // latter compiles only where the element type happens to be char.
            const auto utf8 = root.u8string();
            const QString folder = QString::fromUtf8(
                reinterpret_cast<const char*>(utf8.data()), int(utf8.size()));
            (void)videovault::app::sync::resealForPasswordChange(
                videovault::app::sync::stateDirectoryFor(folder), *current, *next);
        }
        current->wipe();
        next->wipe();
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
    // Sync keys came from the old password: the host locks sync until the vault
    // is unlocked again, so a stale engine can never re-seal the state wrongly.
    emit passwordChanged();
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
