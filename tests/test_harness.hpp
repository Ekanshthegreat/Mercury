// Mercury — minimal test harness.
//
// A single header so tests need no external framework. Tests declare cases
// with MERCURY_TEST(name) and assert with MERCURY_CHECK / MERCURY_CHECK_EQ.
// Registered cases are run by mercury::test::run_all(), which prints a summary
// and returns a non-zero exit code on the first failing assertion.

#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace mercury::test {

// Render a value for a failure message. Streamable types are printed directly;
// anything else (e.g. a plain enum without an operator<<) falls back to a
// readable "<unprintable>". This keeps MERCURY_CHECK_EQ usable for every
// comparable type, not just the ones that happen to have a stream operator.
template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                             << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
void debug_write(std::ostream& os, const T& value) {
    if constexpr (is_streamable<T>::value) {
        os << value;
    } else if constexpr (std::is_enum_v<T>) {
        os << "enum(" << static_cast<long long>(value) << ")";
    } else {
        os << "<unprintable>";
    }
}


struct Case {
    std::string name;
    void (*fn)();  // plain function pointer: no std::function, no ABI surprises
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) {
        registry().push_back(Case{name, fn});
    }
};

class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string msg) : msg_(std::move(msg)) {}
    [[nodiscard]] const char* what() const noexcept override { return msg_.c_str(); }

private:
    std::string msg_;
};

inline int run_all() {
    int failed = 0;
    for (const Case& c : registry()) {
        try {
            c.fn();
            std::cout << "[ PASS ] " << c.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[ FAIL ] " << c.name << " -- " << e.what() << "\n";
        }
    }
    std::cout << (failed == 0 ? "OK" : "FAILED") << ": " << registry().size()
              << " case(s), " << failed << " failure(s)\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace mercury::test

#define MERCURY_TEST(name)                                                    \
    static void mercury_test_##name();                                        \
    static ::mercury::test::Registrar mercury_registrar_##name(#name,         \
                                                              mercury_test_##name); \
    static void mercury_test_##name()

#define MERCURY_CHECK(cond)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::ostringstream oss;                                           \
            oss << __FILE__ << ":" << __LINE__ << " check failed: " #cond;    \
            throw ::mercury::test::AssertionFailure(oss.str());               \
        }                                                                     \
    } while (false)

#define MERCURY_CHECK_EQ(a, b)                                                \
    do {                                                                      \
        const auto lhs_ = (a);                                                \
        const auto rhs_ = (b);                                                \
        if (!(lhs_ == rhs_)) {                                                \
            std::ostringstream oss;                                           \
            oss << __FILE__ << ":" << __LINE__ << " expected " #a " == " #b   \
                << " (";                                                      \
            ::mercury::test::debug_write(oss, lhs_);                          \
            oss << " != ";                                                    \
            ::mercury::test::debug_write(oss, rhs_);                          \
            oss << ")";                                                       \
            throw ::mercury::test::AssertionFailure(oss.str());               \
        }                                                                     \
    } while (false)

#define MERCURY_CHECK_THROWS(expr)                                            \
    do {                                                                      \
        bool threw_ = false;                                                  \
        try {                                                                 \
            (void)(expr);                                                     \
        } catch (...) {                                                       \
            threw_ = true;                                                    \
        }                                                                     \
        if (!threw_) {                                                        \
            std::ostringstream oss;                                           \
            oss << __FILE__ << ":" << __LINE__ << " expected throw: " #expr;  \
            throw ::mercury::test::AssertionFailure(oss.str());               \
        }                                                                     \
    } while (false)

#define MERCURY_MAIN()                                                        \
    int main() { return ::mercury::test::run_all(); }
