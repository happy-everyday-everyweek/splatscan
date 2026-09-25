#pragma once

#include <cstdint>

namespace splatscan {

/**
 * nativeStatus 返回的 float 数组字段顺序。
 *
 * Kotlin 侧 SplatCore.Status 必须按同一顺序解析，所以顺序只在这里定义一次：
 * 之前原生与 Kotlin 各自写死 14 与下标，加一个字段要改两处，很容易错位。
 */
enum StatusField : int32_t {
    kStatusAcceptedFrames = 0,
    kStatusRejectedFrames = 1,
    kStatusStability = 2,
    kStatusCoverage = 3,
    kStatusGaussianCount = 4,
    kStatusRounds = 5,
    kStatusTrackedFeatures = 6,
    kStatusResidual = 7,
    kStatusModelMemoryMb = 8,
    kStatusMemoryCapped = 9,
    kStatusInitialized = 10,
    kStatusRingX = 11,
    kStatusRingY = 12,
    kStatusVulkanActive = 13,
    kStatusRenderedSplats = 14,
    kStatusRenderedTiles = 15,
    kStatusFieldCount = 16,
};

}  // namespace splatscan
