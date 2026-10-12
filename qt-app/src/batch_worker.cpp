#include "videovault/app/batch_worker.hpp"
#include "videovault/app/peer_browser.hpp"
#include "videovault/app/peer_link.hpp"

#include "videovault/core/vault.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace videovault::app {
namespace {

// Shared across the parallel file workers of one batch.
struct BatchState {
    std::atomic<std::size_t> next{0};
    std::atomic<std::int64_t> bytes_done{0};
    std::atomic<int> files_done{0};
    std::atomic<bool> stop{false};
    std::mutex error_mutex;
    QString first_error;
};

constexpr int kMaxParallelFiles = 4;

// Bytes asked from the phone per request. The answer travels as one HTTP body.
constexpr int kPeerChunkBytes = 512 * 1024;

// A phone name may hold characters Windows does not allow in a file name.
QString safeFileName(const QString& name) {
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        const bool bad = ch.unicode() < 0x20 || QStringLiteral("\\/:*?\"<>|").contains(ch);
        out += bad ? QLatin1Char('_') : ch;
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.'))) out.chop(1);
    return out.isEmpty() ? QStringLiteral("file") : out;
}

// Reuses a folder of the same name so a second copy merges into the first.
videovault::core::Result<std::int64_t> folderIn(
    videovault::core::Vault& vault, const std::int64_t parent, const QString& name) {
    if (auto listed = vault.list_folders(); listed) {
        for (const auto& folder : listed.value()) {
            if (folder.parent_id == parent
                && QString::fromStdString(folder.name).compare(name, Qt::CaseInsensitive) == 0) {
                return folder.id;
            }
        }
    }
    return vault.create_folder(parent, name.toStdString());
}

int workerCount() {
    const auto cores = std::thread::hardware_concurrency();
    const auto count = cores == 0 ? 2 : static_cast<int>(cores);
    return std::clamp(count, 1, kMaxParallelFiles);
}

// Runs `work(index)` concurrently over [0, count); stops pulling new items
// once `state->stop` is set (first error). `fileFinished` is emitted from the
// finishing threads as each item completes.
template <typename Work>
void runParallel(const std::shared_ptr<BatchState>& state,
                 const std::size_t count,
                 BatchWorker* worker,
                 Work&& work) {
    const int threads = workerCount();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            while (!state->stop.load()) {
                const std::size_t index = state->next.fetch_add(1);
                if (index >= count) {
                    break;
                }
                work(static_cast<int>(index));
                const int done = state->files_done.fetch_add(1) + 1;
                emit worker->fileFinished(done, static_cast<int>(count));
            }
        });
    }
    for (auto& thread : pool) {
        thread.join();
    }
}

} // namespace

BatchWorker::BatchWorker(QObject* parent)
    : QObject(parent) {}

void BatchWorker::importFiles(
    std::shared_ptr<std::vector<std::filesystem::path>> sources,
    std::shared_ptr<videovault::core::Vault> vault,
    const std::int64_t folder_id) {
    // Size pass first so the aggregate bar has a total (plaintext bytes).
    std::vector<std::int64_t> sizes;
    sizes.reserve(sources->size());
    std::int64_t total_bytes = 0;
    for (const auto& path : *sources) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto safe = error ? 0 : static_cast<std::int64_t>(size);
        sizes.push_back(safe);
        total_bytes += safe;
    }

    const auto state = std::make_shared<BatchState>();
    std::atomic<int> imported{0};
    std::atomic<int> skipped{0};
    runParallel(state, sources->size(), this,
        [&](const int index) {
            const auto reported = std::make_shared<std::atomic<std::int64_t>>(0);
            bool duplicate = false;
            const auto outcome = vault->import_file((*sources)[index],
                [this, state, reported, file_size = sizes[index], total_bytes](const double fraction) {
                    // The core reports a cumulative fraction. Adding that
                    // fraction on every tick counted the same bytes many
                    // times and ran the bar off the end of a large drop.
                    const auto clamped = std::clamp(fraction, 0.0, 1.0);
                    const auto target = static_cast<std::int64_t>(
                        clamped * static_cast<double>(file_size));
                    const auto previous = reported->exchange(target);
                    const auto delta = target - previous;
                    if (delta <= 0) {
                        return;
                    }
                    const auto done = state->bytes_done.fetch_add(delta) + delta;
                    emit progress(std::min(done, total_bytes), total_bytes);
                },
                folder_id, &duplicate);
            if (!outcome) {
                std::lock_guard<std::mutex> guard(state->error_mutex);
                if (state->first_error.isEmpty()) {
                    state->first_error = QString::fromUtf8(
                        videovault::core::user_message(outcome.error().code).data());
                    state->stop.store(true);
                }
            } else if (duplicate) {
                ++skipped;
            } else {
                ++imported;
                const auto when = static_cast<qlonglong>(std::chrono::duration_cast<
                    std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                emit videoArrived(
                    static_cast<qlonglong>(outcome.value()),
                    QFileInfo(QString::fromStdWString((*sources)[index].wstring())).fileName(),
                    sizes[index],
                    static_cast<qlonglong>(folder_id),
                    when);
            }
        });

    if (state->first_error.isEmpty()) {
        const auto added = imported.load();
        const auto duplicates = skipped.load();
        QString message;
        if (added == 0 && duplicates > 0) {
            message = QStringLiteral("Already in the vault.");
        } else if (duplicates > 0) {
            message = QStringLiteral("Imported %1 file(s). Skipped %2 already in the vault.")
                .arg(added)
                .arg(duplicates);
        } else {
            message = QStringLiteral("Imported %1 file(s).").arg(added);
        }
        emit finished(true, message, added);
    } else {
        emit finished(false, state->first_error, imported.load());
    }
}

void BatchWorker::importTree(
    std::shared_ptr<std::vector<std::filesystem::path>> sources,
    std::shared_ptr<videovault::core::Vault> vault,
    const std::int64_t folder_id) {
    const auto root = vault->root_path();
    const auto inside_vault = [&root](const std::filesystem::path& candidate) {
        std::error_code error;
        const auto file = std::filesystem::weakly_canonical(candidate, error);
        const auto vault_root = std::filesystem::weakly_canonical(root, error);
        if (file.empty() || vault_root.empty()) {
            return false;
        }
        const auto relative = file.lexically_relative(vault_root).wstring();
        return !relative.empty() && relative != L".." && relative.rfind(L"..\\", 0) != 0
            && relative.rfind(L"../", 0) != 0;
    };

    std::map<std::pair<std::int64_t, QString>, std::int64_t> known;
    if (auto listed = vault->list_folders(); listed) {
        for (const auto& folder : listed.value()) {
            known.emplace(
                std::make_pair(folder.parent_id, QString::fromStdString(folder.name).toLower()),
                folder.id);
        }
    }

    const auto folder_name = [](QString name) {
        name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1F]")), QStringLiteral("_"));
        name = name.trimmed();
        while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
            name.chop(1);
        }
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
            name = QStringLiteral("Folder");
        }
        if (name.size() > 120) {
            name.truncate(120);
        }
        return name;
    };

    QString failure;
    int folders_made = 0;
    const auto ensure_folder = [&](const std::int64_t parent, const QString& raw) -> std::int64_t {
        const QString name = folder_name(raw);
        const auto key = std::make_pair(parent, name.toLower());
        if (const auto found = known.find(key); found != known.end()) {
            return found->second;
        }
        auto created = vault->create_folder(parent, name.toStdString());
        if (!created) {
            if (failure.isEmpty()) {
                failure = QString::fromUtf8(
                    videovault::core::user_message(created.error().code).data());
            }
            return -1;
        }
        known.emplace(key, created.value());
        ++folders_made;
        emit folderArrived(static_cast<qlonglong>(created.value()),
            static_cast<qlonglong>(parent), name);
        return created.value();
    };

    struct TreeFile {
        std::filesystem::path path;
        std::int64_t folder_id{0};
        std::int64_t size{0};
    };
    std::vector<TreeFile> jobs;
    const auto consider_file = [&](const std::filesystem::path& path, const std::int64_t parent) {
        const auto leaf = QString::fromStdWString(path.filename().wstring());
        if (leaf.compare(QStringLiteral("desktop.ini"), Qt::CaseInsensitive) == 0
            || leaf.compare(QStringLiteral("Thumbs.db"), Qt::CaseInsensitive) == 0
            || leaf.startsWith(QLatin1Char('.'))) {
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return;
        }
        const auto size = std::filesystem::file_size(path, error);
        jobs.push_back(TreeFile{path, parent, error ? 0 : static_cast<std::int64_t>(size)});
    };

    const auto walk = [&](auto&& self, const std::filesystem::path& directory, const std::int64_t parent) -> void {
        std::error_code error;
        std::filesystem::directory_iterator it(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::directory_iterator end;
        for (; !error && it != end; it.increment(error)) {
            std::error_code kind_error;
            if (it->is_directory(kind_error)) {
                const auto id = ensure_folder(parent, QString::fromStdWString(it->path().filename().wstring()));
                if (id < 0) {
                    return;
                }
                self(self, it->path(), id);
            } else {
                consider_file(it->path(), parent);
            }
        }
    };

    for (const auto& source : *sources) {
        if (inside_vault(source)) {
            failure = QStringLiteral("That folder is already inside the vault.");
            break;
        }
        std::error_code error;
        if (std::filesystem::is_directory(source, error)) {
            const auto id = ensure_folder(folder_id, QString::fromStdWString(source.filename().wstring()));
            if (id < 0) {
                break;
            }
            walk(walk, source, id);
        } else {
            consider_file(source, folder_id);
        }
        if (!failure.isEmpty()) {
            break;
        }
    }

    if (!failure.isEmpty() && jobs.empty() && folders_made == 0) {
        emit finished(false, failure, 0);
        return;
    }

    std::int64_t total_bytes = 0;
    for (const auto& job : jobs) {
        total_bytes += job.size;
    }
    const auto state = std::make_shared<BatchState>();
    std::atomic<int> imported{0};
    std::atomic<int> skipped{0};
    const auto shared_jobs = std::make_shared<std::vector<TreeFile>>(std::move(jobs));
    runParallel(state, shared_jobs->size(), this, [&](const int index) {
        const auto& job = (*shared_jobs)[static_cast<std::size_t>(index)];
        const auto reported = std::make_shared<std::atomic<std::int64_t>>(0);
        bool duplicate = false;
        const auto outcome = vault->import_file(job.path,
            [this, state, reported, file_size = job.size, total_bytes](const double fraction) {
                const auto clamped = std::clamp(fraction, 0.0, 1.0);
                const auto target = static_cast<std::int64_t>(clamped * static_cast<double>(file_size));
                const auto previous = reported->exchange(target);
                const auto delta = target - previous;
                if (delta <= 0) {
                    return;
                }
                const auto done = state->bytes_done.fetch_add(delta) + delta;
                emit progress(std::min(done, total_bytes), std::max<std::int64_t>(total_bytes, 1));
            },
            job.folder_id, &duplicate);
        if (!outcome) {
            std::lock_guard<std::mutex> guard(state->error_mutex);
            if (state->first_error.isEmpty()) {
                state->first_error = QString::fromUtf8(
                    videovault::core::user_message(outcome.error().code).data());
                state->stop.store(true);
            }
        } else if (duplicate) {
            ++skipped;
        } else {
            ++imported;
            const auto when = static_cast<qlonglong>(std::chrono::duration_cast<
                std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            emit videoArrived(
                static_cast<qlonglong>(outcome.value()),
                QString::fromStdWString(job.path.filename().wstring()),
                job.size,
                static_cast<qlonglong>(job.folder_id),
                when);
        }
    });

    if (!state->first_error.isEmpty()) {
        emit finished(false, state->first_error, imported.load());
        return;
    }
    if (!failure.isEmpty()) {
        emit finished(false, failure, imported.load());
        return;
    }
    const auto added = imported.load();
    const auto duplicates = skipped.load();
    QString message;
    if (added == 0 && duplicates > 0) {
        message = QStringLiteral("Already in the vault.");
    } else if (added == 0 && folders_made > 0) {
        message = QStringLiteral("Imported %1 folder(s).").arg(folders_made);
    } else if (duplicates > 0) {
        message = QStringLiteral("Imported %1 file(s). Skipped %2 already in the vault.")
            .arg(added)
            .arg(duplicates);
    } else {
        message = QStringLiteral("Imported %1 file(s).").arg(added);
    }
    emit finished(true, message, added);
}

void BatchWorker::restoreVideos(
    std::shared_ptr<std::vector<std::int64_t>> ids,
    std::shared_ptr<std::filesystem::path> target_dir,
    std::shared_ptr<videovault::core::Vault> vault) {
    // Weight each video by its plaintext size so the bar reflects real work.
    std::unordered_map<std::int64_t, std::int64_t> weights;
    std::int64_t total_bytes = 0;
    if (auto listed = vault->list_videos(); listed) {
        for (const auto& video : listed.value()) {
            weights.emplace(video.id, static_cast<std::int64_t>(video.original_size));
            total_bytes += static_cast<std::int64_t>(video.original_size);
        }
    }
    if (total_bytes <= 0) {
        total_bytes = static_cast<std::int64_t>(ids->size()) * 1000; // fallback
    }

    const auto state = std::make_shared<BatchState>();
    std::atomic<int> restored{0};
    runParallel(state, ids->size(), this,
        [&](const int index) {
            const auto video_id = (*ids)[index];
            const auto weight = weights.count(video_id) != 0U
                ? weights[video_id] : 1000;
            const auto reported = std::make_shared<std::atomic<std::int64_t>>(0);
            const auto outcome = vault->restore_video(video_id, *target_dir,
                [this, state, reported, weight, total_bytes](const double fraction) {
                    const auto clamped = std::clamp(fraction, 0.0, 1.0);
                    const auto target = static_cast<std::int64_t>(
                        clamped * static_cast<double>(weight));
                    const auto previous = reported->exchange(target);
                    const auto delta = target - previous;
                    if (delta <= 0) {
                        return;
                    }
                    const auto done = state->bytes_done.fetch_add(delta) + delta;
                    emit progress(std::min(done, total_bytes), total_bytes);
                });
            if (!outcome) {
                std::lock_guard<std::mutex> guard(state->error_mutex);
                if (state->first_error.isEmpty()) {
                    state->first_error = QString::fromUtf8(
                        videovault::core::user_message(outcome.error().code).data());
                    state->stop.store(true);
                }
            } else {
                ++restored;
            }
        });

    if (state->first_error.isEmpty()) {
        emit finished(true,
            QStringLiteral("Restored %1 file(s).").arg(restored.load()), restored.load());
    } else {
        emit finished(false, state->first_error, restored.load());
    }
}

void BatchWorker::removeVideos(
    std::shared_ptr<std::vector<std::int64_t>> ids,
    std::shared_ptr<videovault::core::Vault> vault) {
    const auto total = static_cast<qlonglong>(ids->size());
    const auto state = std::make_shared<BatchState>();
    std::atomic<int> removed{0};
    runParallel(state, ids->size(), this,
        [&](const int index) {
            const auto outcome = vault->remove_video((*ids)[index]);
            emit progress(static_cast<qlonglong>(state->files_done.load() + 1), total);
            if (!outcome) {
                std::lock_guard<std::mutex> guard(state->error_mutex);
                if (state->first_error.isEmpty()) {
                    state->first_error = QString::fromUtf8(
                        videovault::core::user_message(outcome.error().code).data());
                    state->stop.store(true);
                }
            } else {
                ++removed;
            }
        });

    if (state->first_error.isEmpty()) {
        emit finished(true,
            QStringLiteral("Removed %1 file(s).").arg(removed.load()), removed.load());
    } else {
        emit finished(false, state->first_error, removed.load());
    }
}

void BatchWorker::copyFromPeer(
    std::shared_ptr<PeerCopyPlan> plan,
    std::shared_ptr<PeerLink> peer,
    std::shared_ptr<videovault::core::Vault> vault) {
    // Each byte is counted twice: once arriving from the phone, once sealed.
    const auto total = std::max<qlonglong>(1, static_cast<qlonglong>(plan->total_bytes) * 2);
    qlonglong done = 0;
    const int fileCount = static_cast<int>(plan->files.size());

    std::unordered_map<std::int64_t, std::int64_t> made;
    const auto localFolder = [&made, &plan](const std::int64_t phoneFolder) {
        const auto found = made.find(phoneFolder);
        return found == made.end() ? plan->destination : found->second;
    };
    for (const auto& folder : plan->folders) {
        auto local = folderIn(*vault, localFolder(folder.parent_id), folder.name);
        if (!local) {
            emit finished(false, QStringLiteral("Could not create the folder \"%1\": %2")
                .arg(folder.name, QString::fromUtf8(
                    videovault::core::user_message(local.error().code).data())), 0);
            return;
        }
        made[folder.id] = local.value();
        emit folderArrived(
            static_cast<qlonglong>(local.value()),
            static_cast<qlonglong>(localFolder(folder.parent_id)),
            folder.name);
    }

    QTemporaryDir staging;
    if (!staging.isValid()) {
        emit finished(false, QStringLiteral("Could not create a temporary folder for the copy."), 0);
        return;
    }

    int copied = 0;
    QString failure;
    for (int index = 0; index < fileCount && failure.isEmpty(); ++index) {
        const PeerFile& file = plan->files[static_cast<std::size_t>(index)];
        // The vault names the item after the imported file, so keep the name.
        const QString dir = staging.filePath(QString::number(index));
        QDir().mkpath(dir);
        const QString path = QDir(dir).filePath(safeFileName(file.name));
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            failure = QStringLiteral("Could not write a temporary copy of \"%1\".").arg(file.name);
            break;
        }
        std::uint64_t offset = 0U;
        while (offset < file.size) {
            if (plan->stop && plan->stop->load()) {
                failure = QStringLiteral("Canceled.");
                break;
            }
            const auto ask = static_cast<int>(std::min<std::uint64_t>(kPeerChunkBytes, file.size - offset));
            QJsonObject request;
            request.insert(QStringLiteral("op"), QStringLiteral("read"));
            request.insert(QStringLiteral("id"), static_cast<double>(file.id));
            request.insert(QStringLiteral("offset"), static_cast<double>(offset));
            request.insert(QStringLiteral("size"), ask);
            const auto reply = peer->call(request, 60000);
            if (!reply || reply->status != 200 || reply->body.isEmpty()) {
                failure = peer->linked()
                    ? QStringLiteral("The phone stopped answering while copying \"%1\".").arg(file.name)
                    : QStringLiteral("The phone disconnected while copying \"%1\".").arg(file.name);
                break;
            }
            if (out.write(reply->body) != reply->body.size()) {
                failure = QStringLiteral("Could not write a temporary copy of \"%1\".").arg(file.name);
                break;
            }
            offset += static_cast<std::uint64_t>(reply->body.size());
            done += reply->body.size();
            emit progress(std::min(done, total), total);
        }
        out.close();
        if (!failure.isEmpty()) {
            QFile::remove(path);
            break;
        }

        const auto reported = std::make_shared<qlonglong>(0);
        const auto base = done;
        bool duplicate = false;
        auto imported = vault->import_file(
            std::filesystem::path(path.toStdWString()),
            [this, reported, base, total, size = file.size](const double fraction) {
                const auto now = static_cast<qlonglong>(
                    std::clamp(fraction, 0.0, 1.0) * static_cast<double>(size));
                if (now <= *reported) return;
                *reported = now;
                emit progress(std::min(base + now, total), total);
            },
            localFolder(file.folder_id), &duplicate);
        QFile::remove(path);
        if (!imported) {
            failure = QStringLiteral("\"%1\": %2").arg(file.name, QString::fromUtf8(
                videovault::core::user_message(imported.error().code).data()));
            break;
        }
        done = base + static_cast<qlonglong>(file.size);
        if (!duplicate) {
            ++copied;
            const auto when = static_cast<qlonglong>(std::chrono::duration_cast<
                std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            emit videoArrived(
                static_cast<qlonglong>(imported.value()),
                file.name,
                static_cast<qlonglong>(file.size),
                static_cast<qlonglong>(localFolder(file.folder_id)),
                when);
        }
        emit fileFinished(copied, fileCount);
    }

    if (failure.isEmpty()) {
        emit finished(true, QStringLiteral("Copied %1 file(s) and %2 folder(s) from %3.")
            .arg(copied).arg(plan->folders.size()).arg(plan->phone), copied);
    } else {
        emit finished(false, failure, copied);
    }
}

QString packageFailure(const videovault::core::VaultError& error) {
    if (!error.technical_detail.empty()) {
        return QString::fromStdString(error.technical_detail);
    }
    return QString::fromUtf8(videovault::core::user_message(error.code).data());
}

void BatchWorker::exportPackage(
    std::shared_ptr<videovault::core::Vault> vault,
    std::shared_ptr<std::filesystem::path> destination,
    std::shared_ptr<std::string> password,
    std::shared_ptr<std::vector<videovault::core::Vault::PackageItem>> files,
    std::shared_ptr<std::vector<std::string>> directories,
    bool compress) {
    const auto outcome = vault->export_package(
        *destination, *password, *files, *directories, compress,
        [this](const double fraction) {
            const auto done = static_cast<qlonglong>(
                std::clamp(fraction, 0.0, 1.0) * 1000.0);
            emit progress(done, 1000);
        });
    password->assign(password->size(), '\0');
    password->clear();
    if (!outcome) {
        emit finished(false, packageFailure(outcome.error()), 0);
        return;
    }
    const auto count = static_cast<int>(files->size());
    emit finished(true, count == 0
        ? QStringLiteral("Exported the package.")
        : QStringLiteral("Exported %1 file(s).").arg(count), count);
}

void BatchWorker::importPackage(
    std::shared_ptr<videovault::core::Vault> vault,
    std::shared_ptr<std::filesystem::path> source,
    std::shared_ptr<std::string> password,
    std::int64_t folder_id) {
    const auto when = static_cast<qlonglong>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto outcome = vault->import_package(
        *source, *password, folder_id,
        [this](const double fraction) {
            const auto done = static_cast<qlonglong>(
                std::clamp(fraction, 0.0, 1.0) * 1000.0);
            emit progress(done, 1000);
        },
        [this, when](const std::int64_t id, const std::string& name,
            const std::uint64_t size, const std::int64_t parent) {
            emit videoArrived(
                static_cast<qlonglong>(id),
                QString::fromStdString(name),
                static_cast<qlonglong>(size),
                static_cast<qlonglong>(parent),
                when);
        },
        [this](const std::int64_t id, const std::int64_t parent, const std::string& name) {
            emit folderArrived(
                static_cast<qlonglong>(id),
                static_cast<qlonglong>(parent),
                QString::fromStdString(name));
        });
    password->assign(password->size(), '\0');
    password->clear();
    if (!outcome) {
        emit finished(false, packageFailure(outcome.error()), 0);
        return;
    }
    emit finished(true, QStringLiteral("Imported %1 new file(s).").arg(outcome.value()),
        outcome.value());
}

} // namespace videovault::app
