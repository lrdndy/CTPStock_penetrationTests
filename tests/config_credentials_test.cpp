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
Options parse(std::initializer_list<const char*> arguments) {
    std::vector<std::string> values{"ctp_test"};
    for (const char* argument : arguments) values.emplace_back(argument);
    std::vector<char*> pointers;
    for (auto& value : values) pointers.push_back(value.data());
    return parseOptions(static_cast<int>(pointers.size()), pointers.data());
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
            rejects([&] { readConfig(path.string(), secret); }, "permits only");
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
        test("MD login accepts callback request ID zero", [] {
            State state;
            CThostFtdcRspUserLoginField login{};
            CThostFtdcRspInfoField info{};
            Secrets secrets;
            const fs::path logPath = testRoot / "md-zero-id" / "run.log";
            fs::create_directories(logPath.parent_path());
            Logger logger(logPath, secrets);
            MdSpi spi(state, logger);
            field(login.UserID, "MD_ZERO_ID", "UserID");
            require(state.begin(Stage::Login, 1, 2), "Cannot begin MD login state");
            spi.OnRspUserLogin(&login, &info, 0, true);
            const Result result = state.wait();
            require(result.ok, "MD login callback request ID zero was rejected");
            require(result.acceptedZeroRequestId, "MD zero-ID compatibility was not recorded");
            require(textField(result.login.UserID) == "MD_ZERO_ID", "MD login payload was not retained");
            std::ifstream file(logPath, std::ios::binary);
            std::ostringstream content;
            content << file.rdbuf();
            require(content.str().find("MD CALLBACK OnRspUserLogin callback_request_id=0") != std::string::npos,
                    "Raw MD callback request ID was not logged before matching");
        });
        test("strict request matching still rejects callback ID zero", [] {
            State state;
            CThostFtdcRspUserLoginField wrong{}, correct{};
            CThostFtdcRspInfoField info{};
            field(wrong.UserID, "WRONG_ZERO", "UserID");
            field(correct.UserID, "STRICT_OK", "UserID");
            require(state.begin(Stage::Login, 2, 2), "Cannot begin strict login state");
            state.response(Stage::Login, &wrong, &info, 0, true);
            state.response(Stage::Login, &correct, &info, 2, true);
            const Result result = state.wait();
            require(result.ok, "Strict matching rejected the correct callback ID");
            require(!result.acceptedZeroRequestId, "Strict matching incorrectly accepted callback ID zero");
            require(textField(result.login.UserID) == "STRICT_OK", "Strict matching retained wrong response");
        });
        test("MD error callback accepts request ID zero", [] {
            State state;
            CThostFtdcRspInfoField info{};
            info.ErrorID = 77;
            require(state.begin(Stage::Login, 1, 2), "Cannot begin MD error state");
            state.error(&info, 0, true, RequestIdPolicy::AllowZero);
            const Result result = state.wait();
            require(!result.ok && result.hasInfo && result.info.ErrorID == 77,
                    "MD zero-ID error response was not retained");
            require(result.acceptedZeroRequestId, "MD zero-ID error compatibility was not recorded");
        });
        test("basic test is dry-run unless send flag and confirmation are both present", [] {
            const auto dry = parse({"--mode", "trader", "--test", "basic", "--instrument", "DUMMY_OPT",
                                    "--exchange", "SSE", "--direction", "buy", "--offset", "open",
                                    "--price", "0.0123"});
            require(!dry.sendOrder, "Basic test unexpectedly enabled live transmission");
            rejects([] {
                (void)parse({"--mode", "trader", "--test", "basic", "--instrument", "DUMMY_OPT",
                             "--exchange", "SSE", "--direction", "buy", "--offset", "open",
                             "--price", "0.0123", "--send-order"});
            }, "requires --confirm");
            const auto live = parse({"--mode", "trader", "--test", "basic", "--instrument", "DUMMY_OPT",
                                     "--exchange", "SZSE", "--direction", "sell", "--offset", "close",
                                     "--price", "0.0123", "--send-order", "--confirm", "SEND_ONE_ORDER"});
            require(live.sendOrder, "Explicitly confirmed live test was not enabled");
        });
        test("basic strategy constructs volume-one GFD speculative limit order", [] {
            Config config;
            config.investor = config.user;
            Options options;
            options.test = "basic"; options.instrument = "DUMMY_OPT"; options.exchange = "SSE";
            options.direction = "buy"; options.offset = "open"; options.price = 0.0123;
            const auto order = basicOrderRequest(config, options, "7");
            require(textField(order.InstrumentID) == "DUMMY_OPT" && textField(order.ExchangeID) == "SSE",
                    "Basic strategy lost instrument or exchange");
            require(textField(order.OrderRef) == "7" && order.OrderPriceType == THOST_FTDC_OPT_LimitPrice,
                    "Basic strategy lost order reference or limit type");
            require(order.Direction == THOST_FTDC_D_Buy && order.CombOffsetFlag[0] == THOST_FTDC_OF_Open,
                    "Basic strategy direction or offset changed");
            require(order.CombHedgeFlag[0] == THOST_FTDC_HF_Speculation && order.VolumeTotalOriginal == 1,
                    "Basic strategy hedge flag or volume-one cap changed");
            require(order.TimeCondition == THOST_FTDC_TC_GFD && order.VolumeCondition == THOST_FTDC_VC_AV &&
                    order.MinVolume == 1 && order.ContingentCondition == THOST_FTDC_CC_Immediately,
                    "Basic strategy execution conditions changed");
        });
        test("order lifecycle waits for queueing then verifies program cancellation", [] {
            OrderLifecycle lifecycle;
            lifecycle.start("8");
            CThostFtdcOrderField order{};
            field(order.OrderRef, "8", "order_ref");
            field(order.InstrumentID, "DUMMY_OPT", "instrument");
            field(order.ExchangeID, "SSE", "exchange");
            order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
            order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            lifecycle.returnedOrder(&order);
            const auto queued = lifecycle.waitUntilCancelableOrDone(1);
            require(queued.readyToCancel && !queued.done, "Queued order was not made cancelable");
            require(lifecycle.beginCancellation(), "Cancelable order changed before cancellation");
            lifecycle.cancellationSubmitted(0);
            order.OrderStatus = THOST_FTDC_OST_Canceled;
            lifecycle.returnedOrder(&order);
            const auto canceled = lifecycle.waitUntilDone(1);
            require(canceled.ok && canceled.cancelAttempted, "Program cancellation was not verified");
        });
        test("fully traded order cannot claim cancellation passed", [] {
            OrderLifecycle lifecycle;
            lifecycle.start("9");
            CThostFtdcOrderField order{};
            field(order.OrderRef, "9", "order_ref");
            order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
            order.OrderStatus = THOST_FTDC_OST_AllTraded;
            lifecycle.returnedOrder(&order);
            const auto result = lifecycle.waitUntilCancelableOrDone(1);
            require(result.done && !result.ok && !result.cancelAttempted,
                    "Fully traded order incorrectly passed cancellation");
        });
        test("canceled callback during rejected cancel call cannot pass", [] {
            OrderLifecycle lifecycle;
            lifecycle.start("10");
            CThostFtdcOrderField order{};
            field(order.OrderRef, "10", "order_ref");
            order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
            order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            lifecycle.returnedOrder(&order);
            require(lifecycle.waitUntilCancelableOrDone(1).readyToCancel, "Order did not queue");
            require(lifecycle.beginCancellation(), "Cancellation could not begin");
            order.OrderStatus = THOST_FTDC_OST_Canceled; // 模拟 SDK 在 ReqOrderAction 返回前同步派发回调。
            lifecycle.returnedOrder(&order);
            lifecycle.cancellationSubmitted(-2);
            const auto result = lifecycle.waitUntilDone(1);
            require(result.done && !result.ok, "Rejected cancellation incorrectly passed after early callback");
        });
        setTestEnv("");
        std::cout << passed << " offline regression tests passed; no SDK connection was attempted.\n";
        return 0;
    } catch (...) {
        // Deliberately omit exception payloads to keep regression output secret-free.
        return 1;
    }
}
