// Exercise the real live-rate engine with a fake SDK and real callback router.
// Every file is in the runner's temporary folder. No SDK/network/real orders.
#define main ctp_connectivity_application_main
#include "../src/main.cpp"
#undef main
#include <functional>
#include <future>

namespace {
fs::path testRoot;
int passed = 0;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
template<class F> void rejects(F function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected invalid live-rate configuration to fail");
}
std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents; contents << input.rdbuf(); return contents.str();
}
void test(const char* name, const std::function<void()>& body) {
    const auto directory = testRoot / ("case-" + std::to_string(passed));
    fs::create_directories(directory); fs::current_path(directory);
    body(); ++passed; std::cout << "PASS " << name << '\n';
}
Config config() {
    Config c; c.broker = "TEST"; c.user = c.investor = "DUMMY";
    c.dailyMaxOrderCount = 20; c.perSecondMaxOrderCount = 2; return c;
}
Options options() {
    Options o; o.test = "rate-live"; o.mode = "trader"; o.sendOrder = true;
    o.confirmation = "SEND_RATE_TEST_ORDERS"; o.maxOrders = 2; o.timeout = 1;
    o.instrument = "DUMMY_OPT"; o.exchange = "SSE"; o.direction = "buy";
    o.offset = "open"; o.price = 0.01; return o;
}
Result login() {
    Result result; result.ok = true;
    field(result.login.TradingDay, "20260915", "day");
    field(result.login.MaxOrderRef, "8", "ref");
    result.login.FrontID = 3; result.login.SessionID = -123;
    return result;
}
struct FakeApi {
    TraderSpi& spi;
    int inserts = 0, cancels = 0;
    int rejectAt = 0, immediateRejectAt = 0, throwAt = 0, disconnectAt = 0;
    int slowAt = 0, corruptAt = 0, fillAt = 0, genericErrorAt = 0;
    int nullGenericErrorAt = 0, zeroGenericErrorAt = 0;
    int cancelThrowAt = 0;
    bool noCancelReply = false, repliesAfterAllCancels = false, cancelReject = false;
    std::vector<CThostFtdcOrderField> orders;
    std::vector<int> insertIds, cancelIds;
    std::vector<std::string> canceledRefs;
    explicit FakeApi(TraderSpi& value) : spi(value) {}
    int ReqOrderInsert(CThostFtdcInputOrderField* request, int id) {
        ++inserts; insertIds.push_back(id);
        require(request->RequestID == id, "Insert request ID mismatch");
        require(request->VolumeTotalOriginal == 1, "Order size exceeded one lot");
        CThostFtdcOrderField order{};
        field(order.OrderRef, textField(request->OrderRef), "ref");
        field(order.InstrumentID, textField(request->InstrumentID), "instrument");
        field(order.ExchangeID, textField(request->ExchangeID), "exchange");
        field(order.OrderSysID, "OFFLINE_" + std::to_string(inserts), "system");
        order.FrontID = 3; order.SessionID = -123;
        order.VolumeTotalOriginal = order.VolumeTotal = 1;
        order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
        order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
        orders.push_back(order);
        if (immediateRejectAt == inserts) return -2;
        if (throwAt == inserts) throw std::runtime_error("Fake SDK insertion exception");
        CThostFtdcRspInfoField error{}; error.ErrorID = 21;
        if (rejectAt == inserts) { spi.OnRspOrderInsert(nullptr, &error, id, true); return 0; }
        if (genericErrorAt == inserts) { spi.OnRspError(&error, id, true); return 0; }
        if (nullGenericErrorAt == inserts) { spi.OnRspError(nullptr, id, true); return 0; }
        if (zeroGenericErrorAt == inserts) { error.ErrorID = 0; spi.OnRspError(&error, id, true); return 0; }
        if (disconnectAt == inserts) spi.OnFrontDisconnected(0x1001);
        if (fillAt == inserts) {
            orders.back().OrderStatus = THOST_FTDC_OST_AllTraded;
            orders.back().VolumeTraded = 1; orders.back().VolumeTotal = 0;
        }
        spi.OnRtnOrder(&orders.back());
        if (corruptAt == inserts) {
            std::ofstream broken(dailyOrderStatePath(config()), std::ios::trunc);
            broken << "invalid_state\n";
        }
        if (slowAt == inserts) std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        return 0;
    }
    int ReqOrderAction(CThostFtdcInputOrderActionField* action, int id) {
        ++cancels; cancelIds.push_back(id); canceledRefs.push_back(textField(action->OrderRef));
        require(action->RequestID == id, "Cancel request ID mismatch");
        require(action->FrontID == 3 && action->SessionID == -123, "Lost seeded session identifiers");
        if (cancelThrowAt == cancels) throw std::runtime_error("Fake SDK cancellation exception");
        if (cancelReject) {
            CThostFtdcRspInfoField error{}; error.ErrorID = 26;
            spi.OnRspOrderAction(nullptr, &error, id, true); return 0;
        }
        if (noCancelReply || (repliesAfterAllCancels && cancels < inserts)) return 0;
        for (auto& order : orders) {
            if (repliesAfterAllCancels || textField(order.OrderRef) == textField(action->OrderRef)) {
                order.OrderStatus = THOST_FTDC_OST_Canceled;
                spi.OnRtnOrder(&order);
            }
        }
        return 0;
    }
};
struct Fixture {
    Config c = config(); Options o = options(); Result loggedIn = login();
    Secrets secret; Logger log{fs::current_path() / "run.log", secret};
    State state; BatchOrders batch; TraderSpi spi{state, log, nullptr, &batch}; FakeApi api{spi};
    bool run() { return testLiveRateFunction(api, c, o, loggedIn, batch, log, false); }
    std::string text() { return readFile(fs::current_path() / "run.log"); }
};
Options parse(std::initializer_list<const char*> extras) {
    std::vector<std::string> args{"offline", "--test", "rate-live", "--mode", "trader",
        "--instrument", "DUMMY_OPT", "--exchange", "SSE", "--direction", "buy",
        "--offset", "open", "--price", "0.01"};
    for (auto value : extras) args.emplace_back(value);
    std::vector<char*> pointers; for (auto& arg : args) pointers.push_back(arg.data());
    return parseOptions(static_cast<int>(pointers.size()), pointers.data());
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    testRoot = fs::absolute(argv[1]);
    const auto originalDirectory = fs::current_path();
    try {
        test("N real submissions then check-only N+1 block and individual cleanup", [] {
            Fixture f; require(f.run(), "Expected proven rate limit and cleanup");
            require(f.api.inserts == 2 && f.api.cancels == 2, "Incorrect wire call budget");
            require(f.api.insertIds == std::vector<int>({100, 102}), "Insert request IDs not unique");
            require(f.api.cancelIds == std::vector<int>({101, 103}), "Cancel request IDs not unique");
            require(f.api.canceledRefs == std::vector<std::string>({"9", "10"}), "Cleanup refs incorrect");
            require(f.text().find("candidate_number=3") != std::string::npos, "Missing next candidate evidence");
            require(f.text().find("check_only=YES quota_reserved=NO api_call=NOT_SENT") != std::string::npos,
                    "Next candidate incorrectly described");
            DailyOrderCounter counter(dailyOrderStatePath(f.c), f.c.dailyMaxOrderCount, f.c);
            require(counter.check("20260915").submittedBefore == 2, "Check-only candidate consumed quota");
        });
        test("cleanup submissions precede waiting for any cancellation", [] {
            Fixture f; f.api.repliesAfterAllCancels = true;
            require(f.run() && f.api.cancels == 2, "Cleanup did not submit all actions first");
        });
        test("dry run does not send or create counter state", [] {
            Fixture f; f.o.sendOrder = false;
            require(f.run() && f.api.inserts == 0, "Dry run sent an order");
            require(!fs::exists(dailyOrderStatePath(f.c)), "Dry run created risk state");
        });
        test("confirmation and budget are enforced by engine", [] {
            Fixture f; f.o.confirmation = "SEND_ONE_ORDER";
            require(!f.run() && !f.api.inserts, "Wrong confirmation permitted orders");
        });
        test("large threshold is rejected before sending", [] {
            Fixture f; f.c.perSecondMaxOrderCount = f.o.maxOrders = 11;
            require(!f.run() && !f.api.inserts, "Hard cap exceeded");
        });
        test("smaller budget cannot silently lower configured threshold", [] {
            Fixture f; f.o.maxOrders = 1;
            require(!f.run() && !f.api.inserts, "Mismatched budget permitted orders");
        });
        test("daily quota must permit N+1 so it cannot mask second limit", [] {
            Fixture f; f.c.dailyMaxOrderCount = 2;
            require(!f.run() && !f.api.inserts, "Insufficient daily quota permitted burst");
        });
        test("nonempty rate window is rejected before sending", [] {
            Fixture f; DailyOrderCounter counter(dailyOrderStatePath(f.c), f.c.dailyMaxOrderCount, f.c);
            (void)counter.reserveAt("20260915", orderUptimeMs() + 2000);
            require(!f.run() && !f.api.inserts, "Existing window was mixed into burst");
        });
        test("slow first call stops burst and still cleans up", [] {
            Fixture f; f.api.slowAt = 1;
            require(!f.run() && f.api.inserts == 1 && f.api.cancels == 1, "Expired window caused extra call");
        });
        test("slow final call cannot produce false rolling-window pass", [] {
            Fixture f; f.api.slowAt = 2;
            require(!f.run() && f.api.inserts == 2 && f.api.cancels == 2, "Slow burst was accepted or retried");
            require(f.text().find("rate_limit_trigger=NOT_PROVEN") != std::string::npos, "Slow burst claimed proof");
        });
        test("null payload rejection routes by unique insert request ID", [] {
            Fixture f; f.api.rejectAt = 2;
            require(!f.run() && f.api.inserts == 2 && f.api.cancels == 1, "Null rejection misrouted");
            require(f.api.canceledRefs[0] == "9", "Canceled the known rejected order");
        });
        test("immediate submission failure stops burst without retry", [] {
            Fixture f; f.api.immediateRejectAt = 1;
            require(!f.run() && f.api.inserts == 1 && f.api.cancels == 0, "Immediate rejection was retried");
        });
        test("mid-burst exception cleans earlier and ambiguous current order", [] {
            Fixture f; f.api.throwAt = 2;
            require(!f.run() && f.api.inserts == 2 && f.api.cancels == 2, "Exception skipped outstanding cleanup");
            require(readFile(dailyOrderStatePath(f.c)).find("submission_pending=1") != std::string::npos,
                    "Interrupted submission state was silently cleared");
        });
        test("risk state finalization failure does not skip cleanup", [] {
            Fixture f; f.api.corruptAt = 1;
            require(!f.run() && f.api.inserts == 1 && f.api.cancels == 1, "State failure skipped cleanup");
        });
        test("disconnect stops burst and cleanup retains session routing", [] {
            Fixture f; f.api.disconnectAt = 1;
            require(!f.run() && f.api.inserts == 1 && f.api.cancels == 1, "Disconnect did not stop and clean up");
        });
        test("generic order error uses its role and allows ambiguous cleanup", [] {
            Fixture f; f.api.genericErrorAt = 2;
            require(!f.run() && f.api.inserts == 2 && f.api.cancels == 2, "Generic error lost request routing or cleanup");
        });
        test("null OnRspError stops burst and retains cleanup of ambiguous order", [] {
            Fixture f; f.api.nullGenericErrorAt = 1;
            require(!f.run() && f.api.inserts == 1 && f.api.cancels == 1,
                    "Null error callback allowed another order or skipped cleanup");
        });
        test("zero-code OnRspError stops burst and retains cleanup of ambiguous order", [] {
            Fixture f; f.api.zeroGenericErrorAt = 1;
            require(!f.run() && f.api.inserts == 1 && f.api.cancels == 1,
                    "Zero-code error callback allowed another order or skipped cleanup");
        });
        test("expired send deadline calls no sender and consumes no quota", [] {
            const auto c = config();
            DailyOrderCounter counter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
            int sends = 0;
            rejects([&] { (void)counter.submit("20260915", [&] { ++sends; return 0; }, Clock::now()); });
            require(sends == 0, "Expired deadline invoked sender");
            require(counter.check("20260915").submittedBefore == 0, "Expired deadline consumed quota");
        });
        test("deadline expires during lock contention without later sending", [] {
            const auto c = config();
            DailyOrderCounter holder(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
            DailyOrderCounter waiter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
            std::promise<void> acquired;
            auto ready = acquired.get_future();
            std::exception_ptr workerError;
            std::thread worker([&] {
                bool signaled = false;
                try {
                    (void)holder.submit("20260915", [&] {
                        acquired.set_value(); signaled = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        return 0;
                    });
                } catch (...) {
                    workerError = std::current_exception();
                    if (!signaled) acquired.set_exception(workerError);
                }
            });
            struct Join { std::thread& value; ~Join() { if (value.joinable()) value.join(); } } join{worker};
            ready.get();
            int sends = 0;
            rejects([&] {
                (void)waiter.submit("20260915", [&] { ++sends; return 0; },
                                    Clock::now() + std::chrono::milliseconds(30));
            });
            worker.join();
            if (workerError) std::rethrow_exception(workerError);
            require(sends == 0, "Waiter sent after its deadline expired under lock contention");
            require(waiter.check("20260915").submittedBefore == 1, "Waiter consumed additional quota");
        });
        test("unknown residual fails once under shared timeout", [] {
            Fixture f; f.api.noCancelReply = true; const auto started = Clock::now();
            require(!f.run() && f.api.cancels == 2, "Unknown cleanup was retried or accepted");
            require(Clock::now() - started < std::chrono::milliseconds(1800), "Waits used N independent deadlines");
            require(f.text().find("residual_unknown=2") != std::string::npos, "Unknown residual omitted");
            require(f.text().find("OPERATOR_CHECK_REQUIRED") != std::string::npos, "Missing manual reconciliation notice");
        });
        test("null cancellation errors route individually and are not retried", [] {
            Fixture f; f.api.cancelReject = true;
            require(!f.run() && f.api.cancels == 2, "Cancellation error accepted or retried");
            require(f.text().find("residual_unknown=2") != std::string::npos, "Null cancellation errors lost");
        });
        test("one cancellation exception does not skip other orders", [] {
            Fixture f; f.api.cancelThrowAt = 1;
            require(!f.run() && f.api.cancels == 2, "Cancellation exception skipped remaining orders");
            require(f.text().find("residual_unknown=1") != std::string::npos, "Cancellation exception was marked closed");
        });
        test("fills are closed financial effects without reversal orders", [] {
            Fixture f; f.api.fillAt = 1;
            require(f.run() && f.api.inserts == 2 && f.api.cancels == 1, "Fill was canceled or reversed");
            require(f.text().find("FINANCIAL_EFFECT traded_volume=1 position_reversal=NOT_SENT") != std::string::npos,
                    "Fill effect omitted");
        });
        test("CLI requires budget and separate live confirmation", [] {
            rejects([] { (void)parse({"--max-orders", "2", "--send-order", "--confirm", "SEND_ONE_ORDER"}); });
            rejects([] { (void)parse({"--send-order", "--confirm", "SEND_RATE_TEST_ORDERS"}); });
            rejects([] { (void)parse({"--max-orders", "11"}); });
            rejects([] { (void)parse({"--max-orders", "2", "--order-goal", "fill"}); });
            auto o = parse({"--max-orders", "2", "--send-order", "--confirm", "SEND_RATE_TEST_ORDERS"});
            require(o.sendOrder && o.maxOrders == 2 && o.test == "rate-live", "Valid rate options rejected");
        });
        fs::current_path(originalDirectory);
        std::cout << "PASS " << passed << " live-rate engine checks (fake SDK, no network)\n";
        return 0;
    } catch (const std::exception& error) {
        fs::current_path(originalDirectory);
        std::cerr << "FAIL live-rate engine: " << error.what() << '\n';
        return 1;
    }
}
