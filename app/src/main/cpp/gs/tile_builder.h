#pragma once

#include <cstdint>
#include <vector>

#include "core/math_types.h"
#include "gs/gaussian_params.h"
#include "gs/raster_cpu.h"

namespace splatscan {

/** 渲染瓦片边长。CPU 分桶与计算着色器必须用同一个值，所以只在这里定义一次。 */
constexpr int32_t kTileSize = 16;

/** 与计算着色器见到的布局一一对应：12 个 float，全部按 vec4 对齐。 */
struct SplatGpu {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float inverseScaleX2 = 1.0f;
    float inverseScaleY2 = 1.0f;
    float radius = 0.0f;
    float alphaScale = 0.0f;
    float padding0 = 0.0f;
    float padding1 = 0.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float padding2 = 0.0f;
};

/**
 * 一帧的 GPU 渲染数据：按深度排好序的高斯，加上按 tile 分好的索引桶。
 * 排序与分桶都在 CPU 做，GPU 只负责逐像素合成，这样最省显存与实现复杂度。
 */
struct TileBundle {
    int32_t width = 0;
    int32_t height = 0;
    int32_t tileSize = 16;
    int32_t tilesX = 0;
    int32_t tilesY = 0;
    std::vector<SplatGpu> splats;
    /** 每个 tile 两个元素：起始下标与数量。 */
    std::vector<uint32_t> tileRanges;
    std::vector<uint32_t> tileItems;

    void clear() {
        splats.clear();
        tileRanges.clear();
        tileItems.clear();
        tilesX = 0;
        tilesY = 0;
    }

    bool empty() const { return splats.empty() || tileItems.empty(); }
};

/** 投影、按深度排序并按 tile 分桶。可见性判定与 CPU 光栅化保持一致。 */
void buildTiles(const GaussianParams& model, const Pose& pose, const CameraIntrinsics& camera,
                int32_t tileSize, TileBundle& out);

}  // namespace splatscan