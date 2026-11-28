#pragma once

#include "Game/EntityDiscovery/DiscoveryContract.hpp"
#include "Game/EntityDiscovery/PositiveCandidates.hpp"
#include "Game/Offsets.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OW::EntityDiscovery {

struct RawEntitySlot {
    uint64_t parent = 0;
    uint32_t generation = 0;
    int32_t marker = 0;

    bool operator==(const RawEntitySlot&) const noexcept = default;
    bool MarkerIsNegOne() const noexcept { return marker == -1; }
};

struct PassiveMarkerTelemetry {
    uint64_t sweepsCompleted = 0;
    uint64_t lastSweepSlotsScanned = 0;
    uint64_t lastSweepNonZeroPlausible = 0;
    uint64_t lastSweepMarkerNeg1 = 0;
    uint64_t totalMarkerNeg1Seen = 0;
    uint64_t markerNeg1ConfirmedActors = 0;
    uint64_t markerNeg1ConfirmedWorldObjects = 0;
};

struct IncrementalSlotDiffStats {
    uint64_t blocksObserved = 0;
    uint64_t sweepsCompleted = 0;
    uint64_t sweepsAborted = 0;
    uint64_t slotsAdded = 0;
    uint64_t slotsGenerationChanged = 0;
    uint64_t slotsMarkerChanged = 0;
    uint64_t slotsRemovedOnComplete = 0;
    uint64_t headerDirtyReads = 0;
    uint64_t headerRollingRechecks = 0;
    uint64_t fingerprintChanges = 0;
    PassiveMarkerTelemetry markerTelemetry{};
};

// Unified incremental 16-byte entity-table slot-diff discoverer.
// Keeps 16B slots {parent, generation, marker} + (ActiveProfile, objectKind, componentFingerprint),
// explicitly separates LinkedActor / NoLinkActor / WorldObject, keeps rolling low-rate recheck
// for in-place reuse, and strictly preserves partial-scan non-deletion.
class IncrementalSlotDiff {
public:
    using Fingerprint = PositiveCandidates::Fingerprint;

    struct TrackedEntry {
        RawEntitySlot slot{};
        OW::offset::RuntimeProfile profile = OW::offset::RuntimeProfile::Cn;
        DiscoveredObjectKind objectKind = DiscoveredObjectKind::Unknown;
        Fingerprint fingerprint{};
        uint64_t poolId = 0;
        uint64_t plus8 = 0;
        uint32_t wrapperId = 0;
        uint32_t lastHeaderTick = 0;
        uint32_t lastAttemptTick = 0;
        uint8_t failStreak = 0;
        bool poolRead = false;
        bool headerValid = false;
        bool headerDirty = false;
        bool accepted = false;
        bool markerWasNeg1 = false;
        bool markerConfirmedLogged = false;
    };

    struct HeaderRequest {
        uint64_t parent = 0;
        bool isRollingRecheck = false;
    };

    static constexpr uint32_t RetryBackoffMs(uint8_t failStreak) noexcept {
        return PositiveCandidates::RetryBackoffMs(failStreak);
    }

    void ResetForContext(uint64_t epoch, uint64_t scene, uint64_t registryGen,
                         OW::offset::RuntimeProfile profile) {
        contextEpoch_ = epoch;
        contextScene_ = scene;
        contextRegistryGen_ = registryGen;
        contextProfile_ = profile;
        baselineSlots_.clear();
        members_.clear();
        entries_.clear();
        urgentHeaders_.clear();
        urgentHeaderSet_.clear();
        probes_.clear();
        probeSet_.clear();
        worldObjects_.clear();
        sweepInProgress_ = false;
        sweepSlots_.clear();
        sweepMembers_.clear();
        sweepScannedSlots_ = 0;
        sweepNonZeroPlausible_ = 0;
        sweepMarkerNeg1_ = 0;
        rollingCursor_ = 0;
    }

    bool MatchesContext(uint64_t epoch, uint64_t scene, uint64_t registryGen,
                        OW::offset::RuntimeProfile profile) const noexcept {
        return contextEpoch_ == epoch &&
            contextScene_ == scene &&
            contextRegistryGen_ == registryGen &&
            contextProfile_ == profile;
    }

    void BeginTableSweep(size_t capacitySlots) {
        sweepInProgress_ = true;
        sweepSlots_.assign(capacitySlots, RawEntitySlot{});
        sweepMembers_.clear();
        sweepMembers_.reserve((std::min)(capacitySlots, size_t{8192}));
        sweepScannedSlots_ = 0;
        sweepNonZeroPlausible_ = 0;
        sweepMarkerNeg1_ = 0;
    }

    // Observe one exact table chunk [byteOffset .. byteOffset + byteCount).
    // Additions and changed slots become urgent immediately; unseen old members
    // are NEVER deleted here (partial-scan non-deletion).
    template <class IsPlausibleParent>
    std::vector<uint64_t> ObserveTableBlock(
        uint64_t byteOffset,
        const uint8_t* bytes,
        size_t byteCount,
        IsPlausibleParent&& isPlausibleParent)
    {
        std::vector<uint64_t> blockPlausibleParents;
        if (!bytes || byteCount < 16)
            return blockPlausibleParents;
        ++stats_.blocksObserved;

        const size_t firstSlot = static_cast<size_t>(byteOffset / 16ULL);
        const size_t slotCount = byteCount / 16U;
        if (sweepInProgress_ && firstSlot + slotCount > sweepSlots_.size())
            sweepSlots_.resize(firstSlot + slotCount);

        bool membersAdded = false;
        for (size_t i = 0; i < slotCount; ++i) {
            RawEntitySlot current{};
            const uint8_t* slotBytes = bytes + i * 16U;
            std::memcpy(&current.parent, slotBytes, 8);
            std::memcpy(&current.generation, slotBytes + 8, 4);
            std::memcpy(&current.marker, slotBytes + 12, 4);

            const size_t slotIdx = firstSlot + i;
            ++sweepScannedSlots_;

            const bool plausible = isPlausibleParent(current.parent);
            if (!plausible) {
                if (sweepInProgress_ && slotIdx < sweepSlots_.size())
                    sweepSlots_[slotIdx] = RawEntitySlot{};
                continue;
            }

            ++sweepNonZeroPlausible_;
            const bool markerNeg1 = current.MarkerIsNegOne();
            if (markerNeg1) {
                ++sweepMarkerNeg1_;
                ++stats_.markerTelemetry.totalMarkerNeg1Seen;
            }
            // IMPORTANT: Passive marker == -1 telemetry ONLY. Do NOT prune marker == -1 slots!

            if (sweepInProgress_ && slotIdx < sweepSlots_.size())
                sweepSlots_[slotIdx] = current;
            if (sweepInProgress_)
                sweepMembers_.push_back(current.parent);
            blockPlausibleParents.push_back(current.parent);

            const RawEntitySlot prevSlot =
                slotIdx < baselineSlots_.size() ? baselineSlots_[slotIdx] : RawEntitySlot{};
            auto entryIt = entries_.find(current.parent);
            const bool isNewParent = entryIt == entries_.end();
            const bool genChanged = !isNewParent && entryIt->second.slot.generation != current.generation;
            const bool markerChanged = !isNewParent && entryIt->second.slot.marker != current.marker;
            const bool slotDiffers = isNewParent || prevSlot != current || genChanged || markerChanged;

            if (isNewParent) {
                ++stats_.slotsAdded;
                TrackedEntry fresh{};
                fresh.slot = current;
                fresh.profile = contextProfile_;
                fresh.headerDirty = true;
                fresh.markerWasNeg1 = markerNeg1;
                entries_.emplace(current.parent, std::move(fresh));
                members_.push_back(current.parent);
                membersAdded = true;
                EnqueueUrgentHeader(current.parent);
            } else {
                auto& tracked = entryIt->second;
                if (genChanged) ++stats_.slotsGenerationChanged;
                if (markerChanged) ++stats_.slotsMarkerChanged;
                const bool profileChanged = tracked.profile != contextProfile_;
                tracked.slot = current;
                tracked.markerWasNeg1 = tracked.markerWasNeg1 || markerNeg1;
                if (slotDiffers || !tracked.headerValid || profileChanged) {
                    tracked.profile = contextProfile_;
                    tracked.headerDirty = true;
                    tracked.failStreak = 0;
                    tracked.accepted = false;
                    tracked.lastAttemptTick = 0;
                    if (slotDiffers || profileChanged) {
                        tracked.poolRead = false;
                        tracked.poolId = 0;
                        tracked.objectKind = DiscoveredObjectKind::Unknown;
                        worldObjects_.erase(current.parent);
                    }
                    EnqueueUrgentHeader(current.parent);
                }
            }
        }

        if (membersAdded) {
            std::sort(members_.begin(), members_.end());
            members_.erase(std::unique(members_.begin(), members_.end()), members_.end());
        }
        return blockPlausibleParents;
    }

    // Abort an incomplete table sweep without erasing any baseline members.
    void AbortTableSweep() {
        if (!sweepInProgress_)
            return;
        sweepInProgress_ = false;
        sweepSlots_.clear();
        sweepMembers_.clear();
        ++stats_.sweepsAborted;
    }

    // Commit a verified COMPLETE table sweep. Only here may disappeared parents be retired.
    void CommitCompletedTableSweep() {
        if (!sweepInProgress_)
            return;
        sweepInProgress_ = false;
        std::sort(sweepMembers_.begin(), sweepMembers_.end());
        sweepMembers_.erase(std::unique(sweepMembers_.begin(), sweepMembers_.end()), sweepMembers_.end());

        size_t removed = 0;
        std::erase_if(entries_, [&](const auto& kv) {
            const bool keep = std::binary_search(sweepMembers_.begin(), sweepMembers_.end(), kv.first);
            if (!keep) {
                worldObjects_.erase(kv.first);
                ++removed;
            }
            return !keep;
        });

        stats_.slotsRemovedOnComplete += removed;
        baselineSlots_ = std::move(sweepSlots_);
        members_ = std::move(sweepMembers_);
        if (rollingCursor_ >= members_.size())
            rollingCursor_ = 0;

        // Re-enqueue any member that still lacks a valid clean header once per completed sweep,
        // so transient short reads or aborted header batches recover on the next sweep without livelocking.
        for (uint64_t parent : members_) {
            const auto it = entries_.find(parent);
            if (it != entries_.end() && (!it->second.headerValid || it->second.headerDirty))
                EnqueueUrgentHeader(parent);
        }

        ++stats_.sweepsCompleted;
        ++stats_.markerTelemetry.sweepsCompleted;
        stats_.markerTelemetry.lastSweepSlotsScanned = sweepScannedSlots_;
        stats_.markerTelemetry.lastSweepNonZeroPlausible = sweepNonZeroPlausible_;
        stats_.markerTelemetry.lastSweepMarkerNeg1 = sweepMarkerNeg1_;
    }

    // Select up to `limit` parents for the next unified header batch:
    // dirty/urgent slots first, then up to `rollingBudget` unchanged slots for
    // low-rate in-place allocation reuse detection (e.g. same-address hero switch/respawn).
    std::vector<HeaderRequest> NextHeaderBatch(size_t limit, size_t rollingBudget) {
        std::vector<HeaderRequest> batch;
        batch.reserve(limit);
        std::unordered_set<uint64_t> picked;
        picked.reserve(limit * 2 + 8);

        while (!urgentHeaders_.empty() && batch.size() < limit) {
            const uint64_t parent = urgentHeaders_.front();
            urgentHeaders_.pop_front();
            urgentHeaderSet_.erase(parent);
            if (!Contains(parent))
                continue;
            if (picked.insert(parent).second) {
                batch.push_back({ parent, false });
                ++stats_.headerDirtyReads;
            }
        }

        size_t rollingAdded = 0;
        for (size_t visited = 0;
             visited < members_.size() && batch.size() < limit && rollingAdded < rollingBudget;
             ++visited) {
            const uint64_t parent = members_[rollingCursor_++ % members_.size()];
            if (picked.insert(parent).second) {
                batch.push_back({ parent, true });
                ++rollingAdded;
                ++stats_.headerRollingRechecks;
            }
        }
        return batch;
    }

    // Observe header fingerprint and optional pool ID classification.
    // Explicitly separates WorldObject, LinkedActor, NoLinkActor, and ordinary Unknown objects.
    template <class IsWorldObjectId>
    void ObserveHeader(
        uint64_t parent,
        const Fingerprint& fingerprint,
        bool headerExact,
        uint64_t poolId,
        bool poolRead,
        uint64_t plus8,
        uint32_t wrapperId,
        uint32_t now,
        uint32_t linkType,
        uint32_t heroType,
        uint32_t healthType,
        IsWorldObjectId&& isWorldObjectId,
        bool hasPoolPtr = false)
    {
        auto it = entries_.find(parent);
        if (it == entries_.end())
            return;
        auto& entry = it->second;
        if (!headerExact) {
            entry.headerValid = false;
            entry.headerDirty = true;
            entry.lastHeaderTick = 0;
            return;
        }

        const bool fingerprintChanged = !entry.headerValid || entry.fingerprint != fingerprint;
        if (fingerprintChanged) {
            if (entry.headerValid)
                ++stats_.fingerprintChanges;
            entry.fingerprint = fingerprint;
            entry.headerValid = true;
            entry.accepted = false;
            entry.lastAttemptTick = 0;
            entry.failStreak = 0;
            entry.objectKind = DiscoveredObjectKind::Unknown;
            entry.poolRead = false;
            entry.poolId = 0;
            worldObjects_.erase(parent);
        }
        const bool effectiveHasPoolPtr = hasPoolPtr || poolRead;
        if (!effectiveHasPoolPtr) {
            entry.poolRead = false;
            entry.poolId = 0;
        }
        entry.profile = contextProfile_;
        entry.lastHeaderTick = now;
        entry.plus8 = plus8;
        entry.wrapperId = wrapperId;
        if (poolRead) {
            entry.poolId = poolId;
            entry.poolRead = true;
        }
        if (effectiveHasPoolPtr && !poolRead && !entry.poolRead) {
            // Needed Wave 2 pool read failed: keep headerDirty = true so CachedMemberIds
            // stays incomplete and stale/unverified WorldObject status cannot leak into roster_complete = 1.
            entry.headerDirty = true;
            entry.objectKind = DiscoveredObjectKind::Unknown;
            worldObjects_.erase(parent);
            return;
        }
        entry.headerDirty = false;

        // Classify explicitly: WorldObject > LinkedActor > NoLinkActor > Unknown.
        const bool isWorldObj = entry.poolRead && isWorldObjectId(entry.poolId);
        if (isWorldObj) {
            entry.objectKind = DiscoveredObjectKind::WorldObject;
            worldObjects_.insert(parent);
            if (entry.markerWasNeg1 && !entry.markerConfirmedLogged) {
                entry.markerConfirmedLogged = true;
                ++stats_.markerTelemetry.markerNeg1ConfirmedWorldObjects;
            }
            return;
        }
        worldObjects_.erase(parent);

        const bool hasLink = fingerprint.Has(linkType);
        const bool hasHero = fingerprint.Has(heroType);
        const bool hasHealth = healthType == 0 || fingerprint.Has(healthType);

        if (hasLink) {
            entry.objectKind = DiscoveredObjectKind::LinkedActor;
        } else if (hasHero && hasHealth) {
            entry.objectKind = DiscoveredObjectKind::NoLinkActor;
        } else {
            entry.objectKind = DiscoveredObjectKind::Unknown;
            return;
        }

        const uint32_t retryMs = RetryBackoffMs(entry.failStreak);
        if (fingerprint.match != 0 && !entry.accepted &&
            (!entry.lastAttemptTick || uint32_t(now - entry.lastAttemptTick) >= retryMs)) {
            EnqueueProbe(parent);
        }
    }

    uint64_t NextProbe() {
        while (!probes_.empty()) {
            const uint64_t parent = probes_.front();
            probes_.pop_front();
            probeSet_.erase(parent);
            if (Contains(parent) && !worldObjects_.contains(parent))
                return parent;
        }
        return 0;
    }

    void FinishProbe(uint64_t parent, bool accepted, uint32_t now) {
        auto it = entries_.find(parent);
        if (it == entries_.end())
            return;
        auto& entry = it->second;
        entry.lastAttemptTick = now;
        entry.accepted = accepted;
        entry.failStreak = accepted
            ? 0
            : (entry.failStreak < 4 ? static_cast<uint8_t>(entry.failStreak + 1) : uint8_t{4});
        if (accepted && entry.markerWasNeg1 && !entry.markerConfirmedLogged) {
            entry.markerConfirmedLogged = true;
            ++stats_.markerTelemetry.markerNeg1ConfirmedActors;
        }
    }

    void MarkHeaderDirty(uint64_t parent) {
        auto it = entries_.find(parent);
        if (it == entries_.end())
            return;
        it->second.headerValid = false;
        it->second.headerDirty = true;
        it->second.lastHeaderTick = 0;
        EnqueueUrgentHeader(parent);
    }

    bool Contains(uint64_t parent) const {
        return std::binary_search(members_.begin(), members_.end(), parent);
    }

    bool IsWorldObject(uint64_t parent) const {
        return worldObjects_.contains(parent);
    }

    DiscoveredObjectKind KindOf(uint64_t parent) const {
        const auto it = entries_.find(parent);
        return it == entries_.end() ? DiscoveredObjectKind::Unknown : it->second.objectKind;
    }

    const TrackedEntry* FindEntry(uint64_t parent) const {
        const auto it = entries_.find(parent);
        return it == entries_.end() ? nullptr : &it->second;
    }

    // Complete fresh member UID index (true only when every baseline member has a fresh exact header).
    bool FreshMemberIds(uint32_t now, uint32_t maxAgeMs,
                        std::vector<std::pair<uint64_t, uint32_t>>& ids) const {
        ids.clear();
        if (members_.empty())
            return false;
        ids.reserve(members_.size());
        for (uint64_t parent : members_) {
            const auto it = entries_.find(parent);
            if (it == entries_.end() || !it->second.headerValid || it->second.headerDirty || !it->second.lastHeaderTick ||
                uint32_t(now - it->second.lastHeaderTick) > maxAgeMs) {
                ids.clear();
                return false;
            }
            if (it->second.fingerprint.match != 0)
                ids.emplace_back(parent, it->second.fingerprint.match);
        }
        return true;
    }

    // Member UID index across all cached exact headers (valid for same registry generation).
    // Preserves all valid clean member UIDs even when an unrelated slot has a short header read
    // or pending dirty slot re-read, returning true only when EVERY member in members_ has a valid clean header.
    bool CachedMemberIds(std::vector<std::pair<uint64_t, uint32_t>>& ids) const {
        ids.clear();
        if (members_.empty())
            return false;
        ids.reserve(members_.size());
        bool allComplete = true;
        for (uint64_t parent : members_) {
            const auto it = entries_.find(parent);
            if (it == entries_.end() || !it->second.headerValid || it->second.headerDirty) {
                allComplete = false;
                continue;
            }
            if (it->second.fingerprint.match != 0)
                ids.emplace_back(parent, it->second.fingerprint.match);
        }
        return allComplete;
    }

    void CollectValidationCandidates(
        std::vector<uint64_t>& outLinkedActors,
        std::vector<uint64_t>& outNoLinkActors,
        std::vector<uint64_t>& outWorldObjects) const
    {
        outLinkedActors.clear();
        outNoLinkActors.clear();
        outWorldObjects.clear();
        for (uint64_t parent : members_) {
            const auto it = entries_.find(parent);
            if (it == entries_.end() || !it->second.headerValid || it->second.headerDirty)
                continue;
            switch (it->second.objectKind) {
            case DiscoveredObjectKind::LinkedActor:
                outLinkedActors.push_back(parent);
                break;
            case DiscoveredObjectKind::NoLinkActor:
                outNoLinkActors.push_back(parent);
                break;
            case DiscoveredObjectKind::WorldObject:
                outWorldObjects.push_back(parent);
                break;
            default:
                break;
            }
        }
    }

    const std::vector<uint64_t>& Members() const noexcept { return members_; }
    const std::unordered_set<uint64_t>& WorldObjects() const noexcept { return worldObjects_; }
    size_t PendingUrgentHeaders() const noexcept { return urgentHeaders_.size(); }
    size_t PendingProbes() const noexcept { return probes_.size(); }
    bool HasUnverifiedHeaders() const noexcept {
        for (uint64_t parent : members_) {
            const auto it = entries_.find(parent);
            if (it == entries_.end() || !it->second.headerValid || it->second.headerDirty)
                return true;
        }
        return false;
    }
    const IncrementalSlotDiffStats& Stats() const noexcept { return stats_; }
    OW::offset::RuntimeProfile ContextProfile() const noexcept { return contextProfile_; }

private:
    void EnqueueUrgentHeader(uint64_t parent) {
        if (urgentHeaderSet_.insert(parent).second)
            urgentHeaders_.push_back(parent);
    }

    void EnqueueProbe(uint64_t parent) {
        if (probeSet_.insert(parent).second)
            probes_.push_back(parent);
    }

    uint64_t contextEpoch_ = 0;
    uint64_t contextScene_ = 0;
    uint64_t contextRegistryGen_ = 0;
    OW::offset::RuntimeProfile contextProfile_ = OW::offset::RuntimeProfile::Cn;

    std::vector<RawEntitySlot> baselineSlots_;
    std::vector<uint64_t> members_;
    std::unordered_map<uint64_t, TrackedEntry> entries_;
    std::deque<uint64_t> urgentHeaders_;
    std::unordered_set<uint64_t> urgentHeaderSet_;
    std::deque<uint64_t> probes_;
    std::unordered_set<uint64_t> probeSet_;
    std::unordered_set<uint64_t> worldObjects_;

    bool sweepInProgress_ = false;
    std::vector<RawEntitySlot> sweepSlots_;
    std::vector<uint64_t> sweepMembers_;
    uint64_t sweepScannedSlots_ = 0;
    uint64_t sweepNonZeroPlausible_ = 0;
    uint64_t sweepMarkerNeg1_ = 0;

    size_t rollingCursor_ = 0;
    IncrementalSlotDiffStats stats_{};
};

} // namespace OW::EntityDiscovery
