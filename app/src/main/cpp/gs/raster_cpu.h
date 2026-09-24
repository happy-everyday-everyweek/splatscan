#pragma once

#include <cstdint>
#include <vector>

#include "core/math_types.h"
#include "gs/gaussian_params.h"

namespace splatscan {

/** 相机内参。扫描时用设备相机的主点与焦距推导，训练分辨率下等比缩放。 */
struct CameraIntrinsics {
    float fx = 500.0f;
    float fy = 500.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    int32_t width = 320;
    int32_t height = 240;
};

/** 一帧 RGB 图像，通道为交错的 8 位。 */
struct ImageFrame {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> rgb;
};

struct RasterOptions {
    float minDepth = 0.05f;
    float alphaCutoff = 0.0039f;
    float sigmaCutoff = 3.0f;
};

/**
 * CPU 可微光栅化。
 *
 * 逐像素按深度顺序做前向 alpha 合成，反向传播按标准前向递归求导（含透射率耦合项），
 * 并把屏幕空间的梯度经投影雅可比映射回世界坐标与对数尺度。
 * 单线程实现，训练分辨率下开销可控；并行化与 GPU 版本可以替换 [render] 与 [trainStep]。
 */
class CpuRasterizer {
public:
    explicit CpuRasterizer(RasterOptions options = RasterOptions());

    /** 只做前向，用于预览。 */
    void render(const GaussianParams& model, const Pose& pose, const CameraIntrinsics& camera,
                ImageFrame& out);

    /**
     * 前向 + 反向：把渲染结果与 target 的平方误差传回模型梯度。
     * 返回该帧的平均平方误差。
     */
    float trainStep(GaussianParams& model, const Pose& pose, const CameraIntrinsics& camera,
                    const ImageFrame& target);

private:
    struct VisibleGaussian {
        int32_t index = 0;
        float depth = 0.0f;
        float camX = 0.0f;
        float camY = 0.0f;
        float camZ = 1.0f;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float radius = 0.0f;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float scaleXWorld = 1.0f;
        float scaleYWorld = 1.0f;
        float invScaleX2 = 1.0f;
        float invScaleY2 = 1.0f;
        float alphaScale = 0.0f;
    };

    void project(const GaussianParams& model, const Pose& pose, const CameraIntrinsics& camera,
                 std::vector<VisibleGaussian>& out);

    RasterOptions options_;
};

}  // namespace splatscan