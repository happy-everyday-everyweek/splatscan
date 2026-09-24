package com.splatscan.app.native

/** 原生核返回的状态快照。字段顺序与 nativeStatus 一致，改动需同步两侧。 */
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
) {
    companion object {
        val Empty = NativeStatus(0, 0, 0f, 0f, 0, 0, 0, 1f, 0f, false, false, 0.5f, 0.5f, false)

        const val FIELD_COUNT = 14

        fun from(values: FloatArray): NativeStatus {
            if (values.size < FIELD_COUNT) return Empty
            return NativeStatus(
                acceptedFrames = values[0].toInt(),
                rejectedFrames = values[1].toInt(),
                stability = values[2],
                coverage = values[3],
                gaussianCount = values[4].toInt(),
                rounds = values[5].toInt(),
                trackedFeatures = values[6].toInt(),
                residual = values[7],
                modelMemoryMb = values[8],
                memoryCapped = values[9] > 0.5f,
                initialized = values[10] > 0.5f,
                ringX = values[11],
                ringY = values[12],
                vulkanActive = values[13] > 0.5f,
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
}