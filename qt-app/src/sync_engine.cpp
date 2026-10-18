#include "videovault/app/sync_engine.hpp"

#include "videovault/app/sync_client.hpp"
#include "videovault/core/vault.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <sodium.h>

namespace videovault::app {

namespace {

using sync::Failure;
using sync::RecordRef;

constexpr qint64 kMaxPackage = 1024LL * 1024 * 1024;  // the server's per-package limit
constexpr qint64 kChunk = 1024 * 1024;                 // the server's GET range limit
constexpr char kOutgoing[] = "/outgoing";
constexpr char kStaging[] = "/staging";

[[noreturn]] void fail(const QString& message) { throw Failure(message); }

std::filesystem::path fsPath(const QString& path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::u8path(path.toUtf8().constData());
#endif
}

QString qtPath(const std::filesystem::path& path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.c_str()), int(utf8.size()));
#endif
}

QString coreMessage(core::VaultErrorCode code) {
    const std::string_view text = core::user_message(code);
    return QString::fromUtf8(text.data(), int(text.size()));
}

bool ensureDirectory(const QString& path) {
    if (!QDir().mkpath(path)) return false;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

QString packageHex(const core::VideoInfo& video) {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(video.package_sha256.data()), 32).toHex());
}

QStringList pathParts(const QString& path) {
    QStringList parts;
    for (const auto& part : path.split(QLatin1Char('/'))) {
        if (!part.isEmpty()) parts.append(part);
    }
    return parts;
}

QString folderPathOf(const core::VideoInfo& video, const std::vector<core::FolderInfo>& folders) {
    QStringList parts;
    std::int64_t parent = video.folder_id;
    QSet<std::int64_t> visited;
    while (parent != 0) {
        if (visited.contains(parent)) return {};  // a cycle: the caller reports it
        visited.insert(parent);
        bool found = false;
        for (const auto& folder : folders) {
            if (folder.id != parent) continue;
            parts.prepend(QString::fromStdString(folder.name));
            parent = folder.parent_id;
            found = true;
            break;
        }
        if (!found) return {};
    }
    return parts.join(QLatin1Char('/'));
}

QString sortedTags(const QStringList& tags) {
    QStringList clean = tags;
    clean.removeDuplicates();
    std::sort(clean.begin(), clean.end(), [](const QString& left, const QString& right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return clean.join(QLatin1Char(','));
}

// What the dialog lists: every local item with its record state, and every
// record that has no local copy yet (pending, or removed on another device).
QJsonArray buildItems(const std::shared_ptr<core::Vault>& vault, const sync::SyncState& state) {
    QJsonArray rows;
    auto videos = vault->list_videos();
    auto folders = vault->list_folders();
    if (!videos || !folders) return rows;
    QSet<qint64> seen;
    for (const auto& video : videos.value()) {
        const qint64 localId = video.id;
        seen.insert(localId);
        const RecordRef* live = state.liveRecordFor(localId);
        QString recordId;
        QString stateText = QStringLiteral("queued");
        QString package;
        int version = 0;
        if (live != nullptr) {
            recordId = live->id;
            stateText = QStringLiteral("synced");
            package = live->package;
            version = live->version;
        } else {
            for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
                if (entry->localId != localId) continue;
                recordId = entry->id;
                stateText = QStringLiteral("removed");
                package = entry->package;
                version = entry->version;
                break;
            }
        }
        rows.append(QJsonObject{{QStringLiteral("record"), recordId},
                                {QStringLiteral("local"), double(localId)},
                                {QStringLiteral("name"), QString::fromStdString(video.display_name)},
                                {QStringLiteral("path"), folderPathOf(video, folders.value())},
                                {QStringLiteral("state"), stateText},
                                {QStringLiteral("package"), package},
                                {QStringLiteral("size"), double(video.package_size)},
                                {QStringLiteral("version"), version}});
    }
    for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
        if (entry->localId != 0 && seen.contains(entry->localId)) continue;
        const QString stateText = entry->deleted ? QStringLiteral("removed") : QStringLiteral("pending");
        rows.append(QJsonObject{{QStringLiteral("record"), entry->id},
                                {QStringLiteral("local"), 0.0},
                                {QStringLiteral("name"), entry->name},
                                {QStringLiteral("path"), entry->path},
                                {QStringLiteral("state"), stateText},
                                {QStringLiteral("package"), entry->package},
                                {QStringLiteral("size"), double(entry->size)},
                                {QStringLiteral("version"), entry->version}});
    }
    return rows;
}

struct TransferRow {
    QString name;
    QString direction;
    QString state;
    qint64 done{0};
    qint64 total{0};
};

}  // namespace

// Everything one operation needs, built from an immutable snapshot so the
// worker never touches the engine object. The vault and the client live and die
// inside the worker thread. It is at namespace scope because the engine holds it
// in a shared_ptr<Job> that the UI thread reads while the worker runs.
class Job {
public:
    Job(std::shared_ptr<core::Vault> vault, sync::Keys keys, QString directory, sync::SyncState state,
        int pageLimit)
        : vault_(std::move(vault)), keys_(std::move(keys)), directory_(std::move(directory)),
          state(std::move(state)), pageLimit_(pageLimit) {}

    ~Job() { keys_.wipe(); }

    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    void stop() { stopped_.store(true); }

    sync::SyncState state;
    QJsonArray items;
    QJsonArray vaults;
    QJsonObject stats;
    QString status;
    bool failed{false};
    bool libraryChanged{false};

    QJsonArray transfersSnapshot() const {
        QMutexLocker lock(&transfersMutex_);
        QJsonArray rows;
        for (const auto& row : transfers_) {
            rows.append(QJsonObject{{QStringLiteral("name"), row.name},
                                    {QStringLiteral("direction"), row.direction},
                                    {QStringLiteral("state"), row.state},
                                    {QStringLiteral("done"), double(row.done)},
                                    {QStringLiteral("total"), double(row.total)}});
        }
        return rows;
    }

    // -- operations --------------------------------------------------------

    void signIn(sync::Client& client, const QString& email, const QString& password,
                const QString& label) {
        bump("signedin");
        // The address and the email are kept even when the attempt fails, so a
        // typo in the password does not clear a working server URL. A token
        // from another server is never offered to this one.
        if (state.server != client.server()) state.clearToken();
        state.server = client.server();
        state.email = email;
        client.protocol();
        client.login(email, password);
        const QJsonObject issued = client.issueToken(label);
        const QString token = client.token();
        if (token.isEmpty()) fail(QStringLiteral("The server did not return a device token."));
        state.clearToken();
        state.token = token;
        state.tokenId = issued.value(QStringLiteral("id")).toString();
        save();
        listVaults(client);
        status = QStringLiteral("Signed in as %1.").arg(email);
    }

    void listVaults(sync::Client& client) {
        const QJsonObject body = client.vaults();
        if (!body.value(QStringLiteral("vaults")).isArray()) {
            fail(QStringLiteral("The server returned an invalid vault list."));
        }
        vaults = body.value(QStringLiteral("vaults")).toArray();
        bump("vaults");
        if (status.isEmpty()) {
            status = vaults.isEmpty() ? QStringLiteral("This account has no vault yet. Create one on the website.")
                                      : QStringLiteral("%1 vault(s) on this account.").arg(vaults.size());
        }
    }

    void syncPass(sync::Client& client) {
        client.protocol();
        if (state.vaultId.isEmpty()) fail(QStringLiteral("Choose a vault first."));
        if (state.folder.isEmpty()) fail(QStringLiteral("Choose the local folder to sync first."));

        publishLocal(client);
        pull(client);
        receiveQueued(client);
        tombstoneMissing(client);
        items = buildItems(vault_, state);
        status = summary();
    }

    void receiveOne(sync::Client& client, const QString& recordId) {
        if (state.vaultId.isEmpty()) fail(QStringLiteral("Choose a vault first."));
        auto it = state.records.find(recordId);
        if (it == state.records.end()) fail(QStringLiteral("That item is not known to this device yet; sync first."));
        downloadRecord(client, recordId);
        items = buildItems(vault_, state);
        status = summary();
    }

    void tombstoneOne(sync::Client& client, const QString& recordId) {
        if (state.vaultId.isEmpty()) fail(QStringLiteral("Choose a vault first."));
        const RecordRef answer = client.tombstone(state.vaultId, recordId);
        auto it = state.records.find(recordId);
        if (it != state.records.end()) {
            it->deleted = true;
            if (answer.version > 0) it->version = answer.version;
            if (answer.updated > 0) it->updated = answer.updated;
        }
        state.pendingDownload.removeAll(recordId);
        bump("tombstoned");
        save();
        items = buildItems(vault_, state);
        status = QStringLiteral("Removed on every device. It stays in Removed items until it is deleted permanently.");
    }

    void restoreOne(sync::Client& client, const QString& recordId) {
        if (state.vaultId.isEmpty()) fail(QStringLiteral("Choose a vault first."));
        const RecordRef answer = client.restore(state.vaultId, recordId);
        auto it = state.records.find(recordId);
        if (it == state.records.end()) {
            state.records.insert(answer.id, answer);
        } else {
            it->deleted = false;
            if (answer.version > 0) it->version = answer.version;
            if (answer.updated > 0) it->updated = answer.updated;
            if (it->localId == 0 && !it->package.isEmpty()) queueDownload(it->id);
        }
        bump("restored");
        save();
        items = buildItems(vault_, state);
        status = QStringLiteral("Restored. It will appear on the other devices on their next sync.");
    }

    void purgeOne(sync::Client& client, const QString& recordId) {
        if (state.vaultId.isEmpty()) fail(QStringLiteral("Choose a vault first."));
        const auto it = state.records.find(recordId);
        const qint64 localId = it == state.records.end() ? 0 : it->localId;
        client.purge(state.vaultId, recordId);
        state.records.remove(recordId);
        state.pendingDownload.removeAll(recordId);
        // The local copy goes only when no other record still claims it, so
        // "delete permanently" can never take an item that is still shared.
        if (localId != 0 && state.liveRecordFor(localId) == nullptr && !claimedByOther(localId)) {
            if (vault_->remove_video(localId).has_value()) libraryChanged = true;
        }
        bump("purged");
        save();
        items = buildItems(vault_, state);
        status = QStringLiteral("Deleted permanently, on the server and on this device.");
    }

private:
    // -- state helpers -----------------------------------------------------

    // The .mvpk password this vault shares: stored in the state file so that
    // changing the vault password never orphans packages already uploaded.
    QString packageSecret() const {
        return state.packageKey.isEmpty() ? keys_.package : state.packageKey;
    }

    void bump(const char* counter) {
        const QString name = QString::fromLatin1(counter);
        stats.insert(name, stats.value(name).toInt() + 1);
    }

    void checkStop() const {
        if (stopped_.load()) throw sync::Cancelled{};
    }

    void save() const { sync::saveState(directory_ + QStringLiteral("/state.enc"), keys_.state, state); }

    QString outgoing() const { return directory_ + QString::fromLatin1(kOutgoing); }
    QString staging() const { return directory_ + QString::fromLatin1(kStaging); }

    bool claimedByOther(qint64 localId) const {
        for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
            if (entry->localId == localId) return true;
        }
        return false;
    }

    void queueDownload(const QString& recordId) {
        if (!state.pendingDownload.contains(recordId)) state.pendingDownload.append(recordId);
    }

    void dropPending(const QString& recordId) { state.pendingDownload.removeAll(recordId); }

    // Content addressing is what binds a record to a local item: the package
    // hash identifies the item even when the import deduplicated it.
    // The local item's own package hash, read fresh from the library.
    QString localPackageOf(qint64 localId) const {
        auto videos = vault_->list_videos();
        if (!videos) return {};
        for (const auto& video : videos.value()) {
            if (video.id == localId) return packageHex(video);
        }
        return {};
    }

    // A record whose package this device already holds through another record:
    // that is how a conflict copy binds without downloading the same bytes
    // again, because content addressing makes the package shared.
    qint64 localIdForServerPackage(const QString& package) const {
        if (package.isEmpty()) return 0;
        for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
            if (entry->package == package && entry->localId != 0 && itemExists(entry->localId)) {
                return entry->localId;
            }
        }
        return 0;
    }

    qint64 localIdByName(const QString& name) const {
        auto videos = vault_->list_videos();
        if (!videos) return 0;
        qint64 found = 0;
        for (const auto& video : videos.value()) {
            if (QString::fromStdString(video.display_name) != name) continue;
            if (found != 0) return 0;  // two items share the name: do not guess
            found = video.id;
        }
        return found;
    }

    bool itemExists(qint64 localId) const {
        auto videos = vault_->list_videos();
        if (!videos) return false;
        for (const auto& video : videos.value()) {
            if (video.id == localId) return true;
        }
        return false;
    }

    // -- reading the local library ----------------------------------------

    QStringList tagsOf(qint64 localId) const {
        QStringList tags;
        auto list = vault_->tags_for_video(localId);
        if (!list) return tags;
        for (const auto& tag : list.value()) tags.append(QString::fromStdString(tag.name));
        return tags;
    }

    std::int64_t folderFor(const QString& path) {
        std::int64_t parent = 0;
        for (const auto& part : pathParts(path)) {
            auto folders = vault_->list_folders();
            if (!folders) fail(QStringLiteral("Cannot read the local folders."));
            std::int64_t found = 0;
            for (const auto& folder : folders.value()) {
                if (folder.parent_id == parent && QString::fromStdString(folder.name) == part) {
                    found = folder.id;
                    break;
                }
            }
            if (found == 0) {
                auto created = vault_->create_folder(parent, part.toStdString());
                if (!created) {
                    fail(QStringLiteral("Cannot create the folder \"%1\": %2")
                             .arg(part, coreMessage(created.error().code)));
                }
                found = created.value();
            }
            parent = found;
        }
        return parent;
    }

    // -- upload --------------------------------------------------------

    QString exportItem(const core::VideoInfo& video, const QString& relative) {
        if (!ensureDirectory(outgoing())) fail(QStringLiteral("Cannot create the sync directory."));
        const QString temporary =
            outgoing() + QStringLiteral("/export-") + sync::randomHex(8) + QStringLiteral(".mvpk");
        auto exported = vault_->export_package(fsPath(temporary), packageSecret().toStdString(),
                                               {{video.id, relative.toStdString()}}, {}, false);
        if (!exported) {
            QFile::remove(temporary);
            fail(QStringLiteral("Cannot export \"%1\": %2")
                     .arg(QString::fromStdString(video.display_name), coreMessage(exported.error().code)));
        }
        checkStop();
        const QString package = sync::fileSha256(temporary);
        const QString stored = outgoing() + QLatin1Char('/') + package + QStringLiteral(".mvpk");
        QFile::remove(stored);
        if (!QFile::rename(temporary, stored)) {
            QFile::remove(temporary);
            fail(QStringLiteral("Cannot keep the exported package in the sync directory."));
        }
        return package;
    }

    void publishLocal(sync::Client& client) {
        auto videos = vault_->list_videos();
        if (!videos) fail(QStringLiteral("Cannot read the local library."));
        auto folders = vault_->list_folders();
        if (!folders) fail(QStringLiteral("Cannot read the local folders."));

        for (const auto& video : videos.value()) {
            checkStop();
            const qint64 localId = video.id;
            const QString name = QString::fromStdString(video.display_name);
            const QString path = folderPathOf(video, folders.value());
            const QString package = packageHex(video);
            const QStringList tags = tagsOf(localId);
            const QVector<const RecordRef*> claims = liveClaims(localId);
            if (!claims.isEmpty()) {
                bool matched = false;
                for (const auto* claim : claims) {
                    if (claim->localPackage == package && claim->name == name && claim->path == path
                        && sortedTags(claim->tags) == sortedTags(tags)) {
                        matched = true;
                    }
                }
                if (matched) {
                    bump("unchanged");
                    items = buildItems(vault_, state);  // keep the panel honest while it walks
                    continue;
                }
                if (claims.size() > 1) {
                    // A conflict copy already carries this content: republishing
                    // would mint another copy on every pass.
                    bump("conflict held");
                    continue;
                }
                publish(client, video, name, path, tags, *claims.first());
            } else if (hasClaim(localId)) {
                // Only a tombstone claims it: the user has to restore it, so a
                // pass never resurrects something another device removed.
                bump("removed");
                continue;
            } else {
                publish(client, video, name, path, tags, RecordRef{});
            }
            items = buildItems(vault_, state);
        }
        if (!videos.value().empty()) save();
    }

    QVector<const RecordRef*> liveClaims(qint64 localId) const {
        QVector<const RecordRef*> claims;
        for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
            if (entry->localId == localId && !entry->deleted) claims.append(&entry.value());
        }
        return claims;
    }

    bool hasClaim(qint64 localId) const {
        for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
            if (entry->localId == localId) return true;
        }
        return false;
    }

    // Exports, uploads the blob, then writes the record. A stale base_version
    // becomes a conflict copy: the local version is re-issued under a new
    // record id, so neither side is overwritten and nothing is lost.
    void publish(sync::Client& client, const core::VideoInfo& video, const QString& name,
                 const QString& path, const QStringList& tags, const RecordRef& existing) {
        const bool update = !existing.id.isEmpty();
        const QString localPackage = packageHex(video);
        // A rename, a move or a tag change does not touch the bytes: the record
        // keeps pointing at the package the server already has (exporting again
        // would mint a second copy of the same content, because a package is
        // sealed with a fresh nonce every time).
        const bool bytesSame = update && !existing.package.isEmpty() && existing.localPackage == localPackage;
        const QString relative = path.isEmpty() ? name : path + QLatin1Char('/') + name;
        const int index = beginTransfer(name, QStringLiteral("Upload"), 0);
        QString source;
        QString remotePackage = existing.package;
        qint64 remoteSize = existing.size;
        try {
            if (!bytesSame) {
                const QString package = exportItem(video, relative);
                source = outgoing() + QLatin1Char('/') + package + QStringLiteral(".mvpk");
                const qint64 size = QFileInfo(source).size();
                updateTransfer(index, 0, size);

                // Blob first: a record never points at a package the server
                // does not have, so a reader never sees a dangling row.
                client.putBlob(state.vaultId, package, source,
                               [&](qint64 done, qint64 total) {
                                   updateTransfer(index, done, total);
                                   return !stopped_.load();
                               });
                remotePackage = package;
                remoteSize = size;
            }

            RecordRef record;
            record.id = update ? existing.id : sync::newRecordId();
            record.name = name;
            record.path = path;
            record.tags = tags;
            record.package = remotePackage;
            record.size = remoteSize;
            record.localId = video.id;
            const int base = update ? existing.version : 0;
            writeRecord(client, record, base, video.id, localPackage, index);
            if (!source.isEmpty()) QFile::remove(source);
        } catch (const sync::Cancelled&) {
            if (!source.isEmpty()) QFile::remove(source);
            endTransfer(index, QStringLiteral("stopped"));
            throw;
        } catch (...) {
            if (!source.isEmpty()) QFile::remove(source);
            endTransfer(index, QStringLiteral("failed"));
            throw;
        }
    }

    void writeRecord(sync::Client& client, RecordRef record, int base, qint64 localId,
                     const QString& localPackage, int index) {
        const QString operation = sync::operationId(record.id, base, record.package, record.name,
                                                    record.path, record.tags);
        try {
            RecordRef stored = client.write(state.vaultId, record, base, operation);
            stored.localId = localId;
            stored.localPackage = localPackage;
            state.records.insert(stored.id, stored);
            bump(base == 0 ? "uploaded" : "updated");
            endTransfer(index, QStringLiteral("done"));
            save();
            return;
        } catch (const sync::Conflict& conflict) {
            if (base == 0) {
                // A brand new id colliding is not a version race; retry once
                // with a fresh id rather than giving up on the item.
                record.id = sync::newRecordId();
                const QString retry = sync::operationId(record.id, 0, record.package, record.name,
                                                        record.path, record.tags);
                RecordRef stored = client.write(state.vaultId, record, 0, retry);
                stored.localId = localId;
                stored.localPackage = localPackage;
                state.records.insert(stored.id, stored);
                bump("uploaded");
                endTransfer(index, QStringLiteral("done"));
                save();
                return;
            }
            // MEGA's answer: keep both. The local version goes up under a new
            // record id with the suffix, and the server's row is left alone.
            record.id = sync::newRecordId();
            record.name = sync::conflictCopyName(record.name);
            record.localId = localId;
            const QString copyOperation = sync::operationId(record.id, 0, record.package, record.name,
                                                            record.path, record.tags);
            RecordRef stored = client.write(state.vaultId, record, 0, copyOperation);
            stored.localId = localId;
            stored.localPackage = localPackage;
            state.records.insert(stored.id, stored);
            if (conflict.present) {
                auto current = state.records.find(conflict.current.id);
                if (current == state.records.end()) {
                    RecordRef kept = conflict.current;
                    kept.localId = kept.package == record.package ? localId : 0;
                    state.records.insert(kept.id, kept);
                } else {
                    current->version = conflict.current.version;
                    current->name = conflict.current.name;
                    current->updated = conflict.current.updated;
                }
            }
            bump("conflicts");
            endTransfer(index, QStringLiteral("conflict copy"));
            save();
            const QString serverName = conflict.present ? conflict.current.name : record.name;
            status = QStringLiteral("The server already had a newer version of \"%1\". Both were kept: "
                                    "this device's copy is \"%2\".")
                         .arg(serverName, stored.name);
        }
    }

    // -- pull ----------------------------------------------------------

    void pull(sync::Client& client) {
        const int limit = qBound(1, pageLimit_, 1000);
        int pages = 0;
        for (;;) {
            checkStop();
            const qint64 before = state.cursor;
            const sync::Client::Page page = client.records(state.vaultId, state.cursor, limit);
            bump("pages");
            for (const auto& record : page.records) {
                checkStop();
                apply(record);
            }
            if (page.cursor > state.cursor) state.cursor = page.cursor;
            save();
            if (page.records.size() < limit) break;
            if (state.cursor <= before) break;
            if (++pages > 100000) fail(QStringLiteral("The server kept returning pages of records."));
        }
        stats.insert(QStringLiteral("records"), double(state.records.size()));
    }

    void apply(const RecordRef& incoming) {
        RecordRef record = incoming;
        const auto existing = state.records.constFind(record.id);
        bool contentChanged = false;
        if (existing != state.records.constEnd()) {
            record.localId = existing->localId;
            record.localPackage = existing->localPackage;
            if (existing->version == record.version && existing->deleted == record.deleted
                && existing->package == record.package && existing->name == record.name
                && existing->path == record.path && sortedTags(existing->tags) == sortedTags(record.tags)) {
                bump("unchanged_record");  // already present and matching: untouched
                return;
            }
            // The same record with a different package means the content was
            // edited on another device: the local bytes are stale.
            contentChanged = !existing->package.isEmpty() && existing->package != record.package;
            if (record.localId != 0 && !itemExists(record.localId)) record.localId = 0;
        }
        state.records.insert(record.id, record);
        bump("changed_record");

        if (record.deleted) {
            dropPending(record.id);
            return;
        }
        if (record.package.isEmpty()) return;
        if (contentChanged || record.localId == 0 || !itemExists(record.localId)) {
            queueDownload(record.id);
            return;
        }
        dropPending(record.id);
        record.localPackage = localPackageOf(record.localId);
        state.records.insert(record.id, record);
        reconcileLocal(record);
    }

    // The record is a label for a local item: a rename, a move or a tag change
    // on another device lands here. When two records claim the same item (a
    // conflict copy) nothing is forced, so neither side's name is lost.
    void reconcileLocal(const RecordRef& record) {
        int claims = 0;
        for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
            if (entry->localId == record.localId && !entry->deleted) ++claims;
        }
        if (claims > 1) return;

        auto videos = vault_->list_videos();
        auto folders = vault_->list_folders();
        if (!videos || !folders) return;
        const core::VideoInfo* found = nullptr;
        for (const auto& video : videos.value()) {
            if (video.id == record.localId) { found = &video; break; }
        }
        if (found == nullptr) return;
        if (QString::fromStdString(found->display_name) != record.name) {
            vault_->rename_video(record.localId, record.name.toStdString());
            libraryChanged = true;
        }
        const QString path = folderPathOf(*found, folders.value());
        if (path != record.path) {
            const std::int64_t folder = folderFor(record.path);
            if (vault_->move_video(record.localId, folder).has_value()) libraryChanged = true;
        }
        // Tags travel with the record, so a tag added on another device lands.
        const QStringList local = tagsOf(record.localId);
        auto localTags = vault_->tags_for_video(record.localId);
        if (localTags) {
            for (const auto& tag : localTags.value()) {
                const QString name = QString::fromStdString(tag.name);
                bool keep = false;
                for (const auto& wanted : record.tags) {
                    if (wanted.compare(name, Qt::CaseInsensitive) == 0) keep = true;
                }
                if (!keep) vault_->remove_tag(record.localId, tag.id);
            }
        }
        for (const auto& tag : record.tags) {
            bool present = false;
            for (const auto& have : local) {
                if (have.compare(tag, Qt::CaseInsensitive) == 0) present = true;
            }
            if (!present) vault_->add_tag(record.localId, tag.toStdString());
        }
    }

    // -- download ------------------------------------------------------

    void receiveQueued(sync::Client& client) {
        const QStringList queued = state.pendingDownload;
        for (const auto& id : queued) {
            checkStop();
            downloadRecord(client, id);
            items = buildItems(vault_, state);
        }
    }

    void downloadRecord(sync::Client& client, const QString& recordId) {
        auto it = state.records.find(recordId);
        if (it == state.records.end()) {
            dropPending(recordId);
            return;
        }
        if (it->deleted) {
            dropPending(recordId);
            return;
        }
        if (it->package.isEmpty()) {
            dropPending(recordId);
            return;
        }
        if (it->localId != 0 && itemExists(it->localId)) {
            dropPending(recordId);
            return;
        }
        const RecordRef record = it.value();
        const qint64 previousLocal = record.localId;
        if (!sync::isPackage(record.package)) fail(QStringLiteral("A record carries an invalid package address."));

        const int index = beginTransfer(record.name, QStringLiteral("Download"), record.size);
        try {
            const sync::Client::BlobState head = client.head(state.vaultId, record.package);
            if (!head.exists) {
                // The record was published before its bytes: wait, do not fail.
                bump("waiting");
                endTransfer(index, QStringLiteral("waiting for the other device"));
                return;
            }
            if (!head.complete) {
                bump("waiting");
                endTransfer(index, QStringLiteral("waiting for the other device"));
                return;
            }
            if (head.size <= 0 || head.size > kMaxPackage) fail(QStringLiteral("The server reported an invalid package size."));
            if (!ensureDirectory(staging())) fail(QStringLiteral("Cannot create the sync directory."));

            const QString partial = staging() + QLatin1Char('/') + record.package + QStringLiteral(".part");
            QFile file(partial);
            if (!file.open(QIODevice::ReadWrite)) fail(QStringLiteral("Cannot open the download staging file."));
            qint64 offset = file.size();
            if (offset > head.size) {
                file.resize(0);
                offset = 0;
            }
            if (offset > 0) bump("resumed");
            updateTransfer(index, offset, head.size);
            while (offset < head.size) {
                checkStop();
                const qint64 last = qMin<qint64>(offset + kChunk, head.size - 1);
                const QByteArray chunk = client.getRange(state.vaultId, record.package, offset, last, head.size);
                if (!file.seek(offset) || file.write(chunk) != chunk.size() || !file.flush()) {
                    fail(QStringLiteral("Cannot write the downloaded package; check free space."));
                }
                offset += chunk.size();
                updateTransfer(index, offset, head.size);
            }
            file.close();
            // Verify before the core is allowed to import a single byte.
            if (sync::fileSha256(partial) != record.package) {
                QFile::remove(partial);
                fail(QStringLiteral("A downloaded package does not match its address; it was discarded and will be fetched again."));
            }
            importPackage(partial, record, previousLocal);
            QFile::remove(partial);
            bump("downloaded");
            endTransfer(index, QStringLiteral("done"));
        } catch (const sync::Cancelled&) {
            endTransfer(index, QStringLiteral("stopped"));  // the partial stays for the next pass
            throw;
        } catch (...) {
            endTransfer(index, QStringLiteral("failed"));
            throw;
        }
    }

    // Imports the verified package and binds the record to the item it landed
    // in. Importing re-seals the content, so the item's own package hash is not
    // the server's: the ids the core reports are what identify it, with a name
    // and shared-package fallback for content the vault already had.
    void importPackage(const QString& packagePath, const RecordRef& record, qint64 previousLocal) {
        std::vector<std::int64_t> stored;
        auto imported = vault_->import_package(
            fsPath(packagePath), packageSecret().toStdString(), 0, {},
            [&stored](std::int64_t id, const std::string&, std::uint64_t, std::int64_t) {
                stored.push_back(id);
            });
        if (!imported) {
            fail(QStringLiteral("Cannot import a package: %1").arg(coreMessage(imported.error().code)));
        }
        libraryChanged = true;
        qint64 localId = stored.empty() ? 0 : stored.front();
        if (localId == 0) localId = localIdForServerPackage(record.package);
        if (localId == 0) localId = localIdByName(record.name);
        if (localId == 0) fail(QStringLiteral("The imported package did not appear in the library."));
        auto it = state.records.find(record.id);
        if (it == state.records.end()) {
            RecordRef bound = record;
            bound.localId = localId;
            bound.localPackage = localPackageOf(localId);
            state.records.insert(bound.id, bound);
        } else {
            it->localId = localId;
            it->localPackage = localPackageOf(localId);
        }
        bump("imported");
        dropPending(record.id);
        // A record that arrived with new content replaces the item it used to
        // map to, but only when nothing else still claims that item.
        if (previousLocal != 0 && previousLocal != localId && !claimedByOther(previousLocal)
            && state.liveRecordFor(previousLocal) == nullptr) {
            if (vault_->remove_video(previousLocal).has_value()) libraryChanged = true;
        }
        const RecordRef bound = state.records.value(record.id);
        reconcileLocal(bound);
        save();
    }

    // -- deletes -------------------------------------------------------

    void tombstoneMissing(sync::Client& client) {
        auto videos = vault_->list_videos();
        if (!videos) return;
        QSet<qint64> present;
        for (const auto& video : videos.value()) present.insert(video.id);
        QStringList gone;
        for (auto entry = state.records.cbegin(); entry != state.records.cend(); ++entry) {
            if (entry->deleted || entry->localId == 0) continue;
            if (present.contains(entry->localId)) continue;
            gone.append(entry->id);
        }
        for (const auto& id : gone) {
            checkStop();
            const RecordRef answer = client.tombstone(state.vaultId, id);
            auto entry = state.records.find(id);
            if (entry == state.records.end()) continue;
            entry->deleted = true;
            if (answer.version > 0) entry->version = answer.version;
            bump("tombstoned");
            save();
        }
    }

    // -- panel --------------------------------------------------------

    int beginTransfer(const QString& name, const QString& direction, qint64 total) {
        QMutexLocker lock(&transfersMutex_);
        transfers_.append(TransferRow{name, direction, QStringLiteral("queued"), 0, total});
        return transfers_.size() - 1;
    }

    void updateTransfer(int index, qint64 done, qint64 total) {
        QMutexLocker lock(&transfersMutex_);
        if (index < 0 || index >= transfers_.size()) return;
        transfers_[index].done = done;
        if (total > 0) transfers_[index].total = total;
        transfers_[index].state = QStringLiteral("transferring");
    }

    void endTransfer(int index, const QString& state) {
        QMutexLocker lock(&transfersMutex_);
        if (index < 0 || index >= transfers_.size()) return;
        transfers_[index].state = state;
        if (state == QStringLiteral("done")) transfers_[index].done = transfers_[index].total;
    }

    QString summary() const {
        const int uploaded = stats.value(QStringLiteral("uploaded")).toInt()
            + stats.value(QStringLiteral("updated")).toInt();
        const int imported = stats.value(QStringLiteral("imported")).toInt();
        const int waiting = stats.value(QStringLiteral("waiting")).toInt();
        const int conflicts = stats.value(QStringLiteral("conflicts")).toInt();
        const int removed = stats.value(QStringLiteral("tombstoned")).toInt();
        const int unchanged = stats.value(QStringLiteral("unchanged")).toInt();
        if (uploaded + imported + waiting + conflicts + removed == 0) {
            return QStringLiteral("Up to date: %1 item(s) already matched.").arg(unchanged);
        }
        QString text = QStringLiteral("Synced: %1 published, %2 received, %3 waiting for the other device.")
                           .arg(uploaded).arg(imported).arg(waiting);
        if (conflicts > 0) text += QStringLiteral(" %1 conflict copy(ies) kept both versions.").arg(conflicts);
        if (removed > 0) text += QStringLiteral(" %1 item(s) removed on another device.").arg(removed);
        return text;
    }

    std::shared_ptr<core::Vault> vault_;
    sync::Keys keys_;
    QString directory_;
    int pageLimit_;
    std::atomic_bool stopped_{false};
    mutable QMutex transfersMutex_;
    QVector<TransferRow> transfers_;
};

SyncEngine::SyncEngine(std::shared_ptr<core::Vault> vault, sync::Keys keys, const QString& privateDirectory,
                       QObject* parent)
    : QObject(parent), vault_(std::move(vault)), keys_(std::move(keys)) {
    QString canonical = QFileInfo(qtPath(vault_->root_path())).canonicalFilePath();
    if (canonical.isEmpty()) canonical = qtPath(vault_->root_path());
    directory_ = privateDirectory.isEmpty() ? stateDirectoryFor(canonical) : privateDirectory;
    if (!ensureDirectory(directory_)) {
        status_ = QStringLiteral("Cannot create the private sync directory.");
        failed_ = true;
    } else if (keys_.isEmpty()) {
        status_ = QStringLiteral("Sync is locked: unlock the vault again to derive its sync key.");
        failed_ = true;
    } else {
        try {
            load();
            status_ = state_.signedIn()
                ? QStringLiteral("Signed in as %1. Choose a vault, then press Sync.").arg(state_.email)
                : QStringLiteral("Sign in with the account that owns the vault.");
        } catch (const std::exception& error) {
            failed_ = true;
            status_ = QString::fromUtf8(error.what());
        }
    }
    refreshItems();

    // While an operation runs, the panel is redrawn from the worker's progress
    // snapshot; the timer stops as soon as the engine is idle again.
    pulse_ = new QTimer(this);
    pulse_->setObjectName(QStringLiteral("syncPulse"));
    pulse_->setInterval(150);
    connect(pulse_, &QTimer::timeout, this, [this] {
        if (busy_) emit changed();
        else if (pulse_ != nullptr && pulse_->isActive()) pulse_->stop();
    });
}

SyncEngine::~SyncEngine() {
    stop();
    keys_.wipe();
    state_.clearToken();
}

QString SyncEngine::stateDirectoryFor(const QString& vaultRoot) { return sync::stateDirectoryFor(vaultRoot); }

void SyncEngine::load() {
    const QString path = directory_ + QStringLiteral("/state.enc");
    bool fromLegacySeal = false;
    try {
        state_ = sync::loadState(path, keys_.state);
    } catch (const std::exception&) {
        // Written before the package key was stored: its sealing key came from
        // the package password. Open it once with that key and re-save.
        state_ = sync::loadState(path, keys_.legacyState);
        fromLegacySeal = true;
    }
    const QString adopted = sync::adoptedPackageKey(state_.packageKey, keys_);
    const bool gainedPackageKey = adopted != state_.packageKey;
    state_.packageKey = adopted;
    if (fromLegacySeal || gainedPackageKey) {
        // Persist the package key now: that is what lets a later password
        // change keep every package this device already shares.
        sync::saveState(path, keys_.state, state_);
    }
}

void SyncEngine::save() {
    if (keys_.isEmpty()) return;
    sync::saveState(directory_ + QStringLiteral("/state.enc"), keys_.state, state_);
}

QJsonArray SyncEngine::transfers() const {
    const auto job = job_;
    if (job && busy_) return job->transfersSnapshot();
    return transfers_;
}

int SyncEngine::pendingCount() const {
    int pending = 0;
    for (const auto& value : items_) {
        const QString state = value.toObject().value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("pending") || state == QStringLiteral("queued")) ++pending;
    }
    return pending;
}

void SyncEngine::refreshItems() {
    if (!vault_ || !vault_->is_unlocked() || busy_) return;
    items_ = buildItems(vault_, state_);
}

void SyncEngine::reject(const QString& message) {
    failed_ = true;
    status_ = message;
    emit changed();
}

void SyncEngine::signIn(const QUrl& server, const QString& email, const QString& password,
                        const QString& deviceLabel) {
    if (busy_) return;
    if (!sync::validServer(server)) {
        reject(QStringLiteral("Enter the server URL, for example https://vault.example:8766. "
                              "Plain HTTP is accepted only for 127.0.0.1."));
        return;
    }
    if (!sync::validEmail(email)) {
        reject(QStringLiteral("Enter the account email address used on the sync server."));
        return;
    }
    if (password.isEmpty()) {
        reject(QStringLiteral("Enter the account password."));
        return;
    }
    if (keys_.isEmpty()) {
        reject(QStringLiteral("Sync is locked. Lock and unlock the vault again."));
        return;
    }
    auto secret = std::make_shared<QString>(password);
    const QString address = email.trimmed();
    const QString label = deviceLabel.isEmpty() ? QStringLiteral("MegaVault desktop") : deviceLabel;
    // The address the client talks to is kept before the job starts: the job
    // only learns it from the answer, and a wrong URL must still be reported.
    state_.server = server.toString();
    state_.email = address;
    launch(QStringLiteral("Signing in..."), Launch::SignedOut,
           [secret, address, label](Job& job, sync::Client& client) {
               job.signIn(client, address, *secret, label);
               secret->fill(QLatin1Char('\0'));
               secret->clear();
           });
    emit changed();
}

void SyncEngine::refreshVaults() {
    if (busy_ || !state_.signedIn()) return;
    launch(QStringLiteral("Reading the vault list..."), Launch::Token,
           [](Job& job, sync::Client& client) { job.listVaults(client); });
}

void SyncEngine::signOut() {
    if (busy_) return;
    state_.clearToken();
    state_.vaultId.clear();
    state_.vaultName.clear();
    state_.records.clear();
    state_.pendingDownload.clear();
    state_.cursor = 0;
    vaults_ = QJsonArray();
    save();
    status_ = QStringLiteral("Signed out. The device token was erased from this computer.");
    emit changed();
}

void SyncEngine::chooseVault(const QString& vaultId, const QString& name) {
    if (busy_) return;
    if (vaultId == state_.vaultId) {
        state_.vaultName = name;
        save();
        emit changed();
        return;
    }
    state_.vaultId = vaultId;
    state_.vaultName = name;
    state_.records.clear();
    state_.pendingDownload.clear();
    state_.cursor = 0;
    save();
    refreshItems();
    status_ = QStringLiteral("\"%1\" chosen. Choose the local folder, then press Sync.").arg(name);
    emit changed();
}

bool SyncEngine::setLocalFolder(const QString& path) {
    if (busy_) return false;
    QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) {
        reject(QStringLiteral("That folder does not exist."));
        return false;
    }
    QString root = QFileInfo(qtPath(vault_->root_path())).canonicalFilePath();
    if (root.isEmpty()) root = qtPath(vault_->root_path());
    if (QDir::cleanPath(canonical) != QDir::cleanPath(root)) {
        reject(QStringLiteral("Sync writes into the open vault only. Open \"%1\" as a vault first, then sync.")
                   .arg(canonical));
        return false;
    }
    state_.folder = root;
    save();
    failed_ = false;
    status_ = QStringLiteral("This vault will sync into %1.").arg(root);
    emit changed();
    return true;
}

void SyncEngine::setPageLimit(int limit) { pageLimit_ = qBound(1, limit, 1000); }

void SyncEngine::syncNow() {
    launch(QStringLiteral("Syncing..."), Launch::Vault,
           [](Job& job, sync::Client& client) { job.syncPass(client); });
}

void SyncEngine::receive(const QString& recordId) {
    launch(QStringLiteral("Downloading one item..."), Launch::Vault,
           [recordId](Job& job, sync::Client& client) { job.receiveOne(client, recordId); });
}

void SyncEngine::tombstone(const QString& recordId) {
    launch(QStringLiteral("Removing..."), Launch::Vault,
           [recordId](Job& job, sync::Client& client) { job.tombstoneOne(client, recordId); });
}

void SyncEngine::restore(const QString& recordId) {
    launch(QStringLiteral("Restoring..."), Launch::Vault,
           [recordId](Job& job, sync::Client& client) { job.restoreOne(client, recordId); });
}

void SyncEngine::purge(const QString& recordId) {
    launch(QStringLiteral("Deleting permanently..."), Launch::Vault,
           [recordId](Job& job, sync::Client& client) { job.purgeOne(client, recordId); });
}

void SyncEngine::stop() {
    const auto job = job_;
    if (job) job->stop();
}

void SyncEngine::launch(const QString& label, Launch requirement,
                        const std::function<void(Job&, sync::Client&)>& task) {
    if (busy_) return;
    if (state_.server.isEmpty()) {
        reject(QStringLiteral("Sign in first."));
        return;
    }
    QUrl base(state_.server);
    if (!sync::validServer(base)) {
        reject(QStringLiteral("The stored server address is not usable; sign in again."));
        return;
    }
    if (requirement != Launch::SignedOut && !state_.signedIn()) {
        reject(QStringLiteral("Sign in first."));
        return;
    }
    if (requirement == Launch::Vault && state_.vaultId.isEmpty()) {
        reject(QStringLiteral("Choose a vault first."));
        return;
    }
    const bool needsToken = requirement != Launch::SignedOut;
    busy_ = true;
    failed_ = false;
    status_ = label;
    if (pulse_ != nullptr && !pulse_->isActive()) pulse_->start();
    emit changed();

    auto job = std::make_shared<Job>(vault_, keys_, directory_, state_, pageLimit_);
    job_ = job;
    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, job] {
        watcher->deleteLater();
        if (job_.get() == job.get()) job_.reset();
        busy_ = false;
        state_ = job->state;
        transfers_ = job->transfersSnapshot();
        stats_ = job->stats;
        if (!job->vaults.isEmpty()) vaults_ = job->vaults;
        if (!job->items.isEmpty()) items_ = job->items;
        else refreshItems();
        if (!job->status.isEmpty()) status_ = job->status;
        failed_ = job->failed;
        if (job->libraryChanged) emit libraryChanged();
        emit changed();
        emit finished();
    });
    watcher->setFuture(QtConcurrent::run([job, task, base, needsToken] {
        // The client, its manager and every reply live and die here.
        sync::Client client(base, needsToken ? job->state.token : QString());
        try {
            task(*job, client);
        } catch (const sync::Cancelled&) {
            job->status = QStringLiteral("Stopped. What landed so far is saved and will continue next time.");
        } catch (const std::exception& error) {
            job->failed = true;
            job->status = QString::fromUtf8(error.what());
        }
    }));
}

QString SyncEngine::relativePath(const core::VideoInfo& video, const std::vector<core::FolderInfo>& folders) {
    QString name = QString::fromStdString(video.display_name);
    const QString path = folderPathOf(video, folders);
    return path.isEmpty() ? name : path + QLatin1Char('/') + name;
}

}  // namespace videovault::app
