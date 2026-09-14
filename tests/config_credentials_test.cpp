// Offline configuration regression tests. Never creates an SDK object or connects.
// The runner removes the unused application entry point at link time.
#define main ctp_connectivity_application_main
#include "../src/main.cpp"
#undef main
#include <functional>

namespace {
int passed = 0;
fs::path testRoot;
constexpr const char* testEnv = "CTPSTOCK_CONFIG_TEST_SECRET";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
    if (!file) throw std::runtime_error("Cannot write temporary test configuration");
}
fs::path configFile(const std::string& name, const std::string& content) {
    const fs::path path = testRoot / name / "connection.ini";
    writeFile(path, "[connection]\nuser_id=OFFLINE_TEST_USER\n" + content);
    return path;
}
void setTestEnv(const std::string& value) {
#ifdef _WIN32
    require(_putenv_s(testEnv, value.c_str()) == 0, "Cannot set test environment");
#else
    const int result = value.empty() ? unsetenv(testEnv) : setenv(testEnv, value.c_str(), 1);
    require(result == 0, "Cannot set test environment");
#endif
}
template <typename F> void rejects(F action, const char* reason, const std::string& secret = {}) {
    try { action(); }
    catch (const std::exception& error) {
        const std::string message = error.what();
        require(message.find(reason) != std::string::npos, "Unexpected rejection reason");
        require(secret.empty() || message.find(secret) == std::string::npos,
                "Error exposed a credential value");
        return;
    }
    throw std::runtime_error("Expected rejection did not occur");
}
void test(const char* name, const std::function<void()>& body) {
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
    } catch (...) {
        std::cerr << "FAIL " << name << '\n';
        throw;
    }
}
}

int main(int argc, char** argv) {
    // Used by the runner's POSIX pseudo-terminal check. Test values are synthetic.
    if (argc == 2 && std::string(argv[1]) == "--prompt") {
        setTestEnv("");
        const auto value = getSecret("", testEnv, "OFFLINE_PROMPT: ");
        require(value == "DummyPrompt#;=42", "Prompt input changed");
        std::cout << "PASS hidden prompt input\n";
        return 0;
    }
    if (argc != 2) return 2;
    testRoot = argv[1];
    try {
        test("legacy configuration without credentials", [] {
            Secrets secret;
            const auto c = readConfig(configFile("legacy", "app_id=offline_app\n").string(), secret);
            require(c.user == "OFFLINE_TEST_USER" && c.investor == c.user && c.app == "offline_app",
                    "Connection fields or investor fallback changed");
            require(secret.password.empty() && secret.auth.empty(), "Legacy credentials not empty");
        });
        test("main configuration credentials", [] {
            Secrets secret;
            readConfig(configFile("main", "password=MainDummy42\nauth_code=MainDummyAuth\n").string(), secret);
            require(secret.password == "MainDummy42" && secret.auth == "MainDummyAuth", "Main credentials missing");
        });
        test("BOM credentials section and literal symbols", [] {
            const auto path = testRoot / "symbols" / "custom.ini";
            writeFile(path, "\xEF\xBB\xBF[connection]\r\nuser_id=OFFLINE_TEST_USER\r\n"
                            "[credentials]\r\npassword=Dummy#;=42\r\nauth_code=#Dummy;=Auth\r\n");
            Secrets secret;
            readConfig(path.string(), secret);
            require(secret.password == "Dummy#;=42" && secret.auth == "#Dummy;=Auth", "Symbols or BOM mishandled");
        });
        test("local credentials and connection fields override main", [] {
            const auto path = configFile("local", "password=MainDummy42\nauth_code=MainDummyAuth\n");
            writeFile(path.parent_path() / "connection.local.ini",
                      "[connection]\nuser_id=LOCAL_TEST_USER\n[credentials]\npassword=LocalDummy42\nauth_code=LocalDummyAuth\n");
            Secrets secret;
            const auto c = readConfig(path.string(), secret);
            require(secret.password == "LocalDummy42" && secret.auth == "LocalDummyAuth", "Local override missing");
            require(c.user == "LOCAL_TEST_USER" && c.investor == c.user, "Connection override missing");
        });
        test("blank local values retain each nonempty main credential", [] {
            const auto path = configFile("blank-local", "password=MainDummy42\nauth_code=MainDummyAuth\n");
            writeFile(path.parent_path() / "connection.local.ini", "password= \nauth_code=\n");
            Secrets secret;
            readConfig(path.string(), secret);
            require(secret.password == "MainDummy42" && secret.auth == "MainDummyAuth", "Blank local value erased main");
        });
        test("password and auth override independently", [] {
            const auto path = configFile("partial-local", "password=MainDummy42\nauth_code=MainDummyAuth\n");
            writeFile(path.parent_path() / "connection.local.ini", "auth_code=LocalDummyAuth\n");
            Secrets secret;
            readConfig(path.string(), secret);
            require(secret.password == "MainDummy42" && secret.auth == "LocalDummyAuth", "Partial override lost credential");
        });
        test("explicit local file never chains another local overlay", [] {
            const auto path = testRoot / "explicit" / "custom.local.ini";
            writeFile(path, "password=ExplicitDummy42\n");
            writeFile(path.parent_path() / "custom.local.local.ini", "password=UnexpectedDummy42\n");
            Secrets secret;
            readConfig(path.string(), secret);
            require(secret.password == "ExplicitDummy42", "Explicit local config unexpectedly chained");
        });
        test("custom config basename selects sibling overlay", [] {
            const auto path = testRoot / "custom" / "evaluation.ini";
            writeFile(path, "password=MainDummy42\n");
            writeFile(path.parent_path() / "evaluation.local.ini", "password=CustomDummy42\n");
            Secrets secret;
            readConfig(path.string(), secret);
            require(secret.password == "CustomDummy42", "Custom overlay not selected");
        });
        test("duplicate credential in one file rejected without value", [] {
            const auto path = configFile("duplicate", "password=DummyPrivate42\n[credentials]\npassword=OtherDummy42\n");
            Secrets secret;
            rejects([&] { readConfig(path.string(), secret); }, "Duplicate INI key", "DummyPrivate42");
        });
        test("duplicate local credential rejected without value", [] {
            const auto path = configFile("duplicate-local", "");
            writeFile(path.parent_path() / "connection.local.ini", "auth_code=DummyPrivateAuth\nauth_code=OtherDummyAuth\n");
            Secrets secret;
            rejects([&] { readConfig(path.string(), secret); }, "Duplicate INI key", "DummyPrivateAuth");
        });
        test("malformed credential line rejected without value", [] {
            const auto path = configFile("malformed", "password DummyPrivate42\n");
            Secrets secret;
            rejects([&] { readConfig(path.string(), secret); }, "Invalid INI syntax", "DummyPrivate42");
        });
        test("unknown key rejected without value", [] {
            const auto path = configFile("unknown", "password_typo=DummyPrivate42\n");
            Secrets secret;
            rejects([&] { readConfig(path.string(), secret); }, "Unsupported INI key", "DummyPrivate42");
        });
        test("missing main config rejected", [] {
            Secrets secret;
            rejects([&] { readConfig((testRoot / "missing.ini").string(), secret); }, "Cannot open configuration");
        });
        test("local overlay still obeys evaluation front restriction", [] {
            const auto path = configFile("front", "");
            writeFile(path.parent_path() / "connection.local.ini", "trader_front=tcp://127.0.0.1:1\n");
            Secrets secret;
            rejects([&] { readConfig(path.string(), secret); }, "Phase 1 permits only");
        });
        test("configured value beats environment with no prompt", [] {
            setTestEnv("EnvironmentDummy42");
            std::ostringstream output;
            auto* old = std::cout.rdbuf(output.rdbuf());
            const auto value = getSecret("ConfiguredDummy42", testEnv, "UNEXPECTED_PROMPT");
            std::cout.rdbuf(old);
            require(value == "ConfiguredDummy42" && output.str().empty(), "Configured precedence or prompt failed");
        });
        test("blank config falls back to environment with no prompt", [] {
            setTestEnv("EnvironmentDummy42");
            std::ostringstream output;
            auto* old = std::cout.rdbuf(output.rdbuf());
            const auto value = getSecret("", testEnv, "UNEXPECTED_PROMPT");
            std::cout.rdbuf(old);
            require(value == "EnvironmentDummy42" && output.str().empty(), "Environment fallback or prompt failed");
        });
        test("redaction removes both credentials and control characters", [] {
            Secrets secret;
            secret.password = "Dummy#;=42";
            secret.auth = "DummyAuth42";
            const auto output = clean("bad " + secret.password + "\n" + secret.auth + "\t" + secret.password, secret);
            require(output == "bad [REDACTED] [REDACTED] [REDACTED]", "Credential or control character leaked");
        });
        test("SDK credential field lengths fail without values", [] {
            CThostFtdcReqUserLoginField login{};
            CThostFtdcReqAuthenticateField auth{};
            const std::string password(sizeof(login.Password), 'P');
            const std::string authCode(sizeof(auth.AuthCode), 'A');
            rejects([&] { field(login.Password, password, "Password"); }, "overlong field", password);
            rejects([&] { field(auth.AuthCode, authCode, "AuthCode"); }, "overlong field", authCode);
            field(login.Password, std::string(sizeof(login.Password) - 1, 'P'), "Password");
            require(login.Password[sizeof(login.Password) - 1] == '\0', "Password maximum length not terminated");
        });
        test("credential control characters rejected without values", [] {
            CThostFtdcReqUserLoginField login{};
            rejects([&] { field(login.Password, "Dummy\t42", "Password"); }, "Invalid", "Dummy");
            rejects([&] { field(login.Password, std::string("Dummy\0" "42", 8), "Password"); }, "Invalid", "Dummy");
        });
        setTestEnv("");
        std::cout << passed << " offline configuration tests passed; no SDK connection was attempted.\n";
        return 0;
    } catch (...) {
        // Deliberately omit exception payloads to keep regression output secret-free.
        return 1;
    }
}
