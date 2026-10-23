#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

namespace videovault::app {

// The version written into new content.
//
// Version 3 replaces the flat field list of versions 1 and 2 with a *file of
// records*: each record is a title plus a JSON object body, and every value in
// that body carries an explicit type. Versions 1 and 2 are not read here any
// more; `convertLegacyRecord` rewrites them in place (keeping each item's id,
// folder, tags and imported time), and `recordFormatVersion` reports 1 or 2 so the
// editor can offer that instead of showing a broken record.
inline constexpr int kRecordFormatVersion = 3;

// What a value is. The type is chosen by the user, never sniffed from the text:
// "1234" stays a string unless a number was asked for, and text that happens to
// look like JSON stays text.
//
// Secret is a string the UI masks until the user reveals it. It is stored as a
// plain JSON string; which strings are secret lives in a sidecar list of paths
// (see serializeRecord), so the body stays a pure JSON object that any JSON tool
// can read and no wrapper key can collide with real data.
enum class ValueType { String, Number, Secret, Object, Array };

// One value in a record's body.
//
// Leaves keep their payload as TEXT rather than a parsed tree: the editor shows
// exactly what was entered, and an unfinished edit is never rewritten behind the
// user's back. Containers own their children: an Object holds key/value members
// kept sorted by key, an Array holds unnamed values in order.
struct RecordNode {
    ValueType type{ValueType::String};
    QString text;                                // String/Number/Secret payload
    QVector<RecordNode> items;                   // Array elements, in order
    QVector<QPair<QString, RecordNode>> members; // Object members, sorted by key

    static RecordNode leaf(ValueType type, QString text);
    static RecordNode object();
    static RecordNode array();

    bool isContainer() const;
};

// A value a template computes when it is used.
//
// Templates reuse this container instead of defining a second format: a template is a
// record file flagged `template`, whose entries carry the *sample* value in the body so
// they can be read and edited like any record, plus these generators. Using a template
// replaces each sample with a fresh value and drops the generator, which is what makes
// the result an ordinary record with nothing special left in it.
enum class GeneratorKind { Password, Uuid, Date };

struct GeneratedValue {
    QString pointer;                      // JSON Pointer (RFC 6901) into the body
    GeneratorKind kind{GeneratorKind::Password};
    int length{20};                       // password only; ignored otherwise
};

// One record inside the file: a title and an object body.
struct RecordEntry {
    QString title;
    RecordNode body{RecordNode::object()};
    QVector<GeneratedValue> generated;    // empty for ordinary records
};

// The content of one library item: an ordered list of records. The user moves
// records up and down; the keys inside each body are sorted, not ordered.
//
// `isTemplate` marks a file whose entries are templates to start records from. It is
// stored as a top-level `"template":true`, so the same parser, serializer and editor
// handle both kinds, and the gallery can tell them apart without a second format.
struct RecordDocument {
    QVector<RecordEntry> entries;
    bool isTemplate{false};
    // What the template is called in the picker ("Website login"). Separate from the
    // entry's title, which is the placeholder name the created record starts with.
    QString templateName;
};

// Every serialized record starts with this marker, so the gallery can route an
// item by reading a few bytes of plaintext instead of trusting a file name.
inline constexpr char kRecordMarker[] = "{\"mvp_record\":";

// True when `head` (the first bytes of an item's plaintext) begins a record.
// Tolerates a UTF-8 BOM and leading whitespace.
bool looksLikeRecord(const QByteArray& head);

// True when those bytes begin a *template*: a record file flagged `"template":true`.
// The flag sits right after the version, so a gallery can classify an item from a
// 64-byte read instead of decrypting it in full.
bool looksLikeTemplate(const QByteArray& head);

// The stored format version, or 0 when the bytes are not a record at all.
// 1 and 2 mean "a pre-v3 record the editor can migrate"; 3 is the only version
// parseRecord accepts.
int recordFormatVersion(const QByteArray& bytes);

// Parses version 3 content into `document`. False for malformed JSON, a missing
// marker, a version other than 3, or a body this build cannot represent (a
// boolean or null value, which the five types do not cover). The caller shows
// such content read-only rather than dropping it.
bool parseRecord(const QByteArray& bytes, RecordDocument* document);

// The JSON this build writes, with the marker as the first bytes and no
// whitespace before it:
//
//   {"mvp_record":3,"entries":[
//      {"title":"Web Services",
//       "body":{"another.com":{"api.another.com":"pw"},"example.com":"1234"},
//       "secret":["another.com/api.another.com"]}]}
//
// `secret` lists paths to strings that are secret, using JSON Pointer tokens
// (RFC 6901: `~` is written `~0`, `/` is written `~1`, array elements are their
// index). It is omitted when a record has no secrets.
//
// A Number node whose text is not a valid JSON number is written as a quoted
// string instead, so the container is never broken and no text is lost;
// recordProblem is what reports that case before it can be stored.
QByteArray serializeRecord(const RecordDocument& document);

// Parses one JSON value written as text ("1234", "\"x\"", "{\"a\":1}") into a
// node. False when the text is not a JSON value this build can represent. The
// v1/v2 migration uses this to keep a stored JSON field as JSON instead of
// turning it into text.
bool parseJsonValue(const QString& text, RecordNode* node);

// Converts version 1/2 content (one flat field list) into one version 3 record:
// the field list becomes the body of a single record titled by `fallbackTitle`
// when the old document has no title, `secret` flags become secret strings, and
// fields marked JSON stay JSON. False when the bytes are not readable v1/v2
// content. `note` (optional) receives a warning when a field claimed JSON but no
// longer parsed and had to be kept as text. Version 1 and 2 content is not read
// by parseRecord, so this is the only way across.
bool convertLegacyRecord(const QByteArray& bytes, const QString& fallbackTitle,
    RecordDocument* document, QString* note = nullptr);

// --- validation shared by the editor and the format ------------------------

// JSON number grammar: optional minus, no leading zeros, optional fraction and
// exponent. Nothing else ("1,2", "1.", "0x10", "NaN") passes.
bool isValidNumber(const QString& text);

// The canonical text of a number this build can store: "1e3" becomes "1000" and
// "1.50" becomes "1.5". Empty when the text is not such a number — including
// integers beyond 2^53, whose decimal digits a JSON parser cannot keep (use a
// string for those, and the editor says so instead of changing the value).
QString canonicalNumber(const QString& text);

// A key must not be empty once surrounding whitespace is ignored.
bool isValidKey(const QString& key);

// Empty when the document can be stored; otherwise the first problem found,
// naming the record and the key path so the user can go and fix it.
QString recordProblem(const RecordDocument& document);

// --- tree helpers ----------------------------------------------------------

// Sorts every object's members by key, case-insensitively, and recurses. Called
// after an add or a rename and on parse, so the file and the screen agree.
void sortMembers(RecordNode* object);

// True when this subtree contains a secret, which is what decides whether the
// "reveal all" affordance is offered.
bool nodeHasSecret(const RecordNode& node);

// "string", "number", "secret", "object", "array" — the badge on a row.
QString valueTypeLabel(ValueType type);

// --- templates -------------------------------------------------------------

// "password", "uuid", "date" — the stored name of a generator.
QString generatorKindName(GeneratorKind kind);
bool generatorKindFromName(const QString& name, GeneratorKind* kind);

// Replaces every generated value in `entry` with a fresh one and clears the list.
// False (with `problem` set, naming the path) when a pointer does not resolve to a
// string: a template whose generator cannot be applied is reported rather than leaving
// the sample value in what the user thinks is a generated field.
bool applyGenerators(RecordEntry* entry, QString* problem = nullptr);

// The ordinary record a template produces: one entry, generators applied and dropped,
// and the template flag off. `title` replaces the template's placeholder title when it
// is not empty, because the user names the thing they just created.
RecordDocument cloneTemplate(const RecordEntry& templateEntry,
    const QString& title = QString());

// Where a user's own templates live.
//
// A template is not a file in the library: it is a store the app owns. They sit in one
// folder whose name starts with a dot, the gallery drops that folder when it loads the
// library, and the picker is where they are managed. Keeping them as ordinary vault items
// means they are encrypted and synced like everything else; keeping them out of the gallery
// means a template never turns up as a file somebody has to understand.
QString templateFolderName();
bool isTemplateFolderName(const QString& name);

// The templates shipped with the app, read from :/templates and parsed with
// parseRecord, so a built-in is exactly the format a user's own template is.
QVector<RecordDocument> builtinTemplates();

// Inserts a template's structure into `target`, which must be an object, under `key`.
//
// This is the "part of a record" use: the template's body becomes a nested object, with
// its generators applied on the way in, so a password template inserted mid-edit arrives
// already generated. Because the generators are applied rather than carried over, there is
// no path to re-root: the values are already in place and the inserted subtree is ordinary.
//
// False (with `problem` set) when the target is not an object, the key is empty or already
// a field there, or the template's own generator paths do not resolve.
bool insertTemplate(RecordNode* target, const RecordEntry& templateEntry, const QString& key,
    QString* problem = nullptr);

} // namespace videovault::app
