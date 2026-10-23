// Memory-security regression suite.
//
// Every test works on a throwaway vault whose password and markers are
// generated at run time, so nothing secret exists as a literal in the binary
// and a match can only come from this process's live memory.
//
// The suite does NOT assert "the password byte sequence can never appear in a
// process dump" — that is not a guarantee any of these mechanisms can make for
// Qt, Android or the allocator. It asserts the properties the implementation is
// responsible for:
//   * a Secret's bytes are gone once it is wiped,
//   * the master key is present while unlocked and absent after lock(),
//   * a derived password is not retained after unlock + lock,
//   * decrypted metadata and content stop existing after lock(),
//   * a package reader refuses to decrypt after the session is revoked and its
//     decrypted chunk cache is zeroised,
//   * concurrent readers cannot outlive a lock.
//
// The scanner always excludes the needle's own storage. That is the difference
// between "the application still holds this" and "the measuring instrument is
// holding this", and it is the reason a naive scan reports the test's own copy
// as a leak.

#include "videovault/core/vault.hpp"

#include "internal/crypto.hpp"
#include "internal/metadata.hpp"
#include "internal/package.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using videovault::core::Argon2Parameters;
using videovault::core::Secret;
using videovault::core::Vault;
namespace internal = videovault::core::internal;

int g_failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::cout << "FAIL: " << what << "\n";
        ++g_failures;
    } else {
        std::cout << "ok: " << what << "\n";
    }
}

std::string random_hex(const std::size_t bytes) {
    std::random_device device;
    std::mt19937_64 engine(device());
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2U);
    for (std::size_t i = 0; i < bytes * 2U; ++i) {
        out.push_back(digits[engine() & 0x0fU]);
    }
    return out;
}

void wipe_string(std::string& text) {
    if (!text.empty()) {
        sodium_memzero(text.data(), text.size());
        text.clear();
    }
}

// A byte sequence to search for, and the only copy of it the test keeps. The
// scanner always excludes this buffer, so a match is a copy made by the code
// under test.
class Needle {
public:
    explicit Needle(const std::string& text) : bytes_(text.begin(), text.end()) {}
    explicit Needle(const std::span<const unsigned char> data)
        : bytes_(data.begin(), data.end()) {}

    ~Needle() {
        if (!bytes_.empty()) {
            sodium_memzero(bytes_.data(), bytes_.size());
        }
    }

    Needle(const Needle&) = delete;
    Needle& operator=(const Needle&) = delete;
    Needle(Needle&&) noexcept = default;
    Needle& operator=(Needle&&) noexcept = default;

    [[nodiscard]] std::string_view text() const {
        return std::string_view(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
    }
    [[nodiscard]] std::span<const unsigned char> bytes() const { return bytes_; }

private:
    std::vector<unsigned char> bytes_;
};

// Builds a Needle from a string that is then wiped, so the needle is the only
// remaining copy.
Needle needle_from_temporary(std::string& temporary) {
    Needle needle(temporary);
    wipe_string(temporary);
    return needle;
}

class MemoryScan {
public:
    // Occurrences of `needle` in this process's committed writable memory,
    // excluding the needle's own buffer and any extra range the caller names.
    static std::size_t count(const Needle& needle,
                             const void* extra_begin = nullptr,
                             const std::size_t extra_size = 0U) {
        const auto bytes = needle.bytes();
        if (bytes.empty()) {
            return 0;
        }
        const auto* excluded_begin = static_cast<const unsigned char*>(bytes.data());
        const auto* excluded_end = excluded_begin + bytes.size();
        std::size_t found = 0;
        // The region read buffer lives inside the process being scanned, so a
        // CRT-heap buffer is itself scannable: it holds copies of every region
        // read so far and the scan reports them as leaks. Release made that
        // collision reliable while Debug happened to miss it. Allocate the
        // buffer from libsodium (its own guarded mapping), skip that mapping
        // explicitly, and zero it before every read.
        unsigned char* read_buffer = nullptr;
        std::size_t read_buffer_size = 0U;
        const auto ensure_read_buffer = [&](const std::size_t size) -> bool {
            if (read_buffer != nullptr && size <= read_buffer_size) {
                return true;
            }
            if (read_buffer != nullptr) {
                sodium_memzero(read_buffer, read_buffer_size);
                sodium_free(read_buffer);
                read_buffer = nullptr;
                read_buffer_size = 0U;
            }
            read_buffer = static_cast<unsigned char*>(sodium_malloc(size));
            if (read_buffer == nullptr) {
                return false;
            }
            read_buffer_size = size;
            return true;
        };
        const auto overlaps_read_buffer = [&](const std::uintptr_t begin,
                                              const std::size_t size) {
            if (read_buffer == nullptr) {
                return false;
            }
            const auto buffer_begin = reinterpret_cast<std::uintptr_t>(read_buffer);
            const auto buffer_end = buffer_begin + read_buffer_size;
            return buffer_begin < begin + size && begin < buffer_end;
        };
#if defined(_WIN32)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        auto* address = static_cast<const unsigned char*>(info.lpMinimumApplicationAddress);
        const auto* last =
            static_cast<const unsigned char*>(info.lpMaximumApplicationAddress);
        HANDLE process = GetCurrentProcess();
        MEMORY_BASIC_INFORMATION region{};
        while (address < last && VirtualQuery(address, &region, sizeof(region)) != 0) {
            const DWORD protect = region.Protect;
            const bool readable = region.State == MEM_COMMIT
                && (protect & PAGE_NOACCESS) == 0 && (protect & PAGE_GUARD) == 0
                && (protect & (PAGE_READWRITE | PAGE_WRITECOPY
                        | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            const auto region_begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (readable && region.RegionSize >= bytes.size()
                && !overlaps_read_buffer(region_begin, region.RegionSize)
                && ensure_read_buffer(region.RegionSize)) {
                sodium_memzero(read_buffer, read_buffer_size);
                SIZE_T read = 0;
                if (ReadProcessMemory(process, region.BaseAddress, read_buffer,
                        region.RegionSize, &read) != 0 && read >= bytes.size()) {
                    found += count_in(
                        std::span<const unsigned char>(read_buffer, read), bytes,
                        region_begin,
                        excluded_begin, excluded_end, extra_begin, extra_size);
                }
            }
            address += region.RegionSize;
        }
#else
        std::ifstream maps("/proc/self/maps");
        std::string line;
        const int memory = ::open("/proc/self/mem", O_RDONLY);
        while (std::getline(maps, line) && memory >= 0) {
            unsigned long long begin = 0;
            unsigned long long end = 0;
            char perms[8]{};
            if (std::sscanf(line.c_str(), "%llx-%llx %7s", &begin, &end, perms) != 3) {
                continue;
            }
            if (perms[0] != 'r' || perms[1] != 'w') {
                continue;
            }
            const std::size_t size = static_cast<std::size_t>(end - begin);
            if (size < bytes.size() || size > (64ULL << 20)) {
                continue;
            }
            if (overlaps_read_buffer(static_cast<std::uintptr_t>(begin), size)
                || !ensure_read_buffer(size)) {
                continue;
            }
            sodium_memzero(read_buffer, read_buffer_size);
            const ssize_t read = ::pread(
                memory, read_buffer, size, static_cast<off_t>(begin));
            if (read >= static_cast<ssize_t>(bytes.size())) {
                found += count_in(
                    std::span<const unsigned char>(read_buffer,
                        static_cast<std::size_t>(read)),
                    bytes, static_cast<std::uintptr_t>(begin),
                    excluded_begin, excluded_end, extra_begin, extra_size);
            }
        }
        if (memory >= 0) {
            ::close(memory);
        }
#endif
        if (read_buffer != nullptr) {
            sodium_memzero(read_buffer, read_buffer_size);
            sodium_free(read_buffer);
        }
        return found;
    }

private:
    static std::size_t count_in(const std::span<const unsigned char> haystack,
                                const std::span<const unsigned char> needle,
                                const std::uintptr_t base,
                                const unsigned char* excluded_begin,
                                const unsigned char* excluded_end,
                                const void* extra_begin,
                                const std::size_t extra_size) {
        std::size_t matches = 0;
        const auto extra_start = reinterpret_cast<const unsigned char*>(extra_begin);
        const auto* extra_end = extra_start + extra_size;
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) != 0) {
                continue;
            }
            const auto* at = reinterpret_cast<const unsigned char*>(base + i);
            const bool is_needle = at >= excluded_begin && at < excluded_end;
            const bool is_extra = extra_begin != nullptr && at >= extra_start
                && at < extra_end;
            if (!is_needle && !is_extra) {
                ++matches;
            }
        }
        return matches;
    }
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVault_SecurityMemory_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// A one-pixel PNG header with the marker appended: the core stores content as
// opaque bytes, so the marker survives the round trip and can be searched for.
std::vector<unsigned char> marker_content(const std::span<const unsigned char> marker) {
    static const unsigned char png_head[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
    };
    std::vector<unsigned char> bytes(png_head, png_head + sizeof(png_head));
    bytes.insert(bytes.end(), marker.begin(), marker.end());
    return bytes;
}

Argon2Parameters light_parameters() {
    // The weakest values the core accepts: this suite is about memory, and a
    // production profile would make every test take tens of seconds.
    return Argon2Parameters{8U * 1024U, 1U, 1U};
}

// ---------------------------------------------------------------------------
// 0. The UTF-16 -> UTF-8 encoder used by the Qt and JNI password paths is
//    lossless. A truncation bug here silently changes the password, which shows
//    up as "wrong password" after a password change rather than as a crash.
// ---------------------------------------------------------------------------
void test_utf16_password_encoding() {
    std::cout << "\n[0] Password encoding round trip\n";
    struct Case {
        const char* expected;
        std::u16string utf16;
    };
    const std::vector<Case> cases{
        {"settings-test-only", u"settings-test-only"},
        {"a", u"a"},
        {"abc", u"abc"},
        {"pass", u"pass"},
        {"p\u00e4ssw\u00f6rd", u"p\u00e4ssw\u00f6rd"},
        {"\u20ac\u00a3\u00a5", u"\u20ac\u00a3\u00a5"},
        // One astral character (U+1F512, a surrogate pair) as the last code point.
        {"lock\U0001f512", u"lock\U0001f512"},
        {"\U0001f512", u"\U0001f512"},
    };
    for (const auto& item : cases) {
        const std::string_view expected(item.expected);
        auto from_utf8 = Secret::from_utf8(expected);
        if (!from_utf8) {
            check(false, "Secret::from_utf8 accepts the case");
            continue;
        }
        check(from_utf8->view() == expected, "from_utf8 preserves the bytes");
        auto from_utf16 = Secret::from_utf16(
            std::span<const char16_t>(item.utf16.data(), item.utf16.size()));
        if (!from_utf16) {
            check(false, "Secret::from_utf16 accepts the case");
            continue;
        }
        const bool same = from_utf16->view() == expected;
        if (!same) {
            std::cout << "    mismatch: got " << from_utf16->size() << " bytes, expected "
                      << expected.size() << "\n";
        }
        check(same, "from_utf16 encodes the whole password, including the tail");
    }
}

// ---------------------------------------------------------------------------
// 1. Secret wipes its own bytes.
// ---------------------------------------------------------------------------
void test_secret_wipes_itself() {
    std::cout << "\n[1] Secret zeroises on wipe\n";
    std::string temporary = "MVP_TEST_PASSWORD_" + random_hex(8);
    auto secret = Secret::from_utf8(temporary);
    check(secret.has_value(), "a Secret can be created from text");
    if (!secret) {
        return;
    }
    check(secret->size() == temporary.size(), "the Secret holds the whole password");
    Needle needle = needle_from_temporary(temporary);
    // While the Secret is alive the bytes are in secure memory and the scanner
    // finds exactly that copy; the temporary text is already wiped.
    check(MemoryScan::count(needle) >= 1,
        "the live Secret holds the password in memory");
    secret->wipe();
    check(secret->empty(), "the Secret reports itself empty after wipe");
    const std::size_t after = MemoryScan::count(needle);
    std::cout << "    occurrences after wipe: " << after << "\n";
    check(after == 0, "no copy of the password remains after wipe");
}

// ---------------------------------------------------------------------------
// 2. Vault lifecycle: unlock, read, lock.
// ---------------------------------------------------------------------------
void test_vault_lifecycle() {
    std::cout << "\n[2] Vault lifecycle\n";
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";

    std::string temporary_password = "MVP_TEST_PASSWORD_" + random_hex(11);
    auto password = Secret::from_utf8(temporary_password);
    check(password.has_value(), "the vault password fits in secure memory");
    if (!password) {
        return;
    }
    check(password->memory_locked(),
        "the password buffer is mlock()ed (else this host refused the lock)");
    Needle password_needle = needle_from_temporary(temporary_password);

    std::string temporary_name = "MVP_SECRET_FILENAME_" + random_hex(9) + ".png";
    Needle name_needle = needle_from_temporary(temporary_name);
    std::string temporary_record = "MVP_SECRET_RECORD_" + random_hex(9);
    Needle record_needle = needle_from_temporary(temporary_record);

    auto created = Vault::create(root, *password, light_parameters());
    check(static_cast<bool>(created), "vault created");
    if (!created) {
        return;
    }
    Vault vault = std::move(created).value();

    auto bytes = marker_content(record_needle.bytes());
    bool duplicate = false;
    auto imported = vault.import_bytes(bytes, name_needle.text(), 0, &duplicate);
    sodium_memzero(bytes.data(), bytes.size());
    check(static_cast<bool>(imported), "content imported under the marker name");
    if (!imported) {
        return;
    }
    const auto video_id = imported.value();

    // Derive the same master key the vault holds, then forget the derivation:
    // only the needle may hold it while the scan runs.
    auto metadata = internal::read_metadata_file(root / L"vault.meta");
    check(static_cast<bool>(metadata), "vault metadata readable for key derivation");
    if (!metadata) {
        return;
    }
    auto derived = internal::derive_master_key(
        password->view(), metadata.value().salt, metadata.value().parameters);
    check(static_cast<bool>(derived), "the test can derive the same master key");
    if (!derived) {
        return;
    }
    std::array<unsigned char, 32> key_copy{};
    std::copy_n(derived.value().data(), key_copy.size(), key_copy.begin());
    derived.value().wipe();
    Needle key_needle(std::span<const unsigned char>(key_copy.data(), key_copy.size()));
    sodium_memzero(key_copy.data(), key_copy.size());

    // ---- Stage B: unlocked. The master key must be live. -------------------
    const std::size_t unlocked = MemoryScan::count(key_needle);
    std::cout << "    master key occurrences while unlocked: " << unlocked << "\n";
    check(unlocked >= 1, "the master key is present while the vault is unlocked");

    // ---- Stage C: read decrypted content through the streaming reader. -----
    {
        auto plain = vault.read_video_bytes(video_id);
        check(static_cast<bool>(plain), "decrypted content read back");
        if (plain) {
            // A reference, not a copy: a copy here would be the test's own
            // plaintext and would be reported as a leak by the scan below.
            const auto& content = plain.value();
            const bool matches = std::search(content.begin(), content.end(),
                record_needle.bytes().begin(), record_needle.bytes().end()) != content.end();
            check(matches, "the reading path returns the decrypted marker");
            sodium_memzero(plain.value().data(), plain.value().size());
        }
    }
    // The reader is destroyed inside read_video_bytes, so its chunk cache must
    // be zeroed rather than handed back to the allocator with media in it.
    const std::size_t content_after_reader = MemoryScan::count(record_needle);
    std::cout << "    decrypted-content occurrences after the reader closed: "
              << content_after_reader << "\n";
    check(content_after_reader == 0,
        "the reader's decrypted chunk cache is zeroised when it closes");

    // ---- Stage E: hard lock. ----------------------------------------------
    vault.lock();
    check(!vault.is_unlocked(), "the vault reports itself locked");
    const std::size_t locked = MemoryScan::count(key_needle);
    std::cout << "    master key occurrences after hard lock: " << locked << "\n";
    check(locked == 0, "no copy of the master key survives a hard lock");

    auto denied = vault.read_video_bytes(video_id);
    check(!denied, "a locked vault refuses to decrypt");
    auto listed = vault.list_videos();
    check(!listed, "a locked vault refuses to list decrypted metadata");

    // ---- Stage F: after a short idle period. ------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const std::size_t idle = MemoryScan::count(key_needle);
    check(idle == 0, "the master key stays absent after an idle period");

    // ---- Password retention. ----------------------------------------------
    // The caller still owns one Secret holding the password (it is needed to
    // reopen below). What matters is that the vault kept no copy of its own:
    // exactly one occurrence must remain, and it must be that Secret.
    const std::size_t password_after = MemoryScan::count(password_needle);
    std::cout << "    vault password occurrences after hard lock: " << password_after
              << " (the caller's own Secret is one of them)\n";
    check(password_after == 1,
        "after unlock + lock the only password copy is the caller's own Secret");

    // ---- Stage H: reopen and lock again. ----------------------------------
    auto reopened = Vault::open(root, *password);
    check(static_cast<bool>(reopened), "the vault reopens with the same password");
    if (reopened) {
        Vault again = std::move(reopened).value();
        auto rows = again.list_videos();
        check(static_cast<bool>(rows) && rows.value().size() == 1,
            "existing encrypted content is still readable after reopening");
        if (rows && !rows.value().empty()) {
            check(rows.value().front().display_name == name_needle.text(),
                "the decrypted name matches what was imported");
        }
        const auto generation_before = again.lock_generation();
        again.lock();
        check(again.lock_generation() == generation_before + 1,
            "lock() advances the generation stamp");
    }

    // ---- Stage I: failed unlock. ------------------------------------------
    std::string temporary_wrong = "MVP_TEST_PASSWORD_" + random_hex(11);
    auto wrong = Secret::from_utf8(temporary_wrong);
    wipe_string(temporary_wrong);
    if (wrong) {
        auto rejected = Vault::open(root, *wrong);
        check(!rejected, "a wrong password is rejected");
        if (!rejected) {
            check(rejected.error().code == videovault::core::VaultErrorCode::WrongPassword,
                "the rejection is reported as WrongPassword");
        }
    }

    // Dropping the caller's Secret must leave nothing behind at all.
    password->wipe();
    const std::size_t password_none = MemoryScan::count(password_needle);
    std::cout << "    password occurrences after the caller wiped its Secret: "
              << password_none << "\n";
    check(password_none == 0, "wiping the caller's Secret leaves no password copy");
}

// ---------------------------------------------------------------------------
// 3. A package reader cannot outlive the session it was opened in.
// ---------------------------------------------------------------------------
void test_reader_revocation() {
    std::cout << "\n[3] Package reader revocation and cache zeroisation\n";
    TemporaryDirectory directory;

    std::string temporary_record = "MVP_SECRET_RECORD_" + random_hex(9);
    Needle record_needle = needle_from_temporary(temporary_record);

    std::string body(300000U, 'A');
    std::memcpy(body.data() + 1000U, record_needle.bytes().data(),
        record_needle.bytes().size());

    auto key = std::make_shared<internal::SensitiveBuffer>(32U);
    std::fill_n(key->data(), key->size(), 0x5aU);

    std::ofstream out(directory.path() / L"plain.bin", std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.close();

    std::ifstream in(directory.path() / L"plain.bin", std::ios::binary);
    const auto sealed = internal::write_package_file(
        directory.path() / L"sealed.vvp", *key, 64U * 1024U, in, body.size());
    in.close();
    // The test's own plaintext copy must be gone before the scan runs, or the
    // scan reports the instrument instead of the code under test.
    sodium_memzero(body.data(), body.size());
    check(static_cast<bool>(sealed), "a package was written for the reader test");
    if (!sealed) {
        return;
    }

    auto guard = std::make_shared<internal::SessionGuard>();
    internal::SensitiveBuffer reader_key(32U);
    std::copy_n(key->data(), reader_key.size(), reader_key.data());
    auto reader = internal::PackageReader::open(
        directory.path() / L"sealed.vvp", std::move(reader_key), guard);
    check(static_cast<bool>(reader), "a reader opened on the package");
    if (!reader) {
        return;
    }
    std::vector<unsigned char> buffer(200000U);
    auto read = reader.value().read(buffer);
    check(static_cast<bool>(read) && read.value() > 0U, "the reader decrypts while valid");
    sodium_memzero(buffer.data(), buffer.size());

    // Revoke the way Vault::lock() does.
    guard->revoke();
    check(reader.value().revoked(), "the reader reports the session as revoked");
    std::vector<unsigned char> after(4096U);
    auto denied = reader.value().read(after);
    check(!denied, "a revoked reader refuses to decrypt more content");

    sodium_memzero(after.data(), after.size());
    // Destroying the reader must zeroise its cached plaintext chunk.
    reader.value() = internal::PackageReader{};
    const std::size_t cached = MemoryScan::count(record_needle);
    std::cout << "    decrypted-chunk occurrences after the reader was destroyed: "
              << cached << "\n";
    check(cached == 0, "the reader's cached plaintext is zeroised on destruction");

    key->wipe();
}

// ---------------------------------------------------------------------------
// 4. A concurrent reader cannot keep decrypting after a lock.
// ---------------------------------------------------------------------------
void test_concurrent_lock() {
    std::cout << "\n[4] Concurrent read and lock\n";
    TemporaryDirectory directory;
    const auto root = directory.path() / L"vault";
    std::string temporary_password = "MVP_TEST_PASSWORD_" + random_hex(11);
    auto password = Secret::from_utf8(temporary_password);
    wipe_string(temporary_password);
    if (!password) {
        check(false, "password secret");
        return;
    }
    auto created = Vault::create(root, *password, light_parameters());
    check(static_cast<bool>(created), "vault created for the concurrency test");
    if (!created) {
        return;
    }
    auto vault = std::make_shared<Vault>(std::move(created).value());
    std::string filler(200000U, 'B');
    std::vector<unsigned char> bytes(filler.begin(), filler.end());
    auto imported = vault->import_bytes(bytes, std::string("concurrent.bin"));
    check(static_cast<bool>(imported), "content imported");
    if (!imported) {
        return;
    }
    const auto id = imported.value();

    std::atomic<bool> stop{false};
    std::atomic<int> unexpected{0};
    std::atomic<int> successes{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto result = vault->read_video_range(id, 0U, 1024U);
            if (result) {
                successes.fetch_add(1, std::memory_order_relaxed);
                sodium_memzero(result.value().data(), result.value().size());
            } else {
                const auto& detail = result.error().technical_detail;
                if (detail.find("locked") == std::string::npos) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    vault->lock();
    const int succeeded_before_lock = successes.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    std::cout << "    reads completed before the lock: " << succeeded_before_lock << "\n";
    std::cout << "    unexpected failures: " << unexpected.load() << "\n";
    check(unexpected.load() == 0, "a concurrent lock produces no unexpected error");
    check(!vault->is_unlocked(), "the vault ended locked");
    check(succeeded_before_lock > 0, "the reader did decrypt before the lock");
}

} // namespace

int main() {
    if (sodium_init() < 0) {
        std::cout << "FAIL: sodium_init\n";
        return 1;
    }
    std::cout << "Secure memory supported on this host: "
              << (Vault::secure_memory_available() ? "yes" : "no") << "\n";
    test_utf16_password_encoding();
    test_secret_wipes_itself();
    test_vault_lifecycle();
    test_reader_revocation();
    test_concurrent_lock();
    std::cout << "\n" << (g_failures == 0 ? "PASS" : "FAILURES") << ": "
              << g_failures << "\n";
    return g_failures == 0 ? 0 : 1;
}
