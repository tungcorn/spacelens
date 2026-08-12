#include "core/Classification.hpp"

#include <cctype>
#include <cwctype>
#include <string>
#include <string_view>

namespace spacelens {
namespace {

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

bool startsWithIgnoreCase(std::wstring_view value, std::wstring_view prefix)
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(value[i]) != std::towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool containsChildIgnoreCase(const std::wstring* childNames,
                             std::size_t childCount,
                             std::wstring_view expected)
{
    if (childNames == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < childCount; ++i) {
        if (equalsIgnoreCase(childNames[i], expected)) {
            return true;
        }
    }
    return false;
}

std::wstring_view extensionOf(std::wstring_view fileName)
{
    const std::size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= fileName.size()) {
        return {};
    }
    return fileName.substr(dot + 1);
}

bool isOneOf(std::wstring_view value,
             std::initializer_list<std::wstring_view> choices)
{
    for (const auto choice : choices) {
        if (equalsIgnoreCase(value, choice)) {
            return true;
        }
    }
    return false;
}

std::string compactLowerAscii(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) != 0) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return out;
}

Classification makeClassification(StorageCategory category,
                                  Confidence confidence,
                                  const char* ruleId,
                                  const char* reason)
{
    Classification result;
    result.category = category;
    result.confidence = confidence;
    result.ruleId = ruleId;
    result.reason = reason;
    return result;
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
    const std::string value = compactLowerAscii(text);
    if (value == "buildartifact") {
        return StorageCategory::BuildArtifact;
    }
    if (value == "dependencydirectory") {
        return StorageCategory::DependencyDirectory;
    }
    if (value == "packagecache") {
        return StorageCategory::PackageCache;
    }
    if (value == "idecache") {
        return StorageCategory::IdeCache;
    }
    if (value == "logdata" || value == "log") {
        return StorageCategory::LogData;
    }
    if (value == "temporarydata" || value == "temp") {
        return StorageCategory::TemporaryData;
    }
    if (value == "downloadedaimodel" || value == "aimodel") {
        return StorageCategory::DownloadedAiModel;
    }
    if (value == "archive") {
        return StorageCategory::Archive;
    }
    if (value == "applicationdata") {
        return StorageCategory::ApplicationData;
    }
    if (value == "systemdata") {
        return StorageCategory::SystemData;
    }
    if (value == "userdata") {
        return StorageCategory::UserData;
    }
    return StorageCategory::Unknown;
}

Classification classifyDirectory(std::wstring_view directoryName,
                                 std::wstring_view /*fullPath*/,
                                 const std::wstring* childNames,
                                 std::size_t childCount)
{
    if (containsChildIgnoreCase(childNames, childCount, L"CMakeCache.txt") &&
        containsChildIgnoreCase(childNames, childCount, L"CMakeFiles")) {
        return makeClassification(StorageCategory::BuildArtifact,
                                 Confidence::High,
                                 "cmake-build-dir",
                                 "Directory contains CMakeCache.txt and CMakeFiles/");
    }

    if (containsChildIgnoreCase(childNames, childCount, L"CMakeCache.txt") ||
        containsChildIgnoreCase(childNames, childCount, L"CMakeFiles")) {
        return makeClassification(StorageCategory::BuildArtifact,
                                 Confidence::Medium,
                                 "cmake-build-partial",
                                 "Directory contains a CMake build marker");
    }

    if (equalsIgnoreCase(directoryName, L"node_modules")) {
        return makeClassification(StorageCategory::DependencyDirectory,
                                 Confidence::High,
                                 "node-modules",
                                 "Directory name is node_modules");
    }

    if (equalsIgnoreCase(directoryName, L".git")) {
        return makeClassification(StorageCategory::ApplicationData,
                                 Confidence::Medium,
                                 "git-metadata",
                                 "Directory contains Git metadata");
    }

    if (equalsIgnoreCase(directoryName, L"__pycache__")) {
        return makeClassification(StorageCategory::BuildArtifact,
                                 Confidence::High,
                                 "python-cache",
                                 "Directory contains Python bytecode cache data");
    }

    if (isOneOf(directoryName, {L".vs", L".idea", L".vscode"})) {
        return makeClassification(StorageCategory::IdeCache,
                                 Confidence::Medium,
                                 "ide-cache-dir",
                                 "Directory name matches a common IDE cache/configuration directory");
    }

    if (equalsIgnoreCase(directoryName, L"build") ||
        equalsIgnoreCase(directoryName, L"Debug") ||
        equalsIgnoreCase(directoryName, L"Release") ||
        equalsIgnoreCase(directoryName, L"x64") ||
        startsWithIgnoreCase(directoryName, L"cmake-build-")) {
        return makeClassification(StorageCategory::BuildArtifact,
                                 Confidence::Medium,
                                 "build-dir-name",
                                 "Directory name matches a common build output pattern");
    }

    if (isOneOf(directoryName,
                {L".nuget", L".m2", L".gradle", L"packages", L"pip-cache",
                 L"pip_cache", L"pipcache", L".pip"})) {
        return makeClassification(StorageCategory::PackageCache,
                                 Confidence::Medium,
                                 "package-cache-name",
                                 "Directory name matches a package-manager cache pattern");
    }

    if (isOneOf(directoryName, {L"temp", L"tmp", L"cache", L".cache"})) {
        return makeClassification(StorageCategory::TemporaryData,
                                 Confidence::Medium,
                                 "temp-cache-name",
                                 "Directory name matches a temporary/cache pattern");
    }

    if (isOneOf(directoryName, {L"log", L"logs"})) {
        return makeClassification(StorageCategory::LogData,
                                 Confidence::Medium,
                                 "logs-dir-name",
                                 "Directory name suggests log data");
    }

    return makeClassification(StorageCategory::Unknown,
                              Confidence::Low,
                              "unknown-directory",
                              "No deterministic directory rule matched");
}

Classification classifyFile(std::wstring_view fileName,
                            std::wstring_view /*fullPath*/)
{
    const std::wstring_view extension = extensionOf(fileName);

    if (isOneOf(extension,
                {L"gguf", L"onnx", L"pt", L"pth", L"safetensors", L"ckpt"})) {
        return makeClassification(StorageCategory::DownloadedAiModel,
                                 Confidence::High,
                                 "ai-model-extension",
                                 "File extension matches a common downloaded AI model format");
    }

    if (isOneOf(extension,
                {L"zip", L"7z", L"rar", L"tar", L"gz", L"tgz", L"bz2"})) {
        return makeClassification(StorageCategory::Archive,
                                 Confidence::Medium,
                                 "archive-extension",
                                 "File extension matches a common archive format");
    }

    if (equalsIgnoreCase(extension, L"log")) {
        return makeClassification(StorageCategory::LogData,
                                 Confidence::Medium,
                                 "log-extension",
                                 "File extension is .log");
    }

    if (isOneOf(extension,
                {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"mp3", L"wav", L"flac",
                 L"jpg", L"jpeg", L"png", L"gif", L"webp", L"bmp", L"pdf", L"doc",
                 L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt", L"md", L"rtf",
                 L"csv"})) {
        return makeClassification(StorageCategory::UserData,
                                 Confidence::Medium,
                                 "user-media-document-extension",
                                 "File extension matches a common user media/document type");
    }

    if (isOneOf(extension, {L"tmp", L"temp", L"bak", L"old"})) {
        return makeClassification(StorageCategory::TemporaryData,
                                 Confidence::Low,
                                 "temporary-extension",
                                 "File extension suggests temporary or backup data");
    }

    return makeClassification(StorageCategory::Unknown,
                              Confidence::Low,
                              "unknown-file",
                              "No deterministic file rule matched");
}

}  // namespace spacelens
