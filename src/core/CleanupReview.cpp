#include "core/CleanupReview.hpp"

#include "core/CleanupPlan.hpp"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <sstream>
#include <utility>

namespace spacelens {
namespace {

bool isDriveLetter(wchar_t ch)
{
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

bool isNonZero(const std::array<std::uint8_t, 16>& id)
{
    return std::any_of(id.begin(), id.end(),
                       [](std::uint8_t value) { return value != 0; });
}

bool sameKind(const CleanupCandidate& a, const CleanupCandidate& b) noexcept
{
    return a.kind == b.kind;
}

std::string numberString(std::uint64_t value)
{
    return std::to_string(value);
}

std::string identityText(const CleanupIdentity& identity)
{
    if (!isIdentityAvailable(identity)) {
        return "unavailable";
    }
    std::ostringstream os;
    os << toString(identity.source) << ':' << identity.volumeSerial << ':';
    if (identity.source == CleanupIdentitySource::FileIndex64Fallback) {
        os << identity.fileIndex64;
    } else {
        for (const auto byte : identity.fileId128) {
            char buffer[3]{};
            std::snprintf(buffer, sizeof(buffer), "%02x", byte);
            os << buffer;
        }
    }
    return os.str();
}

void normalizeCandidate(CleanupCandidate& candidate)
{
    const bool hadObjectEvidence = candidate.objectEvidence.available;
    const bool hadRecursiveObjectEvidence =
        hadObjectEvidence &&
        candidate.objectEvidence.sizeScope == CleanupEvidenceScope::Recursive;
    const ByteSize recursiveSize =
        hadRecursiveObjectEvidence ? candidate.objectEvidence.logicalSize
                                   : candidate.sizeAtSelection;
    const FileTimeTicks recursiveNewestWrite =
        hadRecursiveObjectEvidence && candidate.objectEvidence.lastWriteTime != 0
            ? candidate.objectEvidence.lastWriteTime
            : candidate.lastWriteTime;

    if (!candidate.objectEvidence.available) {
        candidate.objectEvidence.available = true;
        candidate.objectEvidence.identity = {};
        candidate.objectEvidence.kind = candidate.kind;
        candidate.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
        candidate.objectEvidence.logicalSize = candidate.kind == ItemKind::File
                                                   ? candidate.sizeAtSelection
                                                   : 0;
        // Directory scalar lastWriteTime is descendant activity, not object mtime.
        candidate.objectEvidence.lastWriteTime =
            candidate.kind == ItemKind::File ? candidate.lastWriteTime : 0;
        candidate.objectEvidence.attributes = candidate.attributes;
    }
    if (candidate.objectEvidence.kind != candidate.kind) {
        candidate.objectEvidence.kind = candidate.kind;
    }
    if (candidate.kind != ItemKind::File) {
        // Legacy scalar candidates and recursive object evidence still need
        // aggregate synthesis. Explicit direct-only captures keep unavailable
        // recursive evidence unavailable so they cannot cover descendants.
        if (!candidate.historicalDirectoryAggregate.available &&
            (!hadObjectEvidence || hadRecursiveObjectEvidence)) {
            candidate.historicalDirectoryAggregate.available = true;
            candidate.historicalDirectoryAggregate.recursiveLogicalSize =
                recursiveSize;
            candidate.historicalDirectoryAggregate.newestDescendantWrite =
                recursiveNewestWrite;
        }
        // A directory's object evidence is direct metadata. Its historical
        // recursive size is kept only in the aggregate evidence above.
        if (candidate.objectEvidence.sizeScope ==
            CleanupEvidenceScope::Recursive) {
            candidate.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
            candidate.objectEvidence.logicalSize = 0;
            // The recursive timestamp is newest-descendant activity, not the
            // directory object's own write time.
            candidate.objectEvidence.lastWriteTime = 0;
        }
    }
    if (candidate.capturedSafety == LocationSafety::Unknown) {
        candidate.capturedSafety = classifyLocation(candidate.path);
    }
}

bool sameStrongIdentity(const CleanupCandidate& a, const CleanupCandidate& b)
{
    const auto ia = identityOf(a);
    const auto ib = identityOf(b);
    return isStrongIdentity(ia) && isStrongIdentity(ib) &&
           identitiesEqual(ia, ib);
}

bool differentStrongIdentity(const CleanupCandidate& a,
                             const CleanupCandidate& b)
{
    const auto ia = identityOf(a);
    const auto ib = identityOf(b);
    return isStrongIdentity(ia) && isStrongIdentity(ib) &&
           !identitiesEqual(ia, ib);
}

void replaceKeepingId(CleanupCandidate& destination, CleanupCandidate source)
{
    const auto id = destination.id;
    source.id = id;
    normalizeCandidate(source);
    destination = std::move(source);
}

}  // namespace

const char* toString(ItemKind kind) noexcept
{
    switch (kind) {
    case ItemKind::File:
        return "File";
    case ItemKind::Directory:
        return "Directory";
    case ItemKind::ReparseDirectory:
        return "ReparseDirectory";
    }
    return "File";
}

const char* toString(CleanupIdentitySource source) noexcept
{
    switch (source) {
    case CleanupIdentitySource::FileId128:
        return "FileId128";
    case CleanupIdentitySource::FileIndex64Fallback:
        return "FileIndex64Fallback";
    case CleanupIdentitySource::Unavailable:
        return "Unavailable";
    }
    return "Unavailable";
}

CleanupIdentity makeFileId128Identity(
    std::uint64_t volumeSerial,
    const std::array<std::uint8_t, 16>& id) noexcept
{
    CleanupIdentity out;
    out.source = CleanupIdentitySource::FileId128;
    out.volumeSerial = volumeSerial;
    out.fileId128 = id;
    return out;
}

CleanupIdentity makeFileIndex64FallbackIdentity(std::uint64_t volumeSerial,
                                                std::uint64_t fileIndex64) noexcept
{
    CleanupIdentity out;
    out.source = CleanupIdentitySource::FileIndex64Fallback;
    out.volumeSerial = volumeSerial;
    out.fileIndex64 = fileIndex64;
    return out;
}

CleanupIdentitySource identityStrength(const CleanupIdentity& identity) noexcept
{
    return identity.source;
}

bool isStrongIdentity(const CleanupIdentity& identity) noexcept
{
    return identity.source == CleanupIdentitySource::FileId128 &&
           identity.volumeSerial != 0 && isNonZero(identity.fileId128);
}

bool isIdentityAvailable(const CleanupIdentity& identity) noexcept
{
    if (identity.source == CleanupIdentitySource::FileId128) {
        return isStrongIdentity(identity);
    }
    return identity.source == CleanupIdentitySource::FileIndex64Fallback &&
           identity.volumeSerial != 0 && identity.fileIndex64 != 0;
}

bool identitiesEqual(const CleanupIdentity& a, const CleanupIdentity& b) noexcept
{
    if (!isIdentityAvailable(a) || !isIdentityAvailable(b) ||
        a.source != b.source || a.volumeSerial != b.volumeSerial) {
        return false;
    }
    if (a.source == CleanupIdentitySource::FileId128) {
        return a.fileId128 == b.fileId128;
    }
    return a.fileIndex64 == b.fileIndex64;
}

bool operator==(const CleanupIdentity& a, const CleanupIdentity& b) noexcept
{
    return identitiesEqual(a, b);
}

bool operator!=(const CleanupIdentity& a, const CleanupIdentity& b) noexcept
{
    return !identitiesEqual(a, b);
}

const char* toString(CleanupEvidenceScope scope) noexcept
{
    switch (scope) {
    case CleanupEvidenceScope::Direct:
        return "Direct";
    case CleanupEvidenceScope::Recursive:
        return "Recursive";
    }
    return "Direct";
}

const char* toString(CleanupValidationState state) noexcept
{
    switch (state) {
    case CleanupValidationState::NotValidated:
        return "NotValidated";
    case CleanupValidationState::Unchanged:
        return "Unchanged";
    case CleanupValidationState::Missing:
        return "Missing";
    case CleanupValidationState::TypeChanged:
        return "TypeChanged";
    case CleanupValidationState::IdentityChanged:
        return "IdentityChanged";
    case CleanupValidationState::IdentityUnavailable:
        return "IdentityUnavailable";
    case CleanupValidationState::Protected:
        return "Protected";
    case CleanupValidationState::Changed:
        return "Changed";
    case CleanupValidationState::DirectUnchangedRecursiveNotRevalidated:
        return "DirectUnchangedRecursiveNotRevalidated";
    case CleanupValidationState::AccessDenied:
        return "AccessDenied";
    case CleanupValidationState::ProbeError:
        return "ProbeError";
    }
    return "NotValidated";
}

const char* toString(CleanupObservation observation) noexcept
{
    switch (observation) {
    case CleanupObservation::Unobserved:
        return "Unobserved";
    case CleanupObservation::Present:
        return "Present";
    case CleanupObservation::Missing:
        return "Missing";
    case CleanupObservation::AccessDenied:
        return "AccessDenied";
    case CleanupObservation::ProbeError:
        return "ProbeError";
    }
    return "Unobserved";
}

const char* toString(CleanupValidationReason reason) noexcept
{
    switch (reason) {
    case CleanupValidationReason::None:
        return "None";
    case CleanupValidationReason::NoCurrentEvidence:
        return "NoCurrentEvidence";
    case CleanupValidationReason::Missing:
        return "Missing";
    case CleanupValidationReason::TypeChanged:
        return "TypeChanged";
    case CleanupValidationReason::IdentityChanged:
        return "IdentityChanged";
    case CleanupValidationReason::IdentityUnavailable:
        return "IdentityUnavailable";
    case CleanupValidationReason::IdentityFormChanged:
        return "IdentityFormChanged";
    case CleanupValidationReason::LogicalSizeChanged:
        return "LogicalSizeChanged";
    case CleanupValidationReason::LastWriteChanged:
        return "LastWriteChanged";
    case CleanupValidationReason::LastAccessChanged:
        return "LastAccessChanged";
    case CleanupValidationReason::AttributesChanged:
        return "AttributesChanged";
    case CleanupValidationReason::SafetyChanged:
        return "SafetyChanged";
    case CleanupValidationReason::ProtectedLocation:
        return "ProtectedLocation";
    case CleanupValidationReason::RecursiveChanged:
        return "RecursiveChanged";
    case CleanupValidationReason::RecursiveNotRevalidated:
        return "RecursiveNotRevalidated";
    case CleanupValidationReason::AccessDenied:
        return "AccessDenied";
    case CleanupValidationReason::ProbeError:
        return "ProbeError";
    }
    return "None";
}

std::vector<std::string> validationReasonNames(CleanupValidationReason reasons)
{
    constexpr CleanupValidationReason kReasons[] = {
        CleanupValidationReason::NoCurrentEvidence,
        CleanupValidationReason::Missing,
        CleanupValidationReason::TypeChanged,
        CleanupValidationReason::IdentityChanged,
        CleanupValidationReason::IdentityUnavailable,
        CleanupValidationReason::IdentityFormChanged,
        CleanupValidationReason::LogicalSizeChanged,
        CleanupValidationReason::LastWriteChanged,
        CleanupValidationReason::LastAccessChanged,
        CleanupValidationReason::AttributesChanged,
        CleanupValidationReason::SafetyChanged,
        CleanupValidationReason::ProtectedLocation,
        CleanupValidationReason::RecursiveChanged,
        CleanupValidationReason::RecursiveNotRevalidated,
        CleanupValidationReason::AccessDenied,
        CleanupValidationReason::ProbeError,
    };
    std::vector<std::string> out;
    for (const auto reason : kReasons) {
        if (hasValidationReason(reasons, reason)) {
            out.emplace_back(toString(reason));
        }
    }
    if (out.empty() && reasons == CleanupValidationReason::None) {
        out.emplace_back(toString(CleanupValidationReason::None));
    }
    return out;
}

const char* toString(CleanupValidationDiffKind kind) noexcept
{
    switch (kind) {
    case CleanupValidationDiffKind::Type:
        return "Type";
    case CleanupValidationDiffKind::Identity:
        return "Identity";
    case CleanupValidationDiffKind::LogicalSize:
        return "LogicalSize";
    case CleanupValidationDiffKind::LastWriteTime:
        return "LastWriteTime";
    case CleanupValidationDiffKind::LastAccessTime:
        return "LastAccessTime";
    case CleanupValidationDiffKind::Attributes:
        return "Attributes";
    case CleanupValidationDiffKind::Safety:
        return "Safety";
    case CleanupValidationDiffKind::RecursiveLogicalSize:
        return "RecursiveLogicalSize";
    case CleanupValidationDiffKind::RecursiveNewestDescendantWrite:
        return "RecursiveNewestDescendantWrite";
    }
    return "LogicalSize";
}

const char* toString(CleanupAddResult result) noexcept
{
    switch (result) {
    case CleanupAddResult::Added:
        return "Added";
    case CleanupAddResult::DuplicateUpdated:
        return "DuplicateUpdated";
    case CleanupAddResult::IdentityConflict:
        return "IdentityConflict";
    case CleanupAddResult::Invalid:
        return "Invalid";
    }
    return "Invalid";
}

std::wstring normalizeCleanupPath(std::wstring_view path)
{
    std::wstring out;
    out.reserve(path.size());
    for (const wchar_t ch : path) {
        out.push_back(ch == L'/' ? L'\\'
                                 : static_cast<wchar_t>(std::towlower(ch)));
    }

    const bool driveRoot = out.size() >= 2 && isDriveLetter(out[0]) &&
                           out[1] == L':' &&
                           out.find_first_not_of(L'\\', 2) == std::wstring::npos;
    if (driveRoot) {
        return std::wstring{out[0], L':', L'\\'};
    }
    while (out.size() > 1 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

bool isPathAncestorOrEqual(std::wstring_view ancestor, std::wstring_view path)
{
    const auto a = normalizeCleanupPath(ancestor);
    const auto p = normalizeCleanupPath(path);
    if (a.empty() || p.empty() || p.size() < a.size() ||
        p.compare(0, a.size(), a) != 0) {
        return false;
    }
    return p.size() == a.size() || a.back() == L'\\' || p[a.size()] == L'\\';
}

bool isStrictPathAncestor(std::wstring_view ancestor, std::wstring_view path)
{
    const auto a = normalizeCleanupPath(ancestor);
    const auto p = normalizeCleanupPath(path);
    return a != p && isPathAncestorOrEqual(a, p);
}

CleanupIdentity identityOf(const CleanupCandidate& candidate) noexcept
{
    return candidate.objectEvidence.identity;
}

CleanupObjectEvidence objectEvidenceOf(const CleanupCandidate& candidate) noexcept
{
    if (candidate.objectEvidence.available) {
        return candidate.objectEvidence;
    }
    CleanupObjectEvidence out;
    out.available = true;
    out.kind = candidate.kind;
    out.sizeScope = CleanupEvidenceScope::Direct;
    out.logicalSize = candidate.kind == ItemKind::File
                          ? candidate.sizeAtSelection
                          : 0;
    out.lastWriteTime = candidate.lastWriteTime;
    out.attributes = candidate.attributes;
    return out;
}

CleanupDirectoryAggregateEvidence historicalAggregateOf(
    const CleanupCandidate& candidate) noexcept
{
    return candidate.historicalDirectoryAggregate;
}

bool refreshCapturedEvidence(CleanupCandidate& candidate)
{
    const auto& current = candidate.currentEvidence;
    if (!current.available || !current.exists ||
        !current.objectEvidence.available) {
        return false;
    }

    const auto& present = current.objectEvidence;
    candidate.kind = present.kind;
    candidate.objectEvidence = present;
    candidate.objectEvidence.available = true;
    candidate.capturedSafety = current.safety;

    if (candidate.kind == ItemKind::File) {
        candidate.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
        candidate.sizeAtSelection = present.logicalSize;
        candidate.lastWriteTime = present.lastWriteTime;
        candidate.attributes = present.attributes;
    } else {
        // Direct object metadata only. Do not invent a recursive logical size
        // from handle metadata; historical aggregate stays historical.
        candidate.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
        if (present.sizeScope == CleanupEvidenceScope::Recursive) {
            candidate.objectEvidence.logicalSize = 0;
            candidate.objectEvidence.lastWriteTime = 0;
        }
        candidate.historicalDirectoryAggregate.revalidated = false;
        candidate.attributes = present.attributes;
    }

    candidate.validation = validateCleanupCandidate(candidate, current);
    return true;
}

CleanupValidation validateCleanupCandidate(const CleanupCandidate& candidate,
                                           const CleanupCurrentEvidence& current)
{
    CleanupValidation out;
    if (current.observation == CleanupObservation::AccessDenied) {
        out.state = CleanupValidationState::AccessDenied;
        out.reasons |= CleanupValidationReason::AccessDenied;
        out.reasons |= CleanupValidationReason::NoCurrentEvidence;
        return out;
    }
    if (current.observation == CleanupObservation::ProbeError) {
        out.state = CleanupValidationState::ProbeError;
        out.reasons |= CleanupValidationReason::ProbeError;
        out.reasons |= CleanupValidationReason::NoCurrentEvidence;
        return out;
    }
    if (!current.available) {
        out.reasons |= CleanupValidationReason::NoCurrentEvidence;
        return out;
    }
    if (!current.exists) {
        out.state = CleanupValidationState::Missing;
        out.reasons |= CleanupValidationReason::Missing;
        return out;
    }

    const auto captured = objectEvidenceOf(candidate);
    const auto& present = current.objectEvidence;
    if (!present.available) {
        out.reasons |= CleanupValidationReason::NoCurrentEvidence;
    } else {
        if (captured.kind != present.kind) {
            out.reasons |= CleanupValidationReason::TypeChanged;
            out.diffs.push_back({CleanupValidationDiffKind::Type,
                                 toString(captured.kind), toString(present.kind)});
        }
        if (isIdentityAvailable(captured.identity) &&
            isIdentityAvailable(present.identity)) {
            if (identitiesEqual(captured.identity, present.identity)) {
                out.objectIdentityMatched = true;
            } else {
                out.reasons |= CleanupValidationReason::IdentityChanged;
                if (identityStrength(captured.identity) !=
                    identityStrength(present.identity)) {
                    out.reasons |= CleanupValidationReason::IdentityFormChanged;
                }
                out.diffs.push_back({CleanupValidationDiffKind::Identity,
                                     identityText(captured.identity),
                                     identityText(present.identity)});
            }
        } else {
            out.reasons |= CleanupValidationReason::IdentityUnavailable;
            out.diffs.push_back({CleanupValidationDiffKind::Identity,
                                 identityText(captured.identity),
                                 identityText(present.identity)});
        }

        if (captured.sizeScope == present.sizeScope &&
            captured.logicalSize != present.logicalSize) {
            out.reasons |= CleanupValidationReason::LogicalSizeChanged;
            out.diffs.push_back({CleanupValidationDiffKind::LogicalSize,
                                 numberString(captured.logicalSize),
                                 numberString(present.logicalSize)});
        }
        if (captured.lastWriteTime != 0 && present.lastWriteTime != 0 &&
            captured.lastWriteTime != present.lastWriteTime) {
            out.reasons |= CleanupValidationReason::LastWriteChanged;
            out.diffs.push_back({CleanupValidationDiffKind::LastWriteTime,
                                 numberString(captured.lastWriteTime),
                                 numberString(present.lastWriteTime)});
        }
        if (captured.lastAccessTime != 0 && present.lastAccessTime != 0 &&
            captured.lastAccessTime != present.lastAccessTime) {
            out.reasons |= CleanupValidationReason::LastAccessChanged;
            out.diffs.push_back({CleanupValidationDiffKind::LastAccessTime,
                                 numberString(captured.lastAccessTime),
                                 numberString(present.lastAccessTime)});
        }
        if (captured.attributes != present.attributes) {
            out.reasons |= CleanupValidationReason::AttributesChanged;
            out.diffs.push_back({CleanupValidationDiffKind::Attributes,
                                 numberString(captured.attributes),
                                 numberString(present.attributes)});
        }
        if (out.reasons == CleanupValidationReason::None) {
            out.directMetadataUnchanged = true;
        }
    }

    if (current.safety == LocationSafety::Protected) {
        out.reasons |= CleanupValidationReason::ProtectedLocation;
    } else if (candidate.capturedSafety != LocationSafety::Unknown &&
               current.safety != LocationSafety::Unknown &&
               candidate.capturedSafety != current.safety) {
        out.reasons |= CleanupValidationReason::SafetyChanged;
        out.diffs.push_back({CleanupValidationDiffKind::Safety,
                             toString(candidate.capturedSafety),
                             toString(current.safety)});
    }

    if (candidate.kind != ItemKind::File) {
        const auto& historical = candidate.historicalDirectoryAggregate;
        if (!current.directoryAggregate.available ||
            !current.directoryAggregate.revalidated) {
            out.reasons |= CleanupValidationReason::RecursiveNotRevalidated;
        } else {
            out.recursiveEvidenceRevalidated = true;
            if (historical.available &&
                historical.recursiveLogicalSize !=
                    current.directoryAggregate.recursiveLogicalSize) {
                out.reasons |= CleanupValidationReason::RecursiveChanged;
                out.diffs.push_back(
                    {CleanupValidationDiffKind::RecursiveLogicalSize,
                     numberString(historical.recursiveLogicalSize),
                     numberString(current.directoryAggregate.recursiveLogicalSize)});
            }
            if (historical.available &&
                historical.newestDescendantWrite != 0 &&
                current.directoryAggregate.newestDescendantWrite != 0 &&
                historical.newestDescendantWrite !=
                    current.directoryAggregate.newestDescendantWrite) {
                out.reasons |= CleanupValidationReason::RecursiveChanged;
                out.diffs.push_back(
                    {CleanupValidationDiffKind::RecursiveNewestDescendantWrite,
                     numberString(historical.newestDescendantWrite),
                     numberString(current.directoryAggregate.newestDescendantWrite)});
            }
        }
    }

    if (hasValidationReason(out.reasons, CleanupValidationReason::Missing)) {
        out.state = CleanupValidationState::Missing;
    } else if (hasValidationReason(out.reasons,
                                   CleanupValidationReason::TypeChanged)) {
        out.state = CleanupValidationState::TypeChanged;
    } else if (hasValidationReason(out.reasons,
                                   CleanupValidationReason::IdentityChanged)) {
        out.state = CleanupValidationState::IdentityChanged;
    } else if (hasValidationReason(
                   out.reasons,
                   CleanupValidationReason::IdentityUnavailable)) {
        out.state = CleanupValidationState::IdentityUnavailable;
    } else if (hasValidationReason(out.reasons,
                                   CleanupValidationReason::ProtectedLocation)) {
        out.state = CleanupValidationState::Protected;
    } else if (hasValidationReason(
                   out.reasons,
                   CleanupValidationReason::RecursiveNotRevalidated) &&
               (out.reasons & ~CleanupValidationReason::RecursiveNotRevalidated) ==
                   CleanupValidationReason::None) {
        out.state = CleanupValidationState::DirectUnchangedRecursiveNotRevalidated;
    } else if (out.reasons == CleanupValidationReason::None) {
        out.state = CleanupValidationState::Unchanged;
    } else {
        out.state = CleanupValidationState::Changed;
    }
    return out;
}

std::vector<CleanupCandidate>::iterator CleanupReview::findIt(
    std::wstring_view path)
{
    const auto key = normalizeCleanupPath(path);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (normalizeCleanupPath(it->path) == key) {
            return it;
        }
    }
    return m_items.end();
}

std::vector<CleanupCandidate>::const_iterator CleanupReview::findIt(
    std::wstring_view path) const
{
    const auto key = normalizeCleanupPath(path);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (normalizeCleanupPath(it->path) == key) {
            return it;
        }
    }
    return m_items.end();
}

CleanupAddOutcome CleanupReview::addDetailed(CleanupCandidate candidate)
{
    if (candidate.path.empty()) {
        return {CleanupAddResult::Invalid, 0, 0};
    }
    normalizeCandidate(candidate);

    // Same strong identity and kind is the same review object, even if a
    // different-kind identity conflict is already present.
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (sameStrongIdentity(*it, candidate) && sameKind(*it, candidate)) {
            const auto id = it->id;
            replaceKeepingId(*it, std::move(candidate));
            return {CleanupAddResult::DuplicateUpdated, id, 0};
        }
    }
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (sameStrongIdentity(*it, candidate) && !sameKind(*it, candidate)) {
            const auto id = m_nextId++;
            const auto conflictingId = it->id;
            candidate.id = id;
            m_items.push_back(std::move(candidate));
            return {CleanupAddResult::IdentityConflict, id, conflictingId};
        }
    }

    const auto candidateKey = normalizeCleanupPath(candidate.path);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (normalizeCleanupPath(it->path) != candidateKey) {
            continue;
        }
        if (!sameKind(*it, candidate)) {
            continue;
        }
        if (differentStrongIdentity(*it, candidate) ||
            isStrongIdentity(identityOf(*it)) !=
                isStrongIdentity(identityOf(candidate))) {
            const auto id = m_nextId++;
            const auto conflictingId = it->id;
            candidate.id = id;
            m_items.push_back(std::move(candidate));
            return {CleanupAddResult::IdentityConflict, id, conflictingId};
        }
        const auto id = it->id;
        replaceKeepingId(*it, std::move(candidate));
        return {CleanupAddResult::DuplicateUpdated, id, 0};
    }

    candidate.id = m_nextId++;
    m_items.push_back(std::move(candidate));
    return {CleanupAddResult::Added, m_items.back().id, 0};
}

std::uint64_t CleanupReview::add(CleanupCandidate candidate)
{
    return addDetailed(std::move(candidate)).id;
}

bool CleanupReview::removeById(std::uint64_t id)
{
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it->id == id) {
            m_items.erase(it);
            return true;
        }
    }
    return false;
}

bool CleanupReview::removeByPath(std::wstring_view path)
{
    const auto key = normalizeCleanupPath(path);
    const auto before = m_items.size();
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                 [&](const CleanupCandidate& item) {
                                     return normalizeCleanupPath(item.path) ==
                                            key;
                                 }),
                  m_items.end());
    return m_items.size() != before;
}

void CleanupReview::clear() noexcept
{
    m_items.clear();
}

bool CleanupReview::replaceValidation(std::uint64_t id,
                                      CleanupCurrentEvidence current,
                                      FileTimeTicks checkedAt)
{
    for (auto& item : m_items) {
        if (item.id != id) {
            continue;
        }
        item.currentEvidence = std::move(current);
        item.validation =
            validateCleanupCandidate(item, item.currentEvidence);
        item.validationCheckedAt = checkedAt;
        return true;
    }
    return false;
}

bool CleanupReview::replaceValidationBatch(
    const std::vector<CleanupValidationReplacement>& updates)
{
    if (updates.empty()) {
        return true;
    }
    for (const auto& update : updates) {
        bool found = false;
        for (const auto& item : m_items) {
            if (item.id == update.id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    for (const auto& update : updates) {
        if (!replaceValidation(update.id, update.current, update.checkedAt)) {
            return false;
        }
    }
    return true;
}

bool CleanupReview::refreshEvidence(std::uint64_t id)
{
    for (auto& item : m_items) {
        if (item.id == id) {
            return refreshCapturedEvidence(item);
        }
    }
    return false;
}

void CleanupReview::resetTo(std::vector<CleanupCandidate> items,
                            std::uint64_t nextId)
{
    std::uint64_t maxId = 0;
    for (const auto& item : items) {
        if (item.id > maxId) {
            maxId = item.id;
        }
    }
    m_items = std::move(items);
    m_nextId = nextId;
    if (maxId + 1 > m_nextId) {
        m_nextId = maxId + 1;
    }
    if (m_nextId == 0) {
        m_nextId = 1;
    }
}

bool CleanupReview::containsPath(std::wstring_view path) const
{
    return findIt(path) != m_items.end();
}

std::optional<CleanupCandidate> CleanupReview::findById(std::uint64_t id) const
{
    for (const auto& item : m_items) {
        if (item.id == id) {
            return item;
        }
    }
    return std::nullopt;
}

std::optional<CleanupCandidate> CleanupReview::findByPath(
    std::wstring_view path) const
{
    auto it = findIt(path);
    if (it == m_items.end()) {
        return std::nullopt;
    }
    return *it;
}

ByteSize CleanupReview::rawTotalLogicalSize() const noexcept
{
    ByteSize total = 0;
    for (const auto& item : m_items) {
        const ByteSize value = item.kind == ItemKind::File
                                   ? objectEvidenceOf(item).logicalSize
                                   : historicalAggregateOf(item).available
                                         ? historicalAggregateOf(item)
                                               .recursiveLogicalSize
                                         : objectEvidenceOf(item).logicalSize;
        if (std::numeric_limits<ByteSize>::max() - total < value) {
            return std::numeric_limits<ByteSize>::max();
        }
        total += value;
    }
    return total;
}

ByteSize CleanupReview::totalLogicalSize() const noexcept
{
    return buildCleanupPlan(*this).summary.uniqueLogicalBytes;
}

std::string CleanupReview::copyReport() const
{
    return buildCleanupPlan(*this).toText();
}

}  // namespace spacelens
