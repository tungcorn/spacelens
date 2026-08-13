#include "TestRunner.hpp"

#include "core/CleanupReviewStore.hpp"
#include "core/Maintenance.hpp"
#include "core/OrdinaryLocation.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/index/Sqlite.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace spacelens;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        namespace fs = std::filesystem;
        path = fs::temp_directory_path() / "spacelens_ordinary_location_tests" /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    [[nodiscard]] std::wstring dbPath() const
    {
        return (path / "state.db").wstring();
    }
};

class MapMetadataReader final : public ICleanupMetadataReader {
public:
    std::map<std::wstring, CleanupMetadataProbe> probes;

    CleanupMetadataProbe read(const std::wstring& path) override
    {
        const auto it = probes.find(path);
        if (it == probes.end()) {
            CleanupMetadataProbe missing;
            missing.outcome = CleanupMetadataProbeOutcome::Missing;
            missing.detail = "not in map";
            return missing;
        }
        return it->second;
    }
};

class MapVolumeReader final : public IVolumeIdentityReader {
public:
    std::map<std::wstring, LocationVolumeEvidence> volumes;

    LocationVolumeEvidence read(const std::wstring& path) const override
    {
        const auto it = volumes.find(path);
        if (it == volumes.end()) {
            return {};
        }
        return it->second;
    }
};

CleanupMetadataProbe presentDirectory(bool reparse = false)
{
    CleanupMetadataProbe probe;
    probe.outcome = CleanupMetadataProbeOutcome::Present;
    probe.isReparse = reparse;
    probe.objectEvidence.available = true;
    probe.objectEvidence.kind = ItemKind::Directory;
    probe.objectEvidence.attributes = reparse ? 0x400u : 0x10u;
    return probe;
}

CleanupMetadataProbe presentFile()
{
    CleanupMetadataProbe probe;
    probe.outcome = CleanupMetadataProbeOutcome::Present;
    probe.objectEvidence.available = true;
    probe.objectEvidence.kind = ItemKind::File;
    probe.objectEvidence.logicalSize = 8;
    probe.objectEvidence.attributes = 32;
    return probe;
}

LocationVolumeEvidence volume(std::uint32_t serial,
                              std::wstring guid = L"\\\\?\\Volume{test}\\")
{
    LocationVolumeEvidence out;
    out.available = serial != 0;
    out.serial = serial;
    out.guid = std::move(guid);
    out.rootPath = L"D:\\";
    return out;
}

OrdinaryLocationDeclaration activeDeclaration(std::wstring path,
                                              std::uint64_t id = 1,
                                              std::uint32_t serial = 0xAABBCCDD)
{
    OrdinaryLocationDeclaration row;
    row.id = id;
    row.configuredPath = path;
    row.normalizedPathKey = normalizeOrdinaryLocationPath(path);
    row.volume = volume(serial);
    row.status = OrdinaryLocationStatus::Active;
    row.detail = "Volume matched";
    return row;
}

OrdinaryLocationPolicy policyWith(std::vector<OrdinaryLocationDeclaration> rows,
                                  std::uint64_t generation = 1)
{
    OrdinaryLocationPolicy policy;
    policy.generation = generation;
    policy.declarations = std::move(rows);
    return policy;
}

void installPresentRoot(MapMetadataReader& probe,
                        MapVolumeReader& volumes,
                        const std::wstring& path,
                        std::uint32_t serial = 0xAABBCCDD)
{
    probe.probes[path] = presentDirectory();
    volumes.volumes[path] = volume(serial);
}

std::string metaValue(const std::wstring& dbPath, std::string_view key)
{
    SqliteDb db(dbPath, SqliteOpen::ReadOnly);
    SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    stmt.bindText(1, key);
    if (!stmt.step()) {
        return {};
    }
    return stmt.columnText(0);
}

bool tablePresent(const std::wstring& dbPath, const char* name)
{
    SqliteDb db(dbPath, SqliteOpen::ReadOnly);
    SqliteStmt stmt(
        db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1;");
    stmt.bindText(1, name);
    return stmt.step();
}

}  // namespace

SPACELENS_TEST(OrdinaryLocation_builtin_classifier_is_unchanged)
{
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Windows\\System32") ==
                      LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"D:\\") == LocationSafety::Protected);
    SPACELENS_REQUIRE(classifyLocation(L"D:\\Projects") ==
                      LocationSafety::Unknown);
    SPACELENS_REQUIRE(classifyLocation(L"E:\\TestData\\sample") ==
                      LocationSafety::Unknown);
    SPACELENS_REQUIRE(classifyLocation(L"C:\\Users\\TestUser\\Projects\\App") ==
                      LocationSafety::Ordinary);
}

SPACELENS_TEST(OrdinaryLocation_matching_is_component_aware)
{
    SPACELENS_REQUIRE(declarationContainsPath(L"D:\\proj", L"D:\\proj\\app"));
    SPACELENS_REQUIRE(declarationContainsPath(L"D:\\proj\\", L"D:\\PROJ\\app"));
    SPACELENS_REQUIRE(
        declarationContainsPath(L"\\\\?\\D:\\proj", L"D:\\proj\\app"));
    SPACELENS_REQUIRE(!declarationContainsPath(L"D:\\proj", L"D:\\project"));
    SPACELENS_REQUIRE(!declarationContainsPath(L"D:\\proj", L"D:\\proj-extra"));
    SPACELENS_REQUIRE(declarationContainsPath(L"D:\\Projects", L"D:\\Projects"));
    SPACELENS_REQUIRE(!declarationContainsPath(L"D:\\Projects",
                                               L"D:\\Projects\\..\\Other"));
}

SPACELENS_TEST(OrdinaryLocation_long_path_prefix_is_stripped)
{
    SPACELENS_REQUIRE_EQ(normalizeOrdinaryLocationPath(L"\\\\?\\D:\\Projects"),
                         normalizeOrdinaryLocationPath(L"D:\\Projects"));
    SPACELENS_REQUIRE_EQ(
        normalizeOrdinaryLocationPath(L"\\\\?\\UNC\\server\\share\\data"),
        normalizeOrdinaryLocationPath(L"\\\\server\\share\\data"));
}

SPACELENS_TEST(OrdinaryLocation_drive_roots_are_too_broad)
{
    SPACELENS_REQUIRE(isDriveRootPathKey(L"C:\\"));
    SPACELENS_REQUIRE(isDriveRootPathKey(L"d:\\"));
    SPACELENS_REQUIRE(!isDriveRootPathKey(L"D:\\Projects"));
}

SPACELENS_TEST(OrdinaryLocation_precedence_protected_sensitive_builtin_then_user)
{
    auto policy = policyWith({activeDeclaration(L"D:\\Projects", 11),
                              activeDeclaration(L"C:\\Windows", 12),
                              activeDeclaration(L"C:\\Users\\TestUser", 13)});

    const auto protectedPath =
        assessLocationSafety(L"C:\\Windows\\notepad.exe", policy);
    SPACELENS_REQUIRE_EQ(protectedPath.safety, LocationSafety::Protected);
    SPACELENS_REQUIRE_EQ(protectedPath.source,
                         LocationSafetySource::BuiltInProtected);

    const auto sensitive =
        assessLocationSafety(L"C:\\Users\\TestUser\\AppData\\Local\\Temp", policy);
    SPACELENS_REQUIRE_EQ(sensitive.safety, LocationSafety::Sensitive);
    SPACELENS_REQUIRE_EQ(sensitive.source, LocationSafetySource::BuiltInSensitive);

    const auto builtinOrdinary =
        assessLocationSafety(L"C:\\Users\\TestUser\\Projects\\App", policy);
    SPACELENS_REQUIRE_EQ(builtinOrdinary.safety, LocationSafety::Ordinary);
    SPACELENS_REQUIRE_EQ(builtinOrdinary.source,
                         LocationSafetySource::BuiltInOrdinary);

    const auto declared =
        assessLocationSafety(L"D:\\Projects\\src\\main.cpp", policy);
    SPACELENS_REQUIRE_EQ(declared.safety, LocationSafety::Ordinary);
    SPACELENS_REQUIRE_EQ(declared.source,
                         LocationSafetySource::UserDeclaredOrdinary);
    SPACELENS_REQUIRE_EQ(declared.declarationId, 11ULL);

    const auto sibling = assessLocationSafety(L"D:\\OtherVolumeData", policy);
    SPACELENS_REQUIRE_EQ(sibling.safety, LocationSafety::Unknown);
    SPACELENS_REQUIRE_EQ(sibling.source, LocationSafetySource::Unknown);
}

SPACELENS_TEST(OrdinaryLocation_empty_policy_preserves_unknown)
{
    OrdinaryLocationPolicy empty;
    SPACELENS_REQUIRE_EQ(effectiveLocationSafety(L"D:\\Projects\\app", empty),
                         LocationSafety::Unknown);
    SPACELENS_REQUIRE_EQ(empty.classify(L"C:\\Windows").safety,
                         LocationSafety::Protected);
}

SPACELENS_TEST(OrdinaryLocation_most_specific_active_declaration_wins)
{
    auto parent = activeDeclaration(L"D:\\Projects", 1);
    auto child = activeDeclaration(L"D:\\Projects\\App", 2);
    auto policy = policyWith({parent, child});

    const auto nested = policy.classify(L"D:\\Projects\\App\\src\\file.cpp");
    SPACELENS_REQUIRE_EQ(nested.source, LocationSafetySource::UserDeclaredOrdinary);
    SPACELENS_REQUIRE_EQ(nested.declarationId, 2ULL);

    const auto cousin = policy.classify(L"D:\\Projects\\Other\\readme.txt");
    SPACELENS_REQUIRE_EQ(cousin.declarationId, 1ULL);
}

SPACELENS_TEST(OrdinaryLocation_inactive_declaration_does_not_authorize)
{
    auto row = activeDeclaration(L"D:\\Projects", 4);
    row.status = OrdinaryLocationStatus::VolumeMismatch;
    const auto policy = policyWith({row});
    SPACELENS_REQUIRE_EQ(effectiveLocationSafety(L"D:\\Projects\\a.bin", policy),
                         LocationSafety::Unknown);
    SPACELENS_REQUIRE(policy.matchingActiveDeclaration(L"D:\\Projects\\a.bin") ==
                      nullptr);
}

SPACELENS_TEST(OrdinaryLocation_evaluate_rejects_invalid_and_blocked_roots)
{
    MapMetadataReader probe;
    MapVolumeReader volumes;

    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"", probe, volumes).result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L".", probe, volumes).result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"..", probe, volumes).result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"relative\\folder", probe, volumes)
            .result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"D:\\", probe, volumes).result,
        OrdinaryLocationAddResult::RejectedTooBroad);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"C:\\Windows", probe, volumes)
            .result,
        OrdinaryLocationAddResult::RejectedProtected);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"C:\\Users\\TestUser", probe,
                                            volumes)
            .result,
        OrdinaryLocationAddResult::RejectedSensitive);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(
            L"C:\\Users\\TestUser\\AppData\\Local\\Temp", probe, volumes)
            .result,
        OrdinaryLocationAddResult::RejectedSensitive);
}

SPACELENS_TEST(OrdinaryLocation_traversal_and_device_paths_are_rejected)
{
    MapMetadataReader probe;
    MapVolumeReader volumes;
    installPresentRoot(probe, volumes, L"C:\\NoSuch\\..\\Windows");
    installPresentRoot(probe, volumes, L"C:\\Windows\\.");
    installPresentRoot(probe, volumes, L"D:\\Projects\\foo\\..");
    installPresentRoot(probe, volumes, L"\\\\.\\C:\\Windows");
    installPresentRoot(probe, volumes, L"\\\\.\\D:\\Projects");

    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"C:\\NoSuch\\..\\Windows", probe,
                                            volumes)
            .result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"C:\\Windows\\.", probe, volumes)
            .result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"D:\\Projects\\foo\\..", probe,
                                            volumes)
            .result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"\\\\.\\C:\\Windows", probe,
                                            volumes)
            .result,
        OrdinaryLocationAddResult::InvalidPath);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(L"\\\\.\\D:\\Projects", probe,
                                            volumes)
            .result,
        OrdinaryLocationAddResult::InvalidPath);

    auto policy = policyWith({activeDeclaration(L"D:\\Projects", 1)});
    const auto escaped =
        assessLocationSafety(L"D:\\Projects\\..\\Windows\\notepad.exe", policy);
    SPACELENS_REQUIRE_EQ(escaped.safety, LocationSafety::Unknown);
    SPACELENS_REQUIRE_EQ(escaped.source, LocationSafetySource::Unknown);

    const auto windowsAlias = assessLocationSafety(
        L"C:\\NoSuch\\..\\Windows\\System32\\notepad.exe", policy);
    SPACELENS_REQUIRE(windowsAlias.safety != LocationSafety::Ordinary);
    SPACELENS_REQUIRE_EQ(windowsAlias.source, LocationSafetySource::Unknown);

    auto poisoned = activeDeclaration(L"C:\\NoSuch\\..\\Windows", 9);
    auto poisonedPolicy = policyWith({poisoned});
    SPACELENS_REQUIRE_EQ(
        assessLocationSafety(L"C:\\NoSuch\\..\\Windows\\System32\\notepad.exe",
                             poisonedPolicy)
            .source,
        LocationSafetySource::Unknown);
    SPACELENS_REQUIRE(poisonedPolicy.matchingActiveDeclaration(
                          L"C:\\NoSuch\\..\\Windows\\System32\\notepad.exe") ==
                      nullptr);

    refreshOrdinaryLocationDeclaration(poisoned, probe, volumes);
    SPACELENS_REQUIRE_EQ(poisoned.status, OrdinaryLocationStatus::Invalid);
}

SPACELENS_TEST(OrdinaryLocation_evaluate_rejects_missing_file_and_reparse)
{
    MapMetadataReader probe;
    MapVolumeReader volumes;
    const std::wstring missing = L"D:\\Projects\\MissingRoot";
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(missing, probe, volumes).result,
        OrdinaryLocationAddResult::PathUnavailable);

    const std::wstring filePath = L"D:\\Projects\\notes.txt";
    probe.probes[filePath] = presentFile();
    volumes.volumes[filePath] = volume(7);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(filePath, probe, volumes).result,
        OrdinaryLocationAddResult::InvalidPath);

    const std::wstring junction = L"D:\\Projects\\LinkRoot";
    probe.probes[junction] = presentDirectory(true);
    volumes.volumes[junction] = volume(7);
    SPACELENS_REQUIRE_EQ(
        evaluateOrdinaryLocationDeclaration(junction, probe, volumes).result,
        OrdinaryLocationAddResult::RejectedReparse);
}

SPACELENS_TEST(OrdinaryLocation_evaluate_persists_volume_unavailable)
{
    MapMetadataReader probe;
    MapVolumeReader volumes;
    const std::wstring root = L"D:\\Projects";
    probe.probes[root] = presentDirectory();
    volumes.volumes[root] = volume(0, L"");

    const auto outcome =
        evaluateOrdinaryLocationDeclaration(root, probe, volumes, 42);
    SPACELENS_REQUIRE_EQ(outcome.result,
                         OrdinaryLocationAddResult::VolumeUnavailable);
    SPACELENS_REQUIRE_EQ(outcome.declaration.status,
                         OrdinaryLocationStatus::VolumeUnavailable);
    SPACELENS_REQUIRE(!outcome.declaration.normalizedPathKey.empty());
    SPACELENS_REQUIRE_EQ(
        effectiveLocationSafety(L"D:\\Projects\\a.bin",
                                policyWith({outcome.declaration})),
        LocationSafety::Unknown);
}

SPACELENS_TEST(OrdinaryLocation_evaluate_active_when_volume_matches)
{
    MapMetadataReader probe;
    MapVolumeReader volumes;
    const std::wstring root = L"E:\\TestData";
    installPresentRoot(probe, volumes, root, 0x11223344);
    const auto outcome =
        evaluateOrdinaryLocationDeclaration(root, probe, volumes, 9);
    SPACELENS_REQUIRE_EQ(outcome.result, OrdinaryLocationAddResult::Added);
    SPACELENS_REQUIRE_EQ(outcome.declaration.status,
                         OrdinaryLocationStatus::Active);
    SPACELENS_REQUIRE_EQ(outcome.declaration.volume.serial, 0x11223344u);
}

SPACELENS_TEST(OrdinaryLocation_refresh_volume_and_path_status)
{
    MapMetadataReader probe;
    MapVolumeReader volumes;
    auto row = activeDeclaration(L"D:\\Projects", 8, 100);
    probe.probes[row.configuredPath] = presentDirectory();
    volumes.volumes[row.configuredPath] = volume(200);
    refreshOrdinaryLocationDeclaration(row, probe, volumes);
    SPACELENS_REQUIRE_EQ(row.status, OrdinaryLocationStatus::VolumeMismatch);

    volumes.volumes[row.configuredPath] = volume(100);
    refreshOrdinaryLocationDeclaration(row, probe, volumes);
    SPACELENS_REQUIRE_EQ(row.status, OrdinaryLocationStatus::Active);

    volumes.volumes[row.configuredPath] = volume(0, L"");
    refreshOrdinaryLocationDeclaration(row, probe, volumes);
    SPACELENS_REQUIRE_EQ(row.status, OrdinaryLocationStatus::VolumeUnavailable);

    probe.probes.erase(row.configuredPath);
    volumes.volumes[row.configuredPath] = volume(100);
    refreshOrdinaryLocationDeclaration(row, probe, volumes);
    SPACELENS_REQUIRE_EQ(row.status, OrdinaryLocationStatus::PathUnavailable);

    probe.probes[row.configuredPath] = presentDirectory(true);
    refreshOrdinaryLocationDeclaration(row, probe, volumes);
    SPACELENS_REQUIRE_EQ(row.status, OrdinaryLocationStatus::Invalid);
}

SPACELENS_TEST(OrdinaryLocation_volume_unavailable_is_not_silently_upgraded)
{
    auto row = activeDeclaration(L"D:\\Projects", 8, 0);
    row.volume.available = false;
    row.status = OrdinaryLocationStatus::VolumeUnavailable;
    MapMetadataReader probe;
    MapVolumeReader volumes;
    probe.probes[row.configuredPath] = presentDirectory();
    volumes.volumes[row.configuredPath] = volume(123);
    refreshOrdinaryLocationDeclaration(row, probe, volumes);
    SPACELENS_REQUIRE_EQ(row.status, OrdinaryLocationStatus::VolumeUnavailable);
    SPACELENS_REQUIRE_EQ(row.volume.serial, 0u);
    SPACELENS_REQUIRE_EQ(
        effectiveLocationSafety(L"D:\\Projects\\a.bin", policyWith({row})),
        LocationSafety::Unknown);
}

SPACELENS_TEST(OrdinaryLocation_descendant_reparse_does_not_inherit_windows)
{
    auto policy = policyWith({activeDeclaration(L"D:\\Projects", 1)});
    SPACELENS_REQUIRE_EQ(
        effectiveLocationSafety(L"C:\\Windows\\System32", policy),
        LocationSafety::Protected);
    SPACELENS_REQUIRE_EQ(
        assessLocationSafety(L"D:\\Projects\\junction-target", policy).source,
        LocationSafetySource::UserDeclaredOrdinary);
}

SPACELENS_TEST(OrdinaryLocation_store_round_trip_and_duplicate_key)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));

    MapMetadataReader probe;
    MapVolumeReader volumes;
    const std::wstring root = L"D:\\Projects";
    installPresentRoot(probe, volumes, root);

    const auto added = controller.addOrdinaryLocation(root, probe, volumes, 5);
    SPACELENS_REQUIRE_EQ(added.result, OrdinaryLocationAddResult::Added);
    SPACELENS_REQUIRE(added.declaration.id != 0);

    const auto again = controller.addOrdinaryLocation(L"d:\\projects\\", probe,
                                                      volumes, 6);
    SPACELENS_REQUIRE_EQ(again.result, OrdinaryLocationAddResult::AlreadyExists);
    SPACELENS_REQUIRE_EQ(again.declaration.id, added.declaration.id);

    const auto firstGen = controller.ordinaryLocationPolicy().generation;
    SPACELENS_REQUIRE(firstGen >= 1);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    const auto loaded = reopened.ordinaryLocationPolicy();
    SPACELENS_REQUIRE_EQ(loaded.declarations.size(), 1u);
    SPACELENS_REQUIRE_EQ(loaded.generation, firstGen);
    SPACELENS_REQUIRE_EQ(loaded.classify(L"D:\\Projects\\src").source,
                         LocationSafetySource::UserDeclaredOrdinary);
    SPACELENS_REQUIRE_EQ(metaValue(dir.dbPath(), "review_schema_version"),
                         std::string("1"));
    SPACELENS_REQUIRE_EQ(metaValue(dir.dbPath(), "location_schema_version"),
                         std::string("1"));
    SPACELENS_REQUIRE(tablePresent(dir.dbPath(),
                                   "ordinary_location_declarations"));
}

SPACELENS_TEST(OrdinaryLocation_volume_unavailable_is_persisted_inactive)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    MapMetadataReader probe;
    MapVolumeReader volumes;
    const std::wstring root = L"E:\\TestData";
    probe.probes[root] = presentDirectory();
    volumes.volumes[root] = volume(0, L"");

    const auto added = controller.addOrdinaryLocation(root, probe, volumes, 1);
    SPACELENS_REQUIRE_EQ(added.result,
                         OrdinaryLocationAddResult::VolumeUnavailable);
    const auto policy = controller.ordinaryLocationPolicy();
    SPACELENS_REQUIRE_EQ(policy.declarations.size(), 1u);
    SPACELENS_REQUIRE_EQ(policy.declarations.front().status,
                         OrdinaryLocationStatus::VolumeUnavailable);
    SPACELENS_REQUIRE_EQ(effectiveLocationSafety(L"E:\\TestData\\file.bin", policy),
                         LocationSafety::Unknown);
}

SPACELENS_TEST(OrdinaryLocation_remove_increments_generation)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    MapMetadataReader probe;
    MapVolumeReader volumes;
    installPresentRoot(probe, volumes, L"D:\\Projects");
    const auto added =
        controller.addOrdinaryLocation(L"D:\\Projects", probe, volumes, 1);
    const auto before = controller.ordinaryLocationPolicy().generation;
    SPACELENS_REQUIRE(controller.removeOrdinaryLocation(added.declaration.id).ok);
    const auto after = controller.ordinaryLocationPolicy();
    SPACELENS_REQUIRE(after.generation > before);
    SPACELENS_REQUIRE(after.declarations.empty());
    SPACELENS_REQUIRE_EQ(effectiveLocationSafety(L"D:\\Projects\\a.bin", after),
                         LocationSafety::Unknown);
}

SPACELENS_TEST(OrdinaryLocation_review_mutations_do_not_erase_declarations)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    MapMetadataReader probe;
    MapVolumeReader volumes;
    installPresentRoot(probe, volumes, L"D:\\Projects");
    SPACELENS_REQUIRE(
        controller.addOrdinaryLocation(L"D:\\Projects", probe, volumes, 1)
            .added());

    CleanupCandidate item;
    item.path = L"D:\\Projects\\scratch.bin";
    item.kind = ItemKind::File;
    item.sizeAtSelection = 4;
    item.capturedSafety = LocationSafety::Ordinary;
    SPACELENS_REQUIRE(controller.add(item).ok);
    SPACELENS_REQUIRE(controller.clear().ok);

    const auto policy = controller.ordinaryLocationPolicy();
    SPACELENS_REQUIRE_EQ(policy.declarations.size(), 1u);
    SPACELENS_REQUIRE_EQ(policy.classify(L"D:\\Projects\\scratch.bin").source,
                         LocationSafetySource::UserDeclaredOrdinary);
}

SPACELENS_TEST(OrdinaryLocation_add_remove_blocked_while_review_mutations_blocked)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    MapMetadataReader probe;
    MapVolumeReader volumes;
    installPresentRoot(probe, volumes, L"D:\\Projects");
    const auto added =
        controller.addOrdinaryLocation(L"D:\\Projects", probe, volumes, 1);
    SPACELENS_REQUIRE(added.added());

    controller.setReviewMutationsBlocked(true);
    const auto blockedAdd =
        controller.addOrdinaryLocation(L"E:\\TestData", probe, volumes, 2);
    SPACELENS_REQUIRE_EQ(blockedAdd.result, OrdinaryLocationAddResult::Error);
    const auto blockedRemove =
        controller.removeOrdinaryLocation(added.declaration.id);
    SPACELENS_REQUIRE(!blockedRemove.ok);
    SPACELENS_REQUIRE_EQ(controller.ordinaryLocationPolicy().declarations.size(),
                         1u);
}

SPACELENS_TEST(OrdinaryLocation_store_status_update_round_trip)
{
    TempDir dir;
    CleanupReviewStore store;
    SPACELENS_REQUIRE(store.open(dir.dbPath()).ok);

    OrdinaryLocationDeclaration row;
    row.configuredPath = L"D:\\Projects";
    row.normalizedPathKey = normalizeOrdinaryLocationPath(row.configuredPath);
    row.createdAt = 1;
    row.volume = volume(50);
    row.status = OrdinaryLocationStatus::Active;
    row.detail = "Volume matched";

    auto added = store.addOrdinaryLocation(row);
    SPACELENS_REQUIRE_EQ(added.result, OrdinaryLocationAddResult::Added);
    SPACELENS_REQUIRE(added.declaration.id != 0);

    added.declaration.status = OrdinaryLocationStatus::VolumeMismatch;
    added.declaration.detail = "Volume serial does not match the declared volume";
    const auto saved = store.saveOrdinaryLocationStatuses({added.declaration});
    SPACELENS_REQUIRE(saved.ok);

    const auto loaded = store.loadOrdinaryLocations();
    SPACELENS_REQUIRE_EQ(loaded.size(), 1u);
    SPACELENS_REQUIRE_EQ(loaded.front().id, added.declaration.id);
    SPACELENS_REQUIRE_EQ(loaded.front().status,
                         OrdinaryLocationStatus::VolumeMismatch);

    SqliteDb db(dir.dbPath(), SqliteOpen::ReadOnly);
    SqliteStmt stmt(db, "SELECT id, status FROM ordinary_location_declarations;");
    SPACELENS_REQUIRE(stmt.step());
    SPACELENS_REQUIRE_EQ(static_cast<std::uint64_t>(stmt.columnInt64(0)),
                         added.declaration.id);
    SPACELENS_REQUIRE_EQ(stmt.columnText(1), std::string("VolumeMismatch"));
}

SPACELENS_TEST(OrdinaryLocation_refresh_policy_updates_persisted_status)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    MapMetadataReader probe;
    MapVolumeReader volumes;
    installPresentRoot(probe, volumes, L"D:\\Projects", 50);
    const auto added =
        controller.addOrdinaryLocation(L"D:\\Projects", probe, volumes, 1);
    SPACELENS_REQUIRE(added.added());
    SPACELENS_REQUIRE(added.declaration.id != 0);

    volumes.volumes[L"D:\\Projects"] = volume(99);
    const auto refreshed = controller.refreshOrdinaryLocations(probe, volumes);
    SPACELENS_REQUIRE_EQ(refreshed.declarations.size(), 1u);
    SPACELENS_REQUIRE_EQ(refreshed.declarations.front().id, added.declaration.id);
    SPACELENS_REQUIRE_EQ(refreshed.declarations.front().status,
                         OrdinaryLocationStatus::VolumeMismatch);

    const auto reloaded = controller.ordinaryLocationPolicy();
    SPACELENS_REQUIRE_EQ(reloaded.declarations.size(), 1u);
    SPACELENS_REQUIRE_EQ(reloaded.declarations.front().id, added.declaration.id);
    SPACELENS_REQUIRE_EQ(std::string(toString(reloaded.declarations.front().status)),
                         std::string("VolumeMismatch"));
}
