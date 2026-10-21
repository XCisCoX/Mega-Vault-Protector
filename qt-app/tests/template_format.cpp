// Templates: the record container plus generators. Checks that the built-ins load, that
// using one produces an ordinary record, and that a template which cannot generate is
// refused instead of quietly keeping its sample value.
#include "videovault/app/record_document.hpp"

#include <QDate>
#include <QRegularExpression>
#include <QStringList>

#include <cstdio>

namespace app = videovault::app;

namespace {

int failures = 0;

// `detail` is optional context (the problem text, a size); it never changes the verdict.
void check(const bool ok, const char* what, const char* detail = nullptr) {
    if (detail != nullptr && *detail != '\0') {
        std::printf("%s %s  [%s]\n", ok ? "PASS" : "FAIL", what, detail);
    } else {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    }
    if (!ok) {
        ++failures;
    }
}

const app::RecordNode* member(const app::RecordNode& object, const QString& key) {
    for (const auto& entry : object.members) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const app::RecordEntry* findTemplate(const QVector<app::RecordDocument>& templates, const QString& title) {
    for (const auto& document : templates) {
        for (const auto& entry : document.entries) {
            if (entry.title == title) {
                return &entry;
            }
        }
    }
    return nullptr;
}

app::RecordDocument templateDocument(const app::RecordEntry& entry) {
    app::RecordDocument document;
    document.isTemplate = true;
    document.entries.push_back(entry);
    return document;
}

} // namespace

int main() {
    // --- the built-ins -----------------------------------------------------
    const auto builtins = app::builtinTemplates();
    check(builtins.size() == 3, "three built-in templates load from the resources");
    for (const auto& document : builtins) {
        check(document.isTemplate, "a built-in is marked as a template");
        check(document.entries.size() == 1, "a built-in holds one template");
    }

    const app::RecordEntry* website = findTemplate(builtins, QStringLiteral("Placeholder Website Name"));
    check(website != nullptr, "the website template is there");
    if (website != nullptr) {
        const app::RecordNode* password = member(website->body, QStringLiteral("Password"));
        const app::RecordNode* username = member(website->body, QStringLiteral("Username"));
        const app::RecordNode* url = member(website->body, QStringLiteral("URL"));
        check(password != nullptr && password->type == app::ValueType::Secret,
            "the password field is a secret");
        check(username != nullptr && username->text == QStringLiteral("Default Username"),
            "the username carries its default value");
        check(url != nullptr && url->text.isEmpty(), "the URL is empty by default");
        check(website->generated.size() == 1 && website->generated.first().kind == app::GeneratorKind::Password
                && website->generated.first().length == 20,
            "the password is generated at length 20");
        check(website->generated.first().pointer == QStringLiteral("Password"),
            "the generator names the Password path");

        // --- using it -------------------------------------------------------
        const auto clone = app::cloneTemplate(*website);
        check(!clone.isTemplate, "a clone is not a template any more");
        check(clone.entries.size() == 1, "a clone holds one record");
        const app::RecordEntry& record = clone.entries.first();
        check(record.generated.isEmpty(), "the clone carries no generators");
        check(record.title == QStringLiteral("Placeholder Website Name"),
            "the clone keeps the placeholder title when the user does not name it");
        const app::RecordNode* generated = member(record.body, QStringLiteral("Password"));
        check(generated != nullptr && generated->type == app::ValueType::Secret,
            "the generated value stays a secret");
        check(generated != nullptr && generated->text.size() == 20,
            "the generated password is 20 characters");
        check(generated != nullptr && !generated->text.isEmpty(),
            "the generated password replaced the empty sample");
        const app::RecordNode* keptUsername = member(record.body, QStringLiteral("Username"));
        check(keptUsername != nullptr && keptUsername->text == QStringLiteral("Default Username"),
            "a field without a generator keeps its default");
        const app::RecordNode* keptUrl = member(record.body, QStringLiteral("URL"));
        check(keptUrl != nullptr && keptUrl->text.isEmpty(), "an empty field stays empty");
        check(app::recordProblem(clone) == QString(), "the clone is a valid record on its own");

        const auto second = app::cloneTemplate(*website);
        check(second.entries.first().generated.isEmpty()
                && member(second.entries.first().body, QStringLiteral("Password"))->text
                    != generated->text,
            "two clones get different passwords");

        const auto named = app::cloneTemplate(*website, QStringLiteral("Bank"));
        check(named.entries.first().title == QStringLiteral("Bank"),
            "the caller's title replaces the placeholder");
        const auto blankName = app::cloneTemplate(*website, QStringLiteral("   "));
        check(blankName.entries.first().title == QStringLiteral("Placeholder Website Name"),
            "a blank title keeps the placeholder");
    }

    // --- a generated date ---------------------------------------------------
    const app::RecordEntry* apiKey = findTemplate(builtins, QStringLiteral("Placeholder Service"));
    check(apiKey != nullptr, "the API key template is there");
    if (apiKey != nullptr) {
        const auto clone = app::cloneTemplate(*apiKey);
        const app::RecordNode* key = member(clone.entries.first().body, QStringLiteral("API key"));
        check(key != nullptr && key->text.size() == 40, "the generated key honours its length");
        const app::RecordNode* expires = member(clone.entries.first().body, QStringLiteral("Expires"));
        check(expires != nullptr && expires->text == QDate::currentDate().toString(Qt::ISODate),
            "the date generator writes today as an ISO date");
    }

    // --- a generated uuid ---------------------------------------------------
    {
        app::RecordEntry entry;
        entry.title = QStringLiteral("Device");
        app::RecordNode body = app::RecordNode::object();
        body.members.push_back({QStringLiteral("Id"), app::RecordNode::leaf(app::ValueType::String, QString())});
        entry.body = body;
        app::GeneratedValue generator;
        generator.pointer = QStringLiteral("Id");
        generator.kind = app::GeneratorKind::Uuid;
        entry.generated.push_back(generator);

        QString problem;
        check(app::applyGenerators(&entry, &problem), "the uuid generator applies");
        const app::RecordNode* id = member(entry.body, QStringLiteral("Id"));
        const QRegularExpression shape(QStringLiteral(
            "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
        check(id != nullptr && shape.match(id->text).hasMatch(),
            "the uuid is version 4 in canonical spelling");
    }

    // --- the file format ----------------------------------------------------
    if (website != nullptr) {
        const QByteArray bytes = app::serializeRecord(templateDocument(*website));
        const QString text = QString::fromUtf8(bytes);
        check(text.startsWith(QStringLiteral("{\"mvp_record\":3")), "a template uses the record container");
        check(text.contains(QStringLiteral("\"template\":true")), "the template flag is written");
        check(text.contains(QStringLiteral("\"generate\":[")), "the generators are written");
        check(text.contains(QStringLiteral("\"kind\":\"password\"")), "the generator kind is written");
        check(text.contains(QStringLiteral("\"length\":20")), "the password length is written");

        app::RecordDocument reparsed;
        check(app::parseRecord(bytes, &reparsed), "a template round-trips through the parser");
        check(reparsed.isTemplate, "the flag survives the round trip");
        check(reparsed.entries.size() == 1 && reparsed.entries.first().generated.size() == 1,
            "the generator survives the round trip");
        check(app::serializeRecord(reparsed) == bytes, "re-serializing is byte-identical");

        // An ordinary record must never grow template keys.
        const auto ordinary = app::cloneTemplate(*website);
        const QString recordText = QString::fromUtf8(app::serializeRecord(ordinary));
        check(!recordText.contains(QStringLiteral("\"template\"")),
            "an ordinary record has no template flag");
        check(!recordText.contains(QStringLiteral("\"generate\"")),
            "an ordinary record has no generators");
    }

    // --- the template's own name -------------------------------------------
    if (website != nullptr) {
        app::RecordDocument named = templateDocument(*website);
        named.templateName = QStringLiteral("Website login");
        const QByteArray bytes = app::serializeRecord(named);
        check(QString::fromUtf8(bytes).contains(QStringLiteral("\"name\":\"Website login\"")),
            "the template's own name is written");
        app::RecordDocument reparsed;
        check(app::parseRecord(bytes, &reparsed)
                && reparsed.templateName == QStringLiteral("Website login"),
            "the template's own name survives a round trip");
        check(!QString::fromUtf8(app::serializeRecord(app::cloneTemplate(*website)))
                   .contains(QStringLiteral("\"name\"")),
            "an ordinary record carries no template name");
        for (const auto& document : builtins) {
            check(!document.templateName.isEmpty(), "every built-in names itself in the picker");
        }
    }

    // --- classifying an item from its first bytes --------------------------
    if (website != nullptr) {
        const QByteArray templateBytes = app::serializeRecord(templateDocument(*website));
        check(app::looksLikeTemplate(templateBytes.left(64)),
            "a template is recognised from 64 bytes, without decrypting the rest");
        check(app::looksLikeRecord(templateBytes.left(64)), "a template is also a record");
        const QByteArray recordBytes = app::serializeRecord(app::cloneTemplate(*website));
        check(!app::looksLikeTemplate(recordBytes.left(64)),
            "a record's first bytes are not mistaken for a template");
        check(app::looksLikeTemplate(QByteArray("\xEF\xBB\xBF  ") + templateBytes.left(64)),
            "a BOM and leading whitespace are tolerated");
        check(!app::looksLikeTemplate(QByteArray()), "empty content is not a template");
    }

    // --- a template as part of a record ------------------------------------
    if (website != nullptr) {
        app::RecordNode host = app::RecordNode::object();
        host.members.push_back({QStringLiteral("Name"),
            app::RecordNode::leaf(app::ValueType::String, QStringLiteral("my host"))});

        QString problem;
        // The default key is what it was in the template: its placeholder title.
        check(app::insertTemplate(&host, *website, website->title, &problem),
            "a template inserts into a record's object", qPrintable(problem));
        check(host.members.size() == 2, "the record has the new nested object beside its field");

        const app::RecordNode* inserted = member(host, QStringLiteral("Placeholder Website Name"));
        check(inserted != nullptr && inserted->type == app::ValueType::Object,
            "the inserted subtree is an object");
        if (inserted != nullptr) {
            const app::RecordNode* password = member(*inserted, QStringLiteral("Password"));
            check(password != nullptr && password->type == app::ValueType::Secret,
                "the inserted password is still a secret");
            check(password != nullptr && password->text.size() == 20,
                "the generator ran on the way in", password != nullptr
                    ? qPrintable(QString::number(password->text.size())) : "missing");
            check(password != nullptr && !password->text.isEmpty(),
                "the inserted value is generated, not the empty sample");
            const app::RecordNode* username = member(*inserted, QStringLiteral("Username"));
            check(username != nullptr && username->text == QStringLiteral("Default Username"),
                "defaults come along with the structure");
        }

        // The whole document must still be an ordinary record, with the nested secret path.
        app::RecordDocument hostDocument;
        hostDocument.entries.push_back(app::RecordEntry{QStringLiteral("Host"), host, {}});
        const QByteArray saved = app::serializeRecord(hostDocument);
        check(!saved.contains("template"), "a record holding an inserted template is not a template");
        app::RecordDocument reloaded;
        check(app::parseRecord(saved, &reloaded), "the record reloads");
        const QString text = QString::fromUtf8(saved);
        check(text.contains(QStringLiteral(
                  "\"secret\":[\"Placeholder Website Name/Password\"]")),
            "the nested password is recorded as a secret at its new path");

        // A second insert of the same key is refused, and nothing changes.
        const int before = host.members.size();
        check(!app::insertTemplate(&host, *website, website->title, &problem),
            "an existing key is refused");
        check(host.members.size() == before, "the refused insert changed nothing");

        // Editing the key at insert time is allowed and lands under the new name.
        check(app::insertTemplate(&host, *website, QStringLiteral("backup login"), &problem),
            "an edited key is accepted", qPrintable(problem));
        check(member(host, QStringLiteral("backup login")) != nullptr,
            "the structure lands under the edited key");

        // Arrays are not a destination.
        app::RecordNode list = app::RecordNode::array();
        check(!app::insertTemplate(&list, *website, QStringLiteral("x"), &problem),
            "an array is not a destination");
        // And a blank key is refused before anything is added.
        check(!app::insertTemplate(&host, *website, QStringLiteral("   "), &problem),
            "a blank key is refused");
    }

    // --- broken templates are refused --------------------------------------
    {
        const QByteArray badKind = R"({"mvp_record":3,"template":true,"entries":[{"title":"x",)"
            R"("body":{"P":""},"generate":[{"path":"P","kind":"magic"}]}]})";
        app::RecordDocument document;
        check(!app::parseRecord(badKind, &document), "an unknown generator kind is refused");

        const QByteArray missingPath = R"({"mvp_record":3,"template":true,"entries":[{"title":"x",)"
            R"("body":{"P":""},"generate":[{"path":"Q","kind":"password","length":12}]}]})";
        check(!app::parseRecord(missingPath, &document), "a generator path that is not there is refused");

        const QByteArray objectPath = R"({"mvp_record":3,"template":true,"entries":[{"title":"x",)"
            R"("body":{"Nested":{"P":""}},"generate":[{"path":"Nested","kind":"password","length":12}]}]})";
        check(!app::parseRecord(objectPath, &document), "a generator aimed at an object is refused");

        const QByteArray shortPassword = R"({"mvp_record":3,"template":true,"entries":[{"title":"x",)"
            R"("body":{"P":""},"generate":[{"path":"P","kind":"password","length":4}]}]})";
        check(!app::parseRecord(shortPassword, &document), "a password shorter than 8 is refused");

        const QByteArray badFlag = R"({"mvp_record":3,"template":"yes","entries":[]})";
        check(!app::parseRecord(badFlag, &document), "a non-boolean template flag is refused");
    }

    // --- generator names ----------------------------------------------------
    {
        app::GeneratorKind kind = app::GeneratorKind::Date;
        check(app::generatorKindFromName(QStringLiteral("password"), &kind)
                && kind == app::GeneratorKind::Password,
            "the password name maps to the password kind");
        check(!app::generatorKindFromName(QStringLiteral("nope"), &kind),
            "an unknown name maps to nothing");
        check(app::generatorKindName(app::GeneratorKind::Uuid) == QStringLiteral("uuid"),
            "the uuid kind is named uuid");
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
