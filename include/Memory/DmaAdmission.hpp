#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace V2::Memory {

enum class DmaReadClass : uint8_t {
    Hot = 0,
    Control = 1,
    Cold = 2,
    Ray = 3,
    Count = 4
};

inline const char* DmaReadClassName(DmaReadClass readClass) noexcept {
    switch (readClass) {
    case DmaReadClass::Hot: return "hot";
    case DmaReadClass::Control: return "control";
    case DmaReadClass::Cold: return "cold";
    case DmaReadClass::Ray: return "ray";
    default: return "unknown";
    }
}

inline uint64_t AdmissionNowUs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct DmaReadContext {
    DmaReadClass readClass = DmaReadClass::Cold;
    uint64_t deadlineUs = 0; // 0 = unbounded
    uint64_t epoch = 0;      // 0 = any open epoch
};

enum class DmaAdmissionRejectReason : uint8_t {
    None = 0,
    Cancelled,
    Closed,
    EpochMismatch,
    DeadlineExpired,
};

struct DmaClassTelemetry {
    uint64_t admitted = 0;
    uint64_t rejectedCancelled = 0;
    uint64_t rejectedClosed = 0;
    uint64_t rejectedEpoch = 0;
    uint64_t rejectedDeadline = 0;
    uint64_t totalWaitUs = 0;
    uint64_t maxWaitUs = 0;
};

struct DmaAdmissionStats {
    std::array<DmaClassTelemetry, static_cast<size_t>(DmaReadClass::Count)> perClass{};
    uint64_t totalAdmitted = 0;
    uint64_t totalWaitUs = 0;
    uint64_t maxWaitUs = 0;
    uint64_t currentEpoch = 0;
    size_t activeReads = 0;
    bool open = false;
    bool serializedPriority = false;
};

// Pre-locked Phase B boundary: unified DMA admission, priority arbitration,
// deadline/cancel/epoch rejection, queue-wait telemetry, and drain barrier.
// In single-threaded Phase A+, serializedPriority defaults to false so normal
// calls incur minimal overhead while still recording wait/class/epoch stats.
class DmaAdmissionController {
public:
    class Ticket {
        friend class DmaAdmissionController;
        DmaAdmissionController* owner_ = nullptr;
        DmaReadClass class_ = DmaReadClass::Cold;
        DmaAdmissionRejectReason rejectReason_ = DmaAdmissionRejectReason::None;
        uint64_t waitUs_ = 0;

        Ticket(DmaAdmissionController* owner, DmaReadClass readClass, uint64_t waitUs)
            : owner_(owner), class_(readClass), waitUs_(waitUs) {}
        explicit Ticket(DmaAdmissionRejectReason reason)
            : rejectReason_(reason) {}

    public:
        Ticket() = default;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              class_(other.class_),
              rejectReason_(other.rejectReason_),
              waitUs_(other.waitUs_) {}
        Ticket& operator=(Ticket&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                class_ = other.class_;
                rejectReason_ = other.rejectReason_;
                waitUs_ = other.waitUs_;
            }
            return *this;
        }
        ~Ticket() { Release(); }

        void Release() {
            if (owner_) {
                owner_->ReleaseTicket(class_);
                owner_ = nullptr;
            }
        }

        explicit operator bool() const noexcept { return owner_ != nullptr; }
        DmaAdmissionRejectReason RejectReason() const noexcept { return rejectReason_; }
        uint64_t WaitUs() const noexcept { return waitUs_; }
    };

    struct ScopedContext {
        DmaReadContext previous;
        explicit ScopedContext(DmaReadClass readClass, uint64_t deadlineUs = 0, uint64_t epoch = 0)
            : previous(CurrentThreadContext()) {
            CurrentThreadContext() = DmaReadContext{readClass, deadlineUs, epoch};
        }
        explicit ScopedContext(const DmaReadContext& next)
            : previous(CurrentThreadContext()) {
            CurrentThreadContext() = next;
        }
        ~ScopedContext() { CurrentThreadContext() = previous; }
        ScopedContext(const ScopedContext&) = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;
    };

    static DmaReadContext& CurrentThreadContext() noexcept {
        thread_local DmaReadContext context{};
        return context;
    }

    void Open(bool bumpEpoch = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bumpEpoch)
            ++epoch_;
        open_ = true;
        cv_.notify_all();
    }

    void NotifyWaiters() {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }

    void CloseAdmission() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
        cv_.notify_all();
    }

    uint64_t WaitDrained() {
        std::unique_lock<std::mutex> lock(mutex_);
        drainedCv_.wait(lock, [this] { return activeTotal_ == 0; });
        return epoch_;
    }

    uint64_t CloseAndDrain() {
        std::unique_lock<std::mutex> lock(mutex_);
        open_ = false;
        cv_.notify_all();
        drainedCv_.wait(lock, [this] { return activeTotal_ == 0; });
        return epoch_;
    }

    uint64_t BumpEpochLockedOrDrained() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ++epoch_;
    }

    void SetSerializedPriorityMode(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        serializedPriority_ = enabled;
        cv_.notify_all();
    }

    bool SerializedPriorityMode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return serializedPriority_;
    }

    uint64_t Epoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return epoch_;
    }

    // Executes an exclusive lifecycle action with strict Drain -> Action -> (optional Reopen).
    template <class Action>
    void ExecuteDrained(bool reopenAfter, Action&& action) {
        std::lock_guard<std::mutex> drainLock(drainActionMutex_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            open_ = false;
            cv_.notify_all();
            drainedCv_.wait(lock, [this] { return activeTotal_ == 0; });
            ++epoch_;
        }
        try {
            action();
        } catch (...) {
            if (reopenAfter) {
                std::lock_guard<std::mutex> lock(mutex_);
                open_ = true;
                cv_.notify_all();
            }
            throw;
        }
        if (reopenAfter) {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
            cv_.notify_all();
        }
    }

    Ticket Acquire(const DmaReadContext& context, const std::atomic<bool>& cancelFlag) {
        const size_t idx = ClassIndex(context.readClass);
        const uint64_t enterUs = AdmissionNowUs();

        if (cancelFlag.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++telemetry_[idx].rejectedCancelled;
            return Ticket(DmaAdmissionRejectReason::Cancelled);
        }
        if (context.deadlineUs != 0 && enterUs >= context.deadlineUs) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++telemetry_[idx].rejectedDeadline;
            return Ticket(DmaAdmissionRejectReason::DeadlineExpired);
        }

        // Allow nested admission on the same thread for THIS controller instance
        // (e.g. helper calling Memory::Read inside a scatter/probe) without self-deadlocking
        // in serialized priority mode, while isolating distinct controller instances.
        if (ThreadNestDepthFor(this) > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelFlag.load(std::memory_order_acquire)) {
                ++telemetry_[idx].rejectedCancelled;
                return Ticket(DmaAdmissionRejectReason::Cancelled);
            }
            if (!open_) {
                ++telemetry_[idx].rejectedClosed;
                return Ticket(DmaAdmissionRejectReason::Closed);
            }
            if (context.epoch != 0 && context.epoch != epoch_) {
                ++telemetry_[idx].rejectedEpoch;
                return Ticket(DmaAdmissionRejectReason::EpochMismatch);
            }
            ++activeTotal_;
            IncrementThreadNestDepth(this);
            ++telemetry_[idx].admitted;
            ++totalAdmitted_;
            return Ticket(this, context.readClass, 0);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        auto reject = [&](DmaAdmissionRejectReason reason) {
            switch (reason) {
            case DmaAdmissionRejectReason::Cancelled: ++telemetry_[idx].rejectedCancelled; break;
            case DmaAdmissionRejectReason::Closed: ++telemetry_[idx].rejectedClosed; break;
            case DmaAdmissionRejectReason::EpochMismatch: ++telemetry_[idx].rejectedEpoch; break;
            case DmaAdmissionRejectReason::DeadlineExpired: ++telemetry_[idx].rejectedDeadline; break;
            default: break;
            }
            return Ticket(reason);
        };

        if (!open_) return reject(DmaAdmissionRejectReason::Closed);
        if (context.epoch != 0 && context.epoch != epoch_)
            return reject(DmaAdmissionRejectReason::EpochMismatch);

        if (serializedPriority_) {
            ++waitingPerClass_[idx];
            auto decrementWaiter = [&]() {
                if (waitingPerClass_[idx] > 0) --waitingPerClass_[idx];
            };
            while (true) {
                if (cancelFlag.load(std::memory_order_acquire)) {
                    decrementWaiter();
                    cv_.notify_all();
                    return reject(DmaAdmissionRejectReason::Cancelled);
                }
                if (!open_) {
                    decrementWaiter();
                    cv_.notify_all();
                    return reject(DmaAdmissionRejectReason::Closed);
                }
                if (context.epoch != 0 && context.epoch != epoch_) {
                    decrementWaiter();
                    cv_.notify_all();
                    return reject(DmaAdmissionRejectReason::EpochMismatch);
                }
                const uint64_t nowUs = AdmissionNowUs();
                if (context.deadlineUs != 0 && nowUs >= context.deadlineUs) {
                    decrementWaiter();
                    cv_.notify_all();
                    return reject(DmaAdmissionRejectReason::DeadlineExpired);
                }

                if (CanAdmitSerializedLocked(context.readClass)) {
                    decrementWaiter();
                    break;
                }

                if (context.deadlineUs != 0) {
                    const uint64_t remainUs = context.deadlineUs > nowUs ? (context.deadlineUs - nowUs) : 0;
                    cv_.wait_for(lock, std::chrono::microseconds(remainUs));
                } else {
                    cv_.wait(lock);
                }
            }
        }

        const uint64_t grantedUs = AdmissionNowUs();
        const uint64_t waitUs = grantedUs >= enterUs ? (grantedUs - enterUs) : 0;

        ++activeTotal_;
        IncrementThreadNestDepth(this);
        if (serializedPriority_) {
            if (context.readClass == DmaReadClass::Hot)
                ++consecutiveHotGrants_;
            else
                consecutiveHotGrants_ = 0;
        }
        ++telemetry_[idx].admitted;
        telemetry_[idx].totalWaitUs += waitUs;
        telemetry_[idx].maxWaitUs = (std::max)(telemetry_[idx].maxWaitUs, waitUs);
        ++totalAdmitted_;
        totalWaitUs_ += waitUs;
        maxWaitUs_ = (std::max)(maxWaitUs_, waitUs);
        return Ticket(this, context.readClass, waitUs);
    }

    DmaAdmissionStats SnapshotStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        DmaAdmissionStats stats{};
        stats.perClass = telemetry_;
        stats.totalAdmitted = totalAdmitted_;
        stats.totalWaitUs = totalWaitUs_;
        stats.maxWaitUs = maxWaitUs_;
        stats.currentEpoch = epoch_;
        stats.activeReads = activeTotal_;
        stats.open = open_;
        stats.serializedPriority = serializedPriority_;
        return stats;
    }

    void ResetStats() {
        std::lock_guard<std::mutex> lock(mutex_);
        telemetry_ = {};
        totalAdmitted_ = 0;
        totalWaitUs_ = 0;
        maxWaitUs_ = 0;
    }

private:
    static constexpr size_t ClassIndex(DmaReadClass readClass) noexcept {
        const auto value = static_cast<size_t>(readClass);
        return value < static_cast<size_t>(DmaReadClass::Count)
            ? value
            : static_cast<size_t>(DmaReadClass::Cold);
    }

    struct ThreadNestEntry {
        const DmaAdmissionController* owner = nullptr;
        size_t depth = 0;
    };

    static std::array<ThreadNestEntry, 8>& ThreadNestTable() noexcept {
        thread_local std::array<ThreadNestEntry, 8> table{};
        return table;
    }

    static size_t ThreadNestDepthFor(const DmaAdmissionController* owner) noexcept {
        const auto& table = ThreadNestTable();
        for (const auto& entry : table) {
            if (entry.owner == owner)
                return entry.depth;
        }
        return 0;
    }

    static void IncrementThreadNestDepth(const DmaAdmissionController* owner) noexcept {
        auto& table = ThreadNestTable();
        for (auto& entry : table) {
            if (entry.owner == owner) {
                ++entry.depth;
                return;
            }
        }
        for (auto& entry : table) {
            if (entry.depth == 0) {
                entry.owner = owner;
                entry.depth = 1;
                return;
            }
        }
    }

    static void DecrementThreadNestDepth(const DmaAdmissionController* owner) noexcept {
        auto& table = ThreadNestTable();
        for (auto& entry : table) {
            if (entry.owner == owner && entry.depth > 0) {
                --entry.depth;
                if (entry.depth == 0)
                    entry.owner = nullptr;
                return;
            }
        }
    }

    bool CanAdmitSerializedLocked(DmaReadClass readClass) const noexcept {
        if (activeTotal_ > 0)
            return false;
        if (readClass == DmaReadClass::Hot) {
            // Anti-starvation: after 4 consecutive Hot grants while non-Hot waits,
            // yield one slot so Control/Cold/Ray cannot starve indefinitely.
            const bool lowerWaiting =
                waitingPerClass_[ClassIndex(DmaReadClass::Control)] > 0 ||
                waitingPerClass_[ClassIndex(DmaReadClass::Cold)] > 0 ||
                waitingPerClass_[ClassIndex(DmaReadClass::Ray)] > 0;
            if (lowerWaiting && consecutiveHotGrants_ >= kMaxConsecutiveHotBeforeYield)
                return false;
            return true;
        }
        // Lower-priority classes yield if Hot is waiting (unless Hot hit its anti-starvation cap).
        if (waitingPerClass_[ClassIndex(DmaReadClass::Hot)] > 0 &&
            consecutiveHotGrants_ < kMaxConsecutiveHotBeforeYield)
            return false;
        if (readClass == DmaReadClass::Cold || readClass == DmaReadClass::Ray) {
            if (waitingPerClass_[ClassIndex(DmaReadClass::Control)] > 0)
                return false;
        }
        return true;
    }

    void ReleaseTicket(DmaReadClass /*readClass*/) {
        DecrementThreadNestDepth(this);
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeTotal_ > 0)
            --activeTotal_;
        if (activeTotal_ == 0)
            drainedCv_.notify_all();
        cv_.notify_all();
    }

    static constexpr uint32_t kMaxConsecutiveHotBeforeYield = 4;

    mutable std::mutex drainActionMutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable drainedCv_;
    bool open_ = false;
    bool serializedPriority_ = false;
    uint64_t epoch_ = 0;
    size_t activeTotal_ = 0;
    uint32_t consecutiveHotGrants_ = 0;
    std::array<size_t, static_cast<size_t>(DmaReadClass::Count)> waitingPerClass_{};
    std::array<DmaClassTelemetry, static_cast<size_t>(DmaReadClass::Count)> telemetry_{};
    uint64_t totalAdmitted_ = 0;
    uint64_t totalWaitUs_ = 0;
    uint64_t maxWaitUs_ = 0;
};

} // namespace V2::Memory
