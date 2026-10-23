#include <videovault/extensions/runtime.hpp>
#include <quickjs.h>
#include <memory>

namespace videovault::extensions {
namespace {
struct Value {
    JSContext* ctx;
    JSValue value;
    ~Value() { JS_FreeValue(ctx, value); }
};
struct Budget {
    std::chrono::steady_clock::time_point deadline;
    bool expired = false;
};
int interrupt(JSRuntime*, void* opaque) {
    auto& b = *static_cast<Budget*>(opaque);
    b.expired = b.expired || std::chrono::steady_clock::now() >= b.deadline;
    return b.expired;
}
ExecutionResult failure(const char* error) { return {false, {}, error}; }
JSValue ownProperty(JSContext* c, JSValueConst object, const char* name) {
    if (!JS_IsObject(object)) return JS_UNDEFINED;
    const auto atom = JS_NewAtom(c, name);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    JSPropertyDescriptor descriptor{};
    const int found = JS_GetOwnProperty(c, &descriptor, object, atom);
    JS_FreeAtom(c, atom);
    if (found < 0) return JS_EXCEPTION;
    if (!found) return JS_UNDEFINED;
    JS_FreeValue(c, descriptor.value);
    JS_FreeValue(c, descriptor.getter);
    JS_FreeValue(c, descriptor.setter);
    return JS_GetPropertyStr(c, object, name);
}
JSValue jsonValue(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || JS_IsUndefined(argv[1]) || JS_IsSymbol(argv[1]) ||
        JS_IsFunction(c, argv[1]) || JS_IsBigInt(argv[1]) || JS_IsPromise(argv[1]))
        return JS_ThrowTypeError(c, "Unsupported JSON value");
    return JS_DupValue(c, argv[1]);
}
}
ExecutionResult invoke(std::string_view source, std::string_view command,
                       std::string_view input_json, const Limits& limits) try {
    constexpr std::size_t mib = 1024 * 1024;
    if (limits.heap_bytes < mib || limits.heap_bytes > 64 * mib ||
        limits.stack_bytes < 16 * 1024 || limits.stack_bytes > mib ||
        limits.timeout.count() < 1 || limits.timeout.count() > 5000 ||
        !limits.source_bytes || limits.source_bytes > mib ||
        !limits.input_bytes || limits.input_bytes > 8 * mib ||
        !limits.output_bytes || limits.output_bytes > 8 * mib)
        return failure("Invalid execution limits");
    if (source.size() > limits.source_bytes || input_json.size() > limits.input_bytes)
        return failure("Input size limit exceeded");
    if (command.empty() || command.size() > 64 || command == "constructor" ||
        command == "__proto__" || command == "prototype") return failure("Invalid command identifier");
    if (command.front() < 'a' || command.front() > 'z') return failure("Invalid command identifier");
    for (char ch : command) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
            return failure("Invalid command identifier");
    }
    Budget budget{std::chrono::steady_clock::now() + limits.timeout};
    std::unique_ptr<JSRuntime, decltype(&JS_FreeRuntime)> rt(JS_NewRuntime(), JS_FreeRuntime);
    if (!rt) return failure("Runtime unavailable");
    JS_SetMemoryLimit(rt.get(), limits.heap_bytes);
    JS_SetMaxStackSize(rt.get(), limits.stack_bytes);
    JS_SetCanBlock(rt.get(), false); // Atomics.wait must not bypass interrupts.
    JS_SetInterruptHandler(rt.get(), interrupt, &budget);
    std::unique_ptr<JSContext, decltype(&JS_FreeContext)> ctx(JS_NewContext(rt.get()), JS_FreeContext);
    if (!ctx) return failure("Runtime unavailable");
    auto* c = ctx.get();
    std::string script(source), input(input_json), name(command);
    Value initialized{c, JS_Eval(c, script.c_str(), script.size(), "extension", JS_EVAL_TYPE_GLOBAL)};
    if (JS_IsException(initialized.value)) return failure("Script failed");
    Value global{c, JS_GetGlobalObject(c)};
    Value extension{c, ownProperty(c, global.value, "extension")};
    if (JS_IsException(extension.value)) return failure("Command unavailable");
    Value commands{c, ownProperty(c, extension.value, "commands")};
    if (JS_IsException(commands.value)) return failure("Command unavailable");
    Value function{c, ownProperty(c, commands.value, name.c_str())};
    if (!JS_IsFunction(c, function.value)) return failure("Command unavailable");
    Value argument{c, JS_ParseJSON(c, input.c_str(), input.size(), "input")};
    if (JS_IsException(argument.value)) return failure("Invalid input JSON");
    Value result{c, JS_Call(c, function.value, commands.value, 1, &argument.value)};
    if (JS_IsException(result.value) || JS_IsPromise(result.value)) return failure("Command failed");
    Value replacer{c, JS_NewCFunction(c, jsonValue, "", 2)};
    if (JS_IsException(replacer.value)) return failure("Invalid command result");
    Value json{c, JS_JSONStringify(c, result.value, replacer.value, JS_UNDEFINED)};
    if (JS_IsException(json.value) || JS_IsUndefined(json.value)) return failure("Invalid command result");
    size_t length = 0;
    const char* text = JS_ToCStringLen(c, &length, json.value);
    if (!text) return failure("Invalid command result");
    // Check before copying guest output into host memory.
    if (length > limits.output_bytes || interrupt(rt.get(), &budget)) {
        JS_FreeCString(c, text);
        return failure("Execution limit exceeded");
    }
    std::string output;
    try { output.assign(text, length); }
    catch (...) { JS_FreeCString(c, text); throw; }
    JS_FreeCString(c, text);
    return {true, std::move(output), {}};
} catch (const std::bad_alloc&) {
    return failure("Runtime allocation failed");
}
}
