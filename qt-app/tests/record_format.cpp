// Headless check of the record content format, version 3.
//
// A record is stored as ordinary encrypted bytes, so the JSON container, the
// routing marker and the secret paths are a persisted format: an escaping
// mistake would corrupt a stored value, and a marker that moved would make an
// item unopenable. These checks compile against the real record_document.cpp and
// cover the shapes a user actually builds: nested objects, arrays, secrets at
// depth, numbers, and the migration entry points.

#include "videovault/app/record_document.hpp"

#include <QByteArray>
#include <QString>

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>

namespace {

using namespace videovault::app;

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

RecordNode text(const QString& value) {
    return RecordNode::leaf(ValueType::String, value);
}

RecordNode secret(const QString& value) {
    return RecordNode::leaf(ValueType::Secret, value);
}

RecordNode number(const QString& value) {
    return RecordNode::leaf(ValueType::Number, value);
}

RecordNode object(std::initializer_list<QPair<QString, RecordNode>> members) {
    RecordNode node = RecordNode::object();
    for (const auto& member : members) {
        node.members.append(member);
    }
    return node;
}

RecordNode array(std::initializer_list<RecordNode> items) {
    RecordNode node = RecordNode::array();
    for (const auto& item : items) {
        node.items.append(item);
    }
    return node;
}

const RecordNode* findMember(const RecordNode& node, const QString& key) {
    for (const auto& member : node.members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

// Arrays are ordered and compared by index; object members are looked up by key,
// because the format keeps them sorted rather than in the order they were added.
bool sameNode(const RecordNode& left, const RecordNode& right) {
    if (left.type != right.type || left.text != right.text) {
        return false;
    }
    if (left.items.size() != right.items.size() || left.members.size() != right.members.size()) {
        return false;
    }
    for (int index = 0; index < left.items.size(); ++index) {
        if (!sameNode(left.items.at(index), right.items.at(index))) {
            return false;
        }
    }
    for (const auto& member : left.members) {
        const RecordNode* other = findMember(right, member.first);
        if (other == nullptr || !sameNode(member.second, *other)) {
            return false;
        }
    }
    return true;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main() {
    // --- the marker is always the first bytes ---------------------------------
    {
        RecordDocument document;
        const QByteArray bytes = serializeRecord(document);
        expect(bytes.startsWith("{\"mvp_record\":3"), "the marker must be byte 0 of version 3");
        expect(looksLikeRecord(bytes), "serialized content must look like a record");
        expect(recordFormatVersion(bytes) == 3, "serialized content must report version 3");

        RecordDocument parsed;
        expect(parseRecord(bytes, &parsed), "an empty file must parse");
        expect(parsed.entries.isEmpty(), "an empty file must have no records");
    }

    // --- the scenario from the feature request -------------------------------
    // "Web Services": example.com -> 1234, another.com -> {base, api} secrets.
    // "API keys": deepseek -> {endpoint, keys: [sk-1, sk-2, sk-3]}.
    RecordDocument scenario;
    {
        RecordEntry web;
        web.title = QStringLiteral("Web Services");
        web.body = object({
            {QStringLiteral("example.com"), text(QStringLiteral("1234"))},
            {QStringLiteral("another.com"),
                object({
                    {QStringLiteral("base.another.com"), secret(QStringLiteral("base-pw"))},
                    {QStringLiteral("api.another.com"), secret(QStringLiteral("api-pw"))},
                })},
        });
        scenario.entries.append(web);

        RecordEntry api;
        api.title = QStringLiteral("API keys");
        api.body = object({
            {QStringLiteral("deepseek"),
                object({
                    {QStringLiteral("endpoint"), text(QStringLiteral("example-endpoint/v1"))},
                    {QStringLiteral("keys"),
                        array({secret(QStringLiteral("sk-1")), secret(QStringLiteral("sk-2")),
                            secret(QStringLiteral("sk-3"))})},
                })},
        });
        scenario.entries.append(api);
    }

    const QByteArray scenarioBytes = serializeRecord(scenario);
    const std::string scenarioText = asStd(scenarioBytes);
    {
        // Keys are sorted in the file, so the screen and the bytes agree.
        expect(scenarioText.find("\"another.com\"") < scenarioText.find("\"example.com\""),
            "object members must be sorted by key (another.com before example.com)");
        expect(scenarioText.find("\"endpoint\"") < scenarioText.find("\"keys\""),
            "nested object members must be sorted too");
        expect(scenarioText.find("\"mvp_record\":3") == 1, "the version must be in the marker object");

        // Secrets are paths, and the body stays a plain JSON object.
        expect(contains(scenarioText, "\"secret\":["), "a record with secrets must list their paths");
        expect(contains(scenarioText, "another.com/api.another.com"),
            "a nested secret must be recorded by path");
        expect(contains(scenarioText, "deepseek/keys/0"),
            "an array element secret must be recorded by index");
        expect(!contains(scenarioText, "$secret"),
            "a secret must not be wrapped in the body: the body stays pure JSON");
        expect(!contains(scenarioText, "\"fields\""),
            "version 3 has no flat field list");

        RecordDocument back;
        expect(parseRecord(scenarioBytes, &back), "the scenario must parse");
        expect(back.entries.size() == 2, "both records must survive");
        expect(back.entries.at(0).title == QStringLiteral("Web Services")
                && back.entries.at(1).title == QStringLiteral("API keys"),
            "record order and titles must survive");
        expect(sameNode(back.entries.at(0).body, scenario.entries.at(0).body),
            "the first record's body must survive byte for byte");
        expect(sameNode(back.entries.at(1).body, scenario.entries.at(1).body),
            "the second record's body must survive byte for byte");
        expect(back.entries.at(0).body.members.size() == 2
                && back.entries.at(0).body.members.at(0).first == QStringLiteral("another.com")
                && back.entries.at(0).body.members.at(1).first == QStringLiteral("example.com"),
            "parsed members must come back sorted by key");

        const RecordNode* another = findMember(back.entries.at(0).body, QStringLiteral("another.com"));
        expect(another != nullptr && another->type == ValueType::Object, "another.com must be an object");
        const RecordNode* api = another == nullptr ? nullptr
                                                   : findMember(*another, QStringLiteral("api.another.com"));
        expect(api != nullptr && api->type == ValueType::Secret,
            "a nested string marked secret must come back as a secret");
        expect(api != nullptr && api->text == QStringLiteral("api-pw"),
            "a secret must keep its real value");

        const RecordNode* deepseek = findMember(back.entries.at(1).body, QStringLiteral("deepseek"));
        const RecordNode* keys = deepseek == nullptr ? nullptr : findMember(*deepseek, QStringLiteral("keys"));
        expect(keys != nullptr && keys->type == ValueType::Array && keys->items.size() == 3,
            "the keys array must keep three elements");
        expect(keys != nullptr && keys->items.at(1).type == ValueType::Secret
                && keys->items.at(1).text == QStringLiteral("sk-2"),
            "an array element marked secret must come back as a secret");
    }

    // --- numbers -------------------------------------------------------------
    {
        expect(isValidNumber(QStringLiteral("0")), "0 is a number");
        expect(isValidNumber(QStringLiteral("-0.5")), "-0.5 is a number");
        expect(isValidNumber(QStringLiteral("1e3")), "1e3 is valid JSON number syntax");
        expect(!isValidNumber(QStringLiteral("01")), "leading zeros are not JSON");
        expect(!isValidNumber(QStringLiteral("+1")), "a leading plus is not JSON");
        expect(!isValidNumber(QStringLiteral("1.")), "a trailing dot is not JSON");
        expect(!isValidNumber(QStringLiteral("1,2")), "a comma list is not a number");
        expect(!isValidNumber(QString()), "empty text is not a number");

        expect(canonicalNumber(QStringLiteral("1234")) == QStringLiteral("1234"),
            "a plain integer is already canonical");
        expect(canonicalNumber(QStringLiteral("1e3")) == QStringLiteral("1000"),
            "an exponent must be normalised to digits");
        expect(canonicalNumber(QStringLiteral("1.50")) == QStringLiteral("1.5"),
            "trailing zeros must be dropped");
        expect(canonicalNumber(QStringLiteral("12345678901234567890")).isEmpty(),
            "an integer beyond 2^53 must be refused rather than silently changed");
        expect(canonicalNumber(QStringLiteral("0.1")) == QStringLiteral("0.1"),
            "a short decimal must survive");

        RecordDocument doc;
        RecordEntry entry;
        entry.title = QStringLiteral("Numbers");
        entry.body = object({{QStringLiteral("price"), number(QStringLiteral("12.5"))},
            {QStringLiteral("count"), number(QStringLiteral("7"))}});
        doc.entries.append(entry);
        expect(recordProblem(doc).isEmpty(), "storable numbers must pass validation");
        RecordDocument back;
        expect(parseRecord(serializeRecord(doc), &back), "numbers must round-trip");
        const RecordNode* price = back.entries.isEmpty() ? nullptr
                                                         : findMember(back.entries.at(0).body, QStringLiteral("price"));
        expect(price != nullptr && price->type == ValueType::Number
                && price->text == QStringLiteral("12.5"),
            "a decimal number must come back exactly");

        RecordDocument broken;
        RecordEntry brokenEntry;
        brokenEntry.title = QStringLiteral("Bad");
        brokenEntry.body = object({{QStringLiteral("card"), number(QStringLiteral("12ab"))}});
        broken.entries.append(brokenEntry);
        const QString problem = recordProblem(broken);
        expect(!problem.isEmpty(), "an unstorable number must be reported");
        expect(problem.contains(QStringLiteral("card")), "the report must name the field");
    }

    // --- validation ----------------------------------------------------------
    {
        RecordDocument duplicate;
        RecordEntry entry;
        entry.title = QStringLiteral("Dup");
        entry.body = object(
            {{QStringLiteral("same"), text(QStringLiteral("a"))},
                {QStringLiteral("same"), text(QStringLiteral("b"))}});
        duplicate.entries.append(entry);
        expect(!recordProblem(duplicate).isEmpty(), "two fields with one name must be refused");

        RecordDocument unnamed;
        RecordEntry unnamedEntry;
        unnamedEntry.title = QStringLiteral("Unnamed");
        unnamedEntry.body = object({{QString(), text(QStringLiteral("x"))}});
        unnamed.entries.append(unnamedEntry);
        expect(!recordProblem(unnamed).isEmpty(), "an empty field name must be refused");

        RecordDocument fine;
        RecordEntry fineEntry;
        fineEntry.title = QStringLiteral("Fine");
        fineEntry.body = object({{QStringLiteral("a"), text(QStringLiteral("b"))}});
        fine.entries.append(fineEntry);
        expect(recordProblem(fine).isEmpty(), "a plain record must pass validation");
        expect(!isValidKey(QStringLiteral("   ")), "a whitespace-only key must be refused");
        expect(isValidKey(QStringLiteral(" spacing kept ")), "a key with text is a key");
    }

    // --- escaping, including keys that contain the path separators -----------
    {
        RecordDocument doc;
        RecordEntry entry;
        entry.title = QStringLiteral("quote\" back\\slash\nnewline");
        entry.body = object({{QStringLiteral("a/b~c"), secret(QStringLiteral("v\"1\\2"))}});
        doc.entries.append(entry);
        const QByteArray bytes = serializeRecord(doc);
        RecordDocument back;
        expect(parseRecord(bytes, &back), "escaped content must parse");
        expect(!back.entries.isEmpty() && back.entries.at(0).title == entry.title,
            "the title must survive escaping");
        const RecordNode* node = back.entries.isEmpty() ? nullptr
                                                        : findMember(back.entries.at(0).body,
                                                              QStringLiteral("a/b~c"));
        // A key containing '/' and '~' must still be found, and its secret flag
        // must survive the JSON Pointer escaping.
        expect(node != nullptr && node->text == QStringLiteral("v\"1\\2"),
            "a value with quotes and backslashes must survive");
        expect(node != nullptr && node->type == ValueType::Secret,
            "a secret under a key containing / and ~ must still be a secret");
    }

    // --- versions and rejected content --------------------------------------
    {
        expect(recordFormatVersion(QByteArray(
                   "{\"mvp_record\":2,\"title\":\"Old\",\"fields\":[]}")) == 2,
            "version 2 must be recognised as a record that needs migrating");
        expect(recordFormatVersion(QByteArray(
                   "{\"mvp_record\":1,\"title\":\"Old\",\"fields\":[]}")) == 1,
            "version 1 must be recognised as a record that needs migrating");
        expect(recordFormatVersion(QByteArray("{\"mvp_record\":99}")) == 0,
            "an unknown version must not be reported as readable");

        RecordDocument parsed;
        expect(!parseRecord(QByteArray("{\"mvp_record\":2,\"title\":\"Old\",\"fields\":[]}"), &parsed),
            "version 2 must be refused by the version 3 parser");
        expect(!parseRecord(QByteArray("%PDF-1.4\n"), &parsed), "a PDF must not parse as a record");
        expect(!parseRecord(QByteArray("{\"mvp_record\":3,"), &parsed), "malformed JSON must be refused");
        expect(!parseRecord(QByteArray("{\"mvp_record\":3,\"entries\":{}}"), &parsed),
            "entries must be an array");
        expect(!parseRecord(QByteArray(
                   "{\"mvp_record\":3,\"entries\":[{\"title\":\"t\",\"body\":{\"a\":true}}]}"),
                   &parsed),
            "a boolean has no type in this model, so the content must be refused");
        expect(!parseRecord(QByteArray(
                   "{\"mvp_record\":3,\"entries\":[{\"title\":\"t\",\"body\":{\"a\":null}}]}"),
                   &parsed),
            "null has no type in this model, so the content must be refused");
    }

    // --- migration entry point ----------------------------------------------
    {
        RecordNode node;
        expect(parseJsonValue(QStringLiteral("{\"a\":1}"), &node)
                && node.type == ValueType::Object && node.members.size() == 1,
            "parseJsonValue must read an object");
        expect(parseJsonValue(QStringLiteral("123"), &node)
                && node.type == ValueType::Number && node.text == QStringLiteral("123"),
            "parseJsonValue must read a number");
        expect(parseJsonValue(QStringLiteral("\"x\""), &node)
                && node.type == ValueType::String && node.text == QStringLiteral("x"),
            "parseJsonValue must read a quoted string");
        expect(parseJsonValue(QStringLiteral("[1,2]"), &node)
                && node.type == ValueType::Array && node.items.size() == 2,
            "parseJsonValue must read an array");
        expect(!parseJsonValue(QStringLiteral("sk-1"), &node),
            "bare text is not a JSON value");
        expect(!parseJsonValue(QStringLiteral("1,2"), &node), "a comma list is not one value");
    }

    // --- version 1/2 migration ----------------------------------------------
    {
        const QByteArray v1 = R"json({"mvp_record":1,"title":"Old","fields":[
            {"name":"user","value":"me"},
            {"name":"pw","value":"hunter2","secret":true}]})json";
        RecordDocument doc;
        QString note;
        expect(convertLegacyRecord(v1, QStringLiteral("Fallback"), &doc, &note),
            "version 1 content must convert");
        expect(doc.entries.size() == 1, "a flat field list must become one record");
        expect(!doc.entries.isEmpty() && doc.entries.at(0).title == QStringLiteral("Old"),
            "the old title must be kept");
        const RecordNode* pw = doc.entries.isEmpty() ? nullptr
                                                     : findMember(doc.entries.at(0).body, QStringLiteral("pw"));
        expect(pw != nullptr && pw->type == ValueType::Secret
                && pw->text == QStringLiteral("hunter2"),
            "a v1 secret flag must become a secret string");
        const RecordNode* user = doc.entries.isEmpty() ? nullptr
                                                       : findMember(doc.entries.at(0).body, QStringLiteral("user"));
        expect(user != nullptr && user->type == ValueType::String
                && user->text == QStringLiteral("me"),
            "a v1 plain field must become a string");
        expect(note.isEmpty(), "a clean migration must not warn");
        expect(doc.entries.isEmpty() || doc.entries.at(0).body.members.size() == 2,
            "both v1 fields must survive");

        // Version 2 could carry real JSON values behind the json flag.
        const QByteArray v2 = R"json({"mvp_record":2,"title":"Keys","fields":[
            {"name":"count","value":3,"json":true},
            {"name":"meta","value":{"a":"b"},"json":true},
            {"name":"note","value":"plain"},
            {"name":"dup","value":"one"},
            {"name":"dup","value":"two"}]})json";
        RecordDocument fromV2;
        expect(convertLegacyRecord(v2, QString(), &fromV2, &note),
            "version 2 content must convert");
        const RecordNode* count = fromV2.entries.isEmpty() ? nullptr
            : findMember(fromV2.entries.at(0).body, QStringLiteral("count"));
        expect(count != nullptr && count->type == ValueType::Number
                && count->text == QStringLiteral("3"),
            "a json-flagged number must become a number");
        const RecordNode* meta = fromV2.entries.isEmpty() ? nullptr
            : findMember(fromV2.entries.at(0).body, QStringLiteral("meta"));
        expect(meta != nullptr && meta->type == ValueType::Object
                && meta->members.size() == 1,
            "a json-flagged object must become an object");
        const RecordNode* second = fromV2.entries.isEmpty() ? nullptr
            : findMember(fromV2.entries.at(0).body, QStringLiteral("dup (2)"));
        expect(second != nullptr && second->text == QStringLiteral("two"),
            "a duplicate v1 name must be numbered instead of overwriting");

        // A field that claims JSON but no longer parses keeps its text, and the
        // caller is told which happened.
        const QByteArray broken = R"json({"mvp_record":2,"title":"Broken","fields":[
            {"name":"bad","value":"{oops","json":true}]})json";
        RecordDocument brokenDoc;
        expect(convertLegacyRecord(broken, QString(), &brokenDoc, &note),
            "unparsable JSON text must still convert");
        const RecordNode* bad = brokenDoc.entries.isEmpty() ? nullptr
            : findMember(brokenDoc.entries.at(0).body, QStringLiteral("bad"));
        expect(bad != nullptr && bad->type == ValueType::String
                && bad->text == QStringLiteral("{oops"),
            "unparsable JSON text must be kept as text");
        expect(!note.isEmpty(), "keeping JSON as text must be reported");

        // A missing title falls back to the item's name.
        const QByteArray untitled = R"json({"mvp_record":1,"title":"","fields":[]})json";
        RecordDocument untitledDoc;
        expect(convertLegacyRecord(untitled, QStringLiteral("From the gallery"), &untitledDoc, &note)
                && !untitledDoc.entries.isEmpty()
                && untitledDoc.entries.at(0).title == QStringLiteral("From the gallery"),
            "an empty title must fall back to the item name");

        // Only v1/v2 convert; anything else is refused.
        RecordDocument ignored;
        expect(!convertLegacyRecord(QByteArray("{\"mvp_record\":3,\"entries\":[]}"), QString(),
                   &ignored, &note),
            "version 3 content must not be migrated");
        expect(!convertLegacyRecord(QByteArray("%PDF-1.4\n"), QString(), &ignored, &note),
            "a non-record must not be migrated");
    }

    // --- small helpers -------------------------------------------------------
    {
        expect(valueTypeLabel(ValueType::String) == QStringLiteral("string")
                && valueTypeLabel(ValueType::Number) == QStringLiteral("number")
                && valueTypeLabel(ValueType::Secret) == QStringLiteral("secret")
                && valueTypeLabel(ValueType::Object) == QStringLiteral("object")
                && valueTypeLabel(ValueType::Array) == QStringLiteral("array"),
            "every type needs a label");

        const RecordNode plain = object({{QStringLiteral("a"), text(QStringLiteral("b"))}});
        expect(!nodeHasSecret(plain), "a plain object has no secret");
        const RecordNode withArraySecret = object({{QStringLiteral("list"),
            array({text(QStringLiteral("x")), secret(QStringLiteral("y"))})}});
        expect(nodeHasSecret(withArraySecret), "a secret inside an array must be found");
        expect(nodeHasSecret(object({{QStringLiteral("o"),
                   object({{QStringLiteral("s"), secret(QStringLiteral("v"))}})}})),
            "a secret nested two levels down must be found");
    }

    if (failures == 0) {
        std::cout << "record format checks succeeded\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " record format check(s) failed\n";
    return EXIT_FAILURE;
}
