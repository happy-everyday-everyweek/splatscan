#include "engine/scan_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "gs/ply_io.h"
#include "vio/pose_solver.h"

namespace splatscan {

namespace {

constexpr float kInitialDepth = 1.0f;
constexpr int32_t kCoverageBins = 8;
constexpr float kShC0 = 0.28209479177387814f;
/** 单帧相机位移上限。视觉解偶发错误时宁可不动，也不能把相机弹飞。 */
constexpr float kMaxCenterStep = 0.12f;

}  // namespace

ScanEngine::ScanEngine(EngineConfig config) : config_(config), rasterizer_() {
    model_.ensureCapacity(std::min(config_.maxGaussians, 4096));
    camera_.width = config_.trainWidth;
    camera_.height = config_.trainHeight;
    camera_.fx = static_cast<float>(config_.trainWidth) * 1.2f;
    camera_.fy = camera_.fx;
    camera_.cx = static_cast<float>(config_.trainWidth) * 0.5f;
    camera_.cy = static_cast<float>(config_.trainHeight) * 0.5f;
    lastGray_.assign(static_cast<size_t>(config_.trainWidth) * config_.trainHeight, 0);
    lastRgb_.assign(static_cast<size_t>(config_.trainWidth) * config_.trainHeight * 3, 0);
}

void ScanEngine::start() {
    if (state_ == EngineState::Scanning) return;
    state_ = EngineState::Scanning;
}

ScanEngine::~ScanEngine() { detachSurface(); }

bool ScanEngine::attachSurface(ANativeWindow* window) {
    if (window == nullptr) return false;

    std::lock_guard<std::mutex> renderLock(rendererMutex_);
    if (rendererReady_) return true;

    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    window_ = window;

    if (!renderer_.initialize(window, config_.trainWidth, config_.trainHeight)) {
        ANativeWindow_release(window_);
        window_ = nullptr;
        status_.vulkanActive = false;
        return false;
    }

    rendererReady_ = true;
    status_.vulkanActive = true;
    renderRunning_.store(true);
    renderThread_ = std::thread([this]() { renderLoop(); });
    return true;
}

void ScanEngine::detachSurface() {
    renderRunning_.store(false);
    if (renderThread_.joinable()) {
        renderThread_.join();
    }

    std::lock_guard<std::mutex> renderLock(rendererMutex_);
    if (rendererReady_) {
        renderer_.shutdown();
        rendererReady_ = false;
    }
    status_.vulkanActive = false;
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

void ScanEngine::renderLoop() {
    using namespace std::chrono_literals;
    TileBundle bundle;
    while (renderRunning_.load()) {
        {
            std::lock_guard<std::mutex> lock(modelMutex_);
            if (model_.count > 0) {
                buildTiles(model_, pose_, camera_, 16, bundle);
            } else {
                bundle.clear();
            }
        }

        if (!bundle.empty()) {
            std::lock_guard<std::mutex> renderLock(rendererMutex_);
            if (rendererReady_) {
                renderer_.renderFrame(bundle);
            }
        }
        std::this_thread::sleep_for(16ms);
    }
}

void ScanEngine::pause() {
    if (state_ != EngineState::Scanning) return;
    state_ = EngineState::Paused;
}

void ScanEngine::stop() {
    state_ = EngineState::Idle;
    hasPendingFrame_ = false;
}

void ScanEngine::reconfigure(int32_t trainWidth, int32_t trainHeight, int32_t maxGaussians,
                             int32_t iterationsPerBatch) {
    if (trainWidth == config_.trainWidth && trainHeight == config_.trainHeight &&
        maxGaussians == config_.maxGaussians &&
        iterationsPerBatch == config_.iterationsPerBatch) {
        return;
    }
    config_.trainWidth = trainWidth;
    config_.trainHeight = trainHeight;
    config_.maxGaussians = maxGaussians;
    config_.iterationsPerBatch = iterationsPerBatch;
    camera_.width = trainWidth;
    camera_.height = trainHeight;
    camera_.fx = static_cast<float>(trainWidth) * 1.2f;
    camera_.fy = camera_.fx;
    camera_.cx = static_cast<float>(trainWidth) * 0.5f;
    camera_.cy = static_cast<float>(trainHeight) * 0.5f;
    model_.ensureCapacity(std::min(maxGaussians, 4096));
    hasPendingFrame_ = false;

    {
        std::lock_guard<std::mutex> renderLock(rendererMutex_);
        if (rendererReady_) {
            renderer_.setRenderSize(trainWidth, trainHeight);
        }
    }
}

void ScanEngine::submitImu(float ax, float ay, float az, float gx, float gy, float gz,
                           int64_t timestampNs) {
    const Vec3 acceleration{ax, ay, az};
    const Vec3 gyro{gx, gy, gz};

    // 第一帧直接把读数当作重力方向，避免重力估计从零开始收敛期间把重力当成运动加速度
    if (lastImuNs_ == 0) {
        gravity_ = acceleration;
        lastImuNs_ = timestampNs;
        return;
    }

    float dt = 0.0f;
    if (timestampNs > lastImuNs_) {
        dt = static_cast<float>(timestampNs - lastImuNs_) * 1e-9f;
        if (dt > 0.2f) dt = 0.0f;
    }
    lastImuNs_ = timestampNs;

    // 重力方向用低通估计
    gravity_ = gravity_ * 0.95f + acceleration * 0.05f;

    if (dt > 0.0f) {
        rotation_ = PoseSolver::integrateRotation(rotation_, gyro, dt, gravity_);
    }

    // 位移完全交给视觉求解：加速度计在手持场景下二次积分会迅速发散，
    // 之前把比力当运动加速度积分正是相机被弹飞的原因。
    velocity_ = velocity_ * 0.9f + (acceleration - gravity_) * dt;
    prevAcceleration_ = acceleration;
}

void ScanEngine::submitFrame(const uint8_t* y, int32_t width, int32_t height, int32_t yStride,
                             const uint8_t* u, const uint8_t* v, int32_t uvStride,
                             int32_t uvPixelStride, int64_t timestampNs) {
    if (state_ != EngineState::Scanning) return;
    if (width <= 0 || height <= 0) return;

    downsample(y, width, height, yStride, u, v, uvStride, uvPixelStride, config_.trainWidth,
               config_.trainHeight, pendingRgb_, pendingGray_);
    pendingTimestampNs_ = timestampNs;
    hasPendingFrame_ = true;
}

void ScanEngine::downsample(const uint8_t* y, int32_t width, int32_t height, int32_t yStride,
                            const uint8_t* u, const uint8_t* v, int32_t uvStride,
                            int32_t uvPixelStride, int32_t outWidth, int32_t outHeight,
                            std::vector<uint8_t>& rgb, std::vector<uint8_t>& gray) {
    rgb.assign(static_cast<size_t>(outWidth) * outHeight * 3, 0);
    gray.assign(static_cast<size_t>(outWidth) * outHeight, 0);
    const float scaleX = static_cast<float>(width) / static_cast<float>(outWidth);
    const float scaleY = static_cast<float>(height) / static_cast<float>(outHeight);

    for (int32_t oy = 0; oy < outHeight; ++oy) {
        const int32_t sy = std::min(height - 1, static_cast<int32_t>((oy + 0.5f) * scaleY));
        for (int32_t ox = 0; ox < outWidth; ++ox) {
            const int32_t sx = std::min(width - 1, static_cast<int32_t>((ox + 0.5f) * scaleX));
            const int32_t yValue = y[sy * yStride + sx];
            const size_t outIndex = static_cast<size_t>(oy) * outWidth + ox;
            gray[outIndex] = static_cast<uint8_t>(yValue);

            int32_t chromaY = sy / 2;
            int32_t chromaX = sx / 2;
            const int32_t uIndex = chromaY * uvStride + chromaX * uvPixelStride;
            const int32_t vIndex = chromaY * uvStride + chromaX * uvPixelStride;
            const int32_t uValue = u != nullptr ? u[uIndex] - 128 : 0;
            const int32_t vValue = v != nullptr ? v[vIndex] - 128 : 0;

            // BT.601 limited range
            const float yf = static_cast<float>(yValue);
            const float r = yf + 1.402f * static_cast<float>(vValue);
            const float g = yf - 0.344136f * static_cast<float>(uValue) -
                            0.714136f * static_cast<float>(vValue);
            const float b = yf + 1.772f * static_cast<float>(uValue);
            rgb[outIndex * 3 + 0] = static_cast<uint8_t>(clampf(r, 0.0f, 255.0f));
            rgb[outIndex * 3 + 1] = static_cast<uint8_t>(clampf(g, 0.0f, 255.0f));
            rgb[outIndex * 3 + 2] = static_cast<uint8_t>(clampf(b, 0.0f, 255.0f));
        }
    }
}

void ScanEngine::initializeFromFrame(const std::vector<uint8_t>& rgb) {
    model_.count = 0;
    const int32_t step = 2;
    const float scale = 0.01f;
    for (int32_t y = step / 2; y < config_.trainHeight; y += step) {
        for (int32_t x = step / 2; x < config_.trainWidth; x += step) {
            if (model_.count >= config_.maxGaussians) break;
            const size_t index = (static_cast<size_t>(y) * config_.trainWidth + x) * 3;
            const float px = (static_cast<float>(x) - camera_.cx) / camera_.fx * kInitialDepth;
            const float py = (static_cast<float>(y) - camera_.cy) / camera_.fy * kInitialDepth;
            // 球谐 DC 与颜色的关系是 rgb = 0.5 + SH_C0 * f_dc，
            // 这里必须除以 SH_C0，首帧渲染才能和相机画面一致。
            const Vec3 color{
                (static_cast<float>(rgb[index]) / 255.0f - 0.5f) / kShC0,
                (static_cast<float>(rgb[index + 1]) / 255.0f - 0.5f) / kShC0,
                (static_cast<float>(rgb[index + 2]) / 255.0f - 0.5f) / kShC0,
            };
            model_.add({px, py, kInitialDepth}, std::log(scale), std::log(scale), color, 2.0f);
        }
    }
    cameraCenter_ = {0.0f, 0.0f, 0.0f};
    cameraRotation_ = rotation_;
    previousRotation_ = rotation_;
    updatePoseFromCamera();
    status_.initialized = model_.count > 0;
}

void ScanEngine::updatePoseFromCamera() {
    pose_.rotation = cameraRotation_;
    const Vec3 rotated = cameraRotation_ * cameraCenter_;
    pose_.translation = {-rotated.x, -rotated.y, -rotated.z};
}

void ScanEngine::processFrame(const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& gray) {
    if (!status_.initialized) {
        initializeFromFrame(rgb);
        lastGray_ = gray;
        lastRgb_ = rgb;
        features_.clear();
        tracker_.detect(lastGray_.data(), config_.trainWidth, config_.trainHeight,
                        config_.trainWidth, features_, config_.maxFeatures);
        ++status_.acceptedFrames;
        trainOnImage(rgb, pose_);
        lastGray_ = gray;
        return;
    }

    tracks_.clear();
    tracks_.reserve(features_.size());
    for (const FeaturePoint& point : features_) {
        FeatureTrack track;
        track.previous = point;
        tracks_.push_back(track);
    }

    const int32_t tracked = tracker_.track(lastGray_.data(), gray.data(), config_.trainWidth,
                                           config_.trainHeight, config_.trainWidth, tracks_);
    // 相对旋转：把上一帧的相机坐标旋到当前帧。对极约束里的平移因此是相对量。
    const Mat3 relativeRotation = rotation_ * previousRotation_.transpose();

    // 平移的绝对尺度单目不可观测。用特征视差估计更靠谱：
    // 对深度约 kInitialDepth 的场景，位移量级与归一化视差同阶。
    float parallaxSum = 0.0f;
    int32_t parallaxCount = 0;
    for (const FeatureTrack& track : tracks_) {
        if (!track.valid) continue;
        const float dx = (track.current.x - track.previous.x) / camera_.fx;
        const float dy = (track.current.y - track.previous.y) / camera_.fy;
        parallaxSum += std::sqrt(dx * dx + dy * dy);
        ++parallaxCount;
    }
    const float medianParallax =
        parallaxCount > 0 ? parallaxSum / static_cast<float>(parallaxCount) : 0.0f;
    const float scaleHint =
        clampf(medianParallax * kInitialDepth, 0.002f, kMaxCenterStep * 0.8f);

    const PoseSolution solution =
        PoseSolver::solveTranslation(tracks_, camera_, relativeRotation, scaleHint);

    status_.trackedFeatures = tracked;
    bool poseAccepted = false;

    if (solution.valid && tracked >= 20) {
        const Vec3 deltaCenter = -(relativeRotation.transpose() * solution.translation);
        if (length(deltaCenter) < kMaxCenterStep) {
            cameraCenter_ = cameraCenter_ + deltaCenter;
            cameraRotation_ = rotation_;
            previousRotation_ = rotation_;
            poseAccepted = true;
            status_.stability = clampf(solution.inlierRatio, 0.0f, 1.0f);
        }
    }
    if (!poseAccepted && tracked >= 8) {
        // 跟踪够用但平移解不可信：只更新旋转，位移保持不变
        cameraRotation_ = rotation_;
        previousRotation_ = rotation_;
        poseAccepted = true;
        status_.stability = clampf(static_cast<float>(tracked) / 60.0f, 0.0f, 1.0f);
    }
    if (poseAccepted) {
        ++status_.acceptedFrames;
    } else {
        ++status_.rejectedFrames;
        status_.stability = 0.0f;
    }
    updatePoseFromCamera();

    lastGray_ = gray;
    lastRgb_ = rgb;

    // 特征点过少时重新检测，跟随场景变化
    if (tracked < config_.maxFeatures / 2) {
        features_.clear();
        tracker_.detect(lastGray_.data(), config_.trainWidth, config_.trainHeight,
                        config_.trainWidth, features_, config_.maxFeatures);
    } else {
        std::vector<FeaturePoint> refreshed;
        refreshed.reserve(tracks_.size());
        for (const FeatureTrack& track : tracks_) {
            if (track.valid) refreshed.push_back(track.current);
        }
        features_.swap(refreshed);
    }

    // 位姿不可信的帧不参与优化：错误位姿会把模型拉成噪声
    if (poseAccepted) {
        trainOnImage(rgb, pose_);
    }
}

void ScanEngine::trainOnImage(const std::vector<uint8_t>& rgb, const Pose& pose) {
    ImageFrame target;
    target.width = config_.trainWidth;
    target.height = config_.trainHeight;
    target.rgb = rgb;

    float residual = status_.residual;
    const int32_t iterations = std::max(1, config_.iterationsPerBatch);
    for (int32_t i = 0; i < iterations; ++i) {
        residual = rasterizer_.trainStep(model_, pose, camera_, target);
        model_.adamStep(config_.learningRate, 0.9f, 0.999f, 1e-8f);
    }
    status_.residual = status_.residual * 0.7f + residual * 0.3f;
    status_.rounds += iterations;

    // 稠密化与剪枝：按观测次数与误差决定，规模受配置上限约束
    if (status_.rounds % 16 == 0) {
        for (int32_t i = 0; i < model_.count; ++i) {
            const float gx = model_.gradPosition[i * 3 + 0];
            const float gy = model_.gradPosition[i * 3 + 1];
            const float gz = model_.gradPosition[i * 3 + 2];
            if (std::fabs(gx) + std::fabs(gy) + std::fabs(gz) > 1e-4f) {
                model_.observationCount[i] += 1;
                model_.meanError[i] = model_.meanError[i] * 0.8f + status_.residual * 0.2f;
            }
        }
        const int32_t before = model_.count;
        model_.densify(0.005f, 2, config_.maxGaussians);
        model_.prune(0.005f, 1.0f);
        if (before != model_.count) status_.memoryCapped = model_.count >= config_.maxGaussians;
    }

    status_.gaussianCount = model_.count;
    status_.modelMemoryMb = static_cast<float>(model_.memoryBytes() / (1024.0 * 1024.0));
}

void ScanEngine::updateCoverage() {
    // 覆盖度：把高斯投影到训练分辨率下的网格里，统计被占用的格子比例
    int32_t bins[kCoverageBins * kCoverageBins] = {0};
    int32_t occupied = 0;
    for (int32_t i = 0; i < model_.count; ++i) {
        const Vec3 world{model_.position[i * 3 + 0], model_.position[i * 3 + 1],
                         model_.position[i * 3 + 2]};
        const Vec3 cameraSpace = pose_.transform(world);
        if (cameraSpace.z <= 0.05f) continue;
        const float u = camera_.fx * cameraSpace.x / cameraSpace.z + camera_.cx;
        const float v = camera_.fy * cameraSpace.y / cameraSpace.z + camera_.cy;
        if (u < 0.0f || v < 0.0f || u >= config_.trainWidth || v >= config_.trainHeight) continue;
        const int32_t bx = std::min(kCoverageBins - 1,
                                    static_cast<int32_t>(u * kCoverageBins / config_.trainWidth));
        const int32_t by = std::min(kCoverageBins - 1,
                                    static_cast<int32_t>(v * kCoverageBins / config_.trainHeight));
        if (bins[by * kCoverageBins + bx] == 0) {
            bins[by * kCoverageBins + bx] = 1;
            ++occupied;
        }
    }
    status_.coverage = static_cast<float>(occupied) /
                       static_cast<float>(kCoverageBins * kCoverageBins);
}

void ScanEngine::step() {
    std::lock_guard<std::mutex> lock(modelMutex_);
    if (hasPendingFrame_) {
        hasPendingFrame_ = false;
        if (state_ == EngineState::Scanning) {
            processFrame(pendingRgb_, pendingGray_);
        }
    } else if (state_ == EngineState::Paused && status_.initialized && !lastRgb_.empty()) {
        // 暂停时继续优化已经收下的帧，画质在等待中提升
        trainOnImage(lastRgb_, pose_);
    }

    updateCoverage();
    status_.gaussianCount = model_.count;
    status_.modelMemoryMb = static_cast<float>(model_.memoryBytes() / (1024.0 * 1024.0));

    // 记录当前扫描点在画面上的位置，供界面上的圆环使用
    const Vec3 origin = pose_.transform(Vec3{0.0f, 0.0f, 0.0f});
    if (origin.z > 0.05f) {
        const float u = camera_.fx * origin.x / origin.z + camera_.cx;
        const float v = camera_.fy * origin.y / origin.z + camera_.cy;
        status_.lastRingX = clampf(u / static_cast<float>(config_.trainWidth), 0.0f, 1.0f);
        status_.lastRingY = clampf(v / static_cast<float>(config_.trainHeight), 0.0f, 1.0f);
    }
}

void ScanEngine::renderPreview(std::vector<uint8_t>& rgba, int32_t width, int32_t height) {
    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    if (!status_.initialized || model_.count == 0) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i] = 16;
            rgba[i + 1] = 16;
            rgba[i + 2] = 20;
            rgba[i + 3] = 255;
        }
        return;
    }

    ImageFrame frame;
    rasterizer_.render(model_, pose_, camera_, frame);
    for (int32_t y = 0; y < height; ++y) {
        const int32_t sy = std::min(config_.trainHeight - 1, y * config_.trainHeight / height);
        for (int32_t x = 0; x < width; ++x) {
            const int32_t sx = std::min(config_.trainWidth - 1, x * config_.trainWidth / width);
            const size_t source = (static_cast<size_t>(sy) * config_.trainWidth + sx) * 3;
            const size_t target = (static_cast<size_t>(y) * width + x) * 4;
            rgba[target] = frame.rgb[source];
            rgba[target + 1] = frame.rgb[source + 1];
            rgba[target + 2] = frame.rgb[source + 2];
            rgba[target + 3] = 255;
        }
    }
}
bool ScanEngine::loadModel(const std::string& path) {
    std::lock_guard<std::mutex> lock(modelMutex_);
    GaussianParams loaded;
    Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 1.0f;
    if (!readPlyModel(path, loaded, center, radius)) {
        return false;
    }
    model_ = std::move(loaded);
    viewerFocus_ = center;
    viewerRadius_ = radius;
    viewerZoom_ = 1.0f;
    viewerYaw_ = 0.0f;
    viewerPitch_ = 0.35f;
    status_.gaussianCount = model_.count;
    status_.initialized = model_.count > 0;
    status_.modelMemoryMb = static_cast<float>(model_.memoryBytes() / (1024.0 * 1024.0));
    updateViewerPose();
    return true;
}

void ScanEngine::setViewerMode(bool enabled) {
    std::lock_guard<std::mutex> lock(modelMutex_);
    viewerMode_ = enabled;
    if (enabled) {
        updateViewerPose();
    }
}

void ScanEngine::setViewerOrbit(float yawRadians, float pitchRadians, float zoom) {
    std::lock_guard<std::mutex> lock(modelMutex_);
    viewerYaw_ = yawRadians;
    viewerPitch_ = clampf(pitchRadians, -1.4f, 1.4f);
    viewerZoom_ = clampf(zoom, 0.25f, 8.0f);
    updateViewerPose();
}

void ScanEngine::updateViewerPose() {
    const float distance = viewerRadius_ * 2.5f * viewerZoom_;
    const float cosPitch = std::cos(viewerPitch_);
    const float sinPitch = std::sin(viewerPitch_);
    const float cosYaw = std::cos(viewerYaw_);
    const float sinYaw = std::sin(viewerYaw_);

    const Vec3 offset{sinYaw * cosPitch, sinPitch, cosYaw * cosPitch};
    const Vec3 cameraPosition = viewerFocus_ + offset * distance;

    // 相机看向焦点：相机空间的 z 轴指向相机方向，与投影约定一致
    const Vec3 zAxis = normalize(cameraPosition - viewerFocus_);
    Vec3 up{0.0f, 1.0f, 0.0f};
    if (std::fabs(dot(zAxis, up)) > 0.98f) up = Vec3{0.0f, 0.0f, 1.0f};
    const Vec3 xAxis = normalize(cross(up, zAxis));
    const Vec3 yAxis = cross(zAxis, xAxis);

    Mat3 rotation;
    rotation.m[0] = xAxis.x;
    rotation.m[1] = xAxis.y;
    rotation.m[2] = xAxis.z;
    rotation.m[3] = yAxis.x;
    rotation.m[4] = yAxis.y;
    rotation.m[5] = yAxis.z;
    rotation.m[6] = zAxis.x;
    rotation.m[7] = zAxis.y;
    rotation.m[8] = zAxis.z;

    pose_.rotation = rotation;
    const Vec3 rotated = rotation * cameraPosition;
    pose_.translation = {-rotated.x, -rotated.y, -rotated.z};
}

bool ScanEngine::writePly(const std::string& path) const {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;

    std::fprintf(file,
                 "ply\nformat binary_little_endian 1.0\ncomment Generated by SplatScan\n"
                 "element vertex %d\n"
                 "property float x\nproperty float y\nproperty float z\n"
                 "property float nx\nproperty float ny\nproperty float nz\n"
                 "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
                 "property float opacity\n"
                 "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
                 "property float rot_0\nproperty float rot_1\nproperty float rot_2\n"
                 "property float rot_3\nend_header\n",
                 model_.count);

    for (int32_t i = 0; i < model_.count; ++i) {
        const float values[17] = {
            model_.position[i * 3 + 0],
            model_.position[i * 3 + 1],
            model_.position[i * 3 + 2],
            0.0f,
            0.0f,
            0.0f,
            model_.color[i * 3 + 0],
            model_.color[i * 3 + 1],
            model_.color[i * 3 + 2],
            model_.opacity[i],
            model_.logScale[i * 2 + 0],
            model_.logScale[i * 2 + 1],
            model_.logScale[i * 2 + 1],
            1.0f,
            0.0f,
            0.0f,
            0.0f,
        };
        std::fwrite(values, sizeof(float), 17, file);
    }
    std::fclose(file);
    return true;
}

}  // namespace splatscan