#include <videovault/extensions/runtime.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

// Development-only host file access: guest JavaScript never receives a file API.
int main(int argc, char** argv) {
    using namespace videovault::extensions;
    if (argc != 4) {
        std::cerr << "Usage: MegaVaultExtensionRunner <script-file> <command> <input-json>\n";
        return 2;
    }
    const Limits limits;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) { std::cerr << "Cannot read script\n"; return 2; }
    std::string source(limits.source_bytes + 1, '\0');
    file.read(source.data(), static_cast<std::streamsize>(source.size()));
    source.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || source.size() > limits.source_bytes) {
        std::cerr << "Script read failed or size limit exceeded\n"; return 2;
    }
    auto bounded = [](const char* text, std::size_t maximum) {
        std::size_t length = 0;
        while (length <= maximum && text[length]) ++length;
        return std::string_view(text, length);
    };
    auto result = invoke(source, bounded(argv[2], 64), bounded(argv[3], limits.input_bytes), limits);
    if (!result.ok) { std::cerr << result.error << '\n'; return 1; }
    std::cout << result.json << '\n';
    return 0;
}
