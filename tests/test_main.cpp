#include <cstring>
#include <stdexcept>

#include "core/rt_guard.h"
#include "test.h"

namespace pm::test {

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

namespace {
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace

void fail(const char* file, int line, const std::string& msg) {
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + msg);
}

}  // namespace pm::test

int main(int argc, char** argv) {
    using namespace pm::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (const auto& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        pm::rt::resetViolations();
        try {
            c.fn();
            std::printf("  ok    %s\n", c.name);
            ++passed;
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n        %s\n", c.name, e.what());
            ++failed;
        }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    if (failed == 0) std::printf("PASS\n");
    return failed ? 1 : 0;
}
