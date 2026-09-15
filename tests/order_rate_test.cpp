// Offline tests of the real order counter. No SDK object or order is created.
// Injectable monotonic timestamps make one-second boundaries deterministic.
#define main ctp_connectivity_application_main
#include "../src/main.cpp"
#undef main
#include <atomic>
#include <functional>
#include <thread>

namespace {
fs::path testRoot;
int passed = 0;
constexpr const char* day = "20260915";

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
template <typename F> void rejects(F action) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid state/configuration was accepted");
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
Config configuration(int daily = 20, int second = 2) {
    Config c;
    c.broker = "OFFLINE_RATE_BROKER";
    c.user = "OFFLINE_RATE_USER";
    c.dailyMaxOrderCount = daily;
    c.perSecondMaxOrderCount = second;
    return c;
}
fs::path statePath(const char* name) { return testRoot / name / "state.ini"; }
void writeFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    if (!file) throw std::runtime_error("Cannot write offline risk fixture");
}
std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read offline risk fixture");
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    testRoot = argv[1];
    try {
        test("per-second limit accepts main configuration and local override", [] {
            const auto path = statePath("config-local").parent_path() / "connection.ini";
            writeFile(path, "[risk]\ndaily_max_order_count=20\nper_second_max_order_count=2\n");
            writeFile(path.parent_path() / "connection.local.ini",
                      "[risk]\nper_second_max_order_count=3\n");
            Secrets secrets;
            const auto c = readConfig(path.string(), secrets);
            require(c.dailyMaxOrderCount == 20 && c.perSecondMaxOrderCount == 3,
                    "Per-second local override did not preserve daily limit");
        });
        test("per-second limit rejects invalid configured values", [] {
            const std::vector<std::string> values{"0", "-1", "1.5", "1000000000", "abc"};
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto path = testRoot / ("config-invalid-" + std::to_string(i)) / "connection.ini";
                writeFile(path, "[risk]\ndaily_max_order_count=20\nper_second_max_order_count=" + values[i] + "\n");
                rejects([&] { Secrets secrets; (void)readConfig(path.string(), secrets); });
            }
        });
        test("missing per-second limit prevents a live reservation", [] {
            auto c = configuration();
            c.perSecondMaxOrderCount = 0;
            int apiCalls = 0;
            rejects([&] {
                DailyOrderCounter counter(statePath("missing-limit"), c.dailyMaxOrderCount, c);
                (void)counter.submit(day, [&] { ++apiCalls; return 0; });
            });
            require(apiCalls == 0, "Missing limit did not prevent API invocation");
        });
        test("combined policy checks both limits without consuming a blocked slot", [] {
            const auto allow = evaluateOrderRisk(10, 1, 3, 1, day);
            require(allow.allowed && allow.submittedAfter == 2 && allow.secondAfter == 2 &&
                    allow.blockedRule.empty(), "Allowed combined decision is incorrect");
            const auto rate = evaluateOrderRisk(10, 1, 3, 3, day);
            require(!rate.allowed && rate.submittedAfter == 1 && rate.secondAfter == 3 &&
                    rate.blockedRule == "per_second_max_order_count", "Rate block consumed a daily slot");
            const auto daily = evaluateOrderRisk(1, 1, 3, 1, day);
            require(!daily.allowed && daily.submittedAfter == 1 && daily.secondAfter == 1 &&
                    daily.blockedRule == "daily_max_order_count", "Daily block consumed a rate slot");
            const auto both = evaluateOrderRisk(1, 1, 3, 3, day);
            require(!both.allowed && both.submittedAfter == 1 && both.secondAfter == 3,
                    "Both limits exceeded but reservation was consumed");
        });
        test("rolling one-second window crosses clock second and expires exactly at 1000 ms", [] {
            const auto c = configuration();
            DailyOrderCounter counter(statePath("rolling-boundary"), c.dailyMaxOrderCount, c);
            require(counter.reserveAt(day, 1999).allowed, "First reservation blocked");
            require(counter.reserveAt(day, 2000).allowed, "Second reservation blocked");
            const auto block = counter.reserveAt(day, 2000);
            require(!block.allowed && block.secondBefore == 2 && block.submittedAfter == 2,
                    "Clock-second boundary reset the rolling window");
            require(!counter.reserveAt(day, 2998).allowed, "Timestamp expired before 1000 ms");
            const auto expiry = counter.reserveAt(day, 2999);
            require(expiry.allowed && expiry.secondBefore == 1 && expiry.secondAfter == 2 &&
                    expiry.submittedBefore == 2 && expiry.submittedAfter == 3,
                    "Timestamp did not expire at exactly 1000 ms");
            const auto next = counter.reserveAt(day, 3000);
            require(next.allowed && next.secondBefore == 1 && next.peakPerSecond == 2,
                    "Sliding expiry or peak changed incorrectly");
        });
        test("rate count and peak survive a new counter instance", [] {
            const auto c = configuration(20, 3);
            const auto path = statePath("instances");
            {
                DailyOrderCounter first(path, c.dailyMaxOrderCount, c);
                require(first.reserveAt(day, 5000).allowed, "First instance could not reserve");
                require(first.reserveAt(day, 5001).allowed, "Second reservation blocked");
                require(first.reserveAt(day, 5002).allowed, "Third reservation blocked");
            }
            DailyOrderCounter second(path, c.dailyMaxOrderCount, c);
            const auto block = second.reserveAt(day, 5003);
            require(!block.allowed && block.submittedBefore == 3 && block.secondBefore == 3 &&
                    block.peakPerSecond == 3, "New instance forgot the rate limit or peak");
            const auto allow = second.reserveAt(day, 6002);
            require(allow.allowed && allow.secondBefore == 0 && allow.peakPerSecond == 3,
                    "Expired window lost historical peak");
        });
        test("daily block leaves the rolling window unchanged", [] {
            const auto c = configuration(2, 3);
            const auto path = statePath("daily-block");
            {
                DailyOrderCounter first(path, c.dailyMaxOrderCount, c);
                require(first.reserveAt(day, 1000).allowed, "First reservation blocked");
                require(first.reserveAt(day, 1001).allowed, "Second reservation blocked");
                const auto block = first.reserveAt(day, 1002);
                require(!block.allowed && block.blockedRule == "daily_max_order_count" &&
                        block.secondBefore == 2 && block.secondAfter == 2,
                        "Daily block consumed a rate slot");
            }
            // Test-only threshold change lets us inspect the same persisted rate window.
            const auto raised = configuration(10, 3);
            DailyOrderCounter second(path, raised.dailyMaxOrderCount, raised);
            const auto allow = second.reserveAt(day, 1003);
            require(allow.allowed && allow.submittedBefore == 2 && allow.secondBefore == 2,
                    "Blocked attempt was persisted as a submission");
        });
        test("legacy daily-only state migrates without losing daily count", [] {
            const auto path = statePath("legacy");
            writeFile(path, "trading_day=20260915\nsubmitted_count=7\n");
            const auto c = configuration(20, 2);
            {
                DailyOrderCounter counter(path, c.dailyMaxOrderCount, c);
                const auto allow = counter.reserveAt(day, 2000);
                require(allow.allowed && allow.submittedBefore == 7 && allow.submittedAfter == 8 &&
                        allow.secondBefore == 0 && allow.secondAfter == 1,
                        "Migration reset the daily counter or invented recent submissions");
            }
            DailyOrderCounter again(path, c.dailyMaxOrderCount, c);
            const auto allow = again.reserveAt(day, 2001);
            require(allow.allowed && allow.submittedBefore == 8 && allow.secondBefore == 1,
                    "Migrated state did not persist the rolling window");
        });
        test("trading-day rollover resets daily count while preserving the recent second", [] {
            const auto c = configuration(2, 2);
            DailyOrderCounter counter(statePath("trading-day-rollover"), c.dailyMaxOrderCount, c);
            require(counter.reserveAt(day, 1000).allowed, "First reservation blocked");
            require(counter.reserveAt(day, 1001).allowed, "Second reservation blocked");
            const auto block = counter.reserveAt("20260916", 1002);
            require(!block.allowed && block.submittedBefore == 0 && block.submittedAfter == 0 &&
                    block.secondBefore == 2 && block.blockedRule == "per_second_max_order_count",
                    "Trading-day rollover reset the rolling second or retained daily count");
            const auto allow = counter.reserveAt("20260916", 2001);
            require(allow.allowed && allow.submittedBefore == 0 && allow.submittedAfter == 1 &&
                    allow.secondBefore == 0, "Next trading day did not accept after rolling expiry");
        });
        test("uptime rollback conservatively holds a full rate window for one second", [] {
            const auto c = configuration();
            const auto path = statePath("uptime-rollback");
            {
                DailyOrderCounter before(path, c.dailyMaxOrderCount, c);
                require(before.reserveAt(day, 10000).allowed, "First reservation blocked");
                require(before.reserveAt(day, 10001).allowed, "Second reservation blocked");
            }
            DailyOrderCounter after(path, c.dailyMaxOrderCount, c);
            const auto reset = after.reserveAt(day, 500);
            require(!reset.allowed && reset.secondBefore == 2 && reset.submittedAfter == 2,
                    "Uptime rollback bypassed rate control");
            require(!after.reserveAt(day, 501).allowed && !after.reserveAt(day, 1499).allowed,
                    "Conservative rollback window expired early");
            const auto expiry = after.reserveAt(day, 1500);
            require(expiry.allowed && expiry.secondBefore == 0 && expiry.submittedBefore == 2,
                    "Rollback window was rebased repeatedly or lost daily count");
        });
        test("concurrent counter instances share one atomic daily and rate reservation", [] {
            const auto c = configuration(5, 3);
            const auto path = statePath("concurrent");
            constexpr int clients = 12;
            std::atomic<int> ready{0}, allowed{0}, blocked{0}, errors{0};
            std::atomic<bool> start{false};
            std::vector<std::thread> workers;
            for (int i = 0; i < clients; ++i) {
                workers.emplace_back([&] {
                    ++ready;
                    while (!start.load()) std::this_thread::yield();
                    try {
                        DailyOrderCounter counter(path, c.dailyMaxOrderCount, c);
                        const auto result = counter.reserveAt(day, 10000);
                        if (result.allowed) ++allowed;
                        else if (result.blockedRule == "per_second_max_order_count") ++blocked;
                        else ++errors;
                    } catch (...) { ++errors; }
                });
            }
            while (ready.load() != clients) std::this_thread::yield();
            start = true;
            for (auto& worker : workers) worker.join();
            require(errors == 0 && allowed == 3 && blocked == clients - 3,
                    "Concurrent instances bypassed the shared rate limit");
            DailyOrderCounter check(path, c.dailyMaxOrderCount, c);
            const auto persisted = check.reserveAt(day, 11000);
            require(persisted.allowed && persisted.submittedBefore == 3 && persisted.secondBefore == 0 &&
                    persisted.peakPerSecond == 3, "Concurrent count or peak was lost");
        });
        test("real submission wrapper never calls the sender when either risk limit blocks", [] {
            int apiCalls = 0;
            const auto c = configuration(10, 2);
            const auto path = statePath("submit-rate-block");
            {
                DailyOrderCounter seed(path, c.dailyMaxOrderCount, c);
                const auto future = orderUptimeMs() + 60000;
                require(seed.reserveAt(day, future).allowed && seed.reserveAt(day, future).allowed,
                        "Cannot prepare full rate window");
            }
            DailyOrderCounter rate(path, c.dailyMaxOrderCount, c);
            const auto rateBlock = rate.submit(day, [&] { ++apiCalls; return 0; });
            require(!rateBlock.risk.allowed && rateBlock.risk.blockedRule == "per_second_max_order_count" &&
                    apiCalls == 0, "Rate-blocked submission called the sender");
            const auto dailyConfig = configuration(1, 2);
            DailyOrderCounter daily(statePath("submit-daily-block"), dailyConfig.dailyMaxOrderCount, dailyConfig);
            require(daily.reserveAt(day, 1).allowed, "Cannot prepare full daily count");
            const auto dailyBlock = daily.submit(day, [&] { ++apiCalls; return 0; });
            require(!dailyBlock.risk.allowed && dailyBlock.risk.blockedRule == "daily_max_order_count" &&
                    apiCalls == 0, "Daily-blocked submission called the sender");
        });
        test("submission wrapper counts a rejected SDK return exactly once", [] {
            const auto c = configuration();
            const auto path = statePath("submit-rejected");
            DailyOrderCounter counter(path, c.dailyMaxOrderCount, c);
            int apiCalls = 0;
            const auto result = counter.submit(day, [&] {
                ++apiCalls;
                const auto reserved = readFile(path);
                require(reserved.find("submission_pending=1\n") != std::string::npos &&
                        reserved.find("submitted_count=1\n") != std::string::npos,
                        "Sender was called before pending attempt was persisted");
                return -7;
            });
            require(result.risk.allowed && result.immediateRc == -7 && result.stateFinalized &&
                    result.risk.submittedAfter == 1 && apiCalls == 1,
                    "Rejected SDK return lost its attempted submission count");
            require(readFile(path).find("submission_pending=0\n") != std::string::npos,
                    "Finished SDK call left pending state");
            const auto next = counter.reserveAt(day, orderUptimeMs() + 2000);
            require(next.allowed && next.submittedBefore == 1,
                    "Rejected SDK return did not persist exactly one attempt");
        });
        test("interrupted submission state blocks further calls and preserves evidence", [] {
            const auto c = configuration();
            const auto path = statePath("interrupted-submission");
            const std::string pending = "trading_day=20260915\nsubmitted_count=1\n"
                "peak_per_second=1\nsubmission_pending=1\nrecent_uptime_ms=1000\n";
            writeFile(path, pending);
            DailyOrderCounter counter(path, c.dailyMaxOrderCount, c);
            int apiCalls = 0;
            rejects([&] { (void)counter.submit(day, [&] { ++apiCalls; return 0; }); });
            rejects([&] { (void)counter.reserveAt("20260916", 10000); });
            require(apiCalls == 0 && readFile(path) == pending,
                    "Interrupted submission was silently reset or called sender");
        });
        test("malformed extended state fails closed without rewriting evidence", [] {
            const auto c = configuration();
            const auto path = statePath("corrupt-extended");
            {
                DailyOrderCounter valid(path, c.dailyMaxOrderCount, c);
                require(valid.reserveAt(day, 1000).allowed, "Cannot create valid extended state");
            }
            std::istringstream source(readFile(path));
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(source, line)) lines.push_back(line);
            bool exercisedExtendedField = false;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const auto equal = lines[i].find('=');
                if (equal == std::string::npos) continue;
                const auto key = trim(lines[i].substr(0, equal));
                if (key == "trading_day" || key == "submitted_count") continue;
                std::string corrupt;
                for (std::size_t j = 0; j < lines.size(); ++j)
                    corrupt += (i == j ? key + "=INVALID_OFFLINE_STATE" : lines[j]) + "\n";
                writeFile(path, corrupt);
                rejects([&] {
                    DailyOrderCounter broken(path, c.dailyMaxOrderCount, c);
                    (void)broken.reserveAt(day, 1001);
                });
                require(readFile(path) == corrupt, "Invalid evidence was silently overwritten");
                exercisedExtendedField = true;
            }
            require(exercisedExtendedField, "State has no persisted per-second fields");
        });
        test("partially missing extended state fails closed", [] {
            const auto c = configuration();
            const auto path = statePath("incomplete-extended");
            const std::vector<std::string> extra{
                "peak_per_second=1\n", "submission_pending=0\n", "recent_uptime_ms=1000\n"};
            for (std::size_t omitted = 0; omitted < extra.size(); ++omitted) {
                std::string contents = "trading_day=20260915\nsubmitted_count=1\n";
                for (std::size_t i = 0; i < extra.size(); ++i)
                    if (i != omitted) contents += extra[i];
                writeFile(path, contents);
                rejects([&] {
                    DailyOrderCounter counter(path, c.dailyMaxOrderCount, c);
                    (void)counter.reserveAt(day, 1001);
                });
                require(readFile(path) == contents, "Incomplete state was silently overwritten");
            }
        });
        std::cout << "PASS " << passed << " offline per-second order risk tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL order risk tests: " << error.what() << '\n';
        return 1;
    }
}
