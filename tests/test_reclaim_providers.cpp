#include "TestRunner.hpp"

#include "core/Classification.hpp"
#include "core/ReclaimProvider.hpp"

#include <string>
#include <vector>

using namespace spacelens;

namespace {

ReclaimProbeInput dirInput(std::wstring path, std::wstring name,
                           LocationSafety safety = LocationSafety::Ordinary)
{
    ReclaimProbeInput in;
    in.path = std::move(path);
    in.name = std::move(name);
    in.kind = ItemKind::Directory;
    in.safety = safety;
    return in;
}

}  // namespace

SPACELENS_TEST(Provider_cargo_requires_marker)
{
    ReclaimProviderContext ctx;
    const std::wstring cargoToml = L"Cargo.toml";
    auto with = dirInput(L"D:\\proj\\target", L"target");
    with.siblingNames = &cargoToml;
    with.siblingCount = 1;
    auto hit = classifyReclaimOwnership(with, ctx);
    SPACELENS_REQUIRE(hit.has_value());
    SPACELENS_REQUIRE(hit->ownership.provider == "cargo");
    SPACELENS_REQUIRE(hit->actionability ==
                      ReclaimActionability::ActionableWithoutContentJudgment);
    SPACELENS_REQUIRE(!hit->action.executionSupported);

    auto fake = dirInput(L"D:\\proj\\target", L"target");
    SPACELENS_REQUIRE(!classifyReclaimOwnership(fake, ctx).has_value());
}

SPACELENS_TEST(Provider_cmake_requires_cache_and_files)
{
    ReclaimProviderContext ctx;
    const std::wstring children[] = {L"CMakeCache.txt", L"CMakeFiles"};
    auto with = dirInput(L"D:\\proj\\build", L"build");
    with.childNames = children;
    with.childCount = 2;
    auto hit = classifyReclaimOwnership(with, ctx);
    SPACELENS_REQUIRE(hit.has_value());
    SPACELENS_REQUIRE(hit->ownership.provider == "cmake");

    auto fake = dirInput(L"D:\\proj\\build", L"build");
    SPACELENS_REQUIRE(!classifyReclaimOwnership(fake, ctx).has_value());
}

SPACELENS_TEST(Provider_dotnet_requires_project_sibling)
{
    ReclaimProviderContext ctx;
    const std::wstring sibling = L"App.csproj";
    auto with = dirInput(L"D:\\proj\\bin", L"bin");
    with.siblingNames = &sibling;
    with.siblingCount = 1;
    auto hit = classifyReclaimOwnership(with, ctx);
    SPACELENS_REQUIRE(hit.has_value());
    SPACELENS_REQUIRE(hit->ownership.provider == "dotnet");

    auto fake = dirInput(L"D:\\proj\\bin", L"bin");
    SPACELENS_REQUIRE(!classifyReclaimOwnership(fake, ctx).has_value());
}

SPACELENS_TEST(Provider_nuget_official_and_dotnuget_packages)
{
    ReclaimProviderContext ctx;
    ctx.nugetGlobalPackages = L"C:\\Users\\me\\.nuget\\packages";
    auto official =
        dirInput(L"C:\\Users\\me\\.nuget\\packages", L"packages");
    auto hit = classifyReclaimOwnership(official, ctx);
    SPACELENS_REQUIRE(hit.has_value());
    SPACELENS_REQUIRE(hit->ownership.provider == "nuget");
    SPACELENS_REQUIRE(hit->disruption == ReclaimDisruption::Low);

    auto nested = dirInput(L"D:\\work\\.nuget\\packages", L"packages");
    SPACELENS_REQUIRE(classifyReclaimOwnership(nested, ctx).has_value());
}

SPACELENS_TEST(Provider_npm_requires_package_json)
{
    ReclaimProviderContext ctx;
    const std::wstring pkg = L"package.json";
    auto with = dirInput(L"D:\\app\\node_modules", L"node_modules");
    with.siblingNames = &pkg;
    with.siblingCount = 1;
    auto hit = classifyReclaimOwnership(with, ctx);
    SPACELENS_REQUIRE(hit.has_value());
    SPACELENS_REQUIRE(hit->ownership.provider == "npm");

    auto orphan = dirInput(L"D:\\app\\node_modules", L"node_modules");
    SPACELENS_REQUIRE(!classifyReclaimOwnership(orphan, ctx).has_value());
}

SPACELENS_TEST(Provider_pip_cache_and_venv)
{
    ReclaimProviderContext ctx;
    auto cache = dirInput(L"D:\\tmp\\pip-cache", L"pip-cache");
    auto cacheHit = classifyReclaimOwnership(cache, ctx);
    SPACELENS_REQUIRE(cacheHit.has_value());
    SPACELENS_REQUIRE(cacheHit->ownership.provider == "pip");

    const std::wstring marker = L"pyvenv.cfg";
    auto venv = dirInput(L"D:\\app\\.venv", L".venv");
    venv.childNames = &marker;
    venv.childCount = 1;
    auto venvHit = classifyReclaimOwnership(venv, ctx);
    SPACELENS_REQUIRE(venvHit.has_value());
    SPACELENS_REQUIRE(venvHit->ownership.provider == "pip");
}

SPACELENS_TEST(Provider_protected_never_authoritative)
{
    ReclaimProviderContext ctx;
    const std::wstring cargo = L"Cargo.toml";
    auto in = dirInput(L"C:\\Windows\\target", L"target", LocationSafety::Protected);
    in.siblingNames = &cargo;
    in.siblingCount = 1;
    SPACELENS_REQUIRE(!classifyReclaimOwnership(in, ctx).has_value());
    SPACELENS_REQUIRE(isProtectedFromReclaim(LocationSafety::Protected));
    SPACELENS_REQUIRE(!isProtectedFromReclaim(LocationSafety::Ordinary));
}

SPACELENS_TEST(Provider_review_only_personal_and_vm)
{
    SPACELENS_REQUIRE(isReviewOnlyClassification(StorageCategory::UserData,
                                                 L"D:\\Videos\\a.mp4", L"a.mp4"));
    SPACELENS_REQUIRE(isReviewOnlyClassification(StorageCategory::Unknown,
                                                 L"D:\\vm\\disk.vhdx", L"disk.vhdx"));
    SPACELENS_REQUIRE(!isReviewOnlyClassification(StorageCategory::BuildArtifact,
                                                  L"D:\\proj\\target", L"target"));
}

SPACELENS_TEST(Provider_action_never_executable)
{
    ReclaimProviderContext ctx;
    const std::wstring cargo = L"Cargo.toml";
    auto in = dirInput(L"D:\\proj\\target", L"target");
    in.siblingNames = &cargo;
    in.siblingCount = 1;
    const auto hit = classifyReclaimOwnership(in, ctx);
    SPACELENS_REQUIRE(hit.has_value());
    SPACELENS_REQUIRE(!hit->action.executionSupported);
    SPACELENS_REQUIRE(hit->action.humanAuthorizationRequired);
}
