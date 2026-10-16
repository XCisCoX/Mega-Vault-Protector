#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace videovault::app {

// The version written into new content.
//
// Version 1 (every field a plain string) is still read: a version 1 document is
// a valid version 2 document whose fields are all Text, so old records keep
// working with no rewriting pass and no data loss. Content that claims a version
// this build does not know is refused, and the editor shows it read-only rather
// than guessing at its meaning.
inline constexpr int kRecordFormatVersion = 2;

// One key/value pair of a record.
//
// `secret` is the only stored presentation flag. `json` says whether `value`
// holds JSON rather than a plain string, which is what lets a record carry a
// number, an object or an array and still keep a simple password simple.
//
// Both kinds keep their value as TEXT, not as a parsed tree: an editor can then
// show exactly what the user typed, an unfinished edit is never silently
// rewritten, and a value that stops parsing is reported instead of discarded.
struct RecordField {
    QString name;
    QString value;
    bool secret{false};
    bool json{false};
    // Members of this field that this build does not recognise. They are kept so
    // that data written by a later version survives an edit here.
    QJsonObject extras;
};

// A flat key/value document. The core stores it as opaque bytes inside an
// ordinary encrypted package, so a record is an ordinary library item: the same
// folders, tags, search, sharing and password-change behavior as media.
struct RecordDocument {
    QString title;
    QVector<RecordField> fields;
    // Unknown members of the document itself, kept for the same reason as
    // RecordField::extras.
    QJsonObject extras;
};

// Every serialized record starts with this marker, so the gallery can route an
// item by reading a few bytes of plaintext instead of trusting a file name.
inline constexpr char kRecordMarker[] = "{\"mvp_record\":";

// True when `head` (the first bytes of an item's plaintext) begins a record.
// Tolerates a UTF-8 BOM and leading whitespace.
bool looksLikeRecord(const QByteArray& head);

// The stored format version, or 0 when the bytes are not a record this build can
// read (not a record at all, malformed, or a newer version).
int recordFormatVersion(const QByteArray& bytes);

// Parses record content. False for malformed JSON, a missing marker, or a
// version this build does not know; the caller shows the raw text rather than
// dropping it.
bool parseRecord(const QByteArray& bytes, RecordDocument* record);

// True when `text` is a single JSON value: an object, an array, a string, a
// number, true, false, or null.
bool isValidJsonValue(const QString& text);

// Why the document cannot be stored, empty when it can. Today the only reason is
// a field marked Json whose text does not parse, which is what the editor checks
// before saving so an unfinished JSON edit is reported instead of stored.
QString recordProblem(const RecordDocument& record);

// The JSON the app writes, at kRecordFormatVersion, with the marker as the first
// bytes and no whitespace before it (which is what looksLikeRecord tests for).
//
// The `json` marker on a field means the stored value really is a JSON value, so
// it is only written when the text parses. A field marked Json whose text does
// not parse is written as a plain string with the marker dropped: the text is
// kept character for character, and the format never has to represent the
// contradiction. recordProblem is what reports that case to the user.
QByteArray serializeRecord(const RecordDocument& record);

// How a value should be presented. Inferred from the value itself so the UI can
// adapt without the user picking a type. Never stored: it is recomputed as the
// value changes.
enum class ValueKind { Plain, Url, Email, Secret, Multiline, Number };

ValueKind classifyValue(const QString& value, bool secret);

// Short badge text for a kind (empty for Plain).
QString valueKindLabel(ValueKind kind);

// The badge text for a whole field: "json" for a JSON value, otherwise the
// inferred kind of its text.
QString recordFieldKindLabel(const RecordField& field);

} // namespace videovault::app
