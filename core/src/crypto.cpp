#include "internal/crypto.hpp"

#include <argon2.h>
#include <sodium.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace videovault::core::internal {
namespace {

constexpr std::size_t kMasterKeyBytes = 32U;
constexpr std::uint32_t kMinimumMemoryKiB = 8U * 1024U;
constexpr std::uint32_t kMaximumMemoryKiB = 1024U * 1024U;
constexpr std::uint32_t kMaximumIterations = 20U;
constexpr std::uint32_t kMaximumParallelism = 16U;
constexpr std::size_t kMaximumPasswordBytes = 4096U;

std::once_flag g_sodium_once;
int g_sodium_status = -1;
bool g_secure_memory_supported = false;

VaultError crypto_error(std::string detail) {
    return {VaultErrorCode::CryptoFailure, std::move(detail)};
}

// Probes mlock() once at initialization so the UI can report when the OS
// refuses to lock secret pages (RLIMIT_MEMLOCK, hardened kernels, Android
// vendor restrictions). Sodium's own allocator falls back to an unlocked
// heap allocation in that case; the bytes are still wiped on free.
bool probe_secure_memory() noexcept {
    void* probe = sodium_malloc(64U);
    if (probe == nullptr) {
        return false;
    }
    const bool locked = sodium_mlock(probe, 64U) == 0;
    if (locked) {
        (void)sodium_munlock(probe, 64U);
    }
    sodium_free(probe);
    return locked;
}

} // namespace

// Encodes UTF-16 code units into UTF-8 bytes.
[[nodiscard]] std::size_t utf16_utf8_length(const std::span<const char16_t> text) noexcept {
    const char16_t* units = text.data();
    const std::size_t count = text.size();
    std::size_t bytes = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        std::uint32_t code = units[i];
        if (code >= 0xD800U && code <= 0xDBFFU && i + 1U < count
            && units[i + 1U] >= 0xDC00U && units[i + 1U] <= 0xDFFFU) {
            code = 0x10000U + ((code - 0xD800U) << 10U)
                + (static_cast<std::uint32_t>(units[i + 1U]) - 0xDC00U);
            ++i;
        } else if (code >= 0xD800U && code <= 0xDFFFU) {
            code = 0xFFFDU; // lone surrogate
        }
        bytes += code < 0x80U ? 1U : (code < 0x800U ? 2U : (code < 0x10000U ? 3U : 4U));
    }
    return bytes;
}

std::size_t encode_utf16_as_utf8(
    const std::span<const char16_t> text,
    const std::span<unsigned char> destination) noexcept {
    const char16_t* units = text.data();
    const std::size_t count = text.size();
    unsigned char* out = destination.data();
    std::size_t written = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        std::uint32_t code = units[i];
        if (code >= 0xD800U && code <= 0xDBFFU && i + 1U < count
            && units[i + 1U] >= 0xDC00U && units[i + 1U] <= 0xDFFFU) {
            code = 0x10000U + ((code - 0xD800U) << 10U)
                + (static_cast<std::uint32_t>(units[i + 1U]) - 0xDC00U);
            ++i;
        } else if (code >= 0xD800U && code <= 0xDFFFU) {
            code = 0xFFFDU;
        }
        // How many bytes this code point needs, checked exactly: a blanket
        // "room for 4" test truncates any password whose remaining slack is
        // smaller than that (that bug ate the tail of most passwords).
        const std::size_t needed = code < 0x80U ? 1U
            : (code < 0x800U ? 2U : (code < 0x10000U ? 3U : 4U));
        if (written + needed > destination.size()) {
            break; // never write past the caller's buffer
        }
        if (code < 0x80U) {
            out[written++] = static_cast<unsigned char>(code);
        } else if (code < 0x800U) {
            out[written++] = static_cast<unsigned char>(0xC0U | (code >> 6U));
            out[written++] = static_cast<unsigned char>(0x80U | (code & 0x3FU));
        } else if (code < 0x10000U) {
            out[written++] = static_cast<unsigned char>(0xE0U | (code >> 12U));
            out[written++] = static_cast<unsigned char>(0x80U | ((code >> 6U) & 0x3FU));
            out[written++] = static_cast<unsigned char>(0x80U | (code & 0x3FU));
        } else {
            out[written++] = static_cast<unsigned char>(0xF0U | (code >> 18U));
            out[written++] = static_cast<unsigned char>(0x80U | ((code >> 12U) & 0x3FU));
            out[written++] = static_cast<unsigned char>(0x80U | ((code >> 6U) & 0x3FU));
            out[written++] = static_cast<unsigned char>(0x80U | (code & 0x3FU));
        }
    }
    return written;
}


SensitiveBuffer::SensitiveBuffer(const std::size_t size) : size_(size) {
    if (size == 0U) {
        return;
    }
    data_ = static_cast<unsigned char*>(sodium_malloc(size));
    if (data_ == nullptr) {
        size_ = 0U;
        throw std::bad_alloc{};
    }
    locked_ = sodium_mlock(data_, size_) == 0;
    sodium_memzero(data_, size_);
}

SensitiveBuffer::~SensitiveBuffer() {
    release();
}

SensitiveBuffer::SensitiveBuffer(SensitiveBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0U)),
      locked_(std::exchange(other.locked_, false)) {}

SensitiveBuffer& SensitiveBuffer::operator=(SensitiveBuffer&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0U);
        locked_ = std::exchange(other.locked_, false);
    }
    return *this;
}

void SensitiveBuffer::wipe() noexcept {
    if (data_ != nullptr) {
        sodium_memzero(data_, size_);
    }
}

std::size_t SensitiveBuffer::assign(const std::span<const unsigned char> source) noexcept {
    if (data_ == nullptr) {
        return 0U;
    }
    const std::size_t copied = std::min(size_, source.size());
    if (copied > 0U) {
        std::copy_n(source.begin(), copied, data_);
    }
    if (copied < size_) {
        sodium_memzero(data_ + copied, size_ - copied);
    }
    return copied;
}

void SensitiveBuffer::release() noexcept {
    if (data_ == nullptr) {
        return;
    }
    sodium_memzero(data_, size_);
    if (locked_) {
        (void)sodium_munlock(data_, size_);
    }
    sodium_free(data_);
    data_ = nullptr;
    size_ = 0U;
    locked_ = false;
}

bool secure_memory_supported() noexcept {
    return g_secure_memory_supported;
}

void wipe_bytes(const std::span<unsigned char> bytes) noexcept {
    if (!bytes.empty()) {
        sodium_memzero(bytes.data(), bytes.size());
    }
}

Result<bool> initialize_crypto() {
    std::call_once(g_sodium_once, [] {
        g_sodium_status = sodium_init();
        if (g_sodium_status >= 0) {
            g_secure_memory_supported = probe_secure_memory();
        }
    });
    if (g_sodium_status < 0) {
        return crypto_error("libsodium initialization failed");
    }
    return true;
}


bool is_valid_argon2_parameters(const Argon2Parameters& parameters) noexcept {
    return parameters.memory_kib >= kMinimumMemoryKiB
        && parameters.memory_kib <= kMaximumMemoryKiB
        && parameters.iterations >= 1U
        && parameters.iterations <= kMaximumIterations
        && parameters.parallelism >= 1U
        && parameters.parallelism <= kMaximumParallelism;
}

VaultError validate_argon2_parameters(
    const Argon2Parameters& parameters,
    const std::string_view password) {
    if (password.empty()) {
        return {VaultErrorCode::InvalidArgument, "password must not be empty"};
    }
    if (password.size() > kMaximumPasswordBytes) {
        return {VaultErrorCode::InvalidArgument, "password exceeds the supported byte length"};
    }
    if (!is_valid_argon2_parameters(parameters)) {
        return {VaultErrorCode::InvalidArgument, "Argon2id parameters are outside supported safety bounds"};
    }
    return {VaultErrorCode::InvalidArgument, {}};
}

Result<SensitiveBuffer> derive_master_key(
    const std::string_view password,
    const std::span<const unsigned char, 16> salt,
    const Argon2Parameters& parameters) {
    const auto validation = validate_argon2_parameters(parameters, password);
    if (!validation.technical_detail.empty()) {
        return validation;
    }

    try {
        SensitiveBuffer output(kMasterKeyBytes);
        const int status = argon2id_hash_raw(
            parameters.iterations,
            parameters.memory_kib,
            parameters.parallelism,
            password.data(),
            password.size(),
            salt.data(),
            salt.size(),
            output.data(),
            output.size());
        if (status != ARGON2_OK) {
            return crypto_error(std::string("Argon2id failed: ") + argon2_error_message(status));
        }
        return std::move(output);
    } catch (const std::bad_alloc&) {
        return crypto_error("secure memory allocation failed during Argon2id");
    }
}

Result<SensitiveBuffer> derive_subkey(
    const SensitiveBuffer& master_key,
    const std::uint64_t subkey_id,
    const std::array<char, 8>& context) {
    if (master_key.size() != crypto_kdf_KEYBYTES) {
        return crypto_error("invalid master key length for domain-separated derivation");
    }
    try {
        SensitiveBuffer output(32U);
        if (crypto_kdf_derive_from_key(
                output.data(), output.size(), subkey_id, context.data(), master_key.data()) != 0) {
            return crypto_error("domain-separated key derivation failed");
        }
        return std::move(output);
    } catch (const std::bad_alloc&) {
        return crypto_error("secure memory allocation failed during subkey derivation");
    }
}

void random_bytes(const std::span<unsigned char> destination) noexcept {
    randombytes_buf(destination.data(), destination.size());
}

Sha256Digest sha256(const std::span<const unsigned char> data) {
    Sha256Digest digest{};
    crypto_hash_sha256(digest.data(), data.data(), data.size());
    return digest;
}

Sha256Accumulator::Sha256Accumulator() {
    crypto_hash_sha256_init(&state_);
}

void Sha256Accumulator::update(const std::span<const unsigned char> data) noexcept {
    if (!finished_ && !data.empty()) {
        crypto_hash_sha256_update(&state_, data.data(), data.size());
    }
}

Sha256Digest Sha256Accumulator::digest() noexcept {
    Sha256Digest digest{};
    if (!finished_) {
        crypto_hash_sha256_final(&state_, digest.data());
        finished_ = true;
    }
    return digest;
}

Result<std::vector<unsigned char>> encrypt_xchacha20_poly1305(
    const std::span<const unsigned char> plaintext,
    const std::span<const unsigned char> associated_data,
    const std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        return crypto_error("invalid XChaCha20-Poly1305 key length");
    }

    std::vector<unsigned char> ciphertext(
        plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long ciphertext_size = 0U;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            ciphertext.data(), &ciphertext_size,
            plaintext.data(), plaintext.size(),
            associated_data.data(), associated_data.size(),
            nullptr, nonce.data(), key.data()) != 0) {
        sodium_memzero(ciphertext.data(), ciphertext.size());
        return crypto_error("XChaCha20-Poly1305 encryption failed");
    }
    ciphertext.resize(static_cast<std::size_t>(ciphertext_size));
    return ciphertext;
}

Result<std::vector<unsigned char>> decrypt_xchacha20_poly1305(
    const std::span<const unsigned char> ciphertext,
    const std::span<const unsigned char> associated_data,
    const std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key,
    const VaultErrorCode authentication_error) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return VaultError{authentication_error, "invalid authenticated record"};
    }

    std::vector<unsigned char> plaintext(
        ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long plaintext_size = 0U;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext.data(), &plaintext_size, nullptr,
            ciphertext.data(), ciphertext.size(),
            associated_data.data(), associated_data.size(),
            nonce.data(), key.data()) != 0) {
        sodium_memzero(plaintext.data(), plaintext.size());
        return VaultError{authentication_error, "authenticated password-verification record rejected"};
    }
    plaintext.resize(static_cast<std::size_t>(plaintext_size));
    return plaintext;
}

bool constant_time_equal(
    const std::span<const unsigned char> left,
    const std::span<const unsigned char> right) noexcept {
    return left.size() == right.size()
        && sodium_memcmp(left.data(), right.data(), left.size()) == 0;
}

} // namespace videovault::core::internal
