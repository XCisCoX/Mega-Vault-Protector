#include "videovault/app/music_library.hpp"

#include "videovault/app/icons.hpp"
#include "videovault/app/music_window.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace videovault::app {
namespace {

// One place that decides what counts as music. The gallery's own list lives in
// main_window.cpp, so this stays a copy of the rule rather than a second truth:
// the file name is the only signal the vault stores for an imported item.
bool isMusicName(const QString& name) {
    static const QStringList kAudio{
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("wav"),
        QStringLiteral("wave"), QStringLiteral("ogg"), QStringLiteral("oga"),
        QStringLiteral("opus"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("wma"), QStringLiteral("aiff"), QStringLiteral("aif"),
        QStringLiteral("alac"), QStringLiteral("mp2"), QStringLiteral("mka")};
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) {
        return false;
    }
    return kAudio.contains(name.mid(dot + 1).toLower());
}

QString withoutExtension(const QString& name) {
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? name.left(dot) : name;
}

QString compactSize(const qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = bytes / 1024.0;
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', kb < 10 ? 1 : 0);
    }
    const double mb = kb / 1024.0;
    if (bytes < static_cast<qint64>(1024) * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
    }
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

// A note, drawn rather than shipped as a bitmap so it follows the palette.
void paintNote(QPainter& painter, const QRectF& box, const QColor& colour) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    const qreal headW = box.width() * 0.34;
    const qreal headH = headW * 0.78;
    const QPointF head(box.left() + headW * 0.55, box.bottom() - headH * 0.6);
    painter.drawEllipse(QRectF(head.x() - headW / 2.0, head.y() - headH / 2.0, headW, headH));
    const qreal stemX = head.x() + headW * 0.42;
    painter.setPen(QPen(colour, std::max(1.4, box.width() * 0.085), Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(stemX, head.y() - headH * 0.2),
        QPointF(stemX, box.top() + box.height() * 0.12));
    QPainterPath flag;
    flag.moveTo(stemX, box.top() + box.height() * 0.12);
    flag.cubicTo(stemX + box.width() * 0.30, box.top() + box.height() * 0.18,
        stemX + box.width() * 0.30, box.top() + box.height() * 0.38,
        stemX + box.width() * 0.08, box.top() + box.height() * 0.42);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colour, std::max(1.4, box.width() * 0.085), Qt::SolidLine,
        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(flag);
    painter.restore();
}

// One row: cover tile, title, "artist · size", and a play affordance on hover.
class TrackRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, 60);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect row = option.rect.adjusted(2, 2, -2, -2);
        const QPalette& palette = option.palette;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (selected || hovered) {
            QColor fill = isDarkTheme() ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 16);
            if (selected) {
                fill = palette.color(QPalette::Highlight);
                fill.setAlpha(70);
            }
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(row, 12, 12);
        }

        const QRectF tile(row.left() + 8, row.center().y() - 21.0, 42, 42);
        QColor tint = palette.color(QPalette::Highlight);
        tint.setAlpha(isDarkTheme() ? 46 : 34);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint);
        painter->drawRoundedRect(tile, 12, 12);
        paintNote(*painter, tile.adjusted(9, 9, -9, -9), palette.color(QPalette::WindowText));

        const QString title = index.data(Qt::UserRole + 1).toString();
        const QString detail = index.data(Qt::UserRole + 2).toString();
        const int textLeft = static_cast<int>(tile.right()) + 14;
        const int textWidth = std::max(40, row.right() - textLeft - 16);
        QFont titleFont(QStringLiteral("Segoe UI"));
        titleFont.setPixelSize(14);
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(selected ? palette.color(QPalette::HighlightedText)
                                 : palette.color(QPalette::WindowText));
        painter->drawText(QRect(textLeft, row.top() + 10, textWidth, 20),
            Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(title, Qt::ElideRight, textWidth));

        QFont detailFont(QStringLiteral("Segoe UI"));
        detailFont.setPixelSize(11);
        painter->setFont(detailFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(QRect(textLeft, row.top() + 31, textWidth, 18),
            Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(detail, Qt::ElideRight, textWidth));
        painter->restore();
    }
};

} // namespace

MusicLibraryPage::MusicLibraryPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("musicLibraryPage"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("musicLibraryHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    headerLayout->setSpacing(12);
    auto* back = new QPushButton(QStringLiteral("‹ Library"), header);
    back->setProperty("quiet", true);
    back->setCursor(Qt::PointingHandCursor);
    back->setFocusPolicy(Qt::NoFocus);
    auto* title = new QLabel(QStringLiteral("Music"), header);
    QFont titleFont(QStringLiteral("Segoe UI"));
    titleFont.setPointSize(17);
    titleFont.setBold(true);
    title->setFont(titleFont);
    count_ = new QLabel(header);
    search_ = new QLineEdit(header);
    search_->setObjectName(QStringLiteral("musicSearchField"));
    search_->setPlaceholderText(QStringLiteral("Search your music"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(220);
    search_->setMaximumWidth(340);
    provider_ = new QPushButton(QStringLiteral("Monochrome"), header);
    provider_->setObjectName(QStringLiteral("musicProviderButton"));
    icons::withText(provider_, QStringLiteral("wifi"));
    provider_->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(highlight); background: transparent; border: none;"
        " padding: 4px 10px; font-weight: 600; }"
        "QPushButton:hover { background: palette(alternate-base); border-radius: 12px; }"));
    provider_->setCursor(Qt::PointingHandCursor);
    provider_->setFocusPolicy(Qt::NoFocus);
    provider_->setToolTip(QStringLiteral("Search the installed music provider"));
    provider_->hide();
    playAll_ = new QPushButton(QStringLiteral("Play all"), header);
    playAll_->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(highlight); background: transparent; border: none;"
        " padding: 4px 10px; font-weight: 600; }"
        "QPushButton:hover { color: palette(highlight); background: palette(alternate-base);"
        " border-radius: 12px; }"));
    playAll_->setCursor(Qt::PointingHandCursor);
    playAll_->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(back);
    headerLayout->addWidget(title);
    headerLayout->addWidget(count_);
    headerLayout->addStretch(1);
    headerLayout->addWidget(search_);
    headerLayout->addWidget(provider_);
    headerLayout->addWidget(playAll_);
    root->addWidget(header);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("musicTracks"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    list_->setMouseTracking(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setItemDelegate(new TrackRowDelegate(list_));
    list_->setStyleSheet(QStringLiteral("QListWidget#musicTracks { outline: none; }"));
    root->addWidget(list_, 1);

    empty_ = new QLabel(this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setText(QStringLiteral(
        "No music yet.\nImport MP3, FLAC, WAV, M4A or OGG files and they appear here."));
    empty_->hide();
    root->addWidget(empty_, 1);

    connect(back, &QPushButton::clicked, this, &MusicLibraryPage::closed);
    connect(search_, &QLineEdit::textChanged, this,
        [this](const QString& text) { applyFilter(text); });
    connect(playAll_, &QPushButton::clicked, this, [this] { playRow(0); });
    connect(provider_, &QPushButton::clicked, this, &MusicLibraryPage::providerRequested);
    connect(list_, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) { playRow(list_->row(item)); });
    connect(list_, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item) { playRow(list_->row(item)); });

    auto refreshPalette = [this] {
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor secondary = palette().color(QPalette::PlaceholderText);
        const QColor hairline = palette().color(QPalette::Midlight);
        QColor line = hairline;
        line.setAlpha(isDarkTheme() ? 90 : 150);
        setStyleSheet(QStringLiteral(
            "QWidget#musicLibraryPage { background: %1; }"
            "QWidget#musicLibraryHeader { background: %2; border-bottom: 1px solid %3; }"
            "QLabel { color: %4; background: transparent; }")
            .arg(palette().color(QPalette::Window).name(),
                 palette().color(QPalette::Base).name(), line.name(), ink.name()));
        count_->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
            .arg(secondary.name()));
        empty_->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; background: transparent;")
            .arg(secondary.name()));
        search_->setStyleSheet(QStringLiteral(
            "QLineEdit#musicSearchField { color: palette(window-text);"
            " background: palette(base); border: 1px solid palette(midlight);"
            " border-radius: 14px; padding: 6px 12px; min-height: 24px; }"
            "QLineEdit#musicSearchField:focus { border-color: palette(highlight); }"));
        list_->setStyleSheet(QStringLiteral(
            "QListWidget#musicTracks { background: transparent; border: none; outline: none; }"
            "QListWidget#musicTracks::item { border: none; }"));
        list_->viewport()->update();
    };
    refreshPalette();
    connect(qApp, &QApplication::paletteChanged, this, [refreshPalette](const QPalette&) {
        refreshPalette();
    });
}

void MusicLibraryPage::setVault(const std::shared_ptr<videovault::core::Vault>& vault) {
    vault_ = vault;
    rebuild();
}

void MusicLibraryPage::setProviderAvailable(const bool available) {
    if (provider_ != nullptr) {
        provider_->setVisible(available);
    }
}

void MusicLibraryPage::refresh() {
    rebuild();
}

void MusicLibraryPage::rebuild() {
    tracks_.clear();
    if (vault_ != nullptr && vault_->is_unlocked()) {
        if (const auto listed = vault_->list_videos(); listed) {
            for (const auto& video : listed.value()) {
                const QString name = QString::fromStdString(video.display_name);
                if (!isMusicName(name)) {
                    continue;
                }
                Track track;
                track.id = video.id;
                track.size = static_cast<qint64>(video.original_size);
                // "Artist - Title.ext" is the convention a music file carries;
                // anything else keeps its name and an empty artist line.
                QString stem = withoutExtension(name).trimmed();
                const int dash = stem.indexOf(QStringLiteral(" - "));
                if (dash > 0) {
                    track.artist = stem.left(dash).trimmed();
                    track.title = stem.mid(dash + 3).trimmed();
                } else {
                    track.title = stem;
                }
                tracks_.push_back(std::move(track));
            }
        }
    }
    std::sort(tracks_.begin(), tracks_.end(), [](const Track& left, const Track& right) {
        const int byTitle = QString::compare(left.title, right.title, Qt::CaseInsensitive);
        return byTitle != 0 ? byTitle < 0 : left.id < right.id;
    });
    applyFilter(search_ != nullptr ? search_->text() : QString());
}

void MusicLibraryPage::applyFilter(const QString& text) {
    shown_.clear();
    const QString needle = text.trimmed();
    for (const auto& track : tracks_) {
        if (needle.isEmpty() || track.title.contains(needle, Qt::CaseInsensitive)
            || track.artist.contains(needle, Qt::CaseInsensitive)) {
            shown_.push_back(track);
        }
    }
    list_->clear();
    for (const auto& track : shown_) {
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(track.id));
        item->setData(Qt::UserRole + 1, track.title);
        item->setData(Qt::UserRole + 2, track.artist.isEmpty()
            ? compactSize(track.size)
            : QStringLiteral("%1 · %2").arg(track.artist, compactSize(track.size)));
        item->setToolTip(track.title);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
    const bool any = !shown_.empty();
    list_->setVisible(any);
    empty_->setVisible(!any);
    if (count_ != nullptr) {
        count_->setText(tracks_.empty() ? QString()
            : QStringLiteral("%1 track%2").arg(tracks_.size())
                  .arg(tracks_.size() == 1 ? QString() : QStringLiteral("s")));
    }
    if (playAll_ != nullptr) {
        playAll_->setEnabled(any);
    }
    if (!any) {
        empty_->setText(tracks_.empty()
            ? QStringLiteral("No music yet.\nImport MP3, FLAC, WAV, M4A or OGG files "
                             "and they appear here.")
            : QStringLiteral("Nothing matches that search."));
    }
}

std::vector<MusicLibraryPage::Track> MusicLibraryPage::playableOrder() const {
    return shown_;
}

void MusicLibraryPage::ensurePlayer() {
    if (player_ != nullptr) {
        return;
    }
    player_ = new MusicWindow(this);
    player_->setVisible(false);
    // At the bottom of the page, exactly where the library's own bar sits.
    if (auto* column = qobject_cast<QVBoxLayout*>(layout()); column != nullptr) {
        column->addWidget(player_);
    }
}

void MusicLibraryPage::playRow(const int row) {
    if (vault_ == nullptr || !vault_->is_unlocked() || shown_.empty()) {
        return;
    }
    const int index = std::clamp(row, 0, static_cast<int>(shown_.size()) - 1);
    std::vector<MusicTrack> queue;
    queue.reserve(shown_.size());
    for (const auto& track : shown_) {
        queue.push_back(MusicTrack{track.id, track.title});
    }
    ensurePlayer();
    player_->playQueue(vault_, queue, index);
    player_->setVisible(true);
    if (list_ != nullptr) {
        list_->setCurrentRow(index);
    }
}

} // namespace videovault::app
