#pragma once

#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;

namespace videovault::app {

class PeerLink;

struct PeerFolder {
    std::int64_t id{0};
    std::int64_t parent_id{0};
    QString name;
};

struct PeerFile {
    std::int64_t id{0};
    std::int64_t folder_id{0};
    QString name;
    std::uint64_t size{0U};
};

// What to copy out of the phone's library. Folders are listed parents first.
// A folder or file whose parent is not in the plan lands in `destination`.
struct PeerCopyPlan {
    QString phone;
    std::vector<PeerFolder> folders;
    std::vector<PeerFile> files;
    std::int64_t destination{0};
    std::uint64_t total_bytes{0U};
    // Set when the user cancels the copy that is already moving.
    std::shared_ptr<std::atomic_bool> stop;
};

// The swapped phone's library, read-only. Nothing here can change or remove
// the phone's files; the only action is copying into this PC's library.
class PeerBrowser final : public QDialog {
public:
    PeerBrowser(std::shared_ptr<PeerLink> peer, QWidget* parent = nullptr);
    ~PeerBrowser() override;

    void setOnCopy(std::function<void(std::shared_ptr<PeerCopyPlan>)> callback);
    void setOnUnlink(std::function<void()> callback);
    void reload();

private:
    struct Listing;

    void applyListing(const std::shared_ptr<Listing>& listing);
    void showFolder(std::int64_t folder_id);
    void rebuildCrumbs();
    void loadThumbnails(const std::vector<std::int64_t>& ids);
    void applyThumbnail(quint64 generation, std::int64_t id, const QByteArray& jpeg);
    void updateCopyButton();
    void requestCopy();

    std::shared_ptr<PeerLink> peer_;
    QString phone_;
    std::vector<PeerFolder> folders_;
    std::vector<PeerFile> files_;
    std::int64_t current_{0};
    bool loaded_{false};
    std::shared_ptr<std::atomic<quint64>> generation_;
    QHash<qlonglong, QPixmap> thumbs_;

    QWidget* crumbHost_{nullptr};
    QHBoxLayout* crumbLayout_{nullptr};
    QListWidget* list_{nullptr};
    QLabel* status_{nullptr};
    QPushButton* copy_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* unlink_{nullptr};

    std::function<void(std::shared_ptr<PeerCopyPlan>)> onCopy_;
    std::function<void()> onUnlink_;
};

} // namespace videovault::app
