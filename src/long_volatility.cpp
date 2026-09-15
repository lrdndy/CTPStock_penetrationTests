// Standalone strategy entry: reuse published connectivity/order/risk code.
// The connectivity application's main function is compiled but never called.
#define main connectivity_entry_unused
#include "main.cpp"
#undef main
#include "long_volatility_core.hpp"
#include <atomic>

namespace long_vol {
Settings parse(int argc, char** argv) {
    Settings s; std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!seen.insert(arg).second) throw std::runtime_error("参数重复：" + arg);
        if (arg == "--help" || arg == "-h") { s.help = true; continue; }
        if (arg == "--send-order") { s.send = true; continue; }
        if (arg == "--no-prompt") { s.noPrompt = true; continue; }
        if (arg != "--config" && arg != "--exchange" && arg != "--call" && arg != "--put" &&
            arg != "--call-price" && arg != "--put-price" && arg != "--confirm" &&
            arg != "--timeout" && arg != "--fill-wait") throw std::runtime_error("未知参数：" + arg);
        if (++i == argc) throw std::runtime_error("参数缺少值：" + arg);
        const std::string value = argv[i];
        if (arg == "--config") s.config = value;
        else if (arg == "--exchange") s.exchange = value;
        else if (arg == "--call") s.call = value;
        else if (arg == "--put") s.put = value;
        else if (arg == "--confirm") s.confirm = value;
        else if (arg == "--call-price") s.callPrice = decimalPrice(value);
        else if (arg == "--put-price") s.putPrice = decimalPrice(value);
        else {
            if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("等待秒数必须为 1..300 的整数。");
            const int seconds = std::stoi(value);
            if (seconds < 1 || seconds > 300) throw std::runtime_error("等待秒数必须为 1..300 的整数。");
            if (arg == "--timeout") s.timeout = seconds; else s.fillWait = seconds;
        }
    }
    if (s.exchange != "SSE" && s.exchange != "SZSE") throw std::runtime_error("交易所须为 SSE 或 SZSE。");
    if (s.send && s.confirm != confirmation) throw std::runtime_error("实发两腿须指定 --confirm SEND_LONG_VOLATILITY_ORDERS。");
    if (!s.send && !s.confirm.empty()) throw std::runtime_error("演练模式不接受实发确认口令。");
    return s;
}
std::string input(const std::string& question, bool noPrompt) {
    if (noPrompt) throw std::runtime_error("缺少输入或有效行情，--no-prompt 模式已停止。");
    std::cout << "做多波动率策略：" << question << std::flush;
    std::string value;
    if (!std::getline(std::cin, value) || trim(value).empty()) throw std::runtime_error("未提供输入，已停止。");
    return trim(value);
}
void completeContracts(Settings& s) {
    if (s.call.empty()) s.call = input("请输入认购期权合约代码：", s.noPrompt);
    if (s.put.empty()) s.put = input("请输入认沽期权合约代码：", s.noPrompt);
    for (const auto& id : {s.call, s.put})
        if (id.size() != 8 || id.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("请输入 8 位股票期权合约代码，不能填写 ETF 代码。");
    if (s.call == s.put) throw std::runtime_error("认购和认沽不能使用同一个合约代码。");
}

// One outstanding query per type. Only the matching final callback completes it.
template<class T> class Query {
    std::mutex mutex_; std::condition_variable cv_;
    int id_ = -1; bool active_ = false, done_ = false;
    Clock::time_point deadline_{};
    std::string error_; std::vector<T> rows_;
public:
    void begin(int id, int seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        id_ = id; active_ = true; done_ = false; error_.clear(); rows_.clear();
        deadline_ = Clock::now() + std::chrono::seconds(seconds);
    }
    bool error(int id, const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || done_ || id != id_) return false;
        error_ = error; done_ = true; cv_.notify_all(); return true;
    }
    void disconnected() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ && !done_) { error_ = "连接断开，查询已停止。"; done_ = true; cv_.notify_all(); }
    }
    void response(T* row, CThostFtdcRspInfoField* info, int id, bool last) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || done_ || id != id_ || Clock::now() >= deadline_) return;
        if (info && info->ErrorID) { error_ = sdkMessage(*info); done_ = true; }
        else {
            if (row) {
                if (rows_.size() >= 2) { error_ = "精确查询返回过多记录。"; done_ = true; }
                else rows_.push_back(*row);
            }
            if (last) done_ = true;
        }
        if (done_) cv_.notify_all();
    }
    T wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool received = cv_.wait_until(lock, deadline_, [&] { return done_; });
        active_ = false;
        if (!received) throw std::runtime_error("查询超时，迟到回调不再采用。");
        if (!error_.empty()) throw std::runtime_error(error_);
        if (rows_.size() != 1) throw std::runtime_error("精确查询未返回唯一记录。");
        return rows_[0];
    }
};
class Spi final : public CThostFtdcTraderSpi {
    TraderSpi base_; Logger& log_;
public:
    Query<CThostFtdcInstrumentField> instruments;
    Query<CThostFtdcDepthMarketDataField> quotes;
    std::atomic<bool> connected{false};
    Spi(State& state, Logger& log, BatchOrders& batch) : base_(state, log, nullptr, &batch), log_(log) {}
    void OnFrontConnected() override { connected = true; base_.OnFrontConnected(); }
    void OnFrontDisconnected(int r) override {
        connected = false; instruments.disconnected(); quotes.disconnected(); base_.OnFrontDisconnected(r);
    }
    void OnHeartBeatWarning(int n) override { base_.OnHeartBeatWarning(n); }
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* p, CThostFtdcRspInfoField* e, int id, bool last) override { base_.OnRspAuthenticate(p, e, id, last); }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField* e, int id, bool last) override { base_.OnRspUserLogin(p, e, id, last); }
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField* e, int id, bool last) override { base_.OnRspQryTradingAccount(p, e, id, last); }
    void OnRspOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* e, int id, bool last) override { base_.OnRspOrderInsert(p, e, id, last); }
    void OnRspOrderAction(CThostFtdcInputOrderActionField* p, CThostFtdcRspInfoField* e, int id, bool last) override { base_.OnRspOrderAction(p, e, id, last); }
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* e) override { base_.OnErrRtnOrderInsert(p, e); }
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* p, CThostFtdcRspInfoField* e) override { base_.OnErrRtnOrderAction(p, e); }
    void OnRtnOrder(CThostFtdcOrderField* p) override { base_.OnRtnOrder(p); }
    void OnRtnTrade(CThostFtdcTradeField* p) override {
        base_.OnRtnTrade(p); // Lifecycle correlates session/order identity before claiming a fill.
    }
    void OnRspQryInstrument(CThostFtdcInstrumentField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "LONG_VOL", "OnRspQryInstrument", id, last, e); instruments.response(p, e, id, last);
    }
    void OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField* p, CThostFtdcRspInfoField* e, int id, bool last) override {
        logCallback(log_, "LONG_VOL", "OnRspQryDepthMarketData", id, last, e); quotes.response(p, e, id, last);
    }
    void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override {
        const std::string message = e && e->ErrorID ? sdkMessage(*e) : "查询收到无有效错误码的 OnRspError。";
        if (instruments.error(id, message) || quotes.error(id, message)) {
            logCallback(log_, "LONG_VOL", "OnRspError", id, last, e); return;
        }
        // Late query errors cannot interrupt a later order stage.
        if (id >= 10 && id < 100) { logCallback(log_, "LONG_VOL_LATE_QUERY", "OnRspError", id, last, e); return; }
        base_.OnRspError(e, id, last);
    }
};

bool run(const Config& c, const Secrets& secret, const Settings& s, const fs::path& flow, Logger& log) {
    State state; BatchOrders batch; Spi spi(state, log, batch);
    const std::string flowPath = flow.generic_string() + "/"; std::string front = c.trader;
    CThostFtdcReqAuthenticateField auth{};
    field(auth.BrokerID, c.broker, "broker_id"); field(auth.UserID, c.user, "user_id");
    field(auth.AppID, c.app, "app_id"); field(auth.AuthCode, secret.auth, "auth_code");
    auto login = loginRequest(c, secret);
    CThostFtdcQryTradingAccountField account{};
    field(account.BrokerID, c.broker, "broker_id"); field(account.InvestorID, c.investor, "investor_id");
    // Request buffers and batch remain alive until API Release has joined its threads.
    std::array<CThostFtdcQryInstrumentField, 2> contractRequests{};
    std::array<CThostFtdcQryDepthMarketDataField, 4> quoteRequests{};
    std::array<Leg, 2> legs{};
    legs[0].label = "认购腿"; legs[0].instrument = s.call;
    legs[1].label = "认沽腿"; legs[1].instrument = s.put;
    std::unique_ptr<CThostFtdcTraderApi, ApiDeleter<CThostFtdcTraderApi>> api(CThostFtdcTraderApi::CreateFtdcTraderApi(flowPath.c_str()));
    if (!api) throw std::runtime_error("无法创建交易 API。");
    api->RegisterSpi(&spi); api->SubscribePrivateTopic(THOST_TERT_QUICK); api->SubscribePublicTopic(THOST_TERT_QUICK);
    api->RegisterFront(front.data());
    if (!runStage(state, log, "TRADER", Stage::Connect, 0, s.timeout, [&] { api->Init(); return 0; })) return false;
    if (!runStage(state, log, "TRADER", Stage::Authenticate, 1, s.timeout, [&] { return api->ReqAuthenticate(&auth, 1); })) return false;
    Result loginResult;
    if (!runStage(state, log, "TRADER", Stage::Login, 2, s.timeout, [&] { return api->ReqUserLogin(&login, 2); }, &loginResult)) return false;
    const auto loginClock = Clock::now();
    const int loginSeconds = secondsOfDay(textField(loginResult.login.LoginTime));
    const std::string day = textField(loginResult.login.TradingDay);
    if (!runStage(state, log, "TRADER", Stage::Account, 3, s.timeout, [&] { return api->ReqQryTradingAccount(&account, 3); })) return false;
    auto lastQuery = Clock::now(); int queryId = 10;
    auto paceQuery = [&] {
        std::this_thread::sleep_until(lastQuery + std::chrono::milliseconds(1100));
        if (!spi.connected || batch.hasFailure()) throw std::runtime_error("连接异常，停止后续查询及报单。");
        lastQuery = Clock::now();
    };
    for (std::size_t i = 0; i < 2; ++i) {
        auto& req = contractRequests[i]; field(req.InstrumentID, legs[i].instrument, "instrument"); field(req.ExchangeID, s.exchange, "exchange");
        paceQuery(); const int id = queryId++; spi.instruments.begin(id, s.timeout);
        const int rc = api->ReqQryInstrument(&req, id);
        if (rc) spi.instruments.error(id, "合约查询提交失败，返回=" + std::to_string(rc));
        legs[i].contract = spi.instruments.wait();
    }
    validatePair(legs, s.exchange, day);
    log.write("做多波动率策略：合约配对验证通过，标的=" + textField(legs[0].contract.UnderlyingInstrID) +
              " 到期日=" + textField(legs[0].contract.ExpireDate) + " 行权价=" + number(legs[0].contract.StrikePrice));
    std::size_t quoteIndex = 0;
    auto ask = [&](Leg& leg) {
        if (quoteIndex >= quoteRequests.size()) throw std::runtime_error("报价查询次数超出本次预算。");
        auto& req = quoteRequests[quoteIndex++]; field(req.InstrumentID, leg.instrument, "instrument"); field(req.ExchangeID, s.exchange, "exchange");
        paceQuery(); const int id = queryId++; spi.quotes.begin(id, std::min(s.timeout, 10));
        const int rc = api->ReqQryDepthMarketData(&req, id);
        if (rc) spi.quotes.error(id, "行情查询提交失败，返回=" + std::to_string(rc));
        const auto q = spi.quotes.wait(); const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - loginClock).count();
        const int serverSeconds = loginSeconds < 0 ? -1 : loginSeconds + static_cast<int>(elapsed);
        if (!useAsk(leg, q, s, day, serverSeconds, now)) throw std::runtime_error("无有效卖一价、无卖盘数量、行情过期或时间信息不完整。");
        log.write("做多波动率策略：" + leg.label + "行情卖一=" + number(leg.price) + " 行情时间=" + textField(q.UpdateTime));
    };
    bool prompted = false;
    for (std::size_t i = 0; i < 2; ++i) {
        auto& leg = legs[i]; const double manual = i == 0 ? s.callPrice : s.putPrice;
        if (manual > 0) { leg.price = manual; leg.source = "手动指定限价"; }
        else {
            try { ask(leg); }
            catch (const std::exception& e) {
                log.write("做多波动率策略：" + leg.label + "无法自动报价，原因=" + e.what());
                leg.price = decimalPrice(input(leg.label + "请手动输入买入限价：", s.noPrompt));
                leg.source = "行情不可用，手动输入限价"; leg.deadline = Clock::time_point::max(); prompted = true;
            }
        }
        validatePrice(leg);
    }
    // All manual input happens before the first order. Refresh any automatic
    // quote after an interactive pause; never reuse a pre-prompt stale quote.
    if (prompted) for (auto& leg : legs) if (leg.source == "行情卖一价（限价报单）") ask(leg);
    if (!spi.connected || batch.hasFailure()) throw std::runtime_error("连接异常，禁止开始双腿报单。");
    log.write("做多波动率策略：两腿各买入开仓 1 张；顺序发送，中间间隔约 1.1 秒；不保证同时成交。");
    return execute(*api, c, s, loginResult.login, legs, batch, log);
}
int application(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
#endif
    Secrets secret;
    try {
        auto s = parse(argc, argv);
        if (s.help) {
            std::cout << "做多波动率策略（买入跨式） " << version << "\n"
                "--call 认购代码 --put 认沽代码 --exchange SSE|SZSE\n"
                "--call-price 价格 --put-price 价格：可选，手动指定买入限价。\n"
                "不指定价格：查询有效卖一价；查询失败则提示手动输入。\n"
                "--timeout 1..300 --fill-wait 1..300 --config 文件 --no-prompt\n"
                "默认只查询并演练，不报单。实发两腿须同时给出：\n"
                "--send-order --confirm SEND_LONG_VOLATILITY_ORDERS\n"
                "每腿固定买入开仓 1 张，均为限价单；不保证成交，不自动追价或平仓。\n";
            return 0;
        }
        completeContracts(s);
        const Config c = readConfig(s.config, secret);
        if (s.send && (c.dailyMaxOrderCount < 2 || c.perSecondMaxOrderCount < 1))
            throw std::runtime_error("请按报备表配置每日及每秒报单上限；本策略需至少 2 笔每日报单额度。");
        secret.password = getSecret(secret.password, "CTP_PASSWORD", "交易密码（隐藏输入）：");
        secret.auth = getSecret(secret.auth, "CTP_AUTH_CODE", "认证码（隐藏输入）：");
        std::string runId; const auto logDir = createRunLogDirectory(runId); Logger log(logDir / "run.log", secret);
        log.write(std::string("做多波动率策略：启动 strategy_version=") + version + " connectivity_version=" + kVersion);
        log.write(std::string("交易 API：") + CThostFtdcTraderApi::GetApiVersion());
        log.write(std::string("做多波动率策略：运行模式=") + (s.send ? "真实报单，最多 2 笔，各 1 张" : "查询与演练，不报单"));
        log.write("做多波动率策略：每日上限=" + std::to_string(c.dailyMaxOrderCount) + " 每秒上限=" + std::to_string(c.perSecondMaxOrderCount));
        const auto flow = fs::path("flow") / runId / "long_vol"; fs::create_directories(flow);
        bool ok = false;
        try { ok = run(c, secret, s, flow, log); }
        catch (const std::exception& e) { log.write(std::string("做多波动率策略：启动或查询失败，") + e.what()); }
        log.write(std::string("做多波动率策略：最终结果=") + (ok ? "通过" : "未通过") + (s.send ? " 模式=实发" : " 模式=演练，未测试成交"));
        log.write("EVIDENCE log=" + fs::absolute(logDir / "run.log").string()); return ok ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << "做多波动率策略：错误，" << clean(e.what(), secret) << '\n'; return 2; }
}
} // namespace long_vol
#ifndef LONG_VOL_TESTING
int main(int argc, char** argv) { return long_vol::application(argc, argv); }
#endif
