#include "TestRunner.hpp"
#include "core/SizeParse.hpp"

using namespace spacelens;

SPACELENS_TEST(SizeParse_bytes_plain)
{
    const auto r = parseSize("1024");
    SPACELENS_REQUIRE(r.error.empty());
    SPACELENS_REQUIRE_EQ(r.bytes, 1024ULL);
}

SPACELENS_TEST(SizeParse_kb_binary)
{
    const auto r = parseSize("1KB");
    SPACELENS_REQUIRE(r.error.empty());
    SPACELENS_REQUIRE_EQ(r.bytes, 1024ULL);
}

SPACELENS_TEST(SizeParse_mb_binary)
{
    const auto r = parseSize("500MB");
    SPACELENS_REQUIRE(r.error.empty());
    SPACELENS_REQUIRE_EQ(r.bytes, 500ULL * 1024ULL * 1024ULL);
}

SPACELENS_TEST(SizeParse_invalid_unit)
{
    const auto r = parseSize("10XB");
    SPACELENS_REQUIRE(!r.error.empty());
}

SPACELENS_TEST(SizeParse_empty)
{
    const auto r = parseSize("");
    SPACELENS_REQUIRE(!r.error.empty());
}
