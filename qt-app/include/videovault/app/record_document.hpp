#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace videovault::app {

// One key/value pair of a record. `secret` is the only stored presentation
// flag: it marks a value that stays masked until the user asks to see it.
struct RecordField {
    QString name;
    QString value;
    bool secret{false};
};

// A flat key/value document. The core stores it as opaque bytes inside an
// ordinary encrypted package, so a record is an ordinary library item: same
// folders, tags, search, sharing and password-change behavior as media.
struct RecordDocument {
    QString title;
    QVector<RecordField> fields;
};

// Every serialized record starts with this marker, so the gallery can route an
// item by reading a few bytes of plaintext instead of trusting a file name.
inline constexpr char kRecordMarker[] = "{\"mvp_record\":";

// True when `head` (the first bytes of an item's plaintext) begins a record.
// Tolerates a UTF-8 BOM and leading whitespace.
bool looksLikeRecord(const QByteArray& head);

// The JSON the app writes. The marker is the first key and no whitespace
// precedes it, which is exactly what looksLikeRecord tests for.
QByteArray serializeRecord(const RecordDocument& record);

// Parses record content. False for malformed JSON or a missing marker; the
// caller shows the raw text rather than dropping it.
bool parseRecord(const QByteArray& bytes, RecordDocument* record);

// How a value should be presented. Inferred from the value itself so the UI can
// adapt without the user picking a type. Never stored: it is recomputed as the
// value changes, so it cannot go stale.
enum class ValueKind { Plain, Url, Email, Secret, Multiline, Number };

ValueKind classifyValue(const QString& value, bool secret);

// Short badge text for a kind (empty for Plain).
QString valueKindLabel(ValueKind kind);

} // namespace videovault::app
