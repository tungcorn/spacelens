#include "core/Classification.hpp"

#include "core/DirectoryTree.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>

#include <cctype>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

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

bool endsWithIgnoreCase(std::wstring_view value, std::wstring_view suffix)
{
    if (value.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (std::towlower(value[offset + i]) != std::towlower(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool containsNameIgnoreCase(const std::wstring* names, std::size_t count,
                            std::wstring_view expected)
{
    if (names == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (equalsIgnoreCase(names[i], expected)) {
            return true;
        }
    }
    return false;
}

bool containsNameSuffixIgnoreCase(const std::wstring* names, std::size_t count,
                                  std::wstring_view suffix)
{
    if (names == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (endsWithIgnoreCase(names[i], suffix)) {
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

bool isOneOf(std::wstring_view value, std::initializer_list<std::wstring_view> choices)
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

std::string narrowAscii(std::wstring_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch < 128) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::wstring normalizePathKey(std::wstring_view path)
{
    std::wstring out(path);
    for (wchar_t& ch : out) {
        if (ch == L'/') {
            ch = L'\\';
        } else {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
    }
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

std::wstring knownFolderPath(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(id, 0, nullptr, &path);
    if (FAILED(hr) || path == nullptr) {
        if (path != nullptr) {
            ::CoTaskMemFree(path);
        }
        return {};
    }
    std::wstring out(path);
    ::CoTaskMemFree(path);
    return out;
}

std::wstring queryTempPath()
{
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD n = ::GetTempPathW(MAX_PATH, buffer);
    if (n == 0 || n > MAX_PATH) {
        return {};
    }
    std::wstring out(buffer, n);
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) {
        out.pop_back();
    }
    return out;
}

Classification makeClassification(StorageCategory category, Confidence confidence,
                                  const char* ruleId, const char* reason,
                                  const char* ecosystem = "",
                                  std::wstring_view marker = {})
{
    Classification result;
    result.category = category;
    result.confidence = confidence;
    result.ruleId = ruleId;
    result.reason = reason;
    result.ecosystem = ecosystem;
    result.marker = narrowAscii(marker);
    return result;
}

bool hasProjectSibling(const std::wstring* siblingNames, std::size_t siblingCount)
{
    return containsNameIgnoreCase(siblingNames, siblingCount, L"CMakeLists.txt") ||
           containsNameIgnoreCase(siblingNames, siblingCount, L"CMakeCache.txt") ||
           containsNameIgnoreCase(siblingNames, siblingCount, L"package.json") ||
           containsNameIgnoreCase(siblingNames, siblingCount, L"Cargo.toml") ||
           containsNameIgnoreCase(siblingNames, siblingCount, L"pyproject.toml") ||
           containsNameIgnoreCase(siblingNames, siblingCount, L"requirements.txt") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".csproj") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".fsproj") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".vbproj") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".sln");
}

bool hasDotnetSibling(const std::wstring* siblingNames, std::size_t siblingCount)
{
    return containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".csproj") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".fsproj") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".vbproj") ||
           containsNameSuffixIgnoreCase(siblingNames, siblingCount, L".sln");
}

}  // namespace

const std::wstring& knownDownloadsFolder()
{
    static const std::wstring path = knownFolderPath(FOLDERID_Downloads);
    return path;
}

const std::wstring& knownTempFolder()
{
    static const std::wstring path = queryTempPath();
    return path;
}

const std::wstring& knownLocalAppDataFolder()
{
    static const std::wstring path = knownFolderPath(FOLDERID_LocalAppData);
    return path;
}

bool pathIsUnderFolder(std::wstring_view path, std::wstring_view folder)
{
    if (path.empty() || folder.empty()) {
        return false;
    }
    const std::wstring normalizedPath = normalizePathKey(path);
    const std::wstring normalizedFolder = normalizePathKey(folder);
    if (normalizedFolder.empty()) {
        return false;
    }
    if (normalizedPath == normalizedFolder) {
        return true;
    }
    if (normalizedPath.size() <= normalizedFolder.size()) {
        return false;
    }
    if (normalizedPath.compare(0, normalizedFolder.size(), normalizedFolder) != 0) {
        return false;
    }
    return normalizedPath[normalizedFolder.size()] == L'\\';
}

bool pathHasComponent(std::wstring_view path, std::wstring_view component)
{
    if (path.empty() || component.empty()) {
        return false;
    }
    const std::wstring normalized = normalizePathKey(path);
    std::wstring needle(component);
    for (wchar_t& ch : needle) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    std::size_t pos = 0;
    if (normalized.size() >= 2 && normalized[1] == L':') {
        pos = 2;
    }
    while (pos < normalized.size()) {
        while (pos < normalized.size() && normalized[pos] == L'\\') {
            ++pos;
        }
        if (pos == normalized.size()) {
            break;
        }
        const std::size_t begin = pos;
        while (pos < normalized.size() && normalized[pos] != L'\\') {
            ++pos;
        }
        if (equalsIgnoreCase(std::wstring_view(normalized).substr(begin, pos - begin),
                             needle)) {
            return true;
        }
    }
    return false;
}

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

bool isKnownStorageCategoryName(std::string_view text) noexcept
{
    const std::string value = compactLowerAscii(text);
    if (value == "unknown") {
        return true;
    }
    return parseStorageCategory(text) != StorageCategory::Unknown;
}

Classification classifyDirectory(std::wstring_view directoryName,
                                 std::wstring_view fullPath,
                                 const std::wstring* childNames,
                                 std::size_t childCount,
                                 const std::wstring* siblingNames,
                                 std::size_t siblingCount)
{
    if (containsNameIgnoreCase(childNames, childCount, L"CMakeCache.txt") &&
        containsNameIgnoreCase(childNames, childCount, L"CMakeFiles")) {
        return makeClassification(StorageCategory::BuildArtifact, Confidence::High,
                                  "cmake-build-dir",
                                  "Directory contains CMakeCache.txt and CMakeFiles/",
                                  "cmake", L"CMakeCache.txt");
    }

    if (containsNameIgnoreCase(childNames, childCount, L"CMakeCache.txt") ||
        containsNameIgnoreCase(childNames, childCount, L"CMakeFiles")) {
        return makeClassification(StorageCategory::BuildArtifact, Confidence::Medium,
                                  "cmake-build-partial",
                                  "Directory contains a CMake build marker", "cmake",
                                  L"CMakeFiles");
    }

    if (containsNameIgnoreCase(childNames, childCount, L"pyvenv.cfg")) {
        return makeClassification(StorageCategory::DependencyDirectory, Confidence::High,
                                  "python-venv",
                                  "Directory contains pyvenv.cfg (Python virtualenv)",
                                  "python", L"pyvenv.cfg");
    }

    if (isOneOf(directoryName, {L".venv", L"venv"}) &&
        (containsNameIgnoreCase(siblingNames, siblingCount, L"pyproject.toml") ||
         containsNameIgnoreCase(siblingNames, siblingCount, L"requirements.txt") ||
         containsNameIgnoreCase(siblingNames, siblingCount, L"Pipfile") ||
         containsNameIgnoreCase(siblingNames, siblingCount, L"setup.py") ||
         containsNameIgnoreCase(siblingNames, siblingCount, L"setup.cfg"))) {
        return makeClassification(
            StorageCategory::DependencyDirectory, Confidence::Medium, "python-venv-name",
            "venv/.venv next to a Python project marker", "python", directoryName);
    }

    if (equalsIgnoreCase(directoryName, L"node_modules")) {
        return makeClassification(StorageCategory::DependencyDirectory, Confidence::High,
                                  "node-modules", "Directory name is node_modules",
                                  "node", L"node_modules");
    }

    if (equalsIgnoreCase(directoryName, L".git")) {
        return makeClassification(StorageCategory::ApplicationData, Confidence::Medium,
                                  "git-metadata", "Directory contains Git metadata",
                                  "git", L".git");
    }

    if (equalsIgnoreCase(directoryName, L"__pycache__")) {
        return makeClassification(StorageCategory::BuildArtifact, Confidence::High,
                                  "python-cache",
                                  "Directory contains Python bytecode cache data",
                                  "python", L"__pycache__");
    }

    if (isOneOf(directoryName, {L".vs", L".idea", L".vscode"})) {
        return makeClassification(
            StorageCategory::IdeCache, Confidence::Medium, "ide-cache-dir",
            "Directory name matches a common IDE cache/configuration directory", "",
            directoryName);
    }

    if (equalsIgnoreCase(directoryName, L"target") &&
        (containsNameIgnoreCase(siblingNames, siblingCount, L"Cargo.toml") ||
         containsNameIgnoreCase(childNames, childCount, L"CACHEDIR.TAG"))) {
        return makeClassification(
            StorageCategory::BuildArtifact, Confidence::High, "rust-target-dir",
            "Rust target/ directory next to Cargo.toml or CACHEDIR.TAG", "rust",
            L"target");
    }

    if (isOneOf(directoryName, {L"bin", L"obj"}) &&
        hasDotnetSibling(siblingNames, siblingCount)) {
        return makeClassification(
            StorageCategory::BuildArtifact, Confidence::High, "dotnet-bin-obj",
            "bin/ or obj/ next to a .NET project or solution file", "dotnet",
            directoryName);
    }

    if (isOneOf(directoryName, {L"dist", L"out"}) &&
        hasProjectSibling(siblingNames, siblingCount)) {
        return makeClassification(
            StorageCategory::BuildArtifact, Confidence::Medium, "dist-out-project",
            "dist/ or out/ next to a project marker", "", directoryName);
    }

    if (startsWithIgnoreCase(directoryName, L"cmake-build-")) {
        return makeClassification(
            StorageCategory::BuildArtifact, Confidence::Medium, "cmake-build-prefix",
            "Directory name matches a CMake generated-build prefix", "cmake",
            directoryName);
    }

    if (equalsIgnoreCase(directoryName, L"build") &&
        hasProjectSibling(siblingNames, siblingCount)) {
        return makeClassification(
            StorageCategory::BuildArtifact, Confidence::Medium, "build-dir-project",
            "build/ next to a project marker", "", L"build");
    }

    if (isOneOf(directoryName, {L"Debug", L"Release", L"x64"}) &&
        (hasDotnetSibling(siblingNames, siblingCount) ||
         containsNameIgnoreCase(siblingNames, siblingCount, L"CMakeLists.txt") ||
         containsNameIgnoreCase(childNames, childCount, L"CMakeCache.txt"))) {
        return makeClassification(
            StorageCategory::BuildArtifact, Confidence::Medium, "msvc-config-dir",
            "MSVC configuration directory next to project/build evidence", "dotnet",
            directoryName);
    }

    if (isOneOf(directoryName,
                {L".nuget", L".m2", L".gradle", L"pip-cache", L"pip_cache", L"pipcache",
                 L".pip"})) {
        return makeClassification(
            StorageCategory::PackageCache, Confidence::Medium, "package-cache-name",
            "Directory name matches a package-manager cache pattern",
            equalsIgnoreCase(directoryName, L".nuget")   ? "nuget"
            : equalsIgnoreCase(directoryName, L".m2")    ? "java"
            : equalsIgnoreCase(directoryName, L".gradle") ? "java"
                                                         : "python",
            directoryName);
    }

    if (equalsIgnoreCase(directoryName, L"packages") &&
        (pathHasComponent(fullPath, L".nuget") ||
         containsNameIgnoreCase(siblingNames, siblingCount, L".nuget") ||
         hasDotnetSibling(siblingNames, siblingCount))) {
        return makeClassification(
            StorageCategory::PackageCache, Confidence::High, "nuget-packages-path",
            "packages/ under a NuGet or .NET project context", "nuget", L"packages");
    }

    if (equalsIgnoreCase(directoryName, L"NuGet") &&
        pathIsUnderFolder(fullPath, knownLocalAppDataFolder())) {
        return makeClassification(
            StorageCategory::PackageCache, Confidence::Medium, "nuget-localappdata",
            "NuGet directory under the user's LocalAppData", "nuget", L"NuGet");
    }

    if (pathIsUnderFolder(fullPath, knownTempFolder())) {
        return makeClassification(
            StorageCategory::TemporaryData, Confidence::High, "known-temp-folder",
            "Path is under the Windows user temporary directory", "", L"Temp");
    }

    if (equalsIgnoreCase(directoryName, L".cache")) {
        return makeClassification(
            StorageCategory::TemporaryData, Confidence::Medium, "dot-cache-name",
            "Directory name is .cache (common toolchain cache)", "", L".cache");
    }

    if (isOneOf(directoryName, {L"temp", L"tmp", L"cache"})) {
        return makeClassification(
            StorageCategory::TemporaryData, Confidence::Low, "temp-cache-name-weak",
            "Directory name matches a generic temp/cache word without extra context",
            "", directoryName);
    }

    if (isOneOf(directoryName, {L"log", L"logs"})) {
        return makeClassification(StorageCategory::LogData, Confidence::Medium,
                                  "logs-dir-name", "Directory name suggests log data",
                                  "", directoryName);
    }

    return makeClassification(StorageCategory::Unknown, Confidence::Low,
                              "unknown-directory",
                              "No deterministic directory rule matched");
}

Classification classifyFile(std::wstring_view fileName, std::wstring_view fullPath)
{
    const std::wstring_view extension = extensionOf(fileName);

    if (isOneOf(extension,
                {L"gguf", L"onnx", L"pt", L"pth", L"safetensors", L"ckpt"})) {
        return makeClassification(
            StorageCategory::DownloadedAiModel, Confidence::High, "ai-model-extension",
            "File extension matches a common downloaded AI model format", "",
            extension);
    }

    if (isOneOf(extension, {L"zip", L"7z", L"rar", L"tar", L"gz", L"tgz", L"bz2"})) {
        auto cls = makeClassification(
            StorageCategory::Archive, Confidence::Medium, "archive-extension",
            "File extension matches a common archive format", "", extension);
        if (pathIsUnderFolder(fullPath, knownDownloadsFolder())) {
            cls.marker = "downloads";
        }
        return cls;
    }

    if (isOneOf(extension, {L"iso", L"img"})) {
        auto cls = makeClassification(
            StorageCategory::Archive, Confidence::Medium, "disk-image-extension",
            "File extension matches a disk image format", "", extension);
        if (pathIsUnderFolder(fullPath, knownDownloadsFolder())) {
            cls.marker = "downloads";
        }
        return cls;
    }

    if (isOneOf(extension, {L"msi", L"msu", L"cab"})) {
        auto cls = makeClassification(
            StorageCategory::Archive, Confidence::Medium, "installer-extension",
            "File extension matches a Windows installer/package format", "",
            extension);
        if (pathIsUnderFolder(fullPath, knownDownloadsFolder())) {
            cls.marker = "downloads";
        }
        return cls;
    }

    if (equalsIgnoreCase(extension, L"log")) {
        return makeClassification(StorageCategory::LogData, Confidence::Medium,
                                  "log-extension", "File extension is .log", "",
                                  extension);
    }

    if (isOneOf(extension,
                {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"mp3", L"wav", L"flac",
                 L"jpg", L"jpeg", L"png", L"gif", L"webp", L"bmp", L"pdf", L"doc",
                 L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt", L"md", L"rtf",
                 L"csv"})) {
        return makeClassification(
            StorageCategory::UserData, Confidence::Medium, "user-media-document-extension",
            "File extension matches a common user media/document type", "", extension);
    }

    if (isOneOf(extension, {L"tmp", L"temp", L"bak", L"old"})) {
        return makeClassification(
            StorageCategory::TemporaryData, Confidence::Low, "temporary-extension",
            "File extension suggests temporary or backup data", "", extension);
    }

    return makeClassification(StorageCategory::Unknown, Confidence::Low, "unknown-file",
                              "No deterministic file rule matched");
}

Classification classifyDirectoryFromTree(const DirectoryTree& tree, DirIndex idx)
{
    if (tree.empty() || idx == InvalidDirIndex) {
        return makeClassification(StorageCategory::Unknown, Confidence::Low,
                                  "unknown-directory",
                                  "No deterministic directory rule matched");
    }
    const auto& node = tree.dir(idx);
    std::vector<std::wstring> children;
    children.reserve(node.children.size() + node.files.size());
    for (const DirIndex child : node.children) {
        children.push_back(tree.dir(child).name);
    }
    for (const FileIndex file : node.files) {
        children.push_back(tree.file(file).name);
    }

    std::vector<std::wstring> siblings;
    if (node.parent != InvalidDirIndex) {
        const auto& parent = tree.dir(node.parent);
        siblings.reserve(parent.children.size() + parent.files.size());
        for (const DirIndex child : parent.children) {
            if (child != idx) {
                siblings.push_back(tree.dir(child).name);
            }
        }
        for (const FileIndex file : parent.files) {
            siblings.push_back(tree.file(file).name);
        }
    }

    return classifyDirectory(node.name, tree.pathOfDirectory(idx), children.data(),
                             children.size(), siblings.data(), siblings.size());
}

}  // namespace spacelens
