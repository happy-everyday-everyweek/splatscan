#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math_types.h"
#include "gs/gaussian_params.h"
#include "gs/raster_cpu.h"
#include "vio/feature_tracker.h"

namespace splatscan {

enum class EngineState { Idle, Scanning, Paused };

struct EngineConfig {
    int32_t trainWidth = 160;
    int32_t trainHeight = 120;
    int32_t maxGaussians = 40000;
    int32_t iterationsPerBatch = 4;
    int32_t maxFeatures = 200;
    float learningRate = 0.02f;
};

struct EngineStatus {
    int32_t acceptedFrames = 0;
    int32_t rejectedFrames = 0;
    float stability = 0.0f;
    float coverage = 0.0f;
    int32_t gaussianCount = 0;
    int32_t rounds = 0;
    int32_t trackedFeatures = 0;
    float residual = 1.0f;
    float modelMemoryMb = 0.0f;
    bool memoryCapped = false;
    bool initialized = false;
    float lastRingX = 0.5f;
    float lastRingY = 0.5f;
};

/**
 * 扫描引擎：把相机帧与惯性数据变成不断生长的高斯模型。
 *
 * 三条流程：初始化（首帧铺一层高斯）、跟踪（特征 + IMU 求位姿）、增量训练
 * （每个关键帧批次跑若干次可微光栅化迭代）。暂停时继续用已收下的帧反复优化。
 */
class ScanEngine {
public:
    explicit ScanEngine(EngineConfig config);

    void start();
    void pause();
    void stop();

    void submitFrame(const uint8_t* y, int32_t width, int32_t height, int32_t yStride,
                     const uint8_t* u, const uint8_t* v, int32_t uvStride, int32_t uvPixelStride,
                     int64_t timestampNs);

    void submitImu(float ax, float ay, float az, float gx, float gy, float gz,
                   int64_t timestampNs);

    /** 由界面按固定节奏调用：消化待处理帧、推进训练。 */
    void step();

    void renderPreview(std::vector<uint8_t>& rgba, int32_t width, int32_t height);

    bool writePly(const std::string& path) const;

    void reconfigure(int32_t trainWidth, int32_t trainHeight, int32_t maxGaussians,
                     int32_t iterationsPerBatch);

    const EngineStatus& status() const { return status_; }

private:
    void initializeFromFrame(const std::vector<uint8_t>& rgb);
    void processFrame(const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& gray);
    void trainOnImage(const std::vector<uint8_t>& rgb, const Pose& pose);
    void updateCoverage();
    static void downsample(const uint8_t* y, int32_t width, int32_t height, int32_t yStride,
                           const uint8_t* u, const uint8_t* v, int32_t uvStride,
                           int32_t uvPixelStride, int32_t outWidth, int32_t outHeight,
                           std::vector<uint8_t>& rgb, std::vector<uint8_t>& gray);

    EngineConfig config_;
    EngineStatus status_ = {};
    EngineState state_ = EngineState::Idle;

    GaussianParams model_;
    CpuRasterizer rasterizer_;
    FeatureTracker tracker_;
    CameraIntrinsics camera_ = {};

    Pose pose_ = {};
    Mat3 rotation_ = Mat3::identity();
    Vec3 gravity_{0.0f, 0.0f, 0.0f};
    Vec3 prevAcceleration_{0.0f, 0.0f, 0.0f};
    Vec3 velocity_{0.0f, 0.0f, 0.0f};
    int64_t lastImuNs_ = 0;

    std::vector<uint8_t> lastGray_;
    std::vector<uint8_t> lastRgb_;
    std::vector<FeaturePoint> features_;
    std::vector<FeatureTrack> tracks_;

    bool hasPendingFrame_ = false;
    std::vector<uint8_t> pendingRgb_;
    std::vector<uint8_t> pendingGray_;
    int64_t pendingTimestampNs_ = 0;

    int32_t gaussianCappedFrames_ = 0;
};

}  // namespace splatscan