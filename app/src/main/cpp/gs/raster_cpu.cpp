#include "gs/raster_cpu.h"

#include <algorithm>
#include <cmath>

namespace splatscan {

namespace {

constexpr float kShC0 = 0.28209479177387814f;

inline float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

inline float toLinear(uint8_t value) { return static_cast<float>(value) / 255.0f; }

inline uint8_t toByte(float value) {
    return static_cast<uint8_t>(clampf(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

CpuRasterizer::CpuRasterizer(RasterOptions options) : options_(options) {}

void CpuRasterizer::project(const GaussianParams& model, const Pose& pose,
                            const CameraIntrinsics& camera,
                            std::vector<VisibleGaussian>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(model.count));

    for (int32_t i = 0; i < model.count; ++i) {
        const Vec3 world{model.position[i * 3 + 0], model.position[i * 3 + 1],
                         model.position[i * 3 + 2]};
        const Vec3 cameraSpace = pose.transform(world);
        if (cameraSpace.z <= options_.minDepth) continue;

        const float invZ = 1.0f / cameraSpace.z;
        const float u = camera.fx * cameraSpace.x * invZ + camera.cx;
        const float v = camera.fy * cameraSpace.y * invZ + camera.cy;

        const float scaleXWorld = std::exp(model.logScale[i * 2 + 0]);
        const float scaleYWorld = std::exp(model.logScale[i * 2 + 1]);
        const float scaleX = std::max(scaleXWorld * camera.fx * invZ, 0.3f);
        const float scaleY = std::max(scaleYWorld * camera.fy * invZ, 0.3f);
        const float radius = options_.sigmaCutoff * std::max(scaleX, scaleY);

        if (u + radius < 0.0f || u - radius > static_cast<float>(camera.width)) continue;
        if (v + radius < 0.0f || v - radius > static_cast<float>(camera.height)) continue;

        VisibleGaussian item;
        item.index = i;
        item.depth = cameraSpace.z;
        item.camX = cameraSpace.x;
        item.camY = cameraSpace.y;
        item.camZ = cameraSpace.z;
        item.centerX = u;
        item.centerY = v;
        item.radius = radius;
        item.scaleX = scaleX;
        item.scaleY = scaleY;
        item.scaleXWorld = scaleXWorld;
        item.scaleYWorld = scaleYWorld;
        item.invScaleX2 = 1.0f / (scaleX * scaleX);
        item.invScaleY2 = 1.0f / (scaleY * scaleY);
        item.alphaScale = sigmoid(model.opacity[i]);
        out.push_back(item);
    }

    std::sort(out.begin(), out.end(),
              [](const VisibleGaussian& a, const VisibleGaussian& b) { return a.depth < b.depth; });
}

void CpuRasterizer::render(const GaussianParams& model, const Pose& pose,
                           const CameraIntrinsics& camera, ImageFrame& out) {
    const int32_t width = camera.width;
    const int32_t height = camera.height;
    out.width = width;
    out.height = height;
    out.rgb.assign(static_cast<size_t>(width) * height * 3, 0);

    std::vector<VisibleGaussian> visible;
    project(model, pose, camera, visible);
    if (visible.empty()) return;

    for (int32_t y = 0; y < height; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int32_t x = 0; x < width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            float transmittance = 1.0f;
            float accumR = 0.0f;
            float accumG = 0.0f;
            float accumB = 0.0f;
            for (const VisibleGaussian& item : visible) {
                const float dx = px - item.centerX;
                const float dy = py - item.centerY;
                if (std::fabs(dx) > item.radius || std::fabs(dy) > item.radius) continue;
                const float power =
                    -0.5f * (dx * dx * item.invScaleX2 + dy * dy * item.invScaleY2);
                if (power < -9.0f) continue;
                const float alpha = clampf(item.alphaScale * std::exp(power), 0.0f, 0.99f);
                if (alpha < options_.alphaCutoff) continue;
                if (transmittance < 0.01f) break;
                const float contribution = transmittance * alpha;
                accumR += contribution * model.color[item.index * 3 + 0];
                accumG += contribution * model.color[item.index * 3 + 1];
                accumB += contribution * model.color[item.index * 3 + 2];
                transmittance *= (1.0f - alpha);
            }
            const size_t offset = (static_cast<size_t>(y) * width + x) * 3;
            out.rgb[offset] = toByte(0.5f + kShC0 * accumR);
            out.rgb[offset + 1] = toByte(0.5f + kShC0 * accumG);
            out.rgb[offset + 2] = toByte(0.5f + kShC0 * accumB);
        }
    }
}

float CpuRasterizer::trainStep(GaussianParams& model, const Pose& pose,
                               const CameraIntrinsics& camera, const ImageFrame& target) {
    const int32_t width = camera.width;
    const int32_t height = camera.height;
    if (target.width != width || target.height != height) return 1.0f;

    model.zeroGradients();

    std::vector<VisibleGaussian> visible;
    project(model, pose, camera, visible);
    if (visible.empty()) return 1.0f;

    double totalError = 0.0;
    std::vector<const VisibleGaussian*> items;
    std::vector<float> alphas;
    std::vector<float> transmittances;
    std::vector<float> weights;
    std::vector<float> dxs;
    std::vector<float> dys;

    for (int32_t y = 0; y < height; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int32_t x = 0; x < width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const size_t pixelOffset = (static_cast<size_t>(y) * width + x) * 3;
            const float targetR = toLinear(target.rgb[pixelOffset]);
            const float targetG = toLinear(target.rgb[pixelOffset + 1]);
            const float targetB = toLinear(target.rgb[pixelOffset + 2]);

            items.clear();
            alphas.clear();
            transmittances.clear();
            weights.clear();
            dxs.clear();
            dys.clear();

            float transmittance = 1.0f;
            float accumR = 0.0f;
            float accumG = 0.0f;
            float accumB = 0.0f;

            for (const VisibleGaussian& item : visible) {
                const float dx = px - item.centerX;
                const float dy = py - item.centerY;
                if (std::fabs(dx) > item.radius || std::fabs(dy) > item.radius) continue;
                const float power =
                    -0.5f * (dx * dx * item.invScaleX2 + dy * dy * item.invScaleY2);
                if (power < -9.0f) continue;
                const float weight = std::exp(power);
                const float alpha = clampf(item.alphaScale * weight, 0.0f, 0.99f);
                if (alpha < options_.alphaCutoff) continue;
                if (transmittance < 0.01f) break;

                transmittances.push_back(transmittance);
                alphas.push_back(alpha);
                weights.push_back(weight);
                dxs.push_back(dx);
                dys.push_back(dy);
                items.push_back(&item);

                const float contribution = transmittance * alpha;
                accumR += contribution * model.color[item.index * 3 + 0];
                accumG += contribution * model.color[item.index * 3 + 1];
                accumB += contribution * model.color[item.index * 3 + 2];
                transmittance *= (1.0f - alpha);
            }

            if (items.empty()) {
                totalError += 1.0;
                continue;
            }

            const float renderedR = 0.5f + kShC0 * accumR;
            const float renderedG = 0.5f + kShC0 * accumG;
            const float renderedB = 0.5f + kShC0 * accumB;
            const float errorR = renderedR - targetR;
            const float errorG = renderedG - targetG;
            const float errorB = renderedB - targetB;
            totalError += static_cast<double>(errorR * errorR + errorG * errorG + errorB * errorB);

            // 对合成颜色的偏导（除以 3 让三个通道的梯度量级与单通道一致）
            const float gradR = 2.0f * errorR * kShC0 / 3.0f;
            const float gradG = 2.0f * errorG * kShC0 / 3.0f;
            const float gradB = 2.0f * errorB * kShC0 / 3.0f;

            float suffixR = 0.0f;
            float suffixG = 0.0f;
            float suffixB = 0.0f;

            for (size_t k = items.size(); k-- > 0;) {
                const VisibleGaussian& item = *items[k];
                const int32_t index = item.index;
                const float alpha = alphas[k];
                const float trans = transmittances[k];
                const float weight = weights[k];
                const float dx = dxs[k];
                const float dy = dys[k];

                const float colorR = model.color[index * 3 + 0];
                const float colorG = model.color[index * 3 + 1];
                const float colorB = model.color[index * 3 + 2];

                model.gradColor[index * 3 + 0] += gradR * trans * alpha;
                model.gradColor[index * 3 + 1] += gradG * trans * alpha;
                model.gradColor[index * 3 + 2] += gradB * trans * alpha;

                const float invOneMinusAlpha = 1.0f / std::max(1.0f - alpha, 1e-3f);
                const float dAlpha =
                    gradR * colorR * trans - gradR * suffixR * invOneMinusAlpha +
                    gradG * colorG * trans - gradG * suffixG * invOneMinusAlpha +
                    gradB * colorB * trans - gradB * suffixB * invOneMinusAlpha;

                model.gradOpacity[index] += dAlpha * weight * alpha * (1.0f - alpha);

                // 屏幕坐标与尺度的梯度
                const float dWeightDx = weight * (-dx * item.invScaleX2);
                const float dWeightDy = weight * (-dy * item.invScaleY2);
                const float dWeightDsx = weight * dx * dx / (item.scaleX * item.scaleX * item.scaleX);
                const float dWeightDsy = weight * dy * dy / (item.scaleY * item.scaleY * item.scaleY);

                const float dLdWeight = dAlpha * item.alphaScale;
                const float dLdU = -dLdWeight * dWeightDx;
                const float dLdV = -dLdWeight * dWeightDy;

                const float invZ = 1.0f / item.camZ;
                const float z2 = item.camZ * item.camZ;
                // d(u)/d(x,y,z)
                const float duDx = camera.fx * invZ;
                const float duDz = -camera.fx * item.camX / z2;
                const float dvDy = camera.fy * invZ;
                const float dvDz = -camera.fy * item.camY / z2;

                // 世界坐标在相机系下的梯度，再左乘 R^T 回到世界系
                const Vec3 gradCamera{dLdU * duDx, dLdV * dvDy, dLdU * duDz + dLdV * dvDz};
                const Mat3 rotationTranspose = pose.rotation.transpose();
                const Vec3 gradWorld = rotationTranspose * gradCamera;

                model.gradPosition[index * 3 + 0] += gradWorld.x;
                model.gradPosition[index * 3 + 1] += gradWorld.y;
                model.gradPosition[index * 3 + 2] += gradWorld.z;

                const float dLdScaleXScreen = dLdWeight * dWeightDsx;
                const float dLdScaleYScreen = dLdWeight * dWeightDsy;
                const float dLdScaleXWorld = dLdScaleXScreen * camera.fx * invZ;
                const float dLdScaleYWorld = dLdScaleYScreen * camera.fy * invZ;
                model.gradLogScale[index * 2 + 0] += dLdScaleXWorld * item.scaleXWorld;
                model.gradLogScale[index * 2 + 1] += dLdScaleYWorld * item.scaleYWorld;

                suffixR += gradR * alpha * colorR * trans;
                suffixG += gradG * alpha * colorG * trans;
                suffixB += gradB * alpha * colorB * trans;
            }
        }
    }

    const double pixels = static_cast<double>(width) * height;
    return static_cast<float>(totalError / pixels);
}

}  // namespace splatscan