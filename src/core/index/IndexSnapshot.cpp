#include "core/index/IndexSnapshot.hpp"

#include "core/index/IndexStore.hpp"

#include <cstdio>
#include <sstream>

namespace spacelens {
namespace {

void appendJsonString(std::ostringstream& os, std::string_view text)
{
    os << '"';
    for (unsigned char ch : text) {
        switch (ch) {
        case '"':
            os << "\\\"";
            break;
        case '\\':
            os << "\\\\";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            if (ch < 0x20) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                os << buf;
            } else {
                os << static_cast<char>(ch);
            }
            break;
        }
    }
    os << '"';
}

}  // namespace

const char* toString(SnapshotAgeState state) noexcept
{
    switch (state) {
    case SnapshotAgeState::Known:
        return "known";
    case SnapshotAgeState::Unknown:
        return "unknown";
    case SnapshotAgeState::ClockSkew:
        return "clock_skew";
    }
    return "unknown";
}

const char* toString(SnapshotPublishKind kind) noexcept
{
    switch (kind) {
    case SnapshotPublishKind::Full:
        return "full";
    case SnapshotPublishKind::Incremental:
        return "incremental";
    case SnapshotPublishKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* toString(IndexAgeGateResult result) noexcept
{
    switch (result) {
    case IndexAgeGateResult::NotRequested:
        return "not_requested";
    case IndexAgeGateResult::Satisfied:
        return "satisfied";
    case IndexAgeGateResult::TooOld:
        return "too_old";
    case IndexAgeGateResult::FreshnessUnknown:
        return "freshness_unknown";
    }
    return "not_requested";
}

IndexSnapshotEvidence evaluateIndexSnapshot(const IndexPublishMetadata& meta,
                                            FileTimeTicks nowTicks)
{
    IndexSnapshotEvidence ev;
    ev.basis = "published_snapshot";
    const FileTimeTicks fullTicks = meta.fullIndexedAtTicks != 0
                                        ? meta.fullIndexedAtTicks
                                        : meta.rootIndexedAtTicks;
    ev.fullIndexedAtUtc = fileTimeTicksToIsoUtc(fullTicks);

    ev.publishedAtTicks = meta.lastRefreshAtTicks != 0 ? meta.lastRefreshAtTicks
                                                       : meta.rootIndexedAtTicks;
    if (!meta.rootIndexedAtIso.empty() &&
        ev.publishedAtTicks == meta.rootIndexedAtTicks) {
        ev.publishedAtUtc = meta.rootIndexedAtIso;
    } else {
        ev.publishedAtUtc = fileTimeTicksToIsoUtc(ev.publishedAtTicks);
    }

    if (meta.lastRefreshMethod == "usn") {
        ev.publishKind = SnapshotPublishKind::Incremental;
    } else if (ev.publishedAtTicks != 0) {
        ev.publishKind = SnapshotPublishKind::Full;
    }

    if (ev.publishedAtTicks == 0 || nowTicks == 0) {
        ev.ageState = SnapshotAgeState::Unknown;
        ev.ageMs = 0;
        return ev;
    }
    if (ev.publishedAtTicks > nowTicks) {
        ev.ageState = SnapshotAgeState::ClockSkew;
        ev.ageMs = 0;
        return ev;
    }

    ev.ageState = SnapshotAgeState::Known;
    ev.ageSeconds = (nowTicks - ev.publishedAtTicks) / kFileTimeTicksPerSecond;
    ev.ageMs = (nowTicks - ev.publishedAtTicks) / 10'000ULL;
    return ev;
}

IndexAgeDecision evaluateIndexAgeGate(
    const IndexSnapshotEvidence& evidence,
    std::optional<std::uint64_t> maxAgeSeconds)
{
    IndexAgeDecision decision;
    decision.evidence = evidence;
    decision.requestedMaxAgeSeconds = maxAgeSeconds;
    if (!maxAgeSeconds.has_value()) {
        decision.result = IndexAgeGateResult::NotRequested;
        return decision;
    }
    if (evidence.ageState != SnapshotAgeState::Known || !evidence.ageSeconds) {
        decision.result = IndexAgeGateResult::FreshnessUnknown;
        return decision;
    }
    decision.result = (*evidence.ageSeconds <= *maxAgeSeconds)
                          ? IndexAgeGateResult::Satisfied
                          : IndexAgeGateResult::TooOld;
    return decision;
}

std::string indexFreshnessJsonObject(const IndexSnapshotEvidence& evidence,
                                     const IndexAgeDecision& decision)
{
    std::ostringstream os;
    os << "{\"basis\":";
    appendJsonString(os, evidence.basis);
    os << ",\"age_state\":";
    appendJsonString(os, toString(evidence.ageState));
    if (evidence.publishKind != SnapshotPublishKind::Unknown) {
        os << ",\"publish_kind\":";
        appendJsonString(os, toString(evidence.publishKind));
    }
    if (!evidence.publishedAtUtc.empty()) {
        os << ",\"published_at_utc\":";
        appendJsonString(os, evidence.publishedAtUtc);
    }
    if (evidence.ageSeconds.has_value()) {
        os << ",\"age_seconds\":" << *evidence.ageSeconds;
    }
    if (decision.requestedMaxAgeSeconds.has_value()) {
        os << ",\"max_age_seconds\":" << *decision.requestedMaxAgeSeconds;
        if (decision.result == IndexAgeGateResult::Satisfied) {
            os << ",\"max_age_satisfied\":true";
        } else if (decision.result == IndexAgeGateResult::TooOld ||
                   decision.result == IndexAgeGateResult::FreshnessUnknown) {
            os << ",\"max_age_satisfied\":false";
        }
    }
    os << '}';
    return os.str();
}

std::string formatSnapshotAgeHuman(const IndexSnapshotEvidence& evidence)
{
    if (evidence.ageState == SnapshotAgeState::Known && evidence.ageSeconds) {
        return "Index snapshot: " + std::to_string(*evidence.ageSeconds) +
               "s old";
    }
    if (evidence.ageState == SnapshotAgeState::ClockSkew) {
        return "Index snapshot: publication time is in the future (clock skew)";
    }
    return "Index snapshot: age unknown";
}

std::string formatIndexAgeGateError(const IndexAgeDecision& decision)
{
    if (decision.result == IndexAgeGateResult::TooOld &&
        decision.evidence.ageSeconds && decision.requestedMaxAgeSeconds) {
        return "Indexed snapshot is " +
               std::to_string(*decision.evidence.ageSeconds) +
               " seconds old; requested maximum is " +
               std::to_string(*decision.requestedMaxAgeSeconds) +
               " seconds. No refresh was performed.";
    }
    if (decision.result == IndexAgeGateResult::FreshnessUnknown) {
        if (decision.evidence.ageState == SnapshotAgeState::ClockSkew) {
            return "Indexed snapshot publication time is in the future "
                   "(clock skew); freshness cannot satisfy a maximum age. "
                   "No refresh was performed.";
        }
        return "Indexed snapshot age is unknown; freshness cannot satisfy a "
               "maximum age. No refresh was performed.";
    }
    return {};
}

}  // namespace spacelens
