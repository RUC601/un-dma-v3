#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace OW::AcquisitionTelemetry {

struct CycleSample {
    double hotPrepareMs = 0.0;
    double cameraPrepareMs = 0.0;
    double fieldExecClearMs = 0.0;
    double guardPrepareMs = 0.0;
    double guardExecClearMs = 0.0;
    double dmaWaitMs = 0.0;
    double coldTotalMs = 0.0;
    double coldDiscoveryMs = 0.0;
    double coldMaintenanceMs = 0.0;
    size_t fieldDistinctPages = 0;
    size_t guardDistinctPages = 0;
    size_t fieldScatterReqs = 0;
    size_t guardScatterReqs = 0;
    uint32_t registryCapacity = 0;
    double cameraAgeMs = 0.0;
    bool tailCameraRefreshed = false;
};

struct PhaseWindow {
    size_t cycles = 0;
    double hotPrepareSumMs = 0.0, hotPrepareMaxMs = 0.0;
    double cameraPrepareSumMs = 0.0, cameraPrepareMaxMs = 0.0;
    double fieldExecClearSumMs = 0.0, fieldExecClearMaxMs = 0.0;
    double guardPrepareSumMs = 0.0, guardPrepareMaxMs = 0.0;
    double guardExecClearSumMs = 0.0, guardExecClearMaxMs = 0.0;
    double dmaWaitSumMs = 0.0, dmaWaitMaxMs = 0.0;
    double coldTotalSumMs = 0.0, coldTotalMaxMs = 0.0;
    double coldDiscoverySumMs = 0.0, coldDiscoveryMaxMs = 0.0;
    double coldMaintenanceSumMs = 0.0, coldMaintenanceMaxMs = 0.0;
    uint64_t fieldPagesSum = 0;
    size_t fieldPagesMax = 0;
    uint64_t guardPagesSum = 0;
    size_t guardPagesMax = 0;
    uint64_t fieldReqsSum = 0;
    size_t fieldReqsMax = 0;
    uint64_t guardReqsSum = 0;
    size_t guardReqsMax = 0;
    uint32_t registryCapacityMax = 0;
    double cameraAgeSumMs = 0.0, cameraAgeMaxMs = 0.0;
    uint64_t tailCameraRefreshes = 0;

    void Record(const CycleSample& sample) noexcept {
        ++cycles;
        auto accum = [](double& sum, double& peak, double value) {
            sum += value;
            peak = (std::max)(peak, value);
        };
        accum(hotPrepareSumMs, hotPrepareMaxMs, sample.hotPrepareMs);
        accum(cameraPrepareSumMs, cameraPrepareMaxMs, sample.cameraPrepareMs);
        accum(fieldExecClearSumMs, fieldExecClearMaxMs, sample.fieldExecClearMs);
        accum(guardPrepareSumMs, guardPrepareMaxMs, sample.guardPrepareMs);
        accum(guardExecClearSumMs, guardExecClearMaxMs, sample.guardExecClearMs);
        accum(dmaWaitSumMs, dmaWaitMaxMs, sample.dmaWaitMs);
        accum(coldTotalSumMs, coldTotalMaxMs, sample.coldTotalMs);
        accum(coldDiscoverySumMs, coldDiscoveryMaxMs, sample.coldDiscoveryMs);
        accum(coldMaintenanceSumMs, coldMaintenanceMaxMs, sample.coldMaintenanceMs);
        fieldPagesSum += sample.fieldDistinctPages;
        fieldPagesMax = (std::max)(fieldPagesMax, sample.fieldDistinctPages);
        guardPagesSum += sample.guardDistinctPages;
        guardPagesMax = (std::max)(guardPagesMax, sample.guardDistinctPages);
        fieldReqsSum += sample.fieldScatterReqs;
        fieldReqsMax = (std::max)(fieldReqsMax, sample.fieldScatterReqs);
        guardReqsSum += sample.guardScatterReqs;
        guardReqsMax = (std::max)(guardReqsMax, sample.guardScatterReqs);
        registryCapacityMax = (std::max)(registryCapacityMax, sample.registryCapacity);
        accum(cameraAgeSumMs, cameraAgeMaxMs, sample.cameraAgeMs);
        if (sample.tailCameraRefreshed)
            ++tailCameraRefreshes;
    }

    double Mean(double sum) const noexcept {
        return cycles ? sum / static_cast<double>(cycles) : 0.0;
    }

    double MeanCount(uint64_t sum) const noexcept {
        return cycles ? static_cast<double>(sum) / static_cast<double>(cycles) : 0.0;
    }

    void Reset() noexcept { *this = {}; }
};

} // namespace OW::AcquisitionTelemetry
