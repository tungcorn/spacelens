#pragma once

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
    for (const auto& [name, fn] : registry()) {
        try {
            fn();
            std::cout << "[ PASS ] " << name << '\n';
        } catch (const Failure& ex) {
            std::cout << "[ FAIL ] " << name << " — " << ex.what() << '\n';
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "[ FAIL ] " << name << " — unexpected: " << ex.what()
                      << '\n';
            ++failed;
        } catch (...) {
            std::cout << "[ FAIL ] " << name << " — unknown exception\n";
            ++failed;
        }
    }

    std::cout << registry().size() - failed << " passed, " << failed
              << " failed\n";
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
        const auto& _va = (a);                                                 \
        const auto& _vb = (b);                                                 \
        if (!(_va == _vb)) {                                                   \
            throw ::spacelens::test::Failure(                                  \
                std::string("expected equality: ") + #a + " == " + #b);        \
        }                                                                      \
    } while (0)
