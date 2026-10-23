#pragma once

#include "videovault/core/vault.hpp"

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace videovault::app {

class PeerLink;
struct PeerCopyPlan;

// Runs batch operations (multi-file import, multi-video restore) off the UI
// thread with up to four files processed in parallel, and reports progress to
// the UI thread via queued signals — the UI event loop is never blocked.
//
// `progress` carries a smooth aggregate byte count (monotonic, reported per
// chunk inside each file, so the bar moves continuously even for one large
// file); `fileFinished` reports "n of N files" for the status text. The slots
// take shared_ptr payloads so nothing is copied across threads; the vault is
// held alive for the duration of the batch.
class BatchWorker final : public QObject {
    Q_OBJECT
public:
    explicit BatchWorker(QObject* parent = nullptr);

public slots:
    void importFiles(
        std::shared_ptr<std::vector<std::filesystem::path>> sources,
        std::shared_ptr<videovault::core::Vault> vault,
        std::int64_t folder_id);

    // Files land in `folder_id`. Each directory is recreated in the vault,
    // including the folders inside it, and its files are imported there.
    void importTree(
        std::shared_ptr<std::vector<std::filesystem::path>> sources,
        std::shared_ptr<videovault::core::Vault> vault,
        std::int64_t folder_id);

    void restoreVideos(
        std::shared_ptr<std::vector<std::int64_t>> ids,
        std::shared_ptr<std::filesystem::path> target_dir,
        std::shared_ptr<videovault::core::Vault> vault);

    void removeVideos(
        std::shared_ptr<std::vector<std::int64_t>> ids,
        std::shared_ptr<videovault::core::Vault> vault);

    void exportPackage(
        std::shared_ptr<videovault::core::Vault> vault,
        std::shared_ptr<std::filesystem::path> destination,
        std::shared_ptr<videovault::core::Secret> password,
        std::shared_ptr<std::vector<videovault::core::Vault::PackageItem>> files,
        std::shared_ptr<std::vector<std::string>> directories,
        bool compress);

    void importPackage(
        std::shared_ptr<videovault::core::Vault> vault,
        std::shared_ptr<std::filesystem::path> source,
        std::shared_ptr<videovault::core::Secret> password,
        std::int64_t folder_id);

    // Copies folders and files out of a swapped phone's library, one file at
    // a time, recreating the folder structure under plan->destination.
    void copyFromPeer(
        std::shared_ptr<PeerCopyPlan> plan,
        std::shared_ptr<PeerLink> peer,
        std::shared_ptr<videovault::core::Vault> vault);

signals:
    // Aggregate bytes processed / total bytes (qlonglong: imports can exceed
    // 2 GB). Monotonic and delivered per chunk, so the bar moves smoothly.
    void progress(qlonglong done_bytes, qlonglong total_bytes);

    // One more file/video finished.
    void fileFinished(int done_files, int total_files);

    // One new vault file or folder. Delivered as it lands, not at the end of
    // the batch. Duplicates are not announced.
    void videoArrived(qlonglong id, QString name, qlonglong size, qlonglong folderId, qlonglong importedAt);
    void folderArrived(qlonglong id, qlonglong parentId, QString name);

    // Emitted once at the end: ok=true with the success count and a summary,
    // or ok=false with the first error message and the partial count.
    void finished(bool ok, QString message, int count);
};

} // namespace videovault::app
