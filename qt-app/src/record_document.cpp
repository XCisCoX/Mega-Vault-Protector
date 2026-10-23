#include "videovault/app/record_document.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
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

// Defined with the template helpers below; parseRecord uses it to check that a
// generator's path really names a string.
RecordNode* nodeAtPointer(RecordNode& root, const QString& pointer);

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

bool looksLikeTemplate(const QByteArray& head) {
    // The name is optional and may follow, so only the flag is required here.
    return looksLikeRecord(head) && head.contains("\"template\":true");
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
    if (sorted.isTemplate) {
        json += QStringLiteral(",\"template\":true");
        if (!sorted.templateName.trimmed().isEmpty()) {
            json += QStringLiteral(",\"name\":\"");
            json += escapeJson(sorted.templateName.trimmed());
            json += QLatin1Char('"');
        }
    }
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
        // Ordinary records have no generators, so this key never appears in one.
        if (!entry.generated.isEmpty()) {
            json += QStringLiteral(",\"generate\":[");
            for (int generator = 0; generator < entry.generated.size(); ++generator) {
                if (generator > 0) {
                    json += QLatin1Char(',');
                }
                const GeneratedValue& value = entry.generated.at(generator);
                json += QStringLiteral("{\"path\":\"");
                json += escapeJson(value.pointer);
                json += QStringLiteral("\",\"kind\":\"");
                json += generatorKindName(value.kind);
                json += QLatin1Char('\"');
                if (value.kind == GeneratorKind::Password) {
                    json += QStringLiteral(",\"length\":");
                    json += QString::number(value.length);
                }
                json += QLatin1Char('}');
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
    const QJsonValue templateValue = root.value(QStringLiteral("template"));
    if (!templateValue.isUndefined()) {
        if (!templateValue.isBool()) {
            return false;
        }
        parsed.isTemplate = templateValue.toBool();
    }
    const QJsonValue nameValue = root.value(QStringLiteral("name"));
    if (!nameValue.isUndefined()) {
        if (!nameValue.isString()) {
            return false;
        }
        parsed.templateName = nameValue.toString();
    }
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

        const QJsonValue generated = entryObject.value(QStringLiteral("generate"));
        if (!generated.isUndefined()) {
            if (!generated.isArray()) {
                return false;
            }
            for (const QJsonValue& item : generated.toArray()) {
                if (!item.isObject()) {
                    return false;
                }
                const QJsonObject object = item.toObject();
                const QJsonValue path = object.value(QStringLiteral("path"));
                const QJsonValue kind = object.value(QStringLiteral("kind"));
                if (!path.isString() || path.toString().isEmpty() || !kind.isString()) {
                    return false;
                }
                GeneratedValue value;
                value.pointer = path.toString();
                if (!generatorKindFromName(kind.toString(), &value.kind)) {
                    return false;
                }
                const QJsonValue length = object.value(QStringLiteral("length"));
                if (!length.isUndefined()) {
                    if (!length.isDouble()) {
                        return false;
                    }
                    value.length = length.toInt();
                }
                if (value.kind == GeneratorKind::Password && (value.length < 8 || value.length > 128)) {
                    return false;
                }
                entry.generated.push_back(value);
            }
        }

        sortMembersInto(&entry.body);
        // A generator whose path does not name a string would silently keep the sample
        // value, so it is rejected here and the editor shows the file read-only.
        for (const GeneratedValue& value : entry.generated) {
            const RecordNode* target = nodeAtPointer(entry.body, value.pointer);
            if (target == nullptr
                || (target->type != ValueType::String && target->type != ValueType::Secret)) {
                return false;
            }
        }
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

// --- templates -------------------------------------------------------------

namespace {

// Walks a JSON Pointer (RFC 6901) to the node it names. The empty pointer names the
// body itself, which is never a string and so never a generator target.
RecordNode* nodeAtPointer(RecordNode& root, const QString& pointer) {
    if (pointer.isEmpty()) {
        return &root;
    }
    // Relative, like the secret list's paths ("Password", "a/b"). A leading slash is
    // tolerated so a file written by another JSON-Pointer-aware tool still resolves.
    QString path = pointer;
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    const QStringList tokens = path.split(QLatin1Char('/'));
    RecordNode* node = &root;
    for (int index = 0; index < tokens.size(); ++index) {
        const QString token = pointerUnescape(tokens.at(index));
        if (node->type == ValueType::Object) {
            RecordNode* found = nullptr;
            for (auto& member : node->members) {
                if (member.first == token) {
                    found = &member.second;
                    break;
                }
            }
            if (found == nullptr) {
                return nullptr;
            }
            node = found;
        } else if (node->type == ValueType::Array) {
            bool ok = false;
            const int position = token.toInt(&ok);
            if (!ok || position < 0 || position >= node->items.size()) {
                return nullptr;
            }
            node = &node->items[position];
        } else {
            return nullptr;
        }
    }
    return node;
}

// Sets a leaf's text, keeping its type. Only strings and secrets are generator targets.
bool setLeafText(RecordNode* node, const QString& text) {
    if (node == nullptr) {
        return false;
    }
    if (node->type != ValueType::String && node->type != ValueType::Secret) {
        return false;
    }
    node->text = text;
    return true;
}

// No 0/O/1/l/I: the value is read from a screen and typed onto another device.
constexpr auto kPasswordUpper = "ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr auto kPasswordLower = "abcdefghijkmnopqrstuvwxyz";
constexpr auto kPasswordDigits = "23456789";
constexpr auto kPasswordSymbols = "!@#$%^&*-_=+?";

QString randomPassword(const int length) {
    const QString classes[] = {QString::fromLatin1(kPasswordUpper), QString::fromLatin1(kPasswordLower),
        QString::fromLatin1(kPasswordDigits), QString::fromLatin1(kPasswordSymbols)};
    QString alphabet;
    for (const auto& group : classes) {
        alphabet += group;
    }
    QRandomGenerator* generator = QRandomGenerator::system();
    QString out;
    out.reserve(length);
    // One from each class first, so a generated password is never only lower-case.
    for (const auto& group : classes) {
        if (out.size() < length) {
            out += group.at(static_cast<int>(generator->bounded(group.size())));
        }
    }
    while (out.size() < length) {
        out += alphabet.at(static_cast<int>(generator->bounded(alphabet.size())));
    }
    // Shuffle so the guaranteed characters are not always in the first four places.
    for (int index = out.size() - 1; index > 0; --index) {
        const int other = static_cast<int>(generator->bounded(index + 1));
        const QChar held = out.at(index);
        out[index] = out.at(other);
        out[other] = held;
    }
    return out;
}

QString randomUuid() {
    quint32 parts[4];
    QRandomGenerator* generator = QRandomGenerator::system();
    for (auto& part : parts) {
        part = generator->generate();
    }
    // Version 4, variant 1 (RFC 4122), then the canonical 8-4-4-4-12 spelling.
    parts[1] = (parts[1] & 0xFFFF0FFFu) | 0x00004000u;
    parts[2] = (parts[2] & 0x3FFFFFFFu) | 0x80000000u;
    return QStringLiteral("%1-%2-%3-%4-%5")
        .arg(parts[0], 8, 16, QLatin1Char('0'))
        .arg(parts[1] >> 16, 4, 16, QLatin1Char('0'))
        .arg(parts[1] & 0xFFFF, 4, 16, QLatin1Char('0'))
        .arg(parts[2] >> 16, 4, 16, QLatin1Char('0'))
        .arg(QStringLiteral("%1%2").arg(parts[2] & 0xFFFF, 4, 16, QLatin1Char('0'))
                 .arg(parts[3], 8, 16, QLatin1Char('0')));
}

} // namespace

QString generatorKindName(const GeneratorKind kind) {
    switch (kind) {
    case GeneratorKind::Password:
        return QStringLiteral("password");
    case GeneratorKind::Uuid:
        return QStringLiteral("uuid");
    case GeneratorKind::Date:
        return QStringLiteral("date");
    }
    return QString();
}

bool generatorKindFromName(const QString& name, GeneratorKind* kind) {
    if (kind == nullptr) {
        return false;
    }
    if (name == QLatin1String("password")) {
        *kind = GeneratorKind::Password;
        return true;
    }
    if (name == QLatin1String("uuid")) {
        *kind = GeneratorKind::Uuid;
        return true;
    }
    if (name == QLatin1String("date")) {
        *kind = GeneratorKind::Date;
        return true;
    }
    return false;
}

bool applyGenerators(RecordEntry* entry, QString* problem) {
    if (entry == nullptr) {
        return false;
    }
    for (const GeneratedValue& value : entry->generated) {
        RecordNode* target = nodeAtPointer(entry->body, value.pointer);
        QString text;
        switch (value.kind) {
        case GeneratorKind::Password:
            text = randomPassword(value.length);
            break;
        case GeneratorKind::Uuid:
            text = randomUuid();
            break;
        case GeneratorKind::Date:
            text = QDate::currentDate().toString(Qt::ISODate);
            break;
        }
        if (!setLeafText(target, text)) {
            if (problem != nullptr) {
                *problem = value.pointer;
            }
            return false;
        }
    }
    entry->generated.clear();
    return true;
}

RecordDocument cloneTemplate(const RecordEntry& templateEntry, const QString& title) {
    RecordEntry clone = templateEntry;
    if (!title.trimmed().isEmpty()) {
        clone.title = title.trimmed();
    }
    // A generator that cannot be applied leaves the sample in place rather than an empty
    // field, and parseRecord already refuses such a template, so this cannot hide a fault.
    applyGenerators(&clone);
    RecordDocument document;
    document.isTemplate = false;
    document.entries.push_back(clone);
    return document;
}

QString templateFolderName() {
    return QStringLiteral(".templates");
}

bool isTemplateFolderName(const QString& name) {
    return name == templateFolderName();
}

bool insertTemplate(RecordNode* target, const RecordEntry& templateEntry, const QString& key,
    QString* problem) {
    const auto fail = [problem](const QString& text) {
        if (problem != nullptr) {
            *problem = text;
        }
        return false;
    };
    if (target == nullptr || target->type != ValueType::Object) {
        return fail(QStringLiteral("Templates are inserted into an object."));
    }
    const QString name = key.trimmed();
    if (!isValidKey(name)) {
        return fail(QStringLiteral("A field needs a name."));
    }
    for (const auto& member : target->members) {
        if (member.first == name) {
            return fail(QStringLiteral("\"%1\" is already a field here.").arg(name));
        }
    }
    RecordEntry copy = templateEntry;
    if (!applyGenerators(&copy, problem)) {
        return false;
    }
    target->members.append({name, copy.body});
    sortMembers(target);
    return true;
}

QVector<RecordDocument> builtinTemplates() {
    QVector<RecordDocument> templates;
    const QStringList files = QDir(QStringLiteral(":/templates"))
                                  .entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& file : files) {
        QFile resource(QStringLiteral(":/templates/") + file);
        if (!resource.open(QIODevice::ReadOnly)) {
            continue;
        }
        RecordDocument document;
        if (parseRecord(resource.readAll(), &document) && document.isTemplate) {
            templates.push_back(document);
        }
    }
    return templates;
}

} // namespace videovault::app
