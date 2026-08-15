#include "core/ReclaimEvidence.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace spacelens {
namespace {

std::string compactLower(std::string_view text)
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

}  // namespace

const char* toString(ReclaimConfidence value) noexcept
{
    switch (value) {
    case ReclaimConfidence::Verified:
        return "Verified";
    case ReclaimConfidence::Strong:
        return "Strong";
    case ReclaimConfidence::Heuristic:
        return "Heuristic";
    case ReclaimConfidence::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* toString(ReclaimActionability value) noexcept
{
    switch (value) {
    case ReclaimActionability::ActionableWithoutContentJudgment:
        return "ActionableWithoutContentJudgment";
    case ReclaimActionability::RequiresContentJudgment:
        return "RequiresContentJudgment";
    case ReclaimActionability::InformationalOnly:
        return "InformationalOnly";
    }
    return "InformationalOnly";
}

const char* toString(ReclaimDisruption value) noexcept
{
    switch (value) {
    case ReclaimDisruption::Low:
        return "Low";
    case ReclaimDisruption::Moderate:
        return "Moderate";
    case ReclaimDisruption::Higher:
        return "Higher";
    case ReclaimDisruption::Review:
        return "Review";
    }
    return "Review";
}

const char* toString(ReclaimBasis value) noexcept
{
    switch (value) {
    case ReclaimBasis::UniqueAllocatedBytes:
        return "unique_allocated_bytes";
    case ReclaimBasis::ProviderOwnedUniqueAllocatedBytes:
        return "provider_owned_unique_allocated_bytes";
    case ReclaimBasis::SnapshotAllocatedBytes:
        return "snapshot_allocated_bytes";
    case ReclaimBasis::IncompleteHardlinkCoverage:
        return "incomplete_hardlink_coverage";
    case ReclaimBasis::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* toString(ReclaimConsequence value) noexcept
{
    switch (value) {
    case ReclaimConsequence::NoneKnown:
        return "none_known";
    case ReclaimConsequence::RebuildRequired:
        return "rebuild_required";
    case ReclaimConsequence::DependencyReinstallRequired:
        return "dependency_reinstall_required";
    case ReclaimConsequence::RedownloadRequired:
        return "redownload_required";
    case ReclaimConsequence::ContentJudgmentRequired:
        return "content_judgment_required";
    case ReclaimConsequence::Unknown:
        return "unknown";
    }
    return "unknown";
}

ReclaimConfidence parseReclaimConfidence(std::string_view text) noexcept
{
    const std::string v = compactLower(text);
    if (v == "verified") {
        return ReclaimConfidence::Verified;
    }
    if (v == "strong") {
        return ReclaimConfidence::Strong;
    }
    if (v == "heuristic") {
        return ReclaimConfidence::Heuristic;
    }
    return ReclaimConfidence::Unknown;
}

ReclaimActionability parseReclaimActionability(std::string_view text) noexcept
{
    const std::string v = compactLower(text);
    if (v == "actionablewithoutcontentjudgment") {
        return ReclaimActionability::ActionableWithoutContentJudgment;
    }
    if (v == "requirescontentjudgment") {
        return ReclaimActionability::RequiresContentJudgment;
    }
    return ReclaimActionability::InformationalOnly;
}

ReclaimDisruption parseReclaimDisruption(std::string_view text) noexcept
{
    const std::string v = compactLower(text);
    if (v == "low") {
        return ReclaimDisruption::Low;
    }
    if (v == "moderate") {
        return ReclaimDisruption::Moderate;
    }
    if (v == "higher") {
        return ReclaimDisruption::Higher;
    }
    return ReclaimDisruption::Review;
}

ReclaimBasis parseReclaimBasis(std::string_view text) noexcept
{
    const std::string v = compactLower(text);
    if (v == "uniqueallocatedbytes") {
        return ReclaimBasis::UniqueAllocatedBytes;
    }
    if (v == "providerowneduniqueallocatedbytes") {
        return ReclaimBasis::ProviderOwnedUniqueAllocatedBytes;
    }
    if (v == "snapshotallocatedbytes") {
        return ReclaimBasis::SnapshotAllocatedBytes;
    }
    if (v == "incompletehardlinkcoverage") {
        return ReclaimBasis::IncompleteHardlinkCoverage;
    }
    return ReclaimBasis::Unknown;
}

ReclaimConsequence parseReclaimConsequence(std::string_view text) noexcept
{
    const std::string v = compactLower(text);
    if (v == "noneknown") {
        return ReclaimConsequence::NoneKnown;
    }
    if (v == "rebuildrequired") {
        return ReclaimConsequence::RebuildRequired;
    }
    if (v == "dependencyreinstallrequired") {
        return ReclaimConsequence::DependencyReinstallRequired;
    }
    if (v == "redownloadrequired") {
        return ReclaimConsequence::RedownloadRequired;
    }
    if (v == "contentjudgmentrequired") {
        return ReclaimConsequence::ContentJudgmentRequired;
    }
    return ReclaimConsequence::Unknown;
}

}  // namespace spacelens
