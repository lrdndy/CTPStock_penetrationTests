#include "ThostFtdcMdApi.h"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <conio.h>
#endif

using namespace ctp_sopt;

namespace {

constexpr char kFront[] = "tcp://101.226.254.157:32213";
constexpr char kBrokerId[] = "1000";
constexpr char kUserId[] = "887120202987";
constexpr int kRequestId = 1;
constexpr auto kTimeout = std::chrono::seconds(60);

std::string now() {
    const auto current = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(current);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

void log(const std::string& message) {
    std::cout << now() << " " << message << std::endl;
}

std::string readPassword() {
    std::string password;
    std::cout << "Trading password (input is hidden): ";

#ifdef _WIN32
    for (;;) {
        const int key = _getch();
        if (key == '\r' || key == '\n') {
            std::cout << std::endl;
            return password;
        }
        if (key == '\b') {
            if (!password.empty()) {
                password.pop_back();
            }
            continue;
        }
        if (key == 3) {
            std::cout << std::endl;
            return {};
        }
        if (key >= 32 && key <= 126) {
            password.push_back(static_cast<char>(key));
        }
    }
#else
    std::getline(std::cin, password);
    return password;
#endif
}

template <std::size_t N>
bool copyField(char (&destination)[N], const std::string& value, const char* name) {
    if (value.size() >= N) {
        std::cerr << "ERROR: " << name << " is too long; maximum is " << (N - 1)
                  << " bytes." << std::endl;
        return false;
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    return true;
}

class MdSpi final : public CThostFtdcMdSpi {
public:
    MdSpi(CThostFtdcMdApi& api, std::string password)
        : api_(api), password_(std::move(password)) {}

    void OnFrontConnected() override {
        log("CALLBACK OnFrontConnected");

        CThostFtdcReqUserLoginField request{};
        if (!copyField(request.BrokerID, kBrokerId, "BrokerID") ||
            !copyField(request.UserID, kUserId, "UserID") ||
            !copyField(request.Password, password_, "Password")) {
            finish(false);
            return;
        }

        log("CALL ReqUserLogin request_id=1");
        const int result = api_.ReqUserLogin(&request, kRequestId);
        log("RETURN ReqUserLogin immediate_rc=" + std::to_string(result));
        if (result != 0) {
            finish(false);
        }
    }

    void OnRspUserLogin(CThostFtdcRspUserLoginField* login,
                        CThostFtdcRspInfoField* info,
                        int requestId,
                        bool isLast) override {
        const int errorId = info == nullptr ? 0 : info->ErrorID;
        const char* errorMessage = info == nullptr ? "" : info->ErrorMsg;

        log("CALLBACK OnRspUserLogin request_id=" + std::to_string(requestId) +
            " is_last=" + std::to_string(isLast ? 1 : 0) +
            " error_id=" + std::to_string(errorId) +
            " error_msg=" + errorMessage);

        if (login != nullptr) {
            log("LOGIN_DATA trading_day=" + std::string(login->TradingDay) +
                " login_time=" + login->LoginTime +
                " front_id=" + std::to_string(login->FrontID) +
                " session_id=" + std::to_string(login->SessionID));
        }

        if (isLast) {
            finish(errorId == 0 && login != nullptr);
        }
    }

    void OnRspError(CThostFtdcRspInfoField* info,
                    int requestId,
                    bool isLast) override {
        const int errorId = info == nullptr ? 0 : info->ErrorID;
        const char* errorMessage = info == nullptr ? "" : info->ErrorMsg;
        log("CALLBACK OnRspError request_id=" + std::to_string(requestId) +
            " is_last=" + std::to_string(isLast ? 1 : 0) +
            " error_id=" + std::to_string(errorId) +
            " error_msg=" + errorMessage);
        if (isLast) {
            finish(false);
        }
    }

    void OnFrontDisconnected(int reason) override {
        log("CALLBACK OnFrontDisconnected reason=" + std::to_string(reason));
    }

    void OnHeartBeatWarning(int timeLapse) override {
        log("CALLBACK OnHeartBeatWarning time_lapse=" + std::to_string(timeLapse));
    }

    bool wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_.wait_for(lock, kTimeout, [this] { return done_; })) {
            log("RESULT FAIL timeout: no final login/error callback within 60 seconds");
            return false;
        }
        return success_;
    }

private:
    void finish(bool success) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
            success_ = success;
        }
        condition_.notify_one();
    }

    CThostFtdcMdApi& api_;
    std::string password_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
    bool success_ = false;
};

}  // namespace

int main() {
    std::cout << "Minimal CTP stock-options MD login test" << std::endl;
    std::cout << "API: " << CThostFtdcMdApi::GetApiVersion() << std::endl;
    std::cout << "Front: " << kFront << std::endl;
    std::cout << "BrokerID: " << kBrokerId << std::endl;
    std::cout << "UserID: " << kUserId << std::endl;
    std::cout << "Scope: connect and login only; no subscription or trading" << std::endl;

    const std::string password = readPassword();
    if (password.empty()) {
        std::cerr << "ERROR: Password was empty." << std::endl;
        return 2;
    }

    std::filesystem::create_directories("flow_minimal_md");
    CThostFtdcMdApi* api = CThostFtdcMdApi::CreateFtdcMdApi("flow_minimal_md\\");
    if (api == nullptr) {
        std::cerr << "ERROR: CreateFtdcMdApi returned null." << std::endl;
        return 2;
    }

    MdSpi spi(*api, password);
    api->RegisterSpi(&spi);
    char front[] = "tcp://101.226.254.157:32213";
    api->RegisterFront(front);
    log("CALL Init");
    api->Init();

    const bool success = spi.wait();
    api->RegisterSpi(nullptr);
    api->Release();

    log(success ? "RESULT PASS login succeeded" : "RESULT FAIL login did not succeed");
    return success ? 0 : 1;
}
