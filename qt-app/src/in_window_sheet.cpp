#include "videovault/app/in_window_sheet.hpp"

#include "videovault/app/icons.hpp"

#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace videovault::app {

InWindowSheet::InWindowSheet(QWidget* host)
    : QWidget(host == nullptr ? nullptr : host->window()) {
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral(
        "InWindowSheet { background: rgba(0, 0, 0, 160); }"
        "QFrame#inWindowCard { background: #161618; border-radius: 16px; }"
        "QFrame#inWindowCard QLabel { color: white; background: transparent; }"
        "QFrame#inWindowCard QLineEdit, QFrame#inWindowCard QComboBox {"
        "  color: white; background: #1c1c22; border: none; border-radius: 8px;"
        "  min-height: 36px; padding: 0 10px; }"
        "QFrame#inWindowCard QComboBox QAbstractItemView {"
        "  color: white; background: #1c1c22; selection-background-color: #3390ec; }"
        "QFrame#inWindowCard QCheckBox { color: white; background: transparent; }"
        "QFrame#inWindowCard QListWidget {"
        "  color: white; background: #1c1c22; border: none; border-radius: 8px; outline: none; }"
        "QFrame#inWindowCard QListWidget::item { padding: 6px 8px; }"
        "QFrame#inWindowCard QListWidget::item:selected { background: rgba(51, 144, 236, 90); }"
        "QPushButton#inWindowPrimary {"
        "  color: white; background: #3390ec; border: none; border-radius: 10px;"
        "  min-height: 36px; padding: 0 16px; }"
        "QPushButton#inWindowSecondary {"
        "  color: white; background: #2a2a32; border: none; border-radius: 10px;"
        "  min-height: 36px; padding: 0 16px; }"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->addStretch(1);

    card_ = new QFrame(this);
    card_->setObjectName(QStringLiteral("inWindowCard"));
    card_->setFixedWidth(420);
    auto* column = new QVBoxLayout(card_);
    column->setContentsMargins(22, 18, 22, 18);
    column->setSpacing(12);

    title_ = new QLabel(card_);
    title_->setStyleSheet(QStringLiteral("font: 600 16px \"Segoe UI\";"));
    message_ = new QLabel(card_);
    message_->setWordWrap(true);
    message_->hide();
    body_ = new QVBoxLayout();
    body_->setSpacing(10);
    buttons_ = new QHBoxLayout();
    buttons_->setSpacing(8);
    buttons_->addStretch(1);
    column->addWidget(title_);
    column->addWidget(message_);
    column->addLayout(body_);
    column->addLayout(buttons_);
    outer->addWidget(card_, 0, Qt::AlignHCenter);
    outer->addStretch(1);

    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] { finish(0); });
}

void InWindowSheet::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, 160));
}

void InWindowSheet::setTitle(const QString& title) {
    title_->setText(title);
}

void InWindowSheet::setMessage(const QString& message, const bool problem) {
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
    message_->setStyleSheet(problem
        ? QStringLiteral("color: #ff6b6b; background: transparent;")
        : QStringLiteral("color: #aeaeb2; background: transparent;"));
}

QLineEdit* InWindowSheet::addTextEntry(
    const int echoMode, const QString& initial, const QString& placeholder) {
    entry_ = new QLineEdit(card_);
    entry_->setEchoMode(static_cast<QLineEdit::EchoMode>(echoMode));
    entry_->setText(initial);
    entry_->setPlaceholderText(placeholder);
    body_->addWidget(entry_);
    connect(entry_, &QLineEdit::returnPressed, this, [this] { finish(1); });
    return entry_;
}

QPushButton* InWindowSheet::addButton(const QString& label, const int result, const bool primary,
    const QString& icon) {
    auto* button = new QPushButton(label, card_);
    button->setObjectName(primary
        ? QStringLiteral("inWindowPrimary")
        : QStringLiteral("inWindowSecondary"));
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    if (!icon.isEmpty()) {
        button->setIconSize(QSize(15, 15));
        if (primary) {
            // The primary sheet button sits on the accent fill, so the glyph is white
            // in both themes rather than following ButtonText.
            button->setIcon(icons::lucideSolid(icon, QColor(0xff, 0xff, 0xff)));
        } else {
            icons::apply(button, icon, icons::Tone::Quiet);
        }
    }
    buttons_->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, result] { finish(result); });
    return button;
}

int InWindowSheet::run() {
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return 0;
    }
    setGeometry(host->rect());
    host->installEventFilter(this);
    show();
    raise();
    if (entry_ != nullptr) {
        entry_->setFocus();
        entry_->selectAll();
    }
    loop_.exec();
    hide();
    if (parentWidget() != nullptr) {
        parentWidget()->removeEventFilter(this);
    }
    return result_;
}

void InWindowSheet::finish(const int result) {
    result_ = result;
    if (loop_.isRunning()) {
        loop_.quit();
    }
}

QString InWindowSheet::text() const {
    return entry_ == nullptr ? QString() : entry_->text();
}

void InWindowSheet::mousePressEvent(QMouseEvent* event) {
    if (card_ != nullptr && !card_->geometry().contains(event->pos())) {
        finish(0);
        return;
    }
    QWidget::mousePressEvent(event);
}

bool InWindowSheet::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && parentWidget() != nullptr) {
        setGeometry(parentWidget()->rect());
        raise();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace videovault::app
