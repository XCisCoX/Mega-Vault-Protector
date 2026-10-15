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

    // ---- version detection ----
    {
        expect(recordFormatVersion(QByteArray("{\"mvp_record\":1,\"fields\":[]}")) == 1,
            "Version 1 content must report version 1.");
        expect(recordFormatVersion(QByteArray("{\"mvp_record\":2,\"fields\":[]}")) == 2,
            "Version 2 content must report version 2.");
        expect(recordFormatVersion(QByteArray("{\"mvp_record\":9,\"fields\":[]}")) == 0,
            "A version this build does not know must be refused, not guessed at.");
        expect(recordFormatVersion(QByteArray("{\"other\":1}")) == 0,
            "Another document is not a record.");
        expect(recordFormatVersion(QByteArray("%PDF-1.4")) == 0,
            "Content that is not a record has no version.");
    }

    // ---- version 1 content keeps working (migration, no data loss) ----
    {
        const QByteArray stored = QByteArray(
            "{\"mvp_record\":1,\"title\":\"Old record\",\"fields\":["
            "{\"name\":\"key\",\"value\":\"api-key-value\",\"secret\":true},"
            "{\"name\":\"note\",\"value\":\"plain text\"}]}");
        RecordDocument migrated;
        expect(parseRecord(stored, &migrated),
            "A version 1 record must still parse.");
        expect(migrated.title == QStringLiteral("Old record")
                && migrated.fields.size() == 2U,
            "Version 1 content must keep its title and fields.");
        expect(migrated.fields.at(0).secret && !migrated.fields.at(0).json
                && migrated.fields.at(0).value == QStringLiteral("api-key-value"),
            "Version 1 values must arrive as untouched text with their secret flag.");
        expect(!migrated.fields.at(1).secret, "Version 1 secret defaults to false.");

        const QByteArray rewritten = serializeRecord(migrated);
        expect(recordFormatVersion(rewritten) == 2,
            "Saving migrated content must write the current version.");
        RecordDocument again;
        expect(parseRecord(rewritten, &again) && again.title == migrated.title
                && sameFields(again, migrated),
            "Version 1 content must survive a rewrite with no data loss.");
    }

    // ---- JSON values of every common type ----
    {
        struct Case {
            const char* text;
            const char* expected;
            const char* what;
        };
        const Case cases[] = {
            {"42", "\"value\":42", "A number must stay a number."},
            {"-1.5", "\"value\":-1.5", "A negative decimal must stay a number."},
            {"\"text\"", "\"value\":\"text\"", "A JSON string must stay a string."},
            {"true", "\"value\":true", "True must stay a boolean."},
            {"false", "\"value\":false", "False must stay a boolean."},
            {"null", "\"value\":null", "Null must stay null."},
            {"{\"a\":1,\"b\":[1,2]}", "\"value\":{\"a\":1,\"b\":[1,2]}",
                "An object must stay an object."},
            {"[1,2,3]", "\"value\":[1,2,3]", "An array must stay an array."},
        };
        for (const auto& item : cases) {
            RecordDocument doc;
            doc.title = QStringLiteral("t");
            RecordField field;
            field.name = QStringLiteral("j");
            field.json = true;
            field.value = QString::fromUtf8(item.text);
            doc.fields.push_back(field);

            const QByteArray bytes = serializeRecord(doc);
            expect(asStd(bytes).find(item.expected) != std::string::npos, item.what);
            RecordDocument back;
            expect(parseRecord(bytes, &back) && back.fields.size() == 1U
                    && back.fields.at(0).json
                    && back.fields.at(0).value == QString::fromUtf8(item.text),
                "A JSON value must round-trip unchanged.");
        }

        // A text field whose content looks like JSON stays text.
        RecordDocument text_doc;
        text_doc.fields.push_back(
            RecordField{QStringLiteral("looks"), QStringLiteral("{\"a\":1}"), false});
        const QByteArray text_bytes = serializeRecord(text_doc);
        expect(asStd(text_bytes).find("\"value\":\"{\\\"a\\\":1}\"") != std::string::npos,
            "A text field must keep JSON-looking text as a string.");
        RecordDocument text_back;
        expect(parseRecord(text_bytes, &text_back) && !text_back.fields.at(0).json
                && text_back.fields.at(0).value == QStringLiteral("{\"a\":1}"),
            "JSON-looking text must come back as text, not as an object.");
    }

    // ---- what counts as one JSON value ----
    {
        const char* good[] = {"42", "-1.5", "0", "12345678901234567890", "\"hi\"",
            "true", "false", "null", "{}", "[]", "{\"a\":[1,{\"b\":null}]}",
            "  [1]  ", "\"[1,2]\""};
        for (const char* text : good) {
            expect(isValidJsonValue(QString::fromUtf8(text)),
                "A single JSON value must be accepted.");
        }
        const char* bad[] = {"", "   ", "abc", "{", "}", "[1,2", "1,2",
            "{\"a\":1}{}", "{\"a\":}", "'single'", "NaN", "\"unterminated",
            "{\"a\":1,}"};
        for (const char* text : bad) {
            expect(!isValidJsonValue(QString::fromUtf8(text)),
                "Text that is not one JSON value must be rejected.");
        }
    }

    // ---- a JSON field that does not parse is reported, not stored broken ----
    {
        RecordDocument doc;
        doc.title = QStringLiteral("t");
        RecordField field;
        field.name = QStringLiteral("Config");
        field.json = true;
        field.value = QStringLiteral("{oops");
        doc.fields.push_back(field);

        const QString problem = recordProblem(doc);
        expect(!problem.isEmpty() && problem.contains(QStringLiteral("Config")),
            "An invalid JSON field must be reported by name.");
        expect(recordProblem(original).isEmpty(),
            "A record whose fields are text has no problem.");
        expect(!recordProblem(doc).contains(QStringLiteral("hidden")),
            "The report must not invent detail it does not have.");

        // The container stays readable and the typed text is kept.
        const QByteArray bytes = serializeRecord(doc);
        expect(recordFormatVersion(bytes) == 2,
            "Content written from an unfinished JSON field is still a record.");
        RecordDocument back;
        expect(parseRecord(bytes, &back) && back.fields.size() == 1U
                && back.fields.at(0).value == QStringLiteral("{oops"),
            "A field whose JSON does not parse must be stored as text, not lost.");
        expect(!back.fields.at(0).json,
            "The json marker must be dropped when the text was stored as text.");

        RecordDocument good;
        good.fields.push_back(RecordField{QStringLiteral("a"), QStringLiteral("plain"), false});
        expect(recordProblem(good).isEmpty(), "A text-only record must pass validation.");
        RecordDocument good_json;
        RecordField json_field;
        json_field.name = QStringLiteral("j");
        json_field.json = true;
        json_field.value = QStringLiteral("{\"a\":1}");
        good_json.fields.push_back(json_field);
        expect(recordProblem(good_json).isEmpty(), "Valid JSON must pass validation.");
    }

    // ---- members this build does not know survive an edit ----
    {
        const QByteArray stored = QByteArray(
            "{\"mvp_record\":2,\"title\":\"t\",\"color\":\"red\",\"fields\":["
            "{\"name\":\"n\",\"value\":\"v\",\"secret\":false,\"weight\":7}]}");
        RecordDocument doc;
        expect(parseRecord(stored, &doc), "Content with unknown members must parse.");
        expect(doc.extras.value(QStringLiteral("color")).toString() == QStringLiteral("red")
                && doc.fields.size() == 1U
                && doc.fields.at(0).extras.value(QStringLiteral("weight")).toInt() == 7,
            "Unknown members must be kept on the document and on the field.");

        const QByteArray rewritten = serializeRecord(doc);
        expect(asStd(rewritten).find("\"color\":\"red\"") != std::string::npos
                && asStd(rewritten).find("\"weight\":7") != std::string::npos,
            "Unknown members must be written back out.");
        RecordDocument back;
        expect(parseRecord(rewritten, &back)
                && back.extras.value(QStringLiteral("color")).toString() == QStringLiteral("red")
                && back.fields.at(0).extras.value(QStringLiteral("weight")).toInt() == 7,
            "Unknown members must survive a full round trip.");
    }

    // ---- large values ----
    {
        RecordDocument doc;
        doc.title = QStringLiteral("big");
        RecordField prose;
        prose.name = QStringLiteral("prose");
        prose.value = QString(1024 * 1024, QLatin1Char('x'));
        doc.fields.push_back(prose);
        RecordField list;
        list.name = QStringLiteral("payload");
        list.json = true;
        list.value = QStringLiteral("{\"pad\":\"")
            + QString(1024 * 1024, QLatin1Char('y')) + QStringLiteral("\"}");
        doc.fields.push_back(list);

        const QByteArray bytes = serializeRecord(doc);
        RecordDocument back;
        expect(parseRecord(bytes, &back) && back.fields.size() == 2U,
            "A megabyte-sized record must round-trip.");
        expect(back.fields.at(0).value.size() == 1024 * 1024,
            "A megabyte of text must come back whole.");
        expect(back.fields.at(1).json
                && back.fields.at(1).value.contains(QString(1024 * 1024, QLatin1Char('y'))),
            "A megabyte of JSON must come back whole and still parse.");
        expect(recordProblem(back).isEmpty(),
            "A large but valid record must not be reported as a problem.");
    }

    // ---- the field badge ----
    {
        RecordField json_field;
        json_field.name = QStringLiteral("j");
        json_field.json = true;
        json_field.value = QStringLiteral("42");
        expect(recordFieldKindLabel(json_field) == QStringLiteral("json"),
            "A JSON field must be labelled json even when its text looks numeric.");

        RecordField secret_field;
        secret_field.value = QStringLiteral("hunter2");
        secret_field.secret = true;
        expect(recordFieldKindLabel(secret_field) == QStringLiteral("password"),
            "A secret text field must stay labelled password.");

        RecordField url_field;
        url_field.value = QStringLiteral("https://example.com");
        expect(recordFieldKindLabel(url_field) == QStringLiteral("URL"),
            "A text field must keep its inferred kind.");
    }

    if (failures != 0) {
        std::cerr << failures << " record format check(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Record format checks succeeded.\n";
    return EXIT_SUCCESS;
}
