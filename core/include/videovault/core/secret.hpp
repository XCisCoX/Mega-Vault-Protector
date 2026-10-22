#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace videovault::core {

// A move-only owner for secret bytes: vault passwords, package passwords and
// anything else that must not linger in ordinary heap or string memory.
//
// Design rules this type exists to enforce:
//   * The bytes live in libsodium secure memory (sodium_malloc), never in a
//     std::string, QString, std::vector or QByteArray, so no implicitly shared
//     or reallocated copy of the plaintext is created on the way in.
//   * The buffer is wiped with sodium_memzero() before it is released
//     (sodium_free() zeroes again), so the owned bytes do not survive the
//     object.
//   * sodium_mlock() is attempted at construction. memory_locked() reports the
//     result, so callers can surface "the OS refused to lock this page" rather
//     than pretending the stronger guarantee holds.
//   * Copying is deleted: a secret has exactly one owner at a time, and passing
//     it to a worker moves it.
//
// What this type cannot do (documented in docs/security/REMAINING_RISKS.md):
//   * It cannot wipe the caller's original std::string, QString or Kotlin
//     String bytes. Those copies are the caller's responsibility.
//   * It cannot stop a privileged attacker who can read a running process from
//     reading locked pages. mlock only keeps them out of swap.
class Secret final {
public:
    // Password inputs are capped to keep Argon2id input bounded, matching the
    // core's existing limit.
    static constexpr std::size_t kMaxBytes = 4096U;

    Secret() noexcept;
    ~Secret();

    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;

    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;

    // Copies UTF-8 (or arbitrary) bytes into secure memory. Returns nullopt
    // when the text is longer than kMaxBytes or secure memory is unavailable.
    [[nodiscard]] static std::optional<Secret> from_utf8(std::string_view text);

    // Encodes UTF-16 code units straight into the secure buffer as UTF-8, with
    // no intermediate std::string / QByteArray copy. Invalid lone surrogates
    // are replaced with U+FFFD so the result is always valid UTF-8.
    [[nodiscard]] static std::optional<Secret> from_utf16(
        std::span<const char16_t> text);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Read-only view of the owned bytes. Valid only while this object lives and
    // has not been wiped.
    [[nodiscard]] std::string_view view() const noexcept;

    // True when sodium_mlock() succeeded for this buffer.
    [[nodiscard]] bool memory_locked() const noexcept;

    // Zeroes and releases the bytes. Safe to call more than once.
    void wipe() noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    std::size_t size_{0};
};

} // namespace videovault::core
