#include "core/Classification.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

namespace spacelens {
namespace {

std::wstring toLower(std::wstring_view in)
{
    std::wstring out(in);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

std::string narrowAscii(std::wstring_view in)
{
    std::string out;
    out.reserve(in.size());
    for (const wchar_t ch : in) {
        if (ch < 128) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

bool childEquals(const std::wstring* children, std::size_t count, std::wstring_view name)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (equalsIgnoreCase(children[i], name)) {
            return true;
        }
    }
    return false;
}

std::wstring_view extensionOf(std::wstring_view name)
{
    const auto pos = name.find_last_of(L'.');
    if (pos == std::wstring_view::npos || pos + 1 >= name.size()) {
        return {};
    }
    // Ignore leading-dot names like ".gitignore"
    if (pos == 0) {
        return {};
    }
    return name.substr(pos + 1);
}

bool leafIs(std::wstring_view name, std::wstring_view expected)
{
    return equalsIgnoreCase(name, expected);
}

bool leafStartsWith(std::wstring_view name, std::wstring_view prefix)
{
    if (name.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(name[i]) != std::towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* toString(StorageCategory category) noexcept
{
    switch (category) {
    case StorageCategory::BuildArtifact:
        return "BuildArtifact";
    case StorageCategory::DependencyDirectory:
        return "DependencyDirectory";
    case StorageCategory::PackageCache:
        return "PackageCache";
    case StorageCategory::IdeCache:
        return "IdeCache";
    case StorageCategory::LogData:
        return "LogData";
    case StorageCategory::TemporaryData:
        return "TemporaryData";
    case StorageCategory::DownloadedAiModel:
        return "DownloadedAiModel";
    case StorageCategory::Archive:
        return "Archive";
    case StorageCategory::ApplicationData:
        return "ApplicationData";
    case StorageCategory::SystemData:
        return "SystemData";
    case StorageCategory::UserData:
        return "UserData";
    case StorageCategory::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* toString(Confidence confidence) noexcept
{
    switch (confidence) {
    case Confidence::High:
        return "High";
    case Confidence::Medium:
        return "Medium";
    case Confidence::Low:
        return "Low";
    }
    return "Low";
}

StorageCategory parseStorageCategory(std::string_view text) noexcept
{
    std::string lower;
    lower.reserve(text.size());
    for (unsigned char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    // Accept both enum-style and spaced names.
    if (lower == "buildartifact" || lower == "build_artifact" ||
        lower == "build artifact") {
        return StorageCategory::BuildArtifact;
    }
    if (lower == "dependencydirectory" || lower == "dependency_directory" ||
        lower == "dependency directory") {
        return StorageCategory::DependencyDirectory;
    }
    if (lower == "packagecache" || lower == "package_cache" ||
        lower == "package cache") {
        return StorageCategory::PackageCache;
    }
    if (lower == "idecache" || lower == "ide_cache" || lower == "ide cache") {
        return StorageCategory::IdeCache;
    }
    if (lower == "logdata" || lower == "log_data" || lower == "log data" ||
        lower == "log") {
        return StorageCategory::LogData;
    }
    if (lower == "temporarydata" || lower == "temporary_data" ||
        lower == "temporary data" || lower == "temp") {
        return StorageCategory::TemporaryData;
    }
    if (lower == "downloadedaimodel" || lower == "downloaded_ai_model" ||
        lower == "downloaded ai model" || lower == "ai_model") {
        return StorageCategory::DownloadedAiModel;
    }
    if (lower == "archive") {
        return StorageCategory::Archive;
    }
    if (lower == "applicationdata" || lower == "application_data" ||
        lower == "application data") {
        return StorageCategory::ApplicationData;
    }
    if (lower == "systemdata" || lower == "system_data" || lower == "system data") {
        return StorageCategory::SystemData;
    }
    if (lower == "userdata" || lower == "user_data" || lower == "user data") {
        return StorageCategory::UserData;
    }
    if (lower == "unknown") {
        return StorageCategory::Unknown;
    }
    return StorageCategory::Unknown;
}

Classification classifyDirectory(std::wstring_view directoryName,
                                 std::wstring_view /*fullPath*/,
                                 const std::wstring* childNames,
                                 std::size_t childCount)
{
    Classification out;
    const std::wstring leaf = toLower(directoryName);

    const bool hasCache =
        childNames && childEquals(childNames, childCount, L"CMakeCache.txt");
    const bool hasCMakeFiles =
        childNames && childEquals(childNames, childCount, L"CMakeFiles");
    if (hasCache && hasCMakeFiles) {
        out.category = StorageCategory::BuildArtifact;
        out.confidence = Confidence::High;
        out.ruleId = "cmake-build-dir";
        out.reason =
            "Directory contains CMakeCache.txt and CMakeFiles/";
        return out;
    }
    if (hasCache || hasCMakeFiles) {
        out.category = StorageCategory::BuildArtifact;
        out.confidence = Confidence::Medium;
        out.ruleId = "cmake-build-partial";
        out.reason = "Directory contains CMake build markers";
        return out;
    }

    if (leafIs(leaf, L"node_modules")) {
        out.category = StorageCategory::DependencyDirectory;
        out.confidence = Confidence::High;
        out.ruleId = "node-modules";
        out.reason = "Directory name is node_modules";
        return out;
    }
    if (leafIs(leaf, L".git")) {
        out.category = StorageCategory::ApplicationData;
        out.confidence = Confidence::Medium;
        out.ruleId = "git-metadata";
        out.reason = "Git metadata directory";
        return out;
    }
    if (leafIs(leaf, L"__pycache__") || leafIs(leaf, L".pytest_cache")) {
        out.category = StorageCategory::BuildArtifact;
        out.confidence = Confidence::High;
        out.ruleId = "python-cache";
        out.reason = "Python cache directory";
        return out;
    }
    if (leafIs(leaf, L".vs") || leafIs(leaf, L".idea") || leafIs(leaf, L".vscode")) {
        out.category = StorageCategory::IdeCache;
        out.confidence = Confidence::Medium;
        out.ruleId = "ide-dir";
        out.reason = "IDE configuration/cache directory";
        return out;
    }
    if (leafIs(leaf, L"build") || leafIs(leaf, L"out") || leafIs(leaf, L"bin") ||
        leafIs(leaf, L"obj") || leafIs(leaf, L"debug") || leafIs(leaf, L"release") ||
        leafIs(leaf, L"x64") || leafIs(leaf, L"x86") ||
        leafStartsWith(leaf, L"cmake-build")) {
        out.category = StorageCategory::BuildArtifact;
        out.confidence = Confidence::Medium;
        out.ruleId = "build-dir-name";
        out.reason = "Directory name matches common build output pattern";
        return out;
    }
    if (leafIs(leaf, L".nuget") || leafIs(leaf, L"packages") ||
        leafIs(leaf, L".m2") || leafIs(leaf, L"pip-cache") ||
        leafIs(leaf, L".gradle") || leafIs(leaf, L"Carthage") ||
        leafIs(leaf, L"Pods")) {
        out.category = StorageCategory::PackageCache;
        out.confidence = Confidence::Medium;
        out.ruleId = "package-cache-name";
        out.reason = "Directory name matches package/dependency cache pattern";
        return out;
    }
    if (leafIs(leaf, L"temp") || leafIs(leaf, L"tmp") || leafIs(leaf, L"cache") ||
        leafIs(leaf, L".cache")) {
        out.category = StorageCategory::TemporaryData;
        out.confidence = Confidence::Medium;
        out.ruleId = "temp-cache-name";
        out.reason = "Directory name matches temporary/cache pattern";
        return out;
    }
    if (leafIs(leaf, L"logs") || leafIs(leaf, L"log")) {
        out.category = StorageCategory::LogData;
        out.confidence = Confidence::Medium;
        out.ruleId = "logs-dir-name";
        out.reason = "Directory name suggests log data";
        return out;
    }

    out.category = StorageCategory::Unknown;
    out.confidence = Confidence::Low;
    out.ruleId = "unknown-directory";
    out.reason = "No deterministic directory rule matched";
    return out;
}

Classification classifyFile(std::wstring_view fileName,
                            std::wstring_view /*fullPath*/)
{
    Classification out;
    const std::wstring ext = toLower(extensionOf(fileName));
    const std::string extA = narrowAscii(ext);

    if (extA == "gguf" || extA == "onnx" || extA == "pt" || extA == "pth" ||
        extA == "safetensors" || extA == "ckpt") {
        out.category = StorageCategory::DownloadedAiModel;
        out.confidence = Confidence::High;
        out.ruleId = "ai-model-ext";
        out.reason = "File extension matches common AI model formats";
        return out;
    }
    if (extA == "zip" || extA == "7z" || extA == "rar" || extA == "tar" ||
        extA == "gz" || extA == "tgz" || extA == "bz2") {
        out.category = StorageCategory::Archive;
        out.confidence = Confidence::Medium;
        out.ruleId = "archive-ext";
        out.reason = "File extension is an archive format";
        return out;
    }
    if (extA == "log") {
        out.category = StorageCategory::LogData;
        out.confidence = Confidence::Medium;
        out.ruleId = "log-ext";
        out.reason = "File extension is .log";
        return out;
    }
    if (extA == "obj" || extA == "o" || extA == "pdb" || extA == "ilk" ||
        extA == "lib" || extA == "a" || extA == "pyc" || extA == "pyo") {
        out.category = StorageCategory::BuildArtifact;
        out.confidence = Confidence::Medium;
        out.ruleId = "build-object-ext";
        out.reason = "File extension matches compiler/build intermediate output";
        return out;
    }
    if (extA == "mp4" || extA == "mkv" || extA == "avi" || extA == "mov" ||
        extA == "mp3" || extA == "wav" || extA == "flac" || extA == "jpg" ||
        extA == "jpeg" || extA == "png" || extA == "gif" || extA == "webp" ||
        extA == "pdf" || extA == "doc" || extA == "docx" || extA == "xls" ||
        extA == "xlsx" || extA == "ppt" || extA == "pptx" || extA == "txt" ||
        extA == "md") {
        out.category = StorageCategory::UserData;
        out.confidence = Confidence::Medium;
        out.ruleId = "user-media-doc-ext";
        out.reason = "File extension matches common user media/document types";
        return out;
    }
    if (extA == "tmp" || extA == "temp" || extA == "bak" || extA == "old") {
        out.category = StorageCategory::TemporaryData;
        out.confidence = Confidence::Low;
        out.ruleId = "temp-ext";
        out.reason = "File extension suggests temporary/backup data";
        return out;
    }

    out.category = StorageCategory::Unknown;
    out.confidence = Confidence::Low;
    out.ruleId = "unknown-file";
    out.reason = "No deterministic file rule matched";
    return out;
}

}  // namespace spacelens
