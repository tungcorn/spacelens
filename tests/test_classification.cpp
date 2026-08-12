#include "TestRunner.hpp"
#include "core/Classification.hpp"

using namespace spacelens;

SPACELENS_TEST(Classification_cmake_build_high)
{
    const std::wstring children[] = {L"CMakeCache.txt", L"CMakeFiles", L"app.exe"};
    const auto cls =
        classifyDirectory(L"build", L"D:\\proj\\build", children, 3);
    SPACELENS_REQUIRE(cls.category == StorageCategory::BuildArtifact);
    SPACELENS_REQUIRE(cls.confidence == Confidence::High);
    SPACELENS_REQUIRE(cls.ruleId == "cmake-build-dir");
    SPACELENS_REQUIRE(!cls.reason.empty());
}

SPACELENS_TEST(Classification_unknown_directory)
{
    const std::wstring children[] = {L"readme.txt"};
    const auto cls =
        classifyDirectory(L"docs", L"D:\\proj\\docs", children, 1);
    SPACELENS_REQUIRE(cls.category == StorageCategory::Unknown);
}

SPACELENS_TEST(Classification_gguf_model)
{
    const auto cls = classifyFile(L"sample.gguf", L"D:\\Models\\sample.gguf");
    SPACELENS_REQUIRE(cls.category == StorageCategory::DownloadedAiModel);
    SPACELENS_REQUIRE(cls.confidence == Confidence::High);
}

SPACELENS_TEST(Classification_user_video)
{
    const auto cls = classifyFile(L"old-video.mp4", L"D:\\Videos\\old-video.mp4");
    SPACELENS_REQUIRE(cls.category == StorageCategory::UserData);
}
