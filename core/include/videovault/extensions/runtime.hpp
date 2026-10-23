#pragma once
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace videovault::extensions {
struct Limits {
    std::size_t heap_bytes = 8 * 1024 * 1024;
    std::size_t stack_bytes = 256 * 1024;
    std::chrono::milliseconds timeout{100};
    std::size_t source_bytes = 256 * 1024;
    std::size_t input_bytes = 1024 * 1024;
    std::size_t output_bytes = 1024 * 1024;
};
struct ExecutionResult {
    bool ok;
    std::string json;
    std::string error;
};
// Fresh, synchronous JSON-only invocation. Run off the UI thread. No host
// capabilities are exposed. This is NOT an OS/process security boundary against
// engine vulnerabilities. Errors never contain guest source or input payloads.
ExecutionResult invoke(std::string_view source, std::string_view command,
                       std::string_view input_json, const Limits& limits = {});
}
