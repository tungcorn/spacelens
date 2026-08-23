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
    SPACELENS_REQUIRE(!isMutationDisallowed(LocationSafety::Sensitive));
}

SPACELENS_TEST(Safety_all_drive_roots_and_system_paths_strictly_protected)
{
    // Drive roots
    SPACELENS_REQUIRE(classifyLocation(L"C:\\") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"D:\\") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"E:\\") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"c:/") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"d:/") == LocationSafety::Protected);
    SPACELENS_REQUIRE(isMutationDisallowed(classifyLocation(L"C:\\")));
    SPACELENS_REQUIRE(isMutationDisallowed(classifyLocation(L"D:\\")));

    // System directories
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Windows") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Windows\\System32\\cmd.exe") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Program Files") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Program Files (x86)") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\ProgramData") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\$Recycle.Bin") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"D:\\$Recycle.Bin") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\System Volume Information") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"D:\\System Volume Information") == LocationSafety::Protected);

    // Mutation must be strictly disallowed for all of them
    SPACELENS_REQUIRE(isMutationDisallowed(classifyLocation(L"C:\\Windows")));
    SPACELENS_REQUIRE(isMutationDisallowed(classifyLocation(L"C:\\Program Files")));
    SPACELENS_REQUIRE(isMutationDisallowed(classifyLocation(L"C:\\$Recycle.Bin")));
    SPACELENS_REQUIRE(isMutationDisallowed(classifyLocation(L"D:\\System Volume Information")));

    // Ordinary user storage / Zalo downloads should be allowed for mutation
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Users\\Tung\\Downloads\\test.png") == LocationSafety::Ordinary);
    SPACELENS_REQUIRE(!isMutationDisallowed(classifyLocation(L"C:\\Users\\Tung\\Downloads\\test.png")));
    SPACELENS_REQUIRE(!isMutationDisallowed(classifyLocation(L"D:\\Zalo Received Files\\test.png")));
    SPACELENS_REQUIRE(!isMutationDisallowed(classifyLocation(L"C:\\Users\\Tung\\AppData\\Local\\ZaloPC\\test.png")));
}
