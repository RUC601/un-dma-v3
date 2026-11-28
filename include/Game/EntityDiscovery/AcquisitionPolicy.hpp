#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace OW::EntityDiscovery {

// Runtime fallback / rollback switch for the experimental Phase A+ unified
// incremental discovery & acquisition path. Any invariant breach can trigger
// an immediate deterministic fallback to the legacy path without restart.
struct AcquisitionFallbackSwitch {
    static bool EnvLegacyRequested() noexcept {
        static const bool requested = [] {
            char buffer[16]{};
            const DWORD len = GetEnvironmentVariableA(
                "UN_DMA_ACQUISITION_LEGACY_FALLBACK",
                buffer,
                static_cast<DWORD>(sizeof(buffer)));
            if (len == 0 || len >= sizeof(buffer))
                return false;
            return buffer[0] != '0' && buffer[0] != 'n' && buffer[0] != 'N' &&
                buffer[0] != 'f' && buffer[0] != 'F';
        }();
        return requested;
    }

    static bool UseUnifiedIncrementalPath() noexcept {
        return !LegacyFallbackActive();
    }

    static bool LegacyFallbackActive() noexcept {
        return EnvLegacyRequested() || forcedLegacy().load(std::memory_order_acquire);
    }

    static void TriggerRollback(const char* reason) {
        {
            std::lock_guard<std::mutex> lock(reasonMutex());
            lastReason() = reason ? reason : "unspecified_invariant_failure";
        }
        rollbackCount().fetch_add(1, std::memory_order_acq_rel);
        forcedLegacy().store(true, std::memory_order_release);
    }

    static void ForceFallback(bool enabled, const char* reason = "manual_override") {
        if (enabled) {
            TriggerRollback(reason);
        } else {
            forcedLegacy().store(false, std::memory_order_release);
        }
    }

    static uint64_t RollbackCount() noexcept {
        return rollbackCount().load(std::memory_order_acquire);
    }

    static std::string LastRollbackReason() {
        std::lock_guard<std::mutex> lock(reasonMutex());
        return lastReason();
    }

    static void ResetForTest() {
        forcedLegacy().store(false, std::memory_order_release);
        rollbackCount().store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(reasonMutex());
        lastReason().clear();
    }

private:
    static std::atomic<bool>& forcedLegacy() noexcept {
        static std::atomic<bool> flag{ false };
        return flag;
    }
    static std::atomic<uint64_t>& rollbackCount() noexcept {
        static std::atomic<uint64_t> count{ 0 };
        return count;
    }
    static std::mutex& reasonMutex() noexcept {
        static std::mutex mutex;
        return mutex;
    }
    static std::string& lastReason() noexcept {
        static std::string value;
        return value;
    }
};

} // namespace OW::EntityDiscovery
