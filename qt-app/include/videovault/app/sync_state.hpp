#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>

// The pieces the protocol 4 client shares between its engine, its dialog and
// its tests: validation, package naming, key derivation and the encrypted
// per-device sync state: the small record map and queue that make a pass
// resumable, plus the device token that unlocks the server.
namespace videovault::app::sync {

// Exactly `length` lowercase hex characters. Uppercase is a different string,
// not an alternative spelling, so hashes are taken literally.
bool isHex(const QString& value, int length);

// A package address on the server: the SHA-256 of the package's own bytes.
bool isPackage(const QString& value);

// Record ids the server accepts: [A-Za-z0-9][A-Za-z0-9._-]{0,63}.
bool isRecordId(const QString& value);

// An HTTPS origin, or plain HTTP for a literal loopback host only. No
// credentials, path, query or fragment: fixed paths are joined to it.
bool validServer(const QUrl& url);

// The shape POST /auth/register and /auth/login accept.
bool validEmail(const QString& email);

// Lowercase hex SHA-256 of a file's bytes. Throws when it cannot be read.
QString fileSha256(const QString& path);

// Cryptographically random lowercase hex, `bytes` bytes wide.
QString randomHex(int bytes);

// A fresh record id: 32 lowercase hex characters.
QString newRecordId();

// MEGA's answer to a version conflict: the loser publishes its own copy under
// a new record id, so both survive. The suffix is added once, never stacked.
QString conflictCopyName(const QString& name);

// The idempotency key for one record write. Derived from everything the write
// carries, so a retry after a lost response replays the first answer instead of
// writing twice. `operation_id` must match [A-Za-z0-9._:-]{1,128}.
QString operationId(const QString& recordId, int baseVersion, const QString& package,
                    const QString& name, const QString& path, const QStringList& tags);

// The two secrets a device derives from the vault password it was unlocked
// with. Neither is ever sent to the server: `package` seals the .mvpk the
// device uploads (every device of the same vault derives the same value) and
// `state` seals this device's own state file.
struct Keys {
    QString package;   // 64 lowercase hex: the .mvpk password
    QByteArray state;  // 32 bytes: the XChaCha20-Poly1305 key for state.enc

    bool isEmpty() const { return package.isEmpty() || state.size() != 32; }
    void wipe();
};

// <AppLocalData>/sync-v4/<sha256 of the canonical vault folder>. One device may
// hold several vaults; each keeps its own state.
QString stateDirectoryFor(const QString& vaultRoot);

// Derives the keys from the vault password and this device's random salt. The
// salt is created on first use (create=true) and reused afterwards; losing it
// only means the state file must be rebuilt, never that data is lost.
Keys deriveKeys(const QString& stateDirectory, const QString& vaultPassword, bool create = true);

// One metadata row as the server keeps it, plus the local item it maps to.
struct RecordRef {
    QString id;
    int version{0};
    QString name;
    QString path;      // folder path inside the vault, "" for the root
    QStringList tags;
    QString package;   // 64 hex; empty when the record carries no content
    qint64 size{0};
    bool deleted{false};
    qint64 updated{0};
    qint64 localId{0};               // 0 = no local copy yet (queued for download)
    // The local item's own package hash when this record was last bound or
    // written. Importing re-seals a package, so it is never the server's
    // address; comparing it is what tells "unchanged" from "edited here".
    QString localPackage;

    QJsonObject toJson() const;
    static RecordRef fromJson(const QJsonObject& object);
};

// What this device knows about one remote vault. Encrypted at rest with
// Keys::state: it holds the device token, so it must never be readable without
// the vault password.
struct SyncState {
    QString server;    // origin, e.g. https://vault.example:8766
    QString email;     // the account this device signed in with
    QString token;     // device bearer token, returned by the server exactly once
    QString tokenId;   // its server-side id, for "this device" display
    QString vaultId;
    QString vaultName;
    QString folder;    // the local vault folder this remote vault mirrors
    qint64 cursor{0};  // record cursor; the server's per-vault sequence

    QMap<QString, RecordRef> records;  // record id -> what we know
    QStringList pendingDownload;       // record ids queued for a lazy download

    bool signedIn() const { return !token.isEmpty(); }
    bool ready() const { return signedIn() && !vaultId.isEmpty() && !folder.isEmpty(); }

    // The live (not deleted) record claiming a local item, or nullptr.
    const RecordRef* liveRecordFor(qint64 localId) const;

    QJsonObject toJson() const;
    static SyncState fromJson(const QJsonObject& object);

    void clearToken();
};

// A missing state file is an empty state. A wrong key, a damaged file or an
// unknown version throw instead of silently starting over, which would forget
// the token and re-upload everything.
SyncState loadState(const QString& path, const QByteArray& key);
void saveState(const QString& path, const QByteArray& key, const SyncState& state);

}
