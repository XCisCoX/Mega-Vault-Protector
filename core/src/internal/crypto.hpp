#pragma once

#include "videovault/core/vault.hpp"

#include <sodium.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace videovault::core::internal {

// Move-only owner of a fixed-size sensitive byte buffer.
//
// The allocation comes from sodium_malloc(), so it is guarded, wiped on free
// and (when the OS allows) mlocked. memory_locked() reports whether mlock()
// actually succeeded; callers must not assume it did.
class SensitiveBuffer final {
public:
    SensitiveBuffer() noexcept = default;
    explicit SensitiveBuffer(std::size_t size);
    ~SensitiveBuffer();

    SensitiveBuffer(SensitiveBuffer&& other) noexcept;
    SensitiveBuffer& operator=(SensitiveBuffer&& other) noexcept;

    SensitiveBuffer(const SensitiveBuffer&) = delete;
    SensitiveBuffer& operator=(const SensitiveBuffer&) = delete;

    [[nodiscard]] unsigned char* data() noexcept { return data_; }
    [[nodiscard]] const unsigned char* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

    // True when sodium_mlock() succeeded for this buffer. False means the bytes
    // are still wiped on release but the page may be swappable.
    [[nodiscard]] bool memory_locked() const noexcept { return locked_; }

    // Zeroes the contents but keeps the allocation, so a buffer can be reused
    // for the next write without a fresh mlock.
    void wipe() noexcept;

    // Copies up to size() bytes of `source` into the buffer and returns how
    // many were written. Zeroes any remainder so no stale bytes survive.
    [[nodiscard]] std::size_t assign(std::span<const unsigned char> source) noexcept;

private:
    void release() noexcept;

    unsigned char* data_{nullptr};
    std::size_t size_{0};
    bool locked_{false};
};

// True when the process could lock a probe page at initialize_crypto() time.
// Used to report a weaker guarantee instead of silently claiming mlock worked.
[[nodiscard]] bool secure_memory_supported() noexcept;

// Overwrites `bytes` with zeros in a way the compiler may not elide.
void wipe_bytes(std::span<unsigned char> bytes) noexcept;

// UTF-16 (QString / Java char) to UTF-8 conversion used by Secret::from_utf16
// so a password never passes through an intermediate std::string or QByteArray.
[[nodiscard]] std::size_t utf16_utf8_length(std::span<const char16_t> text) noexcept;
std::size_t encode_utf16_as_utf8(
    std::span<const char16_t> text,
    std::span<unsigned char> destination) noexcept;

// Shared revocation switch for anything that holds decryption material outside
// the vault mutex (package readers, media probes, sharing workers).
//
// Hard lock revokes the switch, so a reader that outlives the lock refuses to
// decrypt anything more even though its already-unwrapped file key only dies
// when the reader is destroyed.
class SessionGuard final {
public:
    SessionGuard() noexcept = default;

    void revoke() noexcept { revoked_.store(true, std::memory_order_release); }
    [[nodiscard]] bool revoked() const noexcept {
        return revoked_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> revoked_{false};
};

[[nodiscard]] Result<bool> initialize_crypto();

[[nodiscard]] VaultError validate_argon2_parameters(
    const Argon2Parameters& parameters,
    std::string_view password);
[[nodiscard]] bool is_valid_argon2_parameters(
    const Argon2Parameters& parameters) noexcept;

[[nodiscard]] Result<SensitiveBuffer> derive_master_key(
    std::string_view password,
    std::span<const unsigned char, 16> salt,
    const Argon2Parameters& parameters);

[[nodiscard]] Result<SensitiveBuffer> derive_subkey(
    const SensitiveBuffer& master_key,
    std::uint64_t subkey_id,
    const std::array<char, 8>& context);

void random_bytes(std::span<unsigned char> destination) noexcept;

using Sha256Digest = std::array<unsigned char, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const unsigned char> data);

class Sha256Accumulator final {
public:
    Sha256Accumulator();

    void update(std::span<const unsigned char> data) noexcept;
    [[nodiscard]] Sha256Digest digest() noexcept;

private:
    crypto_hash_sha256_state state_{};
    bool finished_{false};
};

[[nodiscard]] Result<std::vector<unsigned char>> encrypt_xchacha20_poly1305(
    std::span<const unsigned char> plaintext,
    std::span<const unsigned char> associated_data,
    std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key);

[[nodiscard]] Result<std::vector<unsigned char>> decrypt_xchacha20_poly1305(
    std::span<const unsigned char> ciphertext,
    std::span<const unsigned char> associated_data,
    std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key,
    VaultErrorCode authentication_error);

[[nodiscard]] bool constant_time_equal(
    std::span<const unsigned char> left,
    std::span<const unsigned char> right) noexcept;

} // namespace videovault::core::internal
