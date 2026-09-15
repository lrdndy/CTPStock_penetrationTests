#define LONG_VOL_TESTING
#include "../src/long_volatility.cpp"
#include <limits>

namespace {
using long_vol::Leg;
using long_vol::Settings;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected rejection");
}
Settings settings() {
    Settings s; s.call = "10000001"; s.put = "10000002";
    s.send = true; s.confirm = long_vol::confirmation; s.fillWait = 1; s.timeout = 1; return s;
}
Config config() {
    Config c; c.user = "OFFLINE_VOL"; c.investor = c.user;
    c.dailyMaxOrderCount = 10; c.perSecondMaxOrderCount = 10; return c;
}
CThostFtdcRspUserLoginField login() {
    CThostFtdcRspUserLoginField l{}; field(l.TradingDay, "20260915", "day");
    field(l.MaxOrderRef, "10", "ref"); l.FrontID = 1; l.SessionID = 77; return l;
}
std::array<Leg, 2> pair() {
    std::array<Leg, 2> legs{};
    for (std::size_t i = 0; i < 2; ++i) {
        auto& leg = legs[i]; leg.label = i ? "认沽腿" : "认购腿"; leg.instrument = i ? "10000002" : "10000001";
        leg.price = i ? 0.06 : 0.04; leg.source = "OFFLINE_FIXTURE";
        auto& c = leg.contract;
        field(c.InstrumentID, leg.instrument, "instrument"); field(c.ExchangeID, "SSE", "exchange");
        field(c.UnderlyingInstrID, "DUMMY_ETF", "underlying"); field(c.ExpireDate, "20261001", "expiry");
        c.OptionsType = i ? THOST_FTDC_CP_PutOptions : THOST_FTDC_CP_CallOptions;
        c.PriceTick = 0.0001; c.VolumeMultiple = 10000; c.UnderlyingMultiple = 1; c.StrikePrice = 3;
        c.IsTrading = true; c.MinLimitOrderVolume = 1; c.MaxLimitOrderVolume = 100; c.MinBuyVolume = 1;
    }
    return legs;
}
struct Api {
    BatchOrders& batch;
    std::array<std::string, 2> modes{"fill", "fill"};
    int inserts = 0, cancels = 0;
    bool cancelFails = false;
    std::vector<CThostFtdcInputOrderField> requests;
    BatchOrders::Entry* entry(int id, bool action = false) {
        for (auto* e : batch.entries()) if (id == (action ? e->cancelId : e->insertId)) return e;
        throw std::runtime_error("Unknown fake request id");
    }
    CThostFtdcOrderField order(BatchOrders::Entry* e) {
        auto o = e->lifecycle.current().order;
        field(o.OrderSysID, "SYS" + e->reference, "sys");
        o.VolumeTotalOriginal = 1; o.VolumeTotal = 1; o.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
        return o;
    }
    int ReqOrderInsert(CThostFtdcInputOrderField* p, int id) {
        const auto mode = modes.at(static_cast<std::size_t>(inserts++)); requests.push_back(*p);
        auto* e = entry(id);
        if (mode == "throw") throw std::runtime_error("fake submission exception");
        if (mode == "immediate-fail") return -1;
        if (mode == "unknown") return 0;
        if (mode == "reject") { CThostFtdcRspInfoField info{}; info.ErrorID = 21; batch.insertResponse(p, &info, id); return 0; }
        auto o = order(e);
        if (mode == "fill") { o.OrderStatus = THOST_FTDC_OST_AllTraded; o.VolumeTraded = 1; o.VolumeTotal = 0; }
        else if (mode == "canceled") o.OrderStatus = THOST_FTDC_OST_Canceled;
        else o.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
        batch.returnedOrder(&o); return 0;
    }
    int ReqOrderAction(CThostFtdcInputOrderActionField*, int id) {
        ++cancels; if (cancelFails) return -1;
        auto o = order(entry(id, true)); o.OrderStatus = THOST_FTDC_OST_Canceled; batch.returnedOrder(&o); return 0;
    }
};
struct Capture {
    std::ostringstream text;
    std::streambuf* previous = std::cout.rdbuf(text.rdbuf());
    ~Capture() { std::cout.rdbuf(previous); }
};
struct StdinFixture {
    std::istringstream values{"10000001\n10000002\n0.04\n0.06\nfixture-secret\n"};
    std::streambuf* previous = std::cin.rdbuf(values.rdbuf());
    ~StdinFixture() { std::cin.rdbuf(previous); }
    void untouched() { require(values.tellg() == 0, "Interactive input was consumed"); }
};
struct EnvironmentFixture {
    const char* name = "CTP_LONG_VOL_TEST_SECRET";
    std::string previous;
    bool existed = false;
    EnvironmentFixture() {
#ifdef _WIN32
        char* value = nullptr; std::size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0) throw std::runtime_error("Cannot read test environment");
        std::unique_ptr<char, decltype(&std::free)> buffer(value, &std::free);
        if (buffer) { existed = true; previous = buffer.get(); }
#else
        if (const char* value = std::getenv(name)) { existed = true; previous = value; }
#endif
    }
    void set(const char* value) {
#ifdef _WIN32
        require(_putenv_s(name, value ? value : "") == 0, "Cannot set test environment");
#else
        require((value ? setenv(name, value, 1) : unsetenv(name)) == 0, "Cannot set test environment");
#endif
    }
    ~EnvironmentFixture() { try { set(existed ? previous.c_str() : nullptr); } catch (...) {} }
};
bool execute(Api& api, const Config& c, const Settings& s, const std::array<Leg, 2>& legs,
             BatchOrders& batch, std::string& output, const std::function<void()>& pace = [] {}) {
    Secrets secret; Logger log("run.log", secret); Capture capture;
    const bool result = long_vol::execute(api, c, s, login(), legs, batch, log, pace);
    output = capture.text.str(); return result;
}
Settings cli(std::vector<std::string> args) {
    args.insert(args.begin(), "long_vol_test"); std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data()); return long_vol::parse(static_cast<int>(argv.size()), argv.data());
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto original = fs::current_path(); const auto root = fs::absolute(argv[1]);
    int passed = 0;
    auto test = [&](const char* name, const std::function<void()>& fn) {
        auto dir = root / std::to_string(passed); fs::create_directories(dir); fs::current_path(dir);
        fn(); ++passed; std::cout << "PASS " << name << '\n';
    };
    try {
        test("CLI defaults to no transmission and requires the two-leg token", [] {
            require(!cli({}).send, "Default transmits orders");
            rejects([] { cli({"--send-order"}); });
            rejects([] { cli({"--send-order", "--confirm", "SEND_ONE_ORDER"}); });
            rejects([] { cli({"--call-price", "0.04abc"}); });
            rejects([] { cli({"--put-price", "0.05", "--put-price", "0.06"}); });
            rejects([] { cli({"--direction", "sell"}); });
            require(cli({"--call-price", "0.0427", "--put-price", "0.06"}).callPrice == 0.0427, "Manual input changed");
        });
        test("contract inputs require distinct option codes", [] {
            auto s = settings(); long_vol::validateContracts(s);
            s.call = s.put; rejects([&] { long_vol::validateContracts(s); });
            s.call = "510050"; rejects([&] { long_vol::validateContracts(s); });
        });
        test("missing contracts reject without reading stdin or requiring no-prompt", [] {
            StdinFixture input; Capture output;
            for (const auto& args : {std::vector<std::string>{}, {"--call", "10000001"}, {"--put", "10000002"}, {"--no-prompt"}}) {
                auto s = cli(args); rejects([&] { long_vol::validateContracts(s); });
            }
            auto s = cli({"--call", "10000001", "--put", "10000002", "--no-prompt"});
            long_vol::validateContracts(s);
            input.untouched(); require(output.text.str().empty(), "Printed an interactive prompt");
        });
        test("command prices bypass quotes and omitted prices use automatic quotes", [] {
            StdinFixture input; auto legs = pair(); int queries = 0;
            auto ask = [&](Leg& leg) { ++queries; leg.price = 0.0627; leg.source = "FAKE_ASK"; leg.deadline = Clock::now() + std::chrono::seconds(5); };
            legs[0].deadline = Clock::now() - std::chrono::seconds(1);
            long_vol::resolvePrice(legs[0], 0.0427, "--call-price", ask);
            require(queries == 0 && legs[0].price == 0.0427 && legs[0].deadline == Clock::time_point::max(), "Manual price queried or expired");
            long_vol::resolvePrice(legs[1], 0, "--put-price", ask);
            require(queries == 1 && legs[1].price == 0.0627 && legs[1].source == "FAKE_ASK", "Automatic quote was ignored");
            rejects([&] { long_vol::resolvePrice(legs[0], 0.04275, "--call-price", ask); });
            input.untouched();
        });
        test("unavailable automatic quotes fail with command flag instead of stdin fallback", [] {
            StdinFixture input; Capture output; auto legs = pair();
            for (std::size_t i = 0; i < legs.size(); ++i) {
                const char* option = i ? "--put-price" : "--call-price";
                std::string error;
                try { long_vol::resolvePrice(legs[i], 0, option, [](Leg&) { throw std::runtime_error("FAKE_QUOTE_UNAVAILABLE"); }); }
                catch (const std::exception& e) { error = e.what(); }
                require(error.find(option) != std::string::npos && error.find("FAKE_QUOTE_UNAVAILABLE") != std::string::npos, "Missing actionable quote failure");
            }
            input.untouched(); require(output.text.str().empty(), "Printed an interactive price prompt");
        });
        test("credentials use configuration then environment and never read stdin", [] {
            StdinFixture input; Capture output; EnvironmentFixture environment;
            environment.set("ENV_FIXTURE");
            require(long_vol::nonInteractiveSecret("CONFIG_FIXTURE", environment.name) == "CONFIG_FIXTURE", "Config precedence changed");
            require(long_vol::nonInteractiveSecret("", environment.name) == "ENV_FIXTURE", "Environment credential lost");
            environment.set(""); rejects([&] { long_vol::nonInteractiveSecret("", environment.name); });
            environment.set(nullptr); rejects([&] { long_vol::nonInteractiveSecret("", environment.name); });
            input.untouched(); require(output.text.str().empty(), "Printed a credential prompt");
        });
        test("pair validation rejects wrong type underlying expiry strike or exchange", [] {
            const auto base = pair(); long_vol::validatePair(base, "SSE", "20260915");
            auto p = base; p[1].contract.OptionsType = THOST_FTDC_CP_CallOptions; rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            p = base; field(p[1].contract.UnderlyingInstrID, "OTHER", "underlying"); rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            p = base; field(p[1].contract.ExpireDate, "20261101", "expiry"); rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            p = base; p[1].contract.StrikePrice = 3.1; rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            rejects([&] { long_vol::validatePair(base, "SZSE", "20260915"); });
        });
        test("expired halted or incompatible one-lot contracts fail", [] {
            auto p = pair(); p[0].contract.IsTrading = false; rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            p = pair(); p[0].contract.MinLimitOrderVolume = 2; rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            p = pair(); p[1].contract.VolumeMultiple = 5000; rejects([&] { long_vol::validatePair(p, "SSE", "20260915"); });
            rejects([&] { long_vol::validatePair(pair(), "SSE", "20261101"); });
        });
        test("manual prices reject letters nonfinite values and invalid ticks", [] {
            for (const auto* raw : {"0.04abc", "abc", "NaN", "inf", "1e-2", "0", "-1", "1.2.3"}) rejects([&] { long_vol::decimalPrice(raw); });
            auto leg = pair()[0]; leg.price = 0.04005; rejects([&] { long_vol::validatePrice(leg); });
            leg.price = std::numeric_limits<double>::infinity(); rejects([&] { long_vol::validatePrice(leg); });
        });
        test("automatic quote requires matching fresh nonempty sell side", [] {
            auto l = pair()[0]; CThostFtdcDepthMarketDataField q{};
            field(q.InstrumentID, l.instrument, "id"); field(q.ExchangeID, "SSE", "exchange"); field(q.TradingDay, "20260915", "day");
            field(q.UpdateTime, "09:35:00", "time"); q.AskPrice1 = 0.0427; q.AskVolume1 = 1;
            const auto s = settings(); const int now = 9 * 3600 + 35 * 60;
            require(long_vol::useAsk(l, q, s, "20260915", now, Clock::now()) && l.price == 0.0427, "Valid ask rejected");
            require(!long_vol::useAsk(l, q, s, "20260915", now + 10, Clock::now()), "Stale quote accepted");
            require(!long_vol::useAsk(l, q, s, "20260915", -1, Clock::now()), "Missing server clock accepted");
            q.AskVolume1 = 0; require(!long_vol::useAsk(l, q, s, "20260915", now, Clock::now()), "Empty ask accepted");
            q.AskVolume1 = 1; q.UpperLimitPrice = 0.04; require(!long_vol::useAsk(l, q, s, "20260915", now, Clock::now()), "Above exchange limit accepted");
            q.UpperLimitPrice = 0; field(q.InstrumentID, "10000003", "id"); require(!long_vol::useAsk(l, q, s, "20260915", now, Clock::now()), "Foreign quote accepted");
        });
        test("query ignores wrong ids and late callbacks and rejects empty results", [] {
            long_vol::Query<CThostFtdcInstrumentField> q; auto c = pair()[0].contract;
            q.begin(10, 1); q.response(&c, nullptr, 9, true); q.response(&c, nullptr, 10, true);
            require(textField(q.wait().InstrumentID) == "10000001", "Matching row lost");
            q.response(&c, nullptr, 10, true); q.begin(11, 1); q.response(nullptr, nullptr, 11, true); rejects([&] { q.wait(); });
            q.begin(12, 1); q.disconnected(); rejects([&] { q.wait(); });
            q.begin(13, 1); CThostFtdcRspInfoField info{}; info.ErrorID = 1; q.response(&c, &info, 13, true); rejects([&] { q.wait(); });
        });
        test("dry run never calls order APIs or creates counter state", [] {
            BatchOrders b; Api api{b}; auto s = settings(); s.send = false; s.confirm.clear(); std::string out;
            require(execute(api, config(), s, pair(), b, out), "Dry run failed");
            require(api.inserts == 0 && api.cancels == 0 && !fs::exists("state"), "Dry run changed state");
            require(out.find("mode=DRY_RUN") != std::string::npos, "Missing dry-run label");
        });
        test("two genuine fills pass with different one-lot buy-open contracts", [] {
            BatchOrders b; Api api{b}; std::string out;
            require(execute(api, config(), settings(), pair(), b, out), "Two fills failed");
            require(api.inserts == 2 && api.cancels == 0, "Unexpected API calls");
            require(textField(api.requests[0].InstrumentID) != textField(api.requests[1].InstrumentID), "Both legs sent the same instrument");
            for (const auto& p : api.requests) require(p.Direction == THOST_FTDC_D_Buy && p.CombOffsetFlag[0] == THOST_FTDC_OF_Open && p.VolumeTotalOriginal == 1 && p.OrderPriceType == THOST_FTDC_OPT_LimitPrice, "Wrong order fields");
            require(out.find("测试通过：认购与认沽各成交 1 张") != std::string::npos, "Missing Chinese result");
        });
        test("engine requires its own explicit live confirmation", [] {
            BatchOrders b; Api api{b}; auto s = settings(); s.confirm.clear(); std::string out;
            require(!execute(api, config(), s, pair(), b, out) && api.inserts == 0, "Engine bypassed confirmation");
        });
        test("daily remaining quota must cover both legs before any send", [] {
            auto c = config(); c.dailyMaxOrderCount = 2; DailyOrderCounter counter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
            (void)counter.reserve("20260915"); BatchOrders b; Api api{b}; std::string out;
            require(!execute(api, c, settings(), pair(), b, out) && api.inserts == 0, "Partial daily budget sent an order");
        });
        test("first rejection stops second leg without replacement orders", [] {
            BatchOrders b; Api api{b}; api.modes[0] = "reject"; std::string out;
            require(!execute(api, config(), settings(), pair(), b, out), "Rejected first leg passed");
            require(api.inserts == 1 && api.cancels == 0, "Unexpected second leg or cancellation");
        });
        test("canceled first leg stops the next leg", [] {
            BatchOrders b; Api api{b}; api.modes[0] = "canceled"; std::string out;
            require(!execute(api, config(), settings(), pair(), b, out) && api.inserts == 1, "Canceled first leg allowed second order");
        });
        test("second rejection reports partial position and never reverses it", [] {
            BatchOrders b; Api api{b}; api.modes[1] = "reject"; std::string out;
            require(!execute(api, config(), settings(), pair(), b, out), "Partial fill passed");
            require(api.inserts == 2 && api.cancels == 0 && out.find("仅确认一条腿成交") != std::string::npos, "Partial warning or call budget wrong");
        });
        test("both unfilled orders get at most one cancellation and fail fill goal", [] {
            BatchOrders b; Api api{b}; api.modes = {"queue", "queue"}; std::string out;
            require(!execute(api, config(), settings(), pair(), b, out), "Unfilled pair passed");
            require(api.inserts == 2 && api.cancels == 2 && out.find("unknown_orders=0") != std::string::npos, "Cleanup did not close both");
        });
        test("unknown callback and failed cancellation leave explicit residual warning", [] {
            BatchOrders b; Api api{b}; api.modes[1] = "unknown"; api.cancelFails = true; std::string out;
            require(!execute(api, config(), settings(), pair(), b, out), "Unknown residual passed");
            require(api.inserts == 2 && api.cancels == 1 && out.find("unknown_orders=1") != std::string::npos, "Unknown order concealed");
        });
        test("submission exception still cleans an order with unknown outcome", [] {
            BatchOrders b; Api api{b}; api.modes[0] = "throw"; std::string out;
            require(!execute(api, config(), settings(), pair(), b, out), "Exception passed");
            require(api.inserts == 1 && api.cancels == 1, "Exception abandoned cleanup or retried");
        });
        test("expired quote is rejected before first order", [] {
            BatchOrders b; Api api{b}; auto legs = pair(); legs[1].deadline = Clock::now() - std::chrono::seconds(1); std::string out;
            require(!execute(api, config(), settings(), legs, b, out) && api.inserts == 0, "Expired quote sent first leg");
        });
        test("quote expiry between legs prevents second SDK call and quota reservation", [] {
            BatchOrders b; Api api{b}; auto legs = pair(); std::string out;
            require(!execute(api, config(), settings(), legs, b, out, [&] { legs[1].deadline = Clock::now() - std::chrono::seconds(1); }), "Expired second quote passed");
            auto c = config(); DailyOrderCounter counter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
            require(api.inserts == 1 && counter.check("20260915").submittedBefore == 1, "Expired leg sent or consumed quota");
        });
        test("second leg still obeys rolling-second guard and reports partial position", [] {
            auto c = config(); c.perSecondMaxOrderCount = 1; BatchOrders b; Api api{b}; std::string out;
            require(!execute(api, c, settings(), pair(), b, out), "Rate-blocked pair passed");
            require(api.inserts == 1 && out.find("per_second_max_order_count") != std::string::npos && out.find("仅确认一条腿成交") != std::string::npos, "Rate guard bypassed or partial position hidden");
        });
        test("production pacing supports an unchanged one-order-per-second limit", [] {
            auto c = config(); c.perSecondMaxOrderCount = 1; BatchOrders b; Api api{b};
            Secrets secret; Logger log("run.log", secret); Capture capture;
            require(long_vol::execute(api, c, settings(), login(), pair(), b, log), "Production pacing failed");
            require(api.inserts == 2, "Pacing did not send exactly two orders");
        });
        test("strategy shares persistent count with other entry points", [] {
            auto c = config(); DailyOrderCounter counter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
            (void)counter.reserve("20260915"); BatchOrders b; Api api{b}; std::string out;
            require(execute(api, c, settings(), pair(), b, out), "Two-fill run failed");
            require(counter.check("20260915").submittedBefore == 3, "Existing order count was reset or isolated");
        });
        fs::current_path(original); std::cout << passed << " long-volatility tests passed (fake SDK; no live orders)\n"; return 0;
    } catch (const std::exception& e) {
        fs::current_path(original); std::cerr << "FAIL long-volatility test: " << e.what() << '\n'; return 1;
    }
}
