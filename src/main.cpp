// 第一阶段：只验证个股期权评测 API 的连接、认证、登录和只读资金查询。
// SDK 的对象/类型在 ctp_sopt 命名空间，不能混用期货版 CTP 头文件或 DLL。
#include "ThostFtdcTraderApi.h"
#include "ThostFtdcMdApi.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace ctp_sopt;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock; // 超时用单调时钟，避免系统校时影响等待时间。
constexpr const char* kVersion = "v0.1.0";
constexpr const char* kTraderFront = "tcp://101.226.254.157:32205";
constexpr const char* kMdFront = "tcp://101.226.254.157:32213";

struct Config {
    std::string broker = "1000", user = "887120202987", investor;
    std::string trader = kTraderFront, md = kMdFront;
    std::string app = "client_shunjingsf_v1.0.0"; // 券商分配的 AppID，独立于本程序版本。
};
struct Options {
    std::string mode = "all", config = "config/connection.ini";
    int timeout = 30;
    bool skipQuery = false, help = false, version = false;
};

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { o.help = true; continue; }
        if (arg == "--version") { o.version = true; continue; }
        if (arg == "--skip-query") { o.skipQuery = true; continue; }
        if (arg != "--mode" && arg != "--config" && arg != "--timeout")
            throw std::runtime_error("Unknown option; use --help.");
        if (++i == argc) throw std::runtime_error("Missing option value; use --help.");
        std::string value = argv[i];
        if (arg == "--mode") o.mode = value;
        else if (arg == "--config") o.config = value;
        else {
            if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("--timeout must be 1..300 seconds.");
            o.timeout = std::stoi(value);
            if (o.timeout < 1 || o.timeout > 300) throw std::runtime_error("--timeout must be 1..300 seconds.");
        }
    }
    if (o.mode != "all" && o.mode != "trader" && o.mode != "md")
        throw std::runtime_error("--mode must be trader, md, or all.");
    return o;
}
Config readConfig(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open configuration file; run from project root or use --config.");
    Config c;
    std::set<std::string> seen;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[connection]") continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) throw std::runtime_error("Invalid INI syntax at line " + std::to_string(number));
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (!seen.insert(key).second) throw std::runtime_error("Duplicate INI key at line " + std::to_string(number));
        if (key == "broker_id") c.broker = value;
        else if (key == "user_id") c.user = value;
        else if (key == "investor_id") c.investor = value;
        else if (key == "trader_front") c.trader = value;
        else if (key == "md_front") c.md = value;
        else if (key == "app_id") c.app = value;
        else throw std::runtime_error("Unsupported INI key at line " + std::to_string(number) +
                                      "; password/auth code must use hidden prompts or environment variables.");
    }
    if (c.investor.empty()) c.investor = c.user;
    // 第一阶段固定评测前置和 BrokerID，防止配置误指向其他环境。
    if (c.broker != "1000" || c.trader != kTraderFront || c.md != kMdFront)
        throw std::runtime_error("Phase 1 permits only BrokerID 1000 and the supplied evaluation fronts.");
    return c;
}

template <std::size_t N> void field(char (&out)[N], const std::string& in, const char* name) {
    // 超长输入必须报错，不能静默截断密码、账号或 AppID。
    if (in.empty() || in.size() >= N || in.find_first_of("\r\n\t") != std::string::npos || in.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("Invalid or overlong field: ") + name);
    std::memset(out, 0, N);
    std::memcpy(out, in.data(), in.size());
}
template <std::size_t N> std::string textField(const char (&value)[N]) {
    return std::string(value, std::find(value, value + N, '\0'));
}
void eraseSecret(std::string& s) {
    // 尽量清除当前字符串缓冲；操作系统环境和 SDK 内部副本不受此函数控制。
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (std::size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
struct Secrets {
    std::string password, auth;
    ~Secrets() { eraseSecret(password); eraseSecret(auth); }
};
std::string getSecret(const char* envName, const char* prompt) {
    if (const char* value = std::getenv(envName); value && *value) return value;
    std::cout << prompt << std::flush;
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode = 0;
    if (!GetConsoleMode(input, &oldMode) || !SetConsoleMode(input, oldMode & ~ENABLE_ECHO_INPUT))
        throw std::runtime_error("Hidden input requires an interactive console, or set the credential environment variable.");
    struct Restore { HANDLE h; DWORD mode; ~Restore() { SetConsoleMode(h, mode); } } restore{input, oldMode};
#else
    termios oldMode{};
    if (tcgetattr(STDIN_FILENO, &oldMode) != 0)
        throw std::runtime_error("Hidden input requires a terminal, or set the credential environment variable.");
    termios hidden = oldMode;
    hidden.c_lflag &= ~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) throw std::runtime_error("Cannot disable terminal echo.");
    struct Restore { termios mode; ~Restore() { tcsetattr(STDIN_FILENO, TCSANOW, &mode); } } restore{oldMode};
#endif
    std::string value;
    if (!std::getline(std::cin, value)) throw std::runtime_error("Credential input was cancelled.");
    std::cout << '\n';
    if (value.empty()) throw std::runtime_error("Credential must not be empty.");
    return value;
}

std::tm localTime(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}
std::string timestamp(bool filename = false) {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto tm = localTime(t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream out;
    out << std::put_time(&tm, filename ? "%Y%m%d_%H%M%S" : "%Y-%m-%d %H:%M:%S")
        << (filename ? "_" : ".") << std::setfill('0') << std::setw(3) << ms;
    if (!filename) out << " LOCAL";
    return out.str();
}
std::string clean(std::string s, const Secrets& secret) {
    // 先脱敏，再移除换行和控制字符，避免远端 ErrorMsg 注入伪造日志行。
    for (const auto* value : {&secret.password, &secret.auth}) {
        if (value->empty()) continue;
        std::size_t pos = 0;
        while ((pos = s.find(*value, pos)) != std::string::npos) {
            s.replace(pos, value->size(), "[REDACTED]"); pos += 10;
        }
    }
    for (char& ch : s) if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = ' ';
    return s;
}
std::string sdkMessage(const CThostFtdcRspInfoField& info) {
    std::string raw = textField(info.ErrorMsg);
#ifdef _WIN32
    // 这套 Windows SDK 的中文错误信息按 GBK/CP936 转成 UTF-8 显示和写入日志。
    const int n = MultiByteToWideChar(936, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (n > 0) {
        std::wstring wide(n, L'\0');
        MultiByteToWideChar(936, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), n);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            std::string utf8(bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, utf8.data(), bytes, nullptr, nullptr);
            return utf8;
        }
    }
#endif
    return raw;
}
class Logger {
    std::ofstream file_;
    const Secrets& secret_;
public:
    Logger(const fs::path& path, const Secrets& secret) : file_(path, std::ios::binary), secret_(secret) {
        if (!file_) throw std::runtime_error("Cannot create run log.");
    }
    void write(const std::string& message) {
        const std::string line = timestamp() + " " + clean(message, secret_);
        std::cout << line << '\n';
        file_ << line << '\n'; file_.flush();
        if (!file_) throw std::runtime_error("Writing run log failed; test stopped.");
    }
};

enum class Stage { Connect, Authenticate, Login, Account };
const char* stageName(Stage s) {
    switch (s) {
    case Stage::Connect: return "connect"; case Stage::Authenticate: return "authenticate";
    case Stage::Login: return "login"; case Stage::Account: return "query-account";
    }
    return "unknown";
}
struct Result {
    bool ok = false, hasInfo = false;
    std::string reason;
    CThostFtdcRspInfoField info{};
    CThostFtdcRspUserLoginField login{};
    std::vector<CThostFtdcTradingAccountField> accounts;
};

// API 回调在 SDK 线程执行。它们只复制数据和通知主线程，不等条件、不发下一条请求。
// 一次只允许一个阶段在途；请求编号和阶段同时匹配，超时后收到的旧应答一律忽略。
class State {
    std::mutex mutex_;
    std::condition_variable cv_;
    Stage stage_ = Stage::Connect;
    int request_ = 0;
    bool active_ = false, done_ = false, disconnected_ = false;
    int disconnectReason_ = 0;
    Clock::time_point deadline_{};
    Result result_;
    void finish(bool ok, const std::string& reason) {
        result_.ok = ok; result_.reason = reason; done_ = true; cv_.notify_all();
    }
    bool matching(Stage stage, int request) const {
        return active_ && !done_ && stage_ == stage && request_ == request && Clock::now() < deadline_;
    }
public:
    bool begin(Stage stage, int request, int timeout) {
        std::lock_guard<std::mutex> lock(mutex_);
        stage_ = stage; request_ = request; active_ = true; done_ = false; result_ = {};
        deadline_ = Clock::now() + std::chrono::seconds(timeout);
        if (disconnected_) finish(false, "disconnected reason=" + std::to_string(disconnectReason_));
        return !done_;
    }
    void connected() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (matching(Stage::Connect, 0) && !disconnected_) finish(true, "front connected");
    }
    void disconnected(int reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnected_ = true; disconnectReason_ = reason;
        if (active_) finish(false, "disconnected reason=" + std::to_string(reason) + "; no application retry");
    }
    void immediate(int rc) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Req* 的返回值只说明本地提交是否成功，rc=0 并不表示认证/登录成功。
        if (rc != 0 && active_) finish(false, "request submission rejected; immediate_rc=" + std::to_string(rc));
    }
    template <typename T> void response(Stage stage, T* data, CThostFtdcRspInfoField* info, int request, bool last) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matching(stage, request)) return;
        if (info && info->ErrorID != 0) {
            result_.info = *info; result_.hasInfo = true; finish(false, "asynchronous API error"); return;
        }
        if constexpr (std::is_same_v<T, CThostFtdcRspUserLoginField>) {
            if (data) result_.login = *data;
        } else if constexpr (std::is_same_v<T, CThostFtdcTradingAccountField>) {
            if (data) result_.accounts.push_back(*data);
        }
        if (!last) return; // 多条查询结果必须等 bIsLast，不能把第一行当作查询完成。
        if (!data && stage != Stage::Account) finish(false, "final response has no required payload");
        else finish(true, "final response received"); // 资金查询允许 data=nullptr、bIsLast=true 的空结果。
    }
    void error(CThostFtdcRspInfoField* info, int request, bool last) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matching(stage_, request)) return;
        if (info && info->ErrorID != 0) {
            result_.info = *info; result_.hasInfo = true; finish(false, "OnRspError");
        } else if (last) finish(false, "OnRspError without a nonzero error code; no valid business response");
    }
    Result wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_until(lock, deadline_, [this] { return done_; }))
            finish(false, "stage timeout; late replies ignored; no application retry");
        active_ = false;
        return result_; // 深复制结果；主线程解锁后输出，不持锁进行文件/控制台 I/O。
    }
};

class TraderSpi final : public CThostFtdcTraderSpi {
    State& state_;
public:
    explicit TraderSpi(State& state) : state_(state) {}
    void OnFrontConnected() override { state_.connected(); }
    void OnFrontDisconnected(int reason) override { state_.disconnected(reason); }
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        state_.response(Stage::Authenticate, p, e, id, last);
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        state_.response(Stage::Login, p, e, id, last);
    }
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        state_.response(Stage::Account, p, e, id, last);
    }
    void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override { state_.error(e, id, last); }
};
class MdSpi final : public CThostFtdcMdSpi {
    State& state_;
public:
    explicit MdSpi(State& state) : state_(state) {}
    void OnFrontConnected() override { state_.connected(); }
    void OnFrontDisconnected(int reason) override { state_.disconnected(reason); }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        state_.response(Stage::Login, p, e, id, last);
    }
    void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override { state_.error(e, id, last); }
};
template <typename Api> struct ApiDeleter {
    void operator()(Api* api) const {
        // unique_ptr 在主线程退栈时释放 API；此时 SPI 和 State 仍存活。
        // 不能 delete API、不能在回调中 Release，也不能 Join 后才 Release（Join 会阻塞）。
        if (api) { api->RegisterSpi(nullptr); api->Release(); }
    }
};
bool report(Logger& log, const char* channel, Stage stage, int id, const Result& r) {
    std::string message = std::string(channel) + " stage=" + stageName(stage) + " request_id=" + std::to_string(id) +
        " status=" + (r.ok ? "PASS" : "FAIL") + " " + r.reason;
    if (r.hasInfo) message += " ErrorID=" + std::to_string(r.info.ErrorID) + " ErrorMsg=" + sdkMessage(r.info);
    log.write(message);
    if (r.ok && stage == Stage::Login) {
        log.write(std::string(channel) + " LOGIN_OK user=" + textField(r.login.UserID) +
                  " broker=" + textField(r.login.BrokerID) + " trading_day=" + textField(r.login.TradingDay) +
                  " login_time=" + textField(r.login.LoginTime) + " front_id=" + std::to_string(r.login.FrontID) +
                  " session_id=" + std::to_string(r.login.SessionID));
    }
    if (r.ok && stage == Stage::Account) {
        log.write("TRADER account_query_rows=" + std::to_string(r.accounts.size()) +
                  (r.accounts.empty() ? "; query completed empty (not proof of an available trading account)" : ""));
        for (const auto& a : r.accounts) {
            std::ostringstream line;
            line << "TRADER account=" << textField(a.AccountID) << " currency=" << textField(a.CurrencyID)
                 << " balance=" << std::fixed << std::setprecision(2) << a.Balance << " available=" << a.Available;
            log.write(line.str());
        }
    }
    return r.ok;
}
template <typename Submit> bool runStage(State& state, Logger& log, const char* channel,
                                        Stage stage, int id, int timeout, Submit submit) {
    log.write(std::string(channel) + " stage=" + stageName(stage) + " START request_id=" + std::to_string(id) +
              " timeout_seconds=" + std::to_string(timeout));
    if (state.begin(stage, id, timeout)) {
        const int rc = submit();
        state.immediate(rc);
        if (stage != Stage::Connect) log.write(std::string(channel) + " request_id=" + std::to_string(id) +
                                               " immediate_rc=" + std::to_string(rc) + " (submission only)");
    }
    return report(log, channel, stage, id, state.wait());
}
CThostFtdcReqUserLoginField loginRequest(const Config& c, const Secrets& s) {
    CThostFtdcReqUserLoginField request{};
    field(request.BrokerID, c.broker, "broker_id"); field(request.UserID, c.user, "user_id");
    field(request.Password, s.password, "password");
    // 此处不手填 IP/MAC，不伪造终端信息；SDK 实际采集和后台核验情况须在实体机登录后向券商确认。
    return request;
}
bool testTrader(const Config& c, const Secrets& s, const Options& o, const fs::path& flow, Logger& log) {
    State state;
    TraderSpi spi(state);
    const std::string flowPath = flow.generic_string() + "/";
    std::string front = c.trader;
    CThostFtdcReqAuthenticateField auth{};
    field(auth.BrokerID, c.broker, "broker_id"); field(auth.UserID, c.user, "user_id");
    field(auth.AppID, c.app, "app_id"); field(auth.AuthCode, s.auth, "auth_code");
    auto login = loginRequest(c, s);
    CThostFtdcQryTradingAccountField query{};
    field(query.BrokerID, c.broker, "broker_id"); field(query.InvestorID, c.investor, "investor_id");
    // API 最后构造、最先释放：回调对象、地址字符串和请求缓冲都活到 Release 返回。
    std::unique_ptr<CThostFtdcTraderApi, ApiDeleter<CThostFtdcTraderApi>> api(CThostFtdcTraderApi::CreateFtdcTraderApi(flowPath.c_str()));
    if (!api) throw std::runtime_error("CreateFtdcTraderApi returned null.");
    api->RegisterSpi(&spi);
    // QUICK 从登录后的新消息开始；不会重放历史私有/公共流。本例不处理报单、成交回报。
    api->SubscribePrivateTopic(THOST_TERT_QUICK); api->SubscribePublicTopic(THOST_TERT_QUICK);
    api->RegisterFront(front.data());
    if (!runStage(state, log, "TRADER", Stage::Connect, 0, o.timeout, [&] { api->Init(); return 0; })) return false;
    if (!runStage(state, log, "TRADER", Stage::Authenticate, 1, o.timeout, [&] { return api->ReqAuthenticate(&auth, 1); })) return false;
    log.write("TRADER LOGIN_START user=" + c.user + " broker=" + c.broker + " app_id=" + c.app);
    if (!runStage(state, log, "TRADER", Stage::Login, 2, o.timeout, [&] { return api->ReqUserLogin(&login, 2); })) return false;
    if (o.skipQuery) { log.write("TRADER account query SKIPPED (--skip-query)"); return true; }
    // 不猜测账户币种/业务类型；其他筛选字段保持 SDK 零初始化值，读取该投资者返回的数据。
    return runStage(state, log, "TRADER", Stage::Account, 3, o.timeout, [&] { return api->ReqQryTradingAccount(&query, 3); });
}
bool testMd(const Config& c, const Secrets& s, const Options& o, const fs::path& flow, Logger& log) {
    State state;
    MdSpi spi(state);
    const std::string flowPath = flow.generic_string() + "/";
    std::string front = c.md;
    auto login = loginRequest(c, s);
    std::unique_ptr<CThostFtdcMdApi, ApiDeleter<CThostFtdcMdApi>> api(CThostFtdcMdApi::CreateFtdcMdApi(flowPath.c_str(), false, false));
    if (!api) throw std::runtime_error("CreateFtdcMdApi returned null.");
    api->RegisterSpi(&spi);
    api->RegisterFront(front.data());
    if (!runStage(state, log, "MD", Stage::Connect, 0, o.timeout, [&] { api->Init(); return 0; })) return false;
    log.write("MD LOGIN_START user=" + c.user + " broker=" + c.broker);
    // 此版行情 API 没有 ReqAuthenticate；行情登录成功也不能替代交易端认证/登录成功。
    return runStage(state, log, "MD", Stage::Login, 1, o.timeout, [&] { return api->ReqUserLogin(&login, 1); });
}
std::string administratorStatus() {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "UNKNOWN";
    TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
    CloseHandle(token);
    return ok ? (elevation.TokenIsElevated ? "YES" : "NO") : "UNKNOWN";
#else
    return "N/A (non-Windows syntax/development environment)";
#endif
}
void printVersions() {
    std::cout << "CTPStockConnectivity " << kVersion << '\n'
              << "Trader API: " << CThostFtdcTraderApi::GetApiVersion() << '\n'
              << "MD API: " << CThostFtdcMdApi::GetApiVersion() << '\n';
}
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    Secrets secret; // 在异常处理结束后才清除，以便异常输出也能脱敏。
    try {
        const Options options = parseOptions(argc, argv);
        if (options.help) {
            std::cout << "Usage: ctp_stock_connect.exe [--mode all|trader|md] [--config FILE]\n"
                         "       [--timeout 1..300] [--skip-query] [--version] [--help]\n"
                         "Default: all, config/connection.ini, 30 seconds PER STAGE, account query enabled.\n"
                         "Credentials: hidden console prompts or CTP_PASSWORD / CTP_AUTH_CODE.\n"
                         "MD-only mode does not need CTP_AUTH_CODE. No order or password-update operations.\n";
            return 0;
        }
        printVersions();
        if (options.version) return 0; // help/version 不读配置、不索取密码、不连接网络。
        const Config config = readConfig(options.config);
        secret.password = getSecret("CTP_PASSWORD", "Trading password (hidden): ");
        if (options.mode != "md") secret.auth = getSecret("CTP_AUTH_CODE", "Authentication code (hidden): ");
        // 先校验所有固定长度字段，避免连上前置后才发现输入被截断或无效。
        (void)loginRequest(config, secret);
        CThostFtdcReqAuthenticateField check{};
        field(check.AppID, config.app, "app_id");
        if (options.mode != "md") field(check.AuthCode, secret.auth, "auth_code");
        CThostFtdcQryTradingAccountField qcheck{};
        field(qcheck.InvestorID, config.investor, "investor_id");
#ifdef _WIN32
        const auto pid = GetCurrentProcessId();
#else
        const auto pid = getpid();
#endif
        std::string runId = timestamp(true) + "_pid" + std::to_string(pid);
        fs::create_directories("logs"); fs::create_directories("flow");
        fs::path logDir = fs::path("logs") / runId;
        // 毫秒+PID 通常足够；仍检测碰撞，任何运行都不覆盖旧日志/流文件。
        for (int n = 1; !fs::create_directory(logDir); ++n) logDir = fs::path("logs") / (runId + "_" + std::to_string(n));
        runId = logDir.filename().string();
        const fs::path flowDir = fs::path("flow") / runId;
        if (fs::exists(flowDir)) throw std::runtime_error("Flow directory collision; rerun to obtain a new run ID.");
        fs::create_directories(flowDir / "trader"); fs::create_directories(flowDir / "md");
        Logger log(logDir / "run.log", secret);
        log.write(std::string("PROGRAM version=") + kVersion + " sdk_package=traderAPI_3.7.0_T_20231127");
        log.write(std::string("API trader=") + CThostFtdcTraderApi::GetApiVersion() + " md=" + CThostFtdcMdApi::GetApiVersion());
        log.write("RUN id=" + runId + " mode=" + options.mode + " user=" + config.user + " broker=" + config.broker);
        log.write("CONFIG app_id=" + config.app + " investor_id=" + config.investor);
        log.write("FRONTS trader=" + config.trader + " md=" + config.md);
        log.write("TIME timestamps=host_local_wall_clock; elapsed_timeouts=monotonic; check local clock before evidence capture");
        const std::string admin = administratorStatus();
        log.write("HOST administrator=" + admin + " physical_machine=NOT_VERIFIED (operator must confirm)");
        if (admin != "YES") log.write("NOTICE formal evaluation login requires an elevated Windows console on a physical machine");
        log.write("SCOPE connect/auth/login/read-only-account-query; no market subscription, order, cancel, settlement or password change");
        bool traderOk = true, mdOk = true;
        if (options.mode != "md") traderOk = testTrader(config, secret, options, flowDir / "trader", log);
        // all 模式中行情是独立诊断项；交易失败后仍只执行一次行情连接和登录。
        if (options.mode != "trader") mdOk = testMd(config, secret, options, flowDir / "md", log);
        log.write(std::string("RESULT overall=") + (traderOk && mdOk ? "PASS" : "FAIL") +
                  " trader=" + (options.mode == "md" ? "NOT_RUN" : traderOk ? "PASS" : "FAIL") +
                  " md=" + (options.mode == "trader" ? "NOT_RUN" : mdOk ? "PASS" : "FAIL"));
        log.write("EVIDENCE log=" + fs::absolute(logDir / "run.log").string());
        log.write("NEXT successful login should be reported to broker the same day; this program does not notify anyone");
        return traderOk && mdOk ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << timestamp() << " FATAL " << clean(error.what(), secret) << '\n';
        return 2;
    }
}
