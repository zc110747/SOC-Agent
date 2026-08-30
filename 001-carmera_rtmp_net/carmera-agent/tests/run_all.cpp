#include "test_harness.h"

int main() {
    int failed = 0;
    int total = 0;
    for (const auto& c : test::registry()) {
        ++total;
        std::cout << "[ RUN ] " << c.name << "\n";
        bool ok = c.fn();
        if (ok) {
            std::cout << "[ OK  ] " << c.name << "\n";
        } else {
            std::cout << "[FAIL ] " << c.name << "\n";
            ++failed;
        }
    }
    std::cout << "\n==== " << (total - failed) << "/" << total
              << " tests passed ====\n";
    return failed == 0 ? 0 : 1;
}
