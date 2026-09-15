#pragma once
#include <array>

// Include after main.cpp: reuse the existing request builder, lifecycle and
// cross-process daily/rolling-second guard; no second set of counters.
namespace long_vol {
constexpr const char* version = "v0.1.0";
constexpr const char* confirmation = "SEND_LONG_VOLATILITY_ORDERS";
struct Settings {
    std::string config = "config/connection.ini", exchange = "SSE", call, put, confirm;
    double callPrice = 0, putPrice = 0;
    int timeout = 60, fillWait = 10;
    bool send = false, noPrompt = false, help = false;
};
struct Leg {
    std::string label, instrument, source;
    CThostFtdcInstrumentField contract{};
    double price = 0;
    Clock::time_point deadline = Clock::time_point::max();
};
bool positivePrice(double p) { return std::isfinite(p) && p > 0 && p < 100000000.0; }
double decimalPrice(const std::string& raw) {
    if (raw.empty() || raw.find_first_not_of("0123456789.") != std::string::npos ||
        std::count(raw.begin(), raw.end(), '.') > 1)
        throw std::runtime_error("价格格式错误：请输入大于 0 的十进制数字，不能包含字母。");
    std::size_t used = 0; double value = 0;
    try { value = std::stod(raw, &used); }
    catch (...) { throw std::runtime_error("价格格式错误：无法转换为有效数字。"); }
    if (used != raw.size() || !positivePrice(value))
        throw std::runtime_error("价格格式错误：价格须大于 0 且小于 100000000。");
    return value;
}
std::string number(double value) {
    std::ostringstream out; out << std::fixed << std::setprecision(8) << value; return out.str();
}
Options orderOptions(const Settings& s, const Leg& leg) {
    Options o; o.mode = "trader"; o.test = "basic"; o.instrument = leg.instrument;
    o.exchange = s.exchange; o.direction = "buy"; o.offset = "open"; o.price = leg.price;
    o.orderGoal = "fill"; o.fillWait = s.fillWait; o.timeout = s.timeout; return o;
}
void validatePrice(const Leg& leg) {
    const double tick = leg.contract.PriceTick;
    if (!positivePrice(leg.price) || !positivePrice(tick))
        throw std::runtime_error(leg.label + "价格或最小变动价位无效。");
    const double nearest = std::round(leg.price / tick) * tick;
    if (std::abs(nearest - leg.price) > std::max(tick * 0.000001, 1e-12))
        throw std::runtime_error(leg.label + "价格不是合约最小变动价位的整数倍。");
}
void validatePair(const std::array<Leg, 2>& legs, const std::string& exchange, const std::string& day) {
    if (!validTradingDay(day)) throw std::runtime_error("登录返回的交易日无效。");
    if (legs[0].instrument.empty() || legs[0].instrument == legs[1].instrument)
        throw std::runtime_error("认购与认沽必须是两个不同的期权合约代码。");
    for (std::size_t i = 0; i < legs.size(); ++i) {
        const auto& c = legs[i].contract;
        if (textField(c.InstrumentID) != legs[i].instrument || textField(c.ExchangeID) != exchange)
            throw std::runtime_error("查询返回的合约代码或交易所不匹配。");
        if (c.OptionsType != (i == 0 ? THOST_FTDC_CP_CallOptions : THOST_FTDC_CP_PutOptions))
            throw std::runtime_error(legs[i].label + "合约的认购／认沽类型不正确。");
        if (!c.IsTrading || !validTradingDay(textField(c.ExpireDate)) || textField(c.ExpireDate) < day ||
            c.MinLimitOrderVolume > 1 || c.MaxLimitOrderVolume < 1 || c.MinBuyVolume > 1 ||
            !positivePrice(c.PriceTick) || !positivePrice(c.StrikePrice) || c.VolumeMultiple <= 0)
            throw std::runtime_error(legs[i].label + "合约不可交易、已到期，或不支持本测试的 1 张限价买入。");
    }
    const auto& a = legs[0].contract; const auto& b = legs[1].contract;
    if (textField(a.UnderlyingInstrID).empty() || textField(a.UnderlyingInstrID) != textField(b.UnderlyingInstrID) ||
        textField(a.ExpireDate) != textField(b.ExpireDate) || a.StrikePrice != b.StrikePrice ||
        a.VolumeMultiple != b.VolumeMultiple || a.UnderlyingMultiple != b.UnderlyingMultiple)
        throw std::runtime_error("买入跨式要求两腿标的、到期日、行权价及合约单位一致。");
}
int secondsOfDay(const std::string& time) {
    if (time.size() != 8 || time[2] != ':' || time[5] != ':' ||
        time.substr(0, 2).find_first_not_of("0123456789") != std::string::npos ||
        time.substr(3, 2).find_first_not_of("0123456789") != std::string::npos ||
        time.substr(6, 2).find_first_not_of("0123456789") != std::string::npos) return -1;
    const int h = std::stoi(time.substr(0, 2)), m = std::stoi(time.substr(3, 2)), s = std::stoi(time.substr(6, 2));
    return h < 24 && m < 60 && s < 60 ? h * 3600 + m * 60 + s : -1;
}
bool useAsk(Leg& leg, const CThostFtdcDepthMarketDataField& q, const Settings& s,
            const std::string& day, int estimatedServerSeconds, Clock::time_point received) {
    const int quoteSeconds = secondsOfDay(textField(q.UpdateTime));
    const int age = estimatedServerSeconds - quoteSeconds;
    if (textField(q.InstrumentID) != leg.instrument || textField(q.ExchangeID) != s.exchange ||
        textField(q.TradingDay) != day || quoteSeconds < 0 || estimatedServerSeconds < 0 ||
        age < -3 || age >= 10 || q.AskVolume1 < 1 || !positivePrice(q.AskPrice1)) return false;
    if ((positivePrice(q.UpperLimitPrice) && q.AskPrice1 > q.UpperLimitPrice) ||
        (positivePrice(q.LowerLimitPrice) && q.AskPrice1 < q.LowerLimitPrice)) return false;
    leg.price = q.AskPrice1; leg.source = "行情卖一价（限价报单）";
    leg.deadline = received + std::chrono::seconds(10 - std::max(0, age));
    try { validatePrice(leg); } catch (...) { leg.price = 0; return false; }
    return true;
}

// A bounded, non-atomic two-leg execution. Never chase, resend, or reverse a fill.
// Each order is one lot. Both legs use the same account counter as basic/rate.
template<class Api> bool execute(Api& api, const Config& c, const Settings& s,
        const CThostFtdcRspUserLoginField& login, const std::array<Leg, 2>& legs,
        BatchOrders& batch, Logger& log,
        const std::function<void()>& pace = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1100)); }) {
    bool operational = true;
    auto write = [&](const std::string& line) { try { log.write("做多波动率策略：" + line); } catch (...) { operational = false; } };
    std::vector<BatchOrders::Entry*> entries;
    try {
        if (s.fillWait < 1 || s.fillWait > 300 || s.timeout < 1 || s.timeout > 300)
            throw std::runtime_error("等待时长必须为 1..300 秒。");
        if (s.send && s.confirm != confirmation) throw std::runtime_error("缺少双腿报单确认口令。");
        validatePair(legs, s.exchange, textField(login.TradingDay));
        for (const auto& leg : legs) {
            validatePrice(leg);
            if (Clock::now() >= leg.deadline) throw std::runtime_error("行情报价已过期，请重新运行获取行情；本次不报单。");
            write(leg.label + " 合约=" + leg.instrument + " 买入开仓 数量=1 限价=" + number(leg.price) + " 来源=" + leg.source);
        }
        if (!operational) throw std::runtime_error("证据日志写入失败。");
        if (!s.send) { write("演练完成，未报单；成交结果=未测试 mode=DRY_RUN"); return operational; }
        const std::string day = textField(login.TradingDay);
        DailyOrderCounter counter(dailyOrderStatePath(c), c.dailyMaxOrderCount, c);
        const auto before = counter.check(day);
        if (!before.allowed || static_cast<long long>(before.configuredLimit) - before.submittedBefore < 2)
            throw std::runtime_error("风控不允许启动：每日剩余额度须至少 2 笔，且当前每秒额度未用完。");
        batch.prepare(c, orderOptions(s, legs[0]), login, 2);
        entries = batch.entries();
        for (std::size_t i = 0; i < 2; ++i) {
            auto* e = entries[i]; e->request = basicOrderRequest(c, orderOptions(s, legs[i]), e->reference);
            e->request.RequestID = e->insertId;
            CThostFtdcOrderField ids{};
            field(ids.OrderRef, e->reference, "order_ref"); field(ids.InstrumentID, legs[i].instrument, "instrument");
            field(ids.ExchangeID, s.exchange, "exchange"); ids.FrontID = login.FrontID; ids.SessionID = login.SessionID;
            e->lifecycle.start(e->reference, true, ids);
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (i) pace(); // One deliberate gap; supports a filed one-order/second limit without retrying.
            if (!operational || batch.hasFailure()) throw std::runtime_error("连接或前一条腿失败，停止发送后续腿。");
            if (i && entries[0]->lifecycle.current().done && !entries[0]->lifecycle.current().filled)
                throw std::runtime_error("认购腿已经终结且未成交，停止认沽腿报单。");
            auto* e = entries[i];
            write("准备提交" + legs[i].label + " order_ref=" + e->reference);
            if (!operational) throw std::runtime_error("证据日志写入失败。");
            const auto result = counter.submit(day, [&] {
                e->submitted = true;
                return api.ReqOrderInsert(&e->request, e->insertId);
            }, legs[i].deadline);
            if (!result.risk.allowed) throw std::runtime_error("报单被风控拦截：" + result.risk.blockedRule);
            e->lifecycle.insertSubmitted(result.immediateRc);
            write(legs[i].label + "提交返回=" + std::to_string(result.immediateRc) +
                  "（仅表示提交，不代表成交） 每日已计数=" + std::to_string(result.risk.submittedAfter) +
                  " 最近一秒已计数=" + std::to_string(result.risk.secondAfter));
            if (!result.stateFinalized || result.immediateRc != 0)
                throw std::runtime_error("报单返回失败或计数落盘失败，停止后续报单并核对已发委托。");
        }
        write("等待两腿成交，最长 " + std::to_string(s.fillWait) + " 秒；未成交部分随后申请撤单。");
        const auto deadline = Clock::now() + std::chrono::seconds(s.fillWait);
        while (Clock::now() < deadline && !batch.hasFailure()) {
            if (entries[0]->lifecycle.current().done && entries[1]->lifecycle.current().done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (const std::exception& e) { operational = false; write(std::string("执行中止：") + e.what()); }
    // Cleanup always runs after any possible send, including errors and unknown callbacks.
    const auto cleanupDeadline = Clock::now() + std::chrono::seconds(s.timeout);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto* e = entries[i]; if (!e->submitted) continue;
        try {
            auto state = e->lifecycle.current();
            if (state.filled || state.canceled || (!state.residualUnknown && state.done)) continue;
            if (!e->lifecycle.beginCancellation(true)) continue;
            state = e->lifecycle.current();
            if (state.filled || state.canceled) continue;
            e->action = basicCancelRequest(c, orderOptions(s, legs[i]), state.order, e->reference);
            const int rc = api.ReqOrderAction(&e->action, e->cancelId);
            e->lifecycle.cancellationSubmitted(rc);
            write(legs[i].label + "未成交，申请一次撤单，返回=" + std::to_string(rc));
        } catch (const std::exception& e) { operational = false; write(std::string("撤单异常：") + e.what()); }
    }
    int filled = 0, unknown = 0, sent = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto* e = entries[i];
        if (!e->submitted) { write(legs[i].label + "未报单"); continue; }
        ++sent;
        auto state = e->lifecycle.current();
        if (!state.done) state = e->lifecycle.waitUntilDoneAt(cleanupDeadline);
        if (state.filled) ++filled;
        if (state.residualUnknown) ++unknown;
        write(legs[i].label + " 合约=" + legs[i].instrument + " order_ref=" + e->reference +
              " 已成交=" + std::to_string(state.tradedVolume) + " 已撤单=" + (state.canceled ? "是" : "否") +
              " 委托状态待核对=" + (state.residualUnknown ? "是" : "否"));
    }
    if (filled == 1) write("目前仅确认一条腿成交，组合未完整确认；请在交易终端核对持仓和另一腿状态。程序不自动反向平仓。");
    if (unknown) write("存在状态不明委托，请立即在交易终端核对并处理残单。");
    const bool success = operational && sent == 2 && filled == 2 && unknown == 0;
    write(std::string(success ? "测试通过：认购与认沽各成交 1 张。" : "测试未通过：尚未完整确认两腿成交或执行异常。") +
          " LONG_VOL_RESULT status=" + (success ? "PASS" : "FAIL") + " sent=" + std::to_string(sent) +
          " filled_legs=" + std::to_string(filled) + " unknown_orders=" + std::to_string(unknown));
    return success && operational;
}
} // namespace long_vol
