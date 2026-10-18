// Live part of the sync protocol 4 client: two local vaults, one real
// sync_server, one account and one device token. Start it through
// tests/run_sync_live.py, which provisions the account, the token and two
// vaults with the server's own CLI, then passes:
//
//   VideoVaultSyncEngineTests <server-url> <token> <email> <account-password> \
//                             <vault-a-id> <vault-b-id>
//
// Without arguments it prints that it was skipped and exits 77 (CTest's
// SKIP_RETURN_CODE), so a plain test run does not pretend to have covered it.
#include "videovault/app/sync_client.hpp"
#include "videovault/app/sync_engine.hpp"
#include "videovault/app/sync_state.hpp"
#include "videovault/core/vault.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <iostream>
#include <memory>
#include <stdexcept>

using videovault::app::SyncEngine;
using videovault::app::sync::Client;
using videovault::app::sync::RecordRef;

namespace {

void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

void section(const std::string& title) { std::cout << "\n== " << title << "\n"; }

std::filesystem::path fsPath(const QString& path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::u8path(path.toUtf8().constData());
#endif
}

QString nativePath(const std::filesystem::path& path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::shared_ptr<videovault::core::Vault> makeVault(const QString& path, const QString& password) {
    auto created = videovault::core::Vault::create(fsPath(path), password.toStdString(), {8192, 1, 1});
    if (!created) throw std::runtime_error("cannot create a test vault");
    return std::make_shared<videovault::core::Vault>(std::move(created.value()));
}

void waitFor(SyncEngine& engine, int timeoutMs = 180000) {
    if (!engine.busy()) throw std::runtime_error("the operation never started");
    QEventLoop loop;
    bool finished = false;
    QObject::connect(&engine, &SyncEngine::finished, &loop, [&] {
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    if (!finished) throw std::runtime_error("the operation did not finish in time");
}

void settle(SyncEngine& engine, const char* what) {
    waitFor(engine);
    if (engine.failed()) {
        throw std::runtime_error(std::string(what) + " failed: " + engine.status().toUtf8().constData());
    }
    std::cout << "    " << what << " -> " << engine.status().toUtf8().constData() << "\n";
}

int stat(const SyncEngine& engine, const char* name) {
    return engine.stats().value(QString::fromLatin1(name)).toInt();
}

QJsonObject itemNamed(const SyncEngine& engine, const QString& name) {
    for (const auto& value : engine.items()) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("name")).toString() == name) return item;
    }
    throw std::runtime_error(("the item \"" + name + "\" is not listed: ").toUtf8().constData());
}

std::int64_t idOf(videovault::core::Vault& vault, const std::string& name) {
    auto videos = vault.list_videos();
    if (!videos) throw std::runtime_error("cannot list the local library");
    for (const auto& video : videos.value()) {
        if (video.display_name == name) return video.id;
    }
    throw std::runtime_error("the expected local item is missing");
}

bool hasLocal(videovault::core::Vault& vault, const std::string& name) {
    auto videos = vault.list_videos();
    if (!videos) return false;
    for (const auto& video : videos.value()) {
        if (video.display_name == name) return true;
    }
    return false;
}

QByteArray contentOf(videovault::core::Vault& vault, const std::string& name) {
    auto bytes = vault.read_video_bytes(idOf(vault, name));
    if (!bytes) throw std::runtime_error("cannot read a local item");
    return QByteArray(reinterpret_cast<const char*>(bytes.value().data()), int(bytes.value().size()));
}

QStringList tagsOf(videovault::core::Vault& vault, const std::string& name) {
    QStringList tags;
    auto list = vault.tags_for_video(idOf(vault, name));
    if (!list) return tags;
    for (const auto& tag : list.value()) tags.append(QString::fromStdString(tag.name));
    return tags;
}

// Every record the server holds for the vault, walked with a big page so the
// assertions see the whole set.
QList<RecordRef> allRecords(Client& client, const QString& vault) {
    QList<RecordRef> records;
    qint64 since = 0;
    for (;;) {
        const auto page = client.records(vault, since, 1000);
        records.append(page.records);
        if (page.records.size() < 1000 || page.cursor <= since) break;
        since = page.cursor;
    }
    return records;
}

int countPackage(const QList<RecordRef>& records, const QString& package) {
    int count = 0;
    for (const auto& record : records) {
        if (record.package == package) ++count;
    }
    return count;
}

}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 7) {
        std::cout << "skipped: run through tests/run_sync_live.py, which creates the account, the token "
                     "and two vaults on a real sync_server\n";
        return 77;
    }
    const QUrl url = QUrl::fromUserInput(QString::fromUtf8(argv[1]));
    const QString harnessToken = QString::fromUtf8(argv[2]);
    const QString email = QString::fromUtf8(argv[3]);
    const QString accountPassword = QString::fromUtf8(argv[4]);
    const QString vaultA = QString::fromUtf8(argv[5]);
    const QString vaultB = QString::fromUtf8(argv[6]);

    QTemporaryDir temp;
    try {
        check(temp.isValid(), "temporary directory");

        // Two devices of one vault: the same vault password, two local
        // databases, two private state directories. That is what makes their
        // packages readable to each other without the server holding a key.
        const QString password = QStringLiteral("one-vault-two-devices");
        auto vaultLocalA = makeVault(temp.path() + "/vault-a", password);
        auto vaultLocalB = makeVault(temp.path() + "/vault-b", password);
        const auto keysA = videovault::app::sync::deriveKeys(temp.path() + "/state-a", password);
        const auto keysB = videovault::app::sync::deriveKeys(temp.path() + "/state-b", password);
        check(keysA.package == keysB.package, "both devices derive the same package password");
        check(keysA.state != keysB.state, "each device has its own state key");

        section("1. sign in with the account, token issued and stored encrypted");
        SyncEngine engineA(vaultLocalA, keysA, temp.path() + "/state-a");
        engineA.signIn(url, email, accountPassword);
        settle(engineA, "sign in (A)");
        check(engineA.signedIn(), "the engine is signed in");
        check(engineA.vaults().size() == 2, "the account lists exactly the two vaults the CLI created");
        {
            QFile state(temp.path() + "/state-a/state.enc");
            check(state.open(QIODevice::ReadOnly), "the state file exists");
            const QByteArray sealed = state.readAll();
            check(!sealed.contains(harnessToken.toLatin1()), "the device token is not stored in the clear");
            check(!sealed.contains(accountPassword.toLatin1()), "the account password is never stored");
            std::cout << "    state.enc is " << sealed.size() << " bytes and holds no plaintext token\n";
        }

        section("2. choose the vault and the local folder");
        engineA.chooseVault(vaultA, QStringLiteral("Vault A"));
        check(engineA.setLocalFolder(nativePath(vaultLocalA->root_path())),
              "the open vault is accepted as the folder");
        check(!engineA.setLocalFolder(temp.path() + "/somewhere-else"),
              "any other folder is refused, because it would be a different local database");
        std::cout << "    refusing a foreign folder: " << engineA.status().toUtf8().constData() << "\n";
        check(engineA.setLocalFolder(nativePath(vaultLocalA->root_path())),
              "the open vault is accepted again");

        section("3. publish a local item: blob first, then the record");
        auto work = vaultLocalA->create_folder(0, "Work");
        check(work.has_value(), "create a folder");
        const std::vector<unsigned char> hello{'h', 'e', 'l', 'l', 'o'};
        auto noteId = vaultLocalA->import_bytes(hello, "note.txt");
        check(noteId.has_value(), "import an item");
        check(vaultLocalA->move_video(noteId.value(), work.value()).has_value(), "file it under Work");
        check(vaultLocalA->add_tag(noteId.value(), "family").has_value(), "tag it");
        engineA.syncNow();
        check(engineA.busy(), "the pass runs on a worker, so the library stays usable");
        settle(engineA, "sync (A)");
        check(stat(engineA, "uploaded") == 1, "one record was published");
        const QJsonObject noteItem = itemNamed(engineA, "note.txt");
        check(noteItem.value("state").toString() == "synced", "the item is listed as on this device");
        const QString noteRecord = noteItem.value("record").toString();
        check(videovault::app::sync::isRecordId(noteRecord), "the item is bound to a record id");

        section("4. record cursor paging");
        {
            Client client(url, harnessToken);
            client.protocol();
            for (int index = 0; index < 8; ++index) {
                RecordRef page;
                page.id = QStringLiteral("page-%1").arg(index + 1);
                page.name = page.id;
                check(client.write(vaultA, page, 0,
                                   videovault::app::sync::operationId(page.id, 0, {}, page.name, {}, {}))
                          .version == 1,
                      "an extra record is created for the paging walk");
            }
            QSet<QString> seen;
            qint64 cursor = 0;
            int pages = 0;
            for (;;) {
                const auto page = client.records(vaultA, cursor, 3);
                ++pages;
                for (const auto& record : page.records) {
                    check(!seen.contains(record.id), "no record is returned twice");
                    seen.insert(record.id);
                }
                if (page.records.size() < 3) { cursor = page.cursor; break; }
                check(page.cursor > cursor, "a short page advances the cursor");
                cursor = page.cursor;
            }
            check(pages >= 3, "nine records take several pages of three");
            check(seen.size() == 9, "every record is reached exactly once");
            check(client.records(vaultA, cursor, 3).records.isEmpty(), "the cursor at the end yields nothing new");
            std::cout << "    " << seen.size() << " records across " << pages << " pages of 3\n";
        }
        engineA.setPageLimit(2);
        engineA.syncNow();
        settle(engineA, "sync (A, page size 2)");
        check(stat(engineA, "pages") >= 4, "the engine walked the cursor in several requests");
        check(stat(engineA, "uploaded") == 0, "nothing was re-published while walking");
        engineA.setPageLimit(500);

        section("5. a second device receives the item (round trip A -> B)");
        SyncEngine engineB(vaultLocalB, keysB, temp.path() + "/state-b");
        engineB.signIn(url, email, accountPassword);
        settle(engineB, "sign in (B)");
        engineB.chooseVault(vaultA, QStringLiteral("Vault A"));
        check(engineB.setLocalFolder(nativePath(vaultLocalB->root_path())),
              "device B picks its own vault folder");
        engineB.syncNow();
        settle(engineB, "sync (B)");
        check(hasLocal(*vaultLocalB, "note.txt"), "device B received the item");
        check(contentOf(*vaultLocalB, "note.txt") == QByteArray("hello"), "the received content is intact");
        check(tagsOf(*vaultLocalB, "note.txt") == QStringList{"family"}, "the tag travelled with the record");
        check(itemNamed(engineB, "note.txt").value("state").toString() == "synced", "B lists it as on this device");
        // The paging records carried no package, so they stay queued, not failed.
        check(engineB.pendingCount() >= 1, "records without bytes yet are queued, not lost");

        section("6. a delete is a tombstone, and it restores");
        engineB.tombstone(noteRecord);
        settle(engineB, "tombstone (B)");
        engineA.syncNow();
        settle(engineA, "sync (A)");
        check(itemNamed(engineA, "note.txt").value("state").toString() == "removed",
              "device A shows the item as removed on another device");
        check(hasLocal(*vaultLocalA, "note.txt"), "the local copy survives a tombstone");
        check(contentOf(*vaultLocalA, "note.txt") == QByteArray("hello"), "and it is still readable");
        engineB.restore(noteRecord);
        settle(engineB, "restore (B)");
        engineA.syncNow();
        settle(engineA, "sync (A)");
        check(itemNamed(engineA, "note.txt").value("state").toString() == "synced",
              "the restore brings the item back on A as well");
        check(hasLocal(*vaultLocalA, "note.txt") && hasLocal(*vaultLocalB, "note.txt"),
              "both devices still hold the item");

        section("7. a version conflict keeps both items");
        check(vaultLocalA->rename_video(noteId.value(), "note-a.txt").has_value(), "rename on A");
        engineA.syncNow();
        settle(engineA, "sync (A)");
        check(vaultLocalB->rename_video(idOf(*vaultLocalB, "note.txt"), "note-b.txt").has_value(), "rename on B");
        engineB.syncNow();
        settle(engineB, "sync (B)");
        check(stat(engineB, "conflicts") == 1, "the stale write became a conflict copy");
        check(hasLocal(*vaultLocalB, "note-b.txt"), "device B keeps its own name");
        check(contentOf(*vaultLocalB, "note-b.txt") == QByteArray("hello"), "and its content");
        {
            Client client(url, harnessToken);
            const auto records = allRecords(client, vaultA);
            QString package;
            for (const auto& record : records) {
                if (record.name == "note-a.txt") package = record.package;
            }
            check(!package.isEmpty(), "the winning record is on the server under A's name");
            check(countPackage(records, package) == 2, "both versions exist as separate records");
            bool hasCopy = false;
            for (const auto& record : records) {
                if (record.name == "note-b.txt (conflict copy)") hasCopy = true;
            }
            check(hasCopy, "the loser was re-issued with the MEGA suffix");
        }
        // Neither side may keep minting copies on later passes.
        engineB.syncNow();
        settle(engineB, "sync (B, again)");
        check(stat(engineB, "conflicts") == 0 && stat(engineB, "uploaded") == 0,
              "a conflict copy is not re-published on the next pass");
        engineA.syncNow();
        settle(engineA, "sync (A)");
        check(hasLocal(*vaultLocalA, "note-a.txt"), "device A's name is unchanged");

        section("8. operation replay does not duplicate");
        {
            Client client(url, harnessToken);
            RecordRef record;
            record.id = QStringLiteral("op-replay");
            record.name = QStringLiteral("replay.txt");
            const QString operation =
                videovault::app::sync::operationId(record.id, 0, {}, record.name, {}, {});
            const RecordRef first = client.write(vaultA, record, 0, operation);
            const RecordRef second = client.write(vaultA, record, 0, operation);
            check(first.version == 1 && second.version == 1, "the replay answers with the first version");
            check(first.updated == second.updated, "the replay answers byte for byte, not a new write");
            int count = 0;
            for (const auto& entry : allRecords(client, vaultA)) {
                if (entry.id == record.id) ++count;
            }
            check(count == 1, "a replay creates no second record");
            bool conflicted = false;
            try {
                client.write(vaultA, record, 0, operation + "-other");
            } catch (const videovault::app::sync::Conflict& conflict) {
                conflicted = true;
                check(conflict.present && conflict.current.version == 1,
                      "a stale write returns the current record with the 409");
            }
            check(conflicted, "a different operation id on a used record is a version conflict");
        }
        {
            // The engine derives the same operation id for the same write, so its
            // own retry is free; and an unchanged pass writes nothing at all.
            Client client(url, harnessToken);
            const auto before = allRecords(client, vaultA);
            int beforeVersion = 0;
            for (const auto& record : before) {
                if (record.id == noteRecord) beforeVersion = record.version;
            }
            engineA.syncNow();
            settle(engineA, "sync (A, unchanged)");
            check(stat(engineA, "uploaded") == 0 && stat(engineA, "updated") == 0,
                  "an up-to-date pass publishes nothing");
            const auto after = allRecords(client, vaultA);
            check(after.size() == before.size(), "the record count is unchanged");
            for (const auto& record : after) {
                if (record.id == noteRecord) check(record.version == beforeVersion, "the version is unchanged");
            }
        }

        section("9. round trip B -> A with a tag");
        const std::vector<unsigned char> fromB{'f', 'r', 'o', 'm', ' ', 'b'};
        check(vaultLocalB->import_bytes(fromB, "from-b.txt").has_value(), "B imports its own item");
        check(vaultLocalB->add_tag(idOf(*vaultLocalB, "from-b.txt"), "draft").has_value(), "B tags it");
        engineB.syncNow();
        settle(engineB, "sync (B)");
        engineA.syncNow();
        settle(engineA, "sync (A)");
        check(hasLocal(*vaultLocalA, "from-b.txt"), "device A received the item from B");
        check(contentOf(*vaultLocalA, "from-b.txt") == QByteArray("from b"), "with its content intact");
        check(tagsOf(*vaultLocalA, "from-b.txt") == QStringList{"draft"}, "and its tag");

        section("10. a queued record downloads lazily, and resumes from a partial file");
        {
            const int bytes = 2500 * 1024;  // spans several 1 MiB ranges
            const std::size_t byteCount = static_cast<std::size_t>(bytes);
            std::vector<unsigned char> payload;
            payload.reserve(byteCount);
            for (int index = 0; index < bytes; ++index) {
                payload.push_back(static_cast<unsigned char>('a' + (index % 26)));
            }
            auto bigId = vaultLocalA->import_bytes(payload, "big.bin");
            check(bigId.has_value(), "A imports a 2.5 MiB item");
            const QString exported = temp.path() + "/big.mvpk";
            check(vaultLocalA->export_package(fsPath(exported), keysA.package.toStdString(),
                                              {{bigId.value(), "big.bin"}}, {}, false)
                      .has_value(),
                  "A exports the package under the shared package password");
            const QString package = videovault::app::sync::fileSha256(exported);
            const qint64 size = QFileInfo(exported).size();

            Client client(url, harnessToken);
            // The record exists before the bytes: device B must queue it and say
            // so, not fail and not lose it.
            RecordRef record;
            record.id = QStringLiteral("big-record");
            record.name = QStringLiteral("big.bin");
            record.package = package;
            record.size = size;
            check(client.write(vaultA, record, 0,
                               videovault::app::sync::operationId(record.id, 0, package, record.name, {}, {}))
                      .version == 1,
                  "the record is published without its blob");
            engineB.syncNow();
            settle(engineB, "sync (B, blob missing)");
            check(stat(engineB, "waiting") >= 1, "the item waits for its bytes");
            check(itemNamed(engineB, "big.bin").value("state").toString() == "pending",
                  "a queued item carries the pending marker");
            check(!hasLocal(*vaultLocalB, "big.bin"), "nothing was imported without the bytes");

            // Now the bytes arrive, and a previous attempt left a partial file.
            client.putBlob(vaultA, package, exported);
            const qint64 half = size / 2;
            const QByteArray head = client.getRange(vaultA, package, 0, half - 1, size);
            check(QDir().mkpath(engineB.stagingDirectory()), "the staging directory exists");
            {
                QFile partial(engineB.stagingDirectory() + "/" + package + ".part");
                check(partial.open(QIODevice::WriteOnly), "seed a partial download");
                check(partial.write(head) == head.size() && partial.flush(), "seed a partial download");
            }
            engineB.syncNow();
            settle(engineB, "sync (B, resumed)");
            check(stat(engineB, "resumed") == 1, "the download resumed from the bytes already on disk");
            check(stat(engineB, "imported") >= 1, "the resumed download imported");
            check(hasLocal(*vaultLocalB, "big.bin"), "the item is on device B");
            const QByteArray expected(reinterpret_cast<const char*>(payload.data()), bytes);
            check(contentOf(*vaultLocalB, "big.bin") == expected, "the resumed content is byte for byte intact");
            check(!QFile::exists(engineB.stagingDirectory() + "/" + package + ".part"),
                  "the staging file was removed after the hash check");
        }

        section("11. delete permanently, without taking the shared item");
        {
            Client client(url, harnessToken);
            QString copyRecord;
            for (const auto& record : allRecords(client, vaultA)) {
                if (record.name == "note-b.txt (conflict copy)") copyRecord = record.id;
            }
            check(!copyRecord.isEmpty(), "the conflict copy is on the server");
            engineA.tombstone(copyRecord);
            settle(engineA, "tombstone the conflict copy (A)");
            check(itemNamed(engineA, "note-a.txt").value("state").toString() == "synced",
                  "the item survives while another record still claims it");
            engineA.purge(copyRecord);
            settle(engineA, "purge the conflict copy (A)");
            bool gone = true;
            for (const auto& record : allRecords(client, vaultA)) {
                if (record.id == copyRecord) gone = false;
            }
            check(gone, "the purged record is gone from the server");
            check(hasLocal(*vaultLocalA, "note-a.txt"), "the shared local item is still there");
            check(contentOf(*vaultLocalA, "note-a.txt") == QByteArray("hello"), "and still readable");
        }

        section("12. a vault password change keeps shared packages readable");
        {
            const QString newPassword = QStringLiteral("a-different-vault-password");
            const QString stateA = temp.path() + "/state-a";
            const std::vector<unsigned char> after{'a', 'f', 't', 'e', 'r'};
            check(vaultLocalA->import_bytes(after, "after-change.txt").has_value(),
                  "A imports one more item");
            engineA.syncNow();
            settle(engineA, "sync (A, before the password change)");

            check(vaultLocalA->change_password(password.toStdString(), newPassword.toStdString(),
                                               {8192, 1, 1}).has_value(),
                  "the vault password is changed on A");
            const auto keysANew = videovault::app::sync::deriveKeys(stateA, newPassword);
            check(keysANew.state != keysA.state, "the new password derives a new state key");
            videovault::app::sync::resealState(stateA, keysA, keysANew);

            const auto resealed =
                videovault::app::sync::loadState(stateA + "/state.enc", keysANew.state);
            check(resealed.packageKey == keysA.package,
                  "the package password survived the change instead of being re-derived");
            check(resealed.signedIn() && resealed.vaultId == vaultA,
                  "the token and the vault choice survived the re-seal");

            // Exactly what the app builds at the next unlock.
            SyncEngine engineANew(vaultLocalA, keysANew, stateA);
            check(engineANew.signedIn(), "the re-sealed state opens with the new password");
            engineANew.syncNow();
            settle(engineANew, "sync (A, after the password change)");

            // B never learns the new vault password: it must still import what A
            // published after the change, because the package key did not move.
            engineB.syncNow();
            settle(engineB, "sync (B, still on the old vault password)");
            check(hasLocal(*vaultLocalB, "after-change.txt"),
                  "device B received the item published after the change");
            check(contentOf(*vaultLocalB, "after-change.txt") == QByteArray("after"),
                  "and decrypted it with the package key both devices share");
        }

        section("13. nothing is left behind");
        {
            Client client(url, harnessToken);
            const auto records = allRecords(client, vaultA);
            int withPackage = 0;
            for (const auto& record : records) {
                if (record.package.isEmpty()) continue;
                ++withPackage;
                const auto blob = client.head(vaultA, record.package);
                check(blob.exists && blob.complete, "every referenced package is complete on the server");
                check(blob.size == record.size, "the record size matches the stored package");
            }
            check(withPackage >= 3, "the vault holds the packages the two devices published");
            std::cout << "    " << records.size() << " records, " << withPackage << " complete packages\n";
        }

        std::cout << "\nALL PROTOCOL 4 CLIENT CHECKS PASSED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
