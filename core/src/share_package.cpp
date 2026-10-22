#include "videovault/core/vault.hpp"

#include "internal/crypto.hpp"

#include <sodium.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>

namespace videovault::core {
namespace {

constexpr std::array<unsigned char, 8> kMagic{
    'M', 'V', 'P', 'K', 0xA7, 0x01, 0xC3, 0x00};
constexpr std::uint8_t kVersion = 1;
constexpr std::uint32_t kChunkBytes = 1U << 20;
constexpr std::size_t kMaxPathBytes = 1024;
constexpr Argon2Parameters kPackageKdf{64U * 1024U, 3U, 1U};

enum class Record : std::uint8_t { File = 1, Chunk = 2, Directory = 3, End = 4 };

VaultError fail(const VaultErrorCode code, std::string detail) {
    return VaultError{code, std::move(detail)};
}

void write_u16(std::ostream& out, const std::uint16_t value) {
    const unsigned char bytes[2] = {
        static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8)};
    out.write(reinterpret_cast<const char*>(bytes), 2);
}

void write_u32(std::ostream& out, const std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 24)};
    out.write(reinterpret_cast<const char*>(bytes), 4);
}

void write_u64(std::ostream& out, const std::uint64_t value) {
    unsigned char bytes[8]{};
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    }
    out.write(reinterpret_cast<const char*>(bytes), 8);
}

bool read_exact(std::istream& in, unsigned char* data, const std::size_t size) {
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount()) == size;
}

std::uint16_t read_u16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_u32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_u64(const unsigned char* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
    }
    return value;
}

// Package paths stay relative and use '/'. Archive tools never see this file,
// and the path itself must not escape the folder the package is imported into.
bool valid_relative(const std::string& path) {
    if (path.empty() || path.size() > kMaxPathBytes || path.front() == '/' || path.back() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.size() > 120) {
            return false;
        }
        if (part.front() == ' ' || part.back() == ' ' || part.back() == '.') {
            return false;
        }
        for (const unsigned char byte : part) {
            if (byte < 0x20U || byte == 0x7FU || byte == '\\' || byte == ':'
                || byte == '*' || byte == '?' || byte == '"' || byte == '<'
                || byte == '>' || byte == '|') {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

std::string parent_path(const std::string& relative) {
    const auto slash = relative.rfind('/');
    return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
}

std::string leaf_name(const std::string& relative) {
    const auto slash = relative.rfind('/');
    return slash == std::string::npos ? relative : relative.substr(slash + 1);
}

Result<bool> push_record(
    crypto_secretstream_xchacha20poly1305_state& state,
    std::ostream& out,
    const std::vector<unsigned char>& plain,
    const unsigned char tag) {
    std::vector<unsigned char> cipher(plain.size() + crypto_secretstream_xchacha20poly1305_ABYTES);
    unsigned long long clen = 0;
    if (crypto_secretstream_xchacha20poly1305_push(
            &state, cipher.data(), &clen, plain.data(), plain.size(), nullptr, 0, tag) != 0) {
        return fail(VaultErrorCode::CryptoFailure, "The package could not be encrypted.");
    }
    write_u32(out, static_cast<std::uint32_t>(clen));
    out.write(reinterpret_cast<const char*>(cipher.data()), static_cast<std::streamsize>(clen));
    if (!out) {
        return fail(VaultErrorCode::DiskFull, "The package could not be written.");
    }
    return true;
}

Result<std::vector<unsigned char>> pull_record(
    crypto_secretstream_xchacha20poly1305_state& state,
    std::istream& in,
    unsigned char* tag,
    bool* saw_final) {
    unsigned char len_bytes[4]{};
    if (!read_exact(in, len_bytes, 4)) {
        return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
    }
    const auto clen = read_u32(len_bytes);
    const auto max_cipher = compressBound(kChunkBytes) + 64U
        + static_cast<std::uint32_t>(crypto_secretstream_xchacha20poly1305_ABYTES);
    if (clen < crypto_secretstream_xchacha20poly1305_ABYTES || clen > max_cipher) {
        return fail(VaultErrorCode::PackageModified, "The package is damaged.");
    }
    std::vector<unsigned char> cipher(clen);
    if (!read_exact(in, cipher.data(), cipher.size())) {
        return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
    }
    std::vector<unsigned char> plain(clen);
    unsigned long long mlen = 0;
    const int status = crypto_secretstream_xchacha20poly1305_pull(
        &state, plain.data(), &mlen, tag, cipher.data(), cipher.size(), nullptr, 0);
    if (status != 0) {
        return fail(
            *saw_final ? VaultErrorCode::PackageModified : VaultErrorCode::WrongPassword,
            *saw_final ? "The package is damaged." : "The package password is incorrect.");
    }
    if (*tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
        *saw_final = true;
    }
    plain.resize(static_cast<std::size_t>(mlen));
    return plain;
}

std::vector<unsigned char> file_record(const std::string& path, const std::uint64_t size) {
    std::vector<unsigned char> plain(1 + 2 + path.size() + 8);
    plain[0] = static_cast<unsigned char>(Record::File);
    plain[1] = static_cast<unsigned char>(path.size());
    plain[2] = static_cast<unsigned char>(path.size() >> 8);
    std::memcpy(plain.data() + 3, path.data(), path.size());
    for (int i = 0; i < 8; ++i) {
        plain[3 + path.size() + static_cast<std::size_t>(i)] =
            static_cast<unsigned char>(size >> (8 * i));
    }
    return plain;
}

std::vector<unsigned char> directory_record(const std::string& path) {
    std::vector<unsigned char> plain(1 + 2 + path.size());
    plain[0] = static_cast<unsigned char>(Record::Directory);
    plain[1] = static_cast<unsigned char>(path.size());
    plain[2] = static_cast<unsigned char>(path.size() >> 8);
    std::memcpy(plain.data() + 3, path.data(), path.size());
    return plain;
}

std::vector<unsigned char> chunk_record(const std::vector<unsigned char>& raw, const bool compress) {
    std::vector<unsigned char> stored = raw;
    unsigned char flag = 0;
    if (compress && !raw.empty()) {
        uLongf bound = compressBound(static_cast<uLong>(raw.size()));
        std::vector<unsigned char> zipped(bound);
        const int status = compress2(
            zipped.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED);
        if (status == Z_OK && bound < raw.size()) {
            zipped.resize(bound);
            stored = std::move(zipped);
            flag = 1;
        }
    }
    std::vector<unsigned char> plain(1 + 1 + 4 + stored.size());
    plain[0] = static_cast<unsigned char>(Record::Chunk);
    plain[1] = flag;
    const auto size = static_cast<std::uint32_t>(raw.size());
    plain[2] = static_cast<unsigned char>(size);
    plain[3] = static_cast<unsigned char>(size >> 8);
    plain[4] = static_cast<unsigned char>(size >> 16);
    plain[5] = static_cast<unsigned char>(size >> 24);
    if (!stored.empty()) {
        std::memcpy(plain.data() + 6, stored.data(), stored.size());
    }
    return plain;
}

bool known_archive(const unsigned char* data, const std::size_t size) {
    if (size >= 7 && std::memcmp(data, "Rar!\x1a\x07", 6) == 0) {
        return true;
    }
    if (size >= 4 && data[0] == 'P' && data[1] == 'K') {
        return true;
    }
    const unsigned char seven[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
    return size >= 6 && std::memcmp(data, seven, 6) == 0;
}

// A package received through sync is untrusted even if its transport digest
// matches. Authenticate and validate the entire stream before importing any
// file or creating folders. This uses bounded chunk memory, not a plaintext
// copy of the complete archive. The caller then rewinds the encrypted file.
Result<bool> validate_records(
    std::istream& in,
    crypto_secretstream_xchacha20poly1305_state state) {
    struct WipeState {
        crypto_secretstream_xchacha20poly1305_state& state;
        ~WipeState() { sodium_memzero(&state, sizeof(state)); }
    } wipe_state{state};
    bool saw_final = false;
    bool active = false;
    std::uint64_t expected = 0;
    std::uint64_t written = 0;
    for (;;) {
        unsigned char tag = 0;
        auto record = pull_record(state, in, &tag, &saw_final);
        if (!record) return record.error();
        auto& plain = record.value();
        struct WipeRecord {
            std::vector<unsigned char>& bytes;
            ~WipeRecord() { sodium_memzero(bytes.data(), bytes.size()); }
        } wipe_record{plain};
        if (plain.empty()) return fail(VaultErrorCode::PackageModified, "The package is damaged.");
        const auto kind = static_cast<Record>(plain[0]);
        if (kind != Record::End && tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            return fail(VaultErrorCode::PackageModified, "The package ended before its final record.");
        }
        if (kind == Record::File || kind == Record::Directory || kind == Record::End) {
            if (active && written != expected) {
                return fail(VaultErrorCode::PackageModified, "A file in the package is the wrong size.");
            }
            active = false;
        }
        if (kind == Record::File || kind == Record::Directory) {
            const std::size_t tail = kind == Record::File ? 8U : 0U;
            if (plain.size() < 3U + tail) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const auto length = read_u16(plain.data() + 1);
            if (plain.size() != 3U + length + tail) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const std::string path(reinterpret_cast<const char*>(plain.data() + 3), length);
            if (!valid_relative(path)) {
                return fail(VaultErrorCode::PackageModified, "The package contains an unsafe path.");
            }
            if (kind == Record::File) {
                active = true;
                expected = read_u64(plain.data() + 3 + length);
                written = 0;
            }
        } else if (kind == Record::Chunk) {
            if (!active || plain.size() < 6 || plain[1] > 1) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const auto full = read_u32(plain.data() + 2);
            if (full > kChunkBytes || full > expected - written) {
                return fail(VaultErrorCode::PackageModified, "A file in the package is larger than it claims.");
            }
            const auto stored = plain.size() - 6;
            if (plain[1] == 0) {
                if (stored != full) return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            } else {
                std::vector<unsigned char> raw(full);
                uLongf count = full;
                const int status = uncompress(raw.data(), &count, plain.data() + 6,
                    static_cast<uLong>(stored));
                sodium_memzero(raw.data(), raw.size());
                if (status != Z_OK || count != full) {
                    return fail(VaultErrorCode::PackageModified, "The package is damaged.");
                }
            }
            written += full;
        } else if (kind == Record::End) {
            if (plain.size() != 1 || tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL
                || in.peek() != std::char_traits<char>::eof()) {
                return fail(VaultErrorCode::PackageModified, "The package is incomplete or has trailing data.");
            }
            return true;
        } else {
            return fail(VaultErrorCode::UnsupportedPackageVersion, "This package uses an unknown record.");
        }
    }
}

} // namespace

Result<bool> Vault::export_package(
    const std::filesystem::path& destination,
    const std::string_view password,
    const std::vector<PackageItem>& files,
    const std::vector<std::string>& directories,
    const bool compress,
    const std::function<void(double)>& progress) {
    auto secret = Secret::from_utf8(password);
    if (!secret) {
        return fail(VaultErrorCode::InvalidArgument,
            "The package password is too long or secure memory is unavailable.");
    }
    auto result = export_package(destination, *secret, files, directories, compress, progress);
    secret->wipe();
    return result;
}

Result<bool> Vault::export_package(
    const std::filesystem::path& destination,
    const Secret& password,
    const std::vector<PackageItem>& files,
    const std::vector<std::string>& directories,
    const bool compress,
    const std::function<void(double)>& progress) {
    auto crypto = internal::initialize_crypto();
    if (!crypto) {
        return crypto.error();
    }
    if (!is_unlocked()) {
        return fail(VaultErrorCode::InvalidArgument, "Unlock the vault before exporting a package.");
    }
    if (files.empty() && directories.empty()) {
        return fail(VaultErrorCode::InvalidArgument, "Choose items or a folder to put in the package.");
    }
    for (const auto& dir : directories) {
        if (!valid_relative(dir)) {
            return fail(VaultErrorCode::InvalidArgument, "A folder path in the package is not valid.");
        }
    }
    std::uint64_t total = 0;
    std::vector<VideoInfo> rows;
    rows.reserve(files.size());
    auto listed = list_videos();
    if (!listed) {
        return listed.error();
    }
    for (const auto& item : files) {
        if (!valid_relative(item.relative_path)) {
            return fail(VaultErrorCode::InvalidArgument, "A file path in the package is not valid.");
        }
        const VideoInfo* found = nullptr;
        for (const auto& video : listed.value()) {
            if (video.id == item.video_id) {
                found = &video;
                break;
            }
        }
        if (found == nullptr) {
            return fail(VaultErrorCode::InvalidArgument, "One of the selected items is no longer in the vault.");
        }
        rows.push_back(*found);
        total += found->original_size;
    }

    std::array<unsigned char, 16> salt{};
    internal::random_bytes(salt);
    auto key = internal::derive_master_key(password.view(), salt, kPackageKdf);
    if (!key) {
        return key.error();
    }
    if (key.value().size() != crypto_secretstream_xchacha20poly1305_KEYBYTES) {
        return fail(VaultErrorCode::CryptoFailure, "The package key could not be derived.");
    }

    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(VaultErrorCode::PermissionDenied, "The package file could not be created.");
    }
    struct Cleanup {
        std::filesystem::path path;
        bool keep{false};
        ~Cleanup() {
            if (!keep) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } cleanup{destination};

    out.write(reinterpret_cast<const char*>(kMagic.data()), static_cast<std::streamsize>(kMagic.size()));
    const unsigned char version_flags[4] = {kVersion, static_cast<unsigned char>(compress ? 1 : 0), 0, 0};
    out.write(reinterpret_cast<const char*>(version_flags), 4);
    write_u32(out, kPackageKdf.memory_kib);
    write_u32(out, kPackageKdf.iterations);
    write_u32(out, kPackageKdf.parallelism);
    out.write(reinterpret_cast<const char*>(salt.data()), static_cast<std::streamsize>(salt.size()));

    crypto_secretstream_xchacha20poly1305_state state{};
    unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header, key.value().data()) != 0) {
        return fail(VaultErrorCode::CryptoFailure, "The package could not be encrypted.");
    }
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    if (!out) {
        return fail(VaultErrorCode::DiskFull, "The package could not be written.");
    }

    for (const auto& dir : directories) {
        auto pushed = push_record(state, out, directory_record(dir), crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
        if (!pushed) {
            return pushed.error();
        }
    }

    std::uint64_t done = 0;
    std::vector<unsigned char> chunk(kChunkBytes);
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& item = files[index];
        const auto& row = rows[index];
        auto pushed = push_record(
            state, out, file_record(item.relative_path, row.original_size),
            crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
        if (!pushed) {
            return pushed.error();
        }
        std::uint64_t offset = 0;
        while (offset < row.original_size) {
            const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, row.original_size - offset));
            auto bytes = read_video_range(item.video_id, offset, want);
            if (!bytes) {
                return bytes.error();
            }
            if (bytes.value().size() != want) {
                return fail(VaultErrorCode::PackageMissing, "An item ended before all of its bytes were read.");
            }
            auto record = chunk_record(bytes.value(), compress);
            auto wrote = push_record(state, out, record, crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
            if (!wrote) {
                return wrote.error();
            }
            offset += want;
            done += want;
            if (progress && total > 0) {
                progress(static_cast<double>(done) / static_cast<double>(total));
            }
        }
    }

    const std::vector<unsigned char> end_record{static_cast<unsigned char>(Record::End)};
    auto finished = push_record(state, out, end_record, crypto_secretstream_xchacha20poly1305_TAG_FINAL);
    if (!finished) {
        return finished.error();
    }
    out.flush();
    if (!out) {
        return fail(VaultErrorCode::DiskFull, "The package could not be written.");
    }
    cleanup.keep = true;
    if (progress) {
        progress(1.0);
    }
    return true;
}

Result<int> Vault::import_package(
    const std::filesystem::path& source,
    const std::string_view password,
    const std::int64_t folder_id,
    const std::function<void(double)>& progress,
    const std::function<void(std::int64_t, const std::string&, std::uint64_t, std::int64_t)>& on_file,
    const std::function<void(std::int64_t, std::int64_t, const std::string&)>& on_folder) {
    auto secret = Secret::from_utf8(password);
    if (!secret) {
        return fail(VaultErrorCode::InvalidArgument,
            "The package password is too long or secure memory is unavailable.");
    }
    auto result = import_package(source, *secret, folder_id, progress, on_file, on_folder);
    secret->wipe();
    return result;
}

Result<int> Vault::import_package(
    const std::filesystem::path& source,
    const Secret& password,
    const std::int64_t folder_id,
    const std::function<void(double)>& progress,
    const std::function<void(std::int64_t, const std::string&, std::uint64_t, std::int64_t)>& on_file,
    const std::function<void(std::int64_t, std::int64_t, const std::string&)>& on_folder) {
    auto crypto = internal::initialize_crypto();
    if (!crypto) {
        return crypto.error();
    }
    if (!is_unlocked()) {
        return fail(VaultErrorCode::InvalidArgument, "Unlock the vault before importing a package.");
    }
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        return fail(VaultErrorCode::PackageMissing, "The package file could not be opened.");
    }
    unsigned char magic[8]{};
    if (!read_exact(in, magic, 8)) {
        return fail(VaultErrorCode::InvalidArgument, "This file is not a vault package.");
    }
    if (known_archive(magic, 8)) {
        return fail(VaultErrorCode::InvalidArgument,
            "This is a normal archive. A vault package has its own format and does not open in WinRAR.");
    }
    if (std::memcmp(magic, kMagic.data(), kMagic.size()) != 0) {
        return fail(VaultErrorCode::InvalidArgument, "This file is not a vault package.");
    }
    unsigned char version_flags[4]{};
    if (!read_exact(in, version_flags, 4) || version_flags[0] != kVersion) {
        return fail(VaultErrorCode::UnsupportedPackageVersion, "This package was made by a newer version.");
    }
    unsigned char kdf_bytes[12]{};
    if (!read_exact(in, kdf_bytes, 12)) {
        return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
    }
    Argon2Parameters kdf;
    kdf.memory_kib = read_u32(kdf_bytes);
    kdf.iterations = read_u32(kdf_bytes + 4);
    kdf.parallelism = read_u32(kdf_bytes + 8);
    std::array<unsigned char, 16> salt{};
    if (!read_exact(in, salt.data(), salt.size())) {
        return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
    }
    auto key = internal::derive_master_key(password.view(), salt, kdf);
    if (!key) {
        return key.error();
    }
    unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (!read_exact(in, header, sizeof(header))) {
        return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
    }
    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header, key.value().data()) != 0) {
        return fail(VaultErrorCode::CryptoFailure, "The package could not be decrypted.");
    }

    const auto records_start = in.tellg();
    auto validated = validate_records(in, state);
    if (!validated) return validated.error();
    in.clear();
    in.seekg(records_start);
    if (!in) return fail(VaultErrorCode::PackageModified, "The package could not be read again.");

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(source, size_error);
    std::unordered_map<std::string, std::int64_t> folder_ids;
    folder_ids.emplace(std::string{}, folder_id);
    auto listed = list_folders();
    if (!listed) {
        return listed.error();
    }
    std::map<std::pair<std::int64_t, std::string>, std::int64_t> existing;
    for (const auto& folder : listed.value()) {
        existing.emplace(std::make_pair(folder.parent_id, folder.name), folder.id);
    }

    std::function<Result<std::int64_t>(const std::string&)> ensure_folder;
    ensure_folder = [&](const std::string& relative) -> Result<std::int64_t> {
        if (relative.empty()) {
            return folder_id;
        }
        if (const auto cached = folder_ids.find(relative); cached != folder_ids.end()) {
            return cached->second;
        }
        const auto parent_rel = parent_path(relative);
        auto parent = ensure_folder(parent_rel);
        if (!parent) {
            return parent.error();
        }
        const auto name = leaf_name(relative);
        const auto key_pair = std::make_pair(parent.value(), name);
        if (const auto found = existing.find(key_pair); found != existing.end()) {
            folder_ids.emplace(relative, found->second);
            return found->second;
        }
        auto created = create_folder(parent.value(), name);
        if (!created) {
            return created.error();
        }
        existing.emplace(key_pair, created.value());
        folder_ids.emplace(relative, created.value());
        if (on_folder) {
            on_folder(created.value(), parent.value(), name);
        }
        return created.value();
    };

    std::error_code mkdir_error;
    // Transient plaintext belongs to the destination vault's security boundary,
    // not a shared OS temp directory (notably /tmp on Android/Linux). Windows
    // inherits the vault directory's ACL; use a private user-owned vault root.
    const auto scratch = root_ / ".import-staging";
    std::filesystem::create_directory(scratch, mkdir_error);
    if (mkdir_error || std::filesystem::is_symlink(scratch, mkdir_error)
        || mkdir_error || !std::filesystem::is_directory(scratch, mkdir_error) || mkdir_error) {
        return fail(VaultErrorCode::PermissionDenied, "A private package staging directory could not be created.");
    }
#if !defined(_WIN32)
    std::filesystem::permissions(scratch, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, mkdir_error);
    if (mkdir_error) return fail(VaultErrorCode::PermissionDenied, "Package staging permissions could not be restricted.");
#endif
    std::array<unsigned char, 16> unique{};
    internal::random_bytes(unique);
    std::string unique_name;
    for (const auto byte : unique) {
        constexpr char hex[] = "0123456789abcdef";
        unique_name.push_back(hex[byte >> 4]);
        unique_name.push_back(hex[byte & 0x0F]);
    }
    const auto scratch_dir = scratch / unique_name;
    if (!std::filesystem::create_directory(scratch_dir, mkdir_error) || mkdir_error) {
        return fail(VaultErrorCode::PermissionDenied, "A unique package staging directory could not be created.");
    }
#if !defined(_WIN32)
    std::filesystem::permissions(scratch_dir, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, mkdir_error);
    if (mkdir_error) {
        std::error_code ignored;
        std::filesystem::remove(scratch_dir, ignored);
        return fail(VaultErrorCode::PermissionDenied, "Package staging permissions could not be restricted.");
    }
#endif
    struct ScratchCleanup {
        std::filesystem::path path;
        ~ScratchCleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } scratch_cleanup{scratch_dir};

    int added = 0;
    int parts = 0;
    struct OpenFile {
        bool active{false};
        std::filesystem::path temp;
        std::ofstream stream;
        std::string relative;
        std::uint64_t expected{0};
        std::uint64_t written{0};
    } current;

    auto close_current = [&]() -> Result<bool> {
        if (!current.active) {
            return true;
        }
        current.stream.close();
        if (current.written != current.expected) {
            return fail(VaultErrorCode::PackageModified, "A file in the package is the wrong size.");
        }
        auto folder = ensure_folder(parent_path(current.relative));
        if (!folder) {
            return folder.error();
        }
        bool already = false;
        // A package import can be resumed after a disconnect, so identical
        // content resolves onto the item already received instead of stacking
        // a second copy of every record on a retry.
        auto imported = import_file(current.temp, {}, folder.value(), &already, true);
        std::error_code ignored;
        std::filesystem::remove(current.temp, ignored);
        const auto relative = current.relative;
        const auto expected = current.expected;
        current.active = false;
        if (!imported) {
            return imported.error();
        }
        if (!already) {
            ++added;
            const auto leaf = leaf_name(relative);
            std::string shown(current.temp.filename().string());
            auto renamed = rename_video(imported.value(), leaf);
            if (renamed) {
                shown = leaf;
            }
            for (int suffix = 2; !renamed && suffix < 80
                && renamed.error().technical_detail.find("already here") != std::string::npos;
                ++suffix) {
                const auto dot = leaf.rfind('.');
                const auto extra = " (" + std::to_string(suffix) + ")";
                std::string candidate = (dot != std::string::npos && dot > 0)
                    ? leaf.substr(0, dot) + extra + leaf.substr(dot)
                    : leaf + extra;
                if (candidate.size() > 120) {
                    break;
                }
                renamed = rename_video(imported.value(), candidate);
                if (renamed) {
                    shown = std::move(candidate);
                }
            }
            if (on_file) {
                on_file(imported.value(), shown, expected, folder.value());
            }
        }
        return true;
    };

    bool saw_final = false;
    bool finished = false;
    while (!finished) {
        unsigned char tag = 0;
        auto record = pull_record(state, in, &tag, &saw_final);
        if (!record) {
            return record.error();
        }
        const auto& plain = record.value();
        if (plain.empty()) {
            return fail(VaultErrorCode::PackageModified, "The package is damaged.");
        }
        const auto kind = static_cast<Record>(plain[0]);
        if (kind == Record::Directory) {
            auto closed = close_current();
            if (!closed) {
                return closed.error();
            }
            if (plain.size() < 3) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const auto path_len = read_u16(plain.data() + 1);
            if (plain.size() != 3U + path_len) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const std::string path(reinterpret_cast<const char*>(plain.data() + 3), path_len);
            if (!valid_relative(path)) {
                return fail(VaultErrorCode::PackageModified, "The package contains an unsafe folder path.");
            }
            auto made = ensure_folder(path);
            if (!made) {
                return made.error();
            }
        } else if (kind == Record::File) {
            auto closed = close_current();
            if (!closed) {
                return closed.error();
            }
            if (plain.size() < 11) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const auto path_len = read_u16(plain.data() + 1);
            if (plain.size() != 3U + path_len + 8U) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const std::string path(reinterpret_cast<const char*>(plain.data() + 3), path_len);
            if (!valid_relative(path)) {
                return fail(VaultErrorCode::PackageModified, "The package contains an unsafe file path.");
            }
            current = {};
            current.active = true;
            current.relative = path;
            current.expected = read_u64(plain.data() + 3 + path_len);
            current.temp = scratch_dir / ("part-" + std::to_string(++parts));
            current.stream.open(current.temp, std::ios::binary | std::ios::trunc);
            if (!current.stream) {
                return fail(VaultErrorCode::DiskFull, "A temporary copy of a packaged file could not be created.");
            }
        } else if (kind == Record::Chunk) {
            if (!current.active || plain.size() < 6) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const bool compressed = plain[1] != 0;
            const auto full = read_u32(plain.data() + 2);
            if (full > kChunkBytes) {
                return fail(VaultErrorCode::PackageModified, "The package is damaged.");
            }
            const auto* data = plain.data() + 6;
            const auto data_size = plain.size() - 6;
            std::vector<unsigned char> raw;
            if (!compressed) {
                if (data_size != full) {
                    return fail(VaultErrorCode::PackageModified, "The package is damaged.");
                }
                raw.assign(data, data + data_size);
            } else {
                raw.resize(full);
                uLongf dest = full;
                if (uncompress(raw.data(), &dest, data, static_cast<uLong>(data_size)) != Z_OK
                    || dest != full) {
                    return fail(VaultErrorCode::PackageModified, "The package is damaged.");
                }
            }
            current.stream.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if (!current.stream) {
                return fail(VaultErrorCode::DiskFull, "The packaged file could not be written.");
            }
            current.written += raw.size();
            if (current.written > current.expected) {
                return fail(VaultErrorCode::PackageModified, "A file in the package is larger than it claims.");
            }
        } else if (kind == Record::End) {
            auto closed = close_current();
            if (!closed) {
                return closed.error();
            }
            if (tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
                return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
            }
            finished = true;
        } else {
            return fail(VaultErrorCode::UnsupportedPackageVersion, "This package uses an unknown record.");
        }
        if (progress && !size_error && file_size > 0) {
            const auto at = in.tellg();
            if (at >= 0) {
                progress(static_cast<double>(at) / static_cast<double>(file_size));
            }
        }
    }
    if (!saw_final) {
        return fail(VaultErrorCode::PackageModified, "The package is incomplete.");
    }
    if (progress) {
        progress(1.0);
    }
    return added;
}

} // namespace videovault::core
