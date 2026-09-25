#include <jni.h>

#include <android/native_window_jni.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/scan_engine.h"
#include "engine/status_fields.h"

namespace {

std::mutex gMutex;
std::unordered_map<int64_t, std::shared_ptr<splatscan::ScanEngine>> gEngines;
int64_t gNextHandle = 1;

std::shared_ptr<splatscan::ScanEngine> findEngine(int64_t handle) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto found = gEngines.find(handle);
    if (found == gEngines.end()) return nullptr;
    return found->second;
}

std::string coreVersionString() { return std::string("splatscan-core/0.9.0"); }

std::string selfTestReport() {
    splatscan::EngineConfig config;
    config.trainWidth = 32;
    config.trainHeight = 24;
    config.maxGaussians = 256;
    config.iterationsPerBatch = 1;
    splatscan::ScanEngine engine(config);

    std::vector<uint8_t> y(64 * 48, 128);
    std::vector<uint8_t> u(32 * 24, 128);
    std::vector<uint8_t> v(32 * 24, 128);
    engine.start();
    engine.submitFrame(y.data(), 64, 48, 64, u.data(), v.data(), 32, 1, 1);
    engine.step();
    engine.step();
    const splatscan::EngineStatus& status = engine.status();

    std::vector<uint8_t> rgba;
    engine.renderPreview(rgba, 16, 12);

    const bool finite = std::isfinite(status.residual) && rgba.size() == 16u * 12u * 4u;
    return std::string("ok=") + (finite ? "true" : "false") +
           " gaussians=" + std::to_string(status.gaussianCount) +
           " rounds=" + std::to_string(status.rounds);
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_splatscan_app_native_SplatCore_nativeVersion(JNIEnv* env, jclass /*clazz*/) {
    return env->NewStringUTF(coreVersionString().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSelfTest(JNIEnv* env, jclass /*clazz*/) {
    return env->NewStringUTF(selfTestReport().c_str());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_splatscan_app_native_SplatCore_nativeCreate(JNIEnv* /*env*/, jclass /*clazz*/,
                                                     jint trainWidth, jint trainHeight,
                                                     jint maxGaussians, jint iterationsPerBatch) {
    splatscan::EngineConfig config;
    config.trainWidth = trainWidth;
    config.trainHeight = trainHeight;
    config.maxGaussians = maxGaussians;
    config.iterationsPerBatch = iterationsPerBatch;

    auto engine = std::make_shared<splatscan::ScanEngine>(config);
    std::lock_guard<std::mutex> lock(gMutex);
    const int64_t handle = gNextHandle++;
    gEngines[handle] = std::move(engine);
    return handle;
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/,
                                                      jlong handle) {
    std::lock_guard<std::mutex> lock(gMutex);
    gEngines.erase(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeStart(JNIEnv* /*env*/, jclass /*clazz*/,
                                                    jlong handle) {
    if (auto engine = findEngine(handle)) engine->start();
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativePause(JNIEnv* /*env*/, jclass /*clazz*/,
                                                    jlong handle) {
    if (auto engine = findEngine(handle)) engine->pause();
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeStop(JNIEnv* /*env*/, jclass /*clazz*/,
                                                   jlong handle) {
    if (auto engine = findEngine(handle)) engine->stop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeReconfigure(JNIEnv* /*env*/, jclass /*clazz*/,
                                                          jlong handle, jint trainWidth,
                                                          jint trainHeight, jint maxGaussians,
                                                          jint iterationsPerBatch) {
    if (auto engine = findEngine(handle)) {
        engine->reconfigure(trainWidth, trainHeight, maxGaussians, iterationsPerBatch);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSubmitFrame(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jbyteArray yPlane, jint width, jint height,
    jint yStride, jbyteArray uPlane, jbyteArray vPlane, jint uvStride, jint uvPixelStride,
    jlong timestampNs) {
    auto engine = findEngine(handle);
    if (engine == nullptr || yPlane == nullptr) return JNI_FALSE;

    jbyte* y = env->GetByteArrayElements(yPlane, nullptr);
    jbyte* u = uPlane != nullptr ? env->GetByteArrayElements(uPlane, nullptr) : nullptr;
    jbyte* v = vPlane != nullptr ? env->GetByteArrayElements(vPlane, nullptr) : nullptr;

    engine->submitFrame(reinterpret_cast<const uint8_t*>(y), width, height, yStride,
                        reinterpret_cast<const uint8_t*>(u), reinterpret_cast<const uint8_t*>(v),
                        uvStride, uvPixelStride, timestampNs);

    if (v != nullptr) env->ReleaseByteArrayElements(vPlane, v, JNI_ABORT);
    if (u != nullptr) env->ReleaseByteArrayElements(uPlane, u, JNI_ABORT);
    env->ReleaseByteArrayElements(yPlane, y, JNI_ABORT);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSubmitImu(JNIEnv* /*env*/, jclass /*clazz*/,
                                                        jlong handle, jfloat ax, jfloat ay,
                                                        jfloat az, jfloat gx, jfloat gy, jfloat gz,
                                                        jlong timestampNs) {
    if (auto engine = findEngine(handle)) {
        engine->submitImu(ax, ay, az, gx, gy, gz, timestampNs);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeStep(JNIEnv* /*env*/, jclass /*clazz*/,
                                                   jlong handle) {
    if (auto engine = findEngine(handle)) engine->step();
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_splatscan_app_native_SplatCore_nativeStatus(JNIEnv* env, jclass /*clazz*/, jlong handle) {
    constexpr int kFieldCount = splatscan::kStatusFieldCount;
    jfloat values[kFieldCount] = {0.0f};
    if (auto engine = findEngine(handle)) {
        const splatscan::EngineStatus& status = engine->status();
        values[splatscan::kStatusAcceptedFrames] = static_cast<jfloat>(status.acceptedFrames);
        values[splatscan::kStatusRejectedFrames] = static_cast<jfloat>(status.rejectedFrames);
        values[splatscan::kStatusStability] = status.stability;
        values[splatscan::kStatusCoverage] = status.coverage;
        values[splatscan::kStatusGaussianCount] = static_cast<jfloat>(status.gaussianCount);
        values[splatscan::kStatusRounds] = static_cast<jfloat>(status.rounds);
        values[splatscan::kStatusTrackedFeatures] = static_cast<jfloat>(status.trackedFeatures);
        values[splatscan::kStatusResidual] = status.residual;
        values[splatscan::kStatusModelMemoryMb] = status.modelMemoryMb;
        values[splatscan::kStatusMemoryCapped] = status.memoryCapped ? 1.0f : 0.0f;
        values[splatscan::kStatusInitialized] = status.initialized ? 1.0f : 0.0f;
        values[splatscan::kStatusRingX] = status.lastRingX;
        values[splatscan::kStatusRingY] = status.lastRingY;
        values[splatscan::kStatusVulkanActive] = status.vulkanActive ? 1.0f : 0.0f;
        values[splatscan::kStatusRenderedSplats] = static_cast<jfloat>(status.renderedSplats);
        values[splatscan::kStatusRenderedTiles] = static_cast<jfloat>(status.renderedTiles);
        values[splatscan::kStatusRenderFps] = status.renderFps;
    }
    jfloatArray result = env->NewFloatArray(kFieldCount);
    env->SetFloatArrayRegion(result, 0, kFieldCount, values);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeRenderPreview(JNIEnv* env, jclass /*clazz*/,
                                                            jlong handle, jintArray outPixels,
                                                            jint width, jint height) {
    auto engine = findEngine(handle);
    if (engine == nullptr || outPixels == nullptr) return;

    std::vector<uint8_t> rgba;
    engine->renderPreview(rgba, width, height);
    std::vector<jint> pixels(static_cast<size_t>(width) * height, 0xFF000000);
    for (size_t i = 0, count = pixels.size(); i < count; ++i) {
        const uint8_t r = rgba[i * 4 + 0];
        const uint8_t g = rgba[i * 4 + 1];
        const uint8_t b = rgba[i * 4 + 2];
        pixels[i] = static_cast<jint>(0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                                      (static_cast<uint32_t>(g) << 8) |
                                      static_cast<uint32_t>(b));
    }
    env->SetIntArrayRegion(outPixels, 0, static_cast<jsize>(pixels.size()), pixels.data());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_splatscan_app_native_SplatCore_nativeAttachSurface(JNIEnv* env, jclass /*clazz*/,
                                                            jlong handle, jobject surface) {
    auto engine = findEngine(handle);
    if (engine == nullptr || surface == nullptr) return JNI_FALSE;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) return JNI_FALSE;
    if (!engine->attachSurface(window)) {
        ANativeWindow_release(window);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeDetachSurface(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong handle) {
    if (auto engine = findEngine(handle)) engine->detachSurface();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_splatscan_app_native_SplatCore_nativeLoadModel(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                                        jstring path) {
    auto engine = findEngine(handle);
    if (engine == nullptr || path == nullptr) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    const bool ok = engine->loadModel(std::string(chars));
    env->ReleaseStringUTFChars(path, chars);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSetViewerMode(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong handle, jboolean enabled) {
    if (auto engine = findEngine(handle)) engine->setViewerMode(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSetVisionPose(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong handle, jboolean enabled) {
    if (auto engine = findEngine(handle)) engine->setVisionPose(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeDepthUnavailable(JNIEnv* /*env*/, jclass /*clazz*/,
                                                               jlong handle) {
    if (auto engine = findEngine(handle)) engine->markDepthUnavailable();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSubmitDepth(JNIEnv* env, jclass /*clazz*/,
                                                          jlong handle, jint width, jint height,
                                                          jfloatArray depth) {
    auto engine = findEngine(handle);
    if (engine == nullptr || depth == nullptr || width <= 0 || height <= 0) return JNI_FALSE;
    const jsize count = env->GetArrayLength(depth);
    if (count < width * height) return JNI_FALSE;

    jfloat* values = env->GetFloatArrayElements(depth, nullptr);
    const bool ok = engine->submitDepth(width, height, values, count);
    env->ReleaseFloatArrayElements(depth, values, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_splatscan_app_native_SplatCore_nativeSetViewerOrbit(JNIEnv* /*env*/, jclass /*clazz*/,
                                                             jlong handle, jfloat yaw, jfloat pitch,
                                                             jfloat zoom) {
    if (auto engine = findEngine(handle)) engine->setViewerOrbit(yaw, pitch, zoom);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_splatscan_app_native_SplatCore_nativeExportPly(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                                        jstring path) {
    auto engine = findEngine(handle);
    if (engine == nullptr || path == nullptr) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    const bool ok = engine->writePly(std::string(chars));
    env->ReleaseStringUTFChars(path, chars);
    return ok ? JNI_TRUE : JNI_FALSE;
}