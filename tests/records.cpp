// Phase 8: text records.
//
// A record is content the app authors in memory instead of importing from a
// file, stored as an ordinary encrypted .vvp package. These checks cover the
// gallery contract a record shares with imported media: folders, tags, search
// listing, byte-exact reads, in-place replacement that keeps the item id, and
// the removal path. The record's own JSON shape is the app's business, so the
// payloads here are opaque strings.
//
// Naming note: "record" here means a text item. The thumbnail mime
// "image/x-mvp-note" elsewhere in the code base is the music placeholder, an
// unrelated concept.

#include "videovault/core/vault.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVaultProtect_Phase8_records_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

int fail(const char* message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

videovault::core::Argon2Parameters testParameters() {
    videovault::core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

std::span<const unsigned char> asBytes(const std::string& text) {
    return std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

std::string asText(const std::vector<unsigned char>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

// The content shape the desktop app writes. Only its leading marker matters to
// routing; the core treats every byte as opaque.
std::string recordJson(const std::string& title, const std::string& value) {
    return "{\"mvp_record\":1,\"title\":\"" + title
        + "\",\"fields\":[{\"name\":\"General key\",\"value\":\"" + value
        + "\",\"secret\":true}]}";
}

std::size_t countPackages(const std::filesystem::path& root) {
    std::size_t count = 0;
    std::error_code error;
    const auto data = root / L"vault-data";
    if (!std::filesystem::is_directory(data, error)) {
        return 0;
    }
    std::filesystem::recursive_directory_iterator it(data, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
        if (it->is_regular_file(error) && it->path().extension() == L".vvp") {
            ++count;
        }
        it.increment(error);
    }
    return count;
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

const videovault::core::VideoInfo* findVideo(
    const std::vector<videovault::core::VideoInfo>& videos,
    const std::int64_t id) {
    for (const auto& video : videos) {
        if (video.id == id) {
            return &video;
        }
    }
    return nullptr;
}

bool hasTag(const videovault::core::VideoInfo& video, const std::string& name) {
    for (const auto& tag : video.tags) {
        if (tag == name) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using namespace videovault::core;
    TemporaryDirectory temp;

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "record test password", testParameters());
    if (!created) {
        return fail("Creating a vault for record tests must succeed.");
    }
    auto& vault = created.value();

    const std::string first_record = recordJson("MyAI API Key", "key123456789");
    const std::string second_record = recordJson("Home router", "hunter2");
    const std::string marker = "\"mvp_record\":1";

    // Creating a record from memory.
    const auto first = vault.import_bytes(asBytes(first_record), "MyAI API Key");
    if (!first || first.value() <= 0) {
        return fail("import_bytes must create a record and return a positive id.");
    }
    const auto second = vault.import_bytes(asBytes(second_record), "Home router");
    if (!second || second.value() <= 0 || second.value() == first.value()) {
        return fail("A second, different record must get its own id.");
    }

    // Byte-exact read back through the same reader playback uses.
    {
        const auto read = vault.read_video_bytes(first.value());
        if (!read) {
            return fail("Reading a record back must succeed.");
        }
        if (asText(read.value()) != first_record) {
            return fail("A record must read back byte-for-byte.");
        }
    }

    // Content deduplication matches import_file: identical bytes reuse the row.
    {
        const auto again = vault.import_bytes(asBytes(first_record), "Another title");
        if (!again || again.value() != first.value()) {
            return fail("Re-importing identical record bytes must return the existing id.");
        }
    }

    // The gallery lists records like any other item, with size and name.
    {
        const auto videos = vault.list_videos();
        if (!videos || videos.value().size() != 2U) {
            return fail("The gallery must list both records.");
        }
        const auto* listed = findVideo(videos.value(), first.value());
        if (listed == nullptr) {
            return fail("The created record must appear in the gallery.");
        }
        if (listed->display_name != "MyAI API Key") {
            return fail("The record must keep its display name.");
        }
        if (listed->original_size != first_record.size()) {
            return fail("The record's recorded plaintext size must match its bytes.");
        }
    }

    // Records live in folders and carry tags, because they are ordinary items.
    std::int64_t folder_id = 0;
    {
        const auto folder = vault.create_folder(0, "Credentials");
        if (!folder || folder.value() <= 0) {
            return fail("Creating a folder for records must succeed.");
        }
        folder_id = folder.value();
        const auto in_folder = vault.import_bytes(
            asBytes(recordJson("Bank", "1234-5678")), "Bank", folder_id);
        if (!in_folder) {
            return fail("Creating a record inside a folder must succeed.");
        }
        const auto videos = vault.list_videos();
        const auto* listed = findVideo(videos.value(), in_folder.value());
        if (listed == nullptr || listed->folder_id != folder_id) {
            return fail("A record created in a folder must report that folder.");
        }
        const auto tagged = vault.add_tag(in_folder.value(), "money");
        if (!tagged) {
            return fail("Tagging a record must succeed.");
        }
        const auto after_tag = vault.list_videos();
        const auto* re_listed = findVideo(after_tag.value(), in_folder.value());
        if (re_listed == nullptr || !hasTag(*re_listed, "money")) {
            return fail("A record's tag must appear in the gallery listing.");
        }
    }

    // In-place replacement: same id, new bytes, new package, old package gone,
    // folder and tags preserved.
    {
        const auto before_package = vault.package_path(first.value());
        if (!before_package) {
            return fail("The record's package must resolve before replacement.");
        }
        const auto tagged = vault.add_tag(first.value(), "work");
        if (!tagged) {
            return fail("Tagging before replacement must succeed.");
        }

        const std::string edited = recordJson("MyAI API Key", "key999999999");
        const auto replaced =
            vault.replace_video_content(first.value(), asBytes(edited), "MyAI API Key v2");
        if (!replaced || !replaced.value()) {
            return fail("Replacing a record's content must succeed.");
        }

        const auto read = vault.read_video_bytes(first.value());
        if (!read || asText(read.value()) != edited) {
            return fail("The edited record must read back byte-for-byte.");
        }

        const auto after_package = vault.package_path(first.value());
        if (!after_package) {
            return fail("The record's package must resolve after replacement.");
        }
        if (after_package.value() == before_package.value()) {
            return fail("A new file key must publish the edit at a new path.");
        }
        std::error_code ignored;
        if (std::filesystem::exists(before_package.value(), ignored)) {
            return fail("The replaced package file must be removed.");
        }
        if (countPackages(root) != 3U) {
            return fail("Replacement must not leave an orphan package behind.");
        }

        const auto videos = vault.list_videos();
        const auto* listed = findVideo(videos.value(), first.value());
        if (listed == nullptr) {
            return fail("The edited record must keep its id in the gallery.");
        }
        if (listed->display_name != "MyAI API Key v2") {
            return fail("Replacement must apply the new display name.");
        }
        if (listed->original_size != edited.size()) {
            return fail("Replacement must update the recorded plaintext size.");
        }
        if (!hasTag(*listed, "work")) {
            return fail("Replacement must preserve the record's tags.");
        }
    }

    // The vault database and the package both hold only ciphertext.
    {
        const auto database = readWholeFile(root / L"vault.db");
        if (database.empty()) {
            return fail("The vault database must exist for the encryption check.");
        }
        if (database.find(marker) != std::string::npos) {
            return fail("The record content must not appear in the vault database.");
        }
        const auto package = vault.package_path(first.value());
        if (!package) {
            return fail("The record package must resolve for the encryption check.");
        }
        if (readWholeFile(package.value()).find("key999999999") != std::string::npos) {
            return fail("The record content must not appear in plaintext in its package.");
        }
    }

    // Editing keeps deduplication honest: the replaced bytes are no longer the
    // vault's copy of the old content, so importing them again is new content.
    {
        const auto reimported = vault.import_bytes(asBytes(first_record), "MyAI API Key");
        if (!reimported || reimported.value() == first.value()) {
            return fail("Bytes replaced by an edit must import as a new item.");
        }
    }

    // Empty content is a valid item (the app always writes its marker, but the
    // core must not require content to be non-empty).
    {
        const auto empty = vault.import_bytes({}, "Empty record");
        if (!empty || empty.value() <= 0) {
            return fail("An empty record must be allowed.");
        }
        const auto read = vault.read_video_bytes(empty.value());
        if (!read || !read.value().empty()) {
            return fail("An empty record must read back as zero bytes.");
        }
    }

    // Names follow the folder/file rule.
    {
        if (vault.import_bytes(asBytes(first_record), "")) {
            return fail("A blank record name must be rejected.");
        }
        if (vault.import_bytes(asBytes(first_record), "   ")) {
            return fail("A whitespace-only record name must be rejected.");
        }
        if (vault.import_bytes(asBytes(first_record), "bad/name")) {
            return fail("A record name with a path separator must be rejected.");
        }
        if (vault.import_bytes(asBytes(first_record), std::string(121U, 'x'))) {
            return fail("A record name longer than 120 characters must be rejected.");
        }
        const auto longest = vault.import_bytes(
            asBytes(recordJson("long", "value")), std::string(120U, 'x'));
        if (!longest) {
            return fail("A 120-character record name must be accepted.");
        }
    }

    // Unknown targets fail cleanly instead of creating stray state.
    {
        if (vault.import_bytes(asBytes(first_record), "Ghost", 999999)) {
            return fail("Importing into an unknown folder must fail.");
        }
        if (vault.replace_video_content(999999, asBytes(first_record), "Ghost")) {
            return fail("Replacing an unknown item must fail.");
        }
        const auto missing = vault.replace_video_content(
            first.value(), asBytes(first_record), "");
        if (missing || missing.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Replacing with a blank name must be rejected.");
        }
        // The rejected replacement must not have touched the stored content.
        const auto read = vault.read_video_bytes(first.value());
        if (!read || asText(read.value()).find("key999999999") == std::string::npos) {
            return fail("A rejected replacement must leave the content untouched.");
        }
    }

    // Persistence across reopen.
    {
        vault.lock();
        auto reopened = Vault::open(root, "record test password");
        if (!reopened) {
            return fail("The vault must reopen with records intact.");
        }
        const auto read = reopened.value().read_video_bytes(first.value());
        if (!read || asText(read.value()).find("key999999999") == std::string::npos) {
            return fail("A record must survive a reopen.");
        }
        const auto videos = reopened.value().list_videos();
        const auto* listed = findVideo(videos.value(), first.value());
        if (listed == nullptr || listed->folder_id != 0) {
            return fail("The record's folder must survive a reopen.");
        }

        // A locked vault refuses record writes.
        reopened.value().lock();
        const auto locked_import = reopened.value().import_bytes(
            asBytes(recordJson("x", "y")), "Locked");
        if (locked_import
            || locked_import.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Creating a record on a locked vault must fail cleanly.");
        }
        const auto locked_replace = reopened.value().replace_video_content(
            first.value(), asBytes(recordJson("x", "y")), "Locked");
        if (locked_replace
            || locked_replace.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Replacing a record on a locked vault must fail cleanly.");
        }
    }

    // Removal deletes the record's package like any other item.
    {
        auto reopened = Vault::open(root, "record test password");
        if (!reopened) {
            return fail("The vault must reopen for the removal check.");
        }
        const auto package = reopened.value().package_path(first.value());
        if (!package) {
            return fail("The record package must resolve before removal.");
        }
        const auto removed = reopened.value().remove_video(first.value());
        if (!removed || !removed.value()) {
            return fail("Removing a record must succeed.");
        }
        std::error_code ignored;
        if (std::filesystem::exists(package.value(), ignored)) {
            return fail("Removing a record must delete its package.");
        }
    }

    std::cout << "Record checks succeeded.\n";
    return EXIT_SUCCESS;
}
