package com.splatscan.app.camera

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.Image
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Size
import android.view.Surface
import androidx.core.content.ContextCompat
import java.util.concurrent.atomic.AtomicBoolean

/** 一帧 YUV 数据，附带步长信息，交给原生核做降采样与色彩转换。 */
class FramePacket(
    val y: ByteArray,
    val u: ByteArray,
    val v: ByteArray,
    val width: Int,
    val height: Int,
    val yStride: Int,
    val uvStride: Int,
    val uvPixelStride: Int,
    val timestampNs: Long,
)

/**
 * Camera2 采集：预览流给界面，分析流给原生核。
 *
 * 分析帧按 [frameIntervalMs] 节流并复用缓冲区，避免每秒几十兆的临时分配；
 * 位姿、增量训练与预览渲染都在原生层。
 */
class CameraController(private val context: Context) {

    var onFrame: ((FramePacket) -> Unit)? = null

    var frameIntervalMs: Long = 160L

    /** 相机选定分析尺寸后回调，界面要用它算预览的旋转与裁切。 */
    var onAnalysisSize: ((Size) -> Unit)? = null

    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null

    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null

    private var previewSurface: Surface? = null
    private var analysisSize: Size = Size(1280, 720)
    private val started = AtomicBoolean(false)

    private var lastFrameNanos = 0L

    private var yBuffer = ByteArray(0)
    private var uBuffer = ByteArray(0)
    private var vBuffer = ByteArray(0)

    val isRunning: Boolean
        get() = started.get()

    val currentAnalysisSize: Size
        get() = analysisSize

    fun start(preview: Surface) {
        val surfaceChanged = previewSurface !== preview
        previewSurface = preview
        if (started.getAndSet(true)) {
            // 预览表面换了（未开始时是全屏、开始后是小窗）必须重建捕获会话。
            // Camera2 的会话绑在旧的输出表面上，那块表面被销毁后整条流水线
            // 连分析流一起停住，表现就是小窗全黑、原生核再也收不到帧。
            if (surfaceChanged) {
                // 延迟一点再重建：切换界面时旧表面的销毁回调可能晚于新表面的创建回调，
                // 立刻绑定会绑到已经作废的那块表面上。
                backgroundHandler?.postDelayed({ recreateSession() }, 150L)
            }
            return
        }
        startBackgroundThread()
        openCamera()
    }

    private fun recreateSession() {
        val camera = device ?: return
        runCatching { session?.close() }
        session = null
        reader?.close()
        reader = null
        createSession(camera)
    }

    fun stop() {
        if (!started.getAndSet(false)) return
        runCatching { session?.close() }
        session = null
        runCatching { device?.close() }
        device = null
        reader?.close()
        reader = null
        stopBackgroundThread()
    }

    private fun startBackgroundThread() {
        if (backgroundThread != null) return
        val thread = HandlerThread("splatscan-camera")
        thread.start()
        backgroundThread = thread
        backgroundHandler = Handler(thread.looper)
    }

    private fun stopBackgroundThread() {
        backgroundThread?.quitSafely()
        runCatching { backgroundThread?.join() }
        backgroundThread = null
        backgroundHandler = null
    }

    private fun openCamera() {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val chosen = chooseCamera(manager) ?: return
        analysisSize = chosen.second
        onAnalysisSize?.invoke(chosen.second)
        runCatching { manager.openCamera(chosen.first, deviceCallback, backgroundHandler) }
    }

    private fun chooseCamera(manager: CameraManager): Pair<String, Size>? = runCatching {
        var fallback: Pair<String, Size>? = null
        for (id in manager.cameraIdList) {
            val characteristics = manager.getCameraCharacteristics(id)
            val facing = characteristics.get(CameraCharacteristics.LENS_FACING)
            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?: continue
            val entry = id to pickAnalysisSize(map)
            if (facing == CameraCharacteristics.LENS_FACING_BACK) return@runCatching entry
            if (fallback == null) fallback = entry
        }
        fallback
    }.getOrNull()

    private fun pickAnalysisSize(map: StreamConfigurationMap): Size {
        val sizes = map.getOutputSizes(ImageFormat.YUV_420_888) ?: return Size(1280, 720)
        val preferred = listOf(1920 to 1080, 1280 to 720, 960 to 540, 640 to 480)
        for ((width, height) in preferred) {
            if (sizes.any { it.width == width && it.height == height }) return Size(width, height)
        }
        return sizes.minByOrNull { it.width * it.height } ?: Size(640, 480)
    }

    private val deviceCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
            device = camera
            createSession(camera)
        }

        override fun onDisconnected(camera: CameraDevice) {
            camera.close()
            device = null
        }

        override fun onError(camera: CameraDevice, error: Int) {
            camera.close()
            device = null
            started.set(false)
        }
    }

    private fun createSession(camera: CameraDevice) {
        val preview = previewSurface ?: return
        val imageReader = ImageReader.newInstance(
            analysisSize.width,
            analysisSize.height,
            ImageFormat.YUV_420_888,
            3,
        )
        imageReader.setOnImageAvailableListener({ source ->
            val image = runCatching { source.acquireLatestImage() }.getOrNull()
            if (image != null) {
                try {
                    handleImage(image)
                } finally {
                    image.close()
                }
            }
        }, backgroundHandler)
        reader = imageReader

        val surfaces = listOf(preview, imageReader.surface)
        runCatching {
            camera.createCaptureSession(
                surfaces,
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(configured: CameraCaptureSession) {
                        session = configured
                        val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            surfaces.forEach { addTarget(it) }
                            set(
                                CaptureRequest.CONTROL_AF_MODE,
                                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO,
                            )
                            set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
                            set(
                                CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                                android.util.Range(30, 30),
                            )
                        }.build()
                        runCatching {
                            configured.setRepeatingRequest(request, null, backgroundHandler)
                        }
                    }

                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        started.set(false)
                    }
                },
                backgroundHandler,
            )
        }
    }

    private fun handleImage(image: Image) {
        val now = System.nanoTime()
        if (lastFrameNanos != 0L && (now - lastFrameNanos) < frameIntervalMs * 1_000_000L) return
        lastFrameNanos = now

        val width = image.width
        val height = image.height
        val planes = image.planes
        val yPlane = planes[0]
        val uPlane = planes[1]
        val vPlane = planes[2]

        val ySize = yPlane.rowStride * height
        if (yBuffer.size < ySize) yBuffer = ByteArray(ySize)
        copyPlane(yPlane.buffer, yBuffer, yPlane.rowStride, width, height, 1, yPlane.pixelStride)

        val chromaHeight = height / 2
        val chromaWidth = width / 2
        val uvSize = uPlane.rowStride * chromaHeight
        if (uBuffer.size < uvSize) uBuffer = ByteArray(uvSize)
        if (vBuffer.size < uvSize) vBuffer = ByteArray(uvSize)
        copyPlane(
            uPlane.buffer, uBuffer, uPlane.rowStride, chromaWidth, chromaHeight,
            uPlane.pixelStride, uPlane.pixelStride,
        )
        copyPlane(
            vPlane.buffer, vBuffer, vPlane.rowStride, chromaWidth, chromaHeight,
            vPlane.pixelStride, vPlane.pixelStride,
        )

        onFrame?.invoke(
            FramePacket(
                y = yBuffer,
                u = uBuffer,
                v = vBuffer,
                width = width,
                height = height,
                yStride = yPlane.rowStride,
                uvStride = uPlane.rowStride,
                uvPixelStride = uPlane.pixelStride,
                timestampNs = image.timestamp,
            ),
        )
    }

    private fun copyPlane(
        buffer: java.nio.ByteBuffer,
        target: ByteArray,
        rowStride: Int,
        width: Int,
        height: Int,
        sourcePixelStride: Int,
        targetPixelStride: Int,
    ) {
        buffer.rewind()
        if (sourcePixelStride == targetPixelStride) {
            val rowBytes = width * sourcePixelStride
            var offset = 0
            for (row in 0 until height) {
                val start = row * rowStride
                if (start + rowBytes > buffer.capacity()) break
                buffer.position(start)
                buffer.get(target, offset, rowBytes)
                offset += rowBytes
            }
        }
    }
}