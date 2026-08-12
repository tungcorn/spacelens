#include "TestRunner.hpp"
#include "core/CleanupReview.hpp"

using namespace spacelens;

SPACELENS_TEST(CleanupReview_add_remove_total)
{
    CleanupReview review;
    CleanupCandidate a;
    a.path = L"D:\\proj\\build";
    a.kind = ItemKind::Directory;
    a.sizeAtSelection = 1000;
    a.reasonAdded = "manual";
    const auto id1 = review.add(a);
    SPACELENS_REQUIRE(id1 != 0);
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 1000ULL);

    CleanupCandidate b;
    b.path = L"D:\\proj\\out.bin";
    b.kind = ItemKind::File;
    b.sizeAtSelection = 500;
    review.add(std::move(b));
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 1500ULL);

    SPACELENS_REQUIRE(review.removeById(id1));
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 500ULL);
}

SPACELENS_TEST(CleanupReview_duplicate_path_updates)
{
    CleanupReview review;
    CleanupCandidate a;
    a.path = L"D:\\Same\\Path";
    a.sizeAtSelection = 10;
    const auto id1 = review.add(a);

    CleanupCandidate b;
    b.path = L"d:\\same\\path";  // case-insensitive duplicate
    b.sizeAtSelection = 99;
    const auto id2 = review.add(b);
    SPACELENS_REQUIRE_EQ(id1, id2);
    SPACELENS_REQUIRE_EQ(review.size(), 1u);
    SPACELENS_REQUIRE_EQ(review.totalLogicalSize(), 99ULL);
}

SPACELENS_TEST(CleanupReview_clear_and_report)
{
    CleanupReview review;
    CleanupCandidate a;
    a.path = L"D:\\x";
    a.sizeAtSelection = 1;
    a.classification.category = StorageCategory::BuildArtifact;
    a.classification.reason = "test";
    review.add(std::move(a));
    const auto report = review.copyReport();
    SPACELENS_REQUIRE(report.find("Cleanup Review") != std::string::npos);
    SPACELENS_REQUIRE(report.find("not authorization") != std::string::npos);
    review.clear();
    SPACELENS_REQUIRE(review.empty());
}
