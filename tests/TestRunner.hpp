#pragma once

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace spacelens::test {

struct Failure : std::exception {
    explicit Failure(std::string message)
        : m_message(std::move(message))
    {
    }

    [[nodiscard]] const char* what() const noexcept override
    {
        return m_message.c_str();
    }

private:
    std::string m_message;
};

using TestFn = void (*)();

inline std::vector<std::pair<const char*, TestFn>>& registry()
{
    static std::vector<std::pair<const char*, TestFn>> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, TestFn fn)
    {
        registry().emplace_back(name, fn);
    }
};

inline int runAll()
{
    int failed = 0;
    const char* only = std::getenv("SPACELENS_TEST_ONLY");
    for (const auto& [name, fn] : registry()) {
        if (only != nullptr && std::string_view{name}.find(only) ==
                                  std::string_view::npos) {
            continue;
        }
        try {
            fn();
            std::cout << "[ PASS ] " << name << std::endl;
        } catch (const Failure& ex) {
            std::cout << "[ FAIL ] " << name << " — " << ex.what() << std::endl;
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "[ FAIL ] " << name << " — unexpected: " << ex.what()
                      << std::endl;
            ++failed;
        } catch (...) {
            std::cout << "[ FAIL ] " << name << " — unknown exception" << std::endl;
            ++failed;
        }
    }

    const std::size_t selected = only == nullptr
                                     ? registry().size()
                                     : static_cast<std::size_t>(std::count_if(
                                           registry().begin(), registry().end(),
                                           [only](const auto& item) {
                                               return std::string_view{item.first}.find(
                                                          only) !=
                                                      std::string_view::npos;
                                           }));
    std::cout << selected - failed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace spacelens::test

#define SPACELENS_TEST(name)                                                   \
    static void name();                                                        \
    static ::spacelens::test::Registrar registrar_##name(#name, &name);        \
    static void name()

#define SPACELENS_REQUIRE(cond)                                                \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw ::spacelens::test::Failure(                                  \
                std::string("requirement failed: ") + #cond);                  \
        }                                                                      \
    } while (0)

#define SPACELENS_REQUIRE_EQ(a, b)                                             \
    do {                                                                       \
        const auto _va = (a);                                                  \
        const auto _vb = (b);                                                  \
        if (!(_va == _vb)) {                                                   \
            throw ::spacelens::test::Failure(                                  \
                std::string("expected equality: ") + #a + " == " + #b);        \
        }                                                                      \
    } while (0)
