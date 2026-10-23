#include "videovault/core/version.hpp"

namespace videovault::core {

#ifndef MVP_VERSION_STRING
#define MVP_VERSION_STRING "2.1.0"
#endif

std::string_view version() noexcept {
    return MVP_VERSION_STRING;
}

bool is_crypto_available() noexcept {
    return true;
}

} // namespace videovault::core
