// Progressive (streamed) import + provider provenance.
//
// These tests drive the real core: a vault on disk, the incremental package
// writer, the AES-GCM chunk format and the SQLCipher schema. Nothing is stubbed.
// They prove the two properties the streaming feature exists for:
//
//   1. bytes are sealed into the vault as they arrive (no plaintext staging
//      file, no whole-track buffer), and
//   2. a package only becomes a library item when every declared byte arrived,
//      so an interrupted or cancelled stream leaves nothing playable behind.

#include "videovault/core/vault.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace core = videovault::core;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVaultProtect_Phase9_" + suffix);
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

core::Argon2Parameters testParameters() {
    core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

std::vector<unsigned char> deterministicBytes(const std::size_t size) {
    std::vector<unsigned char> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>((i * 131U + 17U) & 0xffU);
    }
    return bytes;
}

core::TrackProvenance sampleProvenance(
    const std::string& provider_track_id = "track-4815162342") {
    core::TrackProvenance provenance;
    provenance.provider_id = "org.megavault.monochrome";
    provenance.provider_track_id = provider_track_id;
    provenance.source_url = "https://tracks.monochrome.st/track/" + provider_track_id;
    provenance.title = "Night Drive";
    provenance.artist = "The Signals";
    provenance.album = "Late";
    provenance.duration_ms = 214000U;
    provenance.codec = "flac";
    provenance.quality = "lossless";
    return provenance;
}

// Nothing may be left in vault-data/.tmp: a streamed import that does not
// commit must not leave a staging package (playable or not) on disk.
bool stagingIsEmpty(const std::filesystem::path& root) {
    const auto staging = root / L"vault-data" / L".tmp";
    std::error_code error;
    if (!std::filesystem::exists(staging, error)) {
        return true;
    }
    auto iterator = std::filesystem::directory_iterator(staging, error);
    if (error) {
        return false;
    }
    return iterator == std::filesystem::directory_iterator();
}

// Number of packages published into the content-addressed tree. The staging
// directory (vault-data/.tmp) is deliberately excluded: a package there is an
// import in progress, not a library item.
std::size_t countPublishedPackages(const std::filesystem::path& root) {
    const auto data = root / L"vault-data";
    std::error_code error;
    std::size_t count = 0;
    if (!std::filesystem::exists(data, error)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(data, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != L".vvp") {
            continue;
        }
        if (entry.path().parent_path().filename() == L".tmp") {
            continue;
        }
        ++count;
    }
    return count;
}

bool sameBytes(
    const std::vector<unsigned char>& left,
    const std::vector<unsigned char>& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

// Appends in irregular pieces, the way a network stream arrives.
bool appendInPieces(
    core::StreamImport& import, const std::vector<unsigned char>& bytes,
    const std::vector<std::size_t>& piece_sizes) {
    std::size_t offset = 0;
    std::size_t index = 0;
    while (offset < bytes.size()) {
        const auto piece = std::min(piece_sizes[index % piece_sizes.size()],
            bytes.size() - offset);
        const auto span = std::span<const unsigned char>(bytes).subspan(offset, piece);
        auto stored = import.append(span);
        if (!stored) {
            return false;
        }
        offset += piece;
        ++index;
        if (import.written() != offset) {
            return false;
        }
    }
    return true;
}

// The main path: a streamed track becomes a real vault item, its package reads
// back byte for byte through the streaming reader, and its provider identity is
// resolvable after the fact.
int streamedImportRoundTrip(const std::vector<std::size_t>& piece_sizes,
                            const std::size_t size, const char* label) {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        std::cerr << "create vault failed for " << label << '\n';
        return EXIT_FAILURE;
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(size);
    const auto provenance = sampleProvenance();

    auto begun = vault.begin_stream_import("Night Drive.flac", bytes.size(), 0, &provenance);
    if (!begun) {
        std::cerr << "begin_stream_import failed for " << label << '\n';
        return EXIT_FAILURE;
    }
    auto import = std::move(begun.value());
    if (!import.active() || import.expected_size() != bytes.size() || import.written() != 0U) {
        std::cerr << "import handle started in the wrong state for " << label << '\n';
        return EXIT_FAILURE;
    }
    if (!appendInPieces(import, bytes, piece_sizes)) {
        std::cerr << "append failed for " << label << '\n';
        return EXIT_FAILURE;
    }
    if (import.written() != bytes.size()) {
        std::cerr << "written() did not track the stream for " << label << '\n';
        return EXIT_FAILURE;
    }
    auto committed = import.commit();
    if (!committed) {
        std::cerr << "commit failed for " << label << ": "
                  << committed.error().technical_detail << '\n';
        return EXIT_FAILURE;
    }
    const auto id = committed.value();
    if (import.already_present() || import.active()) {
        std::cerr << "committed import reports the wrong state for " << label << '\n';
        return EXIT_FAILURE;
    }

    auto stored = vault.read_video_bytes(id);
    if (!stored || !sameBytes(stored.value(), bytes)) {
        std::cerr << "stored plaintext differs from the stream for " << label << '\n';
        return EXIT_FAILURE;
    }
    auto size_result = vault.video_size(id);
    if (!size_result || size_result.value() != bytes.size()) {
        std::cerr << "plaintext size is wrong for " << label << '\n';
        return EXIT_FAILURE;
    }
    // Mid-file and tail range reads go through the same authenticated chunk
    // reader the player uses.
    const auto window = std::min<std::size_t>(4096U, bytes.size());
    if (window > 0U) {
        const auto offset = bytes.size() - window;
        auto tail = vault.read_video_range(id, offset, window);
        if (!tail || tail.value().size() != window
            || !std::equal(tail.value().begin(), tail.value().end(), bytes.begin() + offset)) {
            std::cerr << "range read differs from the stream for " << label << '\n';
            return EXIT_FAILURE;
        }
    }

    auto listed = vault.list_videos();
    if (!listed || listed.value().size() != 1U) {
        std::cerr << "library does not hold exactly one item for " << label << '\n';
        return EXIT_FAILURE;
    }
    if (listed.value()[0].display_name != "Night Drive.flac"
        || listed.value()[0].original_size != bytes.size()) {
        std::cerr << "library row does not describe the stream for " << label << '\n';
        return EXIT_FAILURE;
    }

    auto recorded = vault.video_provenance(id);
    if (!recorded || !recorded.value()
        || recorded.value()->provider_track_id != provenance.provider_track_id
        || recorded.value()->title != provenance.title
        || recorded.value()->duration_ms != provenance.duration_ms
        || recorded.value()->codec != provenance.codec) {
        std::cerr << "provenance was not stored with the item for " << label << '\n';
        return EXIT_FAILURE;
    }
    auto resolved = vault.find_video_by_provenance(
        provenance.provider_id, provenance.provider_track_id);
    if (!resolved || !resolved.value() || resolved.value().value() != id) {
        std::cerr << "provider identity does not resolve to the item for " << label << '\n';
        return EXIT_FAILURE;
    }
    auto all = vault.list_video_provenance();
    if (!all || all.value().size() != 1U || all.value()[0].video_id != id) {
        std::cerr << "provenance listing is wrong for " << label << '\n';
        return EXIT_FAILURE;
    }
    if (!stagingIsEmpty(root) || countPublishedPackages(root) != 1U) {
        std::cerr << "unexpected package layout after commit for " << label << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

// A second save of the same provider track resolves to the existing item rather
// than creating a duplicate or a second package.
int duplicateTrackResolvesToExistingItem() {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        return fail("duplicate: create vault failed");
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(200U * 1024U);
    const auto provenance = sampleProvenance();

    auto first_begun = vault.begin_stream_import("Song.flac", bytes.size(), 0, &provenance);
    if (!first_begun) {
        return fail("duplicate: first begin failed");
    }
    auto first = std::move(first_begun.value());
    if (!first.append(bytes)) {
        return fail("duplicate: first append failed");
    }
    auto first_commit = first.commit();
    if (!first_commit) {
        return fail("duplicate: first import failed");
    }
    const auto first_id = first_commit.value();

    auto second_begun = vault.begin_stream_import("Song.flac", bytes.size(), 0, &provenance);
    if (!second_begun) {
        return fail("duplicate: second begin failed");
    }
    auto second = std::move(second_begun.value());
    if (!second.append(bytes)) {
        return fail("duplicate: second append failed");
    }
    auto committed = second.commit();
    if (!committed || !second.already_present() || committed.value() != first_id) {
        return fail("duplicate: second save did not resolve to the first item");
    }
    auto listed = vault.list_videos();
    if (!listed || listed.value().size() != 1U) {
        return fail("duplicate: a second item was created");
    }
    if (countPublishedPackages(root) != 1U || !stagingIsEmpty(root)) {
        return fail("duplicate: the discarded package was left behind");
    }
    return EXIT_SUCCESS;
}

// A stream that ends early must not publish anything: the declared size is part
// of the authenticated header, so the item can never be shorter than promised.
int shortStreamPublishesNothing() {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        return fail("short: create vault failed");
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(300U * 1024U);
    auto begun = vault.begin_stream_import("Partial.flac", bytes.size());
    if (!begun) {
        return fail("short: begin failed");
    }
    auto import = std::move(begun.value());
    if (!import.append(std::span<const unsigned char>(bytes).first(120U * 1024U))) {
        return fail("short: append failed");
    }
    auto committed = import.commit();
    if (committed) {
        return fail("short: an incomplete stream was published");
    }
    if (committed.error().code != core::VaultErrorCode::InvalidArgument) {
        return fail("short: unexpected error code");
    }
    auto listed = vault.list_videos();
    if (!listed || !listed.value().empty()) {
        return fail("short: an item was recorded");
    }
    if (countPublishedPackages(root) != 0U || !stagingIsEmpty(root)) {
        return fail("short: package files were left behind");
    }
    return EXIT_SUCCESS;
}

// Appending past the declared size is refused, and the handle stops being
// usable afterwards.
int oversizedAppendIsRejected() {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        return fail("oversize: create vault failed");
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(64U * 1024U);
    auto begun = vault.begin_stream_import("TooLong.flac", bytes.size() - 10U);
    if (!begun) {
        return fail("oversize: begin failed");
    }
    auto import = std::move(begun.value());
    auto stored = import.append(bytes);
    if (stored) {
        return fail("oversize: append past the declared size was accepted");
    }
    if (import.active()) {
        return fail("oversize: a rejected append left the import active");
    }
    auto committed = import.commit();
    if (committed) {
        return fail("oversize: a failed import still committed");
    }
    if (countPublishedPackages(root) != 0U || !stagingIsEmpty(root)) {
        return fail("oversize: package files were left behind");
    }
    return EXIT_SUCCESS;
}

// Dropping the handle (cancelled save, closed player) discards the staged
// ciphertext instead of leaving an orphan.
int destroyedHandleDiscardsStaging() {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        return fail("destroy: create vault failed");
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(180U * 1024U);
    {
        auto begun = vault.begin_stream_import("Cancelled.flac", bytes.size());
        if (!begun) {
            return fail("destroy: begin failed");
        }
        auto import = std::move(begun.value());
        if (!import.append(std::span<const unsigned char>(bytes).first(90U * 1024U))) {
            return fail("destroy: append failed");
        }
        if (countPublishedPackages(root) != 0U) {
            return fail("destroy: a package was published before commit");
        }
    }
    auto listed = vault.list_videos();
    if (!listed || !listed.value().empty()) {
        return fail("destroy: a cancelled import recorded an item");
    }
    if (countPublishedPackages(root) != 0U || !stagingIsEmpty(root)) {
        return fail("destroy: staging was not cleaned up");
    }
    return EXIT_SUCCESS;
}

// Locking the vault between begin and commit must abort the publication. The
// item is not recorded and nothing is left on disk.
int lockBeforeCommitDiscardsStaging() {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        return fail("lock: create vault failed");
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(150U * 1024U);
    auto begun = vault.begin_stream_import("Locked.flac", bytes.size());
    if (!begun) {
        return fail("lock: begin failed");
    }
    auto import = std::move(begun.value());
    if (!import.append(bytes)) {
        return fail("lock: append failed");
    }
    vault.lock();
    auto committed = import.commit();
    if (committed) {
        return fail("lock: commit succeeded after the vault was locked");
    }
    if (countPublishedPackages(root) != 0U || !stagingIsEmpty(root)) {
        return fail("lock: package files were left behind");
    }

    auto reopened = core::Vault::open(root, std::string_view("Correct-Horse-42"));
    if (!reopened) {
        return fail("lock: reopen failed");
    }
    auto listed = reopened.value().list_videos();
    if (!listed || !listed.value().empty()) {
        return fail("lock: the locked import recorded an item");
    }
    return EXIT_SUCCESS;
}

// Provenance is an identity, not a free-text field: two items cannot claim the
// same provider track, and a bad identity is refused before anything is stored.
int provenanceIdentityIsEnforced() {
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    auto created = core::Vault::create(root, std::string_view("Correct-Horse-42"),
        testParameters());
    if (!created) {
        return fail("identity: create vault failed");
    }
    auto vault = std::move(created.value());
    const auto bytes = deterministicBytes(4096U);
    auto first = vault.import_bytes(bytes, "first.flac");
    auto second = vault.import_bytes(bytes, "second.flac");
    if (!first || !second) {
        return fail("identity: import failed");
    }
    const auto provenance = sampleProvenance("shared-track");
    if (!vault.set_video_provenance(first.value(), provenance)) {
        return fail("identity: attaching provenance failed");
    }
    auto conflicting = vault.set_video_provenance(second.value(), provenance);
    if (conflicting) {
        return fail("identity: two items claim the same provider track");
    }
    auto malformed = sampleProvenance();
    malformed.provider_id.clear();
    if (vault.set_video_provenance(second.value(), malformed)) {
        return fail("identity: an empty provider id was accepted");
    }
    auto bad_hash = sampleProvenance("other-track");
    bad_hash.artwork_sha256 = "NOT-A-SHA256";
    if (vault.set_video_provenance(second.value(), bad_hash)) {
        return fail("identity: a malformed artwork hash was accepted");
    }
    auto resolved = vault.find_video_by_provenance(
        provenance.provider_id, provenance.provider_track_id);
    if (!resolved || !resolved.value() || resolved.value().value() != first.value()) {
        return fail("identity: the tracked item does not resolve");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    // A tail-only stream, an exact multiple of the 64 KiB chunk size (the case
    // where the last chunk is full and still carries the final-chunk flag), and
    // a stream that spans many chunks in odd-sized appends.
    if (streamedImportRoundTrip({1000U}, 1000U, "single-tail-chunk") != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (streamedImportRoundTrip({3U * 64U * 1024U}, 3U * 64U * 1024U,
            "exact-chunk-multiple") != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (streamedImportRoundTrip({7777U, 65536U, 1U, 4096U}, 900U * 1024U + 12345U,
            "many-chunks") != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (duplicateTrackResolvesToExistingItem() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (shortStreamPublishesNothing() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (oversizedAppendIsRejected() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (destroyedHandleDiscardsStaging() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (lockBeforeCommitDiscardsStaging() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (provenanceIdentityIsEnforced() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    std::cout << "Streamed import and provider provenance tests passed.\n";
    return EXIT_SUCCESS;
}
