#include "TestRunner.hpp"

#include "core/SizeFormatter.hpp"

using spacelens::ByteSize;
using spacelens::SizeFormatter;

SPACELENS_TEST(SizeFormatter_zero)
{
    SPACELENS_REQUIRE_EQ(SizeFormatter::format(0), std::string("0 B"));
}

SPACELENS_TEST(SizeFormatter_bytes)
{
    SPACELENS_REQUIRE_EQ(SizeFormatter::format(999), std::string("999 B"));
}

SPACELENS_TEST(SizeFormatter_kilobytes)
{
    SPACELENS_REQUIRE_EQ(SizeFormatter::format(1024), std::string("1.0 KB"));
    SPACELENS_REQUIRE_EQ(SizeFormatter::format(1536), std::string("1.5 KB"));
}

SPACELENS_TEST(SizeFormatter_megabytes)
{
    SPACELENS_REQUIRE_EQ(SizeFormatter::format(1024ULL * 1024ULL),
                         std::string("1.0 MB"));
}

SPACELENS_TEST(SizeFormatter_gigabytes)
{
    const ByteSize oneGb = 1024ULL * 1024ULL * 1024ULL;
    SPACELENS_REQUIRE_EQ(SizeFormatter::format(oneGb), std::string("1.0 GB"));
}
