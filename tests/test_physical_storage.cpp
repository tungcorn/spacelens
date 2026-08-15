#include "TestRunner.hpp"

#include "core/PhysicalStorage.hpp"

#include <limits>
#include <vector>

using namespace spacelens;

SPACELENS_TEST(Physical_coverage_complete_when_all_links_seen)
{
    SPACELENS_REQUIRE(classifyHardLinkCoverage(2, 2, 2, true) ==
                      HardLinkCoverage::Complete);
    SPACELENS_REQUIRE(classifyHardLinkCoverage(1, 1, 1, true) ==
                      HardLinkCoverage::Complete);
}

SPACELENS_TEST(Physical_coverage_incomplete_when_index_or_candidate_short)
{
    SPACELENS_REQUIRE(classifyHardLinkCoverage(3, 2, 3, true) ==
                      HardLinkCoverage::Incomplete);
    SPACELENS_REQUIRE(classifyHardLinkCoverage(3, 3, 1, true) ==
                      HardLinkCoverage::Incomplete);
}

SPACELENS_TEST(Physical_coverage_unknown_without_identity_or_fs_links)
{
    SPACELENS_REQUIRE(classifyHardLinkCoverage(2, 2, 2, false) ==
                      HardLinkCoverage::Unknown);
    SPACELENS_REQUIRE(classifyHardLinkCoverage(0, 1, 1, true) ==
                      HardLinkCoverage::Unknown);
}

SPACELENS_TEST(Physical_summarize_never_uses_logical_as_allocated)
{
    IdentityAllocation a;
    a.identity.fileId = 11;
    a.identity.volumeSerial = 7;
    a.allocatedBytes.reset();
    a.allocationKnown = false;
    a.filesystemLinks = 1;
    a.observedInIndex = 1;
    a.observedInCandidate = 1;

    const auto sum = summarizeIdentities({a});
    SPACELENS_REQUIRE(!sum.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(!sum.exactReclaimBytes.has_value());
    SPACELENS_REQUIRE(!sum.allAllocationKnown);
    SPACELENS_REQUIRE(sum.coverage == HardLinkCoverage::Unknown ||
                      sum.unknownIdentityCount == 1);
}

SPACELENS_TEST(Physical_incomplete_hardlink_never_inflates_exact)
{
    IdentityAllocation complete;
    complete.identity.fileId = 1;
    complete.identity.volumeSerial = 9;
    complete.allocatedBytes = 1000;
    complete.allocationKnown = true;
    complete.filesystemLinks = 1;
    complete.observedInIndex = 1;
    complete.observedInCandidate = 1;

    IdentityAllocation incomplete;
    incomplete.identity.fileId = 2;
    incomplete.identity.volumeSerial = 9;
    incomplete.allocatedBytes = 50000;
    incomplete.allocationKnown = true;
    incomplete.filesystemLinks = 3;
    incomplete.observedInIndex = 1;
    incomplete.observedInCandidate = 1;

    const auto sum = summarizeIdentities({complete, incomplete});
    SPACELENS_REQUIRE(sum.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE_EQ(*sum.uniqueAllocatedBytes, 51000ULL);
    SPACELENS_REQUIRE(sum.exactReclaimBytes.has_value());
    SPACELENS_REQUIRE_EQ(*sum.exactReclaimBytes, 1000ULL);
    SPACELENS_REQUIRE(sum.coverage == HardLinkCoverage::Incomplete);
    SPACELENS_REQUIRE_EQ(sum.incompleteIdentityCount, 1ULL);
}

SPACELENS_TEST(Physical_complete_identities_are_unique)
{
    IdentityAllocation a;
    a.identity.fileId = 42;
    a.identity.volumeSerial = 1;
    a.allocatedBytes = 4096;
    a.allocationKnown = true;
    a.filesystemLinks = 2;
    a.observedInIndex = 2;
    a.observedInCandidate = 2;

    const auto sum = summarizeIdentities({a});
    SPACELENS_REQUIRE(sum.exactReclaimBytes.has_value());
    SPACELENS_REQUIRE_EQ(*sum.exactReclaimBytes, 4096ULL);
    SPACELENS_REQUIRE(sum.coverage == HardLinkCoverage::Complete);
}

SPACELENS_TEST(Physical_add_saturating_overflow)
{
    ByteSize total = std::numeric_limits<ByteSize>::max() - 5;
    SPACELENS_REQUIRE(addSaturating(total, 10));
    SPACELENS_REQUIRE_EQ(total, std::numeric_limits<ByteSize>::max());
    ByteSize small = 3;
    SPACELENS_REQUIRE(!addSaturating(small, 4));
    SPACELENS_REQUIRE_EQ(small, 7ULL);
}

SPACELENS_TEST(Physical_path_ancestry_is_component_safe)
{
    SPACELENS_REQUIRE(pathIsUnderNormalized(L"D:\\proj\\target\\x", L"D:\\proj\\target"));
    SPACELENS_REQUIRE(pathIsUnderNormalized(L"D:\\proj\\target", L"D:\\proj\\target"));
    SPACELENS_REQUIRE(!pathIsUnderNormalized(L"D:\\proj\\target2\\x", L"D:\\proj\\target"));
    SPACELENS_REQUIRE(!pathIsUnderNormalized(L"D:\\proj", L"D:\\proj\\target"));
}
