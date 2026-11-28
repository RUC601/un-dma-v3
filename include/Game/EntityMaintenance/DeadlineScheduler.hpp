#pragma once

#include <algorithm>
#include <cstdint>

namespace OW::EntityMaintenance {

// Deadline-budgeted cold scheduling anchored to nextProcessTick.
// Keeps hot ticks protected by a configurable guard margin while giving cold
// discovery & maintenance bounded quanta from the remaining cycle slack.
struct CycleDeadlineBudget {
    static constexpr uint32_t kMaxConsecutiveSkippedMaintenance = 4;

    uint64_t nextTickUs = 0;
    uint64_t guardMarginUs = 350;
    uint64_t minColdQuantumUs = 200;
    uint64_t maxColdCapUs = 1800;

    uint64_t RemainingUntilDeadlineUs(uint64_t nowUs) const noexcept {
        if (nextTickUs <= nowUs)
            return 0;
        const uint64_t raw = nextTickUs - nowUs;
        if (raw <= guardMarginUs)
            return 0;
        return (std::min)(raw - guardMarginUs, maxColdCapUs);
    }

    bool CanStartQuantum(uint64_t nowUs, uint64_t requiredUs) const noexcept {
        return RemainingUntilDeadlineUs(nowUs) >= requiredUs;
    }

    // Discovery budget: at least one small forward-progress quantum (if !hasActiveHotBatch
    // or if called for the first quantum), bounded by remaining slack before nextProcessTick.
    uint64_t DiscoveryDeadlineUs(uint64_t nowUs, bool hasActiveHotBatch) const noexcept {
        const uint64_t slack = RemainingUntilDeadlineUs(nowUs);
        if (!hasActiveHotBatch)
            return nowUs + (std::max)(slack, uint64_t{ 1000 });
        if (slack == 0)
            return nowUs; // Allow 0 extra quanta beyond minimum single-step service.
        return nowUs + (std::min)(slack, uint64_t{ 900 });
    }

    // Maintenance budget after discovery finishes in the same cycle.
    uint64_t MaintenanceBudgetUs(uint64_t nowUs, bool hasActiveHotBatch) const noexcept {
        const uint64_t slack = RemainingUntilDeadlineUs(nowUs);
        if (!hasActiveHotBatch)
            return (std::max)(slack, uint64_t{ 1000 });
        return slack >= minColdQuantumUs ? slack : 0;
    }

    // Anti-starvation maintenance floor: if hot + discovery leaves < minColdQuantumUs + guardMarginUs
    // for kMaxConsecutiveSkippedMaintenance consecutive cycles, grant a bounded forward-progress
    // maintenance quantum (minColdQuantumUs) so bootstrap/respawn/health maintenance cannot starve.
    uint64_t MaintenanceBudgetUsWithAntiStarvation(
        uint64_t nowUs,
        bool hasActiveHotBatch,
        uint32_t& consecutiveSkippedMaintenance,
        uint32_t maxConsecutiveSkips = kMaxConsecutiveSkippedMaintenance) const noexcept
    {
        const uint64_t budget = MaintenanceBudgetUs(nowUs, hasActiveHotBatch);
        if (budget > 0) {
            consecutiveSkippedMaintenance = 0;
            return budget;
        }
        ++consecutiveSkippedMaintenance;
        if (maxConsecutiveSkips > 0 && consecutiveSkippedMaintenance >= maxConsecutiveSkips) {
            consecutiveSkippedMaintenance = 0;
            return minColdQuantumUs;
        }
        return 0;
    }
};

// Camera tail guard tied to actual camera sample age instead of a blind cold duration threshold.
struct CameraAgeTailGuard {
    static constexpr uint64_t kDefaultRefreshAgeUs = 2500; // 2.5ms

    static uint64_t SampleAgeUs(uint64_t lastCameraSampleUs, uint64_t nowUs) noexcept {
        if (lastCameraSampleUs == 0 || nowUs < lastCameraSampleUs)
            return kDefaultRefreshAgeUs;
        return nowUs - lastCameraSampleUs;
    }

    static bool ShouldRefreshAtTail(
        bool hasActiveHotBatch,
        uint64_t lastCameraSampleUs,
        uint64_t nowUs,
        uint64_t maxAgeUs = kDefaultRefreshAgeUs) noexcept
    {
        if (!hasActiveHotBatch)
            return false;
        return SampleAgeUs(lastCameraSampleUs, nowUs) >= maxAgeUs;
    }
};

} // namespace OW::EntityMaintenance
