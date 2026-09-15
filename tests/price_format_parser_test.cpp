// Test the exact production parser used by scripts/test_price_format.ps1.
// No credentials, API objects, counter files or network are used.
#define main connectivity_entry_not_called
#include "../src/main.cpp"
#undef main

std::string probe(const std::string& raw) {
    std::vector<std::string> args{"parser_test", "--price", raw, "--price-format-test-stop"};
    std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data());
    try { (void)parseOptions(static_cast<int>(argv.size()), argv.data()); }
    catch (const std::exception& e) { return e.what(); }
    return "PARSER_DID_NOT_STOP";
}
int main() {
    try {
        for (const auto* raw : {"abc", "0.0427abc", "NaN", "0", "-1", "1.2.3"}) {
            if (probe(raw) != "--price must be a positive finite number.")
                throw std::runtime_error("Expected production price-format rejection");
        }
        for (const auto* raw : {"0.0427", "1"}) {
            if (probe(raw) != "Unknown option; use --help.")
                throw std::runtime_error("Valid control did not stop at unknown option");
        }
        std::cout << "PASS 8 production parser checks; no SDK connection or orders\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL price format parser: " << e.what() << '\n'; return 1;
    }
}
