#include "TestRunner.hpp"

#include "core/Maintenance.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/RecycleAdapter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace spacelens;
namespace fs = std::filesystem;

namespace {

fs::path makeTempFile(const std::string& tag)
{
    const auto dir = fs::temp_directory_path() / "spacelens_recycle_spike";
    fs::create_directories(dir);
    const auto name =
        tag + "-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".bin";
    const auto path = dir / name;
    std::ofstream out(path, std::ios::binary);
    const char payload[] = "spacelens-recycle-v1";
    out.write(payload, sizeof(payload) - 1);
    out.close();
    return path;
}

MaintenancePlanItem plannedFile(const fs::path& path, const CleanupIdentity& id)
{
    MaintenancePlanItem item;
    item.reviewId = 1;
    item.path = path.wstring();
    item.expectedIdentity = id;
    item.logicalSize = static_cast<ByteSize>(fs::file_size(path));
    item.eligible = true;
    item.safety = LocationSafety::Ordinary;
    return item;
}

}  // namespace

SPACELENS_TEST(RecycleAdapter_unc_path_is_unavailable)
{
    WindowsRecycleAdapter adapter;
    std::string detail;
    SPACELENS_REQUIRE(!adapter.volumeCanRecycle(L"\\\\server\\share\\a.bin", 10,
                                                &detail));
    SPACELENS_REQUIRE(!detail.empty());
}

SPACELENS_TEST(RecycleAdapter_recycles_temp_file_with_bin_evidence)
{
    const auto path = makeTempFile("ok");
    SPACELENS_REQUIRE(fs::exists(path));

    WindowsCleanupMetadataReader reader;
    const auto probe = reader.read(path.wstring());
    SPACELENS_REQUIRE_EQ(probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(isStrongIdentity(probe.objectEvidence.identity));
    SPACELENS_REQUIRE(!probe.isReparse);

    WindowsRecycleAdapter adapter;
    std::string recycleDetail;
    SPACELENS_REQUIRE(adapter.volumeCanRecycle(
        path.wstring(), probe.objectEvidence.logicalSize, &recycleDetail));

    const auto receipt = adapter.recycle(
        plannedFile(path, probe.objectEvidence.identity));

    if (receipt.result == MaintenanceItemResult::UnexpectedPermanentRemoval) {
        throw spacelens::test::Failure(
            "DESIGN_BLOCKED: IFileOperation removed the file without Recycle "
            "Bin item evidence (psiNewlyCreated was null).");
    }

    SPACELENS_REQUIRE_EQ(receipt.result, MaintenanceItemResult::Recycled);
    SPACELENS_REQUIRE(!receipt.recycleParsingName.empty());
    SPACELENS_REQUIRE(!fs::exists(path));
}

SPACELENS_TEST(RecycleAdapter_missing_file_is_not_permanent_success)
{
    const auto path = fs::temp_directory_path() / "spacelens_recycle_spike" /
                      "missing-never-created.bin";
    std::error_code ec;
    fs::remove(path, ec);

    WindowsRecycleAdapter adapter;
    MaintenancePlanItem item;
    item.path = path.wstring();
    item.logicalSize = 1;
    item.eligible = true;
    const auto receipt = adapter.recycle(item);
    SPACELENS_REQUIRE(receipt.result != MaintenanceItemResult::Recycled);
    SPACELENS_REQUIRE(receipt.result !=
                      MaintenanceItemResult::UnexpectedPermanentRemoval);
}

SPACELENS_TEST(RecycleAdapter_directory_is_not_used_by_core_eligibility)
{
    const auto dir = fs::temp_directory_path() / "spacelens_recycle_spike" /
                     "dir-not-recycled";
    fs::create_directories(dir);
    WindowsCleanupMetadataReader reader;
    const auto probe = reader.read(dir.wstring());
    SPACELENS_REQUIRE_EQ(probe.outcome, CleanupMetadataProbeOutcome::Present);
    SPACELENS_REQUIRE(probe.objectEvidence.kind != ItemKind::File ||
                      probe.objectEvidence.kind == ItemKind::Directory ||
                      probe.objectEvidence.kind == ItemKind::ReparseDirectory);

    CleanupCandidate candidate;
    candidate.path = dir.wstring();
    candidate.kind = ItemKind::Directory;
    candidate.objectEvidence = probe.objectEvidence;
    candidate.objectEvidence.kind = ItemKind::Directory;
    const auto item = evaluateMaintenanceEligibility(candidate, reader);
    SPACELENS_REQUIRE(!item.eligible);
    SPACELENS_REQUIRE_EQ(item.blockReason, MaintenanceBlockReason::UnsupportedType);
    SPACELENS_REQUIRE(fs::exists(dir));
}
