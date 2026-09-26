package com.splatscan.app.native

/** 原生核返回的状态快照。字段顺序由原生 engine/status_fields.h 定义，这里必须一致。 */
data class NativeStatus(
    val acceptedFrames: Int,
    val rejectedFrames: Int,
    val stability: Float,
    val coverage: Float,
    val gaussianCount: Int,
    val rounds: Int,
    val trackedFeatures: Int,
    val residual: Float,
    val modelMemoryMb: Float,
    val memoryCapped: Boolean,
    val initialized: Boolean,
    val ringX: Float,
    val ringY: Float,
    val vulkanActive: Boolean,
    val renderedSplats: Int,
    val renderedTiles: Int,
    /** 原生渲染线程自己量的帧率，Vulkan 直出时唯一可信的帧率来源。 */
    val renderFps: Float,
) {
    companion object {
        const val FIELD_ACCEPTED_FRAMES = 0
        const val FIELD_REJECTED_FRAMES = 1
        const val FIELD_STABILITY = 2
        const val FIELD_COVERAGE = 3
        const val FIELD_GAUSSIAN_COUNT = 4
        const val FIELD_ROUNDS = 5
        const val FIELD_TRACKED_FEATURES = 6
        const val FIELD_RESIDUAL = 7
        const val FIELD_MODEL_MEMORY_MB = 8
        const val FIELD_MEMORY_CAPPED = 9
        const val FIELD_INITIALIZED = 10
        const val FIELD_RING_X = 11
        const val FIELD_RING_Y = 12
        const val FIELD_VULKAN_ACTIVE = 13
        const val FIELD_RENDERED_SPLATS = 14
        const val FIELD_RENDERED_TILES = 15
        const val FIELD_RENDER_FPS = 16
        const val FIELD_COUNT = 17

        val Empty = NativeStatus(0, 0, 0f, 0f, 0, 0, 0, 1f, 0f, false, false, 0.5f, 0.5f, false, 0, 0, 0f)

        fun from(values: FloatArray): NativeStatus {
            if (values.size < FIELD_COUNT) return Empty
            return NativeStatus(
                acceptedFrames = values[FIELD_ACCEPTED_FRAMES].toInt(),
                rejectedFrames = values[FIELD_REJECTED_FRAMES].toInt(),
                stability = values[FIELD_STABILITY],
                coverage = values[FIELD_COVERAGE],
                gaussianCount = values[FIELD_GAUSSIAN_COUNT].toInt(),
                rounds = values[FIELD_ROUNDS].toInt(),
                trackedFeatures = values[FIELD_TRACKED_FEATURES].toInt(),
                residual = values[FIELD_RESIDUAL],
                modelMemoryMb = values[FIELD_MODEL_MEMORY_MB],
                memoryCapped = values[FIELD_MEMORY_CAPPED] > 0.5f,
                initialized = values[FIELD_INITIALIZED] > 0.5f,
                ringX = values[FIELD_RING_X],
                ringY = values[FIELD_RING_Y],
                vulkanActive = values[FIELD_VULKAN_ACTIVE] > 0.5f,
                renderedSplats = values[FIELD_RENDERED_SPLATS].toInt(),
                renderedTiles = values[FIELD_RENDERED_TILES].toInt(),
                renderFps = values[FIELD_RENDER_FPS],
            )
        }
    }
}

/**
 * 原生核的 JNI 表面。原生层负责视觉惯性位姿、增量高斯训练与预览渲染。
 * 这是 Kotlin 与原生核之间唯一的接口，保持窄。
 */
object SplatCore {

    private var loaded: Boolean = false
    private var loadError: String? = null

    init {
        try {
            System.loadLibrary("splatscan_core")
            loaded = true
        } catch (error: UnsatisfiedLinkError) {
            loadError = error.message
        }
    }

    val isAvailable: Boolean
        get() = loaded

    val unavailableReason: String?
        get() = loadError

    val version: String
        get() = if (loaded) nativeVersion() else "unavailable"

    fun selfTest(): String = if (loaded) nativeSelfTest() else "unavailable"

    fun create(trainWidth: Int, trainHeight: Int, maxGaussians: Int, iterationsPerBatch: Int): Long =
        if (loaded) nativeCreate(trainWidth, trainHeight, maxGaussians, iterationsPerBatch) else 0L

    fun destroy(handle: Long) {
        if (loaded && handle != 0L) nativeDestroy(handle)
    }

    fun start(handle: Long) {
        if (loaded && handle != 0L) nativeStart(handle)
    }

    fun pause(handle: Long) {
        if (loaded && handle != 0L) nativePause(handle)
    }

    fun stop(handle: Long) {
        if (loaded && handle != 0L) nativeStop(handle)
    }

    fun reconfigure(
        handle: Long,
        trainWidth: Int,
        trainHeight: Int,
        maxGaussians: Int,
        iterationsPerBatch: Int,
    ) {
        if (loaded && handle != 0L) {
            nativeReconfigure(handle, trainWidth, trainHeight, maxGaussians, iterationsPerBatch)
        }
    }

    fun submitFrame(
        handle: Long,
        y: ByteArray,
        width: Int,
        height: Int,
        yStride: Int,
        u: ByteArray?,
        v: ByteArray?,
        uvStride: Int,
        uvPixelStride: Int,
        timestampNs: Long,
    ): Boolean {
        if (!loaded || handle == 0L) return false
        return nativeSubmitFrame(
            handle, y, width, height, yStride, u, v, uvStride, uvPixelStride, timestampNs,
        )
    }

    fun submitImu(
        handle: Long,
        ax: Float,
        ay: Float,
        az: Float,
        gx: Float,
        gy: Float,
        gz: Float,
        timestampNs: Long,
    ) {
        if (loaded && handle != 0L) nativeSubmitImu(handle, ax, ay, az, gx, gy, gz, timestampNs)
    }

    fun step(handle: Long) {
        if (loaded && handle != 0L) nativeStep(handle)
    }

    fun status(handle: Long): NativeStatus =
        if (loaded && handle != 0L) NativeStatus.from(nativeStatus(handle)) else NativeStatus.Empty

    fun renderPreview(handle: Long, pixels: IntArray, width: Int, height: Int) {
        if (loaded && handle != 0L) nativeRenderPreview(handle, pixels, width, height)
    }

    fun exportPly(handle: Long, path: String): Boolean =
        loaded && handle != 0L && nativeExportPly(handle, path)

    /** 绑定渲染目标表面；返回 true 表示已切到 Vulkan 直出，false 表示界面应回退到 CPU 预览。 */
    fun attachSurface(handle: Long, surface: android.view.Surface): Boolean =
        loaded && handle != 0L && nativeAttachSurface(handle, surface)

    fun detachSurface(handle: Long) {
        if (loaded && handle != 0L) nativeDetachSurface(handle)
    }

    /** 查看器：载入 PLY 模型并切到轨道相机模式。 */
    fun loadModel(handle: Long, path: String): Boolean =
        loaded && handle != 0L && nativeLoadModel(handle, path)

    fun setViewerMode(handle: Long, enabled: Boolean) {
        if (loaded && handle != 0L) nativeSetViewerMode(handle, enabled)
    }

    /**
     * 是否用画面估计位移。关掉后只有 IMU 旋转，供排查跟踪/位移算法时做对照。
     */
    fun setVisionPose(handle: Long, enabled: Boolean) {
        if (loaded && handle != 0L) nativeSetVisionPose(handle, enabled)
    }

    /** 提交单目深度模型的结果，原生侧会用它铺初模。数值越大越近（视差型）。 */
    fun submitDepth(handle: Long, width: Int, height: Int, depth: FloatArray): Boolean =
        loaded && handle != 0L && nativeSubmitDepth(handle, width, height, depth)

    /** 深度模型不可用时立刻告知原生侧，避免首帧白等到超时。 */
    fun depthUnavailable(handle: Long) {
        if (loaded && handle != 0L) nativeDepthUnavailable(handle)
    }

    fun setViewerOrbit(handle: Long, yaw: Float, pitch: Float, zoom: Float) {
        if (loaded && handle != 0L) nativeSetViewerOrbit(handle, yaw, pitch, zoom)
    }

    private external fun nativeVersion(): String

    private external fun nativeSelfTest(): String

    private external fun nativeCreate(
        trainWidth: Int,
        trainHeight: Int,
        maxGaussians: Int,
        iterationsPerBatch: Int,
    ): Long

    private external fun nativeDestroy(handle: Long)

    private external fun nativeStart(handle: Long)

    private external fun nativePause(handle: Long)

    private external fun nativeStop(handle: Long)

    private external fun nativeReconfigure(
        handle: Long,
        trainWidth: Int,
        trainHeight: Int,
        maxGaussians: Int,
        iterationsPerBatch: Int,
    )

    private external fun nativeSubmitFrame(
        handle: Long,
        y: ByteArray,
        width: Int,
        height: Int,
        yStride: Int,
        u: ByteArray?,
        v: ByteArray?,
        uvStride: Int,
        uvPixelStride: Int,
        timestampNs: Long,
    ): Boolean

    private external fun nativeSubmitImu(
        handle: Long,
        ax: Float,
        ay: Float,
        az: Float,
        gx: Float,
        gy: Float,
        gz: Float,
        timestampNs: Long,
    )

    private external fun nativeStep(handle: Long)

    private external fun nativeStatus(handle: Long): FloatArray

    private external fun nativeRenderPreview(handle: Long, outPixels: IntArray, width: Int, height: Int)

    private external fun nativeExportPly(handle: Long, path: String): Boolean

    private external fun nativeAttachSurface(handle: Long, surface: android.view.Surface): Boolean

    private external fun nativeDetachSurface(handle: Long)

    private external fun nativeLoadModel(handle: Long, path: String): Boolean

    private external fun nativeSetViewerMode(handle: Long, enabled: Boolean)

    private external fun nativeSetVisionPose(handle: Long, enabled: Boolean)

    private external fun nativeSubmitDepth(
        handle: Long,
        width: Int,
        height: Int,
        depth: FloatArray,
    ): Boolean

    private external fun nativeDepthUnavailable(handle: Long)

    private external fun nativeSetViewerOrbit(handle: Long, yaw: Float, pitch: Float, zoom: Float)
}