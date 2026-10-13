// Headless check of the record content format (Phase 8).
//
// A record is stored as ordinary encrypted bytes, so the JSON container and the
// routing marker are a persisted format: an escaping mistake would corrupt a
// stored value, and a marker that moved would make an item unopenable. These
// checks compile against the real record_document.cpp.

#include "videovault/app/record_document.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(const bool ok, const char* what) {
    if (ok) {
        return;
    }
    ++failures;
    std::cerr << what << '\n';
}

std::string asStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

bool sameFields(const videovault::app::RecordDocument& left,
    const videovault::app::RecordDocument& right) {
    if (left.fields.size() != right.fields.size()) {
        return false;
    }
    for (int index = 0; index < left.fields.size(); ++index) {
        const auto& a = left.fields.at(index);
        const auto& b = right.fields.at(index);
        if (a.name != b.name || a.value != b.value || a.secret != b.secret) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    using namespace videovault::app;

    // The marker must be the very first bytes, always: routing reads a few bytes
    // of plaintext and must never depend on JSON key order.
    RecordDocument original;
    original.title = QStringLiteral("MyAI \"API\" Key");
    original.fields.push_back(RecordField{
        QStringLiteral("General key"), QStringLiteral("key123456789"), true});
    original.fields.push_back(RecordField{QStringLiteral("note"),
        QStringLiteral("line1\nline2\\slash\ttab \"quote\" ünïcode"), false});

    const QByteArray serialized = serializeRecord(original);
    expect(asStd(serialized).rfind("{\"mvp_record\":", 0) == 0,
        "serializeRecord must put the marker at byte 0.");
    expect(looksLikeRecord(serialized),
        "looksLikeRecord must accept serialized output.");

    RecordDocument parsed;
    expect(parseRecord(serialized, &parsed),
        "parseRecord must accept serialized output.");
    expect(parsed.title == original.title,
        "The title must round-trip with quotes intact.");
    expect(sameFields(parsed, original),
        "Every field must round-trip (newline, backslash, tab, unicode).");
    expect(parsed.fields.size() == 2 && parsed.fields.at(0).secret
            && !parsed.fields.at(1).secret,
        "The secret flag must round-trip per field.");

    // A value that would break the container if escaping were wrong.
    RecordDocument delimiters;
    delimiters.fields.push_back(RecordField{
        QStringLiteral("n"), QStringLiteral("]},{]}"), false});
    const QByteArray delimiter_bytes = serializeRecord(delimiters);
    RecordDocument delimiter_round_trip;
    expect(parseRecord(delimiter_bytes, &delimiter_round_trip)
            && delimiter_round_trip.fields.size() == 1
            && delimiter_round_trip.fields.at(0).value == QStringLiteral("]},{]}"),
        "A value containing JSON delimiters must not escape its string.");

    // Content that is not a record must never be routed to the editor.
    expect(!looksLikeRecord(QByteArray()), "Empty content is not a record.");
    expect(!looksLikeRecord(QByteArray("{\"mvp_reco")),
        "A truncated marker is not a record.");
    expect(!looksLikeRecord(QByteArray("%PDF-1.4\n%...\n")),
        "A PDF header is not a record.");
    expect(!looksLikeRecord(QByteArray("\x89PNG\r\n\x1a\n")),
        "A PNG header is not a record.");
    expect(!looksLikeRecord(QByteArray("\xFF\xD8\xFF\xE0", 4)),
        "A JPEG header is not a record.");
    expect(!looksLikeRecord(QByteArray("PK\x03\x04")),
        "A ZIP header is not a record.");
    expect(!looksLikeRecord(QByteArray("{\"other\":1}")),
        "Another JSON document is not a record.");
    expect(looksLikeRecord(QByteArray("\xEF\xBB\xBF{\"mvp_record\":1}")),
        "A UTF-8 BOM before the marker is tolerated.");
    expect(looksLikeRecord(QByteArray("  \n\t{\"mvp_record\":1}")),
        "Leading whitespace before the marker is tolerated.");
    expect(!parseRecord(QByteArray("{\"mvp_record\":1,"), &parsed),
        "parseRecord must reject malformed JSON.");
    expect(!parseRecord(QByteArray("%PDF-1.4"), &parsed),
        "parseRecord must reject content that is not a record.");

    // The inferred presentation. Inferred means it must follow the value.
    expect(classifyValue(QStringLiteral("https://example.com/a?b=c"), false)
            == ValueKind::Url,
        "A URL must classify as Url.");
    expect(classifyValue(QStringLiteral("www.example.com/path"), false)
            == ValueKind::Url,
        "A bare domain must classify as Url.");
    expect(classifyValue(QStringLiteral("erfan@example.com"), false)
            == ValueKind::Email,
        "An e-mail address must classify as Email.");
    expect(classifyValue(QStringLiteral("hunter2"), true) == ValueKind::Secret,
        "The secret flag must win over the value's shape.");
    expect(classifyValue(QStringLiteral("hunter2"), false) == ValueKind::Plain,
        "An unflagged short word must stay Plain.");
    expect(classifyValue(QStringLiteral("line one\nline two"), false)
            == ValueKind::Multiline,
        "Embedded newlines must classify as Multiline.");
    expect(classifyValue(QStringLiteral("+98 912 345 6789"), false)
            == ValueKind::Number,
        "An international phone number must classify as Number.");
    expect(classifyValue(QStringLiteral("12345678"), false) == ValueKind::Number,
        "A plain number must classify as Number.");
    expect(classifyValue(QString(200, QLatin1Char('x')), false) == ValueKind::Multiline,
        "A long string must classify as Multiline.");
    expect(classifyValue(QString(), false) == ValueKind::Plain,
        "Empty content must classify as Plain.");
    expect(valueKindLabel(ValueKind::Secret) == QStringLiteral("password")
            && valueKindLabel(ValueKind::Url) == QStringLiteral("URL")
            && valueKindLabel(ValueKind::Email) == QStringLiteral("email")
            && valueKindLabel(ValueKind::Multiline) == QStringLiteral("text")
            && valueKindLabel(ValueKind::Number) == QStringLiteral("number")
            && valueKindLabel(ValueKind::Plain).isEmpty(),
        "Badge labels must be the expected strings, empty for Plain.");

    if (failures != 0) {
        std::cerr << failures << " record format check(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Record format checks succeeded.\n";
    return EXIT_SUCCESS;
}
