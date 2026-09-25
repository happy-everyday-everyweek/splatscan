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
/** 多视图训练时保留的历史帧数量。留最近几帧就够形成基线，内存开销也只有几百 KB。 */
constexpr size_t kMaxViews = 6;
/** 等深度模型的最多帧数；超时就用平面初值兜底，绝不能让扫描卡住。 */
constexpr int32_t kDepthWaitFrames = 40;
/** 深度模型输出的是「越大越近」的视差型数值，转成距离要取倒数。 */
constexpr bool kDepthIsDisparity = true;

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
    // 从「空闲」重新开始扫描时要清空旧模型：否则第二次扫描会接着上一次的模型继续长。
    const bool freshScan = state_ == EngineState::Idle;
    state_ = EngineState::Scanning;
    if (!freshScan) return;

    model_.count = 0;
    status_.initialized = false;
    views_.clear();
    translationScale_ = 0.0f;
    depthReady_ = false;
    depthFailed_ = false;
    depthMap_.clear();
    depthWidth_ = 0;
    depthHeight_ = 0;
    firstFrameStored_ = false;
    firstRgb_.clear();
    waitingFrames_ = 0;
}

void ScanEngine::markDepthUnavailable() {
    std::lock_guard<std::mutex> lock(modelMutex_);
    depthFailed_ = true;
}

bool ScanEngine::submitDepth(int32_t width, int32_t height, const float* depth, int32_t count) {
    if (width <= 0 || height <= 0 || depth == nullptr ||
        count < static_cast<int32_t>(static_cast<int64_t>(width) * height)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(modelMutex_);
    depthWidth_ = width;
    depthHeight_ = height;
    depthMap_.assign(depth, depth + static_cast<size_t>(width) * static_cast<size_t>(height));
    depthReady_ = true;
    depthFailed_ = false;

    // 深度晚到也没关系：用留存的帧与朝向重建初模，把兜底的平面模型换掉。
    if (firstFrameStored_ && !firstRgb_.empty()) {
        initializeFromDepth(firstRgb_);
    }
    return true;
}

/**
 * 用单目深度模型的结果铺初模。
 *
 * 与平面初值的区别在于：每个高斯的位置由「像素 + 该像素的距离」反投影得到，
 * 所以模型一出生就是三维的，不会掉进「贴着相机的平面」这个退化解。
 */
void ScanEngine::initializeFromDepth(const std::vector<uint8_t>& rgb) {
    const int32_t outWidth = config_.trainWidth;
    const int32_t outHeight = config_.trainHeight;
    if (depthMap_.empty() || rgb.empty() || outWidth <= 0 || outHeight <= 0) return;

    // 训练图像素 → 深度图采样位置（与降采样同一套旋转映射）
    const auto sampleDistance = [&](float normalizedU, float normalizedV) -> float {
        const int32_t su = std::min(
            depthWidth_ - 1,
            std::max(0, static_cast<int32_t>(normalizedU * static_cast<float>(depthWidth_))));
        const int32_t sv = std::min(
            depthHeight_ - 1,
            std::max(0, static_cast<int32_t>(normalizedV * static_cast<float>(depthHeight_))));
        const float raw = depthMap_[static_cast<size_t>(sv) * depthWidth_ + su];
        const float value = kDepthIsDisparity ? (raw > 1e-4f ? 1.0f / raw : 20.0f) : raw;
        return value;
    };

    // 相对深度没有绝对尺度：用中位数把距离定标到 kInitialDepth，保持世界坐标量级稳定。
    float median = 0.0f;
    {
        std::vector<float> samples;
        const int32_t step = 4;
        for (int32_t y = 0; y < outHeight; y += step) {
            for (int32_t x = 0; x < outWidth; x += step) {
                float u = 0.0f;
                float v = 0.0f;
                if (rotateFrames_) {
                    u = (static_cast<float>(y) + 0.5f) / static_cast<float>(outHeight);
                    v = 1.0f - (static_cast<float>(x) + 0.5f) / static_cast<float>(outWidth);
                } else {
                    u = (static_cast<float>(x) + 0.5f) / static_cast<float>(outWidth);
                    v = (static_cast<float>(y) + 0.5f) / static_cast<float>(outHeight);
                }
                samples.push_back(sampleDistance(u, v));
            }
        }
        if (!samples.empty()) {
            const auto middle = samples.begin() + samples.size() / 2;
            std::nth_element(samples.begin(), middle, samples.end());
            median = *middle;
        }
    }
    if (!(median > 1e-4f)) return;

    model_.count = 0;
    cameraCenter_ = {0.0f, 0.0f, 0.0f};
    cameraRotation_ = firstRotation_;
    const Mat3 worldFromCamera = cameraRotation_.transpose();

    const float depthStep = 0.015f;
    for (int32_t y = 0; y < outHeight; y += 2) {
        for (int32_t x = 0; x < outWidth; x += 2) {
            if (model_.count >= config_.maxGaussians) break;
            float u = 0.0f;
            float v = 0.0f;
            if (rotateFrames_) {
                u = (static_cast<float>(y) + 0.5f) / static_cast<float>(outHeight);
                v = 1.0f - (static_cast<float>(x) + 0.5f) / static_cast<float>(outWidth);
            } else {
                u = (static_cast<float>(x) + 0.5f) / static_cast<float>(outWidth);
                v = (static_cast<float>(y) + 0.5f) / static_cast<float>(outHeight);
            }
            const float distance =
                clampf(sampleDistance(u, v) / median * kInitialDepth, kInitialDepth * 0.05f,
                       kInitialDepth * 20.0f);
            const float px = (static_cast<float>(x) - camera_.cx) / camera_.fx * distance;
            const float py = (static_cast<float>(y) - camera_.cy) / camera_.fy * distance;
            const Vec3 world = worldFromCamera * Vec3{px, py, distance};

            const size_t index = (static_cast<size_t>(y) * outWidth + x) * 3;
            const Vec3 color{
                (static_cast<float>(rgb[index]) / 255.0f - 0.5f) / kShC0,
                (static_cast<float>(rgb[index + 1]) / 255.0f - 0.5f) / kShC0,
                (static_cast<float>(rgb[index + 2]) / 255.0f - 0.5f) / kShC0,
            };
            // 初始尺度按深度取比例：远处的面片本来就该更大一点。
            const float scale = depthStep * distance;
            model_.add(world, std::log(scale), std::log(scale), color, 1.5f);
        }
    }

    previousRotation_ = rotation_;
    updatePoseFromCamera();
    status_.initialized = model_.count > 0;
    status_.gaussianCount = model_.count;
}

/** 只看最新一帧优化会让模型退化成「贴在相机前的一块平面」，所以保留最近几帧一起训练。 */
void ScanEngine::trainOnImage(const std::vector<uint8_t>& rgb, const Pose& pose) {
    ViewSample sample;
    sample.rgb = rgb;
    sample.pose = pose;
    if (views_.size() >= kMaxViews) views_.erase(views_.begin());
    views_.push_back(std::move(sample));

    float residual = status_.residual;
    const int32_t iterations = std::max(1, config_.iterationsPerBatch);
    for (int32_t i = 0; i < iterations; ++i) {
        // 轮流使用不同的历史视角：多视角一致性才是三维结构的来源。
        const size_t index = views_.size() > 1
                                 ? static_cast<size_t>(i) % views_.size()
                                 : 0;
        const ViewSample& view = views_[index];
        ImageFrame target;
        target.width = config_.trainWidth;
        target.height = config_.trainHeight;
        target.rgb = view.rgb;
        residual = rasterizer_.trainStep(model_, view.pose, camera_, target);
        model_.adamStep(config_.learningRate, 0.9f, 0.999f, 1e-8f);
    }

    status_.residual = status_.residual * 0.7f + residual * 0.3f;
    status_.rounds += iterations;

    // 稠密化与剪枝。判据必须落在每个高斯自己的证据上：之前用的是全局残差，
    // 只要残差低于阈值就再也不稠密化，模型会永远停在首帧规模，画质也就一直上不去。
    // 现在用该高斯自身的梯度强度表示「这个位置还没拟合好」，再按观测次数筛选。
    if (status_.rounds % 8 == 0) {
        for (int32_t i = 0; i < model_.count; ++i) {
            const float gx = model_.gradPosition[i * 3 + 0];
            const float gy = model_.gradPosition[i * 3 + 1];
            const float gz = model_.gradPosition[i * 3 + 2];
            const float gc = std::fabs(model_.gradColor[i * 3 + 0]) +
                             std::fabs(model_.gradColor[i * 3 + 1]) +
                             std::fabs(model_.gradColor[i * 3 + 2]);
            const float error = std::fabs(gx) + std::fabs(gy) + std::fabs(gz) + gc;
            if (error > 1e-5f) {
                model_.observationCount[i] += 1;
                model_.meanError[i] = std::max(model_.meanError[i] * 0.7f, error);
            }
        }
        const int32_t before = model_.count;
        model_.densify(1e-5f, 2, config_.maxGaussians);
        model_.prune(0.02f, 0.6f);
        if (model_.count != before) status_.memoryCapped = model_.count >= config_.maxGaussians;
    }

    status_.gaussianCount = model_.count;
    status_.modelMemoryMb = static_cast<float>(model_.memoryBytes() / (1024.0 * 1024.0));
}

ScanEngine::~ScanEngine() { detachSurface(); }

bool ScanEngine::attachSurface(ANativeWindow* window) {
    if (window == nullptr) return false;

    {
        std::lock_guard<std::mutex> renderLock(rendererMutex_);
        if (rendererReady_ && window_ == window) return true;
    }

    // 界面切换会整块换掉表面（扫描页 → 查看器页）。这里必须真的把渲染器重绑到新表面上：
    // 以前遇到「已就绪」就直接返回，渲染器会一直往已经销毁的旧表面上画，
    // 而随后的 detach 又把渲染器关掉，于是查看模型永远是黑的。
    detachSurface();

    std::lock_guard<std::mutex> renderLock(rendererMutex_);
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
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    // 必须告诉界面「Vulkan 不可用」：否则界面会继续显示一块没有渲染器的空表面，而不是回退到 CPU 位图。
    status_.vulkanActive = false;
}

void ScanEngine::renderLoop() {
    using namespace std::chrono_literals;
    TileBundle bundle;
    auto lastTick = std::chrono::steady_clock::now();
    int32_t framesSinceTick = 0;
    while (renderRunning_.load()) {
        {
            std::lock_guard<std::mutex> lock(modelMutex_);
            if (model_.count > 0) {
                buildTiles(model_, pose_, camera_, kTileSize, bundle);
            } else {
                bundle.clear();
            }
            status_.renderedSplats = static_cast<int32_t>(bundle.splats.size());
            status_.renderedTiles = bundle.tilesX * bundle.tilesY;
        }

        if (!bundle.empty()) {
            std::lock_guard<std::mutex> renderLock(rendererMutex_);
            if (rendererReady_) {
                renderer_.renderFrame(bundle);
            }
        }

        // 自己量帧率：界面上的位图预览在 Vulkan 直出时根本不刷新，
        // 之前那个读数会一直是 0，和实际完全不符。
        ++framesSinceTick;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count();
        if (elapsedMs >= 500) {
            status_.renderFps = static_cast<float>(framesSinceTick) * 1000.0f /
                                static_cast<float>(elapsedMs);
            framesSinceTick = 0;
            lastTick = now;
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
    const bool sizeChanged = applyTrainingSize(std::max(trainWidth, trainHeight));
    const bool otherChanged = config_.maxGaussians != maxGaussians ||
                              config_.iterationsPerBatch != iterationsPerBatch;
    if (!sizeChanged && !otherChanged) return;

    config_.maxGaussians = maxGaussians;
    config_.iterationsPerBatch = iterationsPerBatch;
    model_.ensureCapacity(std::min(maxGaussians, 4096));
    // 只有训练图尺寸变了才丢待处理帧：旧帧的尺寸已经不匹配了。
    // 之前无条件丢弃，而调节器在扫描中每帧都会微调参数，结果每一帧都被扔掉，
    // 扫描在第二帧就冻住，后面再也不训练。
    if (sizeChanged) hasPendingFrame_ = false;

    if (sizeChanged) {
        std::lock_guard<std::mutex> renderLock(rendererMutex_);
        if (rendererReady_) {
            renderer_.setRenderSize(config_.trainWidth, config_.trainHeight);
        }
    }
}

bool ScanEngine::applyTrainingSize(int32_t longSide) {
    const int32_t side = std::max(32, longSide);
    int32_t width = 0;
    int32_t height = 0;
    if (frameAspect_ >= 1.0f) {
        width = side;
        height = std::max(16, static_cast<int32_t>(static_cast<float>(side) / frameAspect_ + 0.5f));
    } else {
        height = side;
        width = std::max(16, static_cast<int32_t>(static_cast<float>(side) * frameAspect_ + 0.5f));
    }
    if (width == config_.trainWidth && height == config_.trainHeight) return false;

    config_.trainWidth = width;
    config_.trainHeight = height;
    camera_.width = width;
    camera_.height = height;
    camera_.fx = static_cast<float>(width) * 1.2f;
    camera_.fy = camera_.fx;
    camera_.cx = static_cast<float>(width) * 0.5f;
    camera_.cy = static_cast<float>(height) * 0.5f;
    return true;
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
}

void ScanEngine::submitFrame(const uint8_t* y, int32_t width, int32_t height, int32_t yStride,
                             const uint8_t* u, const uint8_t* v, int32_t uvStride,
                             int32_t uvPixelStride, int64_t timestampNs) {
    if (state_ != EngineState::Scanning) return;
    if (width <= 0 || height <= 0) return;

    // 首帧到达时用真实画幅定训练图尺寸，之后所有几何都按它来。
    // 相机缓冲基本是横向的，而界面锁竖屏，所以这里决定是否转 90°：
    // 训练图按旋转后的画幅建，模型就是竖的，也不会被竖屏窗口拉伸。
    if (!aspectInitialized_) {
        aspectInitialized_ = true;
        rotateFrames_ = width > height;
        const float aspect = rotateFrames_
            ? static_cast<float>(height) / static_cast<float>(width)
            : static_cast<float>(width) / static_cast<float>(height);
        if (aspect > 0.1f && aspect < 10.0f) {
            frameAspect_ = aspect;
            if (applyTrainingSize(std::max(config_.trainWidth, config_.trainHeight))) {
                std::lock_guard<std::mutex> renderLock(rendererMutex_);
                if (rendererReady_) {
                    renderer_.setRenderSize(config_.trainWidth, config_.trainHeight);
                }
            }
        }
    }

    downsample(y, width, height, yStride, u, v, uvStride, uvPixelStride, config_.trainWidth,
               config_.trainHeight, rotateFrames_, pendingRgb_, pendingGray_);
    pendingTimestampNs_ = timestampNs;
    hasPendingFrame_ = true;

    // 第一帧要留着：深度模型是异步跑的，回来时要用这一帧的颜色和当时的朝向铺初模。
    if (!firstFrameStored_) {
        firstRgb_ = pendingRgb_;
        firstRotation_ = rotation_;
        firstFrameStored_ = true;
    }
}

void ScanEngine::downsample(const uint8_t* y, int32_t width, int32_t height, int32_t yStride,
                            const uint8_t* u, const uint8_t* v, int32_t uvStride,
                            int32_t uvPixelStride, int32_t outWidth, int32_t outHeight, bool rotate,
                            std::vector<uint8_t>& rgb, std::vector<uint8_t>& gray) {
    rgb.assign(static_cast<size_t>(outWidth) * outHeight * 3, 0);
    gray.assign(static_cast<size_t>(outWidth) * outHeight, 0);
    // 转 90° 时目标图的行取自源图的列，两个方向的映射步长互换。
    const float mapStepX = rotate ? static_cast<float>(width) / static_cast<float>(outHeight)
                                  : static_cast<float>(width) / static_cast<float>(outWidth);
    const float mapStepY = rotate ? static_cast<float>(height) / static_cast<float>(outWidth)
                                  : static_cast<float>(height) / static_cast<float>(outHeight);
    const int32_t blockX = std::max(1, static_cast<int32_t>(mapStepX + 0.5f));
    const int32_t blockY = std::max(1, static_cast<int32_t>(mapStepY + 0.5f));

    for (int32_t oy = 0; oy < outHeight; ++oy) {
        for (int32_t ox = 0; ox < outWidth; ++ox) {
            // 目标像素对应的源图中心。不旋转是常规映射；旋转时目标的列反向取自源的行，
            // 得到顺时针 90°（后置相机在竖屏下的常用方向）。
            const float centerXf = rotate ? (static_cast<float>(oy) + 0.5f) * mapStepX
                                          : (static_cast<float>(ox) + 0.5f) * mapStepX;
            const float centerYf = rotate
                ? static_cast<float>(height) - (static_cast<float>(ox) + 0.5f) * mapStepY
                : (static_cast<float>(oy) + 0.5f) * mapStepY;
            const int32_t y0 = std::min(std::max(0, static_cast<int32_t>(centerYf) - blockY / 2),
                                        std::max(0, height - 1));
            const int32_t y1 = std::min(height, y0 + blockY);
            const int32_t x0 = std::min(std::max(0, static_cast<int32_t>(centerXf) - blockX / 2),
                                        std::max(0, width - 1));
            const int32_t x1 = std::min(width, x0 + blockX);
            const int32_t sampleStepY = std::max(1, (y1 - y0) / 4);
            const int32_t sampleStepX = std::max(1, (x1 - x0) / 4);

            // 块内平均。原来是隔点取一个像素，十几倍缩放下纹理被采成噪点，
            // 特征跟踪会直接失效，训练图也不稳。这里改成对输入块取均值。
            float ySum = 0.0f;
            float uSum = 0.0f;
            float vSum = 0.0f;
            int32_t samples = 0;
            for (int32_t sy = y0; sy < y1; sy += sampleStepY) {
                for (int32_t sx = x0; sx < x1; sx += sampleStepX) {
                    ySum += static_cast<float>(y[sy * yStride + sx]);
                    const int32_t chromaY = std::min(height / 2 - 1, sy / 2);
                    const int32_t chromaX = std::min(width / 2 - 1, sx / 2);
                    const int32_t chromaIndex = chromaY * uvStride + chromaX * uvPixelStride;
                    uSum += u != nullptr ? static_cast<float>(u[chromaIndex] - 128) : 0.0f;
                    vSum += v != nullptr ? static_cast<float>(v[chromaIndex] - 128) : 0.0f;
                    ++samples;
                }
            }
            if (samples <= 0) samples = 1;
            const float yValue = ySum / static_cast<float>(samples);
            const float uValue = uSum / static_cast<float>(samples);
            const float vValue = vSum / static_cast<float>(samples);

            const size_t outIndex = static_cast<size_t>(oy) * outWidth + ox;
            gray[outIndex] = static_cast<uint8_t>(clampf(yValue, 0.0f, 255.0f));

            // BT.601 limited range
            rgb[outIndex * 3 + 0] =
                static_cast<uint8_t>(clampf(yValue + 1.402f * vValue, 0.0f, 255.0f));
            rgb[outIndex * 3 + 1] = static_cast<uint8_t>(
                clampf(yValue - 0.344136f * uValue - 0.714136f * vValue, 0.0f, 255.0f));
            rgb[outIndex * 3 + 2] =
                static_cast<uint8_t>(clampf(yValue + 1.772f * uValue, 0.0f, 255.0f));
        }
    }
}

void ScanEngine::initializeFromFrame(const std::vector<uint8_t>& rgb) {
    model_.count = 0;
    // 首帧的高斯先落在相机坐标系里，再按当前相机朝向转到世界坐标系。
    // 直接把它们当世界坐标用是错误的：只要首帧的 IMU 朝向不是单位阵（手持时基本不会），
    // 整片模型会被旋到相机背后，投影时全部被裁掉，渲染和训练都看不到任何高斯。
    cameraCenter_ = {0.0f, 0.0f, 0.0f};
    cameraRotation_ = rotation_;
    const Mat3 worldFromCamera = cameraRotation_.transpose();

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
            // 深度上加一点确定性抖动：完全共面的初值会直接落进「平面」这个局部最优，
            // 这也是扫描结果看起来像一块平面的原因之一。
            const float jitter =
                1.0f + 0.15f * (static_cast<float>((x * 7 + y * 13) % 11) - 5.0f) / 5.0f;
            const float depth = kInitialDepth * jitter;
            const Vec3 world = worldFromCamera * Vec3{px, py, depth};
            model_.add(world, std::log(scale), std::log(scale), color, 2.0f);
        }
    }
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
        // 首帧模型优先用深度模型的估计结果；它还没回来就等一下（界面显示「深度估计中」），
        // 超过上限帧数仍未回来就用平面初值兜底，保证扫描一定能继续。
        if (!depthReady_ && !depthFailed_) {
            if (waitingFrames_++ < kDepthWaitFrames) return;
        }
        if (depthReady_ && firstFrameStored_) {
            initializeFromDepth(firstRgb_);
        } else {
            initializeFromFrame(rgb);
        }
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
    // 平移的绝对尺度单目不可观测，只能自己定；但尺度必须跨帧一致，
    // 否则每帧的几何互相矛盾，训练结果会互相抵消成一块模糊的平面。
    const float frameScale =
        clampf(medianParallax * kInitialDepth, 0.002f, kMaxCenterStep * 0.8f);
    translationScale_ = translationScale_ <= 0.0f
                            ? frameScale
                            : translationScale_ * 0.8f + frameScale * 0.2f;
    const float scaleHint = translationScale_;

    const PoseSolution solution =
        PoseSolver::solveTranslation(tracks_, camera_, relativeRotation, scaleHint);

    status_.trackedFeatures = tracked;
    bool poseAccepted = false;

    if (solution.valid && tracked >= 20 && visionPose_) {
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

    // 位姿可信度低时不再整帧丢弃：用当前位姿继续优化。丢掉整帧会让整段扫描
    // 停在首帧，比用上一次的位姿训练更糟。
    trainOnImage(rgb, pose_);
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

void ScanEngine::setVisionPose(bool enabled) { visionPose_ = enabled; }

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

    // 相机看向焦点：相机空间的 z 轴指向相机方向，与投影约定一致。
    // 注意模型的上方向是 -Y：高斯坐标直接来自图像坐标系（y 轴向下），
    // 轨道相机若按 +Y 当上方向，整个模型会上下颠倒。
    const Vec3 zAxis = normalize(cameraPosition - viewerFocus_);
    Vec3 up{0.0f, -1.0f, 0.0f};
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
    return writePlyModel(path, model_);
}

}  // namespace splatscan