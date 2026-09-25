package com.splatscan.app.session

import android.content.Context
import android.graphics.Bitmap
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.splatscan.app.R
import com.splatscan.app.camera.FramePacket
import com.splatscan.app.depth.DepthEstimator
import com.splatscan.app.data.ScanRecord
import com.splatscan.app.data.ScanStore
import com.splatscan.app.native.NativeStatus
import com.splatscan.app.native.SplatCore
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 一次扫描的会话。它是界面与原生核之间的编排层：阶段切换、遥测采样、
 * 按调节器更新规格、把原生核的状态翻译成界面需要的数字。
 */
class ScanSession(private val context: Context, private val store: ScanStore) {

    private val telemetrySource = TelemetrySource(context)
    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.CHINA)

    private var handle: Long = 0L
    private var sessionStartMillis = 0L
    private var accuracySum = 0.0
    private var accuracySamples = 0

    var phase: ScanPhase by mutableStateOf(ScanPhase.Idle)
        private set

    var accuracy: Int by mutableStateOf(QualityEstimator.UNKNOWN)
        private set

    var spec: ScanSpec by mutableStateOf(PerfGovernor.initial(totalMemoryMb(context), false))
        private set

    var nativeStatus: NativeStatus by mutableStateOf(NativeStatus.Empty)
        private set

    var tracking: TrackingTelemetry by mutableStateOf(
        TrackingTelemetry(0, 0, 0f, 0f, 0f),
    )
        private set

    var model: ModelTelemetry by mutableStateOf(
        ModelTelemetry(0, 0, 1f, false),
    )
        private set

    var logs: List<String> by mutableStateOf(emptyList())
        private set

    var hint: String? by mutableStateOf(null)
        private set

    var lastSnapshot: TelemetrySnapshot by mutableStateOf(TelemetrySnapshot.Empty)
        private set

    private var lastDiagnosticMillis = 0L

    /** 上一批训练耗时，交给调节器判断设备还有多少余量。 */
    private var lastStepMillis = 0f

    /** 是否用画面估计位移。关掉它便于对照排查跟踪算法本身。 */
    var visionPoseEnabled: Boolean by mutableStateOf(true)
        private set

    private var depthEstimator: DepthEstimator? = null
    private var depthRequested = false
    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

    fun changeVisionPose(enabled: Boolean) {
        visionPoseEnabled = enabled
        if (handle != 0L) SplatCore.setVisionPose(handle, enabled)
        appendLog(if (enabled) "已启用画面位移估计" else "已关闭画面位移估计，只用 IMU 旋转")
    }

    var lastSavedRecord: ScanRecord? by mutableStateOf(null)
        private set

    var previewTick: Int by mutableStateOf(0)
        private set

    private val previewWidth = 160
    private val previewHeight = 120
    private val previewPixels = IntArray(previewWidth * previewHeight)
    private val previewBitmap: Bitmap =
        Bitmap.createBitmap(previewWidth, previewHeight, Bitmap.Config.ARGB_8888)

    private var lastTickNanos: Long = 0L

    val preview: Bitmap
        get() = previewBitmap

    val ringX: Float
        get() = nativeStatus.ringX

    val ringY: Float
        get() = nativeStatus.ringY

    fun ensureEngine() {
        if (handle != 0L || !SplatCore.isAvailable) return
        val memoryCeiling = PerfGovernor.memoryCeilingGaussians(totalMemoryMb(context))
        spec = PerfGovernor.initial(totalMemoryMb(context), isCharging())
        handle = SplatCore.create(
            trainWidth = spec.trainingImageLongSide * 4 / 3,
            trainHeight = spec.trainingImageLongSide,
            maxGaussians = spec.maxGaussians.coerceAtMost(memoryCeiling),
            iterationsPerBatch = spec.iterationsPerBatch,
        )
        appendLog("原生核 ${SplatCore.version}")
        appendLog("初始化引擎：训练分辨率 ${spec.trainingImageLongSide}px，上限 ${spec.maxGaussians} 个高斯")
        SplatCore.setVisionPose(handle, visionPoseEnabled)
    }

    fun release() {
        if (handle != 0L) {
            SplatCore.destroy(handle)
            handle = 0L
        }
    }

    fun start() {
        ensureEngine()
        if (phase == ScanPhase.Scanning) return
        if (phase == ScanPhase.Idle) {
            accuracySum = 0.0
            accuracySamples = 0
            sessionStartMillis = System.currentTimeMillis()
        }
        phase = ScanPhase.Scanning
        // 每次新扫描都重新估一次深度：场景变了，旧的深度图不能复用。
        depthRequested = false
        SplatCore.start(handle)
        appendLog("开始扫描")
        lastTickNanos = System.nanoTime()
    }

    fun pause() {
        if (phase != ScanPhase.Scanning) return
        phase = ScanPhase.Paused
        SplatCore.pause(handle)
        appendLog("已暂停采集，后台继续优化已有帧")
    }

    fun resume() {
        if (phase != ScanPhase.Paused) return
        phase = ScanPhase.Scanning
        SplatCore.start(handle)
        appendLog("继续采集")
    }

    /** 定格：停止一切计算，把模型写入一条扫描记录。 */
    fun stop(): ScanRecord? {
        if (phase == ScanPhase.Idle) return null
        phase = ScanPhase.Finalizing
        SplatCore.stop(handle)
        appendLog("正在定格模型")

        val id = "scan-" + System.currentTimeMillis()
        val directory = store.recordDir(id).apply { mkdirs() }
        val modelFile = File(directory, MODEL_FILE)
        val exported = SplatCore.exportPly(handle, modelFile.absolutePath)
        appendLog(if (exported) "模型已写入 ${modelFile.name}" else "模型导出失败")

        val thumbnail = File(directory, THUMBNAIL_FILE)
        runCatching {
            thumbnail.outputStream().use { previewBitmap.compress(Bitmap.CompressFormat.PNG, 90, it) }
        }

        val meanAccuracy = if (accuracySamples > 0) (accuracySum / accuracySamples).toInt() else -1
        val record = ScanRecord(
            id = id,
            name = defaultName(sessionStartMillis),
            createdAtMillis = System.currentTimeMillis(),
            durationMillis = System.currentTimeMillis() - sessionStartMillis,
            gaussianCount = nativeStatus.gaussianCount,
            meanAccuracy = meanAccuracy,
            modelFileName = MODEL_FILE,
        )
        store.save(record)
        lastSavedRecord = record
        appendLog("已保存：${record.name}，高斯 ${record.gaussianCount} 个，平均精度 $meanAccuracy%")

        phase = ScanPhase.Idle
        return record
    }

    /** 采集循环按固定间隔调用。 */
    fun tick() {
        val now = System.nanoTime()
        val dt = if (lastTickNanos == 0L) 0f else (now - lastTickNanos) / 1_000_000_000f
        lastTickNanos = now

        if (phase == ScanPhase.Idle) return
        val stepStartNanos = System.nanoTime()
        SplatCore.step(handle)
        lastStepMillis = (System.nanoTime() - stepStartNanos) / 1_000_000f

        val status = SplatCore.status(handle)
        nativeStatus = status
        if (status.renderFps > 0.5f) telemetrySource.reportRenderFps(status.renderFps)
        tracking = TrackingTelemetry(
            framesAccepted = status.acceptedFrames,
            framesRejected = status.rejectedFrames,
            poseStability = status.stability,
            coverage = status.coverage,
            reprojectionErrorPx = 0f,
        )
        model = ModelTelemetry(
            gaussianCount = status.gaussianCount,
            iterationRounds = status.rounds,
            reconstructionResidual = status.residual,
            memoryCapped = status.memoryCapped,
        )

        val snapshot = telemetrySource.snapshot(tracking, model, lastStepMillis)
        lastSnapshot = snapshot
        if (phase == ScanPhase.Scanning) {
            spec = PerfGovernor.next(spec, snapshot, dt)
            SplatCore.reconfigure(
                handle,
                trainWidth = spec.trainingImageLongSide * 4 / 3,
                trainHeight = spec.trainingImageLongSide,
                maxGaussians = spec.maxGaussians,
                iterationsPerBatch = spec.iterationsPerBatch,
            )
        }
        accuracy = QualityEstimator.estimate(snapshot)
        if (phase == ScanPhase.Scanning || phase == ScanPhase.Paused) {
            val now = System.currentTimeMillis()
            if (now - lastDiagnosticMillis >= DIAGNOSTIC_INTERVAL_MS) {
                lastDiagnosticMillis = now
                // 暂停时后台仍在优化已有帧，这段也得有日志，不然界面看起来像卡死了。
                val prefix = if (phase == ScanPhase.Paused) "暂停优化" else "诊断"
                appendLog(
                    "$prefix 接受${status.acceptedFrames}/拒绝${status.rejectedFrames}" +
                        " · 跟踪${status.trackedFeatures}点" +
                        " · 稳定${(status.stability * 100f).toInt()}%" +
                        " · 残差${"%.3f".format(status.residual)}" +
                        " · 高斯${status.gaussianCount}" +
                        " · 迭代${status.rounds}轮" +
                        " · 绘制${status.renderedSplats}点/${status.renderedTiles}瓦片" +
                        " · 帧率${status.renderFps.toInt()}",
                )
            }
        }
        if (accuracy != QualityEstimator.UNKNOWN) {
            accuracySum += accuracy
            accuracySamples++
        }
        hint = deriveHint(snapshot)
        updatePreview()
    }

    private fun updatePreview() {
        if (phase == ScanPhase.Idle && !viewerMode) return
        SplatCore.renderPreview(handle, previewPixels, previewWidth, previewHeight)
        previewBitmap.setPixels(previewPixels, 0, previewWidth, 0, 0, previewWidth, previewHeight)
        previewTick++
    }

    fun telemetryFps(): Int {
        val native = nativeStatus.renderFps
        val fps = if (native > 0.5f) native else lastSnapshot.device.renderFps
        return fps.toInt()
    }

    fun telemetrySummary(): String {
        val device = lastSnapshot.device
        val parts = mutableListOf<String>()
        parts += context.getString(R.string.log_fps, device.renderFps.toInt())
        if (device.thermalPressure > 0f) {
            parts += context.getString(R.string.log_thermal, "${(device.thermalPressure * 100).toInt()}%")
        }
        if (device.batteryPercent >= 0) {
            val power = if (device.charging) {
                context.getString(R.string.log_power_charging)
            } else {
                context.getString(
                    R.string.log_power_discharging,
                    "${device.drainPercentPerHour.toInt()}%/h",
                )
            }
            parts += context.getString(R.string.log_power, device.batteryPercent, power)
        }
        parts += context.getString(R.string.log_gaussians, formatCount(model.gaussianCount))
        parts += context.getString(R.string.log_memory, "${nativeStatus.modelMemoryMb.toInt()}MB")
        return parts.joinToString(" · ")
    }

    fun onFrameRendered() = telemetrySource.onFrameRendered()

    /** 把模型窗口的表面交给原生渲染器；成功表示走 Vulkan 直出。 */
    fun attachRenderSurface(surface: android.view.Surface): Boolean {
        if (handle == 0L) return false
        val ok = SplatCore.attachSurface(handle, surface)
        appendLog(if (ok) "渲染已切换到 Vulkan" else "Vulkan 不可用，改用 CPU 预览")
        return ok
    }

    fun detachRenderSurface() {
        if (handle != 0L) SplatCore.detachSurface(handle)
    }

    var viewerMode: Boolean by mutableStateOf(false)
        private set

    private var viewerYaw = 0f
    private var viewerPitch = 0.35f
    private var viewerZoom = 1f

    /** 打开查看器：载入 PLY 模型并切到轨道相机。 */
    fun openModel(path: String): Boolean {
        ensureEngine()
        val ok = SplatCore.loadModel(handle, path)
        SplatCore.setViewerMode(handle, true)
        viewerMode = true
        viewerYaw = 0f
        viewerPitch = 0.35f
        viewerZoom = 1f
        appendLog(if (ok) "已载入模型：$path" else "载入模型失败：$path")
        updatePreview()
        return ok
    }

    /** 查看器里没有扫描页的采集循环，位图预览要靠这里刷新（Vulkan 不可用时全靠它）。 */
    fun refreshViewerPreview() {
        if (viewerMode) updatePreview()
    }

    /**
     * 开扫后的第一帧交给深度模型，异步且只跑一次。
     * 原生侧在结果回来前会等（日志显示「深度估计中」），超时则退回平面初模。
     */
    private fun maybeStartDepthEstimation(packet: FramePacket) {
        if (depthRequested) return
        depthRequested = true
        val estimator = depthEstimator ?: DepthEstimator(context).also { depthEstimator = it }
        val heapBefore = Runtime.getRuntime().let { it.totalMemory() - it.freeMemory() }

        Thread {
            val startedAt = System.nanoTime()
            val depth = estimator.estimate(packet)
            val elapsedMs = (System.nanoTime() - startedAt) / 1_000_000
            val heapAfter = Runtime.getRuntime().let { it.totalMemory() - it.freeMemory() }
            val deltaMb = (heapAfter - heapBefore) / (1024 * 1024)

            mainHandler.post {
                if (handle == 0L) return@post
                if (depth != null) {
                    val ok = SplatCore.submitDepth(
                        handle,
                        DepthEstimator.INPUT_SIZE,
                        DepthEstimator.INPUT_SIZE,
                        depth,
                    )
                    appendLog(
                        "深度估计完成 ${elapsedMs}ms，堆内存 +${deltaMb}MB，" +
                            if (ok) "已按深度铺初模" else "提交失败，退回平面初模",
                    )
                } else {
                    appendLog("深度估计失败（${elapsedMs}ms），退回平面初模")
                }
            }
        }.start()
    }

    fun closeModel() {
        SplatCore.setViewerMode(handle, false)
        viewerMode = false
    }

    /** 查看器手势：水平拖动转偏航、垂直拖动转俯仰、双指缩放拉近拉远。 */
    fun orbitBy(deltaX: Float, deltaY: Float, zoomFactor: Float) {
        if (handle == 0L) return
        viewerYaw -= deltaX * ORBIT_SENSITIVITY
        viewerPitch = (viewerPitch + deltaY * ORBIT_SENSITIVITY).coerceIn(-1.4f, 1.4f)
        if (zoomFactor > 0f) {
            viewerZoom = (viewerZoom / zoomFactor).coerceIn(0.25f, 8f)
        }
        SplatCore.setViewerOrbit(handle, viewerYaw, viewerPitch, viewerZoom)
    }

    fun onCameraFrame(packet: FramePacket) {
        if (phase != ScanPhase.Scanning) return
        maybeStartDepthEstimation(packet)
        SplatCore.submitFrame(
            handle,
            packet.y,
            packet.width,
            packet.height,
            packet.yStride,
            packet.u,
            packet.v,
            packet.uvStride,
            packet.uvPixelStride,
            packet.timestampNs,
        )
    }

    fun onImu(
        ax: Float,
        ay: Float,
        az: Float,
        gx: Float,
        gy: Float,
        gz: Float,
        timestampNs: Long,
    ) {
        SplatCore.submitImu(handle, ax, ay, az, gx, gy, gz, timestampNs)
    }

    fun appendLog(message: String) {
        val line = "${timeFormat.format(Date())}  $message"
        logs = (logs + line).takeLast(MAX_LOG_LINES)
    }

    private fun deriveHint(snapshot: TelemetrySnapshot): String? = when {
        snapshot.tracking.poseStability in 0.01f..0.30f -> "跟踪不稳，放慢移动速度"
        snapshot.device.thermalPressure > 0.75f -> "设备温度偏高，已自动降低负载"
        !snapshot.device.charging && snapshot.device.batteryPercent in 0..15 -> "电量偏低，建议接入电源"
        else -> null
    }

    private fun defaultName(startMillis: Long): String {
        val format = SimpleDateFormat("MM-dd HH:mm", Locale.CHINA)
        return "扫描 " + format.format(Date(if (startMillis > 0) startMillis else System.currentTimeMillis()))
    }

    private fun isCharging(): Boolean = context.registerReceiver(
        null,
        android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED),
    )?.let { intent ->
        val status = intent.getIntExtra(android.os.BatteryManager.EXTRA_STATUS, -1)
        status == android.os.BatteryManager.BATTERY_STATUS_CHARGING ||
            status == android.os.BatteryManager.BATTERY_STATUS_FULL
    } ?: false

    private fun totalMemoryMb(context: Context): Int {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as? android.app.ActivityManager
        val info = android.app.ActivityManager.MemoryInfo()
        manager?.getMemoryInfo(info)
        return (info.totalMem / (1024L * 1024L)).toInt()
    }

    companion object {
        const val MODEL_FILE = "model.ply"
        const val THUMBNAIL_FILE = "thumb.png"
        private const val MAX_LOG_LINES = 200
        private const val DIAGNOSTIC_INTERVAL_MS = 4000L
        private const val ORBIT_SENSITIVITY = 0.008f

        fun formatCount(count: Int): String = when {
            count >= 1_000_000 -> "%.1fM".format(count / 1_000_000f)
            count >= 1_000 -> "%.0fK".format(count / 1_000f)
            else -> count.toString()
        }
    }
}