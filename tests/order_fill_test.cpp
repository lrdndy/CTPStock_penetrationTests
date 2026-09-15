// Offline tests of actual application option parsing and callback lifecycle.
// No SDK object, credentials, connection, order transmission, or real risk state.
#define main ctp_connectivity_application_main
#include "../src/main.cpp"
#undef main
#include <functional>
#include <thread>

namespace {
int passed = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void test(const char* name, const std::function<void()>& body) {
    body();
    ++passed;
    std::cout << "PASS " << name << '\n';
}
Options options(std::initializer_list<const char*> extras = {}) {
    std::vector<std::string> args{"offline", "--mode", "trader", "--test", "basic",
        "--instrument", "DUMMY_OPT", "--exchange", "SSE", "--direction", "buy",
        "--offset", "open", "--price", "0.01"};
    for (const auto* value : extras) args.emplace_back(value);
    std::vector<char*> pointers;
    for (auto& value : args) pointers.push_back(value.data());
    return parseOptions(static_cast<int>(pointers.size()), pointers.data());
}
void rejects(std::initializer_list<const char*> extras) {
    try { (void)options(extras); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid fill options accepted");
}
CThostFtdcOrderField queuedOrder() {
    CThostFtdcOrderField order{};
    field(order.OrderRef, "7", "order_ref");
    field(order.InstrumentID, "DUMMY_OPT", "instrument");
    field(order.ExchangeID, "SSE", "exchange");
    field(order.OrderSysID, "OFFLINE_ORDER", "order_sys_id");
    order.FrontID = 3; order.SessionID = -123;
    order.VolumeTotalOriginal = order.VolumeTotal = 1;
    order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    return order;
}
CThostFtdcTradeField oneTrade() {
    CThostFtdcTradeField trade{};
    field(trade.OrderRef, "7", "order_ref");
    field(trade.InstrumentID, "DUMMY_OPT", "instrument");
    field(trade.ExchangeID, "SSE", "exchange");
    field(trade.OrderSysID, "OFFLINE_ORDER", "order_sys_id");
    field(trade.TradeID, "OFFLINE_TRADE", "trade_id");
    trade.Volume = 1;
    return trade;
}
}

int main() {
    try {
        test("fill options are explicit and preserve one-order confirmation", [] {
            const auto defaults = options();
            require(defaults.orderGoal == "cancel" && !defaults.sendOrder, "Default cancellation/dry-run changed");
            const auto fill = options({"--order-goal", "fill", "--fill-wait", "20",
                                       "--send-order", "--confirm", "SEND_ONE_ORDER"});
            require(fill.orderGoal == "fill" && fill.fillWait == 20 && fill.sendOrder, "Fill options lost");
            rejects({"--order-goal", "fill", "--send-order"});
            rejects({"--order-goal", "other"});
            rejects({"--fill-wait", "10"});
            rejects({"--order-goal", "fill", "--fill-wait", "0"});
            rejects({"--order-goal", "fill", "--fill-wait", "301"});
            rejects({"--order-goal", "fill", "--fill-wait", "1x"});
            rejects({"--test", "risk", "--order-goal", "fill"});
            Config config; config.investor = config.user;
            const auto request = basicOrderRequest(config, fill, "7");
            require(request.VolumeTotalOriginal == 1 && request.TimeCondition == THOST_FTDC_TC_GFD &&
                    request.OrderPriceType == THOST_FTDC_OPT_LimitPrice, "Fill request enlarged order scope");
        });
        test("fill wait continues through queueing until actual trade", [] {
            OrderLifecycle lifecycle;
            lifecycle.start("7", true);
            auto order = queuedOrder(); lifecycle.returnedOrder(&order);
            std::thread callback([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto trade = oneTrade(); lifecycle.returnedTrade(&trade);
            });
            const auto result = lifecycle.waitForFill(1);
            callback.join();
            require(result.ok && result.filled && !result.cancelAttempted && result.tradedVolume == 1,
                    "Queueing ended fill wait or fill was missed");
        });
        test("trade and cumulative order reports are never counted twice", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            auto order = queuedOrder(); order.OrderStatus = THOST_FTDC_OST_AllTraded;
            order.VolumeTraded = 1; order.VolumeTotal = 0;
            lifecycle.returnedOrder(&order);
            auto trade = oneTrade(); lifecycle.returnedTrade(&trade); lifecycle.returnedTrade(&trade);
            lifecycle.returnedOrder(&order);
            require(lifecycle.current().tradedVolume == 1, "Duplicate evidence inflated fill volume");
        });
        test("trade before order and missing trade ID do not duplicate volume", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            auto trade = oneTrade(); trade.TradeID[0] = '\0';
            lifecycle.returnedTrade(&trade); lifecycle.returnedTrade(&trade);
            trade = oneTrade(); lifecycle.returnedTrade(&trade);
            require(!lifecycle.current().filled && lifecycle.current().tradedVolume == 0,
                    "Uncorrelated trade passed before own order identity was confirmed");
            auto order = queuedOrder(); order.OrderStatus = THOST_FTDC_OST_AllTraded;
            order.VolumeTraded = 1; lifecycle.returnedOrder(&order);
            require(lifecycle.current().ok && lifecycle.current().tradedVolume == 1,
                    "Trade before order or missing ID inflated volume");
        });
        test("same-ref trade from another session cannot fill this order", [] {
            auto identifiers = queuedOrder(); identifiers.OrderSysID[0] = '\0';
            OrderLifecycle lifecycle; lifecycle.start("7", true, identifiers);
            auto otherTrade = oneTrade(); field(otherTrade.OrderSysID, "OTHER_ORDER", "order_sys_id");
            lifecycle.returnedTrade(&otherTrade);
            require(!lifecycle.current().filled && !lifecycle.current().done,
                    "Unverified same-ref trade falsely passed fill goal");
            auto otherOrder = queuedOrder(); otherOrder.SessionID = -124;
            field(otherOrder.OrderSysID, "OTHER_ORDER", "order_sys_id");
            otherOrder.OrderStatus = THOST_FTDC_OST_AllTraded;
            lifecycle.returnedOrder(&otherOrder);
            auto ownOrder = queuedOrder(); lifecycle.returnedOrder(&ownOrder);
            require(!lifecycle.current().filled && lifecycle.current().tradedVolume == 0,
                    "Other session trade survived exchange order ID correlation");
            auto noIdentity = oneTrade(); noIdentity.OrderSysID[0] = '\0';
            lifecycle.returnedTrade(&noIdentity);
            require(!lifecycle.current().filled, "Trade without system ID bypassed correlation");
            auto ownTrade = oneTrade(); lifecycle.returnedTrade(&ownTrade);
            require(lifecycle.current().ok && lifecycle.current().tradedVolume == 1,
                    "Verified own trade failed to satisfy fill goal");
        });
        test("own early trade is consumed only after matching session order confirms system ID", [] {
            auto identifiers = queuedOrder(); identifiers.OrderSysID[0] = '\0';
            OrderLifecycle lifecycle; lifecycle.start("7", true, identifiers);
            auto trade = oneTrade(); lifecycle.returnedTrade(&trade); lifecycle.returnedTrade(&trade);
            require(!lifecycle.current().filled, "Early trade bypassed identity verification");
            auto order = queuedOrder(); lifecycle.returnedOrder(&order);
            require(lifecycle.current().ok && lifecycle.current().tradedVolume == 1,
                    "Matching order did not reconcile buffered own trade");
        });
        test("ALL_TRADED is sufficient evidence for the fixed one-lot order", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            auto order = queuedOrder(); order.OrderStatus = THOST_FTDC_OST_AllTraded;
            lifecycle.returnedOrder(&order);
            const auto result = lifecycle.current();
            require(result.ok && result.filled && result.tradedVolume == 1 && !result.residualUnknown,
                    "Final filled order was not recognized");
        });
        test("unrelated order session or instrument does not satisfy fill goal", [] {
            auto identifiers = queuedOrder();
            OrderLifecycle lifecycle; lifecycle.start("7", true, identifiers);
            auto order = identifiers; order.SessionID = -124; order.OrderStatus = THOST_FTDC_OST_AllTraded;
            lifecycle.returnedOrder(&order);
            auto trade = oneTrade(); field(trade.InstrumentID, "OTHER_OPT", "instrument");
            lifecycle.returnedTrade(&trade);
            require(!lifecycle.current().filled && !lifecycle.current().done, "Unrelated callbacks passed fill goal");
        });
        test("fill before cancellation prevents a cancel attempt", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            auto order = queuedOrder(); lifecycle.returnedOrder(&order);
            auto trade = oneTrade(); lifecycle.returnedTrade(&trade);
            require(!lifecycle.beginCancellation(true) && lifecycle.current().ok,
                    "Cancellation started after known fill");
        });
        test("fill during cancellation passes even if cancel submission is rejected", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            auto order = queuedOrder(); lifecycle.returnedOrder(&order);
            require(lifecycle.beginCancellation(true), "Could not begin fallback cancellation");
            auto trade = oneTrade(); lifecycle.returnedTrade(&trade);
            lifecycle.cancellationSubmitted(-2);
            const auto result = lifecycle.current();
            require(result.ok && result.filled && !result.residualUnknown && !result.cancelVerified,
                    "Fill/cancel race lost actual fill evidence");
        });
        test("unfilled timeout with confirmed cancellation fails fill goal safely", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            auto order = queuedOrder(); lifecycle.returnedOrder(&order);
            require(!lifecycle.waitForFill(0).done, "Unfilled wait falsely completed");
            require(lifecycle.beginCancellation(true), "Timeout could not cancel");
            order.OrderStatus = THOST_FTDC_OST_Canceled; lifecycle.returnedOrder(&order);
            lifecycle.cancellationSubmitted(0);
            const auto result = lifecycle.current();
            require(result.done && !result.ok && !result.filled && result.cancelVerified && !result.residualUnknown,
                    "Cancellation was mistaken for fill success");
        });
        test("missing queueing callback still permits login-identifier cancellation", [] {
            auto identifiers = queuedOrder(); identifiers.OrderSysID[0] = '\0';
            OrderLifecycle lifecycle; lifecycle.start("7", true, identifiers);
            require(!lifecycle.waitForFill(0).done && lifecycle.beginCancellation(true),
                    "Missing queueing callback disabled bounded cleanup");
            Config config; config.investor = config.user;
            const auto action = basicCancelRequest(config, options(), lifecycle.current().order, "7");
            require(action.FrontID == 3 && action.SessionID == -123 && textField(action.OrderRef) == "7" &&
                    textField(action.OrderSysID).empty(), "Fallback cancel lost login identifiers");
            require(!lifecycle.beginCancellation(true), "A second cancel attempt was permitted");
        });
        test("cancel rejection and missing terminal callback report unknown residual", [] {
            OrderLifecycle rejected; rejected.start("7", true);
            require(rejected.beginCancellation(true), "Fallback cancellation unavailable");
            rejected.cancellationSubmitted(-2);
            require(rejected.current().done && !rejected.current().ok && rejected.current().residualUnknown,
                    "Cancel rejection hid residual uncertainty");
            OrderLifecycle timedOut; timedOut.start("7", true);
            require(timedOut.beginCancellation(true), "Fallback cancellation unavailable");
            timedOut.cancellationSubmitted(0);
            const auto result = timedOut.waitUntilDone(0);
            require(result.done && !result.ok && result.residualUnknown && !result.cancelVerified,
                    "Missing final callback falsely verified cleanup");
        });
        test("insert rejection fails fill goal without claiming a live remainder", [] {
            OrderLifecycle lifecycle; lifecycle.start("7", true);
            CThostFtdcRspInfoField info{}; info.ErrorID = 21;
            lifecycle.insertResponse(nullptr, &info);
            const auto result = lifecycle.waitForFill(0);
            require(result.done && !result.ok && !result.residualUnknown && !lifecycle.beginCancellation(true),
                    "Rejected order was treated as active or filled");
        });
        test("cancel error cannot be overwritten by immediate success after canceled callback", [] {
            OrderLifecycle lifecycle; lifecycle.start("7");
            auto order = queuedOrder(); lifecycle.returnedOrder(&order);
            require(lifecycle.beginCancellation(), "Could not begin cancellation");
            order.OrderStatus = THOST_FTDC_OST_Canceled; lifecycle.returnedOrder(&order);
            CThostFtdcRspInfoField info{}; info.ErrorID = 21;
            lifecycle.actionResponse(nullptr, &info);
            lifecycle.cancellationSubmitted(0);
            const auto result = lifecycle.current();
            require(result.done && !result.ok && !result.cancelVerified && !result.residualUnknown,
                    "Immediate rc=0 overwrote actual cancel rejection");
        });
        std::cout << passed << " offline fill tests passed; no SDK connection attempted.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
