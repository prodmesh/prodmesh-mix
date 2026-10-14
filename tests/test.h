// Minimal test harness: no dependencies, one binary, `mix-tests [filter]`.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace pm::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& msg);

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

}  // namespace pm::test

#define PM_CAT2(a, b) a##b
#define PM_CAT(a, b) PM_CAT2(a, b)
#define TEST(name)                                                   \
    static void PM_CAT(test_, name)();                               \
    static ::pm::test::Registrar PM_CAT(reg_, name)(#name,           \
                                                    PM_CAT(test_, name)); \
    static void PM_CAT(test_, name)()

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) ::pm::test::fail(__FILE__, __LINE__, #cond);       \
    } while (0)

#define CHECK_MSG(cond, msg)                                            \
    do {                                                                \
        if (!(cond)) ::pm::test::fail(__FILE__, __LINE__, (msg));       \
    } while (0)
