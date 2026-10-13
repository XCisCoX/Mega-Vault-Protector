#include "videovault/app/record_document.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

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

QByteArray serializeRecord(const RecordDocument& record) {
    QString json = QStringLiteral("{\"mvp_record\":1,\"title\":\"");
    json += escapeJson(record.title);
    json += QStringLiteral("\",\"fields\":[");
    for (int index = 0; index < record.fields.size(); ++index) {
        if (index > 0) {
            json += QLatin1Char(',');
        }
        const RecordField& field = record.fields.at(index);
        json += QStringLiteral("{\"name\":\"");
        json += escapeJson(field.name);
        json += QStringLiteral("\",\"value\":\"");
        json += escapeJson(field.value);
        json += QStringLiteral("\",\"secret\":");
        json += field.secret ? QStringLiteral("true") : QStringLiteral("false");
        json += QStringLiteral("}");
    }
    json += QStringLiteral("]}");
    return json.toUtf8();
}

bool parseRecord(const QByteArray& bytes, RecordDocument* record) {
    if (record == nullptr || !looksLikeRecord(bytes)) {
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("mvp_record")).isDouble()) {
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
        field.value = object.value(QStringLiteral("value")).toString();
        field.secret = object.value(QStringLiteral("secret")).toBool(false);
        parsed.fields.push_back(field);
    }
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

} // namespace videovault::app
