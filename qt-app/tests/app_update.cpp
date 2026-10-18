// Deterministic updater tests: no network, downloads, staging or installation.
#include "videovault/app/app_update.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
int checks = 0;
void expect(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
QJsonObject releaseJson(const QString& tag, bool prerelease = false, bool draft = false) {
    const QString name = videovault::app::appUpdateAssetName();
    return QJsonObject{
        {"tag_name", tag}, {"prerelease", prerelease}, {"draft", draft},
        {"assets", QJsonArray{QJsonObject{{"name", name},
            {"browser_download_url", "https://github.com/XCisCoX/Mega-Vault-Protector/releases/download/" + tag + "/" + name},
            {"size", 1234}}}}};
}
QByteArray json(const QJsonArray& values) { return QJsonDocument(values).toJson(); }
QByteArray json(const QJsonObject& value) { return QJsonDocument(value).toJson(); }
QJsonObject withAssetField(QJsonObject release, const QString& key, const QJsonValue& value) {
    auto assets = release["assets"].toArray();
    auto asset = assets[0].toObject();
    asset[key] = value;
    assets[0] = asset;
    release["assets"] = assets;
    return release;
}
void expectNone(const QByteArray& body, const QString& current, bool beta, const char* message) {
    videovault::app::AppRelease result{"stale", QUrl("https://stale.invalid"), 99};
    QString error = "stale";
    expect(!videovault::app::selectReleaseFromJson(body, current, beta, &result, &error)
        && result.version.isEmpty() && result.url.isEmpty() && result.size == 0 && error.isEmpty(), message);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace videovault::app;
    expect(isNewerVersion("2.0.4-beta.10", "2.0.4-beta.9"), "beta counters compare numerically");
    expect(isNewerVersion("2.0.4", "2.0.4-beta.100"), "stable beats every beta counter");
    expect(!isNewerVersion("2.0.4-beta.100", "2.0.4"), "beta never upgrades same-base stable");
    AppRelease selected;
    QString error;
    expect(selectReleaseFromJson(json(releaseJson("v2.0.4")), "2.0.3", false, &selected, &error)
        && selected.version == "2.0.4" && selected.size == 1234 && error.isEmpty(),
        "latest endpoint object selects a newer stable package");
    const QJsonArray feed{releaseJson("v2.0.4"), releaseJson("v2.1.0-beta.2", true),
        releaseJson("v2.1.0-beta.10", true), releaseJson("v9.0.0", false, true)};
    expect(selectReleaseFromJson(json(feed), "2.0.3", true, &selected, &error)
        && selected.version == "2.1.0-beta.10", "beta feed selects highest numeric eligible tag, not first or draft");
    auto unsafe = releaseJson("v2.2.0");
    auto assets = unsafe["assets"].toArray();
    auto asset = assets[0].toObject();
    asset["browser_download_url"] = "https://attacker.invalid/update.zip";
    assets[0] = asset;
    unsafe["assets"] = assets;
    expect(!selectReleaseFromJson(json(unsafe), "2.0.3", true, &selected, &error)
        && selected.version.isEmpty() && !error.isEmpty(), "untrusted asset URLs are rejected");
    for (const auto& body : {QByteArray("{"), QByteArray("null"), QByteArray("{}"),
            QByteArray("[3]"), QByteArray("{\"message\":\"API rate limit exceeded\"}")}) {
        selected = {"stale", QUrl("https://stale.invalid"), 99};
        error = "stale";
        expect(!selectReleaseFromJson(body, "2.0.3", true, &selected, &error)
            && !error.isEmpty() && selected.version.isEmpty() && selected.url.isEmpty() && selected.size == 0,
            "malformed payload/API error is not reported as up to date and clears stale output");
    }
    auto wrongAssets = releaseJson("v2.2.0");
    wrongAssets["assets"] = "not an array";
    expect(!selectReleaseFromJson(json(wrongAssets), "2.0.3", true, &selected, &error)
        && !error.isEmpty(), "invalid assets schema is an error");
    expect(!selectReleaseFromJson(json(feed), "not-a-version", true, &selected, &error)
        && !error.isEmpty(), "invalid installed version is an error, not up to date");
    auto unknownSize = releaseJson("v2.2.0");
    auto sizeAssets = unknownSize["assets"].toArray();
    auto sizeAsset = sizeAssets[0].toObject();
    sizeAsset["size"] = -5;
    sizeAssets[0] = sizeAsset;
    unknownSize["assets"] = sizeAssets;
    expect(selectReleaseFromJson(json(unknownSize), "2.0.3", true, &selected, &error)
        && selected.size == 0, "negative asset size is unknown, never a negative count");
    // Selection must not depend on API publication ordering (all 24 permutations).
    std::array<int, 4> order{0, 1, 2, 3};
    do {
        QJsonArray shuffled;
        for (int index : order) { shuffled.append(feed[index]); }
        expect(selectReleaseFromJson(json(shuffled), "2.0.3", true, &selected, &error)
            && selected.version == "2.1.0-beta.10" && error.isEmpty(), "permuted beta feed has the same winner");
        expect(selectReleaseFromJson(json(shuffled), "2.0.3", false, &selected, &error)
            && selected.version == "2.0.4" && error.isEmpty(), "permuted stable feed excludes prereleases and drafts");
    } while (std::next_permutation(order.begin(), order.end()));
    expect(selectReleaseFromJson(json(QJsonArray{releaseJson("v2.1.0-beta.100", true),
        releaseJson("v2.1.0")}), "2.0.3", true, &selected, &error)
        && selected.version == "2.1.0", "stable selection outranks same-base beta above 99");
    expect(selectReleaseFromJson(json(releaseJson("v2.1.0")), "2.1.0-beta.100", false, &selected, &error),
        "stable channel upgrades installed beta to same-base stable");
    expectNone(json(releaseJson("v2.1.0-beta.100", true)), "2.1.0", true, "beta cannot downgrade installed stable");
    expectNone(json(releaseJson("v2.1.0-beta.1", false)), "2.0.3", false, "beta suffix excludes a misflagged prerelease");
    expectNone(json(releaseJson("v2.1.0", true)), "2.0.3", false, "API prerelease flag excludes even a stable-looking tag");
    expectNone(json(releaseJson("v2.1.0", false, true)), "2.0.3", true, "draft-only feed is not an update");
    expectNone(json(QJsonArray{}), "2.0.3", true, "empty release feed is not an error");
    expectNone(json(releaseJson("v2.0.3")), "2.0.3", false, "same version clears stale output and error");
    expectNone(json(releaseJson("v2.0.2")), "2.0.3", true, "older version is not an error");
    expectNone(json(releaseJson("nightly")), "2.0.3", true, "unrecognized non-version tags are ignored");
    auto noPackage = releaseJson("v2.2.0-beta.1", true);
    noPackage["assets"] = QJsonArray{};
    expect(selectReleaseFromJson(json(QJsonArray{noPackage, releaseJson("v2.0.4")}), "2.0.3", true, &selected, &error)
        && selected.version == "2.0.4" && error.isEmpty(), "skip newer release without platform package and select usable stable");
    expect(!selectReleaseFromJson(json(noPackage), "2.0.3", true, &selected, &error)
        && !error.isEmpty() && selected.url.isEmpty(), "newer release with no package is distinct from up to date");
    expectNone(json(noPackage), "3.0.0", true, "older release without a package is still up to date");
    auto wrongPlatform = withAssetField(releaseJson("v2.2.0"), "name", "unrelated-source.zip");
    expect(!selectReleaseFromJson(json(wrongPlatform), "2.0.3", true, &selected, &error) && !error.isEmpty(),
        "arbitrary archive suffix never substitutes for a platform package");
    expect(selectReleaseFromJson(json(QJsonArray{wrongPlatform, releaseJson("v2.0.4")}), "2.0.3", true, &selected, &error)
        && selected.version == "2.0.4", "wrong-platform release does not hide an installable release");
    const QString base = "https://github.com/XCisCoX/Mega-Vault-Protector/releases/download/v2.2.0/";
    const QString package = appUpdateAssetName();
    const QStringList unsafeUrls{
        "http://github.com/XCisCoX/Mega-Vault-Protector/releases/download/v2.2.0/" + package,
        "https://github.com.evil.invalid/XCisCoX/Mega-Vault-Protector/releases/download/v2.2.0/" + package,
        "https://github.com/Other/Repo/releases/download/v2.2.0/" + package,
        "https://github.com/XCisCoX/Mega-Vault-Protector/archive/refs/tags/v2.2.0.zip",
        "https://user:password@github.com/XCisCoX/Mega-Vault-Protector/releases/download/v2.2.0/" + package,
        "https://github.com:444/XCisCoX/Mega-Vault-Protector/releases/download/v2.2.0/" + package,
        base + package + "#fragment", base + "../" + package,
        base + "%2e%2e/" + package, base + "%2e%2e%2f" + package,
        "/relative.zip", "file:///C:/update.zip", "", "https://release-assets.githubusercontent.com/unbound.zip"};
    for (const auto& url : unsafeUrls) {
        auto bad = withAssetField(releaseJson("v2.2.0"), "browser_download_url", url);
        expect(!selectReleaseFromJson(json(bad), "2.0.3", true, &selected, &error)
            && selected.url.isEmpty() && !error.isEmpty(), "unsafe URL fixture rejected before download");
    }
    auto explicitHttps = withAssetField(releaseJson("v2.2.0"), "browser_download_url",
        "https://github.com:443/XCisCoX/Mega-Vault-Protector/releases/download/v2.2.0/" + package);
    expect(selectReleaseFromJson(json(explicitHttps), "2.0.3", false, &selected, nullptr),
        "explicit HTTPS port and null optional error are supported");
    auto hugeSize = withAssetField(releaseJson("v2.2.0"), "size", 1e100);
    expect(selectReleaseFromJson(json(hugeSize), "2.0.3", false, &selected, &error) && selected.size == 0,
        "out-of-range JSON size never overflows a signed byte count");
    auto absentSize = withAssetField(releaseJson("v2.2.0"), "size", QJsonValue());
    expect(selectReleaseFromJson(json(absentSize), "2.0.3", false, &selected, &error) && selected.size == 0,
        "missing asset size remains unknown");
    expect(!selectReleaseFromJson(json(feed), "2.0.3", true, nullptr, &error) && !error.isEmpty(),
        "null output is reported instead of dereferenced");
    for (const QString& invalid : QStringList{"garbage", "2.0", "2.0.4-betax.1", "2.0.4-beta.-1", "2.0.4-beta.1junk",
            "2.0.4-beta.1\n", "2.0.4\n", "18446744073709551616.0.0", "2.0.4-beta.18446744073709551616"}) {
        const QString shown = QString(invalid).replace(QLatin1Char('\n'), QLatin1String("\\n"));
        expect(!isNewerVersion(invalid, "2.0.3"),
            qPrintable(QStringLiteral("malformed or overflowing latest version is never newer: %1")
                .arg(shown)));
    }
    expect(isNewerVersion("v2.10.0", "2.9.99"), "leading v and multi-digit components compare numerically");
    expect(!isNewerVersion("2.0.4-beta.9", "2.0.4-beta.10"), "beta numeric comparison is antisymmetric");
    expect(!isNewerVersion("2.0.4", "2.0.4"), "equal versions are not newer");
    auto missingA = releaseJson("v2.2.0");
    auto missingB = releaseJson("v2.3.0");
    missingA["assets"] = QJsonArray{};
    missingB["assets"] = QJsonArray{};
    expect(!selectReleaseFromJson(json(QJsonArray{missingA, missingB}), "2.0.3", true, &selected, &error),
        "multiple releases without packages do not select an update");
    const QString missingError = error;
    expect(!selectReleaseFromJson(json(QJsonArray{missingB, missingA}), "2.0.3", true, &selected, &error)
        && error == missingError && error.contains("2.3.0"), "missing-package diagnostic names highest release regardless of ordering");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
