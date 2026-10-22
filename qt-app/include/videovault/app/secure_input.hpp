#pragma once

// Qt-side helpers for moving a password from a widget or a QString into the
// core's secure-memory Secret type, and for wiping the strings that are under
// this application's control.
//
// Framework limitation (documented in docs/security/REMAINING_RISKS.md): Qt's
// QString is implicitly shared and QLineEdit owns an internal copy. This code
// removes the copies the application itself creates and zeroes the buffers it
// owns, but Qt may have allocated and freed other copies along the way. A
// QString cannot be overwritten in place while another owner shares it.

#include "videovault/core/secret.hpp"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <optional>

class QLineEdit;

namespace videovault::app {

// Overwrites this QString's buffer with zeros, then empties it. Detaches first,
// so a shared buffer is not corrupted and only this object's copy is wiped.
void wipeString(QString& text) noexcept;

// Overwrites the bytes of a QByteArray (sodium_memzero), then clears it.
void wipeByteArray(QByteArray& bytes) noexcept;

// Builds a Secret from UTF-16 text without an intermediate UTF-8 QByteArray.
[[nodiscard]] std::optional<core::Secret> secretFromQString(const QString& text);

// Reads a password field, clears the widget, and returns the bytes as a Secret.
// The widget is cleared whether or not the conversion succeeds, so the visible
// field does not keep the password while a worker runs.
[[nodiscard]] std::optional<core::Secret> takeSecretFromLineEdit(QLineEdit* field);

} // namespace videovault::app
