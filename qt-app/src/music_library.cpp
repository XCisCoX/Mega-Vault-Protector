#include "videovault/app/music_library.hpp"

#include "videovault/app/icons.hpp"
#include "videovault/app/in_window_sheet.hpp"
#include "videovault/app/music_window.hpp"
#include "videovault/app/playlist_model.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QComboBox>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QThread>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QListWidget>
#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>

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
        const auto cover = index.data(Qt::DecorationRole).value<QIcon>();
        if (!cover.isNull() && !cover.pixmap(42, 42).isNull()) {
            // Album art when the vault has a decrypted picture for this item;
            // the drawn note stays as the fallback cover.
            QPainterPath clip;
            clip.addRoundedRect(tile, 10, 10);
            painter->save();
            painter->setClipPath(clip);
            painter->drawPixmap(tile.toRect(), cover.pixmap(tile.size().toSize()));
            painter->restore();
        } else {
            QColor tint = palette.color(QPalette::Highlight);
            tint.setAlpha(isDarkTheme() ? 46 : 34);
            painter->setPen(Qt::NoPen);
            painter->setBrush(tint);
            painter->drawRoundedRect(tile, 12, 12);
            paintNote(*painter, tile.adjusted(9, 9, -9, -9), palette.color(QPalette::WindowText));
        }

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
        if (option.state.testFlag(QStyle::State_HasFocus)) {
            painter->setPen(QPen(palette.color(QPalette::Highlight), 1.5, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(row.adjusted(1, 1, -1, -1), 10, 10);
        }
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
    auto* back = new QPushButton(QStringLiteral("Library"), header);
    icons::withText(back, QStringLiteral("arrow-left"));
    back->setProperty("quiet", true);
    back->setCursor(Qt::PointingHandCursor);

    auto* title = new QLabel(QStringLiteral("Music"), header);
    QFont titleFont(QStringLiteral("Segoe UI"));
    titleFont.setPointSize(17);
    titleFont.setBold(true);
    title->setFont(titleFont);
    count_ = new QLabel(header);
    count_->setTextFormat(Qt::PlainText);
    search_ = new QLineEdit(header);
    search_->setObjectName(QStringLiteral("musicSearchField"));
    search_->setPlaceholderText(QStringLiteral("Search your music"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(100);
    search_->setMaximumWidth(480);
    section_ = new QComboBox(header);
    section_->setObjectName(QStringLiteral("musicSectionSelect"));
    section_->addItem(QStringLiteral("Songs"));
    section_->addItem(QStringLiteral("Artists"));
    section_->addItem(QStringLiteral("Recently added"));
    section_->addItem(QStringLiteral("Offline music"));
    section_->addItem(QStringLiteral("Albums"));
    section_->addItem(QStringLiteral("Playlists"));
    section_->setToolTip(QStringLiteral("Choose what this list shows"));
    group_ = new QComboBox(this);
    group_->setObjectName(QStringLiteral("musicGroupSelect"));
    group_->setMinimumContentsLength(18);
    group_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    group_->hide();

    playAll_ = new QPushButton(QStringLiteral("Play all"), header);
    playAll_->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(highlight); background: transparent; border: none;"
        " padding: 4px 10px; font-weight: 600; }"
        "QPushButton:hover { color: palette(highlight); background: palette(alternate-base);"
        " border-radius: 12px; }"));
    playAll_->setCursor(Qt::PointingHandCursor);

    headerLayout->addWidget(back);
    headerLayout->addWidget(title);
    headerLayout->addWidget(count_);
    headerLayout->addStretch(1);

    headerLayout->addWidget(playAll_);
    root->addWidget(header);
    auto* filters = new QWidget(this);
    auto* filterLayout = new QHBoxLayout(filters);
    filterLayout->setContentsMargins(16, 8, 16, 8);
    playlistBack_ = new QPushButton(QStringLiteral("Playlists"), filters);
    icons::withText(playlistBack_, QStringLiteral("arrow-left"));
    playlistBack_->setObjectName(QStringLiteral("musicPlaylistBack"));
    playlistBack_->setCursor(Qt::PointingHandCursor);

    playlistBack_->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(highlight); background: transparent; border: none;"
        " padding: 4px 8px; font-weight: 600; }"
        "QPushButton:hover { background: palette(alternate-base); border-radius: 12px; }"));
    playlistBack_->hide();
    connect(playlistBack_, &QPushButton::clicked, this, [this] {
        openPlaylistId_ = 0;
        search_->clear();
        applyFilter(QString());
    });
    filterLayout->addWidget(section_);
    filterLayout->addWidget(search_, 1);
    filterLayout->addWidget(playlistBack_);
    root->addWidget(filters);
    root->addWidget(group_);

    auto* actions = new QWidget(this);
    auto* actionLayout = new QGridLayout(actions);
    actionLayout->setContentsMargins(16, 4, 16, 8);
    auto button = [actions](const QString& text, const QString& name, const QString& icon) {
        auto* result = new QPushButton(text, actions);
        result->setObjectName(name);
        result->setCursor(Qt::PointingHandCursor);
        icons::withText(result, icon);
        return result;
    };
    newPlaylist_ = button(QStringLiteral("New playlist"), QStringLiteral("musicNewPlaylist"), QStringLiteral("plus"));
    openPlaylist_ = button(QStringLiteral("Open"), QStringLiteral("musicOpenPlaylist"), QStringLiteral("folder-open"));
    renamePlaylist_ = button(QStringLiteral("Rename…"), QStringLiteral("musicRenamePlaylist"), QStringLiteral("pencil"));
    deletePlaylist_ = button(QStringLiteral("Delete playlist…"), QStringLiteral("musicDeletePlaylist"), QStringLiteral("trash"));
    addTracks_ = button(QStringLiteral("Add tracks…"), QStringLiteral("musicAddTracks"), QStringLiteral("plus"));
    removeTrack_ = button(QStringLiteral("Remove track"), QStringLiteral("musicRemoveTrack"), QStringLiteral("minus"));
    moveUp_ = button(QStringLiteral("Move up"), QStringLiteral("musicMoveUp"), QStringLiteral("arrow-up"));
    moveDown_ = button(QStringLiteral("Move down"), QStringLiteral("musicMoveDown"), QStringLiteral("arrow-down"));
    playlistTitle_ = new QLabel(actions);
    playlistTitle_->setTextFormat(Qt::PlainText);
    playlistTitle_->setWordWrap(true);
    actionLayout->addWidget(playlistTitle_, 0, 0, 1, 4);
    actionLayout->addWidget(newPlaylist_, 1, 0);
    actionLayout->addWidget(openPlaylist_, 1, 1);
    actionLayout->addWidget(renamePlaylist_, 1, 2);
    actionLayout->addWidget(deletePlaylist_, 1, 3);
    actionLayout->addWidget(addTracks_, 2, 0);
    actionLayout->addWidget(removeTrack_, 2, 1);
    actionLayout->addWidget(moveUp_, 2, 2);
    actionLayout->addWidget(moveDown_, 2, 3);
    root->addWidget(actions);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("musicPlaylistStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setContentsMargins(16, 4, 16, 8);
    status_->hide();
    root->addWidget(status_);
    connect(newPlaylist_, &QPushButton::clicked, this, [this] { createPlaylist(); });
    connect(openPlaylist_, &QPushButton::clicked, this, [this] { playRow(list_->currentRow()); });
    const auto selectedPlaylist = [this]() -> const Playlist* {
        const auto* item = list_->currentItem();
        return playlistById(openPlaylistId_ != 0 ? openPlaylistId_
            : item != nullptr ? item->data(Qt::UserRole).toLongLong() : 0);
    };
    connect(renamePlaylist_, &QPushButton::clicked, this, [this, selectedPlaylist] {
        if (const auto* playlist = selectedPlaylist()) renamePlaylist(*playlist);
    });
    connect(deletePlaylist_, &QPushButton::clicked, this, [this, selectedPlaylist] {
        if (const auto* playlist = selectedPlaylist()) deletePlaylist(*playlist);
    });
    connect(addTracks_, &QPushButton::clicked, this, &MusicLibraryPage::addPlaylistTracks);
    connect(removeTrack_, &QPushButton::clicked, this, [this] { editSelectedTrack(0); });
    connect(moveUp_, &QPushButton::clicked, this, [this] { editSelectedTrack(-1); });
    connect(moveDown_, &QPushButton::clicked, this, [this] { editSelectedTrack(1); });

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("musicTracks"));
    list_->setAccessibleName(QStringLiteral("Music and playlists"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    list_->setMouseTracking(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setItemDelegate(new TrackRowDelegate(list_));
    list_->setStyleSheet(QStringLiteral("QListWidget#musicTracks { outline: none; }"));
    root->addWidget(list_, 1);
    connect(list_, &QListWidget::itemSelectionChanged, this, &MusicLibraryPage::updateActions);
    connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this,
        [this] { startCoverLoads(); });

    empty_ = new QLabel(this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setText(QStringLiteral(
        "No music yet.\nImport MP3, FLAC, WAV, M4A or OGG files and they appear here."));
    empty_->hide();
    root->addWidget(empty_, 1);

    connect(back, &QPushButton::clicked, this, &MusicLibraryPage::closed);
    connect(section_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int index) {
            currentSection_ = index;
            openPlaylistId_ = 0;
            rebuildGroups();
            applyFilter(search_ != nullptr ? search_->text() : QString());
        });
    connect(group_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this] { applyFilter(search_->text()); });
    connect(search_, &QLineEdit::textChanged, this,
        [this](const QString& text) { applyFilter(text); });
    connect(playAll_, &QPushButton::clicked, this, [this] { playRow(0); });

    connect(list_, &QListWidget::itemActivated, this,
        [this](QListWidgetItem* item) { playRow(list_->row(item)); });
    connect(list_, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item) {
            // Playlist selection is non-destructive; Open/double-click enters it.
            if (currentSection_ != PlaylistsSection) playRow(list_->row(item));
        });
    // Row actions: queue management belongs next to the list, not in a dialog.
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = list_->itemAt(point);
        if (item == nullptr) {
            return;
        }
        const int row = list_->row(item);
        const auto generation = vaultGeneration_;
        QMenu menu(list_);
        // In the playlist list the row is a playlist, so the actions are the
        // playlist's own; anywhere else they are the track's.
        if (currentSection_ == PlaylistsSection && openPlaylistId_ == 0) {
            const auto* chosenPlaylist = playlistById(std::as_const(*item).data(Qt::UserRole).toLongLong());
            if (chosenPlaylist == nullptr) {
                return;
            }
            const auto playlist = *chosenPlaylist;
            auto* open = menu.addAction(icons::lucide(QStringLiteral("folder-open")), QStringLiteral("Open"));
            auto* rename = menu.addAction(icons::lucide(QStringLiteral("pencil")), QStringLiteral("Rename…"));
            auto* remove = menu.addAction(icons::lucide(QStringLiteral("trash")), QStringLiteral("Delete playlist…"));
            rename->setEnabled(!playlistEditBusy_);
            remove->setEnabled(!playlistEditBusy_);
            auto* picked = menu.exec(list_->mapToGlobal(point));
            if (generation != vaultGeneration_) return;
            if (picked == open) {
                openPlaylist(playlist.id);
            } else if (picked == rename) {
                renamePlaylist(playlist);
            } else if (picked == remove) {
                deletePlaylist(playlist);
            }
            return;
        }
        if (row < 0 || row >= static_cast<int>(shown_.size())) return;
        const auto track = shown_[static_cast<std::size_t>(row)];
        const auto playlistId = openPlaylistId_;
        MusicTrack queued;
        queued.id = track.id;
        queued.title = track.title;
        auto* play = menu.addAction(QStringLiteral("Play"));
        auto* next = menu.addAction(QStringLiteral("Play next"));
        auto* append = menu.addAction(QStringLiteral("Add to queue"));
        menu.addSeparator();
        auto* addTo = menu.addMenu(QStringLiteral("Add to playlist"));
        auto* newPlaylist = addTo->addAction(QStringLiteral("New playlist…"));
        std::map<QAction*, std::int64_t> targetPlaylists;
        for (const auto& playlist : playlists_) {
            auto label = playlist.name;
            label.replace(QStringLiteral("&"), QStringLiteral("&&"));
            targetPlaylists.emplace(addTo->addAction(icons::lucide(QStringLiteral("list")), label), playlist.id);
        }
        addTo->setEnabled(!playlistEditBusy_);
        QAction* removeFrom = nullptr;
        if (playlistId != 0) removeFrom = menu.addAction(icons::lucide(QStringLiteral("minus")), QStringLiteral("Remove from this playlist"));
        if (removeFrom) removeFrom->setEnabled(!playlistEditBusy_);
        menu.addSeparator();
        auto* reveal = menu.addAction(QStringLiteral("Details"));
        auto* chosen = menu.exec(list_->mapToGlobal(point));
        if (chosen == nullptr || generation != vaultGeneration_) {
            return;
        }
        if (chosen == play) {
            const auto found = std::find_if(shown_.begin(), shown_.end(), [&track](const Track& current) { return current.id == track.id; });
            if (found != shown_.end()) playRow(static_cast<int>(found - shown_.begin()));
            return;
        }
        if (chosen == newPlaylist) {
            createPlaylist(track.id);
            return;
        }
        if (chosen == removeFrom) {
            if (const auto* playlist = playlistById(playlistId)) {
                auto ids = playlist->trackIds;
                playlist_model::removeTrack(ids, track.id);
                changePlaylistTracks(playlistId, std::move(ids));
            }
            return;
        }
        if (const auto target = targetPlaylists.find(chosen); target != targetPlaylists.end()) {
            if (const auto* playlist = playlistById(target->second)) {
                auto ids = playlist->trackIds;
                if (!playlist_model::appendTrack(ids, track.id)) return;
                changePlaylistTracks(playlist->id, std::move(ids));
            }
            return;
        }
        if (chosen == next || chosen == append) {
            ensurePlayer();
            if (!player_ || !vault_ || !vault_->is_unlocked()) return;
            if (player_->queueSize() == 0) {
                player_->playQueue(vault_, {queued}, 0);
            } else {
                player_->enqueue({queued}, chosen == next);
            }
        } else if (chosen == reveal) {
            const auto detail = QStringLiteral("%1\n%2\n%3")
                                    .arg(track.title,
                                        track.artist.isEmpty() ? QStringLiteral("Unknown artist")
                                                               : track.artist,
                                        track.provider.isEmpty()
                                            ? QStringLiteral("Imported file")
                                            : QStringLiteral("Saved from %1").arg(track.provider));
            InWindowSheet sheet(this);
            sheet.setTitle(QStringLiteral("Track details"));
            sheet.setMessage(QStringLiteral("Saved track information"));
            auto* label = new QLabel(detail, &sheet);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            sheet.body()->addWidget(label);
            sheet.addButton(QStringLiteral("Close"), 0, false);
            sheet.run();
        }
        if (chosen == next || chosen == append) {
            // Reveal the injected central footer without moving it into this page.
            player_->setVisible(true);
        }
    });

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
    applyFilter(QString());
}

void MusicLibraryPage::setVault(const std::shared_ptr<videovault::core::Vault>& vault) {
    if (vault_ != vault) {
        ++vaultGeneration_;
        openPlaylistId_ = 0;
        status_->clear();
        status_->hide();
        const QSignalBlocker searchBlock(search_);
        const QSignalBlocker groupBlock(group_);
        search_->clear();
        group_->clear();
        group_->hide();
        ++coverGeneration_;
        if (coverPump_ != nullptr) {
            coverPump_->stop();
        }
        // MainWindow owns playback and lock/switch revocation. First binding
        // this page must not stop an already-playing central footer.
        list_->clear();
        tracks_.clear();
        shown_.clear();
        playlists_.clear();
        applyFilter(QString());
    }
    vault_ = vault;
    rebuild();
}

void MusicLibraryPage::setPlayer(MusicWindow* player) {
    player_ = player;
}

void MusicLibraryPage::refresh() {
    rebuild();
}

const MusicLibraryPage::Playlist* MusicLibraryPage::playlistById(const std::int64_t id) const {
    for (const auto& playlist : playlists_) {
        if (playlist.id == id) {
            return &playlist;
        }
    }
    return nullptr;
}

void MusicLibraryPage::openPlaylist(const std::int64_t id) {
    if (!playlistById(id)) return;
    openPlaylistId_ = id;
    currentSection_ = PlaylistsSection;
    const QSignalBlocker blocker(section_);
    section_->setCurrentIndex(PlaylistsSection);
    rebuildGroups();
    search_->clear();
    applyFilter(QString());
}

void MusicLibraryPage::createPlaylist(const std::int64_t initialTrack) {
    if (playlistEditBusy_ || !vault_ || !vault_->is_unlocked()) return;
    const auto generation = vaultGeneration_;
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("New playlist"));
    sheet.setMessage(QStringLiteral("Give the playlist a name. It is stored inside the encrypted vault."));
    sheet.addTextEntry(QLineEdit::Normal, QString(), QStringLiteral("Playlist name"));
    sheet.addButton(QStringLiteral("Create"), 1, true, QStringLiteral("plus"));
    sheet.addButton(QStringLiteral("Cancel"), 0, false);
    if (sheet.run() != 1 || generation != vaultGeneration_ || !vault_ || !vault_->is_unlocked()) return;
    const auto name = sheet.text().trimmed();
    if (name.isEmpty()) {
        showError(QStringLiteral("Playlist name cannot be empty. Choose New playlist and enter a name."));
        return;
    }
    const auto created = vault_->create_music_playlist(name.toStdString());
    if (!created) {
        showError(QStringLiteral("Could not create playlist: %1").arg(QString::fromStdString(created.error().technical_detail)));
        return;
    }
    rebuild();
    openPlaylist(created.value());
    if (initialTrack > 0) changePlaylistTracks(created.value(), {initialTrack});
}

void MusicLibraryPage::renamePlaylist(Playlist playlist) {
    if (playlistEditBusy_ || !vault_ || !vault_->is_unlocked()) return;
    const auto generation = vaultGeneration_;
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("Rename playlist"));
    sheet.setMessage(QStringLiteral("Enter a new playlist name."));
    sheet.addTextEntry(QLineEdit::Normal, playlist.name, QStringLiteral("Playlist name"));
    sheet.addButton(QStringLiteral("Save"), 1, true, QStringLiteral("pencil"));
    sheet.addButton(QStringLiteral("Cancel"), 0, false);
    if (sheet.run() != 1 || generation != vaultGeneration_) return;
    const auto name = sheet.text().trimmed();
    if (name.isEmpty()) {
        showError(QStringLiteral("Playlist name cannot be empty. Choose Rename and enter a name."));
        return;
    }
    auto vault = vault_;
    runPlaylistEdit([vault, id = playlist.id, name] {
        const auto result = vault->rename_music_playlist(id, name.toStdString());
        if (!result) return QString::fromStdString(result.error().technical_detail);
        const auto listed = vault->list_music_playlists();
        if (!listed) return QString::fromStdString(listed.error().technical_detail);
        for (const auto& saved : listed.value()) if (saved.id == id && saved.name == name.toStdString()) return QString();
        return QStringLiteral("The renamed playlist could not be verified. Refresh and try again.");
    });
}

void MusicLibraryPage::deletePlaylist(Playlist playlist) {
    if (playlistEditBusy_ || !vault_ || !vault_->is_unlocked()) return;
    const auto generation = vaultGeneration_;
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("Delete playlist"));
    // Do not interpret the user's name as rich text in the confirmation sheet.
    sheet.setMessage(QStringLiteral("Delete this playlist? The encrypted tracks stay in your library; only the playlist is removed."));
    auto* name = new QLabel(playlist.name, &sheet);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    sheet.body()->addWidget(name);
    sheet.addButton(QStringLiteral("Delete playlist"), 1, true, QStringLiteral("trash"));
    sheet.addButton(QStringLiteral("Cancel"), 0, false);
    if (sheet.run() != 1 || generation != vaultGeneration_) return;
    auto vault = vault_;
    runPlaylistEdit([vault, id = playlist.id] {
        const auto result = vault->delete_music_playlist(id);
        if (!result) return QString::fromStdString(result.error().technical_detail);
        const auto listed = vault->list_music_playlists();
        if (!listed) return QString::fromStdString(listed.error().technical_detail);
        for (const auto& saved : listed.value()) if (saved.id == id) return QStringLiteral("The playlist was not deleted. Refresh and try again.");
        return QString();
    });
}

void MusicLibraryPage::changePlaylistTracks(const std::int64_t id, std::vector<std::int64_t> ids) {
    if (playlistEditBusy_ || !vault_ || !vault_->is_unlocked()) return;
    auto vault = vault_;
    runPlaylistEdit([vault, id, ids = std::move(ids)] {
        // Core authenticates and probes every package: never do this on the GUI thread.
        const auto result = vault->set_music_playlist_tracks(id, ids);
        if (!result) return QString::fromStdString(result.error().technical_detail);
        const auto listed = vault->list_music_playlists();
        if (!listed) return QString::fromStdString(listed.error().technical_detail);
        for (const auto& playlist : listed.value()) {
            if (playlist.id == id && playlist.track_ids == ids) return QString();
        }
        return QStringLiteral("The saved playlist did not match the requested order. Refresh and try again.");
    });
}

void MusicLibraryPage::showError(const QString& message) {
    status_->setText(QStringLiteral("Error: %1").arg(message));
    status_->show();
}

void MusicLibraryPage::runPlaylistEdit(std::function<QString()> work) {
    if (playlistEditBusy_ || !vault_) return;
    playlistEditBusy_ = true;
    const auto generation = vaultGeneration_;
    ++coverGeneration_;
    if (coverPump_) coverPump_->stop();
    status_->setText(QStringLiteral("Saving playlist…"));
    status_->show();
    updateActions();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation] {
        const auto error = watcher->result();
        watcher->deleteLater();
        playlistEditBusy_ = false;
        rebuild();
        if (generation != vaultGeneration_) return;
        if (!error.isEmpty()) showError(QStringLiteral("Could not save playlist: %1").arg(error));
        else {
            status_->setText(QStringLiteral("Playlist saved."));
            status_->show();
        }
    });
    const auto vault = vault_;
    const auto lockGeneration = vault->lock_generation();
    watcher->setFuture(QtConcurrent::run([work = std::move(work), vault, lockGeneration] {
        if (vault->lock_generation() != lockGeneration) return QStringLiteral("The vault session changed. Try again after unlocking.");
        try { return work(); }
        catch (const std::exception&) { return QStringLiteral("Unexpected playlist failure. Refresh and try again."); }
    }));
}

void MusicLibraryPage::addPlaylistTracks() {
    if (playlistEditBusy_) return;
    const auto* playlist = playlistById(openPlaylistId_);
    if (!playlist) return;
    const auto id = playlist->id;
    auto ids = playlist->trackIds;
    const auto generation = vaultGeneration_;
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("Add tracks"));
    sheet.setMessage(QStringLiteral("Choose tracks to append. Existing playlist order is kept."));
    auto* filter = new QLineEdit(&sheet);
    filter->setPlaceholderText(QStringLiteral("Search available tracks"));
    sheet.body()->addWidget(filter);
    auto* choices = new QListWidget(&sheet);
    choices->setAccessibleName(QStringLiteral("Tracks to add"));
    choices->setMinimumHeight(180);
    choices->setMaximumHeight(320);
    for (const auto& track : tracks_) {
        if (std::find(ids.begin(), ids.end(), track.id) != ids.end()) continue;
        auto* item = new QListWidgetItem(track.artist.isEmpty() ? track.title
            : track.title + QStringLiteral(" — ") + track.artist, choices);
        item->setData(Qt::UserRole, static_cast<qlonglong>(track.id));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        item->setCheckState(Qt::Unchecked);
    }
    sheet.body()->addWidget(choices);
    connect(filter, &QLineEdit::textChanged, choices, [choices](const QString& text) {
        for (int i = 0; i < choices->count(); ++i)
            choices->item(i)->setHidden(!choices->item(i)->text().contains(text.trimmed(), Qt::CaseInsensitive));
    });
    auto* add = sheet.addButton(QStringLiteral("Add selected"), 1, true, QStringLiteral("plus"));
    add->setEnabled(false);
    connect(choices, &QListWidget::itemChanged, add, [choices, add] {
        bool checked = false;
        for (int i = 0; i < choices->count(); ++i) checked |= choices->item(i)->checkState() == Qt::Checked;
        add->setEnabled(checked);
    });
    if (!choices->count()) sheet.setMessage(QStringLiteral("All library tracks are already in this playlist, or the library is empty."));
    sheet.addButton(QStringLiteral("Cancel"), 0, false);
    if (sheet.run() != 1 || generation != vaultGeneration_) return;
    for (int i = 0; i < choices->count(); ++i) {
        if (choices->item(i)->checkState() == Qt::Checked) ids.push_back(choices->item(i)->data(Qt::UserRole).toLongLong());
    }
    changePlaylistTracks(id, std::move(ids));
}

void MusicLibraryPage::editSelectedTrack(const int direction) {
    const auto* playlist = playlistById(openPlaylistId_);
    const auto* item = list_->currentItem();
    if (!playlist || !item || playlistEditBusy_) return;
    auto ids = playlist->trackIds;
    const auto id = item->data(Qt::UserRole).toLongLong();
    if (direction == 0) {
        if (!playlist_model::removeTrack(ids, id)) return;
    } else {
        if (!search_->text().trimmed().isEmpty() || !playlist_model::moveTrack(ids, id, direction)) return;
    }
    changePlaylistTracks(playlist->id, std::move(ids));
}

void MusicLibraryPage::updateActions() {
    const bool playlists = currentSection_ == PlaylistsSection;
    const bool inside = playlists && openPlaylistId_ != 0;
    const bool selected = list_->currentItem() != nullptr;
    const bool available = vault_ != nullptr && !playlistEditBusy_;
    newPlaylist_->setVisible(playlists);
    newPlaylist_->setEnabled(available);
    openPlaylist_->setVisible(playlists && !inside);
    openPlaylist_->setEnabled(selected && available);
    for (auto* button : {renamePlaylist_, deletePlaylist_}) {
        button->setVisible(playlists);
        button->setEnabled(available && (inside || selected));
    }
    for (auto* button : {addTracks_, removeTrack_, moveUp_, moveDown_}) button->setVisible(inside);
    addTracks_->setEnabled(available);
    removeTrack_->setEnabled(available && selected);
    const bool reorder = available && selected && search_->text().trimmed().isEmpty();
    moveUp_->setEnabled(reorder && list_->currentRow() > 0);
    moveDown_->setEnabled(reorder && list_->currentRow() + 1 < list_->count());
    for (auto* button : {moveUp_, moveDown_}) button->setToolTip(QStringLiteral("Move in saved order. Clear search to reorder tracks."));
    const auto* playlist = playlistById(openPlaylistId_);
    playlistTitle_->setVisible(inside);
    playlistTitle_->setText(playlist ? playlist->name : QString());
    playlistBack_->setVisible(inside);
    section_->setEnabled(!playlistEditBusy_);
    search_->setEnabled(!playlistEditBusy_);
    group_->setEnabled(!playlistEditBusy_);
    list_->setEnabled(!playlistEditBusy_);
    playlistBack_->setEnabled(!playlistEditBusy_);
    playAll_->setEnabled(available && !shown_.empty() && (!playlists || inside));
}

void MusicLibraryPage::rebuild() {
    if (playlistEditBusy_) return;
    tracks_.clear();
    playlists_.clear();
    if (vault_ != nullptr && vault_->is_unlocked()) {
        if (const auto listed = vault_->list_music_playlists(); listed) {
            for (const auto& entry : listed.value()) {
                Playlist playlist;
                playlist.id = entry.id;
                playlist.name = QString::fromStdString(entry.name);
                playlist.trackIds = entry.track_ids;
                playlists_.push_back(std::move(playlist));
            }
        } else {
            showError(QStringLiteral("Could not load playlists: %1").arg(QString::fromStdString(listed.error().technical_detail)));
        }
    }
    // Provider provenance is read once per rebuild: it is what turns a vault item
    // into "the offline copy of a remote track" and gives the offline section
    // something to show.
    std::map<std::int64_t, videovault::core::TrackProvenance> provenance;
    if (vault_ != nullptr && vault_->is_unlocked()) {
        if (const auto listed = vault_->list_video_provenance(); listed) {
            for (const auto& row : listed.value()) {
                provenance[row.video_id] = row;
            }
        } else {
            showError(QStringLiteral("Could not load track details: %1").arg(QString::fromStdString(listed.error().technical_detail)));
        }
    }
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
                track.added = static_cast<qint64>(video.imported_at);
                if (const auto found = provenance.find(video.id); found != provenance.end()) {
                    track.provider = QString::fromStdString(found->second.provider_id);
                    track.album = QString::fromStdString(found->second.album);
                    track.remoteId = QString::fromStdString(found->second.provider_track_id);
                    if (!found->second.title.empty()) {
                        track.title = QString::fromStdString(found->second.title);
                    }
                    if (!found->second.artist.empty()) {
                        track.artist = QString::fromStdString(found->second.artist);
                    }
                }
                // "Artist - Title.ext" is the convention a music file carries;
                // anything else keeps its name and an empty artist line.
                QString stem = withoutExtension(name).trimmed();
                const int dash = stem.indexOf(QStringLiteral(" - "));
                if (track.title.isEmpty()) {
                    track.title = dash > 0 ? stem.mid(dash + 3).trimmed() : stem;
                }
                if (track.artist.isEmpty() && dash > 0) {
                    track.artist = stem.left(dash).trimmed();
                }
                tracks_.push_back(std::move(track));
            }
        } else {
            showError(QStringLiteral("Could not load music: %1").arg(QString::fromStdString(listed.error().technical_detail)));
        }
    }
    std::sort(tracks_.begin(), tracks_.end(), [](const Track& left, const Track& right) {
        const int byTitle = QString::compare(left.title, right.title, Qt::CaseInsensitive);
        return byTitle != 0 ? byTitle < 0 : left.id < right.id;
    });
    rebuildGroups();
    applyFilter(search_ != nullptr ? search_->text() : QString());
}

void MusicLibraryPage::rebuildGroups() {
    const QSignalBlocker block(group_);
    const auto previous = group_->currentData();
    group_->clear();
    const bool albums = currentSection_ == AlbumsSection;
    const bool artists = currentSection_ == ArtistsSection;
    group_->setVisible(albums || artists);
    if (!albums && !artists) {
        return;
    }
    group_->addItem(albums ? QStringLiteral("All albums") : QStringLiteral("All artists"));
    std::map<QString, QStringList> groups;
    for (const auto& track : tracks_) {
        const auto artist = track.artist.isEmpty() ? QStringLiteral("Unknown artist") : track.artist;
        const auto album = track.album.isEmpty() ? QStringLiteral("Unknown album") : track.album;
        const auto label = albums ? album + QStringLiteral(" — ") + artist : artist;
        groups[label] = albums ? QStringList{track.artist, track.album} : QStringList{track.artist};
    }
    for (const auto& entry : groups) {
        group_->addItem(entry.first, entry.second);
    }
    const auto index = group_->findData(previous);
    group_->setCurrentIndex(index < 0 ? 0 : index);
}

void MusicLibraryPage::applyFilter(const QString& text) {
    const auto previousId = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toLongLong() : 0;
    ++coverGeneration_;
    if (coverPump_) coverPump_->stop();
    shown_.clear();
    const QString needle = text.trimmed();
    if (openPlaylistId_ && !playlistById(openPlaylistId_)) openPlaylistId_ = 0;
    const bool playlistList = currentSection_ == PlaylistsSection && openPlaylistId_ == 0;
    const auto matches = [&needle](const Track& track) {
        return needle.isEmpty() || track.title.contains(needle, Qt::CaseInsensitive)
            || track.artist.contains(needle, Qt::CaseInsensitive)
            || track.album.contains(needle, Qt::CaseInsensitive)
            || track.provider.contains(needle, Qt::CaseInsensitive);
    };
    if (const auto* playlist = playlistById(openPlaylistId_)) {
        shown_ = playlist_model::orderedTracks(tracks_, playlist->trackIds, matches);
    } else if (!playlistList) {
        for (const auto& track : tracks_) {
            if ((currentSection_ == ArtistsSection || currentSection_ == AlbumsSection) && group_->currentIndex() > 0) {
                const auto keys = group_->currentData().toStringList();
                if (keys.isEmpty() || track.artist != keys[0]
                    || (currentSection_ == AlbumsSection && (keys.size() != 2 || track.album != keys[1]))) continue;
            }
            if (matches(track)) shown_.push_back(track);
        }
        std::sort(shown_.begin(), shown_.end(), [this](const Track& left, const Track& right) {
            if (currentSection_ == RecentSection) return left.added != right.added ? left.added > right.added : left.id > right.id;
            if (currentSection_ == AlbumsSection) {
                const auto album = QString::compare(left.album, right.album, Qt::CaseInsensitive);
                if (album != 0) return album < 0;
            }
            if (currentSection_ == ArtistsSection || currentSection_ == AlbumsSection) {
                const auto artist = QString::compare(left.artist, right.artist, Qt::CaseInsensitive);
                if (artist != 0) return artist < 0;
            }
            const auto title = QString::compare(left.title, right.title, Qt::CaseInsensitive);
            return title != 0 ? title < 0 : left.id < right.id;
        });
    }
    list_->clear();
    if (playlistList) {
        for (const auto& playlist : playlists_) {
            if (!needle.isEmpty() && !playlist.name.contains(needle, Qt::CaseInsensitive)) continue;
            auto* item = new QListWidgetItem(playlist.name, list_);
            item->setIcon(icons::lucide(QStringLiteral("list")));
            item->setData(Qt::UserRole, static_cast<qlonglong>(playlist.id));
            item->setData(Qt::UserRole + 1, playlist.name);
            item->setData(Qt::UserRole + 2, QStringLiteral("%1 track%2 · Encrypted playlist")
                .arg(playlist.trackIds.size()).arg(playlist.trackIds.size() == 1 ? QString() : QStringLiteral("s")));
            item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(playlist.name.toHtmlEscaped()));
            if (playlist.id == previousId) list_->setCurrentItem(item);
        }
        count_->setText(QStringLiteral("%1 of %2 playlist%3").arg(list_->count()).arg(playlists_.size())
            .arg(playlists_.size() == 1 ? QString() : QStringLiteral("s")));
        empty_->setText(playlists_.empty()
            ? QStringLiteral("No playlists yet.\nChoose New playlist, then Add tracks to build your encrypted playlist.")
            : QStringLiteral("No playlists match that search. Clear search to see all playlists."));
    } else {
        for (const auto& track : shown_) {
            auto* item = new QListWidgetItem(track.title, list_);
            item->setData(Qt::UserRole, static_cast<qlonglong>(track.id));
            item->setData(Qt::UserRole + 1, track.title);
            QStringList detail;
            if (!track.artist.isEmpty()) detail.append(track.artist);
            if (!track.album.isEmpty() && currentSection_ == AlbumsSection) detail.append(track.album);
            if (!track.provider.isEmpty() && currentSection_ == OfflineSection) detail.append(track.provider);
            detail.append(compactSize(track.size));
            item->setData(Qt::UserRole + 2, detail.join(QStringLiteral(" · ")));
            item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(track.title.toHtmlEscaped()));
            if (track.id == previousId) list_->setCurrentItem(item);
        }
        count_->setText(QStringLiteral("%1 track%2").arg(shown_.size())
            .arg(shown_.size() == 1 ? QString() : QStringLiteral("s")));
        if (const auto* playlist = playlistById(openPlaylistId_)) {
            count_->setText(QStringLiteral("%1 of %2 track%3").arg(shown_.size()).arg(playlist->trackIds.size())
                .arg(playlist->trackIds.size() == 1 ? QString() : QStringLiteral("s")));
        }
        empty_->setText(openPlaylistId_ && needle.isEmpty()
            ? QStringLiteral("This playlist is empty.\nChoose Add tracks to build it. Your library files stay encrypted.")
            : tracks_.empty() ? QStringLiteral("No music yet.\nImport MP3, FLAC, WAV, M4A or OGG files and they appear here.")
                             : QStringLiteral("No tracks match these filters. Clear search or choose another group."));
    }
    list_->setVisible(list_->count() != 0);
    empty_->setVisible(list_->count() == 0);
    search_->setPlaceholderText(playlistList ? QStringLiteral("Search playlists") : QStringLiteral("Search tracks, artists or albums"));
    updateActions();
    if (!playlistList) startCoverLoads();
}

void MusicLibraryPage::startCoverLoads() {
    if (playlistEditBusy_ || (currentSection_ == PlaylistsSection && openPlaylistId_ == 0)) return;
    ++coverGeneration_;
    coverCursor_ = std::max(0, list_->indexAt(QPoint(4, 4)).row());
    const auto visible = list_->viewport()->rect().adjusted(0, -120, 0, 120);
    // Keep cover pixels only around the viewport, not one icon for every track
    // ever scrolled past. Metadata stays cheap; decrypted images stay bounded.
    for (int row = 0; row < list_->count(); ++row) {
        auto* item = list_->item(row);
        if (!list_->visualItemRect(item).intersects(visible)) {
            item->setData(Qt::DecorationRole, QVariant());
        }
    }
    if (vault_ == nullptr || !vault_->is_unlocked() || shown_.empty()) {
        if (coverPump_ != nullptr) {
            coverPump_->stop();
        }
        return;
    }
    if (coverPump_ == nullptr) {
        coverPump_ = new QTimer(this);
        coverPump_->setSingleShot(true);
        coverPump_->setInterval(0);
        connect(coverPump_, &QTimer::timeout, this, [this] { loadNextCover(); });
    }
    coverPump_->start();
}

void MusicLibraryPage::loadNextCover() {
    if (playlistEditBusy_ || coverBusy_ || coverCursor_ >= static_cast<int>(shown_.size()) || vault_ == nullptr
        || !vault_->is_unlocked()) {
        return;
    }
    while (coverCursor_ < list_->count()) {
        auto* item = list_->item(coverCursor_);
        if (list_->visualItemRect(item).top() > list_->viewport()->height() + 120) {
            return;
        }
        if (item->data(Qt::DecorationRole).isNull()) {
            break;
        }
        ++coverCursor_;
    }
    if (coverCursor_ >= list_->count()) {
        return;
    }
    coverBusy_ = true;
    const auto row = coverCursor_++;
    const auto generation = coverGeneration_;
    const auto id = shown_[static_cast<std::size_t>(row)].id;
    auto vault = vault_;
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, generation, row] {
        const auto image = watcher->result();
        watcher->deleteLater();
        coverBusy_ = false;
        if (playlistEditBusy_ || generation != coverGeneration_ || row >= list_->count()
            || vault_ == nullptr || !vault_->is_unlocked()) {
            if (!playlistEditBusy_ && coverPump_ != nullptr && vault_ != nullptr && vault_->is_unlocked()) {
                coverPump_->start(10);
            }
            return;
        }
        if (!image.isNull()) {
            auto* item = list_->item(row);
            if (item != nullptr) {
                item->setData(Qt::DecorationRole,
                    QIcon(QPixmap::fromImage(image.scaled(84, 84, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation))));
            }
        }
        if (coverPump_ != nullptr) {
            coverPump_->start(10);
        }
    });
    watcher->setFuture(QtConcurrent::run([vault, id] {
        QImage cover;
        // The stored thumbnail is a decrypted JPEG; audio imports get one from
        // their embedded picture, so a failed probe simply leaves the note tile.
        const auto thumbnail = vault->display_thumbnail(id);
        if (thumbnail) {
            cover.loadFromData(thumbnail.value().bytes.data(),
                static_cast<int>(thumbnail.value().bytes.size()));
        }
        return cover;
    }));
}

void MusicLibraryPage::ensurePlayer() {
    // No fallback ownership: production injects the one central footer. Never
    // steal an existing shared player's parent or accidentally create another.
    if (!player_) showError(QStringLiteral("The music player is unavailable. Return to the library and try again."));
}

void MusicLibraryPage::playRow(const int row) {
    if (vault_ == nullptr || !vault_->is_unlocked()) {
        return;
    }
    // In the Playlists section a row is a playlist, not a track.
    if (currentSection_ == PlaylistsSection && openPlaylistId_ == 0) {
        const auto* item = list_->item(row);
        if (item != nullptr) {
            openPlaylist(std::as_const(*item).data(Qt::UserRole).toLongLong());
        }
        return;
    }
    if (shown_.empty()) {
        return;
    }
    const int index = std::clamp(row, 0, static_cast<int>(shown_.size()) - 1);
    std::vector<MusicTrack> queue;
    queue.reserve(shown_.size());
    for (const auto& track : shown_) {
        queue.push_back(MusicTrack{track.id, track.title});
    }
    ensurePlayer();
    if (!player_) return;
    player_->playQueue(vault_, queue, index);
    player_->setVisible(true);
    if (list_ != nullptr) {
        list_->setCurrentRow(index);
    }
}

} // namespace videovault::app
