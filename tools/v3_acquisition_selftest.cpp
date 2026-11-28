#include "Game/EntityDiscovery/AcquisitionPolicy.hpp"
#include "Game/EntityDiscovery/DiscoveryContract.hpp"
#include "Game/EntityDiscovery/IncrementalSlotDiff.hpp"
#include "Game/EntityMaintenance/DeadlineScheduler.hpp"
#include "Game/EngineTrace/UnicornBackend.hpp"
#include "Memory/DmaAdmission.hpp"
#include "Memory/Memory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool cond, const char* msg) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s\n", msg);
    }
}

void TestDmaAdmissionHotVsRayAndDrainedMutex() {
    V2::Memory::DmaAdmissionController ctrl;
    ctrl.Open(true);
    ctrl.SetSerializedPriorityMode(true);
    std::atomic<bool> cancel{false};

    // Acquire Hot ticket and verify Ray waits until Hot releases.
    auto hotTicket = ctrl.Acquire({V2::Memory::DmaReadClass::Hot, 0, 0}, cancel);
    Check(static_cast<bool>(hotTicket), "Hot ticket acquired");

    std::atomic<bool> rayEntered{false};
    std::atomic<bool> rayAcquired{false};
    std::thread rayThread([&] {
        rayEntered.store(true, std::memory_order_release);
        auto rayTicket = ctrl.Acquire({V2::Memory::DmaReadClass::Ray, 0, 0}, cancel);
        rayAcquired.store(static_cast<bool>(rayTicket), std::memory_order_release);
    });
    while (!rayEntered.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    Check(!rayAcquired.load(std::memory_order_acquire), "Ray blocked while Hot holds serialized lane");
    hotTicket.Release();
    rayThread.join();
    Check(rayAcquired.load(std::memory_order_acquire), "Ray acquired after Hot released");

    // Concurrent ExecuteDrained mutual exclusion (Audit Fix #2):
    // Thread B must not overlap or reopen admission while Thread A's drained action runs.
    std::atomic<bool> inA{false};
    std::atomic<bool> releaseA{false};
    std::atomic<bool> bStartedWhileAIn{false};
    std::thread tA([&] {
        ctrl.ExecuteDrained(true, [&] {
            inA.store(true, std::memory_order_release);
            while (!releaseA.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            inA.store(false, std::memory_order_release);
        });
    });
    while (!inA.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::thread tB([&] {
        ctrl.ExecuteDrained(false, [&] {
            if (inA.load(std::memory_order_acquire))
                bStartedWhileAIn.store(true, std::memory_order_release);
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    releaseA.store(true, std::memory_order_release);
    tA.join();
    tB.join();
    Check(!bStartedWhileAIn.load(std::memory_order_acquire), "ExecuteDrained actions serialized strictly");
    // Thread B executed with reopenAfter=false after A finished, so admission stays closed.
    auto closedTicket = ctrl.Acquire({V2::Memory::DmaReadClass::Hot, 0, 0}, cancel);
    Check(!closedTicket && closedTicket.RejectReason() == V2::Memory::DmaAdmissionRejectReason::Closed,
        "Final ExecuteDrained(false) keeps admission closed");
}

void TestPageStoreNonBlockingCacheAndInFlightDedup() {
    auto store = std::make_shared<OW::EngineTrace::PageStore>();
    constexpr uint64_t kPageHit = 0x10000ull;
    constexpr uint64_t kPageMiss = 0x20000ull;
    store->pages.emplace(kPageHit, std::vector<uint8_t>(4096, 0x5Au));

    std::atomic<bool> fetchStarted{false};
    std::atomic<bool> releaseFetch{false};
    std::atomic<int> dmaFetches{0};

    // Simulate UnicornBackend::EnsurePage's PageStore protocol directly across 3 concurrent engines:
    // Engine A fetches kPageMiss (slow DMA); Engine B hits kPageHit immediately;
    // Engine C faults kPageMiss and deduplicates onto A's single DMA fetch.
    auto fetchFromStore = [&](uint64_t page, std::vector<uint8_t>& outBytes) -> bool {
        outBytes.assign(4096, 0);
        bool served = false;
        {
            std::unique_lock<std::mutex> lock(store->mutex);
            for (;;) {
                auto it = store->pages.find(page);
                if (it != store->pages.end()) {
                    std::memcpy(outBytes.data(), it->second.data(), 4096);
                    ++store->stats.hits;
                    served = true;
                    break;
                }
                if (store->inFlightPages.insert(page).second)
                    break;
                store->cv.wait(lock, [&] {
                    return store->inFlightPages.count(page) == 0 ||
                           store->pages.count(page) != 0;
                });
            }
        }
        if (!served) {
            ++dmaFetches;
            fetchStarted.store(true, std::memory_order_release);
            while (!releaseFetch.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::fill(outBytes.begin(), outBytes.end(), 0xA5u);
            std::lock_guard<std::mutex> lock(store->mutex);
            store->inFlightPages.erase(page);
            store->pages.emplace(page, outBytes);
            ++store->stats.fetches;
            store->cv.notify_all();
        }
        return true;
    };

    std::vector<uint8_t> bytesA, bytesB, bytesC;
    std::thread engA([&] { fetchFromStore(kPageMiss, bytesA); });
    while (!fetchStarted.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Engine B MUST hit cached kPageHit immediately while Engine A is blocked in slow DMA!
    const auto t0 = std::chrono::steady_clock::now();
    const bool hitOk = fetchFromStore(kPageHit, bytesB);
    const auto hitElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    Check(hitOk && bytesB[0] == 0x5Au && hitElapsedMs < 50,
        "Cached PageStore hit is not blocked by another engine's in-flight DMA fetch");

    // Engine C faults same kPageMiss -> waits on cv and deduplicates onto A's fetch.
    std::thread engC([&] { fetchFromStore(kPageMiss, bytesC); });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    releaseFetch.store(true, std::memory_order_release);
    engA.join();
    engC.join();

    Check(dmaFetches.load() == 1, "Duplicate concurrent fault on same page triggers only 1 DMA fetch");
    Check(bytesA[0] == 0xA5u && bytesC[0] == 0xA5u, "Both A and C receive fetched page bytes");
    Check(store->stats.fetches == 1 && store->stats.hits == 2, "PageStore stats track 1 fetch + 2 hits");
}

void TestCaptureRaySetupSeedCancelAndDeadline() {
    // V3 Gate #1: CaptureRaySetupSeedWithProbes aborts immediately when cancel/epoch predicate fires.
    std::atomic<bool> cancelled{true};
    std::atomic<int> enumCalls{0};
    std::atomic<int> readCalls{0};
    Memory::RaySetupSeed seed{};
    const bool okCancelled = Memory::CaptureRaySetupSeedWithProbes(
        0x100000ull, 0x7FF600000000ull, 0x5A98000u, 500, seed,
        [&](std::vector<Memory::ThreadSnapshot>&) { ++enumCalls; return true; },
        [&](uintptr_t, void*, size_t) { ++readCalls; return true; },
        [&]() { return cancelled.load(std::memory_order_acquire); });
    Check(!okCancelled && enumCalls.load() == 0 && readCalls.load() == 0,
        "CaptureRaySetupSeedWithProbes aborts before touching SDK when already cancelled");

    // Mid-scan cancellation inside inner thread/page loop stops immediately.
    cancelled.store(false, std::memory_order_release);
    enumCalls.store(0);
    readCalls.store(0);
    const bool okMidCancel = Memory::CaptureRaySetupSeedWithProbes(
        0x100000ull, 0x7FF600000000ull, 0x5A98000u, 2000, seed,
        [&](std::vector<Memory::ThreadSnapshot>& threads) {
            ++enumCalls;
            threads.clear();
            Memory::ThreadSnapshot t{};
            t.tid = 1;
            t.stackLimit = 0x200000ull;
            t.stackBase = 0x220000ull; // 32 pages
            threads.push_back(t);
            return true;
        },
        [&](uintptr_t, void* buf, size_t sz) {
            std::memset(buf, 0, sz);
            if (++readCalls >= 2)
                cancelled.store(true, std::memory_order_release);
            return true;
        },
        [&]() { return cancelled.load(std::memory_order_acquire); });
    Check(!okMidCancel && enumCalls.load() == 1 && readCalls.load() == 2,
        "CaptureRaySetupSeedWithProbes aborts inside inner page loop on cancel");
}

void TestIncrementalSlotDiffAndDeadlineAntiStarvation() {
    OW::EntityDiscovery::IncrementalSlotDiff diff;
    diff.ResetForContext(1, 1, 1, OW::offset::RuntimeProfile::Cn);
    diff.BeginTableSweep(2);
    std::array<uint8_t, 32> block{};
    const uint64_t parent1 = 0x100000ull;
    const uint32_t gen1 = 1;
    const int32_t markerNeg1 = -1;
    std::memcpy(block.data(), &parent1, 8);
    std::memcpy(block.data() + 8, &gen1, 4);
    std::memcpy(block.data() + 12, &markerNeg1, 4);
    diff.ObserveTableBlock(0, block.data(), block.size(), [](uint64_t p) { return p >= 0x10000ull; });
    diff.CommitCompletedTableSweep();

    // Passive marker == -1 telemetry keeps slot (never prunes).
    Check(diff.Contains(parent1) && diff.Stats().markerTelemetry.lastSweepMarkerNeg1 == 1,
        "IncrementalSlotDiff retains marker==-1 slot and records passive telemetry");

    OW::EntityDiscovery::PositiveCandidates::Fingerprint fp{};
    fp.match = 42;
    diff.ObserveHeader(parent1, fp, true, 0x999ull, true, 0, 0, 100, 1, 2, 3,
        [](uint64_t id) { return id == 0x999ull; }, true);
    Check(diff.IsWorldObject(parent1), "WorldObject classified on exact poolId");

    // Audit Fix #6: generation change invalidates stale WorldObject immediately, and failed pool read keeps headerDirty.
    diff.BeginTableSweep(2);
    const uint32_t gen2 = 2;
    std::memcpy(block.data() + 8, &gen2, 4);
    diff.ObserveTableBlock(0, block.data(), block.size(), [](uint64_t p) { return p >= 0x10000ull; });
    diff.CommitCompletedTableSweep();
    Check(!diff.IsWorldObject(parent1), "Generation change invalidates stale WorldObject classification");
    diff.ObserveHeader(parent1, fp, true, 0, false, 0, 0, 200, 1, 2, 3,
        [](uint64_t id) { return id == 0x999ull; }, /*hasPoolPtr=*/true);
    std::vector<std::pair<uint64_t, uint32_t>> ids;
    Check(!diff.CachedMemberIds(ids) && diff.HasUnverifiedHeaders(),
        "Failed Wave 2 pool read keeps entry dirty and CachedMemberIds incomplete");

    // Audit Fix #9: CycleDeadlineBudget anti-starvation floor.
    OW::EntityMaintenance::CycleDeadlineBudget tightBudget{1000};
    uint32_t skipped = 0;
    uint32_t grantedCount = 0;
    for (int i = 0; i < 8; ++i) {
        const uint64_t b = tightBudget.MaintenanceBudgetUsWithAntiStarvation(900, true, skipped);
        if (b > 0) ++grantedCount;
    }
    Check(grantedCount == 2, "CycleDeadlineBudget anti-starvation grants bounded quantum every 4 tight cycles");

    // Phase B SingleWriterHandoff contract: D.Va skeleton_form & ActiveProfile isolation.
    OW::EntityDiscovery::SingleWriterHandoff handoff;
    handoff.ResetForContext(1, 2, 3, OW::offset::RuntimeProfile::Cn);
    auto plan = std::make_shared<OW::EntityDiscovery::ComponentPlan>();
    plan->actorAddress = 0x50000ull;
    plan->commitSeq = handoff.NextActorCommitSeq(0x50000ull);
    plan->connectionEpoch = 1;
    plan->sceneGeneration = 2;
    plan->registryGeneration = 3;
    plan->profile = OW::offset::RuntimeProfile::Cn;
    plan->skeletonForm = OW::SkeletonSemantics::Form::DvaMech;
    plan->objectKind = OW::EntityDiscovery::DiscoveredObjectKind::LinkedActor;
    Check(handoff.PublishPlan(plan), "SingleWriterHandoff publishes plan");
    Check(handoff.FindPlan(0x50000ull, 1, 2, 3, OW::offset::RuntimeProfile::Cn,
            OW::SkeletonSemantics::Form::DvaPilot, OW::EntityDiscovery::DiscoveredObjectKind::LinkedActor) == nullptr,
        "SingleWriterHandoff rejects D.Va pilot lookup against mech plan");
    Check(handoff.FindPlan(0x50000ull, 1, 2, 3, OW::offset::RuntimeProfile::World155284,
            OW::SkeletonSemantics::Form::DvaMech, OW::EntityDiscovery::DiscoveredObjectKind::LinkedActor) == nullptr,
        "SingleWriterHandoff rejects cross-profile lookup");
}

} // namespace

int main() {
    TestDmaAdmissionHotVsRayAndDrainedMutex();
    TestPageStoreNonBlockingCacheAndInFlightDedup();
    TestCaptureRaySetupSeedCancelAndDeadline();
    TestIncrementalSlotDiffAndDeadlineAntiStarvation();
    std::printf("V3AcquisitionPhaseAPlusSelfTest checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
