// Minimal test helpers (no external dependency).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace test {

inline int failures = 0;
inline int checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++test::checks;                                                                                                \
        if (!(cond)) {                                                                                                 \
            ++test::failures;                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                              \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        ++test::checks;                                                                                                \
        auto va_ = (a);                                                                                                \
        auto vb_ = (b);                                                                                                \
        if (!(va_ == vb_)) {                                                                                           \
            ++test::failures;                                                                                          \
            std::printf("FAIL %s:%d: %s == %s  (%s vs %s)\n", __FILE__, __LINE__, #a, #b, test::str(va_).c_str(),      \
                        test::str(vb_).c_str());                                                                       \
        }                                                                                                              \
    } while (0)

inline std::string str(const std::string& s) {
    return "'" + s + "'";
}
inline std::string str(const char* s) {
    return std::string("'") + s + "'";
}
inline std::string str(bool b) {
    return b ? "true" : "false";
}
template <class T>
std::string str(const T& v) {
    return std::to_string(v);
}
inline std::string str(const std::uint8_t v) {
    return std::to_string(static_cast<int>(v));
}
inline std::string str(const std::vector<std::uint8_t>& v) {
    std::string s = "[" + std::to_string(v.size()) + " bytes]";
    return s;
}

inline int report(const char* name) {
    std::printf("%s: %d checks, %d failures\n", name, checks, failures);
    return failures ? 1 : 0;
}

} // namespace test
