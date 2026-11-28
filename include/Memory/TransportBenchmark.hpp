#pragma once

#include "Memory/Memory.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace V2::Memory {

struct LatencySummary {
    size_t samples = 0;
    size_t failures = 0;
    double minUs = 0.0;
    double meanUs = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
};

struct QuantumRow {
    size_t bytes = 0;
    LatencySummary latency{};
};

struct ScatterPageRow {
    size_t pages = 0;
    size_t distinctPages = 0;
    LatencySummary latency{};
};

struct TransportCapabilityMatrix {
    std::string deviceString;
    bool ok = false;
    std::vector<QuantumRow> contiguousQuanta;
    std::vector<ScatterPageRow> scatterBatches;
    LatencySummary singleThreadScatter16{};
    LatencySummary dualThreadScatter16Primary{};
    LatencySummary dualThreadScatter16Secondary{};
    double contentionRatio = 1.0;
    size_t recommendedTableQuantumBytes = 65536;
    bool recommendsSerializedHotLane = true;
};

inline LatencySummary SummarizeLatencies(std::vector<double> values, size_t failures) {
    LatencySummary summary{};
    summary.samples = values.size();
    summary.failures = failures;
    if (values.empty())
        return summary;
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (double v : values) sum += v;
    summary.minUs = values.front();
    summary.maxUs = values.back();
    summary.meanUs = sum / static_cast<double>(values.size());
    const auto pct = [&](double p) {
        const size_t idx = (std::min)(
            values.size() - 1,
            static_cast<size_t>(p * static_cast<double>(values.size() - 1)));
        return values[idx];
    };
    summary.p95Us = pct(0.95);
    summary.p99Us = pct(0.99);
    return summary;
}

// Read-only transport quantum and concurrency microbenchmark.
// Never writes to target memory; requires an initialized Memory session and a readable base address.
inline TransportCapabilityMatrix RunTransportBenchmark(
    ::Memory& memory,
    uint64_t readableBaseAddress,
    size_t iterations = 24,
    size_t readableSpanBytes = 512 * 1024)
{
    TransportCapabilityMatrix matrix{};
    matrix.deviceString = memory.GetDmaDeviceString();
    if (!memory.IsDmaInitialized() || !readableBaseAddress || iterations == 0)
        return matrix;

    const size_t boundedIterations = std::clamp<size_t>(iterations, 4, 200);
    const size_t maxDistinctPages = (std::max)(size_t{1}, readableSpanBytes / 4096ULL);
    const std::array<size_t, 4> quanta{ 4096, 16384, 32768, 65536 };
    std::vector<uint8_t> scratch(65536);

    for (size_t bytes : quanta) {
        std::vector<double> samples;
        samples.reserve(boundedIterations);
        size_t failures = 0;
        for (size_t i = 0; i < boundedIterations; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = memory.ReadFreshExact(readableBaseAddress, scratch.data(), bytes);
            const double us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - t0).count();
            if (ok) samples.push_back(us);
            else ++failures;
        }
        matrix.contiguousQuanta.push_back({ bytes, SummarizeLatencies(std::move(samples), failures) });
    }

    auto measureScatterPages = [&](size_t pageCount, size_t iters) -> LatencySummary {
        std::vector<double> samples;
        samples.reserve(iters);
        size_t failures = 0;
        auto handle = memory.CreateScatterHandle();
        if (!handle)
            return LatencySummary{ 0, iters, 0, 0, 0, 0, 0 };
        std::vector<std::array<uint8_t, 64>> buffers(pageCount);
        std::vector<DWORD> bytesRead(pageCount, 0);
        for (size_t iter = 0; iter < iters; ++iter) {
            if (!handle) {
                ++failures;
                continue;
            }
            std::fill(bytesRead.begin(), bytesRead.end(), 0);
            bool prepOk = true;
            for (size_t p = 0; p < pageCount; ++p) {
                const uint64_t addr = readableBaseAddress + (p % maxDistinctPages) * 4096ULL;
                prepOk &= memory.AddScatterReadRequest(
                    handle, addr, buffers[p].data(), buffers[p].size(), &bytesRead[p]);
            }
            if (!prepOk) {
                if (!memory.ClearScatterHandle(handle)) {
                    memory.CloseScatterHandle(handle);
                    handle = memory.CreateScatterHandle();
                }
                ++failures;
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const bool execOk = memory.ExecuteReadScatter(handle);
            const double us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - t0).count();
            bool allFullRead = execOk;
            if (allFullRead) {
                for (size_t p = 0; p < pageCount; ++p) {
                    if (bytesRead[p] != buffers[p].size()) {
                        allFullRead = false;
                        break;
                    }
                }
            }
            if (allFullRead) {
                samples.push_back(us);
            } else {
                if (!execOk && !memory.ClearScatterHandle(handle)) {
                    memory.CloseScatterHandle(handle);
                    handle = memory.CreateScatterHandle();
                }
                ++failures;
            }
        }
        if (handle)
            memory.CloseScatterHandle(handle);
        return SummarizeLatencies(std::move(samples), failures);
    };

    const std::array<size_t, 6> pageCounts{ 1, 8, 16, 32, 64, 128 };
    for (size_t pages : pageCounts) {
        const size_t distinct = (std::min)(pages, maxDistinctPages);
        matrix.scatterBatches.push_back({ pages, distinct, measureScatterPages(pages, boundedIterations) });
    }

    // Calibrate single-thread vs 2-thread concurrent scatter contention (16 pages).
    matrix.singleThreadScatter16 = measureScatterPages(16, boundedIterations);
    std::atomic<bool> startFlag{ false };
    LatencySummary secondarySummary{};
    std::thread contender([&]() {
        while (!startFlag.load(std::memory_order_acquire))
            std::this_thread::yield();
        secondarySummary = measureScatterPages(16, boundedIterations);
    });
    startFlag.store(true, std::memory_order_release);
    matrix.dualThreadScatter16Primary = measureScatterPages(16, boundedIterations);
    contender.join();
    matrix.dualThreadScatter16Secondary = secondarySummary;

    if (matrix.singleThreadScatter16.meanUs > 1.0 && matrix.dualThreadScatter16Primary.meanUs > 0.0) {
        matrix.contentionRatio =
            matrix.dualThreadScatter16Primary.meanUs / matrix.singleThreadScatter16.meanUs;
    }

    // Pick largest contiguous quantum whose p95 stays within 2.5x of 4KB latency (default 64KB, fallback 16KB/32KB).
    if (matrix.contiguousQuanta.size() == 4 && matrix.contiguousQuanta[0].latency.p95Us > 0.0) {
        const double baseP95 = matrix.contiguousQuanta[0].latency.p95Us;
        matrix.recommendedTableQuantumBytes = 4096;
        for (const auto& row : matrix.contiguousQuanta) {
            if (row.latency.samples > 0 && row.latency.p95Us <= (std::max)(baseP95 * 2.5, 900.0))
                matrix.recommendedTableQuantumBytes = row.bytes;
        }
    }
    matrix.recommendsSerializedHotLane = matrix.contentionRatio >= 1.35;
    matrix.ok = !matrix.contiguousQuanta.empty() && matrix.contiguousQuanta[0].latency.samples > 0;
    return matrix;
}

inline void PrintTransportCapabilityMatrix(const TransportCapabilityMatrix& matrix) {
    std::printf("\n=== Transport Capability Matrix ===\n");
    std::printf("device=%s ok=%d contention_ratio=%.2fx recommended_table_quantum=%zuB serialize_hot_lane=%d\n",
        matrix.deviceString.c_str(),
        matrix.ok ? 1 : 0,
        matrix.contentionRatio,
        matrix.recommendedTableQuantumBytes,
        matrix.recommendsSerializedHotLane ? 1 : 0);
    std::printf("[Contiguous ReadFreshExact]\n");
    for (const auto& row : matrix.contiguousQuanta) {
        std::printf("  %6zuB: samples=%zu fail=%zu min=%.1fus mean=%.1fus p95=%.1fus p99=%.1fus max=%.1fus\n",
            row.bytes, row.latency.samples, row.latency.failures,
            row.latency.minUs, row.latency.meanUs, row.latency.p95Us, row.latency.p99Us, row.latency.maxUs);
    }
    std::printf("[Scatter ExecuteRead+Clear]\n");
    for (const auto& row : matrix.scatterBatches) {
        std::printf("  %3zu reqs (%3zu distinct pages): samples=%zu fail=%zu min=%.1fus mean=%.1fus p95=%.1fus p99=%.1fus max=%.1fus\n",
            row.pages, row.distinctPages, row.latency.samples, row.latency.failures,
            row.latency.minUs, row.latency.meanUs, row.latency.p95Us, row.latency.p99Us, row.latency.maxUs);
    }
    std::printf("[Concurrency Contention (16 pages)]\n");
    std::printf("  1-thread mean=%.1fus p99=%.1fus | 2-thread primary mean=%.1fus p99=%.1fus | secondary mean=%.1fus\n",
        matrix.singleThreadScatter16.meanUs, matrix.singleThreadScatter16.p99Us,
        matrix.dualThreadScatter16Primary.meanUs, matrix.dualThreadScatter16Primary.p99Us,
        matrix.dualThreadScatter16Secondary.meanUs);
    std::printf("===================================\n");
}

} // namespace V2::Memory
