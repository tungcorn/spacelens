#include "TestRunner.hpp"

#include "core/TopKCollector.hpp"

#include <algorithm>
#include <functional>
#include <random>
#include <vector>

using spacelens::TopKCollector;

struct IntLess {
    bool operator()(int a, int b) const { return a < b; }
};

SPACELENS_TEST(TopK_basic)
{
    TopKCollector<int, IntLess> top(3);
    for (int v : {1, 5, 3, 9, 2, 8, 4}) {
        top.consider(v);
    }
    const auto got = top.sortedDescending();
    SPACELENS_REQUIRE(got.size() == 3);
    SPACELENS_REQUIRE(got[0] == 9);
    SPACELENS_REQUIRE(got[1] == 8);
    SPACELENS_REQUIRE(got[2] == 5);
}

SPACELENS_TEST(TopK_fewer_than_k)
{
    TopKCollector<int, IntLess> top(10);
    top.consider(2);
    top.consider(1);
    const auto got = top.sortedDescending();
    SPACELENS_REQUIRE(got.size() == 2);
    SPACELENS_REQUIRE(got[0] == 2);
    SPACELENS_REQUIRE(got[1] == 1);
}

SPACELENS_TEST(TopK_k_zero)
{
    TopKCollector<int, IntLess> top(0);
    top.consider(42);
    SPACELENS_REQUIRE(top.size() == 0);
    SPACELENS_REQUIRE(top.sortedDescending().empty());
}

SPACELENS_TEST(TopK_matches_full_sort)
{
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 1'000'000);

    std::vector<int> data;
    data.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        data.push_back(dist(rng));
    }

    constexpr std::size_t k = 50;
    TopKCollector<int, IntLess> top(k);
    for (int v : data) {
        top.consider(v);
    }
    auto got = top.sortedDescending();

    std::vector<int> expected = data;
    std::sort(expected.begin(), expected.end(), std::greater<int>());
    expected.resize(k);

    SPACELENS_REQUIRE(got.size() == k);
    for (std::size_t i = 0; i < k; ++i) {
        SPACELENS_REQUIRE(got[i] == expected[i]);
    }
}
