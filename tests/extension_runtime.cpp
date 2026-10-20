#include <videovault/extensions/runtime.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <vector>
using namespace videovault::extensions;
namespace {
int failures = 0;
int checks = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
std::string script(std::string_view body) {
    return "globalThis.extension={commands:{run:function(input){" + std::string(body) + "}}};";
}
void reject(std::string_view source, const char* name, const Limits& limits = {}) {
    auto r = invoke(source, "run", "null", limits);
    check(!r.ok && r.json.empty() && !r.error.empty(), name);
}
}
int main() {
    auto r = invoke(script("return {title:'Upper',text:input.text.toUpperCase()};"), "run", R"({"text":"hello"})");
    check(r.ok && r.json == R"({"title":"Upper","text":"HELLO"})", "basic transform");
    check(!invoke("extension={commands:{}}", "constructor", "null").ok, "constructor denial");
    check(!invoke("extension={commands:{}}", "__proto__", "null").ok, "prototype denial");
    check(!invoke("extension={commands:Object.create({run:()=>42})}", "run", "null").ok, "own commands only");
    reject(script("return Promise.resolve(42);"), "promise refusal");
    reject(script("return undefined;"), "undefined refusal");
    reject(script("const a={};a.self=a;return a;"), "cycle refusal");
    Limits small;
    small.source_bytes = 10;
    reject(script("return 1;"), "source limit", small);
    small = {}; small.input_bytes = 2;
    check(!invoke(script("return input;"), "run", "null", small).ok, "input limit");
    small = {}; small.output_bytes = 10;
    reject(script("return 'abcdefghijklmn';"), "output limit", small);
    small = {}; small.timeout = std::chrono::milliseconds(0);
    reject(script("return 1;"), "zero timeout invalid", small);
    small = {}; small.heap_bytes = 0;
    reject(script("return 1;"), "zero heap invalid", small);
    small = {}; small.stack_bytes = 0;
    reject(script("return 1;"), "zero stack invalid", small);
    r = invoke(script("return input;"), "run", R"({"unicode":"\u06cc\u0627\u062f\u062f\u0627\u0634\u062a \ud83c\udfb5","items":[1,true,null]})");
    check(r.ok && r.json.find("items") != std::string::npos, "unicode nested JSON");
    reject("this is not javascript !!!", "malformed source");
    check(!invoke(script("return 1;"), "run", "{broken}").ok, "malformed input");
    check(!invoke(script("return 1;"), "missing", "null").ok, "missing command");
    reject("", "missing extension");
    reject("extension=null", "null extension");
    reject("extension={commands:42}", "primitive commands");
    check(!invoke(script("return 1;"), std::string(65, 'a'), "null").ok, "long command");
    check(!invoke(script("return 1;"), "run.x", "null").ok, "path traversal command");
    check(!invoke(script("return 1;"), std::string("run\0x", 5), "null").ok, "embedded NUL command");
    r = invoke(script("return [typeof std,typeof os,typeof fetch,typeof require,typeof process,typeof XMLHttpRequest,typeof console,typeof vault,typeof readFile,typeof WebSocket];"), "run", "null");
    check(r.ok && r.json == R"(["undefined","undefined","undefined","undefined","undefined","undefined","undefined","undefined","undefined","undefined"])", "no ambient IO or vault capabilities");
    reject("import * as fs from 'std';", "no static module access");
    reject(script("return import('os');"), "no dynamic module access");
    reject(script("return {nested:Promise.resolve(1)};"), "nested promise refusal");
    reject(script("return {nested:undefined};"), "nested undefined refusal");
    reject(script("return ()=>1;"), "function result refusal");
    reject(script("return 1n;"), "bigint result refusal");
    reject(script("return Symbol('x');"), "symbol result refusal");
    auto secret = invoke(script("throw new Error('PRIVATE_PAYLOAD');"), "run", "null");
    check(!secret.ok && secret.error.find("PRIVATE_PAYLOAD") == std::string::npos, "no guest error disclosure");
    small = {}; small.timeout = std::chrono::milliseconds(30);
    const auto started = std::chrono::steady_clock::now();
    reject("while(true){}", "initialization timeout", small);
    reject(script("while(true){}"), "command timeout", small);
    reject(script("return {toJSON(){while(true){}}};"), "toJSON timeout", small);
    reject(script("return {get text(){while(true){}}};"), "output getter timeout", small);
    reject("extension={commands:{get run(){while(true){}}}}", "command getter timeout", small);
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(3), "timeouts complete promptly");
    small = {}; small.heap_bytes = 2 * 1024 * 1024; small.timeout = std::chrono::milliseconds(1000);
    const auto allocationStarted = std::chrono::steady_clock::now();
    reject(script("const a=[];while(true)a.push(new Array(10000).fill(42));"), "heap allocation cap", small);
    check(std::chrono::steady_clock::now() - allocationStarted < std::chrono::milliseconds(800), "heap fails before timeout");
    reject(script("return Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0);"), "Atomics cannot block");
    reject(script("return new Uint8Array(64*1024*1024);"), "typed array heap cap");
    reject(script("return new SharedArrayBuffer(64*1024*1024);"), "shared buffer heap cap");
    reject(script("function recurse(){return recurse()+1;}return recurse();"), "stack recursion cap");
    for (int i=0; i<10; ++i) {
        r = invoke(script("globalThis.counter=(globalThis.counter||0)+1;return counter;"), "run", "null");
        check(r.ok && r.json == "1", "fresh isolated runtime");
    }
    std::vector<std::future<bool>> concurrent;
    for (int i=0; i<8; ++i) concurrent.push_back(std::async(std::launch::async, [] {
        auto result = invoke(script("return input.value*2;"), "run", R"({"value":21})");
        return result.ok && result.json == "42";
    }));
    for (auto& task : concurrent) check(task.get(), "concurrent isolated runtime");
    std::cout << "ExtensionRuntime: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
