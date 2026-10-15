#include "videovault/app/record_window.hpp"

#include "videovault/core/vault.hpp"

#include <QClipboard>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <span>
#include <string>

namespace videovault::app {
namespace {

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

QPushButton* flatButton(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setProperty("quiet", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// Copied secrets leave the clipboard on their own after this delay.
constexpr int kClipboardClearMs = 30000;
constexpr int kMaxValueLines = 8;

struct LoadOutcome {
    bool ok{false};
    QString error;
    QByteArray bytes;
};

} // namespace

// ---------------------------------------------------------------------------
// RecordFieldCard
// ---------------------------------------------------------------------------
RecordFieldCard::RecordFieldCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("recordCard"));
    setFrameShape(QFrame::StyledPanel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout();
    header->setSpacing(6);

    name_ = new QLineEdit(this);
    name_->setPlaceholderText(QStringLiteral("Name"));
    header->addWidget(name_, 1);

    badge_ = new QLabel(this);
    badge_->setObjectName(QStringLiteral("recordKindBadge"));
    header->addWidget(badge_);

    reveal_ = flatButton(QStringLiteral("Reveal"), this);
    secret_ = flatButton(QStringLiteral("Secret"), this);
    secret_->setCheckable(true);
    copy_ = flatButton(QStringLiteral("Copy"), this);
    remove_ = flatButton(QStringLiteral("Remove"), this);
    header->addWidget(reveal_);
    header->addWidget(secret_);
    header->addWidget(copy_);
    header->addWidget(remove_);
    layout->addLayout(header);

    value_ = new QPlainTextEdit(this);
    value_->setObjectName(QStringLiteral("recordValue"));
    value_->setPlaceholderText(QStringLiteral("Value"));
    value_->setTabChangesFocus(true);
    value_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(value_);

    connect(name_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (updating_) {
            return;
        }
        field_.name = name_->text();
        notifyChanged();
    });
    connect(value_, &QPlainTextEdit::textChanged, this, [this] {
        adjustValueHeight();
        if (updating_) {
            return;
        }
        // While masked, the editor holds dots; the model is left alone so the
        // real value cannot be replaced by them.
        if (!(field_.secret && !revealed_)) {
            field_.value = value_->toPlainText();
        }
        refreshKind();
        notifyChanged();
    });
    connect(secret_, &QPushButton::toggled, this, [this](const bool on) {
        field_.secret = on;
        if (!on) {
            revealed_ = false;
        }
        applyMask();
        refreshKind();
        notifyChanged();
    });
    connect(reveal_, &QPushButton::clicked, this, [this] {
        revealed_ = !revealed_;
        applyMask();
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        if (onCopy) {
            onCopy(field_.name, field_.value, field_.secret);
        }
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (onRemove) {
            onRemove(this);
        }
    });
}

void RecordFieldCard::setField(const RecordField& field) {
    field_ = field;
    revealed_ = false;
    updating_ = true;
    name_->setText(field_.name);
    value_->setPlainText(field_.value);
    secret_->setChecked(field_.secret);
    updating_ = false;
    applyMask();
    refreshKind();
    adjustValueHeight();
}

void RecordFieldCard::setEditable(const bool editable) {
    editable_ = editable;
    name_->setReadOnly(!editable);
    secret_->setEnabled(editable);
    remove_->setVisible(editable);
    value_->setReadOnly(!editable || (field_.secret && !revealed_));
}

bool RecordFieldCard::matches(const QString& needle) const {
    if (needle.isEmpty()) {
        return true;
    }
    if (field_.name.contains(needle, Qt::CaseInsensitive)) {
        return true;
    }
    // Secret values are deliberately not searchable: matching on them would
    // leak their content through the search box.
    if (!field_.secret && field_.value.contains(needle, Qt::CaseInsensitive)) {
        return true;
    }
    return false;
}

void RecordFieldCard::focusName() {
    name_->setFocus();
    name_->selectAll();
}

void RecordFieldCard::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    adjustValueHeight();
}

void RecordFieldCard::applyMask() {
    const bool masked = field_.secret && !revealed_;
    reveal_->setVisible(field_.secret);
    reveal_->setText(revealed_ ? QStringLiteral("Hide") : QStringLiteral("Reveal"));

    updating_ = true;
    if (masked) {
        QString dots;
        dots.fill(QChar(0x2022), std::clamp(int(field_.value.size()), 8, 16));
        value_->setPlainText(dots);
    } else {
        value_->setPlainText(field_.value);
    }
    updating_ = false;
    value_->setReadOnly(!editable_ || masked);
    adjustValueHeight();
}

void RecordFieldCard::refreshKind() {
    // The badge is read off the field rather than the raw text, so a field that
    // is marked as JSON keeps saying so while its text is being edited.
    const QString label = recordFieldKindLabel(field_);
    badge_->setText(label);
    badge_->setVisible(!label.isEmpty());

    const ValueKind kind = classifyValue(field_.value, field_.secret);
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    switch (kind) {
    case ValueKind::Secret:
    case ValueKind::Url:
    case ValueKind::Number:
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        break;
    case ValueKind::Plain:
    case ValueKind::Email:
    case ValueKind::Multiline:
        break;
    }
    if (field_.json) {
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    if (value_->font() != font) {
        value_->setFont(font);
        adjustValueHeight();
    }
}

void RecordFieldCard::adjustValueHeight() {
    const QFontMetrics metrics(value_->font());
    const int line = std::max(metrics.lineSpacing(), 1);
    // The document must be told the wrapping width for wrapped values to report
    // the right height, otherwise a long value collapses to one line.
    value_->document()->setTextWidth(value_->viewport()->width());
    const int content = static_cast<int>(value_->document()->size().height()) + 12;
    value_->setFixedHeight(
        std::clamp(content, line + 14, kMaxValueLines * line + 14));
}

void RecordFieldCard::notifyChanged() {
    if (onChanged) {
        onChanged();
    }
}

// ---------------------------------------------------------------------------
// RecordWindow
// ---------------------------------------------------------------------------
RecordWindow::RecordWindow(QWidget* parent) : QWidget(parent) {
    buildInterface();
    clipboardTimer_ = new QTimer(this);
    clipboardTimer_->setSingleShot(true);
    connect(clipboardTimer_, &QTimer::timeout, this, [this] {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (!clipboardValue_.isEmpty() && clipboard->text() == clipboardValue_) {
            clipboard->clear();
            setStatus(QStringLiteral("Clipboard cleared."), false);
        }
        clipboardValue_.clear();
    });
}

void RecordWindow::buildInterface() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto* header = new QHBoxLayout();
    header->setSpacing(6);
    auto* titleLabel = new QLabel(QStringLiteral("Record"), this);
    titleLabel->setObjectName(QStringLiteral("recordTitleLabel"));
    header->addWidget(titleLabel);

    titleEdit_ = new QLineEdit(this);
    titleEdit_->setPlaceholderText(QStringLiteral("Title"));
    titleEdit_->setMinimumWidth(220);
    header->addWidget(titleEdit_, 1);

    addButton_ = flatButton(QStringLiteral("Add field"), this);
    revertButton_ = flatButton(QStringLiteral("Revert"), this);
    saveButton_ = new QPushButton(QStringLiteral("Save"), this);
    saveButton_->setProperty("primary", true);
    auto* closeButton = flatButton(QStringLiteral("Close"), this);
    header->addWidget(addButton_);
    header->addWidget(revertButton_);
    header->addWidget(saveButton_);
    header->addWidget(closeButton);
    layout->addLayout(header);

    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(6);
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("Search fields…"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(200);
    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(QStringLiteral("recordCountLabel"));
    searchRow->addWidget(searchEdit_, 1);
    searchRow->addWidget(countLabel_);
    layout->addLayout(searchRow);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    cardHost_ = new QWidget(scroll_);
    cardLayout_ = new QVBoxLayout(cardHost_);
    cardLayout_->setContentsMargins(0, 0, 0, 0);
    cardLayout_->setSpacing(10);
    emptyHint_ = new QLabel(QStringLiteral(
        "No fields yet. Use \"Add field\" to store a name and a value, "
        "for example a password, an e-mail address or a note."),
        cardHost_);
    emptyHint_->setObjectName(QStringLiteral("recordEmptyHint"));
    emptyHint_->setWordWrap(true);
    cardLayout_->addWidget(emptyHint_);
    cardLayout_->addStretch(1);
    scroll_->setWidget(cardHost_);
    layout->addWidget(scroll_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("recordStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(closeButton, &QPushButton::clicked, this, [this] { emit closeRequested(); });
    connect(addButton_, &QPushButton::clicked, this, [this] {
        addField(RecordField{QString(), QString(), false}, true);
        markChanged();
    });
    connect(saveButton_, &QPushButton::clicked, this, [this] { save(); });
    connect(revertButton_, &QPushButton::clicked, this, [this] { revert(); });
    connect(titleEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        markChanged();
    });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        applySearch();
    });

    saveButton_->setEnabled(false);
    revertButton_->setEnabled(false);
}

void RecordWindow::openRecord(std::shared_ptr<core::Vault> vault,
    const std::int64_t record_id, const QString& title) {
    vault_ = std::move(vault);
    recordId_ = record_id;
    loaded_.clear();

    for (auto* card : cards_) {
        cardLayout_->removeWidget(card);
        card->deleteLater();
    }
    cards_.clear();

    titleEdit_->setText(title);
    searchEdit_->clear();
    setEditable(true);
    setStatus(QString(), false);
    updateEmptyState();
    setBusy(true);
    load();
}

void RecordWindow::load() {
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
        const bool parsed = parseRecord(loaded_, &document);
        if (!parsed) {
            // Written by something else, or damaged. Show it read-only so an
            // edit can never overwrite content this editor does not understand.
            RecordDocument raw;
            raw.title = titleEdit_->text();
            RecordField field;
            field.name = QStringLiteral("content");
            field.value = QString::fromUtf8(loaded_);
            raw.fields.push_back(field);
            fill(raw);
            setEditable(false);
            setStatus(QStringLiteral(
                "This item is not a readable record. It is shown read-only so "
                "nothing is lost."), true);
            return;
        }
        fill(document);
        setEditable(true);
        setStatus(QString(), false);
    });
    watcher->setFuture(QtConcurrent::run([vault, id]() -> LoadOutcome {
        LoadOutcome outcome;
        if (!vault) {
            outcome.error = QStringLiteral("The vault is not open.");
            return outcome;
        }
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
    for (auto* card : cards_) {
        cardLayout_->removeWidget(card);
        card->deleteLater();
    }
    cards_.clear();

    titleEdit_->setText(document.title);
    for (const auto& field : document.fields) {
        addField(field, false);
    }
    updateEmptyState();
    refreshDirty();
}

void RecordWindow::addField(const RecordField& field, const bool focus) {
    auto* card = new RecordFieldCard(cardHost_);
    card->setField(field);
    card->onChanged = [this] { markChanged(); };
    card->onRemove = [this](RecordFieldCard* removed) { removeCard(removed); };
    card->onCopy = [this](const QString& name, const QString& value, const bool secret) {
        copyValue(name, value, secret);
    };
    // Insert before the trailing hint and stretch.
    cardLayout_->insertWidget(std::max(0, cardLayout_->count() - 2), card);
    cards_.push_back(card);
    updateEmptyState();
    if (focus) {
        card->focusName();
    }
}

void RecordWindow::removeCard(RecordFieldCard* card) {
    cardLayout_->removeWidget(card);
    card->deleteLater();
    cards_.erase(std::remove(cards_.begin(), cards_.end(), card), cards_.end());
    updateEmptyState();
    markChanged();
}

void RecordWindow::updateEmptyState() {
    emptyHint_->setVisible(cards_.empty());
    applySearch();
}

void RecordWindow::applySearch() {
    const QString needle = searchEdit_->text().trimmed();
    int visible = 0;
    for (auto* card : cards_) {
        const bool match = card->matches(needle);
        card->setVisible(match);
        if (match) {
            ++visible;
        }
    }
    if (needle.isEmpty()) {
        countLabel_->setText(QStringLiteral("%1 field(s)").arg(int(cards_.size())));
    } else {
        countLabel_->setText(QStringLiteral("%1 of %2 fields")
            .arg(visible).arg(int(cards_.size())));
    }
}

void RecordWindow::markChanged() {
    refreshDirty();
}

void RecordWindow::refreshDirty() {
    // The title may have been typed, a card may have been edited: compare the
    // serialized form with what is on disk.
    const bool dirty = editable_ && serializeRecord(collect()) != loaded_;
    saveButton_->setEnabled(dirty);
    revertButton_->setEnabled(dirty);
}

RecordDocument RecordWindow::collect() const {
    RecordDocument document;
    document.title = titleEdit_->text().trimmed();
    for (const auto* card : cards_) {
        RecordField field = card->field();
        if (field.name.trimmed().isEmpty() && field.value.isEmpty()) {
            continue;
        }
        if (field.name.trimmed().isEmpty()) {
            field.name = QStringLiteral("value");
        }
        document.fields.push_back(field);
    }
    return document;
}

void RecordWindow::setEditable(const bool editable) {
    editable_ = editable;
    titleEdit_->setReadOnly(!editable);
    addButton_->setEnabled(editable);
    for (auto* card : cards_) {
        card->setEditable(editable);
    }
    if (!editable) {
        saveButton_->setEnabled(false);
        revertButton_->setEnabled(false);
    }
}

void RecordWindow::save() {
    if (!vault_ || !vault_->is_unlocked() || recordId_ <= 0 || !editable_) {
        return;
    }
    const RecordDocument document = collect();
    if (document.title.isEmpty()) {
        setStatus(QStringLiteral("Give the record a title before saving."), true);
        titleEdit_->setFocus();
        return;
    }
    // A value marked as JSON has to parse before it is stored: the container
    // would otherwise hold text that claims to be JSON and is not.
    const QString problem = recordProblem(document);
    if (!problem.isEmpty()) {
        setStatus(problem, true);
        return;
    }
    const QByteArray bytes = serializeRecord(document);
    if (bytes == loaded_) {
        setStatus(QStringLiteral("Nothing to save."), false);
        return;
    }

    setBusy(true);
    const auto vault = vault_;
    const auto id = recordId_;
    const std::string title = document.title.toStdString();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, bytes] {
        const QString error = watcher->result();
        watcher->deleteLater();
        setBusy(false);
        if (!error.isEmpty()) {
            setStatus(error, true);
            return;
        }
        loaded_ = bytes;
        refreshDirty();
        setStatus(QStringLiteral("Saved."), false);
        emit saved();
    });
    watcher->setFuture(QtConcurrent::run([vault, id, bytes, title]() -> QString {
        if (!vault) {
            return QStringLiteral("The vault is not open.");
        }
        const auto result = vault->replace_video_content(id, bytesOf(bytes), title);
        if (!result || !result.value()) {
            return result ? QStringLiteral("The change was not stored.")
                          : recordErrorText(result.error());
        }
        return QString();
    }));
}

void RecordWindow::revert() {
    if (!vault_ || recordId_ <= 0) {
        return;
    }
    setBusy(true);
    load();
}

void RecordWindow::copyValue(const QString& name, const QString& value, const bool secret) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(value);
    clipboardValue_ = value;
    const QString label = name.trimmed().isEmpty() ? QStringLiteral("value") : name;

    if (secret) {
        // A password should not linger in the clipboard.
        clipboardTimer_->start(kClipboardClearMs);
        setStatus(QStringLiteral("Copied \"%1\". The clipboard clears in %2 seconds.")
                      .arg(label).arg(kClipboardClearMs / 1000), false);
    } else {
        clipboardTimer_->stop();
        setStatus(QStringLiteral("Copied \"%1\".").arg(label), false);
    }
}

void RecordWindow::setStatus(const QString& message, const bool problem) {
    status_->setText(message);
    status_->setProperty("problem", problem);
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

void RecordWindow::setBusy(const bool busy) {
    addButton_->setEnabled(!busy && editable_);
    titleEdit_->setReadOnly(busy || !editable_);
    if (busy) {
        saveButton_->setEnabled(false);
        revertButton_->setEnabled(false);
    } else {
        refreshDirty();
    }
}

} // namespace videovault::app
