#include "videovault/app/sync_state.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <stdexcept>

#include <sodium.h>

namespace videovault::app::sync {
namespace {

constexpr char kMagic[] = "MVS4";
constexpr char kContext[] = "MegaVault-sync-state-v4";
constexpr char kPackageContext[] = "MegaVault-package-key-v4";
constexpr int kKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;   // 32
constexpr int kNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
constexpr int kTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;      // 16
constexpr int kSaltBytes = crypto_pwhash_SALTBYTES;                       // 16
constexpr qint64 kMaxStateBytes = 32 * 1024 * 1024;

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().constData());
}

void ensureCrypto() {
    if (sodium_init() < 0) fail(QStringLiteral("Crypto initialization failed."));
}

bool ensureDirectory(const QString& path) {
    if (!QDir().mkpath(path)) return false;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

QByteArray readSalt(const QString& directory, bool create) {
    const QString path = directory + QStringLiteral("/kdf.salt");
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read this device's sync salt."));
        const QByteArray salt = file.read(kSaltBytes + 1);
        if (salt.size() != kSaltBytes) fail(QStringLiteral("This device's sync salt is damaged; remove it to start over."));
        return salt;
    }
    if (!create) fail(QStringLiteral("This device has no sync salt yet; sign in again."));

    QByteArray salt(kSaltBytes, 0);
    randombytes_buf(salt.data(), salt.size());
    QSaveFile out(path);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly)) fail(QStringLiteral("Cannot write this device's sync salt."));
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (out.write(salt) != salt.size() || !out.commit()) {
        fail(QStringLiteral("Cannot save this device's sync salt durably."));
    }
    return salt;
}

}  // namespace

bool isHex(const QString& value, int length) {
    if (value.size() != length) return false;
    for (const auto character : value) {
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9'))
              || (character >= QLatin1Char('a') && character <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

bool isPackage(const QString& value) { return isHex(value, 64); }

bool isRecordId(const QString& value) {
    if (value.isEmpty() || value.size() > 64) return false;
    const auto first = value.at(0).toLatin1();
    const bool starts = (first >= '0' && first <= '9') || (first >= 'a' && first <= 'z')
        || (first >= 'A' && first <= 'Z');
    if (!starts) return false;
    for (const auto character : value) {
        const auto value8 = character.toLatin1();
        const bool allowed = (value8 >= '0' && value8 <= '9') || (value8 >= 'a' && value8 <= 'z')
            || (value8 >= 'A' && value8 <= 'Z') || value8 == '.' || value8 == '_' || value8 == '-';
        if (!allowed) return false;
    }
    return true;
}

bool validServer(const QUrl& url) {
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != QLatin1String("/"))) {
        return false;
    }
    return url.scheme() == QLatin1String("https")
        || (url.scheme() == QLatin1String("http")
            && (url.host() == QLatin1String("127.0.0.1") || url.host() == QLatin1String("::1")
                || url.host() == QLatin1String("localhost")));
}

bool validEmail(const QString& email) {
    if (email.isEmpty() || email.size() > 190) return false;
    const int at = email.indexOf(QLatin1Char('@'));
    if (at <= 0 || at > 64 || at != email.lastIndexOf(QLatin1Char('@'))) return false;
    const QString domain = email.mid(at + 1);
    if (domain.isEmpty()) return false;
    const QStringList labels = domain.split(QLatin1Char('.'));
    if (labels.size() < 2) return false;
    for (const auto& label : labels) {
        if (label.isEmpty() || label.size() > 63) return false;
        for (const auto character : label) {
            if (character.isSpace() || character == QLatin1Char('@')) return false;
        }
    }
    for (const auto character : email.left(at)) {
        if (character.isSpace() || character == QLatin1Char('@')) return false;
    }
    return true;
}

QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read the file to hash it."));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) fail(QStringLiteral("Cannot read the file to hash it."));
    return QString::fromLatin1(hash.result().toHex());
}

QString randomHex(int bytes) {
    ensureCrypto();
    if (bytes <= 0) return {};
    QByteArray buffer(bytes, 0);
    randombytes_buf(buffer.data(), buffer.size());
    return QString::fromLatin1(buffer.toHex());
}

QString newRecordId() { return randomHex(16); }

QString conflictCopyName(const QString& name) {
    static const QString suffix = QStringLiteral(" (conflict copy)");
    if (name.endsWith(suffix)) return name.left(255);
    // The server refuses a name over 255 characters, so the suffix always fits.
    const int room = 255 - suffix.size();
    return name.left(room) + suffix;
}

QString operationId(const QString& recordId, int baseVersion, const QString& package,
                    const QString& name, const QString& path, const QStringList& tags) {
    QStringList fields;
    fields << QStringLiteral("mvp-op-4") << recordId << QString::number(baseVersion) << package << name
           << path << tags.join(QLatin1Char('\x1f'));
    return QString::fromLatin1(
        QCryptographicHash::hash(fields.join(QLatin1Char('\x1e')).toUtf8(), QCryptographicHash::Sha256).toHex());
}

void Keys::wipe() {
    package.fill(QLatin1Char('\0'));
    package.clear();
    if (!state.isEmpty()) {
        sodium_memzero(state.data(), size_t(state.size()));
        state.clear();
    }
    if (!legacyState.isEmpty()) {
        sodium_memzero(legacyState.data(), size_t(legacyState.size()));
        legacyState.clear();
    }
}

QString adoptedPackageKey(const QString& stored, const Keys& keys) {
    if (!stored.isEmpty()) return stored;
    return keys.package;
}

QString stateDirectoryFor(const QString& vaultRoot) {
    QString canonical = QFileInfo(vaultRoot).canonicalFilePath();
    if (canonical.isEmpty()) canonical = QDir::cleanPath(vaultRoot);
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/sync-v4/")
        + QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex());
}

Keys deriveKeys(const QString& stateDirectory, const QString& vaultPassword, bool create) {
    ensureCrypto();
    if (vaultPassword.isEmpty()) fail(QStringLiteral("The vault password is required to unlock sync."));
    if (!ensureDirectory(stateDirectory)) fail(QStringLiteral("Cannot create the private sync directory."));

    const QByteArray salt = readSalt(stateDirectory, create);
    const QByteArray password = vaultPassword.toUtf8();

    // The package key is device independent on purpose: two devices of the
    // same vault must derive the same .mvpk password, and neither ever sends
    // it anywhere. The device salt below is what keeps the two state files
    // unreadable to each other.
    QByteArray package(kKeyBytes, 0);
    // A fixed, non-secret salt: identical input must give identical output on
    // every device of this vault.
    const QByteArray fixedSalt = QByteArray(kPackageContext).left(kSaltBytes);
    if (crypto_pwhash(reinterpret_cast<unsigned char*>(package.data()), size_t(package.size()),
                      password.constData(), static_cast<unsigned long long>(password.size()),
                      reinterpret_cast<const unsigned char*>(fixedSalt.constData()),
                      crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        fail(QStringLiteral("Cannot derive the sync keys: not enough memory for the password hash."));
    }

    Keys keys;
    keys.package = QString::fromLatin1(package.toHex());

    const QByteArray message = QByteArray(kContext) + salt;
    QByteArray state(kKeyBytes, 0);
    if (crypto_generichash(reinterpret_cast<unsigned char*>(state.data()), size_t(state.size()),
                           reinterpret_cast<const unsigned char*>(message.constData()),
                           static_cast<unsigned long long>(message.size()),
                           reinterpret_cast<const unsigned char*>(package.constData()),
                           size_t(package.size())) != 0) {
        sodium_memzero(package.data(), size_t(package.size()));
        fail(QStringLiteral("Cannot derive this device's sync key."));
    }
    keys.legacyState = state;

    // The state key is derived from the password and this device's salt, so
    // rotating the password only re-seals state.enc. The package key inside it
    // does not change, which is what keeps already shared packages readable.
    QByteArray current(kKeyBytes, 0);
    if (crypto_pwhash(reinterpret_cast<unsigned char*>(current.data()), size_t(current.size()),
                      password.constData(), static_cast<unsigned long long>(password.size()),
                      reinterpret_cast<const unsigned char*>(salt.constData()),
                      crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        sodium_memzero(package.data(), size_t(package.size()));
        sodium_memzero(state.data(), size_t(state.size()));
        fail(QStringLiteral("Cannot derive this device's sync key: not enough memory."));
    }
    keys.state = current;
    sodium_memzero(package.data(), size_t(package.size()));
    return keys;
}

QJsonObject RecordRef::toJson() const {
    QJsonArray tagArray;
    for (const auto& tag : tags) tagArray.append(tag);
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("version"), version},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("path"), path},
                       {QStringLiteral("tags"), tagArray},
                       {QStringLiteral("package"), package},
                       {QStringLiteral("size"), size},
                       {QStringLiteral("deleted"), deleted},
                       {QStringLiteral("updated"), updated},
                       {QStringLiteral("local"), localId},
                       {QStringLiteral("localPackage"), localPackage}};
}

RecordRef RecordRef::fromJson(const QJsonObject& object) {
    RecordRef record;
    record.id = object.value(QStringLiteral("id")).toString();
    record.version = object.value(QStringLiteral("version")).toInt();
    record.name = object.value(QStringLiteral("name")).toString();
    record.path = object.value(QStringLiteral("path")).toString();
    for (const auto& tag : object.value(QStringLiteral("tags")).toArray()) {
        const QString name = tag.toString();
        if (!name.isEmpty() && name.size() <= 64) record.tags.append(name);
    }
    record.package = object.value(QStringLiteral("package")).toString();
    if (!record.package.isEmpty() && !isPackage(record.package)) record.package.clear();
    record.size = qint64(object.value(QStringLiteral("size")).toDouble());
    record.deleted = object.value(QStringLiteral("deleted")).toBool();
    record.updated = qint64(object.value(QStringLiteral("updated")).toDouble());
    record.localId = qint64(object.value(QStringLiteral("local")).toDouble());
    record.localPackage = object.value(QStringLiteral("localPackage")).toString();
    if (!record.localPackage.isEmpty() && !isPackage(record.localPackage)) record.localPackage.clear();
    return record;
}

const RecordRef* SyncState::liveRecordFor(qint64 localId) const {
    if (localId == 0) return nullptr;
    for (auto entry = records.cbegin(); entry != records.cend(); ++entry) {
        if (entry->localId == localId && !entry->deleted) return &entry.value();
    }
    return nullptr;
}

QJsonObject SyncState::toJson() const {
    QJsonArray recordArray;
    for (auto entry = records.cbegin(); entry != records.cend(); ++entry) {
        recordArray.append(entry->toJson());
    }
    QJsonArray download;
    for (const auto& id : pendingDownload) download.append(id);
    QJsonArray remote;
    for (const auto& id : remoteOnly) remote.append(id);
    return QJsonObject{{QStringLiteral("version"), 4},
                       {QStringLiteral("server"), server},
                       {QStringLiteral("email"), email},
                       {QStringLiteral("token"), token},
                       {QStringLiteral("tokenId"), tokenId},
                       {QStringLiteral("packageKey"), packageKey},
                       {QStringLiteral("vault"), vaultId},
                       {QStringLiteral("vaultName"), vaultName},
                       {QStringLiteral("folder"), folder},
                       {QStringLiteral("cursor"), cursor},
                       {QStringLiteral("records"), recordArray},
                       {QStringLiteral("download"), download},
                       {QStringLiteral("remoteOnly"), remote}};
}

SyncState SyncState::fromJson(const QJsonObject& object) {
    SyncState state;
    state.server = object.value(QStringLiteral("server")).toString();
    state.email = object.value(QStringLiteral("email")).toString();
    state.token = object.value(QStringLiteral("token")).toString();
    state.tokenId = object.value(QStringLiteral("tokenId")).toString();
    const QString packageKey = object.value(QStringLiteral("packageKey")).toString();
    if (isHex(packageKey, 64)) state.packageKey = packageKey;
    state.vaultId = object.value(QStringLiteral("vault")).toString();
    state.vaultName = object.value(QStringLiteral("vaultName")).toString();
    state.folder = object.value(QStringLiteral("folder")).toString();
    state.cursor = qint64(object.value(QStringLiteral("cursor")).toDouble());
    if (state.cursor < 0) state.cursor = 0;
    for (const auto& value : object.value(QStringLiteral("records")).toArray()) {
        const RecordRef record = RecordRef::fromJson(value.toObject());
        if (isRecordId(record.id)) state.records.insert(record.id, record);
    }
    for (const auto& value : object.value(QStringLiteral("download")).toArray()) {
        const QString id = value.toString();
        if (isRecordId(id) && !state.pendingDownload.contains(id)) state.pendingDownload.append(id);
    }
    for (const auto& value : object.value(QStringLiteral("remoteOnly")).toArray()) {
        const QString id = value.toString();
        if (isRecordId(id) && !state.remoteOnly.contains(id)) state.remoteOnly.append(id);
    }
    return state;
}

void SyncState::clearToken() {
    token.fill(QLatin1Char('\0'));
    token.clear();
    tokenId.clear();
}

SyncState loadState(const QString& path, const QByteArray& key) {
    ensureCrypto();
    if (key.size() != kKeyBytes) fail(QStringLiteral("This device's sync key is missing."));
    QFile file(path);
    if (!file.exists()) return SyncState{};
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxStateBytes) {
        fail(QStringLiteral("The local sync state is unreadable or too large; restore it from a backup."));
    }
    const QByteArray sealed = file.readAll();
    if (!sealed.startsWith(QByteArray(kMagic)) || sealed.size() < 4 + kNonceBytes + kTagBytes) {
        fail(QStringLiteral("The local sync state is damaged; restore it from a backup."));
    }
    const QByteArray nonce = sealed.mid(4, kNonceBytes);
    const QByteArray cipher = sealed.mid(4 + kNonceBytes);
    QByteArray plain(cipher.size() - kTagBytes, 0);
    unsigned long long plainSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &plainSize, nullptr,
            reinterpret_cast<const unsigned char*>(cipher.constData()),
            static_cast<unsigned long long>(cipher.size()), nullptr, 0,
            reinterpret_cast<const unsigned char*>(nonce.constData()),
            reinterpret_cast<const unsigned char*>(key.constData())) != 0) {
        sodium_memzero(plain.data(), size_t(plain.size()));
        fail(QStringLiteral("This device's sync state belongs to a different vault password. "
                            "Unlock the vault with the password it was synced with, or remove "
                            "state.enc to start over (that only forgets the token and record map)."));
    }
    plain.truncate(int(plainSize));

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(plain, &error);
    sodium_memzero(plain.data(), size_t(plain.size()));
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || document.object().value(QStringLiteral("version")).toInt() != 4) {
        fail(QStringLiteral("The local sync state is damaged; restore it from a backup."));
    }
    return SyncState::fromJson(document.object());
}

void saveState(const QString& path, const QByteArray& key, const SyncState& state) {
    ensureCrypto();
    if (key.size() != kKeyBytes) fail(QStringLiteral("This device's sync key is missing."));
    const QByteArray plain = QJsonDocument(state.toJson()).toJson(QJsonDocument::Compact);
    QByteArray nonce(kNonceBytes, 0);
    randombytes_buf(nonce.data(), nonce.size());
    QByteArray cipher(plain.size() + kTagBytes, 0);
    unsigned long long cipherSize = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(cipher.data()), &cipherSize,
        reinterpret_cast<const unsigned char*>(plain.constData()),
        static_cast<unsigned long long>(plain.size()), nullptr, 0, nullptr,
        reinterpret_cast<const unsigned char*>(nonce.constData()),
        reinterpret_cast<const unsigned char*>(key.constData()));
    cipher.truncate(int(cipherSize));

    QSaveFile out(path);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly)) fail(QStringLiteral("Cannot write the local sync state; check permissions."));
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray bytes = QByteArray(kMagic) + nonce + cipher;
    if (out.write(bytes) != bytes.size() || !out.commit()) {
        fail(QStringLiteral("Cannot save the local sync state durably; check free space."));
    }
}

SyncState loadState(const QString& path, const Keys& keys) {
    try {
        return loadState(path, keys.state);
    } catch (const std::runtime_error&) {
        // A state file written before the package key was stored was sealed
        // with the key derived from that package password; it opens once here
        // and the caller saves it again under the current key.
        if (keys.legacyState.size() != kKeyBytes) throw;
        return loadState(path, keys.legacyState);
    }
}

void resealState(const QString& directory, const Keys& from, const Keys& to) {
    if (to.state.size() != kKeyBytes) fail(QStringLiteral("The new sync key is missing."));
    const QString path = directory + QStringLiteral("/state.enc");
    const SyncState state = loadState(path, from);
    saveState(path, to.state, state);
}

bool resealForPasswordChange(const QString& stateDirectory, const QString& oldPassword,
                             const QString& newPassword) {
    try {
        Keys from = deriveKeys(stateDirectory, oldPassword, false);
        Keys to = deriveKeys(stateDirectory, newPassword, false);
        const bool sealed = [&] {
            resealState(stateDirectory, from, to);
            return true;
        }();
        from.wipe();
        to.wipe();
        return sealed;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace videovault::app::sync
