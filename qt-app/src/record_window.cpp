#include "videovault/app/record_window.hpp"

#include "videovault/app/icons.hpp"

#include "videovault/app/in_window_sheet.hpp"
#include "videovault/core/vault.hpp"

#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPalette>
#include <QScrollArea>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <span>
#include <string>

namespace videovault::app {
namespace {

// Row action buttons, and the fixed-height column that holds them.
constexpr int kRowActionSize = 26;

QString recordErrorText(const core::VaultError& error) {
    const QString message =
        QString::fromStdString(std::string(core::user_message(error.code)));
    return message.isEmpty() ? QString::fromStdString(error.technical_detail) : message;
}

std::span<const unsigned char> bytesOf(const QByteArray& bytes) {
    return std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(bytes.constData()),
        static_cast<std::size_t>(bytes.size()));
}

QByteArray toByteArray(const std::vector<unsigned char>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()));
}

QPushButton* quietButton(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setProperty("quiet", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// A button that only exists for its arrow: no padding, no minimum height, so a
// disclosure control does not make every row tall.
// Same affordances as quietButton(), but icon-only: the tooltip carries the label,
// and the accessible name keeps screen readers honest.
QPushButton* quietIconButton(const QString& icon, const QString& tip, QWidget* parent) {
    auto* button = new QPushButton(parent);
    button->setProperty("quiet", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setIconSize(QSize(16, 16));
    // The app sheet sets a min-height on every QPushButton and QSS wins over
    // setFixedSize, so the button's own sheet has to pin both dimensions.
    button->setStyleSheet(QStringLiteral(
        "QPushButton { padding: 0px; min-width: %1px; max-width: %1px;"
        " min-height: %1px; max-height: %1px; }")
            .arg(kRowActionSize));
    icons::apply(button, icon, icons::Tone::Quiet);
    return button;
}

QPushButton* disclosureButton(QWidget* parent, const QString& color) {
    auto* button = new QPushButton(parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    button->setFixedSize(20, 20);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; background: transparent; color: %1;"
        " padding: 0px; min-height: 0px; font-size: 13px; }"
        "QPushButton:hover { background: rgba(127, 127, 127, 45); border-radius: 6px; }")
            .arg(color));
    return button;
}

// Copied secrets leave the clipboard on their own after this delay.
constexpr int kClipboardClearMs = 30000;
constexpr int kIndentPx = 16;
constexpr int kToggleWidth = 22;
// The left column (indent + disclosure + key + type) is one width for every row,
// so values start on one x at every depth. The action column is measured from the
// font; together they leave the value exactly the space that remains.
constexpr int kTypeGap = 6;   // key -> type, deliberately tight
constexpr int kColumnGap = 8;
constexpr int kLeftColumnWidth = 260;
const QString kMask = QStringLiteral("********");

struct LoadOutcome {
    bool ok{false};
    QString error;
    QByteArray bytes;
};

// A label that reports double clicks, which is how a value or a key is edited
// without hunting for a button.
class ClickLabel final : public QLabel {
public:
    using QLabel::QLabel;
    std::function<void()> onDoubleClick;

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (onDoubleClick) {
            onDoubleClick();
            return;
        }
        QLabel::mouseDoubleClickEvent(event);
    }
};

// A label that elides its text to the width the layout actually gives it, so a
// value fills the space between the fixed columns instead of being cut at a width
// guessed when the row was built. Eliding again on resize keeps that true when the
// window changes size.
class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QLabel(parent) {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void setFullText(const QString& text) {
        full_ = text;
        updateElided();
    }

    std::function<void()> onDoubleClick;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        updateElided();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (onDoubleClick) {
            onDoubleClick();
            return;
        }
        QLabel::mouseDoubleClickEvent(event);
    }

private:
    void updateElided() {
        const QString elided = fontMetrics().elidedText(full_, Qt::ElideRight, qMax(0, width()));
        if (elided != text()) {
            QLabel::setText(elided);
        }
    }

    QString full_;
};

// A row whose buttons are invisible until the pointer is over the row. The buttons
// keep their space, so nothing shifts or re-elides as the pointer moves: only their
// colour changes.
class HoverRow final : public QFrame {
public:
    explicit HoverRow(QWidget* parent) : QFrame(parent) {}

    QVector<QPushButton*> actions;
    QString hoverStyle;

    // Also the starting state, so it must not depend on having been hovered.
    // The buttons are hidden rather than faded: an icon does not follow the text
    // colour, and an invisible button you can still click is a trap. The action
    // column keeps its width, so nothing shifts when they appear.
    void endHover() {
        hovered_ = false;
        setStyleSheet(QString());
        for (auto* button : actions) {
            button->setVisible(false);
        }
    }

protected:
    void enterEvent(QEvent* event) override {
        startHover();
        QFrame::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        // Moving onto a child of the row reports a leave on the frame; the pointer
        // is still inside the row, so keep it lit.
        if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
            endHover();
        }
        QFrame::leaveEvent(event);
    }

private:
    void startHover() {
        if (hovered_) {
            return;
        }
        hovered_ = true;
        if (!hoverStyle.isEmpty()) {
            setStyleSheet(hoverStyle);
        }
        for (auto* button : actions) {
            // Only visibility changes here: clearing the stylesheet would also drop the
            // size constraints that keep the button small.
            button->setVisible(true);
        }
    }

    bool hovered_{false};
};

// Does this subtree contain the query, in a key or in a leaf value? Secrets are
// searched by their real value; the row still renders masked.
bool nodeMatchesQuery(const RecordNode& node, const QString& key, const QString& query) {
    if (key.contains(query, Qt::CaseInsensitive)) {
        return true;
    }
    if (!node.isContainer() && node.text.contains(query, Qt::CaseInsensitive)) {
        return true;
    }
    if (node.type == ValueType::Object) {
        for (const auto& member : node.members) {
            if (nodeMatchesQuery(member.second, member.first, query)) {
                return true;
            }
        }
    } else if (node.type == ValueType::Array) {
        for (const auto& item : node.items) {
            if (nodeMatchesQuery(item, QString(), query)) {
                return true;
            }
        }
    }
    return false;
}

bool entryMatchesQuery(const RecordEntry& entry, const QString& query) {
    return entry.title.contains(query, Qt::CaseInsensitive)
        || nodeMatchesQuery(entry.body, QString(), query);
}

QString elide(const QString& text, const QFont& font, const int width) {
    return QFontMetrics(font).elidedText(text.simplified(), Qt::ElideRight, width);
}

} // namespace

// ---------------------------------------------------------------------------
// RecordWindow
// ---------------------------------------------------------------------------

RecordWindow::RecordWindow(QWidget* parent) : QWidget(parent) {
    clipboardTimer_ = new QTimer(this);
    clipboardTimer_->setSingleShot(true);
    connect(clipboardTimer_, &QTimer::timeout, this, [this] {
        if (clipboardValue_.isEmpty()) {
            return;
        }
        // Only clear if the clipboard still holds the copied secret; anything the
        // user copied since then is theirs.
        if (QGuiApplication::clipboard()->text() == clipboardValue_) {
            QGuiApplication::clipboard()->clear();
        }
        clipboardValue_.clear();
    });
    buildInterface();
    applyStyle();
}

void RecordWindow::buildInterface() {
    setObjectName(QStringLiteral("recordPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(12);

    auto* header = new QHBoxLayout();
    header->setSpacing(6);

    auto* titles = new QVBoxLayout();
    titles->setSpacing(1);
    fileName_ = new QLabel(QStringLiteral("Record"), this);
    fileName_->setObjectName(QStringLiteral("recordFileName"));
    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(QStringLiteral("recordCountLabel"));
    titles->addWidget(fileName_);
    titles->addWidget(countLabel_);
    header->addLayout(titles, 1);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("recordSearch"));
    search_->setPlaceholderText(QStringLiteral("Search"));
    search_->setClearButtonEnabled(true);
    search_->setFixedWidth(190);
    search_->setToolTip(QStringLiteral("Filter by record name, key or value"));
    header->addWidget(search_);

    auto* sortButton = quietIconButton(
        QStringLiteral("chevrons-up-down"), QStringLiteral("Sort records by title"), this);
    auto* sortMenu = new QMenu(sortButton);
    sortMenu->addAction(QStringLiteral("Sort by title, A to Z"), this,
        [this] { sortEntries(true); });
    sortMenu->addAction(QStringLiteral("Sort by title, Z to A"), this,
        [this] { sortEntries(false); });
    connect(sortButton, &QPushButton::clicked, this, [sortButton, sortMenu] {
        sortMenu->exec(sortButton->mapToGlobal(QPoint(0, sortButton->height())));
    });

    addRecordButton_ = quietIconButton(
        QStringLiteral("plus"), QStringLiteral("Add a record to this file"), this);
    revertButton_ = quietIconButton(
        QStringLiteral("undo-2"), QStringLiteral("Revert unsaved changes"), this);
    saveButton_ = new QPushButton(this);
    saveButton_->setProperty("primary", true);
    saveButton_->setIcon(icons::lucideSolid(QStringLiteral("check"), QColor(0xff, 0xff, 0xff)));
    saveButton_->setIconSize(QSize(16, 16));
    saveButton_->setFixedSize(30, 30);
    saveButton_->setStyleSheet(QStringLiteral(
        "QPushButton { padding: 0px; border-radius: 15px; min-width: 30px; max-width: 30px;"
        " min-height: 30px; max-height: 30px; }"));
    saveButton_->setCursor(Qt::PointingHandCursor);
    saveButton_->setFocusPolicy(Qt::TabFocus);
    saveButton_->setToolTip(QStringLiteral("Save changes"));
    saveButton_->setAccessibleName(QStringLiteral("Save changes"));
    auto* closeButton = quietIconButton(QStringLiteral("x"), QStringLiteral("Close"), this);
    header->addWidget(sortButton);
    header->addWidget(addRecordButton_);
    header->addWidget(revertButton_);
    header->addWidget(saveButton_);
    header->addWidget(closeButton);
    layout->addLayout(header);

    // Shown only for content written by an older version: the migration runs here,
    // in place, and keeps the item's id, folder and tags.
    legacyBar_ = new QFrame(this);
    legacyBar_->setObjectName(QStringLiteral("recordLegacyBar"));
    auto* legacyLayout = new QHBoxLayout(legacyBar_);
    legacyLayout->setContentsMargins(14, 10, 10, 10);
    legacyLayout->setSpacing(10);
    legacyLabel_ = new QLabel(legacyBar_);
    legacyLabel_->setObjectName(QStringLiteral("recordLegacyLabel"));
    legacyLabel_->setWordWrap(true);
    migrateButton_ = new QPushButton(QStringLiteral("Migrate"), legacyBar_);
    migrateButton_->setProperty("primary", true);
    migrateButton_->setIcon(icons::lucideSolid(QStringLiteral("refresh-cw"), QColor(0xff, 0xff, 0xff)));
    migrateButton_->setToolTip(QStringLiteral("Convert this record to the current format"));
    legacyLayout->addWidget(legacyLabel_, 1);
    legacyLayout->addWidget(migrateButton_);
    legacyBar_->hide();
    layout->addWidget(legacyBar_);

    errorHint_ = new QLabel(this);
    errorHint_->setObjectName(QStringLiteral("recordEmptyHint"));
    errorHint_->setWordWrap(true);
    errorHint_->hide();
    layout->addWidget(errorHint_);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    host_ = new QWidget(scroll_);
    hostLayout_ = new QVBoxLayout(host_);
    hostLayout_->setContentsMargins(0, 0, 8, 0);
    hostLayout_->setSpacing(12);
    scroll_->setWidget(host_);
    layout->addWidget(scroll_, 1);

    raw_ = new QPlainTextEdit(this);
    raw_->setReadOnly(true);
    raw_->setObjectName(QStringLiteral("recordRaw"));
    raw_->hide();
    layout->addWidget(raw_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("statusNote"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(closeButton, &QPushButton::clicked, this, [this] { requestClose(); });

    // Ctrl+S saves, Ctrl+F jumps to the filter. Escape is handled in keyPressEvent so
    // it can defer to an open sheet.
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this] {
        if (saveButton_ != nullptr && saveButton_->isEnabled()) {
            save();
        }
    });
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        if (search_ != nullptr) {
            search_->setFocus();
            search_->selectAll();
        }
    });
    connect(addRecordButton_, &QPushButton::clicked, this, [this] { addEntry(); });
    connect(saveButton_, &QPushButton::clicked, this, [this] { save(); });
    connect(revertButton_, &QPushButton::clicked, this, [this] { revert(); });
    connect(migrateButton_, &QPushButton::clicked, this, [this] { migrate(); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        query_ = text.trimmed();
        rebuild();
    });

    saveButton_->setEnabled(false);
    revertButton_->setEnabled(false);
}

void RecordWindow::applyStyle() {
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QString strong = dark ? QStringLiteral("#f2f2f7") : QStringLiteral("#1c1c1e");
    const QString body = dark ? QStringLiteral("#aeaeb2") : QStringLiteral("#3a3a3c");
    const QString muted = dark ? QStringLiteral("#8e8e93") : QStringLiteral("#78787d");
    const QString badge = dark ? QStringLiteral("#6c6c74") : QStringLiteral("#9a9aa0");
    const QString card = dark ? QStringLiteral("#161618") : QStringLiteral("#ffffff");
    const QString bar = dark ? QStringLiteral("#1c1c22") : QStringLiteral("#f2f2f7");
    const QString border = dark ? QStringLiteral("rgba(255, 255, 255, 22)")
                                : QStringLiteral("rgba(0, 0, 0, 20)");
    const QString quiet = dark ? QStringLiteral("#d1d1d6") : QStringLiteral("#3a3a3c");
    const QString quietOff = dark ? QStringLiteral("#636366") : QStringLiteral("#b0b0b5");
    disclosureColor_ = dark ? QStringLiteral("#a8a8b0") : QStringLiteral("#78787d");
    // The widest action group sets the column, measured so a larger system font
    // still fits instead of clipping the buttons.
    const QFontMetrics metrics(font());
    const int buttonPadding = 34; // quiet-button padding from the app sheet
    actionColumn_ = metrics.horizontalAdvance(QStringLiteral("Add field")) + buttonPadding
        + metrics.horizontalAdvance(QStringLiteral("Rename")) + buttonPadding
        + metrics.horizontalAdvance(QStringLiteral("Remove")) + buttonPadding + 10;
    rowHoverStyle_ = dark ? QStringLiteral("QFrame#recordRow { background: rgba(255, 255, 255, 14);"
                                           " border-radius: 8px; }")
                          : QStringLiteral("QFrame#recordRow { background: rgba(0, 0, 0, 8);"
                                           " border-radius: 8px; }");

    QString sheet = QStringLiteral(R"(
        QLabel#recordFileName { color: %1; font-size: 20px; font-weight: 600; }
        QLabel#recordCountLabel { color: %3; font-size: 12px; }
        QLabel#recordEmptyHint { color: %3; font-size: 13px; }
        QFrame#recordCard { background: %4; border: 1px solid %5; border-radius: 16px; }
        QFrame#recordRow { background: transparent; border: none; }
        QLabel#recordKey { color: %1; font-size: 13px; font-weight: 600; background: transparent; }
        QLabel#recordValue { color: %2; font-size: 13px; background: transparent; }
        QLabel#recordKindBadge { color: %6; font-size: 10px; background: transparent; }
        QLabel#recordCardTitle { color: %1; font-size: 15px; font-weight: 600; background: transparent; }
        /* The field follows the page palette: the app sheet's own field colours are
           dark in both themes, which reads as a hole in the light one. */
        QLineEdit#recordSearch {
            color: %1; background: %4; border: 1px solid %5;
            min-height: 30px; border-radius: 10px; padding: 0 10px;
        }
        QLineEdit#recordSearch:focus { border: 1px solid #2672c8; }
        QPushButton[quiet="true"]:focus { border: 1px solid #2672c8; border-radius: 7px; }
        QFrame#recordLegacyBar { background: %7; border: 1px solid %5; border-radius: 14px; }
        QLabel#recordLegacyLabel { color: %2; font-size: 13px; }

        QWidget#recordChildren { background: transparent; }
    )")
            // Seven placeholders, seven arguments. Passing eight left %8 holding `guide`
            // and dropped `bar`, so the legacy bar was painted near-transparent in both
            // themes and every call warned that an argument was missing.
            .arg(strong, body, muted, card, border, badge, bar);
    // Appended rather than added as %9/%10: Qt's arg() parses two-digit
    // placeholders badly once an earlier pass has consumed the leading digit.
    // Quiet actions are #d1d1d6 in the app sheet, which disappears on a white card.
    sheet += QStringLiteral("#recordPage QPushButton[quiet=\"true\"] { color: %1; }")
                 .arg(quiet);
    sheet += QStringLiteral(
                 "#recordPage QPushButton[quiet=\"true\"]:disabled { color: %1; }")
                 .arg(quietOff);
    restyling_ = true;
    setStyleSheet(sheet);
    restyling_ = false;
    styledWindowColor_ = palette().color(QPalette::Window);
}

void RecordWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        // applyStyle() calls setStyleSheet(), which sends StyleChange right back
        // here. Re-entering it recursed until the stack overflowed, so a real
        // theme change is the only thing allowed to run it again.
        const QColor window = palette().color(QPalette::Window);
        if (!restyling_ && window != styledWindowColor_) {
            applyStyle();
            if (editable_) {
                rebuild();
            }
        }
    }
    QWidget::changeEvent(event);
}

void RecordWindow::openRecord(std::shared_ptr<core::Vault> vault, const std::int64_t recordId,
    const QString& itemName, const std::int64_t folderId) {
    vault_ = std::move(vault);
    recordId_ = recordId;
    itemName_ = itemName;
    folderId_ = folderId;
    fileName_->setText(itemName.isEmpty() ? QStringLiteral("Record") : itemName);
    fileName_->setToolTip(itemName_);
    load();
}

void RecordWindow::load() {
    if (!vault_ || recordId_ <= 0) {
        return;
    }
    setBusy(true);
    const auto vault = vault_;
    const auto id = recordId_;
    auto* watcher = new QFutureWatcher<LoadOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const LoadOutcome outcome = watcher->result();
        watcher->deleteLater();
        setBusy(false);
        if (!outcome.ok) {
            setStatus(outcome.error, true);
            return;
        }
        loaded_ = outcome.bytes;
        RecordDocument document;
        if (parseRecord(loaded_, &document)) {
            fill(document);
            setStatus(QString(), false);
        } else if (const int version = recordFormatVersion(loaded_); version == 1 || version == 2) {
            showLegacy(loaded_, version);
        } else {
            showUnreadable(loaded_);
        }
        refreshDirty();
    });
    watcher->setFuture(QtConcurrent::run([vault, id] {
        LoadOutcome outcome;
        auto bytes = vault->read_video_bytes(id);
        if (!bytes) {
            outcome.error = recordErrorText(bytes.error());
            return outcome;
        }
        outcome.ok = true;
        outcome.bytes = toByteArray(bytes.value());
        return outcome;
    }));
}

void RecordWindow::fill(const RecordDocument& document) {
    document_ = document;
    editable_ = true;
    legacy_.clear();
    legacyBar_->hide();
    errorHint_->hide();
    raw_->hide();
    scroll_->show();
    addRecordButton_->setEnabled(true);
    search_->setEnabled(true);
    collapsed_.clear();
    revealed_.clear();
    rebuild();
}

void RecordWindow::showLegacy(const QByteArray& bytes, const int version) {
    editable_ = false;
    document_ = RecordDocument{};
    legacy_ = bytes;
    scroll_->hide();
    raw_->show();
    raw_->setPlainText(QString::fromUtf8(bytes));
    addRecordButton_->setEnabled(false);
    saveButton_->setEnabled(false);
    revertButton_->setEnabled(false);
    search_->setEnabled(false);
    search_->clear();
    countLabel_->clear();
    errorHint_->hide();
    legacyLabel_->setText(QStringLiteral(
        "This record was written by version %1 of the format. Migrating rewrites it in "
        "place — same item, same folder and tags — so the editor can open it.").arg(version));
    legacyBar_->show();
    setStatus(QString(), false);
}

void RecordWindow::showUnreadable(const QByteArray& bytes) {
    editable_ = false;
    document_ = RecordDocument{};
    legacy_.clear();
    legacyBar_->hide();
    scroll_->hide();
    raw_->show();
    raw_->setPlainText(QString::fromUtf8(bytes));
    addRecordButton_->setEnabled(false);
    saveButton_->setEnabled(false);
    revertButton_->setEnabled(false);
    search_->setEnabled(false);
    search_->clear();
    countLabel_->clear();
    errorHint_->setText(QStringLiteral(
        "This item is not a record this build can read, so it is shown as raw text and "
        "left untouched."));
    errorHint_->show();
    setStatus(QString(), false);
}

void RecordWindow::migrate() {
    if (legacy_.isEmpty() || !vault_ || recordId_ <= 0) {
        return;
    }
    RecordDocument document;
    QString note;
    if (!convertLegacyRecord(legacy_, itemName_, &document, &note)) {
        setStatus(QStringLiteral("This content could not be converted."), true);
        return;
    }
    const QByteArray bytes = serializeRecord(document);
    if (bytes.isEmpty()) {
        setStatus(QStringLiteral("The converted record could not be prepared for saving."), true);
        return;
    }
    const QString success = note.isEmpty()
        ? QStringLiteral("Migrated.")
        : QStringLiteral("Migrated. %1").arg(note);
    writeContent(bytes, success, [this, bytes] {
        legacy_.clear();
        legacyBar_->hide();
        raw_->hide();
        errorHint_->hide();
        scroll_->show();
        addRecordButton_->setEnabled(true);
        RecordDocument parsed;
        if (parseRecord(bytes, &parsed)) {
            fill(parsed);
        }
    });
}

void RecordWindow::rebuild() {
    if (hostLayout_ == nullptr) {
        return;
    }
    while (QLayoutItem* item = hostLayout_->takeAt(0)) {
        if (item->widget() != nullptr) {
            // Hidden before it is reaped: a rebuild is often triggered from one of
            // these very widgets, so it cannot be deleted inside the signal, but an
            // unparented-yet-alive widget would otherwise paint over the new rows.
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    const bool searching = !query_.isEmpty();
    if (document_.entries.isEmpty()) {
        auto* hint = new QLabel(QStringLiteral(
            "No records yet. \"Add record\" starts one — for example \"Web Services\" — "
            "then add fields to it: text, numbers, secrets, objects and arrays can nest."),
            host_);
        hint->setObjectName(QStringLiteral("recordEmptyHint"));
        hint->setWordWrap(true);
        hint->setContentsMargins(4, 8, 4, 8);
        hostLayout_->addWidget(hint);
    } else {
        int shown = 0;
        for (int index = 0; index < document_.entries.size(); ++index) {
            if (searching && !entryMatchesQuery(document_.entries.at(index), query_)) {
                continue;
            }
            buildEntry(index, hostLayout_);
            ++shown;
        }
        if (searching && shown == 0) {
            auto* hint = new QLabel(
                QStringLiteral("Nothing matches \"%1\".").arg(query_), host_);
            hint->setObjectName(QStringLiteral("recordEmptyHint"));
            hint->setWordWrap(true);
            hint->setContentsMargins(4, 8, 4, 8);
            hostLayout_->addWidget(hint);
        }
    }
    hostLayout_->addStretch(1);
    updateEmptyState();
}

void RecordWindow::buildEntry(const int entryIndex, QVBoxLayout* layout) {
    if (entryIndex < 0 || entryIndex >= document_.entries.size()) {
        return;
    }
    const RecordEntry& entry = document_.entries.at(entryIndex);

    auto* card = new HoverRow(host_);
    card->setObjectName(QStringLiteral("recordCard"));
    card->setAttribute(Qt::WA_Hover, true);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 10, 12, 12);
    cardLayout->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setSpacing(2);
    auto* title = new ClickLabel(
        entry.title.isEmpty() ? QStringLiteral("(untitled record)") : entry.title, card);
    title->setObjectName(QStringLiteral("recordCardTitle"));
    title->setToolTip(QStringLiteral("Double-click to rename this record"));
    title->onDoubleClick = [this, entryIndex] { renameEntry(entryIndex); };
    header->addWidget(title, 1);

    auto* addField = quietIconButton(QStringLiteral("plus"), QStringLiteral("Add a field to this record"), card);
    auto* rename = quietIconButton(
        QStringLiteral("pencil"), QStringLiteral("Rename this record"), card);
    auto* up = quietIconButton(
        QStringLiteral("arrow-up"), QStringLiteral("Move this record up"), card);
    auto* down = quietIconButton(
        QStringLiteral("arrow-down"), QStringLiteral("Move this record down"), card);
    auto* asTemplate = quietIconButton(QStringLiteral("bookmark-plus"),
        QStringLiteral("Save this record as a template"), card);
    auto* remove = quietIconButton(
        QStringLiteral("trash"), QStringLiteral("Delete this record"), card);
    up->setEnabled(entryIndex > 0);
    down->setEnabled(entryIndex + 1 < document_.entries.size());
    header->addWidget(rename);
    header->addWidget(up);
    header->addWidget(down);
    header->addWidget(remove);
    header->addWidget(asTemplate);
    header->addWidget(addField);
    // The card keeps one visible action; the rest show on hover like the rows.
    for (auto* button : {rename, up, down, asTemplate, remove}) {
        card->actions.append(button);
    }
    card->endHover();
    cardLayout->addLayout(header);

    NodePath bodyPath;
    bodyPath.entry = entryIndex;
    const bool titleMatched = !query_.isEmpty()
        && entry.title.contains(query_, Qt::CaseInsensitive);
    buildChildren(card, cardLayout, bodyPath, entry.body, 0,
        QStringLiteral("e%1").arg(entryIndex), titleMatched);

    if (!editable_) {
        addField->setEnabled(false);
        asTemplate->setEnabled(false);
        rename->setEnabled(false);
        up->setEnabled(false);
        down->setEnabled(false);
        remove->setEnabled(false);
    }

    connect(addField, &QPushButton::clicked, this, [this, bodyPath] { addChild(bodyPath); });
    connect(asTemplate, &QPushButton::clicked, this,
        [this, entryIndex] { beginSaveAsTemplateEntry(entryIndex); });
    connect(rename, &QPushButton::clicked, this, [this, entryIndex] { renameEntry(entryIndex); });
    connect(up, &QPushButton::clicked, this, [this, entryIndex] { moveEntry(entryIndex, -1); });
    connect(down, &QPushButton::clicked, this, [this, entryIndex] { moveEntry(entryIndex, 1); });
    connect(remove, &QPushButton::clicked, this, [this, entryIndex] { removeEntry(entryIndex); });

    layout->addWidget(card);
}

void RecordWindow::buildChildren(QWidget* host, QVBoxLayout* layout, const NodePath& path,
    const RecordNode& node, const int depth, const QString& collapseKey, const bool showAll) {
    const bool searching = !query_.isEmpty();
    if (node.type == ValueType::Object) {
        for (int index = 0; index < node.members.size(); ++index) {
            NodePath child = path;
            child.steps.append(index);
            const QString& key = node.members.at(index).first;
            const RecordNode& value = node.members.at(index).second;
            if (searching && !showAll && !nodeMatchesQuery(value, key, query_)) {
                continue;
            }
            buildRow(host, layout, child, value, depth, collapseKey + QLatin1Char('/') + key, key,
                showAll);
        }
        return;
    }
    if (node.type == ValueType::Array) {
        for (int index = 0; index < node.items.size(); ++index) {
            NodePath child = path;
            child.steps.append(index);
            const RecordNode& item = node.items.at(index);
            if (searching && !showAll && !nodeMatchesQuery(item, QString(), query_)) {
                continue;
            }
            buildRow(host, layout, child, item, depth,
                collapseKey + QStringLiteral("/#%1").arg(index), QString(), showAll);
        }
    }
}

void RecordWindow::buildRow(QWidget* parent, QVBoxLayout* layout, const NodePath& path,
    const RecordNode& node, const int depth, const QString& collapseKey, const QString& key,
    const bool showAll) {
    const bool container = node.isContainer();
    const bool named = !key.isEmpty();
    const bool secret = node.type == ValueType::Secret;
    const bool searching = !query_.isEmpty();
    // While filtering, everything on a matching path is open — a collapsed branch
    // that matches would otherwise stay hidden behind its parent.
    const bool open = searching || !collapsed_.contains(collapseKey);
    // A key that matches shows everything under it, not just its matching children.
    const bool childShowAll = showAll
        || (searching && key.contains(query_, Qt::CaseInsensitive));
    const bool masked = secret && !revealed_.contains(collapseKey);

    auto* row = new HoverRow(parent);
    row->setObjectName(QStringLiteral("recordRow"));

    row->setAttribute(Qt::WA_Hover, true);
    row->hoverStyle = rowHoverStyle_;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(4, 3, 0, 3);
    rowLayout->setSpacing(0);

    // Fixed left column. Indentation lives inside it: depth pushes the key right,
    // but the column keeps one width, so the value starts on the same x on every
    // row at every depth.
    auto* left = new QWidget(row);
    left->setFixedWidth(kLeftColumnWidth);
    auto* leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(depth * kIndentPx, 0, 0, 0);
    leftLayout->setSpacing(0); // gaps are explicit: type sits kTypeGap from the key

    // The disclosure column exists on every row, empty where there is nothing to
    // disclose, so keys line up at a depth instead of jumping left on leaves.
    QWidget* toggleHost = new QWidget(left);
    toggleHost->setFixedWidth(kToggleWidth);
    auto* toggleLayout = new QHBoxLayout(toggleHost);
    toggleLayout->setContentsMargins(0, 0, 0, 0);
    toggleLayout->setSpacing(0);
    if (container) {
        auto* toggle = disclosureButton(toggleHost, disclosureColor_);
        toggle->setIcon(icons::lucideSolid(
            open ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
            QColor(disclosureColor_)));
        toggle->setIconSize(QSize(14, 14));
        toggle->setToolTip(open ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
        toggleLayout->addWidget(toggle, 0, Qt::AlignVCenter);
        connect(toggle, &QPushButton::clicked, this,
            [this, collapseKey, open] {
                if (open) {
                    collapsed_.insert(collapseKey);
                } else {
                    collapsed_.remove(collapseKey);
                }
                rebuild();
            });
    }
    leftLayout->addWidget(toggleHost);
    leftLayout->addSpacing(kColumnGap);

    // Key (or the element's index) — double-click renames it.
    const QString keyText = named ? key
                                  : QStringLiteral("%1").arg(path.steps.isEmpty() ? 0
                                                                                 : path.steps.last());

    // The type sits right beside the key, so it is measured at its own 10px size and
    // only the room it needs comes out of the key. Text (the default) shows nothing.
    const QString badgeText = node.type == ValueType::String ? QString()
                                                             : valueTypeLabel(node.type);
    QFont badgeFont = left->font();
    badgeFont.setPixelSize(10);
    const int badgeWidth = badgeText.isEmpty()
        ? 0
        : QFontMetrics(badgeFont).horizontalAdvance(badgeText) + 2;
    const int keyMax = qMax(70, kLeftColumnWidth - depth * kIndentPx - kToggleWidth
            - 2 * kColumnGap - badgeWidth - (badgeText.isEmpty() ? 0 : kTypeGap));

    auto* keyLabel = new ClickLabel(elide(keyText, left->font(), keyMax), left);
    keyLabel->setObjectName(QStringLiteral("recordKey"));
    keyLabel->setMaximumWidth(keyMax); // hugs its text; elides only past this
    keyLabel->setToolTip(named ? QStringLiteral("Double-click to rename") : QString());
    if (named) {
        keyLabel->onDoubleClick = [this, path] { renameNode(path); };
    }
    leftLayout->addWidget(keyLabel);

    if (!badgeText.isEmpty()) {
        auto* badge = new QLabel(badgeText, left);
        badge->setObjectName(QStringLiteral("recordKindBadge"));
        badge->setToolTip(QStringLiteral("Type: %1").arg(badgeText));
        leftLayout->addSpacing(kTypeGap);
        leftLayout->addWidget(badge);
    }
    // Whatever is left of the fixed column is the one gap before the value.
    leftLayout->addStretch(1);
    rowLayout->addWidget(left);

    QString valueText;
    if (container) {
        valueText = node.type == ValueType::Object
            ? QStringLiteral("%1 field%2").arg(node.members.size()).arg(node.members.size() == 1 ? QString() : QStringLiteral("s"))
            : QStringLiteral("%1 item%2").arg(node.items.size()).arg(node.items.size() == 1 ? QString() : QStringLiteral("s"));
    } else if (masked) {
        valueText = kMask;
    } else {
        valueText = node.text;
    }
    auto* valueLabel = new ElidedLabel(row);
    valueLabel->setObjectName(QStringLiteral("recordValue"));
    valueLabel->setToolTip(container || masked ? QString() : node.text);
    valueLabel->setFullText(valueText);
    if (!container) {
        valueLabel->onDoubleClick = [this, path] { editNode(path); };
    }
    rowLayout->addWidget(valueLabel, 1);

    // Fixed action column: the buttons keep one home on the right, and the value
    // gets exactly the space between the two fixed columns.
    auto* actionHost = new QWidget(row);
    actionHost->setFixedWidth(actionColumn_);
    // Hidden action buttons take no space; without a floor on the column the row
    // would shrink at rest and grow as soon as the pointer arrived.
    actionHost->setFixedHeight(kRowActionSize);
    auto* actionLayout = new QHBoxLayout(actionHost);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(2);
    actionLayout->addStretch(1);
    rowLayout->addWidget(actionHost);

    layout->addWidget(row);

    if (container) {
        auto* children = new QWidget(parent);
        children->setObjectName(QStringLiteral("recordChildren"));
        children->setAttribute(Qt::WA_StyledBackground, true);
        auto* childLayout = new QVBoxLayout(children);
        // No left margin: depth is expressed by the row's own left column. A margin
        // here would add to that indent at every level and walk the columns right.
        childLayout->setContentsMargins(0, 2, 0, 2);
        childLayout->setSpacing(2);
        buildChildren(children, childLayout, path, node, depth + 1, collapseKey, childShowAll);
        children->setVisible(open);
        layout->addWidget(children);
    }

    if (!editable_) {
        return;
    }

    // Actions exist in the layout from the start (so nothing moves under the
    // pointer) but only paint while the row is hovered.
    struct RowAction {
        QString icon;
        QString tip;
        std::function<void()> run;
    };
    QVector<RowAction> actions;
    if (container) {
        actions.append({QStringLiteral("plus"),
            node.type == ValueType::Object ? QStringLiteral("Add field")
                                           : QStringLiteral("Add value"),
            [this, path] { addChild(path); }});
        if (named) {
            actions.append({QStringLiteral("pencil"), QStringLiteral("Rename this key"),
                [this, path] { renameNode(path); }});
            // A named object is the unit a template captures, so the authoring entry point
            // sits on the structure itself rather than in the header.
            if (node.type == ValueType::Object) {
                actions.append({QStringLiteral("bookmark-plus"),
                    QStringLiteral("Save this object as a template"),
                    [this, path] { beginSaveAsTemplate(path); }});
            }
        }
        actions.append({QStringLiteral("trash"), QStringLiteral("Remove this key and its contents"),
            [this, path] { removeNode(path); }});
    } else {
        actions.append({QStringLiteral("square-pen"), QStringLiteral("Edit key, value or type"),
            [this, path] { editNode(path); }});
        if (secret) {
            actions.append({masked ? QStringLiteral("eye") : QStringLiteral("eye-off"),
                masked ? QStringLiteral("Reveal this secret") : QStringLiteral("Hide this secret"),
                [this, collapseKey, masked] {
                    if (masked) {
                        revealed_.insert(collapseKey);
                    } else {
                        revealed_.remove(collapseKey);
                    }
                    rebuild();
                }});
        }
        actions.append({QStringLiteral("copy"), QStringLiteral("Copy this value"),
            [this, path] { copyNode(path); }});
        actions.append({QStringLiteral("trash"), QStringLiteral("Remove this field"),
            [this, path] { removeNode(path); }});
    }
    for (const auto& action : actions) {
        auto* button = quietIconButton(action.icon, action.tip, actionHost);
        connect(button, &QPushButton::clicked, this, action.run);
        actionLayout->addWidget(button);
        row->actions.append(button);
    }
    row->endHover();
}

RecordNode* RecordWindow::resolve(const NodePath& path) {
    if (path.entry < 0 || path.entry >= document_.entries.size()) {
        return nullptr;
    }
    RecordNode* node = &document_.entries[path.entry].body;
    for (const int step : path.steps) {
        if (node->type == ValueType::Object) {
            if (step < 0 || step >= node->members.size()) {
                return nullptr;
            }
            node = &node->members[step].second;
        } else if (node->type == ValueType::Array) {
            if (step < 0 || step >= node->items.size()) {
                return nullptr;
            }
            node = &node->items[step];
        } else {
            return nullptr;
        }
    }
    return node;
}

RecordNode* RecordWindow::resolveParent(const NodePath& path, QString* key) {
    if (path.steps.isEmpty()) {
        return nullptr;
    }
    NodePath parent = path;
    parent.steps.removeLast();
    RecordNode* node = resolve(parent);
    if (node == nullptr) {
        return nullptr;
    }
    if (key != nullptr) {
        key->clear();
        if (node->type == ValueType::Object) {
            const int index = path.steps.last();
            if (index >= 0 && index < node->members.size()) {
                *key = node->members.at(index).first;
            }
        }
    }
    return node;
}

QString RecordWindow::pathText(const NodePath& path) const {
    if (path.entry < 0 || path.entry >= document_.entries.size()) {
        return QString();
    }
    const RecordEntry& entry = document_.entries.at(path.entry);
    QString text = entry.title.isEmpty() ? QStringLiteral("(untitled record)") : entry.title;
    const RecordNode* node = &entry.body;
    for (const int step : path.steps) {
        if (node->type == ValueType::Object && step >= 0 && step < node->members.size()) {
            text += QStringLiteral(" / ") + node->members.at(step).first;
            node = &node->members.at(step).second;
        } else if (node->type == ValueType::Array && step >= 0 && step < node->items.size()) {
            text += QStringLiteral("[%1]").arg(step);
            node = &node->items.at(step);
        } else {
            break;
        }
    }
    return text;
}

// ---------------------------------------------------------------------------
// Prompts, drawn in the window (the same sheet the library rename uses)
// ---------------------------------------------------------------------------

bool RecordWindow::promptNode(const QString& heading, const bool withKey, NodeDraft* draft) {
    if (draft == nullptr) {
        return false;
    }
    QString key = draft->key;
    QString value = draft->value;
    auto type = draft->type;
    QString problem;

    for (;;) {
        InWindowSheet sheet(this);
        sheet.setTitle(heading);
        if (!problem.isEmpty()) {
            sheet.setMessage(problem, true);
        }

        QLineEdit* keyEdit = nullptr;
        if (withKey) {
            keyEdit = sheet.addTextEntry(QLineEdit::Normal, key, QStringLiteral("Name"));
        }

        auto* typeRow = new QWidget();
        auto* typeLayout = new QHBoxLayout(typeRow);
        typeLayout->setContentsMargins(0, 0, 0, 0);
        typeLayout->setSpacing(10);
        auto* typeLabel = new QLabel(QStringLiteral("Type"), typeRow);
        typeLabel->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
        auto* typeCombo = new QComboBox(typeRow);
        typeCombo->addItem(QStringLiteral("Text"), static_cast<int>(ValueType::String));
        typeCombo->addItem(QStringLiteral("Number"), static_cast<int>(ValueType::Number));
        typeCombo->addItem(QStringLiteral("Secret"), static_cast<int>(ValueType::Secret));
        typeCombo->addItem(QStringLiteral("Object"), static_cast<int>(ValueType::Object));
        typeCombo->addItem(QStringLiteral("Array"), static_cast<int>(ValueType::Array));
        for (int index = 0; index < typeCombo->count(); ++index) {
            if (typeCombo->itemData(index).toInt() == static_cast<int>(type)) {
                typeCombo->setCurrentIndex(index);
                break;
            }
        }
        typeLayout->addWidget(typeLabel);
        typeLayout->addWidget(typeCombo, 1);
        sheet.body()->addWidget(typeRow);

        auto* valueEdit = new QPlainTextEdit(value, nullptr);
        valueEdit->setPlaceholderText(QStringLiteral("Value"));
        valueEdit->setTabChangesFocus(true);
        valueEdit->setMinimumHeight(84);
        valueEdit->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { color: #ffffff; background: #1c1c22; border: none;"
            " border-radius: 8px; padding: 8px; }"));
        // A container has no value, so the editor is hidden for Object and Array. Without
        // keeping its space the sheet shrinks the moment one of them is picked, which moves
        // the buttons under the cursor mid-click.
        QSizePolicy valuePolicy = valueEdit->sizePolicy();
        valuePolicy.setRetainSizeWhenHidden(true);
        valueEdit->setSizePolicy(valuePolicy);
        valueEdit->setVisible(type != ValueType::Object && type != ValueType::Array);
        sheet.body()->addWidget(valueEdit);

        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), typeCombo,
            [typeCombo, valueEdit](int) {
                const auto picked = static_cast<ValueType>(typeCombo->currentData().toInt());
                valueEdit->setVisible(picked != ValueType::Object && picked != ValueType::Array);
            });

        sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
        sheet.addButton(QStringLiteral("Save"), 1, true, QStringLiteral("check"));
        if (sheet.run() != 1) {
            return false;
        }

        type = static_cast<ValueType>(typeCombo->currentData().toInt());
        if (keyEdit != nullptr) {
            key = keyEdit->text().trimmed();
        }
        value = valueEdit->toPlainText();
        if (withKey && !isValidKey(key)) {
            problem = QStringLiteral("A field needs a name.");
            continue;
        }
        if (type == ValueType::Number) {
            const QString canonical = canonicalNumber(value);
            if (canonical.isEmpty()) {
                problem = QStringLiteral(
                    "\"%1\" cannot be stored as a number without changing its digits. Use "
                    "text for values like long card numbers.").arg(value.trimmed());
                continue;
            }
            value = canonical;
        }
        if (type == ValueType::Object || type == ValueType::Array) {
            value.clear();
        }
        draft->type = type;
        draft->key = key;
        draft->value = value;
        return true;
    }
}

bool RecordWindow::promptText(const QString& heading, const QString& message,
    const QString& initial, QString* out) {
    if (out == nullptr) {
        return false;
    }
    QString current = initial;
    QString problem;
    for (;;) {
        InWindowSheet sheet(this);
        sheet.setTitle(heading);
        if (!problem.isEmpty()) {
            sheet.setMessage(problem, true);
        } else if (!message.isEmpty()) {
            sheet.setMessage(message);
        }
        sheet.addTextEntry(QLineEdit::Normal, current, QStringLiteral("Name"));
        sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
        sheet.addButton(QStringLiteral("Save"), 1, true, QStringLiteral("check"));
        if (sheet.run() != 1) {
            return false;
        }
        current = sheet.text().trimmed();
        if (current.isEmpty()) {
            problem = QStringLiteral("A name is required.");
            continue;
        }
        *out = current;
        return true;
    }
}

bool RecordWindow::confirm(const QString& heading, const QString& message, const QString& action) {
    InWindowSheet sheet(this);
    sheet.setTitle(heading);
    sheet.setMessage(message);
    sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    sheet.addButton(action, 1, true);
    return sheet.run() == 1;
}

// ---------------------------------------------------------------------------
// Structure changes
// ---------------------------------------------------------------------------

// Prompts for a name, then hands the entry to writeTemplate(). Both capture points -- an
// object row and a whole record -- come through here, so they cannot drift apart.
void RecordWindow::promptSaveTemplate(const RecordEntry& entry, const QString& defaultName) {
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("Save as template"));
    sheet.setMessage(QStringLiteral("This becomes a template. It is copied, so the record "
                                    "keeps its own fields and stays a record."));
    auto* nameEdit = sheet.addTextEntry(QLineEdit::Normal, defaultName,
        QStringLiteral("Template name"));
    auto* hint = new QLabel(QStringLiteral(
        "Templates are kept in one place, not in the library. Find them under New from "
        "template, and under Insert template on any object row."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
    sheet.body()->addWidget(hint);
    sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    QPushButton* saveButton = sheet.addButton(QStringLiteral("Save template"), 1, true,
        QStringLiteral("bookmark-plus"));
    const auto validate = [nameEdit, saveButton] {
        saveButton->setEnabled(!nameEdit->text().trimmed().isEmpty());
    };
    connect(nameEdit, &QLineEdit::textChanged, this, [validate](const QString&) { validate(); });
    validate();
    if (sheet.run() != 1) {
        return;
    }
    writeTemplate(nameEdit->text().trimmed(), entry);
}

void RecordWindow::writeTemplate(const QString& name, const RecordEntry& entry) {
    if (vault_ == nullptr || name.isEmpty()) {
        return;
    }
    RecordDocument document;
    document.isTemplate = true;
    document.templateName = name;
    document.entries.push_back(entry);
    const QByteArray bytes = serializeRecord(document);

    setStatus(QStringLiteral("Saving the template\u2026"), false);
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<std::shared_ptr<core::Result<std::int64_t>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, name] {
        const auto outcome = *watcher->result();
        watcher->deleteLater();
        if (!outcome) {
            setStatus(QStringLiteral("The template could not be saved."), true);
            return;
        }
        setStatus(QStringLiteral("Template \"%1\" saved.").arg(name), false);
        emit templateSaved(name);
    });
    watcher->setFuture(QtConcurrent::run([vault, bytes, name] {
        // Templates live in the store folder, created on first use. Not in the record's own
        // folder: a template is not a file the user browses.
        std::int64_t folder_id = 0;
        if (const auto folders = vault->list_folders(); folders) {
            for (const auto& folder : folders.value()) {
                if (isTemplateFolderName(QString::fromStdString(folder.name))) {
                    folder_id = folder.id;
                    break;
                }
            }
        }
        if (folder_id == 0) {
            const auto made = vault->create_folder(0, templateFolderName().toStdString());
            if (!made) {
                return std::make_shared<core::Result<std::int64_t>>(made.error());
            }
            folder_id = made.value();
        }
        const std::span<const unsigned char> content(
            reinterpret_cast<const unsigned char*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size()));
        return std::make_shared<core::Result<std::int64_t>>(
            vault->import_bytes(content, name.toStdString(), folder_id));
    }));
}

void RecordWindow::beginSaveAsTemplate(const NodePath& path) {
    if (!editable_ || vault_ == nullptr) {
        return;
    }
    RecordNode* node = resolve(path);
    QString key;
    resolveParent(path, &key);
    if (node == nullptr || node->type != ValueType::Object) {
        return;
    }
    RecordEntry entry;
    // The key the structure had becomes the default insert name, and the placeholder title
    // when the template is used as a whole record.
    entry.title = key;
    entry.body = *node;          // a copy: the record keeps what it has
    promptSaveTemplate(entry, key);
}

void RecordWindow::beginSaveAsTemplateEntry(const int entryIndex) {
    if (!editable_ || vault_ == nullptr || entryIndex < 0
        || entryIndex >= document_.entries.size()) {
        return;
    }
    const RecordEntry& entry = document_.entries.at(entryIndex);
    RecordEntry copy = entry;
    copy.generated.clear();      // a record has no generators; it is defaults only
    promptSaveTemplate(copy, entry.title);
}

void RecordWindow::beginInsertTemplate(const NodePath& parent) {
    if (!editable_ || vault_ == nullptr) {
        return;
    }
    // The same list the library's "New from template" offers: the built-ins, then the user's
    // own template items, classified by reading each item's first bytes and decrypting in
    // full only the ones that are templates. Mirrors MainWindow::beginNewRecordFromTemplate.
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<std::shared_ptr<QVector<QPair<QString, RecordEntry>>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, parent] {
        const auto found = watcher->result();
        watcher->deleteLater();
        if (found->isEmpty()) {
            setStatus(QStringLiteral("No templates yet. Save a record as a template to get one."),
                true);
            return;
        }
        QMenu menu(this);
        for (const auto& choice : *found) {
            QAction* action = menu.addAction(choice.first);
            action->setData(choice.first);
        }
        QAction* picked = menu.exec(QCursor::pos());
        if (picked == nullptr) {
            return;
        }
        for (const auto& choice : *found) {
            if (choice.first == picked->data().toString()) {
                promptInsertTemplate(parent, choice.second);
                return;
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([vault] {
        auto choices = std::make_shared<QVector<QPair<QString, RecordEntry>>>();
        for (const auto& document : builtinTemplates()) {
            for (const auto& entry : document.entries) {
                choices->push_back({document.templateName.isEmpty() ? entry.title
                                                                    : document.templateName,
                    entry});
            }
        }
        const auto listed = vault->list_videos();
        if (!listed) {
            return choices;
        }
        for (const auto& item : listed.value()) {
            const auto head = vault->read_video_range(item.id, 0, 64);
            if (!head || head.value().empty()) {
                continue;
            }
            if (!looksLikeTemplate(QByteArray(reinterpret_cast<const char*>(head.value().data()),
                    static_cast<int>(head.value().size())))) {
                continue;
            }
            const auto full = vault->read_video_bytes(item.id);
            if (!full) {
                continue;
            }
            RecordDocument document;
            if (!parseRecord(QByteArray(reinterpret_cast<const char*>(full.value().data()),
                                static_cast<int>(full.value().size())),
                    &document)
                || !document.isTemplate) {
                continue;
            }
            for (const auto& entry : document.entries) {
                choices->push_back({document.templateName.isEmpty() ? entry.title
                                                                    : document.templateName,
                    entry});
            }
        }
        return choices;
    }));
}

void RecordWindow::promptInsertTemplate(const NodePath& parent, const RecordEntry& templateEntry) {
    RecordNode* container = resolve(parent);
    if (container == nullptr || container->type != ValueType::Object) {
        return;
    }
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("Insert template"));
    sheet.setMessage(QStringLiteral("Adds \"%1\" here as a new object. Values the template "
                                    "generates are filled in on the way.")
                         .arg(templateEntry.title.isEmpty() ? QStringLiteral("a template")
                                                            : templateEntry.title));
    // Guidance where it is needed: the user is looking at the template list, so tell them
    // how to get their own into it.
    auto* guidance = new QLabel(QStringLiteral(
        "Make your own: right-click a record in the library and choose Save as template."));
    guidance->setWordWrap(true);
    guidance->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
    sheet.body()->addWidget(guidance);

    // The key defaults to what the structure was called in the template, and stays editable.
    auto* keyEdit = sheet.addTextEntry(QLineEdit::Normal, templateEntry.title,
        QStringLiteral("Name"));
    auto* problem = new QLabel(this);
    problem->setWordWrap(true);
    problem->setStyleSheet(QStringLiteral("color: #ff6b6b; background: transparent;"));
    QSizePolicy problemPolicy = problem->sizePolicy();
    problemPolicy.setRetainSizeWhenHidden(true);
    problem->setSizePolicy(problemPolicy);
    problem->hide();
    sheet.body()->addWidget(problem);

    QPushButton* insert = sheet.addButton(QStringLiteral("Insert"), 1, true,
        QStringLiteral("plus"));
    sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));

    // A name that is already a field here is refused by disabling Insert and saying why,
    // rather than by letting the click through to an error.
    const auto validate = [container, keyEdit, insert, problem] {
        const QString key = keyEdit->text().trimmed();
        QString text;
        if (key.isEmpty()) {
            text = QStringLiteral("A field needs a name.");
        } else {
            for (const auto& member : container->members) {
                if (member.first == key) {
                    text = QStringLiteral("\"%1\" is already a field here.").arg(key);
                    break;
                }
            }
        }
        problem->setText(text);
        problem->setVisible(!text.isEmpty());
        insert->setEnabled(text.isEmpty());
    };
    connect(keyEdit, &QLineEdit::textChanged, this, [validate](const QString&) { validate(); });
    validate();

    if (sheet.run() != 1) {
        return;
    }
    QString failure;
    if (!insertTemplate(container, templateEntry, keyEdit->text(), &failure)) {
        setStatus(failure, true);
        return;
    }
    markChanged();
    rebuild();
}

void RecordWindow::addChild(const NodePath& parent) {
    RecordNode* container = resolve(parent);
    if (container == nullptr
        || (container->type != ValueType::Object && container->type != ValueType::Array)) {
        return;
    }
    const bool object = container->type == ValueType::Object;
    if (object) {
        // One "+" on an object row covers both ways of adding structure: a field of your
        // own, or a template's structure inserted whole.
        QMenu menu(this);
        QAction* addFieldAction = menu.addAction(QStringLiteral("Add field"));
        QAction* insertAction = menu.addAction(QStringLiteral("Insert template…"));
        insertAction->setEnabled(editable_);
        QAction* picked = menu.exec(QCursor::pos());
        if (picked == nullptr) {
            return;                        // dismissed without choosing
        }
        if (picked == insertAction) {
            beginInsertTemplate(parent);
            return;
        }
        Q_UNUSED(addFieldAction);
        // Otherwise fall through to the Add field prompt below.
    }
    NodeDraft draft;
    draft.type = ValueType::String;
    if (!promptNode(object ? QStringLiteral("Add field") : QStringLiteral("Add value"), object,
            &draft)) {
        return;
    }
    RecordNode child = draft.type == ValueType::Object
        ? RecordNode::object()
        : draft.type == ValueType::Array ? RecordNode::array()
                                         : RecordNode::leaf(draft.type, draft.value);
    if (object) {
        for (const auto& member : container->members) {
            if (member.first == draft.key) {
                setStatus(QStringLiteral("\"%1\" is already a field here.").arg(draft.key), true);
                return;
            }
        }
        container->members.append({draft.key, child});
    } else {
        container->items.append(child);
    }
    sortMembers(&document_.entries[parent.entry].body);
    markChanged();
    rebuild();
}

void RecordWindow::editNode(const NodePath& path) {
    RecordNode* node = resolve(path);
    if (node == nullptr) {
        return;
    }
    const bool wasContainer = node->isContainer();
    const int childCount = node->type == ValueType::Object
        ? static_cast<int>(node->members.size())
        : static_cast<int>(node->items.size());

    QString key;
    resolveParent(path, &key);
    NodeDraft draft;
    draft.type = node->type;
    draft.key = key;
    draft.value = node->text;
    if (!promptNode(QStringLiteral("Edit value"), !key.isEmpty(), &draft)) {
        return;
    }
    const bool becomesContainer = draft.type == ValueType::Object || draft.type == ValueType::Array;
    if (wasContainer && !becomesContainer && childCount > 0) {
        if (!confirm(QStringLiteral("Change type"),
                QStringLiteral("This discards the %1 item(s) inside \"%2\".")
                    .arg(childCount)
                    .arg(pathText(path)),
                QStringLiteral("Discard"))) {
            return;
        }
    }
    if (becomesContainer) {
        *node = draft.type == ValueType::Object ? RecordNode::object() : RecordNode::array();
    } else {
        *node = RecordNode::leaf(draft.type, draft.value);
    }
    sortMembers(&document_.entries[path.entry].body);
    markChanged();
    rebuild();
}

void RecordWindow::renameNode(const NodePath& path) {
    QString key;
    RecordNode* parent = resolveParent(path, &key);
    if (parent == nullptr || parent->type != ValueType::Object || key.isEmpty()) {
        return;
    }
    QString name;
    if (!promptText(QStringLiteral("Rename field"), QString(), key, &name)) {
        return;
    }
    if (name == key) {
        return;
    }
    const int index = path.steps.last();
    for (int other = 0; other < parent->members.size(); ++other) {
        if (other != index && parent->members.at(other).first == name) {
            setStatus(QStringLiteral("\"%1\" is already a field here.").arg(name), true);
            return;
        }
    }
    parent->members[index].first = name;
    sortMembers(&document_.entries[path.entry].body);
    markChanged();
    rebuild();
}

void RecordWindow::removeNode(const NodePath& path) {
    RecordNode* parent = resolveParent(path, nullptr);
    if (parent == nullptr) {
        return;
    }
    const RecordNode* node = resolve(path);
    const bool container = node != nullptr && node->isContainer();
    if (container
        && !confirm(QStringLiteral("Remove"),
            QStringLiteral("\"%1\" and everything inside it is removed.").arg(pathText(path)),
            QStringLiteral("Remove"))) {
        return;
    }
    if (parent->type == ValueType::Object) {
        parent->members.removeAt(path.steps.last());
    } else {
        parent->items.removeAt(path.steps.last());
    }
    markChanged();
    rebuild();
}

void RecordWindow::copyNode(const NodePath& path) {
    const RecordNode* node = resolve(path);
    if (node == nullptr || node->isContainer()) {
        return;
    }
    copyToClipboard(node->text, node->type == ValueType::Secret,
        QStringLiteral("Copied \"%1\".").arg(pathText(path)));
}

void RecordWindow::copyToClipboard(const QString& text, const bool secret, const QString& note) {
    QGuiApplication::clipboard()->setText(text);
    if (secret) {
        clipboardValue_ = text;
        clipboardTimer_->start(kClipboardClearMs);
        setStatus(QStringLiteral("%1 The clipboard clears in 30 seconds.").arg(note), false);
        return;
    }
    clipboardTimer_->stop();
    clipboardValue_.clear();
    setStatus(note, false);
}

void RecordWindow::addEntry() {
    if (!editable_) {
        return;
    }
    QString title;
    if (!promptText(QStringLiteral("New record"), QStringLiteral("Title"), QString(), &title)) {
        return;
    }
    RecordEntry entry;
    entry.title = title;
    document_.entries.append(entry);
    markChanged();
    rebuild();
}

void RecordWindow::renameEntry(const int entryIndex) {
    if (!editable_ || entryIndex < 0 || entryIndex >= document_.entries.size()) {
        return;
    }
    QString title;
    if (!promptText(QStringLiteral("Rename record"), QStringLiteral("Title"),
            document_.entries.at(entryIndex).title, &title)) {
        return;
    }
    document_.entries[entryIndex].title = title;
    markChanged();
    rebuild();
}

void RecordWindow::removeEntry(const int entryIndex) {
    if (!editable_ || entryIndex < 0 || entryIndex >= document_.entries.size()) {
        return;
    }
    const QString title = document_.entries.at(entryIndex).title;
    if (!confirm(QStringLiteral("Delete record"),
            QStringLiteral("\"%1\" and every field in it is deleted.")
                .arg(title.isEmpty() ? QStringLiteral("(untitled record)") : title),
            QStringLiteral("Delete"))) {
        return;
    }
    document_.entries.removeAt(entryIndex);
    markChanged();
    rebuild();
}

void RecordWindow::moveEntry(const int entryIndex, const int delta) {
    const int target = entryIndex + delta;
    if (!editable_ || entryIndex < 0 || entryIndex >= document_.entries.size() || target < 0
        || target >= document_.entries.size()) {
        return;
    }
    std::swap(document_.entries[entryIndex], document_.entries[target]);
    markChanged();
    rebuild();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void RecordWindow::markChanged() {
    refreshDirty();
}

void RecordWindow::refreshDirty() {
    if (!editable_) {
        saveButton_->setEnabled(false);
        revertButton_->setEnabled(false);
        return;
    }
    const bool dirty = serializeRecord(document_) != loaded_;
    saveButton_->setEnabled(dirty);
    revertButton_->setEnabled(dirty);
}

bool RecordWindow::isDirty() const {
    return editable_ && serializeRecord(document_) != loaded_;
}

void RecordWindow::sortEntries(const bool ascending) {
    if (!editable_) {
        return;
    }
    std::stable_sort(document_.entries.begin(), document_.entries.end(),
        [ascending](const RecordEntry& left, const RecordEntry& right) {
            const int order = QString::compare(left.title, right.title, Qt::CaseInsensitive);
            return ascending ? order < 0 : order > 0;
        });
    rebuild();
    refreshDirty();
    setStatus(QStringLiteral("Sorted by title."), false);
}

void RecordWindow::requestClose() {
    if (isDirty()) {
        InWindowSheet sheet(this);
        sheet.setTitle(QStringLiteral("Discard changes?"));
        sheet.setMessage(QStringLiteral(
            "This record has edits that have not been saved. Closing now loses them."));
        sheet.addButton(QStringLiteral("Keep editing"), 0, false, QStringLiteral("x"));
        sheet.addButton(QStringLiteral("Discard changes"), 1, true, QStringLiteral("trash"));
        if (sheet.run() != 1) {
            return;
        }
    }
    emit closeRequested();
}

void RecordWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        // A sheet runs its own nested loop and handles its own Escape; don't close the
        // page out from under it. The sheet has no Q_OBJECT (its backdrop is painted),
        // so findChildren<InWindowSheet*>() cannot be trusted here - the card it owns
        // is the reliable handle.
        for (auto* card : findChildren<QWidget*>(QStringLiteral("inWindowCard"))) {
            QWidget* sheet = card->parentWidget();
            if (sheet != nullptr && sheet->isVisible()) {
                QWidget::keyPressEvent(event);
                return;
            }
        }
        if (search_ != nullptr && !query_.isEmpty()) {
            search_->clear();
            return;
        }
        requestClose();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RecordWindow::save() {
    if (!editable_ || !vault_ || recordId_ <= 0) {
        return;
    }
    const QString problem = recordProblem(document_);
    if (!problem.isEmpty()) {
        setStatus(problem, true);
        return;
    }
    const QByteArray bytes = serializeRecord(document_);
    if (bytes.isEmpty()) {
        setStatus(QStringLiteral("The record could not be prepared for saving."), true);
        return;
    }
    writeContent(bytes, QStringLiteral("Saved."), nullptr);
}

void RecordWindow::writeContent(const QByteArray& bytes, const QString& success,
    const std::function<void()>& after) {
    if (!vault_ || recordId_ <= 0) {
        return;
    }
    setBusy(true);
    const auto vault = vault_;
    const auto id = recordId_;
    const QString fallback = itemName_;
    auto* watcher = new QFutureWatcher<LoadOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, bytes, success, after] {
            const LoadOutcome outcome = watcher->result();
            watcher->deleteLater();
            setBusy(false);
            if (!outcome.ok) {
                setStatus(outcome.error, true);
                return;
            }
            loaded_ = bytes;
            if (after) {
                after();
            }
            refreshDirty();
            setStatus(success, false);
            emit saved();
        });
    watcher->setFuture(QtConcurrent::run([vault, id, bytes, fallback] {
        LoadOutcome outcome;
        // Keep whatever name the item has now: renaming is done from the gallery,
        // and this window may have been open across a rename.
        QString name = fallback;
        if (auto listed = vault->list_videos(); listed) {
            for (const auto& video : listed.value()) {
                if (video.id == id && !video.display_name.empty()) {
                    name = QString::fromStdString(video.display_name);
                    break;
                }
            }
        }
        auto replaced = vault->replace_video_content(id, bytesOf(bytes), name.toStdString());
        if (!replaced) {
            outcome.error = recordErrorText(replaced.error());
            return outcome;
        }
        outcome.ok = true;
        return outcome;
    }));
}

void RecordWindow::revert() {
    if (!editable_) {
        return;
    }
    RecordDocument document;
    if (!parseRecord(loaded_, &document)) {
        setStatus(QStringLiteral("The stored record could not be read back."), true);
        return;
    }
    fill(document);
    setStatus(QStringLiteral("Reverted."), false);
}

void RecordWindow::setBusy(const bool busy) {
    setEnabled(!busy);
    if (busy) {
        setStatus(QStringLiteral("Working..."), false);
    }
}

void RecordWindow::setStatus(const QString& message, const bool problem) {
    status_->setText(message);
    status_->setProperty("problem", problem);
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

void RecordWindow::updateEmptyState() {
    const int records = document_.entries.size();
    if (!query_.isEmpty()) {
        int shown = 0;
        for (const auto& entry : document_.entries) {
            if (entryMatchesQuery(entry, query_)) {
                ++shown;
            }
        }
        countLabel_->setText(QStringLiteral("%1 of %2 records").arg(shown).arg(records));
        return;
    }
    countLabel_->setText(records == 1 ? QStringLiteral("1 record")
                                      : QStringLiteral("%1 records").arg(records));
}

} // namespace videovault::app
