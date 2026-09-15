// Offline compile/runtime check of the exact getSecret function from main.cpp.
// Windows APIs are test doubles here: no SDK, network, real secrets, or console.
// Standard headers are loaded BEFORE selecting the Windows application branch.
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using HANDLE = void*;
using DWORD = unsigned long;
constexpr DWORD STD_INPUT_HANDLE = static_cast<DWORD>(-10);
constexpr DWORD ENABLE_ECHO_INPUT = 4;
static DWORD consoleMode = ENABLE_ECHO_INPUT;
static int environmentReads = 0, consoleChanges = 0;
static const char* environmentValue = nullptr;
static int environmentError = 0;
static bool consoleAvailable = true;

int testDupenv(char** buffer, std::size_t* length, const char*) {
    ++environmentReads;
    *buffer = nullptr;
    *length = 0;
    if (environmentError) return environmentError;
    if (!environmentValue) return 0;
    *length = std::strlen(environmentValue) + 1;
    *buffer = static_cast<char*>(std::malloc(*length));
    if (!*buffer) throw std::bad_alloc();
    std::memcpy(*buffer, environmentValue, *length);
    return 0;
}
HANDLE GetStdHandle(DWORD) { return nullptr; }
bool GetConsoleMode(HANDLE, DWORD* mode) {
    *mode = consoleMode;
    return consoleAvailable;
}
bool SetConsoleMode(HANDLE, DWORD mode) {
    ++consoleChanges;
    consoleMode = mode;
    return true;
}

#ifndef _WIN32
#define _WIN32
#endif
#define _dupenv_s testDupenv
// The runner extracts this unmodified function from src/main.cpp, not a copy.
#include "get_secret_under_test.inc"
#undef _dupenv_s

void require(bool condition) {
    if (!condition) throw std::runtime_error("Windows credential branch assertion failed");
}
void scenario(const std::string& configured, const char* environment, const std::string& input,
              const std::string& expected, const std::string& expectedError = {},
              int readError = 0, bool available = true) {
    environmentValue = environment;
    environmentError = readError;
    environmentReads = consoleChanges = 0;
    consoleMode = ENABLE_ECHO_INPUT;
    consoleAvailable = available;
    std::istringstream fakeInput(input);
    std::ostringstream fakeOutput;
    auto* savedInput = std::cin.rdbuf(fakeInput.rdbuf());
    auto* savedOutput = std::cout.rdbuf(fakeOutput.rdbuf());
    std::cin.clear();
    std::string result, error;
    try { result = getSecret(configured, "OFFLINE_ENV", "OFFLINE_PROMPT: "); }
    catch (const std::exception& failure) { error = failure.what(); }
    std::cin.rdbuf(savedInput);
    std::cin.clear();
    std::cout.rdbuf(savedOutput);
    require(expectedError.empty() ? error.empty() : error.find(expectedError) != std::string::npos);
    if (expectedError.empty()) require(result == expected);
    require(consoleMode == ENABLE_ECHO_INPUT);
    require(fakeOutput.str().find("Dummy") == std::string::npos);
    require(error.find("Dummy") == std::string::npos);
    require(environmentReads == (configured.empty() ? 1 : 0));
    const bool usesPrompt = configured.empty() && !readError && (!environment || !*environment);
    require(consoleChanges == (usesPrompt && available ? 2 : 0));
    require(usesPrompt || fakeOutput.str().empty());
}

int main() {
    try {
        scenario("ConfiguredDummy", "EnvironmentDummy", "", "ConfiguredDummy");
        scenario("", "EnvironmentDummy", "", "EnvironmentDummy");
        scenario("", nullptr, "PromptDummy\n", "PromptDummy");
        scenario("", "", "PromptDummy\n", "PromptDummy");
        scenario("", nullptr, "\n", "", "must not be empty");
        scenario("", nullptr, "", "", "cancelled");
        scenario("", nullptr, "", "", "Cannot read environment variable", 12);
        scenario("", nullptr, "", "", "interactive console", 0, false);
        std::cout << "PASS 8 Windows getSecret branch checks (test doubles, not native Windows validation)\n";
        return 0;
    } catch (...) {
        std::cerr << "FAIL Windows credential branch checks\n";
        return 1;
    }
}
