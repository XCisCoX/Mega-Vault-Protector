// Characterizes the shared vault boundary used by music providers. The audio
// fixture is generated in RAM; the only persisted content is the encrypted vault.
#include "videovault/core/vault.hpp"

#include <algorithm>
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
class Sandbox {
public:
    Sandbox() {
        path = std::filesystem::temp_directory_path() /
            ("MegaVaultPluginMemory_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~Sandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::vector<unsigned char> wave() {
    std::vector<unsigned char> bytes;
    const auto text = [&bytes](const std::string& value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    const auto u16 = [&bytes](unsigned value) {
        bytes.push_back(static_cast<unsigned char>(value));
        bytes.push_back(static_cast<unsigned char>(value >> 8U));
    };
    const auto u32 = [&bytes](unsigned value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(static_cast<unsigned char>(value >> shift));
    };
    text("RIFF"); u32(8036); text("WAVEfmt "); u32(16);
    u16(1); u16(1); u32(8000); u32(8000); u16(1); u16(8);
    text("data"); u32(8000);
    for (unsigned sample = 0; sample != 8000; ++sample)
        bytes.push_back(static_cast<unsigned char>((sample * 17U) & 255U));
    return bytes;
}

int fail(const char* message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}
}

int main() {
    using namespace videovault::core;
    Sandbox sandbox;
    Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2;
    parameters.parallelism = 1;
    const auto root = sandbox.path / "vault";
    auto created = Vault::create(root, "plugin memory test password", parameters);
    if (!created) return fail("Could not create test vault.");
    auto& vault = created.value();
    auto folder = vault.create_folder(0, "Downloaded music");
    if (!folder) return fail("Could not create target folder.");
    auto audio = wave();
    const auto saved = vault.import_bytes(audio, "Memory-only fixture.wav", folder.value());
    if (!saved) return fail("Memory audio import failed.");
    auto read = vault.read_video_bytes(saved.value());
    if (!read || read.value() != audio) return fail("Audio readback is not byte-exact.");
    auto range = vault.read_video_range(saved.value(), 44, 128);
    if (!range || range.value().size() != 128 ||
        !std::equal(range.value().begin(), range.value().end(), audio.begin() + 44))
        return fail("Playback range read differs from imported audio.");
    auto media = vault.media_info(saved.value());
    if (!media || media.value().codec_name != "pcm_u8" ||
        media.value().duration_ms < 950 || media.value().duration_ms > 1050)
        return fail("Real FFmpeg probe did not recognize the memory-imported WAV.");
    const auto duplicate = vault.import_bytes(audio, "Same audio, another name.wav", 0);
    if (!duplicate || duplicate.value() != saved.value())
        return fail("Duplicate memory download created a second item.");
    auto videos = vault.list_videos();
    if (!videos || videos.value().size() != 1 ||
        videos.value()[0].folder_id != folder.value())
        return fail("Target folder or deduplication was not preserved.");
    if (vault.import_bytes(audio, "Invalid destination.wav", -1))
        return fail("An invalid target folder must not silently become the root.");
    if (vault.import_bytes(audio, "Missing destination.wav", 999999))
        return fail("A missing target folder must not silently become the root.");

    // Inspect the sandbox, not the OS's unrelated temporary files. No fixture
    // source file was created, and no plaintext PCM marker may appear in any
    // persisted vault file. This does not assert against OS paging/crash dumps.
    const auto marker = std::span<const unsigned char>(audio).subspan(44, 512);
    std::size_t packages = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(sandbox.path)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = entry.path().lexically_relative(sandbox.path);
        if (relative.begin() == relative.end() || *relative.begin() != "vault")
            return fail("Import wrote a file outside the vault sandbox.");
        if (entry.path().extension() == ".wav")
            return fail("Import persisted a plaintext WAV.");
        std::ifstream input(entry.path(), std::ios::binary);
        const std::vector<unsigned char> stored{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (std::search(stored.begin(), stored.end(), marker.begin(), marker.end()) != stored.end())
            return fail("Plaintext audio marker leaked into a persisted file.");
        if (entry.path().extension() == ".vvp") ++packages;
    }
    if (packages != 1) return fail("Expected exactly one encrypted audio package.");
    vault.lock();
    if (vault.import_bytes(audio, "Locked import.wav", folder.value()))
        return fail("Locked vault accepted memory audio.");
    auto opened = Vault::open(root, "plugin memory test password");
    if (!opened) return fail("Could not reopen the encrypted vault.");
    auto reopened = opened.value().read_video_bytes(saved.value());
    if (!reopened || reopened.value() != audio)
        return fail("Memory audio did not survive encrypted-vault reopen.");
    std::fill(audio.begin(), audio.end(), 0);
    std::fill(read.value().begin(), read.value().end(), 0);
    std::fill(reopened.value().begin(), reopened.value().end(), 0);
    std::cout << "PASS: RAM audio -> encrypted vault, FFmpeg probe/range read, "
                 "folder/dedup/lock/reopen, no plaintext source or sandbox leakage.\n";
    return EXIT_SUCCESS;
}
