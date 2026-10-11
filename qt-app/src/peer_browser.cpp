#include "videovault/app/peer_browser.hpp"
#include "videovault/app/peer_link.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace videovault::app {
namespace {

constexpr int kKindFile = 0;
constexpr int kKindFolder = 1;
constexpr int kKindUp = 2;
constexpr int kRoleId = Qt::UserRole;
constexpr int kRoleKind = Qt::UserRole + 1;
constexpr int kTile = 132;

QPixmap folderTile() {
    static const QPixmap pix = [] {
        QPixmap image(kTile, kTile);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(140, 196, 255));
        painter.drawRoundedRect(QRectF(kTile * 0.08, kTile * 0.14, kTile * 0.42, kTile * 0.22), 8, 8);
        painter.setBrush(QColor(64, 140, 230));
        painter.drawRoundedRect(QRectF(kTile * 0.06, kTile * 0.30, kTile * 0.88, kTile * 0.56), 12, 12);
        return image;
    }();
    return pix;
}

QPixmap fileTile(const QString& name) {
    QPixmap image(kTile, kTile);
    image.fill(QColor(28, 28, 30));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(142, 142, 147));
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSize(14);
    font.setBold(true);
    painter.setFont(font);
    const QString suffix = QFileInfo(name).suffix().toUpper();
    painter.drawText(image.rect(), Qt::AlignCenter, suffix.isEmpty() ? QStringLiteral("FILE") : suffix);
    return image;
}

QPixmap squareCrop(const QPixmap& source) {
    const QPixmap scaled = source.scaled(kTile, kTile, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = std::max(0, (scaled.width() - kTile) / 2);
    const int y = std::max(0, (scaled.height() - kTile) / 2);
    return scaled.copy(x, y, kTile, kTile);
}

QString sizeText(const std::uint64_t bytes) {
    if (bytes >= 1000ULL * 1000ULL * 1000ULL) {
        return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / 1e9, 0, 'f', 2);
    }
    if (bytes >= 1000ULL * 1000ULL) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / 1e6, 0, 'f', 1);
    }
    return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1e3, 0, 'f', 0);
}

QJsonObject op(const QString& name) {
    QJsonObject request;
    request.insert(QStringLiteral("op"), name);
    return request;
}

} // namespace

struct PeerBrowser::Listing {
    bool ok{false};
    std::vector<PeerFolder> folders;
    std::vector<PeerFile> files;
};

PeerBrowser::PeerBrowser(std::shared_ptr<PeerLink> peer, QWidget* parent)
    : QDialog(parent),
      peer_(std::move(peer)),
      generation_(std::make_shared<std::atomic<quint64>>(0)) {
    phone_ = peer_->name();
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    setWindowTitle(QStringLiteral("%1 · shared with you").arg(phone_));
    resize(980, 660);
    setStyleSheet(QStringLiteral(
        "QDialog { background: #000000; }"
        "QLabel { color: white; background: transparent; }"
        "QLabel#peerHint, QLabel#peerStatus { color: #8e8e93; }"
        "QListWidget { background: #000000; border: none; color: white; outline: none; }"
        "QListWidget::item { color: white; border-radius: 6px; }"
        "QListWidget::item:selected { background: rgba(51, 144, 236, 110); }"
        "QPushButton { background: #1c1c1e; color: white; border: none; border-radius: 14px;"
        "  min-height: 34px; padding: 0 16px; }"
        "QPushButton:hover { background: #2c2c2e; }"
        "QPushButton:disabled { color: #636366; }"
        "QPushButton#peerCopy { background: #3390ec; font-weight: 600; }"
        "QPushButton#peerCopy:hover { background: #4aa0f0; }"
        "QPushButton#peerUnlink { background: #2c1518; color: #ff6b6b; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);

    auto* top = new QHBoxLayout();
    crumbHost_ = new QWidget(this);
    crumbLayout_ = new QHBoxLayout(crumbHost_);
    crumbLayout_->setContentsMargins(0, 0, 0, 0);
    crumbLayout_->setSpacing(2);
    top->addWidget(crumbHost_, 1);
    refresh_ = new QPushButton(QStringLiteral("Refresh"), this);
    unlink_ = new QPushButton(QStringLiteral("Disconnect phone"), this);
    unlink_->setObjectName(QStringLiteral("peerUnlink"));
    top->addWidget(refresh_);
    top->addWidget(unlink_);
    layout->addLayout(top);

    auto* hint = new QLabel(QStringLiteral(
        "This is %1's library. You can look and copy, but not change or remove anything. "
        "Select folders or files, or copy everything in this folder. Folders keep their structure.")
        .arg(phone_), this);
    hint->setObjectName(QStringLiteral("peerHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setIconSize(QSize(kTile, kTile));
    list_->setGridSize(QSize(kTile + 18, kTile + 40));
    list_->setUniformItemSizes(true);
    list_->setWordWrap(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setDragEnabled(false);
    layout->addWidget(list_, 1);

    auto* bottom = new QHBoxLayout();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("peerStatus"));
    copy_ = new QPushButton(this);
    copy_->setObjectName(QStringLiteral("peerCopy"));
    bottom->addWidget(status_, 1);
    bottom->addWidget(copy_);
    layout->addLayout(bottom);

    connect(refresh_, &QPushButton::clicked, this, [this] { reload(); });
    connect(unlink_, &QPushButton::clicked, this, [this] {
        if (onUnlink_) onUnlink_();
    });
    connect(copy_, &QPushButton::clicked, this, [this] { requestCopy(); });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this] { updateCopyButton(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int kind = item->data(kRoleKind).toInt();
        if (kind == kKindFolder || kind == kKindUp) {
            showFolder(item->data(kRoleId).toLongLong());
        }
    });

    rebuildCrumbs();
    updateCopyButton();
    reload();
}

PeerBrowser::~PeerBrowser() {
    generation_->fetch_add(1);
}

void PeerBrowser::setOnCopy(std::function<void(std::shared_ptr<PeerCopyPlan>)> callback) {
    onCopy_ = std::move(callback);
}

void PeerBrowser::setOnUnlink(std::function<void()> callback) {
    onUnlink_ = std::move(callback);
}

void PeerBrowser::reload() {
    generation_->fetch_add(1);
    status_->setText(QStringLiteral("Loading %1's library…").arg(phone_));
    refresh_->setEnabled(false);
    const auto peer = peer_;
    QPointer<PeerBrowser> self(this);
    QtConcurrent::run([peer, self] {
        auto listing = std::make_shared<Listing>();
        const auto folders = peer->call(op(QStringLiteral("folders")));
        const auto videos = folders && folders->status == 200
            ? peer->call(op(QStringLiteral("videos")))
            : std::nullopt;
        if (folders && folders->status == 200 && videos && videos->status == 200) {
            listing->ok = true;
            for (const auto value : QJsonDocument::fromJson(folders->body).array()) {
                const QJsonObject o = value.toObject();
                listing->folders.push_back(PeerFolder{
                    static_cast<std::int64_t>(o.value(QStringLiteral("id")).toDouble()),
                    static_cast<std::int64_t>(o.value(QStringLiteral("parentId")).toDouble()),
                    o.value(QStringLiteral("name")).toString()});
            }
            for (const auto value : QJsonDocument::fromJson(videos->body).array()) {
                const QJsonObject o = value.toObject();
                listing->files.push_back(PeerFile{
                    static_cast<std::int64_t>(o.value(QStringLiteral("id")).toDouble()),
                    static_cast<std::int64_t>(o.value(QStringLiteral("folderId")).toDouble()),
                    o.value(QStringLiteral("name")).toString(),
                    static_cast<std::uint64_t>(o.value(QStringLiteral("size")).toDouble())});
            }
        }
        QMetaObject::invokeMethod(qApp, [self, listing] {
            if (self) self->applyListing(listing);
        }, Qt::QueuedConnection);
    });
}

void PeerBrowser::applyListing(const std::shared_ptr<Listing>& listing) {
    refresh_->setEnabled(true);
    if (!listing->ok) {
        status_->setText(peer_->linked()
            ? QStringLiteral("The phone did not answer. Keep Mega Vault Protect open on it, then Refresh.")
            : QStringLiteral("The phone disconnected."));
        return;
    }
    folders_ = listing->folders;
    files_ = listing->files;
    loaded_ = true;
    const bool stillThere = current_ == 0
        || std::any_of(folders_.begin(), folders_.end(),
            [this](const PeerFolder& folder) { return folder.id == current_; });
    showFolder(stillThere ? current_ : 0);
}

void PeerBrowser::showFolder(const std::int64_t folder_id) {
    current_ = folder_id;
    generation_->fetch_add(1);
    list_->clear();

    auto addItem = [this](const QString& name, const std::int64_t id, const int kind, const QPixmap& pixmap) {
        auto* item = new QListWidgetItem(QIcon(pixmap), name, list_);
        item->setData(kRoleId, static_cast<qlonglong>(id));
        item->setData(kRoleKind, kind);
        item->setToolTip(name);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        return item;
    };

    if (current_ != 0) {
        std::int64_t parent = 0;
        for (const auto& folder : folders_) {
            if (folder.id == current_) {
                parent = folder.parent_id;
                break;
            }
        }
        auto* up = addItem(QStringLiteral(".."), parent, kKindUp, folderTile());
        up->setFlags(up->flags() & ~Qt::ItemIsSelectable);
    }

    std::vector<PeerFolder> childFolders;
    for (const auto& folder : folders_) {
        if (folder.parent_id == current_) childFolders.push_back(folder);
    }
    std::sort(childFolders.begin(), childFolders.end(), [](const PeerFolder& a, const PeerFolder& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    for (const auto& folder : childFolders) {
        addItem(folder.name, folder.id, kKindFolder, folderTile());
    }

    std::vector<PeerFile> childFiles;
    for (const auto& file : files_) {
        if (file.folder_id == current_) childFiles.push_back(file);
    }
    std::sort(childFiles.begin(), childFiles.end(), [](const PeerFile& a, const PeerFile& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    std::vector<std::int64_t> missing;
    std::uint64_t bytes = 0U;
    for (const auto& file : childFiles) {
        bytes += file.size;
        const auto cached = thumbs_.constFind(static_cast<qlonglong>(file.id));
        auto* item = addItem(file.name, file.id, kKindFile,
            cached != thumbs_.constEnd() ? cached.value() : fileTile(file.name));
        item->setToolTip(QStringLiteral("%1\n%2").arg(file.name, sizeText(file.size)));
        if (cached == thumbs_.constEnd()) missing.push_back(file.id);
    }

    rebuildCrumbs();
    status_->setText(childFolders.empty() && childFiles.empty()
        ? QStringLiteral("This folder is empty.")
        : QStringLiteral("%1 folders · %2 files · %3")
              .arg(childFolders.size()).arg(childFiles.size()).arg(sizeText(bytes)));
    updateCopyButton();
    loadThumbnails(missing);
}

void PeerBrowser::rebuildCrumbs() {
    while (QLayoutItem* item = crumbLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    std::vector<std::pair<std::int64_t, QString>> chain;
    std::int64_t cursor = current_;
    for (int guard = 0; guard < 64 && cursor != 0; ++guard) {
        const auto found = std::find_if(folders_.begin(), folders_.end(),
            [cursor](const PeerFolder& folder) { return folder.id == cursor; });
        if (found == folders_.end()) break;
        chain.emplace_back(found->id, found->name);
        cursor = found->parent_id;
    }
    chain.emplace_back(0, phone_);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSize(13);
    for (int index = static_cast<int>(chain.size()) - 1; index >= 0; --index) {
        const auto id = chain[static_cast<std::size_t>(index)].first;
        auto* button = new QPushButton(chain[static_cast<std::size_t>(index)].second, crumbHost_);
        button->setFlat(true);
        button->setFont(font);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(index == 0
            ? QStringLiteral("QPushButton { color: white; background: transparent; padding: 2px 4px; }")
            : QStringLiteral("QPushButton { color: #8ec8ff; background: transparent; padding: 2px 4px; }"));
        connect(button, &QPushButton::clicked, this, [this, id] {
            if (loaded_) showFolder(id);
        });
        crumbLayout_->addWidget(button);
        if (index > 0) {
            auto* slash = new QLabel(QStringLiteral("/"), crumbHost_);
            slash->setStyleSheet(QStringLiteral("color: #8e8e93;"));
            crumbLayout_->addWidget(slash);
        }
    }
    crumbLayout_->addStretch(1);
}

void PeerBrowser::loadThumbnails(const std::vector<std::int64_t>& ids) {
    if (ids.empty()) return;
    const quint64 generation = generation_->load();
    const auto counter = generation_;
    const auto peer = peer_;
    QPointer<PeerBrowser> self(this);
    QtConcurrent::run([ids, generation, counter, peer, self] {
        for (const auto id : ids) {
            if (counter->load() != generation || !peer->linked()) return;
            QJsonObject request = op(QStringLiteral("thumb"));
            request.insert(QStringLiteral("id"), static_cast<double>(id));
            request.insert(QStringLiteral("max"), 320);
            const auto reply = peer->call(request, 20000);
            if (!reply || reply->status != 200 || reply->body.isEmpty()) continue;
            const QByteArray jpeg = reply->body;
            QMetaObject::invokeMethod(qApp, [self, generation, id, jpeg] {
                if (self) self->applyThumbnail(generation, id, jpeg);
            }, Qt::QueuedConnection);
        }
    });
}

void PeerBrowser::applyThumbnail(const quint64 generation, const std::int64_t id, const QByteArray& jpeg) {
    QPixmap pixmap;
    if (!pixmap.loadFromData(jpeg)) return;
    const QPixmap tile = squareCrop(pixmap);
    thumbs_.insert(static_cast<qlonglong>(id), tile);
    if (generation_->load() != generation) return;
    for (int index = 0; index < list_->count(); ++index) {
        auto* item = list_->item(index);
        if (item->data(kRoleKind).toInt() == kKindFile && item->data(kRoleId).toLongLong() == id) {
            item->setIcon(QIcon(tile));
            break;
        }
    }
}

void PeerBrowser::updateCopyButton() {
    int picked = 0;
    for (auto* item : list_->selectedItems()) {
        if (item->data(kRoleKind).toInt() != kKindUp) ++picked;
    }
    copy_->setText(picked > 0
        ? QStringLiteral("Copy %1 selected to my library…").arg(picked)
        : QStringLiteral("Copy everything here to my library…"));
    copy_->setEnabled(loaded_ && list_->count() > (current_ != 0 ? 1 : 0));
}

void PeerBrowser::requestCopy() {
    if (!loaded_ || !onCopy_) return;
    std::vector<std::int64_t> pickedFolders;
    std::vector<std::int64_t> pickedFiles;
    auto take = [&](QListWidgetItem* item) {
        const int kind = item->data(kRoleKind).toInt();
        const auto id = static_cast<std::int64_t>(item->data(kRoleId).toLongLong());
        if (kind == kKindFolder) pickedFolders.push_back(id);
        if (kind == kKindFile) pickedFiles.push_back(id);
    };
    const auto selected = list_->selectedItems();
    if (selected.isEmpty()) {
        for (int index = 0; index < list_->count(); ++index) take(list_->item(index));
    } else {
        for (auto* item : selected) take(item);
    }

    auto plan = std::make_shared<PeerCopyPlan>();
    plan->phone = phone_;
    std::unordered_set<std::int64_t> inPlan;
    for (const auto root : pickedFolders) {
        std::deque<std::int64_t> queue{root};
        while (!queue.empty()) {
            const auto id = queue.front();
            queue.pop_front();
            if (!inPlan.insert(id).second) continue;
            const auto found = std::find_if(folders_.begin(), folders_.end(),
                [id](const PeerFolder& folder) { return folder.id == id; });
            if (found == folders_.end()) continue;
            plan->folders.push_back(*found);
            for (const auto& child : folders_) {
                if (child.parent_id == id) queue.push_back(child.id);
            }
        }
    }
    std::unordered_set<std::int64_t> filesTaken;
    for (const auto& file : files_) {
        const bool inside = inPlan.count(file.folder_id) != 0U;
        const bool chosen = std::find(pickedFiles.begin(), pickedFiles.end(), file.id) != pickedFiles.end();
        if ((inside || chosen) && filesTaken.insert(file.id).second) {
            plan->files.push_back(file);
            plan->total_bytes += file.size;
        }
    }
    if (plan->folders.empty() && plan->files.empty()) {
        status_->setText(QStringLiteral("Nothing to copy here."));
        return;
    }
    onCopy_(plan);
}

} // namespace videovault::app
