#include "TestRunner.hpp"

#include "core/ReclaimPlan.hpp"
#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/PhysicalAccounting.hpp"
#include "core/index/Sqlite.hpp"
#include "platform/windows/FileIdentity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winioctl.h>

#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

using namespace spacelens;

namespace {

class IsolatedDataRoot {
public:
    IsolatedDataRoot()
    {
        wchar_t previous[32768]{};
        const DWORD n =
            ::GetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", previous, 32768);
        if (n > 0 && n < 32768) {
            m_hadPrevious = true;
            m_previous.assign(previous, n);
        }
        namespace fs = std::filesystem;
        m_dir = fs::temp_directory_path() / "spacelens_reclaim_appdata" /
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(m_dir);
        ::SetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", m_dir.wstring().c_str());
    }

    ~IsolatedDataRoot()
    {
        if (m_hadPrevious) {
            ::SetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", m_previous.c_str());
        } else {
            ::SetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", nullptr);
        }
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    IsolatedDataRoot(const IsolatedDataRoot&) = delete;
    IsolatedDataRoot& operator=(const IsolatedDataRoot&) = delete;

private:
    std::filesystem::path m_dir;
    std::wstring m_previous;
    bool m_hadPrevious = false;
};

std::filesystem::path makeTempRoot(const char* tag)
{
    namespace fs = std::filesystem;
    // Do not use %TEMP%: known-temp-folder classification would mark every
    // fixture TemporaryData and therefore actionable.
    wchar_t module[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
    fs::path base = (n > 0 && n < MAX_PATH)
                        ? fs::path(module).parent_path()
                        : fs::current_path();
    auto dir = base / "reclaim_fix" / tag /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

void writeSizedFile(const std::filesystem::path& path, ByteSize bytes)
{
    std::filesystem::create_directories(path.parent_path());
    const HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw spacelens::test::Failure("CreateFileW failed for sized file");
    }
    LARGE_INTEGER size;
    size.QuadPart = static_cast<LONGLONG>(bytes);
    if (!::SetFilePointerEx(h, size, nullptr, FILE_BEGIN) || !::SetEndOfFile(h)) {
        ::CloseHandle(h);
        throw spacelens::test::Failure("SetEndOfFile failed");
    }
    ::CloseHandle(h);
}

void writeText(const std::filesystem::path& path, const char* text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool makeSparseLogical(const std::filesystem::path& path, ByteSize logical)
{
    std::filesystem::create_directories(path.parent_path());
    const HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytes = 0;
    if (!::DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes,
                           nullptr)) {
        ::CloseHandle(h);
        return false;
    }
    FILE_ZERO_DATA_INFORMATION zero{};
    zero.FileOffset.QuadPart = 0;
    zero.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(logical);
    if (!::DeviceIoControl(h, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero), nullptr, 0,
                           &bytes, nullptr)) {
        ::CloseHandle(h);
        return false;
    }
    LARGE_INTEGER size;
    size.QuadPart = static_cast<LONGLONG>(logical);
    const bool ok = ::SetFilePointerEx(h, size, nullptr, FILE_BEGIN) &&
                    ::SetEndOfFile(h);
    ::CloseHandle(h);
    return ok;
}

bool pathEndsWithIgnoreCase(std::wstring_view path, std::wstring_view needle)
{
    if (needle.empty() || path.size() < needle.size()) {
        return false;
    }
    const auto tail = path.substr(path.size() - needle.size());
    for (std::size_t i = 0; i < needle.size(); ++i) {
        if (std::towlower(tail[i]) != std::towlower(needle[i])) {
            return false;
        }
    }
    return true;
}

const ReclaimCandidateEvidence* findPathContains(
    const std::vector<ReclaimCandidateEvidence>& items, std::wstring_view needle)
{
    const ReclaimCandidateEvidence* fallback = nullptr;
    for (const auto& item : items) {
        if (pathEndsWithIgnoreCase(item.path, needle)) {
            return &item;
        }
        if (fallback == nullptr && item.path.find(needle) != std::wstring::npos) {
            fallback = &item;
        }
    }
    return fallback;
}

bool jsonForbidden(const std::string& json)
{
    return json.find("safe_to_delete") != std::string::npos ||
           json.find("should_delete") != std::string::npos ||
           json.find("deletion_recommended") != std::string::npos ||
           json.find("recommended_delete") != std::string::npos;
}

}  // namespace

SPACELENS_TEST(ReclaimIntel_json_contract_and_live_providers)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("live");
    writeText(root / "proj" / "Cargo.toml", "[package]\nname=\"x\"\n");
    writeText(root / "proj" / "target" / "CACHEDIR.TAG", "Signature: 8a477f597d28d172");
    writeSizedFile(root / "proj" / "target" / "release" / "app.exe", 2ULL << 20);
    writeText(root / "proj" / "CMakeLists.txt", "project(x)\n");
    writeText(root / "cmake-build" / "CMakeCache.txt", "CMAKE_COMMAND:INTERNAL=x\n");
    std::filesystem::create_directories(root / "cmake-build" / "CMakeFiles");
    writeSizedFile(root / "cmake-build" / "CMakeFiles" / "a.obj", 2ULL << 20);
    writeText(root / "dotnet" / "App.csproj", "<Project></Project>\n");
    writeSizedFile(root / "dotnet" / "bin" / "App.dll", 2ULL << 20);
    writeSizedFile(root / "dotnet" / "obj" / "App.obj", 2ULL << 20);
    writeText(root / "node" / "package.json", "{}\n");
    writeSizedFile(root / "node" / "node_modules" / "leftpad" / "index.js",
                   2ULL << 20);
    writeSizedFile(root / "orphan" / "node_modules" / "x" / "index.js", 2ULL << 20);
    writeSizedFile(root / ".nuget" / "packages" / "foo" / "1.0.0" / "foo.nupkg",
                   3ULL << 20);
    writeSizedFile(root / "pip-cache" / "wheel.dat", 2ULL << 20);
    writeSizedFile(root / "videos" / "holiday.mp4", 4ULL << 20);
    writeSizedFile(root / "fake-target" / "target" / "out.bin", 2ULL << 20);
    writeSizedFile(root / "fake-build" / "build" / "out.bin", 2ULL << 20);

    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::LiveScan;
    req.limit = 50;
    req.targetFreeBytes = 5ULL << 20;
    const auto report = buildReclaimPlan(req);
    if (!report.ok) {
        throw spacelens::test::Failure(std::string("live plan failed: ") +
                                       report.error);
    }
    const std::string json = report.toJson();
    SPACELENS_REQUIRE(json.find("\"command\":\"reclaim-plan\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"planning_only\":true") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"execution_supported\":false") !=
                      std::string::npos);
    SPACELENS_REQUIRE(json.find("\"filesystem_mutation\":false") !=
                      std::string::npos);
    SPACELENS_REQUIRE(!jsonForbidden(json));

    const auto* cargo = findPathContains(report.actionable, L"\\proj\\target");
    SPACELENS_REQUIRE(cargo != nullptr);
    SPACELENS_REQUIRE(cargo->ownership.provider == "cargo");
    SPACELENS_REQUIRE(cargo->actionability ==
                      ReclaimActionability::ActionableWithoutContentJudgment);

    const auto* cmake = findPathContains(report.actionable, L"cmake-build");
    SPACELENS_REQUIRE(cmake != nullptr);
    SPACELENS_REQUIRE(cmake->ownership.provider == "cmake");

    const auto* bin = findPathContains(report.actionable, L"\\dotnet\\bin");
    SPACELENS_REQUIRE(bin != nullptr);
    SPACELENS_REQUIRE(bin->ownership.provider == "dotnet");

    const auto* node = findPathContains(report.actionable, L"\\node\\node_modules");
    SPACELENS_REQUIRE(node != nullptr);
    SPACELENS_REQUIRE(node->ownership.provider == "npm");

    const auto* nuget = findPathContains(report.actionable, L".nuget");
    SPACELENS_REQUIRE(nuget != nullptr);
    SPACELENS_REQUIRE(nuget->ownership.provider == "nuget");

    const auto* pip = findPathContains(report.actionable, L"pip-cache");
    SPACELENS_REQUIRE(pip != nullptr);
    SPACELENS_REQUIRE(pip->ownership.provider == "pip");

    SPACELENS_REQUIRE(findPathContains(report.actionable, L"holiday.mp4") ==
                      nullptr);
    SPACELENS_REQUIRE(findPathContains(report.actionable, L"\\videos") == nullptr);
    const auto* video = findPathContains(report.reviewOnly, L"holiday.mp4");
    const auto* videosDir = findPathContains(report.reviewOnly, L"\\videos");
    SPACELENS_REQUIRE(video != nullptr || videosDir != nullptr);

    SPACELENS_REQUIRE(findPathContains(report.actionable, L"fake-target") ==
                      nullptr);
    SPACELENS_REQUIRE(findPathContains(report.actionable, L"fake-build") ==
                      nullptr);
    const auto* orphan = findPathContains(report.actionable, L"\\orphan\\node_modules");
    SPACELENS_REQUIRE(orphan == nullptr);

    SPACELENS_REQUIRE(!report.selected.empty());
    for (const auto& sel : report.selected) {
        SPACELENS_REQUIRE(sel.actionability ==
                          ReclaimActionability::ActionableWithoutContentJudgment);
        SPACELENS_REQUIRE(sel.path.find(L"holiday.mp4") == std::wstring::npos);
        SPACELENS_REQUIRE(sel.path.find(L"\\videos") == std::wstring::npos ||
                          sel.ownership.provider.size() > 0);
    }

    // Nested cargo target/release must not also be selected/actionable with parent.
    const auto* release = findPathContains(report.actionable, L"target\\release");
    if (cargo != nullptr && release != nullptr) {
        throw spacelens::test::Failure(
            "parent and child cargo paths both listed as actionable");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_sparse_host_reclaim_is_allocated_not_logical)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("sparse");
    const ByteSize logical = 42ULL << 20;
    const auto vm = root / "machine.vhdx";
    if (!makeSparseLogical(vm, logical)) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        return;
    }
    const auto ident = queryFileIdentity(vm.wstring());
    SPACELENS_REQUIRE(ident.has_value());
    SPACELENS_REQUIRE(ident->sizeBytes == logical);
    if (ident->allocationKnown && ident->allocatedBytes.has_value()) {
        SPACELENS_REQUIRE(*ident->allocatedBytes < logical);
    }

    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::LiveScan;
    req.limit = 20;
    const auto report = buildReclaimPlan(req);
    SPACELENS_REQUIRE(report.ok);
    const auto* item = findPathContains(report.reviewOnly, L"machine.vhdx");
    if (item == nullptr) {
        item = findPathContains(report.actionable, L"machine.vhdx");
    }
    SPACELENS_REQUIRE(item != nullptr);
    SPACELENS_REQUIRE(item->size.logicalBytes == logical);
    if (item->hostReclaimBytes.has_value()) {
        SPACELENS_REQUIRE(*item->hostReclaimBytes < logical);
        SPACELENS_REQUIRE(*item->hostReclaimBytes != logical);
    }
    SPACELENS_REQUIRE(item->actionability !=
                      ReclaimActionability::ActionableWithoutContentJudgment);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_hardlink_exact_and_incomplete)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("hardlink");
    const auto a = root / "inside" / "a.bin";
    const auto b = root / "inside" / "b.bin";
    writeSizedFile(a, 2ULL << 20);
    if (!::CreateHardLinkW(b.wstring().c_str(), a.wstring().c_str(), nullptr)) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        return;
    }

    ReclaimPlanRequest req;
    req.root = (root / "inside").wstring();
    req.source = ReclaimPlanSource::LiveScan;
    const auto complete = buildReclaimPlan(req);
    SPACELENS_REQUIRE(complete.ok);

    const auto outside = root / "outside" / "c.bin";
    std::filesystem::create_directories(outside.parent_path());
    if (!::CreateHardLinkW(outside.wstring().c_str(), a.wstring().c_str(),
                           nullptr)) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        return;
    }
    const auto incomplete = buildReclaimPlan(req);
    SPACELENS_REQUIRE(incomplete.ok);
    bool sawIncomplete = incomplete.overallCoverage == HardLinkCoverage::Incomplete;
    for (const auto* list : {&incomplete.actionable, &incomplete.reviewOnly}) {
        for (const auto& item : *list) {
            if (item.size.hardLinkCoverage == HardLinkCoverage::Incomplete) {
                sawIncomplete = true;
                SPACELENS_REQUIRE(!item.hostReclaimBytes.has_value() ||
                                  item.basis ==
                                      ReclaimBasis::IncompleteHardlinkCoverage);
            }
        }
    }
    SPACELENS_REQUIRE(sawIncomplete);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_index_physical_and_fail_closed)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("index");
    writeText(root / "Cargo.toml", "[package]\nname=\"x\"\n");
    writeText(root / "target" / "CACHEDIR.TAG", "Signature: 8a477f597d28d172");
    writeSizedFile(root / "target" / "lib.rlib", 2ULL << 20);

    const auto built = buildIndexForRoot(root.wstring());
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);
    SPACELENS_REQUIRE_EQ(built.root.schemaVersion, kIndexSchemaVersion);

    {
        auto store = IndexStore::openRead(locateIndex(root.wstring()));
        SPACELENS_REQUIRE(indexHasPhysicalAccounting(store.db()));
    }

    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::PersistentIndex;
    const auto indexed = buildReclaimPlan(req);
    if (!indexed.ok) {
        throw spacelens::test::Failure(std::string("indexed plan failed: ") +
                                       indexed.error);
    }
    SPACELENS_REQUIRE(indexed.sourceUsed == ReclaimPlanSource::PersistentIndex);
    SPACELENS_REQUIRE(indexed.physicalAccounting);
    SPACELENS_REQUIRE(findPathContains(indexed.actionable, L"\\target") != nullptr);
    SPACELENS_REQUIRE(!jsonForbidden(indexed.toJson()));

    {
        auto store = IndexStore::openReadWrite(locateIndex(root.wstring()));
        writePhysicalAccountingFlag(store.db(), false);
    }
    const auto denied = buildReclaimPlan(req);
    SPACELENS_REQUIRE(!denied.ok);
    SPACELENS_REQUIRE(denied.error == "physical_accounting_unavailable");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_auto_falls_back_without_writing_index)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("auto");
    writeText(root / "Cargo.toml", "[package]\nname=\"x\"\n");
    writeText(root / "target" / "CACHEDIR.TAG", "Signature: 8a477f597d28d172");
    writeSizedFile(root / "target" / "x.bin", 2ULL << 20);

    SPACELENS_REQUIRE(!indexDatabaseExists(locateIndex(root.wstring())));
    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::Auto;
    const auto report = buildReclaimPlan(req);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(report.sourceUsed == ReclaimPlanSource::LiveScan);
    SPACELENS_REQUIRE(!indexDatabaseExists(locateIndex(root.wstring())));

    req.source = ReclaimPlanSource::PersistentIndex;
    const auto missing = buildReclaimPlan(req);
    SPACELENS_REQUIRE(!missing.ok);
    SPACELENS_REQUIRE(missing.error == "index_not_found");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_cancel_and_inaccessible)
{
    ReclaimPlanRequest bad;
    bad.root = L"C:\\SpaceLensDefinitelyMissingReclaimRoot\\nope";
    bad.source = ReclaimPlanSource::LiveScan;
    const auto missing = buildReclaimPlan(bad);
    SPACELENS_REQUIRE(!missing.ok);
    SPACELENS_REQUIRE(missing.error == "inaccessible_root");

    IsolatedDataRoot data;
    const auto root = makeTempRoot("cancel");
    writeSizedFile(root / "a.bin", 2ULL << 20);
    std::stop_source src;
    src.request_stop();
    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::LiveScan;
    const auto cancelled = buildReclaimPlan(req, src.get_token());
    SPACELENS_REQUIRE(!cancelled.ok);
    SPACELENS_REQUIRE(cancelled.error == "cancelled");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_target_free_never_picks_review_only)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("targetfree");
    writeText(root / "Cargo.toml", "[package]\nname=\"x\"\n");
    writeText(root / "target" / "CACHEDIR.TAG", "Signature: 8a477f597d28d172");
    writeSizedFile(root / "target" / "big.rlib", 8ULL << 20);
    writeSizedFile(root / "videos" / "clip.mp4", 20ULL << 20);

    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::LiveScan;
    req.targetFreeBytes = 4ULL << 20;
    req.limit = 5;
    const auto report = buildReclaimPlan(req);
    SPACELENS_REQUIRE(report.ok);
    SPACELENS_REQUIRE(!report.selected.empty());
    for (const auto& sel : report.selected) {
        SPACELENS_REQUIRE(sel.actionability ==
                          ReclaimActionability::ActionableWithoutContentJudgment);
        SPACELENS_REQUIRE(sel.path.find(L"clip.mp4") == std::wstring::npos);
        SPACELENS_REQUIRE(sel.path.find(L"\\videos") == std::wstring::npos);
    }
    SPACELENS_REQUIRE(findPathContains(report.actionable, L"\\videos") == nullptr);
    SPACELENS_REQUIRE(findPathContains(report.reviewOnly, L"clip.mp4") != nullptr ||
                      findPathContains(report.reviewOnly, L"\\videos") != nullptr);
    const auto json = report.toJson();
    SPACELENS_REQUIRE(json.find("\"target_met\"") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

SPACELENS_TEST(ReclaimIntel_index_revalidation_drops_missing_and_mismatch)
{
    IsolatedDataRoot data;
    const auto root = makeTempRoot("reval");
    writeText(root / "Cargo.toml", "[package]\nname=\"x\"\n");
    writeText(root / "target" / "CACHEDIR.TAG", "Signature: 8a477f597d28d172");
    writeSizedFile(root / "target" / "lib.rlib", 2ULL << 20);
    writeText(root / "keep" / "Cargo.toml", "[package]\nname=\"y\"\n");
    writeText(root / "keep" / "target" / "CACHEDIR.TAG",
              "Signature: 8a477f597d28d172");
    writeSizedFile(root / "keep" / "target" / "lib.rlib", 3ULL << 20);

    const auto built = buildIndexForRoot(root.wstring());
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    std::error_code ec;
    std::filesystem::remove_all(root / "target", ec);

    ReclaimPlanRequest req;
    req.root = root.wstring();
    req.source = ReclaimPlanSource::PersistentIndex;
    req.targetFreeBytes = 1ULL << 20;
    req.limit = 10;
    const auto missing = buildReclaimPlan(req);
    SPACELENS_REQUIRE(missing.ok);
    SPACELENS_REQUIRE(findPathContains(missing.selected, L"\\keep\\target") !=
                      nullptr);
    for (const auto& sel : missing.selected) {
        SPACELENS_REQUIRE(sel.actionability ==
                          ReclaimActionability::ActionableWithoutContentJudgment);
        SPACELENS_REQUIRE(sel.hostReclaimBytes.has_value());
        if (pathEndsWithIgnoreCase(sel.path, L"\\target") &&
            sel.path.find(L"\\keep\\target") == std::wstring::npos) {
            throw spacelens::test::Failure(
                "deleted cargo target stayed selected");
        }
    }
    for (const auto& item : missing.actionable) {
        if (pathEndsWithIgnoreCase(item.path, L"\\target") &&
            item.path.find(L"\\keep\\target") == std::wstring::npos) {
            throw spacelens::test::Failure(
                "deleted cargo target stayed actionable");
        }
        SPACELENS_REQUIRE(item.confidence != ReclaimConfidence::Verified ||
                          !item.snapshotBased || item.kind != ItemKind::Directory);
    }
    bool sawMissing = false;
    for (const auto& item : missing.reviewOnly) {
        if (pathEndsWithIgnoreCase(item.path, L"\\target") &&
            item.path.find(L"\\keep\\target") == std::wstring::npos) {
            sawMissing = true;
            SPACELENS_REQUIRE(!item.hostReclaimBytes.has_value());
            SPACELENS_REQUIRE(item.liveRevalidated == false);
        }
    }
    SPACELENS_REQUIRE(sawMissing);

    std::filesystem::create_directories(root / "target");
    writeText(root / "target" / "CACHEDIR.TAG", "Signature: 8a477f597d28d172");
    writeSizedFile(root / "target" / "lib.rlib", 2ULL << 20);
    const auto recreated = buildReclaimPlan(req);
    SPACELENS_REQUIRE(recreated.ok);
    for (const auto& item : recreated.selected) {
        if (pathEndsWithIgnoreCase(item.path, L"\\target") &&
            item.path.find(L"\\keep\\target") == std::wstring::npos) {
            throw spacelens::test::Failure(
                "recreated cargo target with new identity stayed selected");
        }
    }
    for (const auto& item : recreated.actionable) {
        if (pathEndsWithIgnoreCase(item.path, L"\\target") &&
            item.path.find(L"\\keep\\target") == std::wstring::npos) {
            throw spacelens::test::Failure(
                "recreated cargo target with new identity stayed actionable");
        }
    }
    std::filesystem::remove_all(root, ec);
}
