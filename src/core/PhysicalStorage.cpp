#include "core/PhysicalStorage.hpp"

#include "core/SafetyPolicy.hpp"

#include <algorithm>
#include <cwctype>

namespace spacelens {

const char* toString(HardLinkCoverage coverage) noexcept
{
    switch (coverage) {
    case HardLinkCoverage::Complete:
        return "complete";
    case HardLinkCoverage::Incomplete:
        return "incomplete";
    case HardLinkCoverage::Unknown:
        return "unknown";
    }
    return "unknown";
}

HardLinkCoverage parseHardLinkCoverage(std::string_view text) noexcept
{
    if (text == "complete") {
        return HardLinkCoverage::Complete;
    }
    if (text == "incomplete") {
        return HardLinkCoverage::Incomplete;
    }
    return HardLinkCoverage::Unknown;
}

HardLinkCoverage classifyHardLinkCoverage(std::uint32_t filesystemLinks,
                                          std::uint32_t observedInIndex,
                                          std::uint32_t observedInCandidate,
                                          bool identityKnown) noexcept
{
    if (!identityKnown || filesystemLinks == 0) {
        return HardLinkCoverage::Unknown;
    }
    if (observedInIndex < filesystemLinks ||
        observedInCandidate < filesystemLinks) {
        return HardLinkCoverage::Incomplete;
    }
    if (observedInCandidate >= filesystemLinks &&
        observedInIndex >= filesystemLinks) {
        return HardLinkCoverage::Complete;
    }
    return HardLinkCoverage::Incomplete;
}

bool addSaturating(ByteSize& total, ByteSize extra) noexcept
{
    constexpr ByteSize kMax = ~ByteSize{0};
    if (extra > kMax - total) {
        total = kMax;
        return true;
    }
    total += extra;
    return false;
}

UniqueAllocation summarizeIdentities(const std::vector<IdentityAllocation>& items)
{
    UniqueAllocation out;
    out.identityCount = items.size();
    if (items.empty()) {
        out.uniqueAllocatedBytes = 0;
        out.exactReclaimBytes = 0;
        out.allAllocationKnown = true;
        out.coverage = HardLinkCoverage::Complete;
        return out;
    }

    ByteSize unique = 0;
    ByteSize exact = 0;
    bool uniqueAny = false;
    bool exactAny = false;
    bool overflow = false;
    bool sawIncomplete = false;
    bool sawUnknown = false;
    bool sawComplete = false;

    for (const auto& item : items) {
        const HardLinkCoverage cov = classifyHardLinkCoverage(
            item.filesystemLinks, item.observedInIndex,
            item.observedInCandidate, item.identity.valid());
        if (!item.allocationKnown || !item.allocatedBytes.has_value()) {
            out.allAllocationKnown = false;
            ++out.unknownIdentityCount;
            sawUnknown = true;
        } else {
            uniqueAny = true;
            overflow = addSaturating(unique, *item.allocatedBytes) || overflow;
        }

        if (cov == HardLinkCoverage::Complete && item.allocationKnown &&
            item.allocatedBytes.has_value()) {
            exactAny = true;
            overflow = addSaturating(exact, *item.allocatedBytes) || overflow;
            ++out.exactIdentityCount;
            sawComplete = true;
        } else if (cov == HardLinkCoverage::Incomplete) {
            ++out.incompleteIdentityCount;
            sawIncomplete = true;
        } else if (cov == HardLinkCoverage::Unknown) {
            sawUnknown = true;
        }
    }

    if (uniqueAny) {
        out.uniqueAllocatedBytes = unique;
    }
    if (exactAny) {
        out.exactReclaimBytes = exact;
    }

    if (sawUnknown && !sawComplete && !sawIncomplete) {
        out.coverage = HardLinkCoverage::Unknown;
    } else if (sawIncomplete || sawUnknown) {
        out.coverage = HardLinkCoverage::Incomplete;
    } else {
        out.coverage = HardLinkCoverage::Complete;
    }
    (void)overflow;
    return out;
}

bool pathIsUnderNormalized(std::wstring_view path, std::wstring_view ancestor)
{
    if (path.empty() || ancestor.empty()) {
        return false;
    }
    const std::wstring p = normalizePathForPolicy(path);
    const std::wstring a = normalizePathForPolicy(ancestor);
    if (p.size() < a.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(p[i]) != std::towlower(a[i])) {
            return false;
        }
    }
    if (p.size() == a.size()) {
        return true;
    }
    if (a.back() == L'\\' || a.back() == L'/') {
        return true;
    }
    return p[a.size()] == L'\\' || p[a.size()] == L'/';
}

}  // namespace spacelens
