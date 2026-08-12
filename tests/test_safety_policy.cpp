#include "TestRunner.hpp"
#include "core/SafetyPolicy.hpp"

using namespace spacelens;

SPACELENS_TEST(Safety_windows_protected)
{
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Windows\\System32") ==
                      LocationSafety::Protected);
}

SPACELENS_TEST(Safety_program_files_protected)
{
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Program Files\\App") ==
                      LocationSafety::Protected);
}

SPACELENS_TEST(Safety_drive_root_protected)
{
    SPACELENS_REQUIRE(classifyLocation(L"C:\\") == LocationSafety::Protected);
}

SPACELENS_TEST(Safety_profile_root_sensitive)
{
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Users\\Tung") ==
                      LocationSafety::Sensitive);
}

SPACELENS_TEST(Safety_user_project_ordinary)
{
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Users\\Tung\\Projects\\App") ==
                      LocationSafety::Ordinary);
}

SPACELENS_TEST(Safety_normalize_trailing_slash)
{
    const auto a = normalizePathForPolicy(L"C:\\Users\\Tung\\");
    const auto b = normalizePathForPolicy(L"C:\\Users\\Tung");
    SPACELENS_REQUIRE(a == b);
    SPACELENS_REQUIRE(classifyLocation(a) == LocationSafety::Sensitive);
}

SPACELENS_TEST(Safety_mutation_disallowed_protected)
{
    SPACELENS_REQUIRE(isMutationDisallowed(LocationSafety::Protected));
    SPACELENS_REQUIRE(!isMutationDisallowed(LocationSafety::Ordinary));
}
