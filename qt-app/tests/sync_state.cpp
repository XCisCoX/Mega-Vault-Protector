// Offline part of the sync protocol 4 client: key derivation, the encrypted
// state file (which holds the device token), record bookkeeping, naming rules
// and the validation shared with the dialog. No server, no vault, no GUI.
#include "videovault/app/sync_state.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

using namespace videovault::app::sync;

namespace {

void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

void check(bool ok, const QString& what) { check(ok, what.toUtf8().constData()); }

QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read a fixture file");
    return file.readAll();
}

}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary directory");

        // -- key derivation -------------------------------------------------
        const QString directory = temp.path() + "/device-a";
        const QString password = QStringLiteral("vault password for the fixture");
        const Keys first = deriveKeys(directory, password);
        const Keys again = deriveKeys(directory, password);
        check(!first.isEmpty(), "the keys are derived");
        check(isPackage(first.package), "the package password is 64 lowercase hex");
        check(first.state.size() == 32, "the state key is 32 bytes");
        check(first.package == again.package, "the same password derives the same package key");
        check(first.state == again.state, "the same salt derives the same state key");
        check(QFile::exists(directory + "/kdf.salt"), "the device salt is kept next to the state");
        check(readAll(directory + "/kdf.salt").size() == 16, "the device salt is 16 bytes");

        // Two devices of one vault must agree on the .mvpk password (so each
        // can open what the other uploaded) and disagree on the state key (so a
        // stolen state file is not portable).
        const Keys deviceB = deriveKeys(temp.path() + "/device-b", password);
        check(deviceB.package == first.package, "devices of one vault share the package password");
        check(deviceB.state != first.state, "each device seals its own state with its own key");

        const Keys other = deriveKeys(temp.path() + "/device-c", QStringLiteral("another password"));
        check(other.package != first.package, "a different vault password gives a different package key");
        check(other.state != first.state, "a different vault password gives a different state key");
        check(!deriveKeys(temp.path() + "/device-d", password).state.isEmpty(), "a new device derives a key");

        // -- the state file -------------------------------------------------
        const QString path = directory + "/state.enc";
        const QString token(64, 'a');
        SyncState state;
        state.server = QStringLiteral("http://127.0.0.1:8766");
        state.email = QStringLiteral("owner@example.com");
        state.token = token;
        state.tokenId = QString(64, 'b');
        state.vaultId = QString(32, 'c');
        state.vaultName = QStringLiteral("Family");
        state.folder = QStringLiteral("C:/vaults/family");
        state.cursor = 42;
        RecordRef record;
        record.id = QStringLiteral("rec-1");
        record.version = 3;
        record.name = QStringLiteral("note.txt");
        record.path = QStringLiteral("Work");
        record.tags << QStringLiteral("family") << QStringLiteral("draft");
        record.package = QString(64, 'd');
        record.size = 1234;
        record.localId = 7;
        state.records.insert(record.id, record);
        state.pendingDownload.append(QStringLiteral("rec-2"));
        saveState(path, first.state, state);

        const QByteArray sealed = readAll(path);
        check(!sealed.contains(token.toLatin1()), "the device token is not on disk in the clear");
        check(!sealed.contains("note.txt"), "the state holds no plaintext item name");
        check(!sealed.contains("owner@example.com"), "the state holds no plaintext account email");
        check(sealed.startsWith("MVS4"), "the state is marked with its own magic");

        const SyncState loaded = loadState(path, first.state);
        check(loaded.token == token, "the device token round trips");
        check(loaded.email == "owner@example.com" && loaded.vaultName == "Family", "account and vault round trip");
        check(loaded.cursor == 42, "the record cursor round trips");
        check(loaded.records.size() == 1, "one record round trips");
        const RecordRef back = loaded.records.value("rec-1");
        check(back.version == 3 && back.name == "note.txt" && back.path == "Work", "record fields round trip");
        check(back.tags.size() == 2 && back.package == record.package && back.localId == 7, "tags, package and mapping round trip");
        check(loaded.pendingDownload == QStringList{QStringLiteral("rec-2")}, "the download queue round trips");

        bool rejected = false;
        try { loadState(path, deviceB.state); } catch (const std::exception&) { rejected = true; }
        check(rejected, "another device's key cannot open this state");

        rejected = false;
        try { loadState(path, other.state); } catch (const std::exception&) { rejected = true; }
        check(rejected, "a different vault password cannot open this state");

        {
            QFile file(path);
            check(file.open(QIODevice::ReadWrite), "reopen the state to tamper with it");
            QByteArray damaged = file.readAll();
            damaged[damaged.size() - 1] = char(damaged.at(damaged.size() - 1) ^ 0x01);
            file.seek(0);
            check(file.write(damaged) == damaged.size(), "tamper with the state");
        }
        rejected = false;
        try { loadState(path, first.state); } catch (const std::exception&) { rejected = true; }
        check(rejected, "a tampered state is refused");

        {
            QFile file(path);
            check(file.open(QIODevice::ReadWrite), "truncate the state");
            file.resize(10);
        }
        rejected = false;
        try { loadState(path, first.state); } catch (const std::exception&) { rejected = true; }
        check(rejected, "a truncated state is refused");

        check(loadState(temp.path() + "/absent.enc", first.state).records.isEmpty(),
              "a missing state starts empty instead of failing");

        // -- bookkeeping ----------------------------------------------------
        SyncState two;
        RecordRef live;
        live.id = QStringLiteral("live");
        live.localId = 9;
        two.records.insert(live.id, live);
        RecordRef gone;
        gone.id = QStringLiteral("gone");
        gone.localId = 9;
        gone.deleted = true;
        two.records.insert(gone.id, gone);
        check(two.liveRecordFor(9) != nullptr && two.liveRecordFor(9)->id == QLatin1String("live"),
              "a tombstone never counts as the claiming record");
        check(two.liveRecordFor(10) == nullptr, "an unclaimed item has no record");
        check(two.liveRecordFor(0) == nullptr, "0 is not a local id");

        // -- naming ---------------------------------------------------------
        check(conflictCopyName("note.txt") == "note.txt (conflict copy)",
              "a conflict copy carries the suffix the plan asks for");
        check(conflictCopyName("note.txt (conflict copy)") == "note.txt (conflict copy)",
              "the suffix is added once, never stacked");
        check(conflictCopyName(QString(300, 'x')).size() == 255, "a conflict name still fits the server's limit");

        const QString operation = operationId("rec-1", 0, QString(64, 'd'), "note.txt", "Work", {"family"});
        check(operation == operationId("rec-1", 0, QString(64, 'd'), "note.txt", "Work", {"family"}),
              "one write derives one operation id, so a retry replays instead of writing twice");
        check(operation != operationId("rec-1", 1, QString(64, 'd'), "note.txt", "Work", {"family"}),
              "a different base version is a different operation");
        check(operation != operationId("rec-2", 0, QString(64, 'd'), "note.txt", "Work", {"family"}),
              "a different record is a different operation");
        check(operation.size() == 64 && isHex(operation, 64), "the operation id is 64 lowercase hex");

        // -- validation shared with the dialog ------------------------------
        check(validServer(QUrl("https://vault.example:8766")), "HTTPS origins are accepted");
        check(validServer(QUrl("http://127.0.0.1:8766")), "loopback HTTP is accepted for development");
        check(validServer(QUrl("http://localhost:8766")), "localhost is loopback");
        check(!validServer(QUrl("http://vault.example:8766")), "remote plain HTTP is refused");
        check(!validServer(QUrl("https://user:secret@vault.example")), "credentials in the URL are refused");
        check(!validServer(QUrl("https://vault.example/a/path")), "URL paths are refused");
        check(!validServer(QUrl("https://vault.example/?q=1")), "URL queries are refused");
        check(!validServer(QUrl("file:///tmp/vault")), "non-network URLs are refused");
        check(!validServer(QUrl()), "an empty URL is refused");

        check(validEmail("owner@example.com"), "a normal address is accepted");
        check(!validEmail("owner"), "an address without a domain is refused");
        check(!validEmail("owner@localhost"), "an address without a dot is refused");
        check(!validEmail("@example.com"), "an address without a local part is refused");
        check(!validEmail("own er@example.com"), "spaces are refused");

        check(isRecordId("rec-1") && isRecordId("0123456789abcdef"), "server-shaped record ids are accepted");
        check(!isRecordId("-rec") && !isRecordId("") && !isRecordId(QString(65, 'a')), "odd record ids are refused");
        check(isPackage(QString(64, 'a')) && !isPackage(QString(64, 'A')), "a package is 64 lowercase hex");
        check(!isPackage(QString(63, 'a') + ".mvpk"), "a package has no file suffix");
        check(isHex(randomHex(16), 32) && randomHex(16) != randomHex(16), "random hex is fresh each time");
        check(isRecordId(newRecordId()), "a fresh record id is one the server accepts");

        const QJsonObject json = state.toJson();
        check(json.value("version").toInt() == 4, "the state declares protocol 4");
        check(SyncState::fromJson(json).records.size() == 1, "the state survives a JSON round trip");

        std::cout << "key derivation, encrypted state (token at rest), record bookkeeping, "
                     "conflict naming, operation ids and validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
