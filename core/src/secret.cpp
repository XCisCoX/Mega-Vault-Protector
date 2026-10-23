// Public Secret type. The storage is the core's existing SensitiveBuffer
// (libsodium secure memory) rather than a second, parallel abstraction.
#include "videovault/core/secret.hpp"

#include "internal/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace videovault::core {

struct Secret::Storage final {
    internal::SensitiveBuffer bytes;
};

Secret::Secret() noexcept = default;

Secret::~Secret() {
    wipe();
}

Secret::Secret(Secret&& other) noexcept
    : storage_(std::move(other.storage_)), size_(std::exchange(other.size_, 0U)) {}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        wipe();
        storage_ = std::move(other.storage_);
        size_ = std::exchange(other.size_, 0U);
    }
    return *this;
}

std::optional<Secret> Secret::from_utf8(const std::string_view text) {
    if (text.size() > kMaxBytes) {
        return std::nullopt;
    }
    (void)internal::initialize_crypto();
    try {
        Secret secret;
        secret.storage_ = std::make_unique<Storage>();
        secret.storage_->bytes = internal::SensitiveBuffer(
            std::max<std::size_t>(text.size(), 1U));
        secret.size_ = secret.storage_->bytes.assign(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(text.data()), text.size()));
        return secret;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

std::optional<Secret> Secret::from_utf16(const std::span<const char16_t> text) {
    const std::size_t length = internal::utf16_utf8_length(text);
    if (length > kMaxBytes) {
        return std::nullopt;
    }
    (void)internal::initialize_crypto();
    try {
        Secret secret;
        secret.storage_ = std::make_unique<Storage>();
        // Allocated at the exact encoded size: the UTF-8 bytes go straight
        // into secure memory, never into a temporary std::string.
        secret.storage_->bytes = internal::SensitiveBuffer(std::max<std::size_t>(length, 1U));
        secret.size_ = internal::encode_utf16_as_utf8(
            text, std::span<unsigned char>(secret.storage_->bytes.data(), length));
        return secret;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

bool Secret::empty() const noexcept {
    return size_ == 0U;
}

std::size_t Secret::size() const noexcept {
    return size_;
}

std::string_view Secret::view() const noexcept {
    if (size_ == 0U || storage_ == nullptr) {
        return {};
    }
    return std::string_view(
        reinterpret_cast<const char*>(storage_->bytes.data()), size_);
}

bool Secret::memory_locked() const noexcept {
    return storage_ != nullptr && storage_->bytes.memory_locked();
}

void Secret::wipe() noexcept {
    if (storage_ != nullptr) {
        storage_->bytes.wipe();
        storage_.reset(); // SensitiveBuffer release: memzero + munlock + sodium_free
    }
    size_ = 0U;
}

} // namespace videovault::core
