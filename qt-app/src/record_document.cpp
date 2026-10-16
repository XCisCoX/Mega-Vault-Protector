#include "videovault/app/record_document.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

namespace videovault::app {
namespace {

QString escapeJson(const QString& text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '"':
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

// The members this build owns. Anything else in a stored document is kept
// verbatim, so a newer version's additions survive an edit here.
const QStringList& documentKeys() {
    static const QStringList keys{
        QStringLiteral("mvp_record"), QStringLiteral("title"), QStringLiteral("fields")};
    return keys;
}

const QStringList& fieldKeys() {
    static const QStringList keys{QStringLiteral("name"), QStringLiteral("value"),
        QStringLiteral("secret"), QStringLiteral("json")};
    return keys;
}

// A JSON value as compact text. Qt 5 has no document form for a bare scalar, so
// one is wrapped in an array and the brackets are dropped again.
QString compactJson(const QJsonValue& value) {
    if (value.isObject()) {
        return QString::fromUtf8(
            QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    QJsonArray wrapper;
    wrapper.append(value);
    const QString text =
        QString::fromUtf8(QJsonDocument(wrapper).toJson(QJsonDocument::Compact));
    return text.mid(1, text.size() - 2);
}

QJsonObject withoutKnownKeys(const QJsonObject& source, const QStringList& known) {
    QJsonObject out;
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (!known.contains(it.key())) {
            out.insert(it.key(), it.value());
        }
    }
    return out;
}

QString extrasJson(const QJsonObject& extras, const QStringList& known) {
    QString out;
    for (auto it = extras.begin(); it != extras.end(); ++it) {
        if (known.contains(it.key())) {
            continue;
        }
        out += QLatin1Char(',');
        out += QLatin1Char('"');
        out += escapeJson(it.key());
        out += QStringLiteral("\":");
        out += compactJson(it.value());
    }
    return out;
}

// Parses the container once and reports the version it carries.
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

const QRegularExpression& urlPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^(https?|ftp|sftp|ssh|git|mailto)://?\\S+$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

const QRegularExpression& bareHostPattern() {
    // A bare domain such as www.example.com/path, with no scheme.
    static const QRegularExpression pattern(
        QStringLiteral("^(www\\.)?[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}(/\\S*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

const QRegularExpression& emailPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9._%+\\-]+@[A-Za-z0-9.\\-]+\\.[A-Za-z]{2,}$"));
    return pattern;
}

const QRegularExpression& numberPattern() {
    // Numbers, phone numbers and PINs: digits with the usual separators, and an
    // optional leading + so an international number such as +98 912 345 6789 is
    // recognised too.
    static const QRegularExpression pattern(QStringLiteral("^\\+?[0-9][0-9 +()./\\-]{2,19}$"));
    return pattern;
}

constexpr int kLongValueLength = 80;

} // namespace

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

bool isValidJsonValue(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    // Checked inside a one-element array because Qt 5's parser only accepts an
    // object or an array at the top level, and requiring exactly one element
    // keeps a comma-separated list such as "1,2" from passing as JSON.
    const QByteArray wrapped = QByteArray("[") + trimmed.toUtf8() + QByteArray("]");
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(wrapped, &error);
    return error.error == QJsonParseError::NoError && document.isArray()
        && document.array().size() == 1;
}

QString recordProblem(const RecordDocument& record) {
    for (int index = 0; index < record.fields.size(); ++index) {
        const RecordField& field = record.fields.at(index);
        if (!field.json || isValidJsonValue(field.value)) {
            continue;
        }
        const QString label = field.name.trimmed().isEmpty()
            ? QStringLiteral("Field %1").arg(index + 1)
            : QStringLiteral("\"%1\"").arg(field.name.trimmed());
        return QStringLiteral("%1 is marked as JSON but is not valid JSON. "
                              "Fix it, or remove the field, before saving.")
            .arg(label);
    }
    return QString();
}

QByteArray serializeRecord(const RecordDocument& record) {
    QString json = QStringLiteral("{\"mvp_record\":");
    json += QString::number(kRecordFormatVersion);
    json += QStringLiteral(",\"title\":\"");
    json += escapeJson(record.title);
    json += QStringLiteral("\",\"fields\":[");
    for (int index = 0; index < record.fields.size(); ++index) {
        if (index > 0) {
            json += QLatin1Char(',');
        }
        const RecordField& field = record.fields.at(index);
        // The marker means the stored value IS a JSON value, so it is only
        // emitted when the text really parses. Text that does not is stored as a
        // plain string with the marker dropped: the text is kept exactly, and
        // the format never has to represent "marked as JSON but not JSON".
        // recordProblem is what tells the user before this happens.
        const bool as_json = field.json && isValidJsonValue(field.value);
        json += QStringLiteral("{\"name\":\"");
        json += escapeJson(field.name);
        json += QStringLiteral("\",\"value\":");
        if (as_json) {
            // Emitted raw: a number stays a number, an object stays an object.
            json += field.value.trimmed();
        } else {
            json += QLatin1Char('"');
            json += escapeJson(field.value);
            json += QLatin1Char('"');
        }
        json += QStringLiteral(",\"secret\":");
        json += field.secret ? QStringLiteral("true") : QStringLiteral("false");
        if (as_json) {
            json += QStringLiteral(",\"json\":true");
        }
        json += extrasJson(field.extras, fieldKeys());
        json += QLatin1Char('}');
    }
    json += QLatin1Char(']');
    json += extrasJson(record.extras, documentKeys());
    json += QLatin1Char('}');
    return json.toUtf8();
}

bool parseRecord(const QByteArray& bytes, RecordDocument* record) {
    if (record == nullptr) {
        return false;
    }
    QJsonObject root;
    if (!readRoot(bytes, &root, nullptr)) {
        return false;
    }

    RecordDocument parsed;
    parsed.title = root.value(QStringLiteral("title")).toString();
    const QJsonArray fields = root.value(QStringLiteral("fields")).toArray();
    parsed.fields.reserve(fields.size());
    for (const auto& entry : fields) {
        const QJsonObject object = entry.toObject();
        RecordField field;
        field.name = object.value(QStringLiteral("name")).toString();
        const QJsonValue value = object.value(QStringLiteral("value"));
        // A field is JSON when it says so, or when its value cannot be anything
        // but JSON (a number, a boolean, null, an object or an array).
        field.json = object.value(QStringLiteral("json")).toBool(false) || !value.isString();
        if (field.json) {
            field.value = value.isUndefined() ? QString() : compactJson(value);
        } else {
            field.value = value.toString();
        }
        field.secret = object.value(QStringLiteral("secret")).toBool(false);
        field.extras = withoutKnownKeys(object, fieldKeys());
        parsed.fields.push_back(field);
    }
    parsed.extras = withoutKnownKeys(root, documentKeys());
    *record = parsed;
    return true;
}

ValueKind classifyValue(const QString& value, const bool secret) {
    if (secret) {
        return ValueKind::Secret;
    }
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return ValueKind::Plain;
    }
    if (trimmed.contains(QLatin1Char('\n'))) {
        return ValueKind::Multiline;
    }
    if (urlPattern().match(trimmed).hasMatch()
        || bareHostPattern().match(trimmed).hasMatch()) {
        return ValueKind::Url;
    }
    if (emailPattern().match(trimmed).hasMatch()) {
        return ValueKind::Email;
    }
    if (numberPattern().match(trimmed).hasMatch()) {
        return ValueKind::Number;
    }
    if (trimmed.size() > kLongValueLength) {
        return ValueKind::Multiline;
    }
    return ValueKind::Plain;
}

QString valueKindLabel(const ValueKind kind) {
    switch (kind) {
    case ValueKind::Secret:
        return QStringLiteral("password");
    case ValueKind::Url:
        return QStringLiteral("URL");
    case ValueKind::Email:
        return QStringLiteral("email");
    case ValueKind::Multiline:
        return QStringLiteral("text");
    case ValueKind::Number:
        return QStringLiteral("number");
    case ValueKind::Plain:
        break;
    }
    return QString();
}

QString recordFieldKindLabel(const RecordField& field) {
    if (field.json) {
        return QStringLiteral("json");
    }
    return valueKindLabel(classifyValue(field.value, field.secret));
}

} // namespace videovault::app
