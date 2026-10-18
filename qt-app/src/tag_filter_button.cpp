#include "videovault/app/tag_filter_button.hpp"
#include "videovault/app/theme.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace videovault::app {
namespace {
constexpr int CountRole = Qt::UserRole + 1;

QColor withAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}
bool darkPaletteField(const QPalette& palette) {
    return palette.color(QPalette::Window).lightness() < 128;
}
QColor secondaryColor(const QPalette& palette) {
    return darkPaletteField(palette) ? QColor(142, 142, 147) : palette.color(QPalette::PlaceholderText);
}
QColor disabledColor(const QPalette& palette) {
    return darkPaletteField(palette) ? QColor(99, 99, 102) : palette.color(QPalette::Disabled, QPalette::ButtonText);
}
QColor hoverColor(const QPalette& palette) {
    return darkPaletteField(palette) ? QColor(44, 44, 46) : palette.color(QPalette::AlternateBase);
}

bool sameSelection(const QStringList& a, const QStringList& b) {
    if (a.size() != b.size()) return false;
    for (const auto& name : a)
        if (!b.contains(name, Qt::CaseInsensitive)) return false;
    return true;
}

class TagPopup final : public QWidget {
public:
    explicit TagPopup(QWidget* parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        // Inherit the live application palette; never pin an open popup dark.
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(withAlpha(palette().color(QPalette::WindowText), 35));
        painter.setBrush(palette().color(QPalette::Button));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);
    }
};

// Retain accessible check state without painting a native checkbox or letting
// the platform editor toggle separately from our row activation handlers.
class TagRowDelegate final : public QStyledItemDelegate {
public:
    explicit TagRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(240, 38);
    }
    bool editorEvent(QEvent*, QAbstractItemModel*, const QStyleOptionViewItem&,
                     const QModelIndex&) override { return false; }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(0, 1, 0, -1);
        const bool current = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (current || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(withAlpha(option.palette.color(current ? QPalette::Highlight : QPalette::WindowText), current ? 45 : 14));
            painter->drawRoundedRect(row, 8, 8);
        }
        if (index.data(Qt::CheckStateRole).toInt() == Qt::Checked) {
            const QPointF c(row.left() + 17, row.center().y());
            QPainterPath check;
            check.moveTo(c.x() - 4, c.y());
            check.lineTo(c.x() - 1, c.y() + 3);
            check.lineTo(c.x() + 5, c.y() - 4);
            painter->setPen(QPen(option.palette.color(QPalette::Highlight), 2.1, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter->drawPath(check);
        }
        painter->setFont(option.font);
        const QString count = index.data(CountRole).toString();
        const int countWidth = option.fontMetrics.horizontalAdvance(count);
        const QRect countRect(row.right() - countWidth - 12, row.top(), countWidth, row.height());
        painter->setPen(secondaryColor(option.palette));
        painter->drawText(countRect, Qt::AlignRight | Qt::AlignVCenter, count);
        const QRect nameRect(row.left() + 34, row.top(),
                             std::max(0, countRect.left() - row.left() - 46), row.height());
        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, nameRect.width()));
        if (option.state.testFlag(QStyle::State_HasFocus)) {
            painter->setPen(QPen(withAlpha(option.palette.color(QPalette::Highlight), 150), 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(row).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        }
        painter->restore();
    }
};
} // namespace

TagFilterButton::TagFilterButton(QWidget* parent) : QPushButton(parent) {
    setObjectName(QStringLiteral("tagFilterButton"));
    setAccessibleName(tr("Filter by tags"));
    setAccessibleDescription(tr("Choose one or more tags. No selection shows all items."));
    setText(tr("All items"));
    setMinimumSize(120, 36);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(this, &QPushButton::clicked, this, &TagFilterButton::showPopup);
}

void TagFilterButton::setTags(const QVector<TagFilterOption>& tags) {
    const auto previous = selected_;
    tags_.clear();
    QSet<QString> seen;
    for (const auto& option : tags) {
        const QString key = option.name.toCaseFolded();
        if (option.name.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        tags_.append(option);
    }
    selected_.clear();
    for (const auto& option : tags_)
        if (previous.contains(option.name, Qt::CaseInsensitive)) selected_.append(option.name);
    if (list_) rebuildRows();
    refreshSelection(previous);
}

QStringList TagFilterButton::selectedTags() const { return selected_; }

void TagFilterButton::setSelectedTags(const QStringList& tags) {
    const auto previous = selected_;
    selected_.clear();
    for (const auto& option : tags_)
        if (tags.contains(option.name, Qt::CaseInsensitive)) selected_.append(option.name);
    refreshSelection(previous);
}

void TagFilterButton::refreshSelection(const QStringList& previous) {
    setText(selected_.isEmpty() ? tr("All items") : selected_.size() == 1
            ? selected_.first() : tr("%1 tags").arg(selected_.size()));
    setToolTip(selected_.isEmpty() ? tr("Filter by tags") : selected_.join(QStringLiteral("\n")));
    if (clear_) clear_->setEnabled(!selected_.isEmpty());
    if (list_) {
        const QSignalBlocker blocker(list_);
        for (int i = 0; i < list_->count(); ++i) {
            auto* item = list_->item(i);
            item->setCheckState(selected_.contains(item->text(), Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked);
        }
        list_->viewport()->update();
    }
    updateGeometry();
    update();
    // Emit last and pass an owned snapshot: a receiver may rebuild this control.
    if (!sameSelection(previous, selected_)) {
        const auto snapshot = selected_;
        emit selectionChanged(snapshot);
    }
}

QSize TagFilterButton::sizeHint() const {
    return QSize(std::max(120, fontMetrics().horizontalAdvance(text()) + 56), 38);
}

void TagFilterButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool open = popup_ && popup_->isVisible();
    painter.setPen(hasFocus() || open ? palette().color(QPalette::Highlight)
                                    : withAlpha(palette().color(QPalette::WindowText), 28));
    painter.setBrush(underMouse() && isEnabled() ? hoverColor(palette())
                                              : palette().color(QPalette::Button));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
    painter.setPen(isEnabled() ? palette().color(QPalette::Text) : disabledColor(palette()));
    painter.setFont(font());
    const QRect labelRect = rect().adjusted(14, 0, -34, 0);
    painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                     fontMetrics().elidedText(text(), Qt::ElideRight, labelRect.width()));
    const QPointF c(width() - 19, height() / 2.0);
    QPainterPath chevron;
    const qreal direction = open ? -1 : 1;
    chevron.moveTo(c.x() - 4, c.y() - 2 * direction);
    chevron.lineTo(c.x(), c.y() + 2 * direction);
    chevron.lineTo(c.x() + 4, c.y() - 2 * direction);
    painter.setPen(QPen(isEnabled() ? secondaryColor(palette()) : disabledColor(palette()),
                        1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(chevron);
}

void TagFilterButton::showPopup() {
    if (!isVisible() || !isEnabled()) return;
    if (popup_ && popup_->isVisible()) { popup_->hide(); return; }
    if (!popup_) {
        popup_ = new TagPopup(this);
        popup_->setObjectName(QStringLiteral("tagFilterPopup"));
        popup_->setAccessibleName(tr("Tag filter"));
        popup_->installEventFilter(this);
        popup_->setStyleSheet(QStringLiteral(R"(
            QLabel { color: #f2f2f7; background: transparent; border: none; }
            QLabel#tagHeading { font-weight: 600; }
            QLabel#tagEmptyState { color: #8e8e93; }
            QLineEdit#tagSearch {
                color: #f2f2f7; background: #2c2c2e; border: 1px solid #3a3a3c;
                border-radius: 9px; min-height: 32px; padding: 0 10px;
                selection-background-color: #3390ec;
            }
            QLineEdit#tagSearch:focus { border-color: #3390ec; }
            QPushButton#clearTagFilter {
                color: #3390ec; background: transparent; border: none;
                border-radius: 6px; min-height: 28px; padding: 0 6px;
            }
            QPushButton#clearTagFilter:hover, QPushButton#clearTagFilter:focus { background: #2c2c2e; }
            QPushButton#clearTagFilter:disabled { color: #636366; }
            QListWidget#tagList { background: transparent; color: #f2f2f7; border: none; outline: none; }
            QScrollBar:vertical { background: transparent; width: 8px; }
            QScrollBar::handle:vertical { background: #48484a; border-radius: 4px; min-height: 24px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
            QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        )"));
        auto* layout = new QVBoxLayout(popup_);
        layout->setContentsMargins(12, 10, 12, 12);
        layout->setSpacing(8);
        auto* header = new QHBoxLayout;
        auto* heading = new QLabel(tr("Tags"), popup_);
        heading->setObjectName(QStringLiteral("tagHeading"));
        header->addWidget(heading);
        header->addStretch();
        clear_ = new QPushButton(tr("Clear"), popup_);
        clear_->setObjectName(QStringLiteral("clearTagFilter"));
        clear_->setAccessibleName(tr("Clear tag filter"));
        clear_->setCursor(Qt::PointingHandCursor);
        clear_->installEventFilter(this);
        header->addWidget(clear_);
        layout->addLayout(header);
        search_ = new QLineEdit(popup_);
        search_->setObjectName(QStringLiteral("tagSearch"));
        search_->setPlaceholderText(tr("Search tags\u2026"));
        search_->setAccessibleName(tr("Search tags"));
        search_->installEventFilter(this);
        layout->addWidget(search_);
        list_ = new QListWidget(popup_);
        list_->setObjectName(QStringLiteral("tagList"));
        list_->setAccessibleName(tr("Available tags"));
        list_->setAccessibleDescription(tr("Use arrow keys to move and Space to toggle a tag."));
        list_->setItemDelegate(new TagRowDelegate(list_));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list_->setMouseTracking(true);
        list_->setUniformItemSizes(true);
        list_->installEventFilter(this);
        layout->addWidget(list_, 1);
        empty_ = new QLabel(popup_);
        empty_->setObjectName(QStringLiteral("tagEmptyState"));
        empty_->setAlignment(Qt::AlignCenter);
        empty_->setMinimumHeight(64);
        layout->addWidget(empty_, 1);
        QWidget::setTabOrder(search_, list_);
        QWidget::setTabOrder(list_, clear_);
        connect(search_, &QLineEdit::textChanged, this, &TagFilterButton::filterRows);
        connect(clear_, &QPushButton::clicked, this, [this] { setSelectedTags({}); });
        connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
            QStringList checked;
            for (int i = 0; i < list_->count(); ++i)
                if (list_->item(i)->checkState() == Qt::Checked) checked.append(list_->item(i)->text());
            setSelectedTags(checked);
        });
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            toggleRow(list_->row(item));
        });
        rebuildRows();
        refreshSelection(selected_);
    }
    search_->clear();
    filterRows();
    positionPopup();
    popup_->show();
    popup_->raise();
    search_->setFocus(Qt::PopupFocusReason);
    update();
}

void TagFilterButton::rebuildRows() {
    const QSignalBlocker blocker(list_);
    const QString current = list_->currentItem() ? list_->currentItem()->text() : QString();
    list_->clear();
    for (const auto& option : tags_) {
        auto* item = new QListWidgetItem(option.name, list_);
        item->setData(CountRole, QString::number(option.count));
        item->setData(Qt::AccessibleTextRole, option.name);
        item->setData(Qt::AccessibleDescriptionRole, tr("%1 items").arg(option.count));
        item->setToolTip(option.name);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(selected_.contains(option.name, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked);
        if (option.name.compare(current, Qt::CaseInsensitive) == 0) list_->setCurrentItem(item);
    }
    filterRows();
}

void TagFilterButton::filterRows() {
    int visible = 0;
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        const bool match = item->text().contains(search_->text(), Qt::CaseInsensitive);
        item->setHidden(!match);
        if (match) ++visible;
    }
    if (list_->currentItem() && list_->currentItem()->isHidden()) list_->setCurrentRow(-1);
    list_->setVisible(visible != 0);
    empty_->setText(tags_.isEmpty() ? tr("No tags yet") : tr("No matching tags"));
    empty_->setVisible(visible == 0);
}

void TagFilterButton::toggleRow(int row) {
    if (row < 0 || row >= list_->count() || list_->item(row)->isHidden()) return;
    QStringList next = selected_;
    const QString name = list_->item(row)->text();
    if (next.contains(name, Qt::CaseInsensitive)) {
        for (int i = next.size() - 1; i >= 0; --i)
            if (next[i].compare(name, Qt::CaseInsensitive) == 0) next.removeAt(i);
    } else next.append(name);
    setSelectedTags(next);
}

void TagFilterButton::positionPopup() {
    const QPoint below = mapToGlobal(QPoint(width(), height() + 6));
    QScreen* screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect available = screen->availableGeometry().adjusted(4, 4, -4, -4);
    const int width = std::min(available.width(), std::max(280, std::min(this->width(), 360)));
    const int height = std::min(available.height(), 106 + std::min(8, std::max(2, tags_.size())) * 38);
    popup_->resize(width, height);
    int y = below.y();
    if (y + height > available.bottom() + 1) y = mapToGlobal(QPoint(0, -6)).y() - height;
    const int x = std::max(available.left(), std::min(below.x() - width, available.right() - width + 1));
    y = std::max(available.top(), std::min(y, available.bottom() - height + 1));
    popup_->move(x, y);
}

void TagFilterButton::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        showPopup();
        event->accept();
        return;
    }
    QPushButton::keyPressEvent(event);
}

void TagFilterButton::hideEvent(QHideEvent* event) {
    if (popup_) popup_->hide();
    QPushButton::hideEvent(event);
}

bool TagFilterButton::eventFilter(QObject* watched, QEvent* event) {
    if (watched == popup_ && event->type() == QEvent::Hide) update();
    if (event->type() == QEvent::KeyPress && popup_ && popup_->isVisible()) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            popup_->hide();
            setFocus(Qt::PopupFocusReason);
            return true;
        }
        if (watched == search_ && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            const bool down = key->key() == Qt::Key_Down;
            for (int n = 0; n < list_->count(); ++n) {
                const int row = down ? n : list_->count() - n - 1;
                if (!list_->item(row)->isHidden()) {
                    list_->setCurrentRow(row);
                    list_->setFocus(Qt::TabFocusReason);
                    break;
                }
            }
            return true;
        }
        if (watched == list_) {
            if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                toggleRow(list_->currentRow());
                return true;
            }
            if (key->key() == Qt::Key_Backspace || (!key->text().isEmpty() && key->text().at(0).isPrint()
                && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))) {
                search_->setFocus(Qt::OtherFocusReason);
                QApplication::sendEvent(search_, key);
                return true;
            }
        }
    }
    return QPushButton::eventFilter(watched, event);
}

} // namespace videovault::app
