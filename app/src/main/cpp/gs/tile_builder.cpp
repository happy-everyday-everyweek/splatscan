#include "gs/tile_builder.h"

#include <algorithm>
#include <cmath>

namespace splatscan {

namespace {

inline float sigmoidf(float value) { return 1.0f / (1.0f + std::exp(-value)); }

struct SortedSplat {
    float depth = 0.0f;
    SplatGpu gpu;
};

}  // namespace

void buildTiles(const GaussianParams& model, const Pose& pose, const CameraIntrinsics& camera,
                int32_t tileSize, TileBundle& out) {
    out.clear();
    if (tileSize <= 0) tileSize = 16;

    out.width = camera.width;
    out.height = camera.height;
    out.tileSize = tileSize;
    out.tilesX = (camera.width + tileSize - 1) / tileSize;
    out.tilesY = (camera.height + tileSize - 1) / tileSize;

    std::vector<SortedSplat> visible;
    visible.reserve(static_cast<size_t>(model.count));

    for (int32_t i = 0; i < model.count; ++i) {
        const Vec3 world{model.position[i * 3 + 0], model.position[i * 3 + 1],
                         model.position[i * 3 + 2]};
        const Vec3 cameraSpace = pose.transform(world);
        if (cameraSpace.z <= 0.05f) continue;

        const float inverseZ = 1.0f / cameraSpace.z;
        const float u = camera.fx * cameraSpace.x * inverseZ + camera.cx;
        const float v = camera.fy * cameraSpace.y * inverseZ + camera.cy;

        const float scaleXWorld = std::exp(model.logScale[i * 2 + 0]);
        const float scaleYWorld = std::exp(model.logScale[i * 2 + 1]);
        const float scaleX = std::max(scaleXWorld * camera.fx * inverseZ, 0.3f);
        const float scaleY = std::max(scaleYWorld * camera.fy * inverseZ, 0.3f);
        const float radius = 3.0f * std::max(scaleX, scaleY);

        if (u + radius < 0.0f || u - radius > static_cast<float>(camera.width)) continue;
        if (v + radius < 0.0f || v - radius > static_cast<float>(camera.height)) continue;

        SortedSplat splat;
        splat.depth = cameraSpace.z;
        splat.gpu.centerX = u;
        splat.gpu.centerY = v;
        splat.gpu.inverseScaleX2 = 1.0f / (scaleX * scaleX);
        splat.gpu.inverseScaleY2 = 1.0f / (scaleY * scaleY);
        splat.gpu.radius = radius;
        splat.gpu.alphaScale = sigmoidf(model.opacity[i]);
        splat.gpu.red = model.color[i * 3 + 0];
        splat.gpu.green = model.color[i * 3 + 1];
        splat.gpu.blue = model.color[i * 3 + 2];
        visible.push_back(splat);
    }

    if (visible.empty()) return;

    std::sort(visible.begin(), visible.end(),
              [](const SortedSplat& a, const SortedSplat& b) { return a.depth < b.depth; });

    out.splats.reserve(visible.size());
    for (const SortedSplat& splat : visible) {
        out.splats.push_back(splat.gpu);
    }

    // 先算每个 tile 的高斯数量，再做前缀和，然后按顺序填索引。
    const size_t tileCount = static_cast<size_t>(out.tilesX) * out.tilesY;
    std::vector<uint32_t> counts(tileCount, 0);

    for (const SortedSplat& splat : visible) {
        const float radius = splat.gpu.radius;
        const int32_t minX = std::max(0, static_cast<int32_t>(std::floor(splat.gpu.centerX - radius)) / tileSize);
        const int32_t maxX = std::min(out.tilesX - 1,
                                      static_cast<int32_t>(std::floor(splat.gpu.centerX + radius)) / tileSize);
        const int32_t minY = std::max(0, static_cast<int32_t>(std::floor(splat.gpu.centerY - radius)) / tileSize);
        const int32_t maxY = std::min(out.tilesY - 1,
                                      static_cast<int32_t>(std::floor(splat.gpu.centerY + radius)) / tileSize);
        for (int32_t ty = minY; ty <= maxY; ++ty) {
            for (int32_t tx = minX; tx <= maxX; ++tx) {
                ++counts[static_cast<size_t>(ty) * out.tilesX + tx];
            }
        }
    }

    out.tileRanges.resize(tileCount * 2);
    uint32_t running = 0;
    for (size_t i = 0; i < tileCount; ++i) {
        out.tileRanges[i * 2] = running;
        out.tileRanges[i * 2 + 1] = counts[i];
        running += counts[i];
    }
    out.tileItems.resize(running);

    std::vector<uint32_t> cursor(tileCount);
    for (size_t i = 0; i < tileCount; ++i) {
        cursor[i] = out.tileRanges[i * 2];
    }

    for (uint32_t index = 0; index < out.splats.size(); ++index) {
        const SplatGpu& splat = out.splats[index];
        const float radius = splat.radius;
        const int32_t minX = std::max(0, static_cast<int32_t>(std::floor(splat.centerX - radius)) / tileSize);
        const int32_t maxX = std::min(out.tilesX - 1,
                                      static_cast<int32_t>(std::floor(splat.centerX + radius)) / tileSize);
        const int32_t minY = std::max(0, static_cast<int32_t>(std::floor(splat.centerY - radius)) / tileSize);
        const int32_t maxY = std::min(out.tilesY - 1,
                                      static_cast<int32_t>(std::floor(splat.centerY + radius)) / tileSize);
        for (int32_t ty = minY; ty <= maxY; ++ty) {
            for (int32_t tx = minX; tx <= maxX; ++tx) {
                const size_t tile = static_cast<size_t>(ty) * out.tilesX + tx;
                out.tileItems[cursor[tile]++] = index;
            }
        }
    }
}

}  // namespace splatscan