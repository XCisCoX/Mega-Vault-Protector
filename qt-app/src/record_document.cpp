#include "videovault/app/record_document.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace videovault::app {
namespace {

QString escapeJson(const QString& text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '\"':
            out += QStringLiteral("\\\"");
            break;
        case '\\':
            out += QStringLiteral("\\\\");
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        case '\t':
            out += QStringLiteral("\\t");
            break;
        case '\b':
            out += QStringLiteral("\\b");
            break;
        case '\f':
            out += QStringLiteral("\\f");
            break;
        default:
            if (ch.unicode() < 0x20) {
                out += QStringLiteral("\\u%1").arg(ch.unicode(), 4, 16, QLatin1Char('0'));
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

// Index of the first non-whitespace byte, skipping a UTF-8 BOM.
int firstContentByte(const QByteArray& head) {
    int index = head.startsWith("\xEF\xBB\xBF") ? 3 : 0;
    while (index < head.size()) {
        const char byte = head.at(index);
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            break;
        }
        ++index;
    }
    return index;
}

const QRegularExpression& numberPattern() {
    // JSON's own grammar. No leading '+', no leading zeros, no bare '.', no
    // leading/trailing whitespace (callers trim first).
    static const QRegularExpression pattern(
        QStringLiteral("^-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?$"));
    return pattern;
}

// Shortest text that reads back as the same double. Qt keeps JSON numbers as
// doubles, so this is also the text a stored number will come back as.
QString numberToText(const double value) {
    if (!std::isfinite(value)) {
        return QString();
    }
    // Plain digits for whole numbers; "1e+06" is valid JSON but is not what the
    // user typed for a count.
    if (std::fabs(value) < 1e15 && value == std::trunc(value)) {
        return QString::number(static_cast<qlonglong>(value));
    }
    for (int precision = 1; precision <= 17; ++precision) {
        const QString text = QString::number(value, 'g', precision);
        bool ok = false;
        const double back = text.toDouble(&ok);
        if (ok && back == value) {
            return text;
        }
    }
    return QString::number(value, 'g', 17);
}

// JSON Pointer tokens (RFC 6901) are how a secret's location is written down.
// The compact JSON text of a stored value, so a v2 "json" field goes through the
// same value parser as everything else.
QString legacyValueText(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }
    QJsonArray wrapper;
    wrapper.append(value);
    const QByteArray text = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(text.mid(1, text.size() - 2));
}

// The digits that carry the value: leading and trailing zeros and an exponent
// are presentation, everything else is the number itself.
QString significantDigits(const QString& numberText) {
    QString digits;
    for (const QChar ch : numberText) {
        if (ch.isDigit()) {
            digits += ch;
        } else if (ch == QLatin1Char('e') || ch == QLatin1Char('E')) {
            break; // the exponent is scale, not precision
        }
    }
    while (!digits.isEmpty() && digits.at(0) == QLatin1Char('0')) {
        digits.remove(0, 1);
    }
    while (!digits.isEmpty() && digits.at(digits.size() - 1) == QLatin1Char('0')) {
        digits.chop(1);
    }
    return digits;
}

QString pointerEscape(QString token) {
    token.replace(QLatin1Char('~'), QStringLiteral("~0"));
    token.replace(QLatin1Char('/'), QStringLiteral("~1"));
    return token;
}

QString pointerUnescape(QString token) {
    token.replace(QStringLiteral("~1"), QStringLiteral("/"));
    token.replace(QStringLiteral("~0"), QStringLiteral("~"));
    return token;
}

QString joinPath(const QString& prefix, const QString& token) {
    // Each token is escaped on its own: escaping the joined path would escape
    // the separators as well, and no stored path would ever match again.
    const QString safe = pointerEscape(token);
    return prefix.isEmpty() ? safe : prefix + QLatin1Char('/') + safe;
}

// A secret is a string, so only leaves are collected; an object or array is
// never itself a secret.
void collectSecrets(const RecordNode& node, const QString& path, QStringList* out) {
    if (out == nullptr) {
        return;
    }
    if (node.type == ValueType::Secret) {
        out->append(path);
        return;
    }
    if (node.type == ValueType::Object) {
        for (const auto& member : node.members) {
            collectSecrets(member.second, joinPath(path, member.first), out);
        }
        return;
    }
    if (node.type == ValueType::Array) {
        for (int index = 0; index < node.items.size(); ++index) {
            collectSecrets(node.items.at(index), joinPath(path, QString::number(index)), out);
        }
    }
}

// Marks the strings named by `paths` as secret. Unknown paths are ignored: a
// hand-edited file must not make the record unreadable.
int applySecrets(RecordNode* node, const QString& path, const QSet<QString>& paths) {
    if (node == nullptr) {
        return 0;
    }
    if (node->type == ValueType::Object) {
        int applied = 0;
        for (auto& member : node->members) {
            applied += applySecrets(&member.second, joinPath(path, member.first), paths);
        }
        return applied;
    }
    if (node->type == ValueType::Array) {
        int applied = 0;
        for (int index = 0; index < node->items.size(); ++index) {
            applied += applySecrets(&node->items[index], joinPath(path, QString::number(index)), paths);
        }
        return applied;
    }
    if (node->type == ValueType::String && paths.contains(path)) {
        node->type = ValueType::Secret;
        return 1;
    }
    return 0;
}

bool appendNode(const RecordNode& node, QString* out) {
    if (out == nullptr) {
        return false;
    }
    switch (node.type) {
    case ValueType::Object:
        *out += QLatin1Char('{');
        for (int index = 0; index < node.members.size(); ++index) {
            if (index > 0) {
                *out += QLatin1Char(',');
            }
            *out += QLatin1Char('\"');
            *out += escapeJson(node.members.at(index).first);
            *out += QStringLiteral("\":");
            if (!appendNode(node.members.at(index).second, out)) {
                return false;
            }
        }
        *out += QLatin1Char('}');
        return true;
    case ValueType::Array:
        *out += QLatin1Char('[');
        for (int index = 0; index < node.items.size(); ++index) {
            if (index > 0) {
                *out += QLatin1Char(',');
            }
            if (!appendNode(node.items.at(index), out)) {
                return false;
            }
        }
        *out += QLatin1Char(']');
        return true;
    case ValueType::Number: {
        // A number that is not representable is written as a string: the text is
        // kept exactly and the JSON stays valid. recordProblem is what stops the
        // user from storing that state in the first place.
        const QString canonical = canonicalNumber(node.text);
        if (!canonical.isEmpty()) {
            *out += canonical;
            return true;
        }
        *out += QLatin1Char('\"');
        *out += escapeJson(node.text);
        *out += QLatin1Char('\"');
        return true;
    }
    case ValueType::String:
    case ValueType::Secret:
        break;
    }
    *out += QLatin1Char('\"');
    *out += escapeJson(node.text);
    *out += QLatin1Char('\"');
    return true;
}

// Parses the container once and reports the version it carries. Versions 1 and 2
// are recognised so recordFormatVersion can ask the user to migrate them.
bool readRoot(const QByteArray& bytes, QJsonObject* root, int* version) {
    if (root == nullptr || !looksLikeRecord(bytes)) {
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    *root = document.object();
    const QJsonValue marker = root->value(QStringLiteral("mvp_record"));
    if (!marker.isDouble()) {
        return false;
    }
    const int found = marker.toInt();
    if (found < 1 || found > kRecordFormatVersion) {
        return false;
    }
    if (version != nullptr) {
        *version = found;
    }
    return true;
}

// Converts one stored JSON value into a node. Booleans and null have no type in
// this model, so they make the record unreadable rather than being coerced into
// text that would be written back differently.
bool nodeFromJson(const QJsonValue& value, RecordNode* node) {
    if (node == nullptr) {
        return false;
    }
    if (value.isString()) {
        *node = RecordNode::leaf(ValueType::String, value.toString());
        return true;
    }
    if (value.isDouble()) {
        const QString text = numberToText(value.toDouble());
        if (text.isEmpty()) {
            return false;
        }
        *node = RecordNode::leaf(ValueType::Number, text);
        return true;
    }
    if (value.isObject()) {
        RecordNode object = RecordNode::object();
        const QJsonObject source = value.toObject();
        object.members.reserve(source.size());
        for (auto it = source.begin(); it != source.end(); ++it) {
            RecordNode child;
            if (!nodeFromJson(it.value(), &child)) {
                return false;
            }
            object.members.append({it.key(), child});
        }
        *node = object;
        return true;
    }
    if (value.isArray()) {
        RecordNode array = RecordNode::array();
        const QJsonArray source = value.toArray();
        array.items.reserve(source.size());
        for (const QJsonValue& element : source) {
            RecordNode child;
            if (!nodeFromJson(element, &child)) {
                return false;
            }
            array.items.append(child);
        }
        *node = array;
        return true;
    }
    return false;
}

void sortMembersInto(RecordNode* node) {
    if (node == nullptr) {
        return;
    }
    if (node->type == ValueType::Object) {
        std::sort(node->members.begin(), node->members.end(),
            [](const QPair<QString, RecordNode>& left, const QPair<QString, RecordNode>& right) {
                const int insensitive = QString::compare(left.first, right.first, Qt::CaseInsensitive);
                if (insensitive != 0) {
                    return insensitive < 0;
                }
                return QString::compare(left.first, right.first) < 0;
            });
        for (auto& member : node->members) {
            sortMembersInto(&member.second);
        }
        return;
    }
    if (node->type == ValueType::Array) {
        for (auto& item : node->items) {
            sortMembersInto(&item);
        }
    }
}

// Why this subtree cannot be stored, naming where the problem is. Empty when the
// subtree is fine.
QString problemIn(const RecordNode& node, const QString& path) {
    switch (node.type) {
    case ValueType::Object: {
        QSet<QString> seen;
        for (const auto& member : node.members) {
            if (!isValidKey(member.first)) {
                return QStringLiteral("%1 has a field with an empty name.").arg(path);
            }
            if (seen.contains(member.first)) {
                return QStringLiteral("%1 has two fields named \"%2\".").arg(path, member.first);
            }
            seen.insert(member.first);
            const QString problem = problemIn(member.second, path + QStringLiteral(" / ") + member.first);
            if (!problem.isEmpty()) {
                return problem;
            }
        }
        return QString();
    }
    case ValueType::Array:
        for (int index = 0; index < node.items.size(); ++index) {
            const QString problem = problemIn(
                node.items.at(index), path + QStringLiteral("[%1]").arg(index));
            if (!problem.isEmpty()) {
                return problem;
            }
        }
        return QString();
    case ValueType::Number:
        if (canonicalNumber(node.text).isEmpty()) {
            return QStringLiteral("%1 is a number, but \"%2\" is not one this build can "
                                  "store. Use a string for it, or a plain decimal number.")
                .arg(path, node.text);
        }
        return QString();
    case ValueType::String:
    case ValueType::Secret:
        return QString();
    }
    return QString();
}

} // namespace

RecordNode RecordNode::leaf(const ValueType type, QString text) {
    RecordNode node;
    node.type = type;
    node.text = std::move(text);
    return node;
}

RecordNode RecordNode::object() {
    RecordNode node;
    node.type = ValueType::Object;
    return node;
}

RecordNode RecordNode::array() {
    RecordNode node;
    node.type = ValueType::Array;
    return node;
}

bool RecordNode::isContainer() const {
    return type == ValueType::Object || type == ValueType::Array;
}

bool looksLikeRecord(const QByteArray& head) {
    const QByteArray marker(kRecordMarker);
    const int start = firstContentByte(head);
    if (start + marker.size() > head.size()) {
        return false;
    }
    return head.mid(start, marker.size()) == marker;
}

int recordFormatVersion(const QByteArray& bytes) {
    QJsonObject root;
    int version = 0;
    if (!readRoot(bytes, &root, &version)) {
        return 0;
    }
    return version;
}

bool isValidNumber(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    return numberPattern().match(trimmed).hasMatch();
}

QString canonicalNumber(const QString& text) {
    if (!isValidNumber(text)) {
        return QString();
    }
    const QString trimmed = text.trimmed();
    bool ok = false;
    const double value = trimmed.toDouble(&ok);
    if (!ok) {
        return QString();
    }
    const QString canonical = numberToText(value);
    if (canonical.isEmpty()) {
        return QString();
    }
    // Formatting differences are fine and get normalised ("1e3" -> "1000",
    // "1.50" -> "1.5"). A change in the significant digits is not: an integer
    // beyond 2^53 comes back with different digits, and that value belongs in a
    // string instead of being silently rewritten.
    if (significantDigits(trimmed) != significantDigits(canonical)) {
        return QString();
    }
    return canonical;
}

bool isValidKey(const QString& key) {
    return !key.trimmed().isEmpty();
}

QString recordProblem(const RecordDocument& document) {
    for (int index = 0; index < document.entries.size(); ++index) {
        const RecordEntry& entry = document.entries.at(index);
        const QString name = entry.title.trimmed().isEmpty()
            ? QStringLiteral("Record %1").arg(index + 1)
            : QStringLiteral("\"%1\"").arg(entry.title.trimmed());
        const QString problem = problemIn(entry.body, name);
        if (!problem.isEmpty()) {
            return problem;
        }
    }
    return QString();
}

QByteArray serializeRecord(const RecordDocument& document) {
    // Sorted on the way out, so the file and the screen agree even if a caller
    // forgot to sort after an edit.
    RecordDocument sorted = document;
    for (auto& entry : sorted.entries) {
        sortMembersInto(&entry.body);
    }

    QString json = QStringLiteral("{\"mvp_record\":");
    json += QString::number(kRecordFormatVersion);
    json += QStringLiteral(",\"entries\":[");
    for (int index = 0; index < sorted.entries.size(); ++index) {
        if (index > 0) {
            json += QLatin1Char(',');
        }
        const RecordEntry& entry = sorted.entries.at(index);
        json += QStringLiteral("{\"title\":\"");
        json += escapeJson(entry.title);
        json += QStringLiteral("\",\"body\":");
        if (!appendNode(entry.body, &json)) {
            return QByteArray();
        }
        QStringList secrets;
        collectSecrets(entry.body, QString(), &secrets);
        if (!secrets.isEmpty()) {
            json += QStringLiteral(",\"secret\":[");
            for (int secret = 0; secret < secrets.size(); ++secret) {
                if (secret > 0) {
                    json += QLatin1Char(',');
                }
                json += QLatin1Char('\"');
                json += escapeJson(secrets.at(secret));
                json += QLatin1Char('\"');
            }
            json += QLatin1Char(']');
        }
        json += QLatin1Char('}');
    }
    json += QStringLiteral("]}");
    return json.toUtf8();
}

bool parseRecord(const QByteArray& bytes, RecordDocument* document) {
    if (document == nullptr) {
        return false;
    }
    QJsonObject root;
    int version = 0;
    if (!readRoot(bytes, &root, &version) || version != kRecordFormatVersion) {
        return false;
    }
    const QJsonValue entriesValue = root.value(QStringLiteral("entries"));
    if (!entriesValue.isArray()) {
        return false;
    }

    RecordDocument parsed;
    const QJsonArray entries = entriesValue.toArray();
    parsed.entries.reserve(entries.size());
    for (const QJsonValue& entryValue : entries) {
        if (!entryValue.isObject()) {
            return false;
        }
        const QJsonObject entryObject = entryValue.toObject();
        RecordEntry entry;

        const QJsonValue title = entryObject.value(QStringLiteral("title"));
        if (title.isString()) {
            entry.title = title.toString();
        } else if (!title.isUndefined()) {
            return false;
        }

        const QJsonValue body = entryObject.value(QStringLiteral("body"));
        if (!body.isUndefined()) {
            if (!body.isObject() || !nodeFromJson(body, &entry.body)) {
                return false;
            }
        }

        const QJsonValue secrets = entryObject.value(QStringLiteral("secret"));
        if (secrets.isArray()) {
            QSet<QString> paths;
            for (const QJsonValue& path : secrets.toArray()) {
                if (!path.isString()) {
                    return false;
                }
                paths.insert(path.toString());
            }
            if (!paths.isEmpty()) {
                applySecrets(&entry.body, QString(), paths);
            }
        } else if (!secrets.isUndefined()) {
            return false;
        }

        sortMembersInto(&entry.body);
        parsed.entries.push_back(entry);
    }
    *document = parsed;
    return true;
}

bool parseJsonValue(const QString& text, RecordNode* node) {
    if (node == nullptr) {
        return false;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    // Qt 5 only accepts an object or an array at the top level, so a scalar is
    // parsed inside a one-element array and unpacked again.
    const QByteArray wrapped = QByteArray("[") + trimmed.toUtf8() + QByteArray("]");
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(wrapped, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return false;
    }
    const QJsonArray elements = document.array();
    if (elements.size() != 1) {
        return false;
    }
    return nodeFromJson(elements.at(0), node);
}

bool convertLegacyRecord(const QByteArray& bytes, const QString& fallbackTitle,
    RecordDocument* document, QString* note) {
    if (document == nullptr) {
        return false;
    }
    const int version = recordFormatVersion(bytes);
    if (version != 1 && version != 2) {
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        return false;
    }
    const QJsonObject root = parsed.object();

    RecordEntry entry;
    entry.title = root.value(QStringLiteral("title")).toString().trimmed();
    if (entry.title.isEmpty()) {
        entry.title = fallbackTitle.trimmed();
    }
    if (entry.title.isEmpty()) {
        entry.title = QStringLiteral("Record");
    }

    int keptAsText = 0;
    const QJsonArray fields = root.value(QStringLiteral("fields")).toArray();
    for (const QJsonValue& value : fields) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject field = value.toObject();
        QString name = field.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            name = QStringLiteral("field");
        }
        // A JSON object cannot hold one name twice; the later ones are numbered.
        QString unique = name;
        int suffix = 2;
        for (;;) {
            bool taken = false;
            for (const auto& member : entry.body.members) {
                if (member.first == unique) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                break;
            }
            unique = QStringLiteral("%1 (%2)").arg(name).arg(suffix++);
        }

        const bool secret = field.value(QStringLiteral("secret")).toBool(false);
        const QJsonValue stored = field.value(QStringLiteral("value"));
        const bool asJson = field.value(QStringLiteral("json")).toBool(false) || !stored.isString();

        RecordNode node;
        if (asJson) {
            if (!parseJsonValue(legacyValueText(stored), &node)) {
                // Text that claimed to be JSON and no longer parses is kept as
                // text rather than dropped, and the caller is told.
                node = RecordNode::leaf(ValueType::String, legacyValueText(stored));
                ++keptAsText;
            }
        } else {
            node = RecordNode::leaf(ValueType::String, stored.toString());
        }
        if (secret && node.type == ValueType::String) {
            node.type = ValueType::Secret;
        }
        entry.body.members.append({unique, node});
    }

    sortMembersInto(&entry.body);
    document->entries.append(entry);
    if (note != nullptr) {
        note->clear();
        if (keptAsText > 0) {
            *note = QStringLiteral("%1 JSON field(s) no longer parsed and were kept as text.")
                        .arg(keptAsText);
        }
    }
    return true;
}

void sortMembers(RecordNode* object) {
    sortMembersInto(object);
}

bool nodeHasSecret(const RecordNode& node) {
    if (node.type == ValueType::Secret) {
        return true;
    }
    if (node.type == ValueType::Object) {
        for (const auto& member : node.members) {
            if (nodeHasSecret(member.second)) {
                return true;
            }
        }
        return false;
    }
    if (node.type == ValueType::Array) {
        for (const auto& item : node.items) {
            if (nodeHasSecret(item)) {
                return true;
            }
        }
    }
    return false;
}

QString valueTypeLabel(const ValueType type) {
    switch (type) {
    case ValueType::String:
        return QStringLiteral("string");
    case ValueType::Number:
        return QStringLiteral("number");
    case ValueType::Secret:
        return QStringLiteral("secret");
    case ValueType::Object:
        return QStringLiteral("object");
    case ValueType::Array:
        return QStringLiteral("array");
    }
    return QString();
}

} // namespace videovault::app
