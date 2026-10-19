// Sync packages must be authenticated to the end before changing a vault.
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
        path_ = std::filesystem::temp_directory_path()
            / ("MegaVaultProtect_sync_packages_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::span<const unsigned char> bytes(const std::string& value) {
    return {reinterpret_cast<const unsigned char*>(value.data()), value.size()};
}
int fail(const char* message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}
} // namespace

int main() {
    using namespace videovault::core;
    TemporaryDirectory scratch;
    const Argon2Parameters parameters{8U * 1024U, 1U, 1U};
    auto sender = Vault::create(scratch.path() / "sender", "sender-local-password", parameters);
    auto receiver = Vault::create(scratch.path() / "receiver", "receiver-local-password", parameters);
    if (!sender || !receiver) return fail("Test vault creation failed.");
    const std::string first = "private-sync-file-one-contents";
    const std::string second = "private-sync-file-two-contents";
    auto one = sender.value().import_bytes(bytes(first), "private-first.txt");
    auto two = sender.value().import_bytes(bytes(second), "private-second.txt");
    if (!one || !two) return fail("Test source import failed.");
    // This is a disposable test-only recovery secret, never a real credential.
    const std::string secret(64, 'a');
    const auto package = scratch.path() / "two-files.mvpk";
    auto exported = sender.value().export_package(package, secret,
        {{one.value(), "private-folder/private-first.txt"},
         {two.value(), "private-folder/private-second.txt"}}, {}, false);
    if (!exported) return fail("Encrypted sync package export failed.");
    std::ifstream input(package, std::ios::binary);
    std::vector<char> ciphertext((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const std::string stored(ciphertext.begin(), ciphertext.end());
    if (stored.find(first) != std::string::npos
        || stored.find("private-folder") != std::string::npos
        || stored.find("private-first.txt") != std::string::npos) {
        return fail("Sync package leaked plaintext content or paths.");
    }
    // The first file is complete before the final record. A truncated or
    // corrupted final record must not leave that file or folders imported.
    if (ciphertext.empty()) return fail("Export unexpectedly produced no bytes.");
    ciphertext.back() ^= 1;
    const auto corrupt = scratch.path() / "corrupt.mvpk";
    std::ofstream output(corrupt, std::ios::binary);
    output.write(ciphertext.data(), static_cast<std::streamsize>(ciphertext.size()));
    output.close();
    auto rejected = receiver.value().import_package(corrupt, secret, 0);
    if (rejected) return fail("A corrupt final authentication tag was accepted.");
    auto rows = receiver.value().list_videos();
    auto folders = receiver.value().list_folders();
    if (!rows || !folders || !rows.value().empty() || !folders.value().empty()) {
        return fail("Unauthenticated sync package changed the destination vault.");
    }
    bool saw_private_staging = false;
    const auto staging = receiver.value().root_path() / ".import-staging";
    auto imported = receiver.value().import_package(package, secret, 0,
        [&](double) {
            if (!std::filesystem::exists(staging)) return;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(staging)) {
                if (entry.is_regular_file()) saw_private_staging = true;
            }
        });
    if (!imported || imported.value() != 2) return fail("Valid encrypted sync package import failed.");
    if (!saw_private_staging) return fail("Package import must stage transient plaintext inside its vault, not shared system temp.");
    if (!std::filesystem::is_empty(staging)) return fail("Package import left plaintext staging files after success.");
    rows = receiver.value().list_videos();
    if (!rows || rows.value().size() != 2) return fail("Receiver must contain both source files.");
    for (const auto& row : rows.value()) {
        auto content = receiver.value().read_video_bytes(row.id);
        if (!content) return fail("Reading received content failed.");
        const std::string plain(content.value().begin(), content.value().end());
        if ((row.display_name == "private-first.txt" && plain != first)
            || (row.display_name == "private-second.txt" && plain != second)) {
            return fail("Sync package round trip changed file content.");
        }
    }
    auto repeated = receiver.value().import_package(package, secret, 0);
    if (!repeated || repeated.value() != 0) return fail("Retrying package import must not duplicate content.");
    std::cout << "Authenticated encrypted sync packages: passed\n";
    return EXIT_SUCCESS;
}
