#pragma once

#include "Game/EntityRosterPolicy.hpp"
#include "Game/Offsets.hpp"
#include "Game/SkeletonSemantics.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OW::EntityDiscovery {

enum class DiscoveredObjectKind : uint8_t {
    Unknown = 0,
    LinkedActor = 1,
    NoLinkActor = 2,
    WorldObject = 3,
};

inline const char* DiscoveredObjectKindName(DiscoveredObjectKind kind) noexcept {
    switch (kind) {
    case DiscoveredObjectKind::LinkedActor: return "linked_actor";
    case DiscoveredObjectKind::NoLinkActor: return "nolink_actor";
    case DiscoveredObjectKind::WorldObject: return "world_object";
    default: return "unknown";
    }
}

// Per-scan transaction token replacing global observed_topology_revision self-sabotage.
// An active scan tracks its own token; external/foreign mutations invalidate older
// tokens without letting the scan's own positive discoveries sabotage its completion.
struct ScanTransactionToken {
    uint64_t tokenId = 0;
    uint64_t startedForeignRevision = 0;
    uint64_t connectionEpoch = 0;
    uint64_t sceneGeneration = 0;
    uint64_t registryGeneration = 0;
    OW::offset::RuntimeProfile profile = OW::offset::RuntimeProfile::Cn;

    bool Valid() const noexcept { return tokenId != 0; }
    bool MatchesContext(uint64_t epoch, uint64_t scene, uint64_t regGen,
                        OW::offset::RuntimeProfile currentProfile) const noexcept {
        return tokenId != 0 &&
            connectionEpoch == epoch &&
            sceneGeneration == scene &&
            registryGeneration == regGen &&
            profile == currentProfile;
    }
};

// Immutable discovery delta handoff contract (pre-locked for Phase B cross-thread adoption).
struct DiscoveryDelta {
    uint64_t publishSeq = 0;
    ScanTransactionToken scanToken{};
    bool authoritativeComplete = false;
    bool bindingComplete = false;
    std::vector<std::pair<uint64_t, uint64_t>> pairs;
    std::vector<DiscoveredObjectKind> pairKinds;
    std::vector<OW::EntityRosterPolicy::Discovery> proofs;
    std::vector<uint64_t> worldObjectParents;
};

// Immutable per-actor component plan handoff contract.
// Carries per-actor monotonic commitSeq, ActiveProfile(), D.Va skeleton_form,
// and objectKind so stale or cross-profile/cross-form plans can never be reused.
struct ComponentPlan {
    uint64_t actorAddress = 0;
    uint64_t linkParent = 0;
    uint64_t commitSeq = 0;
    uint64_t connectionEpoch = 0;
    uint64_t sceneGeneration = 0;
    uint64_t registryGeneration = 0;
    OW::offset::RuntimeProfile profile = OW::offset::RuntimeProfile::Cn;
    OW::SkeletonSemantics::Form skeletonForm = OW::SkeletonSemantics::Form::Unknown;
    DiscoveredObjectKind objectKind = DiscoveredObjectKind::Unknown;
    uint32_t matchId = 0;
    uint64_t heroId = 0;
    uint64_t health = 0;
    uint64_t link = 0;
    uint64_t team = 0;
    uint64_t transform = 0;
    uint64_t velocity = 0;
    uint64_t hero = 0;
    uint64_t bone = 0;
    uint64_t rotation = 0;
    uint64_t skill = 0;
    uint64_t visibility = 0;
    uint64_t angle = 0;
    uint64_t enemyAngle = 0;
    uint64_t stat = 0;

    bool MatchesContext(uint64_t epoch,
                        uint64_t scene,
                        uint64_t regGen,
                        OW::offset::RuntimeProfile currentProfile,
                        OW::SkeletonSemantics::Form expectedForm,
                        DiscoveredObjectKind expectedKind) const noexcept {
        if (actorAddress == 0 || commitSeq == 0)
            return false;
        if (connectionEpoch != epoch || sceneGeneration != scene ||
            registryGeneration != regGen || profile != currentProfile)
            return false;
        if (expectedKind != DiscoveredObjectKind::Unknown && objectKind != expectedKind)
            return false;
        // D.Va mech/pilot isolation: once a form is established, a mismatched form cannot reuse the plan.
        if (expectedForm != OW::SkeletonSemantics::Form::Unknown &&
            skeletonForm != OW::SkeletonSemantics::Form::Unknown &&
            skeletonForm != expectedForm)
            return false;
        return true;
    }
};

// Single-writer immutable handoff store.
// Phase A+ runs on the single acquisition thread; Phase B can read LatestDelta()
// and FindPlan() from the hot thread with zero contract rework.
class SingleWriterHandoff {
public:
    void ResetForContext(uint64_t epoch, uint64_t scene, uint64_t regGen,
                         OW::offset::RuntimeProfile profile) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (contextEpoch_ == epoch && contextScene_ == scene &&
            contextRegistry_ == regGen && contextProfile_ == profile) {
            return;
        }
        contextEpoch_ = epoch;
        contextScene_ = scene;
        contextRegistry_ = regGen;
        contextProfile_ = profile;
        latestDelta_.reset();
        plans_.clear();
        actorCommitSeq_.clear();
    }

    bool PublishDelta(std::shared_ptr<const DiscoveryDelta> delta) {
        if (!delta)
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (delta->scanToken.connectionEpoch != contextEpoch_ ||
            delta->scanToken.sceneGeneration != contextScene_ ||
            delta->scanToken.registryGeneration != contextRegistry_ ||
            delta->scanToken.profile != contextProfile_) {
            return false;
        }
        if (latestDelta_ && delta->publishSeq <= latestDelta_->publishSeq)
            return false;
        latestDelta_ = std::move(delta);
        return true;
    }

    std::shared_ptr<const DiscoveryDelta> LatestDelta() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latestDelta_;
    }

    bool PublishPlan(std::shared_ptr<const ComponentPlan> plan) {
        if (!plan || plan->actorAddress == 0 || plan->commitSeq == 0)
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (plan->connectionEpoch != contextEpoch_ ||
            plan->sceneGeneration != contextScene_ ||
            plan->registryGeneration != contextRegistry_ ||
            plan->profile != contextProfile_) {
            return false;
        }
        const uint64_t previousSeq = actorCommitSeq_[plan->actorAddress];
        if (plan->commitSeq <= previousSeq)
            return false;
        actorCommitSeq_[plan->actorAddress] = plan->commitSeq;
        plans_[plan->actorAddress] = std::move(plan);
        return true;
    }

    uint64_t NextActorCommitSeq(uint64_t actorAddress) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ++actorCommitSeqGenerator_[actorAddress];
    }

    std::shared_ptr<const ComponentPlan> FindPlan(
        uint64_t actorAddress,
        uint64_t epoch,
        uint64_t scene,
        uint64_t regGen,
        OW::offset::RuntimeProfile profile,
        OW::SkeletonSemantics::Form expectedForm = OW::SkeletonSemantics::Form::Unknown,
        DiscoveredObjectKind expectedKind = DiscoveredObjectKind::Unknown) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = plans_.find(actorAddress);
        if (it == plans_.end() || !it->second)
            return nullptr;
        if (!it->second->MatchesContext(epoch, scene, regGen, profile, expectedForm, expectedKind))
            return nullptr;
        return it->second;
    }

    void RetractActor(uint64_t actorAddress) {
        std::lock_guard<std::mutex> lock(mutex_);
        plans_.erase(actorAddress);
    }

    size_t PlanCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return plans_.size();
    }

private:
    mutable std::mutex mutex_;
    uint64_t contextEpoch_ = 0;
    uint64_t contextScene_ = 0;
    uint64_t contextRegistry_ = 0;
    OW::offset::RuntimeProfile contextProfile_ = OW::offset::RuntimeProfile::Cn;
    std::shared_ptr<const DiscoveryDelta> latestDelta_;
    std::unordered_map<uint64_t, std::shared_ptr<const ComponentPlan>> plans_;
    std::unordered_map<uint64_t, uint64_t> actorCommitSeq_;
    std::unordered_map<uint64_t, uint64_t> actorCommitSeqGenerator_;
};

} // namespace OW::EntityDiscovery
