#include "core/ReclaimProvider.hpp"

#include "core/Classification.hpp"
#include "core/Json.hpp"
#include "platform/windows/SafeProcess.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>

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

bool containsName(const std::wstring* names, std::size_t count,
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

bool containsSuffix(const std::wstring* names, std::size_t count,
                    std::wstring_view suffix)
{
    if (names == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (names[i].size() < suffix.size()) {
            continue;
        }
        if (equalsIgnoreCase(
                std::wstring_view(names[i]).substr(names[i].size() - suffix.size()),
                suffix)) {
            return true;
        }
    }
    return false;
}

std::wstring knownFolder(REFKNOWNFOLDERID id)
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

std::wstring userProfile()
{
    return knownFolder(FOLDERID_Profile);
}

std::wstring localAppData()
{
    return knownLocalAppDataFolder();
}

bool underKnown(std::wstring_view path, std::wstring_view folder)
{
    return !folder.empty() && pathIsUnderFolder(path, folder);
}

std::wstring parseDotnetLocalsPath(const std::string& stdoutUtf8)
{
    // Typical: "global-packages: C:\Users\me\.nuget\packages\"
    const auto colon = stdoutUtf8.find(':');
    if (colon == std::string::npos) {
        return {};
    }
    std::size_t i = colon + 1;
    while (i < stdoutUtf8.size() &&
           (stdoutUtf8[i] == ' ' || stdoutUtf8[i] == '\t')) {
        ++i;
    }
    std::size_t end = stdoutUtf8.size();
    while (end > i && (stdoutUtf8[end - 1] == '\r' || stdoutUtf8[end - 1] == '\n' ||
                       stdoutUtf8[end - 1] == ' ' || stdoutUtf8[end - 1] == '\t')) {
        --end;
    }
    if (end <= i) {
        return {};
    }
    return wideFromUtf8(std::string_view(stdoutUtf8).substr(i, end - i));
}

ReclaimProviderHit makeHit(const char* provider, const char* ecosystem,
                           const char* operation, ReclaimConsequence consequence,
                           ReclaimDisruption disruption,
                           ReclaimActionability actionability,
                           ReclaimConfidence confidence,
                           std::vector<std::string> evidence,
                           std::vector<std::string> reasons)
{
    ReclaimProviderHit hit;
    hit.ownership.provider = provider;
    hit.ownership.ecosystem = ecosystem;
    hit.ownership.evidence = std::move(evidence);
    hit.ownership.authoritative = confidence == ReclaimConfidence::Verified ||
                                  confidence == ReclaimConfidence::Strong;
    hit.action.kind = "provider_cleanup";
    hit.action.provider = provider;
    hit.action.operation = operation;
    hit.action.executionSupported = false;
    hit.action.humanAuthorizationRequired = true;
    hit.action.consequence = consequence;
    hit.disruption = disruption;
    hit.actionability = actionability;
    hit.confidence = confidence;
    hit.reasonCodes = std::move(reasons);
    return hit;
}

std::optional<ReclaimProviderHit> classifyCargo(const ReclaimProbeInput& in)
{
    if (in.kind != ItemKind::Directory) {
        return std::nullopt;
    }
    const bool namedTarget = equalsIgnoreCase(in.name, L"target");
    const bool cargoSibling = containsName(in.siblingNames, in.siblingCount,
                                           L"Cargo.toml");
    const bool cacheTag =
        containsName(in.childNames, in.childCount, L"CACHEDIR.TAG");
    if (!namedTarget) {
        return std::nullopt;
    }
    if (!cargoSibling && !cacheTag) {
        return std::nullopt;
    }
    std::vector<std::string> evidence;
    if (cargoSibling) {
        evidence.emplace_back("Cargo.toml");
    }
    if (cacheTag) {
        evidence.emplace_back("CACHEDIR.TAG");
    }
    evidence.emplace_back("cargo_target_directory");
    const ReclaimConfidence conf =
        cargoSibling ? ReclaimConfidence::Strong : ReclaimConfidence::Heuristic;
    return makeHit("cargo", "rust", "clean_generated_build_artifacts",
                   ReclaimConsequence::RebuildRequired, ReclaimDisruption::Moderate,
                   ReclaimActionability::ActionableWithoutContentJudgment, conf,
                   std::move(evidence), {"provider_cargo", "generated_build"});
}

std::optional<ReclaimProviderHit> classifyCMake(const ReclaimProbeInput& in)
{
    if (in.kind != ItemKind::Directory) {
        return std::nullopt;
    }
    const bool cache =
        containsName(in.childNames, in.childCount, L"CMakeCache.txt");
    const bool files =
        containsName(in.childNames, in.childCount, L"CMakeFiles");
    const bool ninja =
        containsName(in.childNames, in.childCount, L"build.ninja");
    if (cache && files) {
        std::vector<std::string> evidence{"CMakeCache.txt", "CMakeFiles"};
        if (ninja) {
            evidence.emplace_back("build.ninja");
        }
        return makeHit("cmake", "cmake", "clean_generated_build_artifacts",
                       ReclaimConsequence::RebuildRequired,
                       ReclaimDisruption::Moderate,
                       ReclaimActionability::ActionableWithoutContentJudgment,
                       ReclaimConfidence::Strong, std::move(evidence),
                       {"provider_cmake", "generated_build"});
    }
    if (cache || files) {
        return makeHit("cmake", "cmake", "clean_generated_build_artifacts",
                       ReclaimConsequence::RebuildRequired,
                       ReclaimDisruption::Moderate,
                       ReclaimActionability::ActionableWithoutContentJudgment,
                       ReclaimConfidence::Heuristic,
                       {cache ? "CMakeCache.txt" : "CMakeFiles"},
                       {"provider_cmake", "partial_cmake_markers"});
    }
    return std::nullopt;
}

std::optional<ReclaimProviderHit> classifyDotNet(const ReclaimProbeInput& in)
{
    if (in.kind != ItemKind::Directory) {
        return std::nullopt;
    }
    const bool binOrObj =
        equalsIgnoreCase(in.name, L"bin") || equalsIgnoreCase(in.name, L"obj");
    if (!binOrObj) {
        return std::nullopt;
    }
    const bool project =
        containsSuffix(in.siblingNames, in.siblingCount, L".csproj") ||
        containsSuffix(in.siblingNames, in.siblingCount, L".fsproj") ||
        containsSuffix(in.siblingNames, in.siblingCount, L".vbproj") ||
        containsSuffix(in.siblingNames, in.siblingCount, L".sln");
    if (!project) {
        return std::nullopt;
    }
    return makeHit("dotnet", "dotnet", "clean_generated_build_artifacts",
                   ReclaimConsequence::RebuildRequired, ReclaimDisruption::Moderate,
                   ReclaimActionability::ActionableWithoutContentJudgment,
                   ReclaimConfidence::Strong,
                   {equalsIgnoreCase(in.name, L"bin") ? "bin" : "obj",
                    "dotnet_project"},
                   {"provider_dotnet", "generated_build"});
}

std::optional<ReclaimProviderHit> classifyNuGet(const ReclaimProbeInput& in,
                                                const ReclaimProviderContext& ctx)
{
    if (in.kind != ItemKind::Directory) {
        return std::nullopt;
    }
    const bool official =
        underKnown(in.path, ctx.nugetGlobalPackages) ||
        underKnown(in.path, ctx.nugetHttpCache) ||
        (equalsIgnoreCase(in.name, L"packages") &&
         (pathHasComponent(in.path, L".nuget") ||
          containsName(in.siblingNames, in.siblingCount, L".nuget"))) ||
        equalsIgnoreCase(in.name, L".nuget") ||
        (equalsIgnoreCase(in.name, L"NuGet") &&
         pathIsUnderFolder(in.path, localAppData()));
    if (!official && in.classification.category != StorageCategory::PackageCache) {
        return std::nullopt;
    }
    if (!official && in.classification.ecosystem != "nuget") {
        return std::nullopt;
    }
    std::vector<std::string> evidence;
    if (underKnown(in.path, ctx.nugetGlobalPackages)) {
        evidence.emplace_back("nuget_global_packages");
    }
    if (underKnown(in.path, ctx.nugetHttpCache)) {
        evidence.emplace_back("nuget_http_cache");
    }
    if (equalsIgnoreCase(in.name, L"packages") ||
        equalsIgnoreCase(in.name, L".nuget") ||
        equalsIgnoreCase(in.name, L"NuGet")) {
        evidence.emplace_back("nuget_cache_directory");
    }
    if (evidence.empty()) {
        return std::nullopt;
    }
    const bool http = underKnown(in.path, ctx.nugetHttpCache);
    return makeHit("nuget", "nuget",
                   http ? "purge_http_cache" : "purge_package_cache",
                   ReclaimConsequence::RedownloadRequired, ReclaimDisruption::Low,
                   ReclaimActionability::ActionableWithoutContentJudgment,
                   official ? ReclaimConfidence::Strong
                            : ReclaimConfidence::Heuristic,
                   std::move(evidence), {"provider_nuget", "package_cache"});
}

std::optional<ReclaimProviderHit> classifyNpm(const ReclaimProbeInput& in,
                                              const ReclaimProviderContext& ctx)
{
    if (in.kind != ItemKind::Directory) {
        return std::nullopt;
    }
    if (underKnown(in.path, ctx.npmCache) ||
        (pathHasComponent(in.path, L"npm-cache") &&
         pathIsUnderFolder(in.path, localAppData()))) {
        return makeHit("npm", "node", "purge_package_cache",
                       ReclaimConsequence::RedownloadRequired,
                       ReclaimDisruption::Low,
                       ReclaimActionability::ActionableWithoutContentJudgment,
                       ReclaimConfidence::Strong, {"npm_cache"},
                       {"provider_npm", "package_cache"});
    }
    if (!equalsIgnoreCase(in.name, L"node_modules")) {
        return std::nullopt;
    }
    const bool packageJson =
        containsName(in.siblingNames, in.siblingCount, L"package.json");
    if (!packageJson) {
        return std::nullopt;
    }
    return makeHit("npm", "node", "reinstall_project_dependencies",
                   ReclaimConsequence::DependencyReinstallRequired,
                   ReclaimDisruption::Higher,
                   ReclaimActionability::ActionableWithoutContentJudgment,
                   ReclaimConfidence::Strong,
                   {"package.json", "node_modules"},
                   {"provider_npm", "developer_dependency"});
}

std::optional<ReclaimProviderHit> classifyPip(const ReclaimProbeInput& in,
                                              const ReclaimProviderContext& ctx)
{
    if (in.kind != ItemKind::Directory) {
        return std::nullopt;
    }
    if (underKnown(in.path, ctx.pipCache) ||
        equalsIgnoreCase(in.name, L"pip-cache") ||
        equalsIgnoreCase(in.name, L"pip_cache") ||
        equalsIgnoreCase(in.name, L"pipcache") ||
        (equalsIgnoreCase(in.name, L"Cache") &&
         pathHasComponent(in.path, L"pip") &&
         pathIsUnderFolder(in.path, localAppData()))) {
        return makeHit("pip", "python", "purge_package_cache",
                       ReclaimConsequence::RedownloadRequired,
                       ReclaimDisruption::Low,
                       ReclaimActionability::ActionableWithoutContentJudgment,
                       ReclaimConfidence::Strong, {"pip_cache"},
                       {"provider_pip", "package_cache"});
    }
    const bool venvMarker =
        containsName(in.childNames, in.childCount, L"pyvenv.cfg");
    const bool venvName =
        equalsIgnoreCase(in.name, L".venv") || equalsIgnoreCase(in.name, L"venv");
    const bool project =
        containsName(in.siblingNames, in.siblingCount, L"pyproject.toml") ||
        containsName(in.siblingNames, in.siblingCount, L"requirements.txt") ||
        containsName(in.siblingNames, in.siblingCount, L"Pipfile") ||
        containsName(in.siblingNames, in.siblingCount, L"setup.py") ||
        containsName(in.siblingNames, in.siblingCount, L"setup.cfg");
    if (venvMarker || (venvName && project)) {
        std::vector<std::string> evidence;
        if (venvMarker) {
            evidence.emplace_back("pyvenv.cfg");
        }
        if (project) {
            evidence.emplace_back("python_project");
        }
        return makeHit("pip", "python", "recreate_virtual_environment",
                       ReclaimConsequence::DependencyReinstallRequired,
                       ReclaimDisruption::Higher,
                       ReclaimActionability::ActionableWithoutContentJudgment,
                       venvMarker ? ReclaimConfidence::Strong
                                  : ReclaimConfidence::Heuristic,
                       std::move(evidence),
                       {"provider_pip", "virtual_environment"});
    }
    return std::nullopt;
}

}  // namespace

bool isProtectedFromReclaim(LocationSafety safety) noexcept
{
    return safety == LocationSafety::Protected;
}

bool isReviewOnlyClassification(StorageCategory category, std::wstring_view path,
                                std::wstring_view name)
{
    if (category == StorageCategory::UserData ||
        category == StorageCategory::Archive ||
        category == StorageCategory::DownloadedAiModel ||
        category == StorageCategory::SystemData ||
        category == StorageCategory::ApplicationData ||
        category == StorageCategory::Unknown) {
        return true;
    }
    const auto extPos = name.find_last_of(L'.');
    if (extPos != std::wstring_view::npos) {
        const auto ext = name.substr(extPos + 1);
        if (equalsIgnoreCase(ext, L"vhd") || equalsIgnoreCase(ext, L"vhdx") ||
            equalsIgnoreCase(ext, L"vmdk") || equalsIgnoreCase(ext, L"iso") ||
            equalsIgnoreCase(ext, L"img") || equalsIgnoreCase(ext, L"qcow2")) {
            return true;
        }
    }
    (void)path;
    return false;
}

ReclaimProviderContext probeProviderLocations(std::stop_token stop)
{
    ReclaimProviderContext ctx;
    const std::wstring profile = userProfile();
    const std::wstring local = localAppData();
    if (!profile.empty()) {
        ctx.nugetGlobalPackages = profile + L"\\.nuget\\packages";
    }
    if (!local.empty()) {
        ctx.nugetHttpCache = local + L"\\NuGet\\v3-cache";
        ctx.npmCache = local + L"\\npm-cache";
        ctx.pipCache = local + L"\\pip\\Cache";
    }

    if (stop.stop_requested()) {
        ctx.probeFailed = true;
        ctx.probeDetail = "cancelled";
        return ctx;
    }

    SafeProcessRequest req;
    req.executableName = L"dotnet.exe";
    req.arguments = {L"nuget", L"locals", L"global-packages", L"--list"};
    req.timeoutMs = 4000;
    const auto run = runAllowlistedProcess(req, stop);
    ctx.nugetProbed = true;
    if (run.status == SafeProcessStatus::Completed && run.exitCode == 0) {
        if (const std::wstring parsed = parseDotnetLocalsPath(run.stdoutUtf8);
            !parsed.empty()) {
            ctx.nugetGlobalPackages = parsed;
        }
    } else if (run.status == SafeProcessStatus::NotFound) {
        ctx.probeDetail = "dotnet_not_found";
    } else if (run.status == SafeProcessStatus::TimedOut) {
        ctx.probeFailed = true;
        ctx.probeDetail = "dotnet_timeout";
    } else if (run.status == SafeProcessStatus::Cancelled) {
        ctx.probeFailed = true;
        ctx.probeDetail = "cancelled";
    } else if (run.status != SafeProcessStatus::Completed) {
        ctx.probeFailed = true;
        ctx.probeDetail = run.detail.empty() ? "dotnet_probe_failed" : run.detail;
    }
    return ctx;
}

std::optional<ReclaimProviderHit> classifyReclaimOwnership(
    const ReclaimProbeInput& input, const ReclaimProviderContext& context)
{
    if (isProtectedFromReclaim(input.safety)) {
        return std::nullopt;
    }
    if (auto hit = classifyCargo(input)) {
        return hit;
    }
    if (auto hit = classifyCMake(input)) {
        return hit;
    }
    if (auto hit = classifyDotNet(input)) {
        return hit;
    }
    if (auto hit = classifyNuGet(input, context)) {
        return hit;
    }
    if (auto hit = classifyNpm(input, context)) {
        return hit;
    }
    if (auto hit = classifyPip(input, context)) {
        return hit;
    }
    return std::nullopt;
}

}  // namespace spacelens
