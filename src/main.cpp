// 个股期权评测 API 的连接测试，以及受显式开关保护的单次限价报单/撤单基础功能测试。
// SDK 的对象/类型在 ctp_sopt 命名空间，不能混用期货版 CTP 头文件或 DLL。
#include "ThostFtdcTraderApi.h"
#include "ThostFtdcMdApi.h"
#include <algorithm>
#include <chrono>
#include <cmath>
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
#include <system_error>
#include <type_traits>
#include <utility>
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
constexpr const char* kVersion = "v0.3.0";
constexpr const char* kTraderFront = "tcp://101.226.254.157:32205";
constexpr const char* kMdFront = "tcp://101.226.254.157:32213";

struct Config {
    std::string broker = "1000", user = "887120202987", investor;
    std::string trader = kTraderFront, md = kMdFront;
    std::string app = "client_shunjingsf_v1.0.0"; // 券商分配的 AppID，独立于本程序版本。
    int dailyMaxOrderCount = 0; // 0 表示尚未按报备表配置；实发报单将拒绝启动。
};
struct Options {
    std::string mode = "all", config = "config/connection.ini";
    std::string test = "connectivity", instrument, exchange, direction, offset, confirmation, riskAction;
    double price = 0.0;
    int timeout = 30;
    bool skipQuery = false, sendOrder = false, help = false, version = false;
};

void eraseSecret(std::string& s) {
    // 尽量清除当前字符串缓冲；配置文件、操作系统环境和 SDK 内部副本不受此函数控制。
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (std::size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
struct Secrets {
    std::string password, auth;
    ~Secrets() { eraseSecret(password); eraseSecret(auth); }
};

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
int parseDailyMaxOrderCount(const std::string& value) {
    if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("daily_max_order_count must be an integer from 1 to 999999999.");
    const auto parsed = std::stoll(value);
    if (parsed < 1 || parsed > 999999999)
        throw std::runtime_error("daily_max_order_count must be an integer from 1 to 999999999.");
    return static_cast<int>(parsed);
}
Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { o.help = true; continue; }
        if (arg == "--version") { o.version = true; continue; }
        if (arg == "--skip-query") { o.skipQuery = true; continue; }
        if (arg == "--send-order") { o.sendOrder = true; continue; }
        if (arg != "--mode" && arg != "--config" && arg != "--timeout" && arg != "--test" &&
            arg != "--instrument" && arg != "--exchange" && arg != "--direction" &&
            arg != "--offset" && arg != "--price" && arg != "--confirm" && arg != "--risk-action")
            throw std::runtime_error("Unknown option; use --help.");
        if (++i == argc) throw std::runtime_error("Missing option value; use --help.");
        std::string value = argv[i];
        if (arg == "--mode") o.mode = value;
        else if (arg == "--config") o.config = value;
        else if (arg == "--timeout") {
            if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("--timeout must be 1..300 seconds.");
            o.timeout = std::stoi(value);
            if (o.timeout < 1 || o.timeout > 300) throw std::runtime_error("--timeout must be 1..300 seconds.");
        } else if (arg == "--test") o.test = value;
        else if (arg == "--instrument") o.instrument = value;
        else if (arg == "--exchange") o.exchange = value;
        else if (arg == "--direction") o.direction = value;
        else if (arg == "--offset") o.offset = value;
        else if (arg == "--confirm") o.confirmation = value;
        else if (arg == "--risk-action") o.riskAction = value;
        else if (arg == "--price") {
            std::size_t used = 0;
            try { o.price = std::stod(value, &used); }
            catch (...) { throw std::runtime_error("--price must be a positive finite number."); }
            if (used != value.size() || !std::isfinite(o.price) || o.price <= 0.0)
                throw std::runtime_error("--price must be a positive finite number.");
        }
    }
    if (o.mode != "all" && o.mode != "trader" && o.mode != "md")
        throw std::runtime_error("--mode must be trader, md, or all.");
    if (o.test != "connectivity" && o.test != "basic" && o.test != "risk")
        throw std::runtime_error("--test must be connectivity, basic, or risk.");
    if (o.test == "connectivity") {
        if (o.sendOrder || !o.instrument.empty() || !o.exchange.empty() || !o.direction.empty() ||
            !o.offset.empty() || o.price != 0.0 || !o.confirmation.empty() || !o.riskAction.empty())
            throw std::runtime_error("Order options require --test basic.");
    } else if (o.test == "basic") {
        if (o.mode != "trader") throw std::runtime_error("--test basic requires --mode trader.");
        if (o.instrument.empty() || o.exchange.empty() || o.direction.empty() || o.offset.empty() || o.price <= 0.0)
            throw std::runtime_error("--test basic requires --instrument, --exchange, --direction, --offset, and --price.");
        if (o.exchange != "SSE" && o.exchange != "SZSE")
            throw std::runtime_error("--exchange must be SSE or SZSE.");
        if (o.direction != "buy" && o.direction != "sell")
            throw std::runtime_error("--direction must be buy or sell.");
        if (o.offset != "open" && o.offset != "close")
            throw std::runtime_error("--offset must be open or close.");
        if (o.sendOrder && o.confirmation != "SEND_ONE_ORDER")
            throw std::runtime_error("Live transmission requires --confirm SEND_ONE_ORDER.");
        if (!o.sendOrder && !o.confirmation.empty())
            throw std::runtime_error("--confirm is valid only with --send-order.");
        if (o.sendOrder && o.skipQuery)
            throw std::runtime_error("Live basic test does not allow --skip-query.");
        if (!o.riskAction.empty()) throw std::runtime_error("--risk-action requires --test risk.");
    } else {
        if (o.riskAction != "settings" && o.riskAction != "trigger")
            throw std::runtime_error("--test risk requires --risk-action settings or trigger.");
        if (o.sendOrder || o.skipQuery || !o.instrument.empty() || !o.exchange.empty() ||
            !o.direction.empty() || !o.offset.empty() || o.price != 0.0)
            throw std::runtime_error("Risk evidence mode does not accept connectivity or order options.");
        if (o.riskAction == "trigger" && o.confirmation != "TRIGGER_DAILY_ORDER_LIMIT")
            throw std::runtime_error("Risk trigger self-test requires --confirm TRIGGER_DAILY_ORDER_LIMIT.");
        if (o.riskAction == "settings" && !o.confirmation.empty())
            throw std::runtime_error("Risk settings display does not accept --confirm.");
    }
    return o;
}
void readConfigFile(const fs::path& path, Config& c, Secrets& secret) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open configuration file; run from project root or use --config.");
    std::set<std::string> seen;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[connection]" || line == "[credentials]" || line == "[risk]") continue;
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
        else if (key == "daily_max_order_count") {
            if (!value.empty()) c.dailyMaxOrderCount = parseDailyMaxOrderCount(value);
        }
        // 凭据不放进会被日志输出的 Config；空值表示继续使用其他来源。
        else if (key == "password") { if (!value.empty()) secret.password = value; }
        else if (key == "auth_code") { if (!value.empty()) secret.auth = value; }
        else throw std::runtime_error("Unsupported INI key at line " + std::to_string(number));
    }
    if (in.bad()) throw std::runtime_error("Failed to read configuration file.");
}
Config readConfig(const std::string& path, Secrets& secret) {
    Config c;
    const fs::path primary(path);
    readConfigFile(primary, c, secret);
    // connection.ini 自动叠加同目录的 connection.local.ini，直接运行 EXE 也生效。
    // 显式指定 *.local.ini 时只读取该文件，不再寻找 *.local.local.ini。
    const std::string filename = primary.filename().string();
    const std::string localSuffix = ".local.ini";
    if (filename.size() < localSuffix.size() ||
        filename.compare(filename.size() - localSuffix.size(), localSuffix.size(), localSuffix) != 0) {
        fs::path local = primary;
        local.replace_extension(".local.ini");
        if (fs::exists(local)) readConfigFile(local, c, secret);
    }
    if (c.investor.empty()) c.investor = c.user;
    // 本项目固定评测前置和 BrokerID，防止配置误指向生产或其他环境。
    if (c.broker != "1000" || c.trader != kTraderFront || c.md != kMdFront)
        throw std::runtime_error("This project permits only BrokerID 1000 and the supplied evaluation fronts.");
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
std::string nextOrderRef(const CThostFtdcRspUserLoginField& login) {
    const std::string current = trim(textField(login.MaxOrderRef));
    unsigned long long value = 0;
    if (!current.empty()) {
        if (current.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Login MaxOrderRef is not numeric; live order test stopped.");
        try { value = std::stoull(current); }
        catch (...) { throw std::runtime_error("Login MaxOrderRef is invalid; live order test stopped."); }
    }
    if (value >= 999999999999ULL)
        throw std::runtime_error("Login MaxOrderRef has no safe room for the next order reference.");
    return std::to_string(value + 1);
}
CThostFtdcInputOrderField basicOrderRequest(const Config& c, const Options& o, const std::string& orderRef) {
    CThostFtdcInputOrderField request{};
    field(request.BrokerID, c.broker, "broker_id");
    field(request.InvestorID, c.investor, "investor_id");
    field(request.UserID, c.user, "user_id");
    field(request.InstrumentID, o.instrument, "instrument");
    field(request.ExchangeID, o.exchange, "exchange");
    field(request.OrderRef, orderRef, "order_ref");
    request.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    request.Direction = o.direction == "buy" ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    request.CombOffsetFlag[0] = o.offset == "open" ? THOST_FTDC_OF_Open : THOST_FTDC_OF_Close;
    request.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    request.LimitPrice = o.price;
    request.VolumeTotalOriginal = 1; // 基础功能测试硬限制 API 报单数量为 1，不提供放大数量的参数。
    request.TimeCondition = THOST_FTDC_TC_GFD;
    request.VolumeCondition = THOST_FTDC_VC_AV;
    request.MinVolume = 1;
    request.ContingentCondition = THOST_FTDC_CC_Immediately;
    request.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    request.IsAutoSuspend = 0;
    request.RequestID = 4;
    request.UserForceClose = 0;
    request.IsSwapOrder = 0;
    return request;
}
std::string orderPlanText(const Options& o, const std::string& orderRef) {
    std::ostringstream line;
    line << "strategy=single-shot-limit instrument=" << o.instrument
         << " exchange=" << o.exchange << " direction=" << o.direction
         << " offset=" << o.offset << " limit_price=" << std::fixed << std::setprecision(6) << o.price
         << " volume=1 time_condition=GFD order_ref=" << orderRef;
    return line.str();
}
std::string getSecret(const std::string& configured, const char* envName, const char* prompt) {
    if (!configured.empty()) return configured;
    if (const char* value = std::getenv(envName); value && *value) return value;
    std::cout << prompt << std::flush;
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode = 0;
    if (!GetConsoleMode(input, &oldMode) || !SetConsoleMode(input, oldMode & ~ENABLE_ECHO_INPUT))
        throw std::runtime_error("Set password/auth_code in the INI configuration, set the credential environment variable, or use an interactive console.");
    struct Restore { HANDLE h; DWORD mode; ~Restore() { SetConsoleMode(h, mode); } } restore{input, oldMode};
#else
    termios oldMode{};
    if (tcgetattr(STDIN_FILENO, &oldMode) != 0)
        throw std::runtime_error("Set password/auth_code in the INI configuration, set the credential environment variable, or use an interactive terminal.");
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
    // 先脱敏，再移除换行和控制字符；保留已经转换为 UTF-8 的中文消息。
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
std::string sdkText(std::string raw) {
#ifdef _WIN32
    // Windows 股票期权 SDK 的本地语言文本使用 GBK/CP936；统一转为 UTF-8 后再显示和写日志。
    if (raw.empty()) return raw;
    const int wideChars = MultiByteToWideChar(936, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wideChars > 0) {
        std::wstring wide(wideChars, L'\0');
        MultiByteToWideChar(936, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), wideChars);
        const int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideChars, nullptr, 0, nullptr, nullptr);
        if (utf8Bytes > 0) {
            std::string utf8(utf8Bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideChars, utf8.data(), utf8Bytes, nullptr, nullptr);
            return utf8;
        }
    }
#endif
    return raw;
}
std::string sdkMessage(const CThostFtdcRspInfoField& info) {
    return sdkText(textField(info.ErrorMsg));
}
const char* orderSubmitStatusName(char status) {
    switch (status) {
    case THOST_FTDC_OSS_InsertSubmitted: return "INSERT_SUBMITTED";
    case THOST_FTDC_OSS_CancelSubmitted: return "CANCEL_SUBMITTED";
    case THOST_FTDC_OSS_ModifySubmitted: return "MODIFY_SUBMITTED";
    case THOST_FTDC_OSS_Accepted: return "ACCEPTED";
    case THOST_FTDC_OSS_InsertRejected: return "INSERT_REJECTED";
    case THOST_FTDC_OSS_CancelRejected: return "CANCEL_REJECTED";
    case THOST_FTDC_OSS_ModifyRejected: return "MODIFY_REJECTED";
    default: return "UNKNOWN_CODE";
    }
}
const char* orderStatusName(char status) {
    switch (status) {
    case THOST_FTDC_OST_AllTraded: return "ALL_TRADED";
    case THOST_FTDC_OST_PartTradedQueueing: return "PART_TRADED_QUEUEING";
    case THOST_FTDC_OST_PartTradedNotQueueing: return "PART_TRADED_NOT_QUEUEING";
    case THOST_FTDC_OST_NoTradeQueueing: return "NO_TRADE_QUEUEING";
    case THOST_FTDC_OST_NoTradeNotQueueing: return "NO_TRADE_NOT_QUEUEING";
    case THOST_FTDC_OST_Canceled: return "CANCELED";
    case THOST_FTDC_OST_Unknown: return "UNKNOWN";
    case THOST_FTDC_OST_NotTouched: return "NOT_TOUCHED";
    case THOST_FTDC_OST_Touched: return "TOUCHED";
    default: return "UNKNOWN_CODE";
    }
}
class Logger {
    std::ofstream file_;
    const Secrets& secret_;
    std::mutex mutex_;
public:
    Logger(const fs::path& path, const Secrets& secret) : file_(path, std::ios::binary), secret_(secret) {
        if (!file_) throw std::runtime_error("Cannot create run log.");
    }
    void write(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = timestamp() + " " + clean(message, secret_);
        std::cout << line << '\n';
        file_ << line << '\n'; file_.flush();
        if (!file_) throw std::runtime_error("Writing run log failed; test stopped.");
    }
};

struct DailyOrderRiskDecision {
    bool allowed = false;
    int configuredLimit = 0;
    int submittedBefore = 0;
    int submittedAfter = 0;
    std::string tradingDay;
};
DailyOrderRiskDecision evaluateDailyOrderRisk(int configuredLimit, int submittedBefore,
                                              const std::string& tradingDay) {
    if (configuredLimit < 1 || submittedBefore < 0)
        throw std::runtime_error("Invalid daily order risk-control state.");
    DailyOrderRiskDecision result;
    result.allowed = submittedBefore < configuredLimit;
    result.configuredLimit = configuredLimit;
    result.submittedBefore = submittedBefore;
    result.submittedAfter = submittedBefore + (result.allowed ? 1 : 0);
    result.tradingDay = tradingDay;
    return result;
}
bool validTradingDay(const std::string& value) {
    return value.size() == 8 && value.find_first_not_of("0123456789") == std::string::npos;
}
unsigned long processIdNumber() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
std::string safeFilePart(std::string value) {
    for (char& ch : value) {
        const bool safe = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
        if (!safe) ch = '_';
    }
    return value.empty() ? "unknown" : value;
}
fs::path dailyOrderStatePath(const Config& c) {
    return fs::path("state") /
        ("daily_order_count_" + safeFilePart(c.broker) + "_" + safeFilePart(c.user) + ".ini");
}

class DailyOrderCounter {
    struct StoredState { std::string tradingDay; int submittedCount = 0; };
    fs::path path_;
    int configuredLimit_;
#ifdef _WIN32
    HANDLE mutex_ = nullptr;
#endif

    StoredState readLocked() const {
        if (!fs::exists(path_)) return {};
        std::ifstream input(path_, std::ios::binary);
        if (!input) throw std::runtime_error("Daily order count state cannot be opened; order blocked.");
        StoredState state;
        bool sawDay = false, sawCount = false;
        std::string line;
        while (std::getline(input, line)) {
            line = trim(line);
            if (line.empty()) continue;
            const auto equal = line.find('=');
            if (equal == std::string::npos)
                throw std::runtime_error("Daily order count state is invalid; order blocked.");
            const std::string key = trim(line.substr(0, equal));
            const std::string value = trim(line.substr(equal + 1));
            if (key == "trading_day" && !sawDay) {
                if (!validTradingDay(value))
                    throw std::runtime_error("Daily order count trading day is invalid; order blocked.");
                state.tradingDay = value; sawDay = true;
            } else if (key == "submitted_count" && !sawCount) {
                if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("Daily order count value is invalid; order blocked.");
                const auto parsed = std::stoll(value);
                if (parsed < 0 || parsed > 999999999)
                    throw std::runtime_error("Daily order count value is invalid; order blocked.");
                state.submittedCount = static_cast<int>(parsed); sawCount = true;
            } else {
                throw std::runtime_error("Daily order count state has duplicate or unknown fields; order blocked.");
            }
        }
        if (input.bad() || !sawDay || !sawCount)
            throw std::runtime_error("Daily order count state is incomplete; order blocked.");
        return state;
    }
    void writeLocked(const StoredState& state) const {
        fs::create_directories(path_.parent_path());
        fs::path temporary = path_;
        temporary += ".tmp." + std::to_string(processIdNumber());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Daily order count state cannot be written; order blocked.");
            output << "trading_day=" << state.tradingDay << '\n'
                   << "submitted_count=" << state.submittedCount << '\n';
            output.flush();
            if (!output) {
                std::error_code ignored; fs::remove(temporary, ignored);
                throw std::runtime_error("Daily order count state write failed; order blocked.");
            }
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored; fs::remove(temporary, ignored);
            throw std::runtime_error("Daily order count state replacement failed; order blocked.");
        }
#else
        std::error_code error;
        fs::rename(temporary, path_, error);
        if (error) {
            std::error_code ignored; fs::remove(temporary, ignored);
            throw std::runtime_error("Daily order count state replacement failed; order blocked.");
        }
#endif
    }
    DailyOrderRiskDecision reserveLocked(const std::string& tradingDay) {
        StoredState state = readLocked();
        if (state.tradingDay != tradingDay) {
            state.tradingDay = tradingDay;
            state.submittedCount = 0;
        }
        const auto decision = evaluateDailyOrderRisk(configuredLimit_, state.submittedCount, tradingDay);
        if (decision.allowed) {
            state.submittedCount = decision.submittedAfter;
            writeLocked(state); // 先保守计数，再实际调用 ReqOrderInsert；失败或拒单也属于一次报单尝试。
        }
        return decision;
    }
public:
    DailyOrderCounter(fs::path path, int configuredLimit, const Config& c)
        : path_(std::move(path)), configuredLimit_(configuredLimit) {
        if (configuredLimit_ < 1) throw std::runtime_error("daily_max_order_count is not configured; order blocked.");
#ifdef _WIN32
        const std::string suffix = safeFilePart(c.broker) + "_" + safeFilePart(c.user);
        const std::wstring name = L"Local\\CTPStockConnectivity_DailyOrderCount_" +
                                  std::wstring(suffix.begin(), suffix.end());
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) throw std::runtime_error("Daily order count mutex creation failed; order blocked.");
#else
        (void)c;
#endif
    }
    ~DailyOrderCounter() {
#ifdef _WIN32
        if (mutex_) CloseHandle(mutex_);
#endif
    }
    DailyOrderCounter(const DailyOrderCounter&) = delete;
    DailyOrderCounter& operator=(const DailyOrderCounter&) = delete;
    DailyOrderRiskDecision reserve(const std::string& tradingDay) {
        if (!validTradingDay(tradingDay))
            throw std::runtime_error("Login did not return a valid trading day; order blocked by risk control.");
#ifdef _WIN32
        const DWORD wait = WaitForSingleObject(mutex_, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            throw std::runtime_error("Daily order count lock timeout; order blocked.");
        struct Release { HANDLE handle; ~Release() { ReleaseMutex(handle); } } release{mutex_};
        return reserveLocked(tradingDay);
#else
        static std::mutex fallbackMutex;
        std::lock_guard<std::mutex> lock(fallbackMutex);
        return reserveLocked(tradingDay);
#endif
    }
};

void showRiskSettingsDialog(int configuredLimit) {
#ifdef _WIN32
    const std::wstring text = L"风控设置（每日最大报单量）\n\n"
        L"报备阈值／每日最大报单量：" + std::to_wstring(configuredLimit) + L" 笔\n"
        L"配置项：daily_max_order_count\n"
        L"统计口径：每次 ReqOrderInsert 调用计 1 笔\n"
        L"统计周期：交易日\n"
        L"计数范围：当前账号及本项目目录";
    MessageBoxW(nullptr, text.c_str(), L"CTPStockConnectivity 风控设置",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
#else
    (void)configuredLimit;
#endif
}
void showRiskTriggerDialog(const DailyOrderRiskDecision& decision, bool selfTest);

std::string localCalendarDay() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d");
    return out.str();
}
fs::path createRunLogDirectory(std::string& runId) {
    runId = timestamp(true) + "_pid" + std::to_string(processIdNumber());
    fs::create_directories("logs");
    fs::path logDir = fs::path("logs") / runId;
    // 毫秒+PID 通常足够；仍检测碰撞，任何运行都不覆盖旧日志。
    for (int n = 1; !fs::create_directory(logDir); ++n)
        logDir = fs::path("logs") / (runId + "_" + std::to_string(n));
    runId = logDir.filename().string();
    return logDir;
}
bool runRiskEvidence(const Config& c, const Options& o, const Secrets& secret) {
    if (c.dailyMaxOrderCount < 1)
        throw std::runtime_error("Risk evidence requires daily_max_order_count in config/connection.local.ini.");
    std::string runId;
    const fs::path logDir = createRunLogDirectory(runId);
    Logger log(logDir / "run.log", secret);
    log.write(std::string("PROGRAM version=") + kVersion + " sdk_package=traderAPI_3.7.5_CP_20251125");
    log.write("RUN id=" + runId + " test=risk risk_action=" + o.riskAction +
              " user=" + c.user + " broker=" + c.broker);
    log.write("RISK CONFIG rule=daily_max_order_count configured_limit=" +
              std::to_string(c.dailyMaxOrderCount) +
              " unit=orders counting_rule=ReqOrderInsert_attempts period=trading_day");
    log.write("RISK NOTICE evidence_mode=LOCAL_SELF_TEST network=NOT_CONNECTED credentials=NOT_USED order_api=NOT_CALLED");
    if (o.riskAction == "settings") {
        showRiskSettingsDialog(c.dailyMaxOrderCount);
        log.write("RISK RESULT status=PASS action=settings screenshot_dialog=REQUESTED");
    } else {
        const auto decision = evaluateDailyOrderRisk(c.dailyMaxOrderCount, c.dailyMaxOrderCount,
                                                     localCalendarDay());
        log.write("RISK SELF_TEST injected_submitted_count=" + std::to_string(decision.submittedBefore) +
                  " configured_limit=" + std::to_string(decision.configuredLimit) +
                  " production_state_file=NOT_READ_OR_WRITTEN");
        showRiskTriggerDialog(decision, true);
        log.write("RISK TRIGGER rule=daily_max_order_count decision=BLOCK api_call=NOT_SENT");
        log.write("RISK RESULT status=PASS action=trigger control=BLOCKED_AS_EXPECTED");
    }
    log.write("EVIDENCE log=" + fs::absolute(logDir / "run.log").string());
    return true;
}
void showRiskTriggerDialog(const DailyOrderRiskDecision& decision, bool selfTest) {
#ifdef _WIN32
    const std::wstring text = L"风控触发：已达到每日最大报单量\n\n"
        L"报备阈值：" + std::to_wstring(decision.configuredLimit) + L" 笔\n"
        L"当日已计数：" + std::to_wstring(decision.submittedBefore) + L" 笔\n"
        L"本次报单：已在本地拦截，未发送至柜台\n"
        L"测试触发：" + std::wstring(selfTest ? L"是（未发送真实报单）" : L"否");
    MessageBoxW(nullptr, text.c_str(), L"CTPStockConnectivity 风控触发",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
#else
    (void)decision; (void)selfTest;
#endif
}

enum class Stage { Connect, Authenticate, Login, Account };
enum class RequestIdPolicy { Strict, AllowZero };
const char* stageName(Stage s) {
    switch (s) {
    case Stage::Connect: return "connect"; case Stage::Authenticate: return "authenticate";
    case Stage::Login: return "login"; case Stage::Account: return "query-account";
    }
    return "unknown";
}
struct Result {
    bool ok = false, hasInfo = false, acceptedZeroRequestId = false;
    std::string reason;
    CThostFtdcRspInfoField info{};
    CThostFtdcRspUserLoginField login{};
    std::vector<CThostFtdcTradingAccountField> accounts;
};

// API 回调在 SDK 线程执行。它们只复制数据和通知主线程，不等条件、不发下一条请求。
// 一次只允许一个阶段在途；默认要求请求编号和阶段同时匹配。
// 已实测该股票期权行情前置把登录请求 1 的回调编号返回为 0，故仅 MD 登录显式兼容 0。
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
    bool matching(Stage stage, int request, RequestIdPolicy policy = RequestIdPolicy::Strict) const {
        const bool requestMatches = request_ == request ||
            (policy == RequestIdPolicy::AllowZero && request == 0);
        return active_ && !done_ && stage_ == stage && requestMatches && Clock::now() < deadline_;
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
    template <typename T> void response(Stage stage, T* data, CThostFtdcRspInfoField* info,
                                        int request, bool last,
                                        RequestIdPolicy policy = RequestIdPolicy::Strict) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matching(stage, request, policy)) return;
        result_.acceptedZeroRequestId = result_.acceptedZeroRequestId ||
            (policy == RequestIdPolicy::AllowZero && request == 0 && request_ != 0);
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
    void error(CThostFtdcRspInfoField* info, int request, bool last,
               RequestIdPolicy policy = RequestIdPolicy::Strict) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matching(stage_, request, policy)) return;
        result_.acceptedZeroRequestId = result_.acceptedZeroRequestId ||
            (policy == RequestIdPolicy::AllowZero && request == 0 && request_ != 0);
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

struct OrderResult {
    bool readyToCancel = false, done = false, ok = false, cancelAttempted = false;
    int tradedVolume = 0;
    std::string reason;
    CThostFtdcOrderField order{};
};

// 报单请求成功没有统一的“最终 OnRsp”语义；接受、成交和撤销主要由 OnRtnOrder/OnRtnTrade 驱动。
// 因此基础功能测试单独维护订单生命周期，不复用上面的 Req.../bIsLast 阶段状态机。
class OrderLifecycle {
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string orderRef_;
    bool readyToCancel_ = false, done_ = false, ok_ = false, cancelAttempted_ = false;
    bool cancelResponseKnown_ = false, cancelSubmissionAccepted_ = false, canceledObserved_ = false;
    int tradedVolume_ = 0;
    std::string reason_;
    CThostFtdcOrderField order_{};

    template <std::size_t N> bool matches(const char (&value)[N]) const {
        return trim(textField(value)) == orderRef_;
    }
    void fail(const std::string& reason) {
        if (done_) return;
        done_ = true; ok_ = false; reason_ = reason; cv_.notify_all();
    }
    OrderResult snapshot() const {
        OrderResult result;
        result.readyToCancel = readyToCancel_; result.done = done_; result.ok = ok_;
        result.cancelAttempted = cancelAttempted_; result.tradedVolume = tradedVolume_;
        result.reason = reason_; result.order = order_;
        return result;
    }
public:
    void start(std::string orderRef) {
        std::lock_guard<std::mutex> lock(mutex_);
        orderRef_ = std::move(orderRef);
        readyToCancel_ = done_ = ok_ = cancelAttempted_ = false;
        cancelResponseKnown_ = cancelSubmissionAccepted_ = canceledObserved_ = false;
        tradedVolume_ = 0; reason_.clear(); order_ = {};
    }
    const std::string& orderRef() const { return orderRef_; }

    void insertResponse(CThostFtdcInputOrderField* input, CThostFtdcRspInfoField* info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (input && !matches(input->OrderRef)) return;
        if (info && info->ErrorID != 0) fail("OnRspOrderInsert rejected the order");
    }
    void insertError(CThostFtdcInputOrderField* input, CThostFtdcRspInfoField* info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (input && !matches(input->OrderRef)) return;
        fail(info && info->ErrorID != 0 ? "OnErrRtnOrderInsert rejected the order"
                                        : "OnErrRtnOrderInsert without a nonzero error code");
    }
    void actionResponse(CThostFtdcInputOrderActionField* action, CThostFtdcRspInfoField* info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (action && !matches(action->OrderRef)) return;
        if (info && info->ErrorID != 0) fail("OnRspOrderAction rejected cancellation");
    }
    void actionError(CThostFtdcOrderActionField* action, CThostFtdcRspInfoField* info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (action && !matches(action->OrderRef)) return;
        fail(info && info->ErrorID != 0 ? "OnErrRtnOrderAction rejected cancellation"
                                        : "OnErrRtnOrderAction without a nonzero error code");
    }
    void responseError(int request, CThostFtdcRspInfoField* info) {
        if (request != 4 && request != 5) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (info && info->ErrorID != 0) fail("OnRspError for order request");
    }
    void insertSubmitted(int rc) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rc != 0) fail("order submission rejected; immediate_rc=" + std::to_string(rc));
    }
    void returnedOrder(CThostFtdcOrderField* order) {
        if (!order) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matches(order->OrderRef) || done_) return;
        order_ = *order;
        tradedVolume_ = std::max(tradedVolume_, order->VolumeTraded);
        if (order->OrderSubmitStatus == THOST_FTDC_OSS_InsertRejected) {
            fail("order insert rejected by counter");
        } else if (order->OrderStatus == THOST_FTDC_OST_Canceled) {
            canceledObserved_ = true;
            if (!cancelAttempted_) {
                done_ = true; ok_ = false;
                reason_ = "order canceled before this program submitted cancellation";
            } else if (cancelResponseKnown_) {
                done_ = true;
                ok_ = cancelSubmissionAccepted_;
                reason_ = ok_ ? "order accepted and remaining quantity canceled"
                              : "order canceled but this program's cancellation submission was rejected";
            }
            cv_.notify_all();
        } else if (order->OrderStatus == THOST_FTDC_OST_AllTraded) {
            done_ = true; ok_ = false;
            reason_ = "order fully traded before cancellation; financial effect occurred and cancel was not verified";
            cv_.notify_all();
        } else if (order->OrderStatus == THOST_FTDC_OST_PartTradedNotQueueing ||
                   order->OrderStatus == THOST_FTDC_OST_NoTradeNotQueueing) {
            fail("order is no longer queued and was not canceled by this program");
        } else if (order->OrderStatus == THOST_FTDC_OST_PartTradedQueueing ||
                   order->OrderStatus == THOST_FTDC_OST_NoTradeQueueing) {
            readyToCancel_ = true;
            cv_.notify_all();
        }
    }
    void returnedTrade(CThostFtdcTradeField* trade) {
        if (!trade) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (matches(trade->OrderRef)) tradedVolume_ += trade->Volume;
    }
    OrderResult waitUntilCancelableOrDone(int timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(timeout), [this] { return readyToCancel_ || done_; }))
            fail("order status timeout before a cancelable or terminal callback");
        return snapshot();
    }
    bool beginCancellation() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_ || !readyToCancel_) return false;
        cancelAttempted_ = true;
        return true;
    }
    OrderResult current() {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot();
    }
    void cancellationSubmitted(int rc) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelResponseKnown_ = true;
        cancelSubmissionAccepted_ = rc == 0;
        if (rc != 0) {
            fail("cancellation submission rejected; immediate_rc=" + std::to_string(rc));
        } else if (canceledObserved_ && !done_) {
            done_ = true; ok_ = true; reason_ = "order accepted and remaining quantity canceled";
            cv_.notify_all();
        }
    }
    OrderResult waitUntilDone(int timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(timeout), [this] { return done_; }))
            fail("cancellation result timeout; check the live order at the counter immediately");
        return snapshot();
    }
};

void logCallback(Logger& log, const char* channel, const char* name,
                 int request, bool last, CThostFtdcRspInfoField* info) {
    std::string message = std::string(channel) + " CALLBACK " + name +
        " callback_request_id=" + std::to_string(request) +
        " is_last=" + std::to_string(last ? 1 : 0);
    if (info) {
        message += " ErrorID=" + std::to_string(info->ErrorID) +
                   " ErrorMsg=" + sdkMessage(*info);
    } else {
        message += " RspInfo=NULL";
    }
    log.write(message);
}

class TraderSpi final : public CThostFtdcTraderSpi {
    State& state_;
    Logger& log_;
    OrderLifecycle* order_;
public:
    TraderSpi(State& state, Logger& log, OrderLifecycle* order = nullptr)
        : state_(state), log_(log), order_(order) {}
    void OnFrontConnected() override { log_.write("TRADER CALLBACK OnFrontConnected"); state_.connected(); }
    void OnFrontDisconnected(int reason) override {
        log_.write("TRADER CALLBACK OnFrontDisconnected reason=" + std::to_string(reason));
        state_.disconnected(reason);
    }
    void OnHeartBeatWarning(int lapse) override {
        log_.write("TRADER CALLBACK OnHeartBeatWarning time_lapse=" + std::to_string(lapse));
    }
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "TRADER", "OnRspAuthenticate", id, last, e);
        state_.response(Stage::Authenticate, p, e, id, last);
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "TRADER", "OnRspUserLogin", id, last, e);
        state_.response(Stage::Login, p, e, id, last);
    }
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "TRADER", "OnRspQryTradingAccount", id, last, e);
        state_.response(Stage::Account, p, e, id, last);
    }
    void OnRspOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "TRADER", "OnRspOrderInsert", id, last, e);
        if (order_) order_->insertResponse(p, e);
    }
    void OnRspOrderAction(CThostFtdcInputOrderActionField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "TRADER", "OnRspOrderAction", id, last, e);
        if (order_) order_->actionResponse(p, e);
    }
    void OnRtnOrder(CThostFtdcOrderField* p) override {
        if (!p) { log_.write("TRADER CALLBACK OnRtnOrder payload=NULL"); return; }
        std::ostringstream line;
        line << "TRADER CALLBACK OnRtnOrder order_ref=" << textField(p->OrderRef)
             << " instrument=" << textField(p->InstrumentID) << " exchange=" << textField(p->ExchangeID)
             << " order_sys_id=" << textField(p->OrderSysID)
             << " submit_status_code=" << p->OrderSubmitStatus
             << " submit_status=" << orderSubmitStatusName(p->OrderSubmitStatus)
             << " order_status_code=" << p->OrderStatus
             << " order_status=" << orderStatusName(p->OrderStatus)
             << " original_volume=" << p->VolumeTotalOriginal << " traded_volume=" << p->VolumeTraded
             << " remaining_volume=" << p->VolumeTotal
             << " status_msg=" << sdkText(textField(p->StatusMsg));
        log_.write(line.str());
        if (order_) order_->returnedOrder(p);
    }
    void OnRtnTrade(CThostFtdcTradeField* p) override {
        if (!p) { log_.write("TRADER CALLBACK OnRtnTrade payload=NULL"); return; }
        std::ostringstream line;
        line << "TRADER CALLBACK OnRtnTrade order_ref=" << textField(p->OrderRef)
             << " instrument=" << textField(p->InstrumentID) << " exchange=" << textField(p->ExchangeID)
             << " order_sys_id=" << textField(p->OrderSysID) << " trade_id=" << textField(p->TradeID)
             << " price=" << std::fixed << std::setprecision(6) << p->Price << " volume=" << p->Volume;
        log_.write(line.str());
        if (order_) order_->returnedTrade(p);
    }
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* e) override {
        std::string line = "TRADER CALLBACK OnErrRtnOrderInsert order_ref=" +
            (p ? textField(p->OrderRef) : std::string("NULL"));
        if (e) line += " ErrorID=" + std::to_string(e->ErrorID) + " ErrorMsg=" + sdkMessage(*e);
        else line += " RspInfo=NULL";
        log_.write(line);
        if (order_) order_->insertError(p, e);
    }
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* p, CThostFtdcRspInfoField* e) override {
        std::string line = "TRADER CALLBACK OnErrRtnOrderAction order_ref=" +
            (p ? textField(p->OrderRef) : std::string("NULL"));
        if (e) line += " ErrorID=" + std::to_string(e->ErrorID) + " ErrorMsg=" + sdkMessage(*e);
        else line += " RspInfo=NULL";
        log_.write(line);
        if (order_) order_->actionError(p, e);
    }
    void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "TRADER", "OnRspError", id, last, e);
        state_.error(e, id, last);
        if (order_) order_->responseError(id, e);
    }
};
class MdSpi final : public CThostFtdcMdSpi {
    State& state_;
    Logger& log_;
public:
    MdSpi(State& state, Logger& log) : state_(state), log_(log) {}
    void OnFrontConnected() override { log_.write("MD CALLBACK OnFrontConnected"); state_.connected(); }
    void OnFrontDisconnected(int reason) override {
        log_.write("MD CALLBACK OnFrontDisconnected reason=" + std::to_string(reason));
        state_.disconnected(reason);
    }
    void OnHeartBeatWarning(int lapse) override {
        log_.write("MD CALLBACK OnHeartBeatWarning time_lapse=" + std::to_string(lapse));
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "MD", "OnRspUserLogin", id, last, e);
        state_.response(Stage::Login, p, e, id, last, RequestIdPolicy::AllowZero);
    }
    void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "MD", "OnRspError", id, last, e);
        state_.error(e, id, last, RequestIdPolicy::AllowZero);
    }
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
    if (r.acceptedZeroRequestId)
        message += "; callback_request_id=0 accepted for MD compatibility";
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
                                        Stage stage, int id, int timeout, Submit submit,
                                        Result* captured = nullptr) {
    log.write(std::string(channel) + " stage=" + stageName(stage) + " START request_id=" + std::to_string(id) +
              " timeout_seconds=" + std::to_string(timeout));
    if (state.begin(stage, id, timeout)) {
        const int rc = submit();
        state.immediate(rc);
        if (stage != Stage::Connect) log.write(std::string(channel) + " request_id=" + std::to_string(id) +
                                               " immediate_rc=" + std::to_string(rc) + " (submission only)");
    }
    const Result result = state.wait();
    if (captured) *captured = result;
    return report(log, channel, stage, id, result);
}
CThostFtdcReqUserLoginField loginRequest(const Config& c, const Secrets& s) {
    CThostFtdcReqUserLoginField request{};
    field(request.BrokerID, c.broker, "broker_id"); field(request.UserID, c.user, "user_id");
    field(request.Password, s.password, "password");
    // 此处不手填 IP/MAC，不伪造终端信息；SDK 实际采集和后台核验情况须在实体机登录后向券商确认。
    return request;
}
CThostFtdcInputOrderActionField basicCancelRequest(const Config& c, const Options& o,
                                                    const CThostFtdcOrderField& order,
                                                    const std::string& expectedOrderRef) {
    CThostFtdcInputOrderActionField action{};
    field(action.BrokerID, c.broker, "broker_id");
    field(action.InvestorID, c.investor, "investor_id");
    field(action.UserID, c.user, "user_id");
    const std::string instrument = trim(textField(order.InstrumentID));
    const std::string exchange = trim(textField(order.ExchangeID));
    const std::string returnedOrderRef = trim(textField(order.OrderRef));
    field(action.InstrumentID, instrument.empty() ? o.instrument : instrument, "instrument");
    field(action.ExchangeID, exchange.empty() ? o.exchange : exchange, "exchange");
    field(action.OrderRef, returnedOrderRef.empty() ? expectedOrderRef : returnedOrderRef, "order_ref");
    const std::string orderSysId = textField(order.OrderSysID);
    if (!orderSysId.empty()) field(action.OrderSysID, orderSysId, "order_sys_id");
    action.FrontID = order.FrontID;
    action.SessionID = order.SessionID;
    action.OrderActionRef = 1;
    action.RequestID = 5;
    action.ActionFlag = THOST_FTDC_AF_Delete;
    return action;
}
bool testBasicFunction(CThostFtdcTraderApi& api, const Config& c, const Options& o,
                       const Result& login, OrderLifecycle& lifecycle, Logger& log) {
    const std::string orderRef = nextOrderRef(login.login);
    const auto order = basicOrderRequest(c, o, orderRef);
    log.write("BASIC STRATEGY PLAN " + orderPlanText(o, orderRef));
    if (!o.sendOrder) {
        log.write("BASIC RESULT status=PASS strategy=PASS order_fields=PASS transmission=NOT_REQUESTED cancel=NOT_RUN");
        log.write("BASIC NOTICE dry-run proves deterministic strategy/request construction only; it does not prove counter order acceptance");
        return true;
    }

    const std::string tradingDay = trim(textField(login.login.TradingDay));
    DailyOrderCounter counter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
    const auto risk = counter.reserve(tradingDay);
    log.write("RISK CHECK rule=daily_max_order_count configured_limit=" +
              std::to_string(risk.configuredLimit) +
              " submitted_before=" + std::to_string(risk.submittedBefore) +
              " submitted_after=" + std::to_string(risk.submittedAfter) +
              " trading_day=" + risk.tradingDay +
              " decision=" + (risk.allowed ? "ALLOW" : "BLOCK") +
              " counting_rule=ReqOrderInsert_attempts state_file=" +
              fs::absolute(dailyOrderStatePath(c)).string());
    if (!risk.allowed) {
        log.write("RISK TRIGGER rule=daily_max_order_count decision=BLOCK api_call=NOT_SENT");
        showRiskTriggerDialog(risk, false);
        log.write("BASIC RESULT status=FAIL strategy=PASS order=BLOCKED_BY_RISK cancel=NOT_RUN "
                  "traded_volume=0 reason=daily maximum order count reached");
        return false;
    }

    lifecycle.start(orderRef);
    log.write("BASIC LIVE_ORDER_START exactly_one_order=YES automatic_retry=NO automatic_reprice=NO");
    auto request = order; // SDK 接口不是 const；缓冲在整个等待阶段保持有效。
    const int insertRc = api.ReqOrderInsert(&request, 4);
    log.write("BASIC CALL ReqOrderInsert request_id=4 immediate_rc=" + std::to_string(insertRc) +
              " (submission only)");
    lifecycle.insertSubmitted(insertRc);
    const OrderResult accepted = lifecycle.waitUntilCancelableOrDone(o.timeout);
    if (accepted.done) {
        log.write("BASIC RESULT status=FAIL strategy=PASS order=NOT_CANCELABLE cancel=NOT_RUN traded_volume=" +
                  std::to_string(accepted.tradedVolume) + " reason=" + accepted.reason);
        return false;
    }

    auto action = basicCancelRequest(c, o, accepted.order, orderRef);
    // 必须在调用 SDK 前原子确认订单仍可撤并设置标志，兼容排队后立刻成交和调用期间回调。
    if (!lifecycle.beginCancellation()) {
        const OrderResult changed = lifecycle.current();
        log.write("BASIC RESULT status=FAIL strategy=PASS order=NOT_CANCELABLE cancel=NOT_RUN traded_volume=" +
                  std::to_string(changed.tradedVolume) + " reason=" + changed.reason);
        return false;
    }
    const int cancelRc = api.ReqOrderAction(&action, 5);
    log.write("BASIC CALL ReqOrderAction request_id=5 immediate_rc=" + std::to_string(cancelRc) +
              " order_ref=" + orderRef + " order_sys_id=" + textField(accepted.order.OrderSysID) +
              " (submission only)");
    lifecycle.cancellationSubmitted(cancelRc);
    const OrderResult final = lifecycle.waitUntilDone(o.timeout);
    log.write(std::string("BASIC RESULT status=") + (final.ok ? "PASS" : "FAIL") +
              " strategy=PASS order=PASS cancel=" + (final.ok ? "PASS" : "FAIL") +
              " traded_volume=" + std::to_string(final.tradedVolume) + " reason=" + final.reason);
    return final.ok;
}
bool testTrader(const Config& c, const Secrets& s, const Options& o, const fs::path& flow, Logger& log) {
    State state;
    OrderLifecycle orderLifecycle;
    TraderSpi spi(state, log, o.test == "basic" ? &orderLifecycle : nullptr);
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
    // QUICK 从登录后的新消息开始；不会重放历史私有/公共流。基础测试处理本次新报单/成交回报。
    api->SubscribePrivateTopic(THOST_TERT_QUICK); api->SubscribePublicTopic(THOST_TERT_QUICK);
    api->RegisterFront(front.data());
    if (!runStage(state, log, "TRADER", Stage::Connect, 0, o.timeout, [&] { api->Init(); return 0; })) return false;
    if (!runStage(state, log, "TRADER", Stage::Authenticate, 1, o.timeout, [&] { return api->ReqAuthenticate(&auth, 1); })) return false;
    log.write("TRADER LOGIN_START user=" + c.user + " broker=" + c.broker + " app_id=" + c.app);
    Result loginResult;
    if (!runStage(state, log, "TRADER", Stage::Login, 2, o.timeout,
                  [&] { return api->ReqUserLogin(&login, 2); }, &loginResult)) return false;
    if (o.skipQuery) {
        log.write("TRADER account query SKIPPED (--skip-query)");
    } else {
        // 不猜测账户币种/业务类型；其他筛选字段保持 SDK 零初始化值，读取该投资者返回的数据。
        if (!runStage(state, log, "TRADER", Stage::Account, 3, o.timeout,
                      [&] { return api->ReqQryTradingAccount(&query, 3); })) return false;
    }
    return o.test == "basic" ? testBasicFunction(*api, c, o, loginResult, orderLifecycle, log) : true;
}
bool testMd(const Config& c, const Secrets& s, const Options& o, const fs::path& flow, Logger& log) {
    State state;
    MdSpi spi(state, log);
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
                         "       --mode trader --test basic --instrument ID --exchange SSE|SZSE\n"
                         "       --direction buy|sell --offset open|close --price PRICE\n"
                         "       [--send-order --confirm SEND_ONE_ORDER]\n"
                         "       --test risk --risk-action settings\n"
                         "       --test risk --risk-action trigger --confirm TRIGGER_DAILY_ORDER_LIMIT\n"
                         "Default: all, config/connection.ini, 30 seconds PER STAGE, account query enabled.\n"
                         "Credentials: nonempty local INI > main INI > CTP_PASSWORD / CTP_AUTH_CODE > hidden prompts.\n"
                         "config/connection.local.ini is loaded automatically when using the default config.\n"
                         "Basic test defaults to dry-run. --send-order transmits one GFD limit order with volume=1\n"
                         "and attempts to cancel it; it may trade before cancellation. No automatic retry/reprice.\n"
                         "Risk evidence mode displays screenshot dialogs without credentials, network, or order calls.\n"
                         "MD-only mode does not need CTP_AUTH_CODE. No password-update operation.\n";
            return 0;
        }
        printVersions();
        if (options.version) return 0; // help/version 不读配置、不索取密码、不连接网络。
        const Config config = readConfig(options.config, secret);
        if (options.test == "risk") return runRiskEvidence(config, options, secret) ? 0 : 1;
        if (options.test == "basic" && options.sendOrder && config.dailyMaxOrderCount < 1)
            throw std::runtime_error("Live order transmission requires daily_max_order_count in config/connection.local.ini.");
        secret.password = getSecret(secret.password, "CTP_PASSWORD", "Trading password (hidden): ");
        if (options.mode != "md") secret.auth = getSecret(secret.auth, "CTP_AUTH_CODE", "Authentication code (hidden): ");
        // 先校验所有固定长度字段，避免连上前置后才发现输入被截断或无效。
        (void)loginRequest(config, secret);
        CThostFtdcReqAuthenticateField check{};
        field(check.AppID, config.app, "app_id");
        if (options.mode != "md") field(check.AuthCode, secret.auth, "auth_code");
        CThostFtdcQryTradingAccountField qcheck{};
        field(qcheck.InvestorID, config.investor, "investor_id");
        if (options.test == "basic") (void)basicOrderRequest(config, options, "1");
        std::string runId;
        const fs::path logDir = createRunLogDirectory(runId);
        fs::create_directories("flow");
        const fs::path flowDir = fs::path("flow") / runId;
        if (fs::exists(flowDir)) throw std::runtime_error("Flow directory collision; rerun to obtain a new run ID.");
        fs::create_directories(flowDir / "trader"); fs::create_directories(flowDir / "md");
        Logger log(logDir / "run.log", secret);
        log.write(std::string("PROGRAM version=") + kVersion + " sdk_package=traderAPI_3.7.5_CP_20251125");
        log.write(std::string("API trader=") + CThostFtdcTraderApi::GetApiVersion() + " md=" + CThostFtdcMdApi::GetApiVersion());
        log.write("RUN id=" + runId + " mode=" + options.mode + " test=" + options.test +
                  " user=" + config.user + " broker=" + config.broker);
        log.write("CONFIG app_id=" + config.app + " investor_id=" + config.investor);
        log.write("RISK CONFIG rule=daily_max_order_count configured_limit=" +
                  (config.dailyMaxOrderCount > 0 ? std::to_string(config.dailyMaxOrderCount) : "NOT_CONFIGURED") +
                  " unit=orders counting_rule=ReqOrderInsert_attempts period=trading_day");
        log.write("FRONTS trader=" + config.trader + " md=" + config.md);
        log.write("TIME timestamps=host_local_wall_clock; elapsed_timeouts=monotonic; check local clock before evidence capture");
        const std::string admin = administratorStatus();
        log.write("HOST administrator=" + admin + " physical_machine=NOT_VERIFIED (operator must confirm)");
        if (admin != "YES") log.write("NOTICE formal evaluation login requires an elevated Windows console on a physical machine");
        if (options.test == "basic") {
            log.write(std::string("SCOPE connect/auth/login/read-only-account-query; single-shot limit strategy; ") +
                      (options.sendOrder ? "exactly one order with volume=1 and cancellation enabled"
                                         : "dry-run only; no order transmitted"));
        } else {
            log.write("SCOPE connect/auth/login/read-only-account-query; no market subscription, order, cancel, settlement or password change");
        }
        bool traderOk = true, mdOk = true;
        if (options.mode != "md") traderOk = testTrader(config, secret, options, flowDir / "trader", log);
        // all 模式中行情是独立诊断项；交易失败后仍只执行一次行情连接和登录。
        if (options.mode != "trader") mdOk = testMd(config, secret, options, flowDir / "md", log);
        log.write(std::string("RESULT overall=") + (traderOk && mdOk ? "PASS" : "FAIL") +
                  " trader=" + (options.mode == "md" ? "NOT_RUN" : traderOk ? "PASS" : "FAIL") +
                  " md=" + (options.mode == "trader" ? "NOT_RUN" : mdOk ? "PASS" : "FAIL") +
                  " basic=" + (options.test != "basic" ? "NOT_RUN" : traderOk ? "PASS" : "FAIL"));
        log.write("EVIDENCE log=" + fs::absolute(logDir / "run.log").string());
        log.write("NEXT successful login should be reported to broker the same day; this program does not notify anyone");
        return traderOk && mdOk ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << timestamp() << " FATAL " << clean(error.what(), secret) << '\n';
        return 2;
    }
}
