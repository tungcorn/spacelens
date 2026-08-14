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

SPACELENS_TEST(Classification_photos_build_not_generated)
{
    const std::wstring children[] = {L"IMG_001.jpg", L"IMG_002.jpg"};
    const auto cls =
        classifyDirectory(L"build", L"D:\\Photos\\build", children, 2);
    SPACELENS_REQUIRE(cls.category == StorageCategory::Unknown);
    SPACELENS_REQUIRE(cls.ruleId == "unknown-directory");
}

SPACELENS_TEST(Classification_build_photos_component_not_build)
{
    SPACELENS_REQUIRE(!pathHasComponent(L"D:\\Family\\Build Photos", L"build"));
    SPACELENS_REQUIRE(pathHasComponent(L"D:\\Family\\build\\raw", L"build"));
}

SPACELENS_TEST(Classification_name_only_temp_is_low)
{
    const auto temp =
        classifyDirectory(L"temp", L"D:\\Projects\\temp", nullptr, 0);
    SPACELENS_REQUIRE(temp.category == StorageCategory::TemporaryData);
    SPACELENS_REQUIRE(temp.confidence == Confidence::Low);
    const auto cache =
        classifyDirectory(L"cache", L"D:\\Projects\\cache", nullptr, 0);
    SPACELENS_REQUIRE(cache.confidence == Confidence::Low);
}

SPACELENS_TEST(Classification_dot_cache_is_medium)
{
    const auto cls =
        classifyDirectory(L".cache", L"D:\\proj\\.cache", nullptr, 0);
    SPACELENS_REQUIRE(cls.category == StorageCategory::TemporaryData);
    SPACELENS_REQUIRE(cls.confidence == Confidence::Medium);
}

SPACELENS_TEST(Classification_rust_target_with_cargo_sibling)
{
    const std::wstring siblings[] = {L"Cargo.toml", L"src"};
    const auto cls = classifyDirectory(L"target", L"D:\\rust-app\\target",
                                       nullptr, 0, siblings, 2);
    SPACELENS_REQUIRE(cls.category == StorageCategory::BuildArtifact);
    SPACELENS_REQUIRE(cls.confidence == Confidence::High);
    SPACELENS_REQUIRE(cls.ecosystem == "rust");
    SPACELENS_REQUIRE(cls.marker == "target");
}

SPACELENS_TEST(Classification_rust_target_without_context_unknown)
{
    const auto cls =
        classifyDirectory(L"target", L"D:\\Photos\\target", nullptr, 0);
    SPACELENS_REQUIRE(cls.category == StorageCategory::Unknown);
}

SPACELENS_TEST(Classification_dotnet_bin_obj_with_csproj)
{
    const std::wstring siblings[] = {L"App.csproj", L"Program.cs"};
    const auto bin = classifyDirectory(L"bin", L"D:\\dotnet-app\\bin", nullptr, 0,
                                       siblings, 2);
    const auto obj = classifyDirectory(L"obj", L"D:\\dotnet-app\\obj", nullptr, 0,
                                       siblings, 2);
    SPACELENS_REQUIRE(bin.category == StorageCategory::BuildArtifact);
    SPACELENS_REQUIRE(bin.confidence == Confidence::High);
    SPACELENS_REQUIRE(bin.ecosystem == "dotnet");
    SPACELENS_REQUIRE(obj.category == StorageCategory::BuildArtifact);
}

SPACELENS_TEST(Classification_bin_without_project_unknown)
{
    const auto cls = classifyDirectory(L"bin", L"D:\\Photos\\bin", nullptr, 0);
    SPACELENS_REQUIRE(cls.category == StorageCategory::Unknown);
}

SPACELENS_TEST(Classification_python_venv_pyvenv_cfg)
{
    const std::wstring children[] = {L"pyvenv.cfg", L"Scripts"};
    const auto cls =
        classifyDirectory(L".venv", L"D:\\py\\.venv", children, 2);
    SPACELENS_REQUIRE(cls.category == StorageCategory::DependencyDirectory);
    SPACELENS_REQUIRE(cls.confidence == Confidence::High);
    SPACELENS_REQUIRE(cls.ecosystem == "python");
}

SPACELENS_TEST(Classification_python_venv_name_with_project_sibling)
{
    const std::wstring siblings[] = {L"pyproject.toml", L"src"};
    const auto cls = classifyDirectory(L".venv", L"D:\\py\\.venv", nullptr, 0,
                                       siblings, 2);
    SPACELENS_REQUIRE(cls.category == StorageCategory::DependencyDirectory);
    SPACELENS_REQUIRE(cls.confidence == Confidence::Medium);
}

SPACELENS_TEST(Classification_iso_msi_are_archives)
{
    const auto iso = classifyFile(L"old.iso", L"D:\\Downloads\\old.iso");
    const auto msi = classifyFile(L"setup.msi", L"D:\\Downloads\\setup.msi");
    SPACELENS_REQUIRE(iso.category == StorageCategory::Archive);
    SPACELENS_REQUIRE(iso.ruleId == "disk-image-extension");
    SPACELENS_REQUIRE(msi.category == StorageCategory::Archive);
    SPACELENS_REQUIRE(msi.ruleId == "installer-extension");
}

SPACELENS_TEST(Classification_unicode_node_modules)
{
    const auto cls = classifyDirectory(L"node_modules",
                                       L"D:\\проекты\\アプリ\\node_modules",
                                       nullptr, 0);
    SPACELENS_REQUIRE(cls.category == StorageCategory::DependencyDirectory);
    SPACELENS_REQUIRE(cls.confidence == Confidence::High);
    SPACELENS_REQUIRE(cls.ecosystem == "node");
}

SPACELENS_TEST(Classification_known_temp_and_path_helpers)
{
    const auto& temp = knownTempFolder();
    if (!temp.empty()) {
        const std::wstring child = temp + L"\\spacelens-cls-probe";
        SPACELENS_REQUIRE(pathIsUnderFolder(child, temp));
        SPACELENS_REQUIRE(pathIsUnderFolder(temp, temp));
        SPACELENS_REQUIRE(!pathIsUnderFolder(L"D:\\not-temp", temp));
    }
    SPACELENS_REQUIRE(!pathIsUnderFolder(L"D:\\a", L""));
}

SPACELENS_TEST(Classification_parse_category_aliases)
{
    SPACELENS_REQUIRE(parseStorageCategory("BuildArtifact") ==
                      StorageCategory::BuildArtifact);
    SPACELENS_REQUIRE(parseStorageCategory("dependencydirectory") ==
                      StorageCategory::DependencyDirectory);
    SPACELENS_REQUIRE(parseStorageCategory("temp") ==
                      StorageCategory::TemporaryData);
    SPACELENS_REQUIRE(parseStorageCategory("nope") == StorageCategory::Unknown);
}

SPACELENS_TEST(Classification_near_matches_are_not_exact)
{
    const auto notes = classifyDirectory(
        L"node_modules_notes", L"D:\\Documents\\node_modules_notes", nullptr, 0);
    const auto cacheImp = classifyDirectory(
        L"cache-important", L"D:\\Archive\\cache-important", nullptr, 0);
    const auto targetSrc = classifyDirectory(
        L"target-source", L"D:\\Projects\\target-source", nullptr, 0);
    const auto backupsBin =
        classifyDirectory(L"bin", L"D:\\Backups\\bin", nullptr, 0);
    SPACELENS_REQUIRE(notes.category == StorageCategory::Unknown);
    SPACELENS_REQUIRE(cacheImp.category == StorageCategory::Unknown);
    SPACELENS_REQUIRE(targetSrc.category == StorageCategory::Unknown);
    SPACELENS_REQUIRE(backupsBin.category == StorageCategory::Unknown);
}
