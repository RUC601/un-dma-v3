#include "EntityWorkers.hpp"
#include "Game/Overwatch.hpp"
#include "Game/EntityDiscovery/WorkSchedule.hpp"
#include "Game/EntityDiscovery/PositiveCandidates.hpp"
#include "Game/EntityDiscovery/AcquisitionPolicy.hpp"
#include "Game/EntityDiscovery/DiscoveryContract.hpp"
#include "Game/EntityDiscovery/IncrementalSlotDiff.hpp"
#include <memory>

namespace {
struct ScopedScatterHandle {
    VMMDLL_SCATTER_HANDLE handle = nullptr;
    ScopedScatterHandle() = default;
    ScopedScatterHandle(const ScopedScatterHandle&) = delete;
    ScopedScatterHandle& operator=(const ScopedScatterHandle&) = delete;
    ScopedScatterHandle(ScopedScatterHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    ScopedScatterHandle& operator=(ScopedScatterHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~ScopedScatterHandle() { Reset(); }
    void Reset() {
        if (handle) {
            mem.CloseScatterHandle(handle);
            handle = nullptr;
        }
    }
    VMMDLL_SCATTER_HANDLE Ensure() {
        if (!handle)
            handle = mem.CreateScatterHandle();
        return handle;
    }
};

struct DiscoveryState {
    std::unique_ptr<OW::EntityScanTask> scan;
    OW::EntityTopologySample topology;
    OW::EntityRegistry::Snapshot registry;
    std::vector<OW::EntityRosterPolicy::Discovery> proofs;
    std::vector<std::pair<uint64_t,uint64_t>> pendingPairs;
    std::vector<uint64_t> knownLinks;
    std::vector<uint64_t> validationLinks;
    std::unordered_set<uint64_t> worldObjects;
    std::vector<OW::EntityRosterPolicy::Discovery> validatedProofs;
    size_t pairCursor=0, publishedPairCount=0;
    size_t validationCursor=0;
    uint64_t validationRevision=0;
    bool validating=false, bindingsComplete=false;
    uint64_t epoch=0, scene=0, completed=0, scanRequest=0;
    DWORD lastScan=0, lastKnownProbe=0, lastLocalProbe=0;
    uint64_t localLink=0;
    OW::EntityRosterPolicy::Discovery lastLocalIdentity{};
    OW::EntityDiscovery::WorkSchedule workSchedule;
    OW::EntityDiscovery::BindingSchedule bindingSchedule;
    OW::EntityDiscovery::PositiveCandidates positive;
    std::vector<uint64_t> positiveTable;
    uint64_t positiveTableOffset=0;
    DWORD positiveTableTick=0,positiveHeaderTick=0;
    bool positiveTableReading=false;
    uint8_t positiveCursor=0;
    uint64_t positiveAttempts=0,positiveIndexAttempts=0,positiveAccepted=0;
    std::chrono::steady_clock::time_point started;
    ScopedScatterHandle positiveHeaderScatter;

    // Phase A+ unified incremental slot-diff discovery, wave scatter, and Phase B handoff boundary.
    OW::offset::RuntimeProfile profile = OW::offset::ActiveProfile();
    OW::EntityDiscovery::IncrementalSlotDiff slotDiff;
    std::unordered_map<uint64_t, OW::EntityHeaderSnapshot> cachedHeaders;
    std::vector<std::pair<uint64_t, uint64_t>> unifiedPairs;
    OW::EntityDiscovery::ScanTransactionToken activeScanToken{};
    uint64_t scanTokenSequence = 0;
    std::unique_ptr<OW::EntityDiscovery::SingleWriterHandoff> handoff =
        std::make_unique<OW::EntityDiscovery::SingleWriterHandoff>();
    uint64_t handoffPublishSeq = 0;
    ScopedScatterHandle waveScatter;
    bool unifiedSweepActive = false;
    bool unifiedTableReading = false;
    bool unifiedTableExact = false;
    bool unifiedRollingDone = false;
    uint64_t unifiedTableOffset = 0;
    bool lastSweepComplete = false;
    bool lastSweepAuthoritative = false;
    bool allowSecondQuantum = false;
    uint64_t midSweepSelfRenewalBumps = 0;
    std::vector<std::pair<uint64_t, uint64_t>> midSweepSelfRenewalPairs;
    std::vector<std::pair<uint64_t, OW::PriorityProbeUnresolvedKind>> unresolvedValidationCandidates;
    bool hadPartialProofBinding = false;

    void ClosePositiveScatter() { positiveHeaderScatter.Reset(); }
    VMMDLL_SCATTER_HANDLE EnsurePositiveScatter() { return positiveHeaderScatter.Ensure(); }
    void CloseWaveScatter() { waveScatter.Reset(); }
    VMMDLL_SCATTER_HANDLE EnsureWaveScatter() { return waveScatter.Ensure(); }
    void ResetForContext(uint64_t nextEpoch, uint64_t nextScene, const OW::EntityRegistry::Snapshot& nextRegistry,
                         OW::offset::RuntimeProfile nextProfile = OW::offset::ActiveProfile()) {
        *this = DiscoveryState{};
        epoch = nextEpoch;
        scene = nextScene;
        registry = nextRegistry;
        profile = nextProfile;
        slotDiff.ResetForContext(nextEpoch, nextScene, nextRegistry.generation, nextProfile);
        if (!handoff)
            handoff = std::make_unique<OW::EntityDiscovery::SingleWriterHandoff>();
        handoff->ResetForContext(nextEpoch, nextScene, nextRegistry.generation, nextProfile);
    }
};

bool PublishDiscovery(DiscoveryState& state,
        std::vector<std::pair<uint64_t,uint64_t>> pairs,
        std::vector<OW::EntityRosterPolicy::Discovery> proofs,
        bool complete, bool bindingComplete=false,
        std::vector<std::pair<uint64_t,uint64_t>> bindings={}, bool scanPositive=false,bool currentPositive=false,
        bool* committed=nullptr, bool selfRenewal=false) {
    if(committed)*committed=false;
    OW::RawEntityScanSnapshot sample{};
    sample.scene_generation=state.scene; sample.connection_epoch=state.epoch;
    sample.registry=state.registry; sample.raw_entities=std::move(pairs);
    sample.discoveries=std::move(proofs); sample.complete=complete;
    sample.binding_complete=bindingComplete; sample.binding_entities=std::move(bindings);
    for(const auto& pair:sample.raw_entities)sample.actual_members.push_back(pair.first);
    sample.scan_started_at=state.started; sample.scan_finished_at=std::chrono::steady_clock::now();
    // A live binding keeps its replacement authority while the full scan is validating.
    sample.source=bindingComplete || currentPositive?2:(state.validating?1:2); sample.valid=true;
    std::vector<std::pair<uint64_t,uint64_t>> renewalPairs;
    if (selfRenewal && !complete && state.unifiedSweepActive && state.activeScanToken.Valid() && !scanPositive) {
        renewalPairs.reserve(sample.raw_entities.size() + sample.binding_entities.size());
        renewalPairs.insert(renewalPairs.end(), sample.raw_entities.begin(), sample.raw_entities.end());
        renewalPairs.insert(renewalPairs.end(), sample.binding_entities.begin(), sample.binding_entities.end());
    }
    std::lock_guard<std::mutex> lock(OW::raw_scan_mutex);
    if(!OW::RuntimeWorkersRunning() || !OW::ProcessConnection::IsConnected() ||
        OW::ProcessConnection::ConnectionEpoch()!=state.epoch ||
        OW::EntitySceneGeneration()!=state.scene || !OW::AcceptEntityRegistry(state.registry))return false;
    const uint64_t expectedRevision = state.activeScanToken.Valid()
        ? state.activeScanToken.startedForeignRevision
        : state.validationRevision;
    sample.complete=OW::EntityRosterPolicy::ScanCanRetire(sample.complete,
        expectedRevision,OW::observed_topology_revision);
    const auto previousRevision=OW::observed_topology_revision;
    auto previousMembers=OW::latest_raw_scan_snapshot.membership_entities;
    OW::MergeRecentRawDiscovery(sample);
    if(!sample.complete) {
        auto currentMembers=sample.membership_entities;
        std::sort(previousMembers.begin(),previousMembers.end());
        std::sort(currentMembers.begin(),currentMembers.end());
        if(previousMembers!=currentMembers)++OW::observed_topology_revision;
    }
    if (!sample.complete && OW::observed_topology_revision != previousRevision &&
        selfRenewal && state.unifiedSweepActive && state.activeScanToken.Valid() && !scanPositive) {
        // Track mid-sweep local_binding/known_binding renewals from THIS owner only when no
        // external/foreign topology revision bump intervened; final commit reconciles them
        // only if every renewed pair is covered by this sweep's own validatedProofs.
        if (state.activeScanToken.startedForeignRevision + state.midSweepSelfRenewalBumps == previousRevision) {
            ++state.midSweepSelfRenewalBumps;
            for (const auto& pair : renewalPairs) {
                if (std::find(state.midSweepSelfRenewalPairs.begin(), state.midSweepSelfRenewalPairs.end(), pair) ==
                    state.midSweepSelfRenewalPairs.end()) {
                    state.midSweepSelfRenewalPairs.push_back(pair);
                }
            }
        }
    }
    // Only this scan's own positive discoveries may advance its deletion fence.
    // External/foreign replacements keep observed_topology_revision ahead of the
    // scan's transaction token so an older scan can never retire the new body.
    if(scanPositive) {
        if(state.validationRevision==previousRevision)
            state.validationRevision=OW::observed_topology_revision;
        if(state.activeScanToken.Valid() &&
           state.activeScanToken.startedForeignRevision + state.midSweepSelfRenewalBumps == previousRevision)
            state.activeScanToken.startedForeignRevision += (OW::observed_topology_revision - previousRevision);
    }
    sample.generation=++OW::raw_scan_publication_sequence;
    const bool authoritative=sample.complete;

    // Pre-locked Phase B single-writer immutable handoff publication.
    {
        auto delta = std::make_shared<OW::EntityDiscovery::DiscoveryDelta>();
        delta->publishSeq = ++state.handoffPublishSeq;
        delta->scanToken = state.activeScanToken.Valid()
            ? state.activeScanToken
            : OW::EntityDiscovery::ScanTransactionToken{
                0, state.validationRevision, state.epoch, state.scene,
                state.registry.generation, state.profile };
        delta->authoritativeComplete = authoritative;
        delta->bindingComplete = bindingComplete;
        delta->pairs = sample.raw_entities;
        delta->proofs = sample.discoveries;
        delta->worldObjectParents.assign(state.worldObjects.begin(), state.worldObjects.end());
        delta->pairKinds.reserve(delta->pairs.size());
        for (const auto& p : delta->pairs) {
            if (state.worldObjects.contains(p.first))
                delta->pairKinds.push_back(OW::EntityDiscovery::DiscoveredObjectKind::WorldObject);
            else if (p.first == p.second)
                delta->pairKinds.push_back(OW::EntityDiscovery::DiscoveredObjectKind::NoLinkActor);
            else
                delta->pairKinds.push_back(OW::EntityDiscovery::DiscoveredObjectKind::LinkedActor);
        }
        if (!state.handoff)
            state.handoff = std::make_unique<OW::EntityDiscovery::SingleWriterHandoff>();
        state.handoff->ResetForContext(state.epoch, state.scene, state.registry.generation, state.profile);
        state.handoff->PublishDelta(std::move(delta));
    }

    OW::latest_raw_scan_snapshot=std::move(sample);
    if(committed)*committed=true;
    return authoritative;
}

static bool ReadUnifiedHeaderBatch(
    DiscoveryState& state,
    const std::vector<OW::EntityDiscovery::IncrementalSlotDiff::HeaderRequest>& requests,
    DWORD now)
{
    if (requests.empty())
        return true;
    const auto handle = state.EnsureWaveScatter();
    if (!handle)
        return false;

    constexpr size_t kMaxBatch = 64;
    const size_t count = (std::min)(requests.size(), kMaxBatch);
    struct HeaderSlotRead {
        std::array<uint8_t, OW::kEntityHeaderSnapshotSize> bytes{};
        OW::EntityDiscovery::PositiveCandidates::Fingerprint fp{};
        uint64_t poolId = 0;
        DWORD headerBytes = 0;
        DWORD poolBytes = 0;
        bool headerPrepared = false;
        bool hasPoolPtr = false;
        bool poolPrepared = false;
    };
    std::array<HeaderSlotRead, kMaxBatch> reads{};

    // Wave 1: full 0x120 EntityHeaderSnapshot (+0x30..+0x150) contains poolPtr, compTable,
    // component bitmap (+0x110..+0x12F), indices (+0x130..+0x133), and matchId (+0x138).
    bool anyHeaderPrepared = false;
    for (size_t i = 0; i < count; ++i) {
        reads[i].headerPrepared = mem.AddScatterReadRequest(
            handle,
            requests[i].parent + OW::kEntityHeaderSnapshotOffset,
            reads[i].bytes.data(),
            reads[i].bytes.size(),
            &reads[i].headerBytes);
        anyHeaderPrepared = anyHeaderPrepared || reads[i].headerPrepared;
    }
    if (!anyHeaderPrepared) {
        state.CloseWaveScatter();
        return false;
    }
    const bool wave1Ok = mem.ExecuteReadScatter(handle);
    if (!wave1Ok) {
        state.CloseWaveScatter();
        return false;
    }
    if (!OW::AcceptEntityRegistry(state.registry))
        return false;

    // Wave 2: poolId for parents with a plausible poolPtr whose classification is
    // unknown/dirty/WorldObject or whose component fingerprint changed (catches in-place reuse).
    bool anyPoolQueued = false;
    bool anyPoolPrepFailed = false;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t parent = requests[i].parent;
        const bool exact = reads[i].headerPrepared && reads[i].headerBytes == reads[i].bytes.size();
        if (!exact)
            continue;
        std::memcpy(reads[i].fp.types.data(), reads[i].bytes.data() + (0x110 - OW::kEntityHeaderSnapshotOffset), 32);
        std::memcpy(&reads[i].fp.match, reads[i].bytes.data() + (OW::offset::Entity_MatchId - OW::kEntityHeaderSnapshotOffset), 4);

        OW::EntityHeaderSnapshot snapshot{};
        snapshot.parent = parent;
        snapshot.valid = true;
        snapshot.range.Assign(parent + OW::kEntityHeaderSnapshotOffset,
            reads[i].bytes.data(), reads[i].bytes.size());
        uint64_t poolPtr = 0;
        snapshot.ReadParentOffset(OW::offset::Entity_PoolPtr, poolPtr);
        poolPtr &= ~0x3Full;
        reads[i].hasPoolPtr = OW::IsPlausibleUserPointer(poolPtr);
        const auto* tracked = state.slotDiff.FindEntry(parent);
        const bool fpChanged = !tracked || !tracked->headerValid || tracked->fingerprint != reads[i].fp;
        const bool needPool = reads[i].hasPoolPtr &&
            (!tracked || !tracked->poolRead || fpChanged || !requests[i].isRollingRecheck ||
             tracked->objectKind == OW::EntityDiscovery::DiscoveredObjectKind::Unknown ||
             tracked->objectKind == OW::EntityDiscovery::DiscoveredObjectKind::WorldObject);
        if (needPool) {
            reads[i].poolPrepared = mem.AddScatterReadRequest(
                handle, poolPtr + OW::offset::Pool_PoolId,
                &reads[i].poolId, sizeof(reads[i].poolId), &reads[i].poolBytes);
            anyPoolQueued = anyPoolQueued || reads[i].poolPrepared;
            anyPoolPrepFailed = anyPoolPrepFailed || !reads[i].poolPrepared;
        }
        state.cachedHeaders.insert_or_assign(parent, snapshot);
        OW::entityScanHeaders.insert_or_assign(parent, std::move(snapshot));
    }

    bool wave2Ok = true;
    if (anyPoolQueued) {
        wave2Ok = mem.ExecuteReadScatter(handle);
        if (!wave2Ok)
            state.CloseWaveScatter();
    } else if (anyPoolPrepFailed) {
        wave2Ok = false;
        state.CloseWaveScatter();
    }
    if (!OW::AcceptEntityRegistry(state.registry))
        return false;

    const auto& offsets = OW::offset::Active();
    for (size_t i = 0; i < count; ++i) {
        const uint64_t parent = requests[i].parent;
        const bool exact = reads[i].headerPrepared && reads[i].headerBytes == reads[i].bytes.size();
        if (!exact) {
            state.cachedHeaders.erase(parent);
            OW::entityScanHeaders.erase(parent);
        }
        const bool poolRead = wave2Ok && reads[i].poolPrepared && reads[i].poolBytes == sizeof(reads[i].poolId);
        state.slotDiff.ObserveHeader(
            parent, reads[i].fp, exact, reads[i].poolId, poolRead, 0, 0,
            now, offsets.typeLink, offsets.typeHeroId, offsets.typeHealth,
            [](uint64_t id) { return V2::WorldObjects::ObjectIndex(id) >= 0; },
            reads[i].hasPoolPtr);
        state.positive.ObserveHeader(parent, reads[i].fp, exact, now, offsets.typeLink, offsets.typeHeroId);
    }
    return true;
}

static void InvalidateCachedHeaderForParent(DiscoveryState& state, uint64_t parent) {
    if (!parent)
        return;
    state.cachedHeaders.erase(parent);
    OW::entityScanHeaders.erase(parent);
    state.slotDiff.MarkHeaderDirty(parent);
}

static size_t EvaluateUnresolvedValidationCandidates(
    DiscoveryState& state,
    const std::unordered_map<uint64_t, OW::EntityHeaderSnapshot>* headersPtr)
{
    if (state.unresolvedValidationCandidates.empty())
        return 0;

    std::unordered_set<uint64_t> knownParents(state.knownLinks.begin(), state.knownLinks.end());
    std::vector<std::pair<uint64_t, uint64_t>> knownActorLinks;
    {
        std::lock_guard<std::mutex> lock(OW::raw_scan_mutex);
        knownActorLinks.reserve(OW::entity_discoveries.actors.size());
        for (const auto& [addr, proof] : OW::entity_discoveries.actors) {
            knownParents.insert(addr);
            knownParents.insert(proof.link);
            knownActorLinks.emplace_back(addr, proof.link);
        }
    }

    size_t blockingUnresolvedCount = 0;
    for (const auto& [candidate, unresolvedKind] : state.unresolvedValidationCandidates) {
        if (state.worldObjects.contains(candidate))
            continue;
        if (state.unifiedSweepActive && state.slotDiff.IsWorldObject(candidate))
            continue;
        bool alreadyValidated = false;
        for (const auto& proof : state.validatedProofs) {
            if (proof.address == candidate || proof.link == candidate) {
                alreadyValidated = true;
                break;
            }
        }
        if (alreadyValidated)
            continue;

        const bool wasKnown = knownParents.contains(candidate);
        if (unresolvedKind == OW::PriorityProbeUnresolvedKind::DefinitiveNonActor && !wasKnown)
            continue;

        // In unified mode, ProbePriorityActor already wave-probed this candidate and
        // determined either a transport/decrypt/map failure or a known actor disappearance;
        // evict stale cached headers and mark dirty so the next sweep re-reads fresh headers.
        if (state.unifiedSweepActive) {
            InvalidateCachedHeaderForParent(state, candidate);
            for (const auto& [addr, link] : knownActorLinks) {
                if (link == candidate && addr != candidate)
                    InvalidateCachedHeaderForParent(state, addr);
            }
            state.bindingsComplete = false;
            ++blockingUnresolvedCount;
            continue;
        }

        // Legacy non-unified path preserves AuditEntityScene check for unclassified WorldObjects.
        auto member = state.topology;
        member.members = { candidate };
        if (OW::AuditEntityScene(member, headersPtr) != OW::EntityRosterPolicy::SceneAudit::Empty) {
            state.bindingsComplete = false;
            ++blockingUnresolvedCount;
        }
    }
    return blockingUnresolvedCount;
}
}

thread_local const char* discoveryOperation="idle";
static void RunPositiveBindingProbe(DiscoveryState& state, DWORD now, bool preferSlotDiffQueue) {
    discoveryOperation="positive_binding";
    uint64_t parent = preferSlotDiffQueue ? state.slotDiff.NextProbe() : 0;
    if (!parent)
        parent = state.positive.NextProbe();
    if (!parent)
        return;
    if (preferSlotDiffQueue && state.slotDiff.IsWorldObject(parent)) {
        state.positive.FinishProbe(parent, false, now);
        state.slotDiff.FinishProbe(parent, false, now);
        return;
    }
    OW::EntityTopologySample topology;
    topology.root = state.registry.root;
    topology.registry = state.registry;
    topology.complete = true;
    topology.member_ids_complete = false;
    topology.members = preferSlotDiffQueue && !state.slotDiff.Members().empty()
        ? state.slotDiff.Members()
        : state.positive.members;
    topology.member_ids_complete = preferSlotDiffQueue && !state.slotDiff.Members().empty()
        ? state.slotDiff.CachedMemberIds(topology.member_ids)
        : state.positive.FreshMemberIds(now, topology.member_ids);
    ++state.positiveAttempts;
    state.positiveIndexAttempts += topology.member_ids_complete;
    std::vector<OW::EntityRosterPolicy::Discovery> proofs;
    const auto* headersPtr = (preferSlotDiffQueue && !state.cachedHeaders.empty()) ? &state.cachedHeaders : nullptr;
    bool scatterPoisoned = false;
    auto pairs = OW::ProbePriorityActor(parent, topology, false, nullptr, nullptr, &proofs, headersPtr, nullptr, false, true,
        state.EnsureWaveScatter(), nullptr, &scatterPoisoned);
    if (scatterPoisoned)
        state.CloseWaveScatter();
    bool accepted = false;
    const bool sameUnifiedScan = preferSlotDiffQueue && state.activeScanToken.Valid() && state.unifiedSweepActive;
    if (!pairs.empty() && !proofs.empty() && OW::AcceptEntityRegistry(state.registry))
        PublishDiscovery(state, std::move(pairs), proofs, false, false, {}, sameUnifiedScan, true, &accepted);
    if (accepted) {
        ++state.positiveAccepted;
        if (std::find(state.knownLinks.begin(), state.knownLinks.end(), parent) == state.knownLinks.end())
            state.knownLinks.push_back(parent);
        Diagnostics::Info("[V3-POSITIVE-DISCOVERY] actor=0x%llX link=0x%llX match=%u hero=0x%llX",
            proofs.front().address, parent, proofs.front().match, proofs.front().hero);
    }
    state.positive.FinishProbe(parent, accepted, now);
    if (preferSlotDiffQueue) {
        state.slotDiff.FinishProbe(parent, accepted, now);
        if (!accepted && std::find(state.knownLinks.begin(), state.knownLinks.end(), parent) != state.knownLinks.end())
            InvalidateCachedHeaderForParent(state, parent);
    }
}

static void AdvancePositiveDiscovery(DiscoveryState& state,DWORD now) {
    // Legacy/direct-caller positive discovery path: one bounded table block, 128 small
    // header fingerprints, or one validated current binding per visit. No scan-index mutation.
    const std::array<bool,3> due{state.positiveTableReading || !state.positiveTableTick || now-state.positiveTableTick>=250,
        !state.positive.members.empty() && (!state.positive.urgent.empty() || !state.positiveHeaderTick || now-state.positiveHeaderTick>=8),
        !state.positive.probes.empty()};
    uint8_t work=3;
    for(uint8_t offset=0;offset<due.size();++offset) {
        const auto index=uint8_t((state.positiveCursor+offset)%due.size());
        if(due[index]){work=index;state.positiveCursor=uint8_t((index+1)%due.size());break;}
    }
    if(work==3)return;
    if(work==0) {
        discoveryOperation="positive_table";
        if(!state.positiveTableReading) {
            state.positiveTable.clear();state.positiveTableOffset=0;state.positiveTableReading=true;
        }
        std::array<uint8_t,0x10000> bytes{};
        const size_t count=OW::EntityRegistry::ChunkBytes(state.registry,state.positiveTableOffset,bytes.size());
        const bool exact=count && mem.ReadFreshExact(state.registry.root+state.positiveTableOffset,bytes.data(),count);
        if(!exact || !OW::AcceptEntityRegistry(state.registry)) {
            state.positiveTableReading=false;state.positiveTableTick=now;
            return;
        }
        std::vector<uint64_t> blockMembers;
        for(size_t offset=0;offset+16<=count;offset+=16) {
            uint64_t parent=0;std::memcpy(&parent,bytes.data()+offset,8);
            if(OW::IsPlausibleUserPointer(parent) && OW::IsLikelyCnEntityParent(parent,state.registry.root,
                state.registry.Bytes(),OW::SDK->dwGameBase,OW::offset::Active().imageSize))
                blockMembers.push_back(parent);
        }
        // Incremental additions need the same current table ticket that the
        // old whole-table commit required. A cached registry check is not enough.
        OW::RefreshEntityRegistry(true);
        if(!OW::AcceptEntityRegistry(state.registry)) {
            state.positiveTableReading=false;state.positiveTableTick=now;
            return;
        }
        state.positiveTable.insert(state.positiveTable.end(),blockMembers.begin(),blockMembers.end());
        state.positive.ObserveAdditionalMembers(std::move(blockMembers));
        state.positiveTableOffset+=count;
        if(state.positiveTableOffset>=state.registry.Bytes()) {
            state.positive.ObserveMembers(std::move(state.positiveTable));
            state.positiveTableReading=false;state.positiveTableTick=now;
        }
        return;
    }
    if(work==2) {
        RunPositiveBindingProbe(state, now, false);
        return;
    }
    discoveryOperation="positive_headers";state.positiveHeaderTick=now;
    constexpr size_t headerBatch=128;
    const auto parents=state.positive.NextHeaders(headerBatch);
    struct Header {std::array<uint8_t,44> bytes{};DWORD actual=0;bool prepared=false;};
    std::array<Header,headerBatch> headers{};
    const auto handle=state.EnsurePositiveScatter();if(!handle)return;
    for(size_t i=0;i<parents.size();++i)headers[i].prepared=mem.AddScatterReadRequest(handle,
        parents[i]+0x110,headers[i].bytes.data(),headers[i].bytes.size(),&headers[i].actual);
    const bool executed=mem.ExecuteReadScatter(handle);
    if(!executed)state.ClosePositiveScatter();
    if(!OW::AcceptEntityRegistry(state.registry))return;
    for(size_t i=0;i<parents.size();++i) {
        OW::EntityDiscovery::PositiveCandidates::Fingerprint value;
        std::memcpy(value.types.data(),headers[i].bytes.data(),32);
        std::memcpy(&value.match,headers[i].bytes.data()+0x28,4);
        state.positive.ObserveHeader(parents[i],value,executed && headers[i].prepared && headers[i].actual==44,
            now,OW::offset::Active().typeLink,OW::offset::Active().typeHeroId);
    }
}
static void AdvanceDiscovery(DiscoveryState& state) {
    Diagnostics::ScopedDmaCallsite tag(Diagnostics::DmaCallsite::EntityScan);
    state.allowSecondQuantum = false;
    const auto epoch=OW::ProcessConnection::ConnectionEpoch();
    const auto scene=OW::EntitySceneGeneration();
    const auto currentProfile=OW::offset::ActiveProfile();
    const auto registry=OW::RefreshEntityRegistry(false);
    if(state.epoch!=epoch || state.scene!=scene || state.registry.generation!=registry.snapshot.generation ||
       state.profile!=currentProfile) {
        state.ResetForContext(epoch, scene, registry.snapshot, currentProfile);
    }
    if(!registry.available)return;
    const DWORD now=static_cast<DWORD>(OW::EntityRegistryNowMs());
    const auto frame=OW::SnapshotFrame();
    const auto& local=frame->local;
    const bool currentMapProfile=
        OW::offset::Active().contextMap==OW::offset::ContextMapMode::Protected155284 ||
        OW::offset::Active().contextMap==OW::offset::ContextMapMode::World155284;
    // Keep only a controller hint across body replacement; the probe still validates the entire new binding.
    if(local.LinkParent && local.AngleBase && local.connection_epoch==epoch && local.scene_generation==scene)
        state.localLink=local.LinkParent;
    const bool useUnifiedIncremental =
#if defined(UNLEASHED_DMA_SESSION_SELFTEST) && !defined(UNLEASHED_DISCOVERY_TEST)
        false; // Compatible legacy fixture façade for EntityRepairSelfTest (inspects state.scan->Pairs() & 1-step cursor).
#else
        OW::EntityDiscovery::AcquisitionFallbackSwitch::UseUnifiedIncrementalPath() &&
        currentMapProfile && OW::DynamicEntityRegistryEnabled();
#endif
    if (!useUnifiedIncremental) {
        if (state.unifiedSweepActive) {
            // Runtime rollback mid-sweep: abort in-flight unified state cleanly instead of
            // falling through to legacy final-publish with empty pairs and stale scanComplete.
            state.slotDiff.AbortTableSweep();
            state.unifiedSweepActive = false;
            state.unifiedTableReading = false;
            state.unifiedTableExact = false;
            state.unifiedRollingDone = false;
            state.unifiedTableOffset = 0;
            state.lastSweepComplete = false;
            state.lastSweepAuthoritative = false;
            state.unifiedPairs.clear();
            state.validating = false;
            state.validationLinks.clear();
            state.validatedProofs.clear();
            state.validationCursor = 0;
            state.bindingsComplete = false;
            state.worldObjects.clear();
            state.proofs.clear();
            state.activeScanToken = {};
            state.midSweepSelfRenewalBumps = 0;
            state.midSweepSelfRenewalPairs.clear();
            state.unresolvedValidationCandidates.clear();
            state.hadPartialProofBinding = false;
            state.lastScan = 0;
            state.CloseWaveScatter();
        }
        state.cachedHeaders.clear();
    }
    // Unified mode replaces duplicate positive_table / positive_headers sweeps whenever
    // no legacy EntityScanTask or pendingPairs drain is in progress.
    const bool unifiedActive = useUnifiedIncremental && !state.scan && state.pendingPairs.empty();
    const bool positiveDue = unifiedActive
        ? (state.slotDiff.PendingProbes() > 0 || !state.positive.probes.empty())
        : (currentMapProfile && OW::DynamicEntityRegistryEnabled() &&
            (state.positiveTableReading || !state.positiveTableTick || now-state.positiveTableTick>=250 ||
             !state.positive.probes.empty() ||
             (!state.positive.members.empty() && (!state.positive.urgent.empty() || now-state.positiveHeaderTick>=8))));
    const auto hasFastMember = [&](uint64_t parent) {
        return (useUnifiedIncremental && state.slotDiff.Contains(parent)) || state.positive.Contains(parent);
    };
    const bool hasAnyFastMembers = (useUnifiedIncremental && !state.slotDiff.Members().empty()) || !state.positive.members.empty();
    constexpr DWORD kUrgentScanIntervalMs = 250;
    constexpr DWORD kSteadyStateUnifiedScanIntervalMs = 900;
    uint64_t pendingRescanRequestCount = 0;
    bool rescanRequested = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pendingRescanRequestCount = OW::entity_topology_rescan_request_count;
        rescanRequested = OW::entity_topology_rescan_request_count !=
            OW::entity_topology_rescan_completed_request_count;
    }
    bool knownLinkNeedsRecovery = false;
    if (state.localLink != 0 &&
        !(local.LinkParent == state.localLink && local.HasUsableBody(now, scene))) {
        knownLinkNeedsRecovery = true;
    }
    if (!knownLinkNeedsRecovery) {
        for (uint64_t link : state.knownLinks) {
            bool usable = (local.LinkParent == link && local.HasUsableBody(now, scene));
            if (!usable) {
                for (const auto& entity : frame->entities) {
                    if (entity.LinkParent == link && entity.HasUsableBody(now, scene)) {
                        usable = true;
                        break;
                    }
                }
            }
            if (!usable) {
                knownLinkNeedsRecovery = true;
                break;
            }
        }
    }
    const bool steadyStateUnified =
        unifiedActive &&
        state.completed > 0 &&
        state.lastSweepComplete &&
        state.lastSweepAuthoritative &&
        !rescanRequested &&
        !state.knownLinks.empty() &&
        !knownLinkNeedsRecovery &&
        !state.slotDiff.HasUnverifiedHeaders() &&
        state.slotDiff.PendingProbes() == 0;
    const DWORD scanIntervalMs = steadyStateUnified
        ? kSteadyStateUnifiedScanIntervalMs
        : ((unifiedActive && rescanRequested && state.scanRequest != pendingRescanRequestCount)
            ? DWORD{ 0 }
            : kUrgentScanIntervalMs);
    const bool localDue =
        state.localLink && (state.topology.complete || (currentMapProfile && hasFastMember(state.localLink))) &&
        (!state.lastLocalProbe || now - state.lastLocalProbe >= 50);
    const bool knownDue =
        !state.knownLinks.empty() && now - state.lastKnownProbe >= 32 &&
        (state.topology.complete || (currentMapProfile && hasAnyFastMembers));
    const bool scanDue =
        state.scan || state.unifiedSweepActive || state.validating ||
        state.pairCursor < state.pendingPairs.size() || !state.lastScan || now - state.lastScan >= scanIntervalMs;
    const auto work = state.workSchedule.Select(localDue, knownDue, scanDue, positiveDue);
    if(work==OW::EntityDiscovery::Work::None)return;
    if(work==OW::EntityDiscovery::Work::PositiveBinding){
        if (unifiedActive)
            RunPositiveBindingProbe(state, now, true);
        else
            AdvancePositiveDiscovery(state, now);
        return;
    }
    if(work==OW::EntityDiscovery::Work::LocalBinding) {
        discoveryOperation="local_binding";
        std::vector<OW::EntityRosterPolicy::Discovery> proofs;
        std::vector<std::pair<uint64_t,uint64_t>> bindings;
        std::vector<OW::EntityRosterPolicy::Discovery> hotActors;
        if(local.HasUsableBody(now,scene))
            hotActors.push_back({local.address,local.LinkParent,local.HeroID,local.match_id,local.anchor_sample_tick_ms});
        bool complete=false;
        const bool localInScanTopology = state.topology.complete &&
            std::binary_search(state.topology.members.begin(),state.topology.members.end(),state.localLink);
        const bool localInSlotDiff = useUnifiedIncremental && state.slotDiff.Contains(state.localLink);
        const bool useLocalPositive = !localInScanTopology && currentMapProfile &&
            (localInSlotDiff || state.positive.Contains(state.localLink));
        OW::EntityTopologySample localPositiveTopology;
        if (useLocalPositive) {
            localPositiveTopology.root = state.registry.root;
            localPositiveTopology.registry = state.registry;
            localPositiveTopology.complete = true;
            if (localInSlotDiff) {
                localPositiveTopology.member_ids_complete = state.slotDiff.CachedMemberIds(localPositiveTopology.member_ids);
                localPositiveTopology.members = state.slotDiff.Members();
            } else {
                localPositiveTopology.member_ids_complete = state.positive.FreshMemberIds(now, localPositiveTopology.member_ids);
                localPositiveTopology.members = state.positive.members;
            }
        }
        bool scatterPoisoned = false;
        auto pairs=OW::ProbePriorityActor(
            state.localLink,
            useLocalPositive ? localPositiveTopology : state.topology,
            true,&bindings,&complete,&proofs,nullptr,&hotActors,true,useLocalPositive,
            state.EnsureWaveScatter(),nullptr,&scatterPoisoned);
        if (scatterPoisoned)
            state.CloseWaveScatter();
        for(const auto& proof:proofs)if(proof.address!=state.lastLocalIdentity.address ||
            proof.match!=state.lastLocalIdentity.match || proof.hero!=state.lastLocalIdentity.hero) {
            Diagnostics::Info("[V3-LOCAL-PROBE] actor=0x%llX link=0x%llX hero=0x%llX match=%u binding_complete=%d",
                proof.address,proof.link,proof.hero,proof.match,int(complete));
            state.lastLocalIdentity=proof;
        }
        if(!pairs.empty())PublishDiscovery(state,std::move(pairs),std::move(proofs),false,complete,std::move(bindings),false,useLocalPositive,nullptr,state.unifiedSweepActive);
        state.lastLocalProbe=static_cast<DWORD>(OW::EntityRegistryNowMs());
        return;
    }
    // Renew known bindings from the same topology used by the resumable full scan.
    if(work==OW::EntityDiscovery::Work::KnownBinding) {
        discoveryOperation="known_binding";
        state.lastKnownProbe=now;
        std::vector<uint8_t> recovery(state.knownLinks.size(),1);
        for(size_t i=0;i<state.knownLinks.size();++i)
            for(const auto& entity:frame->entities)
                if(entity.LinkParent==state.knownLinks[i] && entity.HasUsableBody(now,scene)) {
                    recovery[i]=0;break;
                }
        const auto selected=state.bindingSchedule.Select(recovery);
        const auto parent=state.knownLinks[selected];
        std::vector<OW::EntityRosterPolicy::Discovery> proofs;
        std::vector<std::pair<uint64_t,uint64_t>> bindings;
        bool complete=false;
        std::vector<OW::EntityRosterPolicy::Discovery> hotActors;
        for(const auto& entity:frame->entities)if(entity.HasUsableBody(now,scene))
            hotActors.push_back({entity.address,entity.LinkParent,entity.HeroID,entity.match_id,entity.anchor_sample_tick_ms});
        // During respawn the old UID index can still point at an intact corpse.
        // Consult the controller's current map before that cached index, then
        // retain the probe's exact life/identity and final-map checks.
        const bool inScanTopology = state.topology.complete &&
            std::binary_search(state.topology.members.begin(),state.topology.members.end(),parent);
        const bool currentMap=(recovery[selected] || !inScanTopology) && currentMapProfile;
        OW::EntityTopologySample recoveryTopology;
        const bool slotDiffMember = useUnifiedIncremental && currentMap && state.slotDiff.Contains(parent);
        const bool positiveMember = currentMap && (slotDiffMember || state.positive.Contains(parent));
        const bool freshRecoveryIndex = positiveMember && (slotDiffMember
            ? state.slotDiff.CachedMemberIds(recoveryTopology.member_ids)
            : state.positive.FreshMemberIds(now, recoveryTopology.member_ids));
        // Newly spawned controllers discovered via positive/unified scans may not exist
        // in the older full-scan topology yet; keep renewing them through their
        // live current map so their seats cannot expire before the full scan ends.
        const bool usePositiveTopology = freshRecoveryIndex || (!inScanTopology && positiveMember);
        if(usePositiveTopology) {
            recoveryTopology.root=state.registry.root;recoveryTopology.registry=state.registry;
            recoveryTopology.complete=true;recoveryTopology.member_ids_complete=freshRecoveryIndex;
            recoveryTopology.members=slotDiffMember?state.slotDiff.Members():state.positive.members;
        }
        bool scatterPoisoned = false;
        auto pairs=OW::ProbePriorityActor(parent,usePositiveTopology?recoveryTopology:state.topology,true,
            &bindings,&complete,&proofs,nullptr,&hotActors,currentMap,usePositiveTopology,
            state.EnsureWaveScatter(),nullptr,&scatterPoisoned);
        if (scatterPoisoned)
            state.CloseWaveScatter();
        if(!pairs.empty())PublishDiscovery(state,std::move(pairs),std::move(proofs),false,complete,
            std::move(bindings),false,usePositiveTopology,nullptr,state.unifiedSweepActive);
        return;
    }
    if(state.pairCursor<state.pendingPairs.size()) {
        discoveryOperation="new_binding";
        const auto pair=state.pendingPairs[state.pairCursor++];
        uint64_t objectId=0;
        const auto read=[](uint64_t at,void* data,size_t size){return OW::SDK->read_range(at,data,size);};
        if(pair.first==pair.second && V2::WorldObjects::ReadId(pair.first,read,objectId)) {
            state.worldObjects.insert(pair.first);
            // Objects have no player binding proof and never gain actor-deletion authority.
            PublishDiscovery(state,{pair},{},false);
            return;
        }
        std::vector<OW::EntityRosterPolicy::Discovery> proofs;
        const auto* headersPtr = !OW::entityScanHeaders.empty() ? &OW::entityScanHeaders : nullptr;
        bool scatterPoisoned = false;
        auto pairs=OW::ProbePriorityActor(pair.second,OW::entityScanTopology,false,nullptr,nullptr,&proofs,
            headersPtr,nullptr,false,false,state.EnsureWaveScatter(),nullptr,&scatterPoisoned);
        if (scatterPoisoned)
            state.CloseWaveScatter();
        if(!pairs.empty()) {
            state.topology=OW::entityScanTopology;
            for(const auto& proof:proofs)
                if(std::find(state.knownLinks.begin(),state.knownLinks.end(),proof.link)==state.knownLinks.end())state.knownLinks.push_back(proof.link);
            state.proofs.insert(state.proofs.end(),proofs.begin(),proofs.end());
            PublishDiscovery(state,std::move(pairs),std::move(proofs),false,false,{},true);
        }
        return;
    }

    // Unified incremental slot-diff discovery path (Phase A+).
    // Falls back cleanly to legacy BeginEntityScan when legacy fallback switch is active
    // or when an external/legacy EntityScanTask is already in progress.
    if (useUnifiedIncremental && !state.scan) {
        if (!state.unifiedSweepActive && !state.validating) {
            if (state.lastScan && now - state.lastScan < scanIntervalMs)
                return;
            state.started = std::chrono::steady_clock::now();
            state.lastScan = now;
            state.proofs.clear();
            state.pendingPairs.clear();
            state.worldObjects.clear();
            state.unifiedPairs.clear();
            state.pairCursor = 0;
            state.publishedPairCount = 0;
            state.validationLinks.clear();
            state.validatedProofs.clear();
            state.validationCursor = 0;
            state.validating = false;
            state.bindingsComplete = false;
            state.midSweepSelfRenewalBumps = 0;
            state.midSweepSelfRenewalPairs.clear();
            state.unresolvedValidationCandidates.clear();
            state.hadPartialProofBinding = false;
            state.unifiedSweepActive = true;
            state.unifiedTableReading = true;
            state.unifiedTableExact = true;
            state.unifiedRollingDone = false;
            state.unifiedTableOffset = 0;
            state.slotDiff.BeginTableSweep(state.registry.capacity);
            { std::lock_guard<std::mutex> lock(g_mutex); state.scanRequest = OW::entity_topology_rescan_request_count; }
            {
                std::lock_guard<std::mutex> lock(OW::raw_scan_mutex);
                state.validationRevision = OW::observed_topology_revision;
                state.activeScanToken = {
                    ++state.scanTokenSequence,
                    OW::observed_topology_revision,
                    state.epoch,
                    state.scene,
                    state.registry.generation,
                    state.profile
                };
            }
        }

        if (state.unifiedSweepActive && state.unifiedTableReading) {
            discoveryOperation = "scan_slots";
            std::array<uint8_t, 0x10000> bytes{};
            const size_t count = OW::EntityRegistry::ChunkBytes(state.registry, state.unifiedTableOffset, bytes.size());
            const bool exact = count && mem.ReadFreshExact(state.registry.root + state.unifiedTableOffset, bytes.data(), count);
            if (!exact) {
                state.unifiedTableExact = false;
                state.unifiedTableReading = false;
                state.unifiedSweepActive = false;
                state.lastSweepComplete = false;
                state.lastSweepAuthoritative = false;
                state.slotDiff.AbortTableSweep();
                return;
            }
            OW::RefreshEntityRegistry(true);
            if (!OW::AcceptEntityRegistry(state.registry)) {
                state.unifiedTableExact = false;
                state.unifiedTableReading = false;
                state.unifiedSweepActive = false;
                state.lastSweepComplete = false;
                state.lastSweepAuthoritative = false;
                state.slotDiff.AbortTableSweep();
                return;
            }
            (void)state.slotDiff.ObserveTableBlock(state.unifiedTableOffset, bytes.data(), count,
                [&](uint64_t parent) {
                    return OW::IsPlausibleUserPointer(parent) && OW::IsLikelyCnEntityParent(
                        parent, state.registry.root, state.registry.Bytes(),
                        OW::SDK->dwGameBase, OW::offset::Active().imageSize);
                });
            state.unifiedTableOffset += count;
            if (state.unifiedTableOffset >= state.registry.Bytes()) {
                state.slotDiff.CommitCompletedTableSweep();
                // Prune cached headers for members removed in this completed sweep.
                std::erase_if(state.cachedHeaders, [&](const auto& kv) {
                    return !state.slotDiff.Contains(kv.first);
                });
                std::erase_if(OW::entityScanHeaders, [&](const auto& kv) {
                    return !state.slotDiff.Contains(kv.first);
                });
                state.unifiedTableReading = false;
            }
            state.allowSecondQuantum =
                !steadyStateUnified && !localDue && !knownDue && !positiveDue &&
                state.unifiedSweepActive && !state.validating &&
                (state.unifiedTableReading || state.slotDiff.PendingUrgentHeaders() > 0 || !state.unifiedRollingDone);
            return;
        }

        if (state.unifiedSweepActive && !state.validating) {
            // Drain dirty/urgent headers once per sweep plus one bounded rolling recheck batch.
            // Never loop on HasUnverifiedHeaders() here: a persistent short/unmapped slot must
            // degrade the sweep to partial (non-retiring) instead of livelocking scan_headers forever.
            const bool needHeaders = state.slotDiff.PendingUrgentHeaders() > 0 || !state.unifiedRollingDone;
            if (needHeaders) {
                discoveryOperation = "scan_headers";
                constexpr size_t kHeaderBatchLimit = 64;
                constexpr size_t kRollingRecheckBudget = 16;
                const auto batch = state.slotDiff.NextHeaderBatch(kHeaderBatchLimit, kRollingRecheckBudget);
                state.unifiedRollingDone = true;
                if (!batch.empty()) {
                    if (!ReadUnifiedHeaderBatch(state, batch, now)) {
                        state.unifiedTableExact = false;
                        state.unifiedSweepActive = false;
                        state.lastSweepComplete = false;
                        state.lastSweepAuthoritative = false;
                        return;
                    }
                    // If more dirty/urgent headers remain in this sweep, yield cleanly to keep hot ticks smooth.
                    if (state.slotDiff.PendingUrgentHeaders() > 0) {
                        state.allowSecondQuantum =
                            !steadyStateUnified && !localDue && !knownDue && !positiveDue;
                        return;
                    }
                }
            }

            // Transition from unified incremental slot/header phase into batched roster validation.
            OW::EntityTopologySample nextTopology{};
            nextTopology.registry = state.registry;
            nextTopology.root = state.registry.root;
            nextTopology.complete = state.unifiedTableExact;
            nextTopology.members = state.slotDiff.Members();
            nextTopology.member_ids_complete = state.unifiedTableExact &&
                state.slotDiff.CachedMemberIds(nextTopology.member_ids);
            state.topology = nextTopology;
            OW::entityScanTopology = nextTopology;
            OW::entityScanComplete = state.unifiedTableExact && nextTopology.member_ids_complete;
            OW::entityScanReadComplete = OW::entityScanComplete;
            OW::entityScanRegistry = state.registry;
            OW::entityScanStatus = OW::entityScanComplete
                ? OW::EntityRegistry::ScanStatus::Complete
                : OW::EntityRegistry::ScanStatus::Partial;

            std::vector<uint64_t> linkedActors, noLinkActors, worldObjParents;
            state.slotDiff.CollectValidationCandidates(linkedActors, noLinkActors, worldObjParents);

            state.worldObjects.clear();
            state.unifiedPairs.clear();
            for (uint64_t obj : worldObjParents) {
                state.worldObjects.insert(obj);
                state.unifiedPairs.emplace_back(obj, obj);
            }
            state.validationLinks.clear();
            state.validationLinks.reserve(linkedActors.size() + noLinkActors.size());
            state.validationLinks.insert(state.validationLinks.end(), linkedActors.begin(), linkedActors.end());
            state.validationLinks.insert(state.validationLinks.end(), noLinkActors.begin(), noLinkActors.end());
            std::sort(state.validationLinks.begin(), state.validationLinks.end());
            state.validationLinks.erase(
                std::unique(state.validationLinks.begin(), state.validationLinks.end()),
                state.validationLinks.end());

            state.validating = true;
            state.validationCursor = 0;
            state.validatedProofs.clear();
            state.bindingsComplete = OW::entityScanComplete && state.topology.member_ids_complete;
            // Fall through to validation or publish on next visit to keep slice bounded.
            if (!state.validationLinks.empty())
                return;
        }
    }

    // Legacy BeginEntityScan path (used when legacy fallback switch is active,
    // on non-dynamic/non-155284 profiles, or when an existing scan task is in flight).
    if(!state.unifiedSweepActive && !state.validating) {
        if(!state.scan) {
            if(state.lastScan && now-state.lastScan<250)return;
            state.started=std::chrono::steady_clock::now();state.lastScan=now;
            state.proofs.clear();state.pendingPairs.clear();state.worldObjects.clear();state.pairCursor=0;state.publishedPairCount=0;
            state.validationLinks.clear();state.validatedProofs.clear();state.validationCursor=0;
            state.validating=false;state.bindingsComplete=false;
            state.midSweepSelfRenewalBumps=0;state.midSweepSelfRenewalPairs.clear();
            state.unresolvedValidationCandidates.clear();state.hadPartialProofBinding=false;
            state.scan=std::make_unique<OW::EntityScanTask>(OW::BeginEntityScan());
            {std::lock_guard<std::mutex> lock(g_mutex);state.scanRequest=OW::entity_topology_rescan_request_count;}
            {
                std::lock_guard<std::mutex> lock(OW::raw_scan_mutex);
                state.validationRevision=OW::observed_topology_revision;
                state.activeScanToken = {
                    ++state.scanTokenSequence,
                    OW::observed_topology_revision,
                    state.epoch,
                    state.scene,
                    state.registry.generation,
                    state.profile
                };
            }
        }
        discoveryOperation="scan_slice";
        const auto sliceStarted=std::chrono::steady_clock::now();
        const auto previousPairCount=state.scan->Pairs().size();
        // Keep the 650us time budget, but allow pure in-memory pairing yields to
        // drain without wasting hundreds of discovery visits on empty slices.
        for(int step=0;step<512 && !state.scan->Done();++step) {
            const auto phase=state.scan->handle.promise().progress.phase;
            discoveryOperation=phase==OW::EntityScanPhase::Slots?"scan_slots":phase==OW::EntityScanPhase::Headers?"scan_headers":"scan_pairing";
            state.scan->Resume();
            if(state.scan->Pairs().size()!=previousPairCount ||
                std::chrono::steady_clock::now()-sliceStarted>=std::chrono::microseconds(650))break;
        }
        const auto& pairs=state.scan->Pairs();
        while(state.publishedPairCount<pairs.size())state.pendingPairs.push_back(pairs[state.publishedPairCount++]);
        if(!state.scan->Done() || state.pairCursor<state.pendingPairs.size())return;
        state.topology=OW::entityScanTopology;
        // Revalidate the final paired roster with current headers. A table read can
        // finish successfully while all the actor bindings have become unusable.
        if(!state.validating) {
            state.validating=true;
            state.bindingsComplete=OW::entityScanComplete && state.topology.member_ids_complete;
            for(const auto& pair:pairs)if(!state.worldObjects.contains(pair.first))state.validationLinks.push_back(pair.second);
            for(const auto& [parent,header]:OW::entityScanHeaders) {
                if(state.worldObjects.contains(parent))continue;
                uint64_t linkBits=0,heroBits=0;
                const auto linkType=OW::offset::Active().typeLink,heroType=OW::offset::Active().typeHeroId;
                if((header.ReadParentOffset(0x110+8ull*(linkType>>6),linkBits) && (linkBits&(1ull<<(linkType&63)))) ||
                    (header.ReadParentOffset(0x110+8ull*(heroType>>6),heroBits) && (heroBits&(1ull<<(heroType&63)))))
                    state.validationLinks.push_back(parent);
            }
            std::sort(state.validationLinks.begin(),state.validationLinks.end());
            state.validationLinks.erase(std::unique(state.validationLinks.begin(),state.validationLinks.end()),state.validationLinks.end());
        }
    }

    if(state.validationCursor<state.validationLinks.size()) {
        discoveryOperation="validate_roster";
        const auto& activeHeaders = (state.unifiedSweepActive && !state.cachedHeaders.empty())
            ? state.cachedHeaders
            : OW::entityScanHeaders;
        const auto* headersPtr = activeHeaders.empty() ? nullptr : &activeHeaders;
        // Wave-batch validation: advance up to 4 parents per visit (bounded by 650us)
        // while reusing cached headers instead of passing nullptr and doing 1/cycle.
#if defined(UNLEASHED_DMA_SESSION_SELFTEST) && !defined(UNLEASHED_DISCOVERY_TEST)
        constexpr size_t kValidationBatchMax = 1;
#else
        constexpr size_t kValidationBatchMax = 4;
#endif
        const auto batchStarted = std::chrono::steady_clock::now();
        for (size_t processed = 0;
             processed < kValidationBatchMax && state.validationCursor < state.validationLinks.size();
             ++processed) {
            if (processed > 0 &&
                std::chrono::steady_clock::now() - batchStarted >= std::chrono::microseconds(650))
                break;
            const auto parent=state.validationLinks[state.validationCursor++];
            if (state.worldObjects.contains(parent))
                continue;
            bool resolved=false;
            bool scatterPoisoned=false;
            OW::PriorityProbeUnresolvedKind unresolvedKind = OW::PriorityProbeUnresolvedKind::UnknownOrFailure;
            std::vector<OW::EntityRosterPolicy::Discovery> proofs;
            const auto waveHandle = state.EnsureWaveScatter();
            const auto verified=OW::ProbePriorityActor(parent,state.topology,false,nullptr,&resolved,&proofs,
                headersPtr,nullptr,false,false,waveHandle,&unresolvedKind,&scatterPoisoned);
            if (scatterPoisoned)
                state.CloseWaveScatter();
            if(!verified.empty() && resolved) {
                state.validatedProofs.insert(state.validatedProofs.end(),proofs.begin(),proofs.end());
                state.proofs.insert(state.proofs.end(),proofs.begin(),proofs.end());
                if (state.unifiedSweepActive) {
                    for (const auto& pair : verified) {
                        if (std::find(state.unifiedPairs.begin(), state.unifiedPairs.end(), pair) == state.unifiedPairs.end())
                            state.unifiedPairs.push_back(pair);
                    }
                    PublishDiscovery(state, verified, proofs, false, false, {}, true);
                }
                for (const auto& proof : proofs) {
                    state.slotDiff.FinishProbe(parent, true, now);
                    if (std::find(state.knownLinks.begin(), state.knownLinks.end(), proof.link) == state.knownLinks.end())
                        state.knownLinks.push_back(proof.link);
                }
            } else if(!verified.empty() && !proofs.empty() &&
                (state.unifiedSweepActive ||
                 OW::offset::Active().contextMap==OW::offset::ContextMapMode::World155284)) {
                // A partial unified sweep or current World map may discover a valid body
                // without full roster completeness. Publish its proof without granting retirement.
                state.bindingsComplete=false;
                state.hadPartialProofBinding=true;
                state.proofs.insert(state.proofs.end(),proofs.begin(),proofs.end());
                if (state.unifiedSweepActive) {
                    for (const auto& pair : verified) {
                        if (std::find(state.unifiedPairs.begin(), state.unifiedPairs.end(), pair) == state.unifiedPairs.end())
                            state.unifiedPairs.push_back(pair);
                    }
                    for (const auto& proof : proofs) {
                        state.slotDiff.FinishProbe(parent, true, now);
                        if (std::find(state.knownLinks.begin(), state.knownLinks.end(), proof.link) == state.knownLinks.end())
                            state.knownLinks.push_back(proof.link);
                    }
                }
                PublishDiscovery(state,verified,proofs,false,false,{},true,true);
            } else {
                state.unresolvedValidationCandidates.emplace_back(parent, unresolvedKind);
            }
        }
        return;
    }

    const auto& finalPairs = state.scan ? state.scan->Pairs() : state.unifiedPairs;
    const auto& activeHeaders = (state.unifiedSweepActive && !state.cachedHeaders.empty())
        ? state.cachedHeaders
        : OW::entityScanHeaders;
    const auto* headersPtr = activeHeaders.empty() ? nullptr : &activeHeaders;
    const size_t blockingUnresolvedCount = EvaluateUnresolvedValidationCandidates(state, headersPtr);
    if (state.unifiedSweepActive && state.activeScanToken.Valid() && state.midSweepSelfRenewalBumps > 0) {
        const bool allRenewalsCovered = std::all_of(
            state.midSweepSelfRenewalPairs.begin(),
            state.midSweepSelfRenewalPairs.end(),
            [&](const auto& pair) {
                return std::any_of(
                    state.validatedProofs.begin(),
                    state.validatedProofs.end(),
                    [&](const auto& proof) {
                        return proof.address == pair.first && proof.link == pair.second;
                    });
            });
        if (allRenewalsCovered) {
            state.activeScanToken.startedForeignRevision += state.midSweepSelfRenewalBumps;
        }
    }
    const bool readComplete=OW::entityScanComplete;
    const bool authoritative=OW::EntityRosterPolicy::AuthoritativeRoster(readComplete,
        state.bindingsComplete,state.validatedProofs);
    if(readComplete) {
        discoveryOperation="scene_audit";
        const auto audit=!state.proofs.empty()?OW::EntityRosterPolicy::SceneAudit::Present:
            OW::AuditEntityScene(state.topology,headersPtr);
        OW::RecordEntitySceneAudit(state.epoch,state.registry.generation,state.scene,audit,now);
    }
    const bool complete=PublishDiscovery(state,finalPairs,authoritative?state.validatedProofs:state.proofs,authoritative);
    const char* incompleteReason = "none";
    if (!complete) {
        if (!readComplete)
            incompleteReason = "read_incomplete";
        else if (state.validatedProofs.empty())
            incompleteReason = "no_proofs";
        else if (!state.bindingsComplete)
            incompleteReason = state.hadPartialProofBinding ? "bindings_partial_proof" : "bindings_unresolved_actor";
        else
            incompleteReason = "topology_revision_drift";
    }
    {
        std::lock_guard<std::mutex> lock(OW::raw_scan_mutex);
        OW::observed_entity_topology=state.topology;OW::observed_topology_epoch=epoch;
        state.knownLinks.clear();
        std::unordered_set<uint64_t> seen;
        for(const auto& [address,proof]:OW::entity_discoveries.actors)
            if(seen.insert(proof.link).second)state.knownLinks.push_back(proof.link);
    }
    if(readComplete) {
        OW::AcknowledgeEntityRegistry(state.registry,OW::EntityRegistry::ScanStatus::Complete);
        std::lock_guard<std::mutex> lock(g_mutex);
        OW::entity_topology_rescan_completed_request_count=(std::max)(
            OW::entity_topology_rescan_completed_request_count,state.scanRequest);
    }
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-state.started).count();
    Diagnostics::RecordEntityScanCycle(finalPairs.size(),elapsed>0?1.0/elapsed:0);
    const auto& diffStats = state.slotDiff.Stats();
    Diagnostics::Info("[UNDMA-V3-DISCOVERY] completed=%llu pairs=%zu members=%zu read_complete=%d roster_complete=%d proofs=%zu elapsed_ms=%.1f unified=%d marker_neg1_last=%llu marker_neg1_actors=%llu marker_neg1_objects=%llu dirty_hdrs=%llu rolling_hdrs=%llu incomplete_reason=%s unresolved=%zu",
        ++state.completed,finalPairs.size(),state.topology.members.size(),readComplete,complete,state.validatedProofs.size(),elapsed*1000,
        state.unifiedSweepActive ? 1 : 0,
        static_cast<unsigned long long>(diffStats.markerTelemetry.lastSweepMarkerNeg1),
        static_cast<unsigned long long>(diffStats.markerTelemetry.markerNeg1ConfirmedActors),
        static_cast<unsigned long long>(diffStats.markerTelemetry.markerNeg1ConfirmedWorldObjects),
        static_cast<unsigned long long>(diffStats.headerDirtyReads),
        static_cast<unsigned long long>(diffStats.headerRollingRechecks),
        incompleteReason,
        blockingUnresolvedCount);
    state.scan.reset();
    state.validating=false;
    state.unifiedSweepActive=false;
    state.lastSweepComplete=readComplete;
    state.lastSweepAuthoritative=complete;
    state.activeScanToken={};
    state.midSweepSelfRenewalBumps=0;
    state.midSweepSelfRenewalPairs.clear();
    state.unresolvedValidationCandidates.clear();
    state.hadPartialProofBinding=false;
}

void StepAcquisitionDiscoveryWithBudget(uint64_t deadlineUs) {
    // Always grant the mandatory single-step forward-progress quantum with an unbounded
    // in-flight DMA admission deadline so a tight nextProcessTick never aborts an active
    // scatter wave mid-operation; use deadlineUs solely to gate extra quanta.
    V2::Memory::DmaAdmissionController::ScopedContext scope(V2::Memory::DmaReadClass::Cold, 0);
    const auto started=std::chrono::steady_clock::now();
    discoveryOperation="idle";
    static thread_local DiscoveryState state;
    AdvanceDiscovery(state);
    // Only allow a second bounded quantum during non-steady-state sweep table/header draining
    // when generous slack (>= 600us) remains and no binding probe is due, protecting 300Hz hot cadence.
    if (deadlineUs != 0 && state.allowSecondQuantum) {
        const uint64_t nowUs = V2::Memory::AdmissionNowUs();
        if (nowUs + 600 < deadlineUs) {
            AdvanceDiscovery(state);
        }
    }
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    static thread_local auto reportedAt=started;
    static thread_local double maximum=0;
    static thread_local const char* slowest="idle";
    if(elapsed>maximum){maximum=elapsed;slowest=discoveryOperation;}
    if(started-reportedAt>=std::chrono::seconds(5)) {
        const auto& diffStats = state.slotDiff.Stats();
        Diagnostics::Info("[UNDMA-V3-DISCOVERY-TIME] max_ms=%.3f operation=%s positive_members=%zu positive_headers=%zu positive_attempts=%llu positive_index_attempts=%llu positive_accepted=%llu slot_diff_sweeps=%llu slot_diff_added=%llu slot_diff_gen_chg=%llu slot_diff_marker_chg=%llu marker_neg1_total=%llu marker_neg1_actors=%llu legacy_fallback=%d rollbacks=%llu",
            maximum,slowest,state.positive.members.size(),state.positive.entries.size(),
            state.positiveAttempts,state.positiveIndexAttempts,state.positiveAccepted,
            static_cast<unsigned long long>(diffStats.sweepsCompleted),
            static_cast<unsigned long long>(diffStats.slotsAdded),
            static_cast<unsigned long long>(diffStats.slotsGenerationChanged),
            static_cast<unsigned long long>(diffStats.slotsMarkerChanged),
            static_cast<unsigned long long>(diffStats.markerTelemetry.totalMarkerNeg1Seen),
            static_cast<unsigned long long>(diffStats.markerTelemetry.markerNeg1ConfirmedActors),
            OW::EntityDiscovery::AcquisitionFallbackSwitch::LegacyFallbackActive() ? 1 : 0,
            static_cast<unsigned long long>(OW::EntityDiscovery::AcquisitionFallbackSwitch::RollbackCount()));
        maximum=0;reportedAt=started;
    }
}

void StepAcquisitionDiscovery() {
    StepAcquisitionDiscoveryWithBudget(0);
}
