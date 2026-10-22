#include "videovault/app/secure_input.hpp"

#include <QLineEdit>

#include <sodium.h>

#include <span>

namespace videovault::app {

void wipeString(QString& text) noexcept {
    if (text.isEmpty()) {
        return;
    }
    // QString::data() detaches in Qt 5, so the buffer being zeroed is owned by
    // this object alone and the shared copy stays valid.
    QChar* data = text.data();
    const int count = text.size();
    if (data != nullptr && count > 0) {
        sodium_memzero(data, static_cast<std::size_t>(count) * sizeof(QChar));
    }
    text.clear();
}

void wipeByteArray(QByteArray& bytes) noexcept {
    if (bytes.isEmpty()) {
        return;
    }
    if (!bytes.isNull()) {
        sodium_memzero(bytes.data(), static_cast<std::size_t>(bytes.size()));
    }
    bytes.clear();
}

std::optional<core::Secret> secretFromQString(const QString& text) {
    if (text.isEmpty()) {
        return std::nullopt;
    }
    const auto* units = reinterpret_cast<const char16_t*>(text.utf16());
    return core::Secret::from_utf16(
        std::span<const char16_t>(units, static_cast<std::size_t>(text.size())));
}

std::optional<core::Secret> takeSecretFromLineEdit(QLineEdit* field) {
    if (field == nullptr) {
        return std::nullopt;
    }
    QString text = field->text();
    // Clear the field before the (slow) Argon2id work starts: the password must
    // not stay visible or reachable through the widget while a worker runs.
    field->clear();
    auto secret = secretFromQString(text);
    wipeString(text);
    return secret;
}

} // namespace videovault::app
