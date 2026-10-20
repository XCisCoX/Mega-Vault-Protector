#include "videovault/app/in_window_sheet.hpp"

#include "videovault/app/icons.hpp"
#include "videovault/app/theme.hpp"

#include <algorithm>
#include <QEventLoop>
#include <QFrame>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QTimer>
#include <QScopedValueRollback>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QVBoxLayout>

namespace videovault::app {

InWindowSheet::InWindowSheet(QWidget* host)
    : QWidget(host == nullptr ? nullptr : host->window()) {
    setObjectName(QStringLiteral("inWindowSheet"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    // Palette roles, not hex: the sheet lives inside the window and has to look
    // native in both themes. The blurred, dimmed backdrop is painted in paintEvent.
    setStyleSheet(QStringLiteral(
        "InWindowSheet { background: transparent; }"
        "QFrame#inWindowCard { background: palette(base); border: 1px solid palette(midlight);"
        "  border-radius: 16px; }"
        "QFrame#inWindowCard QLabel { color: palette(window-text); background: transparent; }"
        "QFrame#inWindowCard QLineEdit, QFrame#inWindowCard QComboBox {"
        "  color: palette(text); background: palette(base);"
        "  border: 1px solid palette(midlight); border-radius: 8px;"
        "  min-height: 36px; padding: 0 10px; }"
        "QFrame#inWindowCard QLineEdit:focus, QFrame#inWindowCard QComboBox:focus {"
        "  border-color: palette(highlight); }"
        "QFrame#inWindowCard QComboBox QAbstractItemView {"
        "  color: palette(text); background: palette(base);"
        "  selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }"
        "QFrame#inWindowCard QCheckBox, QFrame#inWindowCard QRadioButton {"
        "  color: palette(window-text); background: transparent; }"
        "QFrame#inWindowCard QListWidget {"
        "  color: palette(text); background: palette(alternate-base); border: none;"
        "  border-radius: 8px; outline: none; }"
        "QFrame#inWindowCard QListWidget::item { padding: 6px 8px; }"
        "QFrame#inWindowCard QListWidget::item:selected {"
        "  background: palette(highlight); color: palette(highlighted-text); }"
        "QPushButton[sheetPrimary='true'] {"
        "  color: palette(highlighted-text); background: palette(highlight); border: none;"
        "  border-radius: 10px; min-height: 36px; padding: 0 16px; font-weight: 600; }"
        "QPushButton[sheetSecondary='true'] {"
        "  color: palette(button-text); background: palette(alternate-base);"
        "  border: 1px solid palette(midlight); border-radius: 10px;"
        "  min-height: 36px; padding: 0 16px; }"
        "QPushButton[sheetSecondary='true']:hover { background: palette(midlight); }"));

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
    // The body lives in a scroll area: a form taller than a short window scrolls
    // instead of being squeezed until half of each field is cut off.
    bodyHost_ = new QWidget;
    body_ = new QVBoxLayout(bodyHost_);
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(10);
    body_->setSizeConstraint(QLayout::SetMinimumSize);
    scroll_ = new QScrollArea(card_);
    scroll_->setObjectName(QStringLiteral("inWindowBody"));
    scroll_->setWidget(bodyHost_);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->viewport()->setAutoFillBackground(false);
    // Only the scroll container is made transparent, in code: a stylesheet
    // wildcard here would also strip the rounded white fields inside it.
    scroll_->setAutoFillBackground(false);
    bodyHost_->setAutoFillBackground(false);
    scroll_->setBackgroundRole(QPalette::NoRole);
    scroll_->viewport()->setBackgroundRole(QPalette::NoRole);
    bodyHost_->setBackgroundRole(QPalette::NoRole);
    scroll_->setStyleSheet(QStringLiteral(
        "QScrollArea#inWindowBody { background: transparent; border: none; }"));
    buttons_ = new QHBoxLayout();
    buttons_->setSpacing(8);
    buttons_->addStretch(1);
    column->addWidget(title_);
    column->addWidget(message_);
    column->addWidget(scroll_, 1);
    column->addLayout(buttons_);
    outer->addWidget(card_, 0, Qt::AlignHCenter);
    outer->addStretch(1);

    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] { finish(0); });
}

void InWindowSheet::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    // The host is blurred and only lightly dimmed, so the window stays visible
    // behind the card instead of disappearing under a black sheet.
    if (!backdrop_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(rect(), backdrop_);
    }
    const bool dark = isDarkTheme();
    painter.fillRect(rect(), QColor(0, 0, 0, dark ? 118 : 58));
}

void InWindowSheet::clampCardHeight() {
    QWidget* host = parentWidget();
    if (host == nullptr || card_ == nullptr || clamping_) {
        return;
    }
    const QScopedValueRollback<bool> guard(clamping_, true);
    const int needed = card_->sizeHint().height();
    const int room = std::max(220, host->height() - 48);
    if (needed <= room) {
        // The card fits: leave it to size itself. Pinning it here re-ran the
        // layout on every resize, which is what hung the offscreen platform.
        return;
    }
    if (card_->height() != room) {
        card_->setFixedHeight(room);
    }
}

void InWindowSheet::captureBackdrop() {
    QWidget* host = parentWidget();
    if (host == nullptr || host->size().isEmpty()) {
        return;
    }
    const QPixmap shot = host->grab();
    if (shot.isNull()) {
        return;
    }
    QGraphicsScene scene;
    QGraphicsPixmapItem item(shot);
    QGraphicsBlurEffect blur;
    // Scaled with the window so a maximised window does not turn to mush.
    blur.setBlurRadius(std::max(8.0, host->width() / 90.0));
    item.setGraphicsEffect(&blur);
    scene.addItem(&item);
    QPixmap out(shot.size());
    out.fill(Qt::transparent);
    QPainter painter(&out);
    scene.render(&painter);
    backdrop_ = out;
}

void InWindowSheet::setTitle(const QString& title) {
    title_->setText(title);
}

void InWindowSheet::setMessage(const QString& message, const bool problem) {
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
    message_->setStyleSheet(problem
        ? QStringLiteral("color: palette(window-text); background: transparent;")
        : QStringLiteral("color: palette(mid); background: transparent;"));
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
    // A property, not objectName: call sites name buttons for tests and for
    // automation, and the look must survive that.
    button->setProperty(primary ? "sheetPrimary" : "sheetSecondary", true);
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

std::function<void(InWindowSheet*)> InWindowSheet::testDriver_;

void InWindowSheet::setTestDriver(std::function<void(InWindowSheet*)> driver) {
    testDriver_ = std::move(driver);
}

int InWindowSheet::run() {
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return 0;
    }
    setGeometry(host->rect());
    host->installEventFilter(this);
    // Captured before the sheet is shown, so the backdrop is the window itself.
    backdrop_ = QPixmap();
    captureBackdrop();
    // Hug the content, but never exceed the window: past that the body scrolls.
    clampCardHeight();
    show();
    raise();
    if (entry_ != nullptr) {
        entry_->setFocus();
        entry_->selectAll();
    }
    if (testDriver_) {
        // A driver stands in for the user: it acts inside the visible sheet.
        testDriver_(this);
        hide();
        if (parentWidget() != nullptr) {
            parentWidget()->removeEventFilter(this);
        }
        return 1;
    }
    loop_.exec();
    hide();
    if (parentWidget() != nullptr) {
        parentWidget()->removeEventFilter(this);
    }
    return result_;
}

void InWindowSheet::addRow(QWidget* widget) {
    if (widget != nullptr) {
        body_->addWidget(widget);
    }
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
        // A shorter window must shrink the card too, or its buttons fall off the
        // bottom while the body keeps the old height. Deferred: querying the card
        // size hint inside the resize event re-entered the layout synchronously.
        if (!clampPending_) {
            clampPending_ = true;
            QTimer::singleShot(0, this, [this] {
                clampPending_ = false;
                clampCardHeight();
            });
        }
        raise();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace videovault::app
