package com.splatscan.app.depth

import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import ai.onnxruntime.TensorInfo
import android.content.Context
import com.splatscan.app.camera.FramePacket
import java.nio.FloatBuffer
import kotlin.math.min

/**
 * 单目深度估计：Depth-Anything-V2-Small（int8 量化，Apache-2.0）。
 *
 * 只在开始扫描时跑一次，给首帧一张相对深度图，原生侧据此反投影出三维初模——
 * 这样模型一出生就有真实结构，不会退化成「贴在相机前的平面」。
 *
 * 模型输出约定：数值越大越近（视差型），世界距离由原生侧取倒数并做中位数定标。
 * 推理在调用方的后台线程上执行，这里不做线程切换。
 */
class DepthEstimator(private val context: Context) {

    private var environment: OrtEnvironment? = null
    private var session: OrtSession? = null
    private var failed = false

    /** 失败原因（类名 + 消息），供界面日志显示。 */
    var lastError: String? = null
        private set

    /** 模型输入输出签名，载入后回显一次，便于核对尺寸与名字。 */
    var modelSummary: String = ""
        private set

    /** 模型要求的输入边长；固定尺寸时以模型为准，动态尺寸时用默认值。 */
    var inputSize: Int = INPUT_SIZE
        private set

    val isAvailable: Boolean
        get() = !failed

    /**
     * 对一帧做深度估计，返回 [inputSize]×[inputSize] 的相对深度。
     * 失败（模型缺失、运行库异常、内存不足）时返回 null，调用方退回平面初模。
     */
    fun estimate(packet: FramePacket): FloatArray? {
        val env = ensureSession() ?: return null
        val active = session ?: return null
        val size = inputSize

        return try {
            val input = buildInput(packet, size)
            val shape = longArrayOf(1, 3, size.toLong(), size.toLong())
            OnnxTensor.createTensor(env, FloatBuffer.wrap(input), shape).use { tensor ->
                active.run(mapOf(active.inputNames.first() to tensor)).use { result ->
                    val value = result.get(0)
                    val buffer = (value as OnnxTensor).floatBuffer
                    val depth = FloatArray(size * size)
                    buffer.get(depth)
                    depth
                }
            }
        } catch (error: Throwable) {
            failed = true
            lastError = describeError(error)
            null
        }
    }

    fun close() {
        runCatching { session?.close() }
        session = null
    }

    private fun ensureSession(): OrtEnvironment? {
        if (failed) return null
        environment?.let { return it }
        return try {
            val env = OrtEnvironment.getEnvironment()
            val options = OrtSession.SessionOptions().apply {
                setIntraOpNumThreads(Runtime.getRuntime().availableProcessors().coerceIn(2, 6))
                setOptimizationLevel(OrtSession.SessionOptions.OptLevel.ALL_OPT)
            }
            // 大模型走文件描述符 + 直接读取，避免在 Java 堆上再放一份 27MB 的副本。
            val bytes = context.assets.open(MODEL_ASSET).use { it.readBytes() }
            val created = env.createSession(bytes, options)
            environment = env
            session = created
            applyModelShape(created)
            env
        } catch (error: Throwable) {
            failed = true
            lastError = describeError(error)
            null
        }
    }

    /** 读模型自己的输入形状：固定尺寸就照它的来，动态尺寸才用默认值。 */
    private fun applyModelShape(session: OrtSession) {
        val input = session.inputInfo.entries.firstOrNull()
        val tensor = input?.value?.info as? TensorInfo
        val shape = tensor?.shape ?: longArrayOf()
        if (shape.size >= 4 && shape[2] > 0 && shape[3] > 0) {
            inputSize = shape[2].toInt()
        }
        val output = session.outputInfo.entries.firstOrNull()
        val outputShape = ((output?.value?.info) as? TensorInfo)?.shape ?: longArrayOf()
        modelSummary =
            "输入 ${input?.key}=${shape.joinToString("x")}，输出 ${output?.key}=" +
                outputShape.joinToString("x")
    }

    private fun describeError(error: Throwable): String {
        val message = error.message?.take(160) ?: ""
        return "${error.javaClass.simpleName}: $message"
    }

    /** YUV 帧 → 518×518 的 RGB，按 ImageNet 均值方差归一化，NCHW 布局。 */
    private fun buildInput(packet: FramePacket, size: Int): FloatArray {
        val plane = FloatArray(3 * size * size)
        val stepX = packet.width.toFloat() / size.toFloat()
        val stepY = packet.height.toFloat() / size.toFloat()
        val area = size * size
        for (y in 0 until size) {
            val sy = min(packet.height - 1, (y * stepY).toInt())
            for (x in 0 until size) {
                val sx = min(packet.width - 1, (x * stepX).toInt())
                val luma = packet.y[sy * packet.yStride + sx].toInt() and 0xFF
                val chromaY = sy / 2
                val chromaX = sx / 2
                val chromaIndex = chromaY * packet.uvStride + chromaX * packet.uvPixelStride
                val u = (packet.u[chromaIndex].toInt() and 0xFF) - 128
                val v = (packet.v[chromaIndex].toInt() and 0xFF) - 128
                val r = (luma + 1.402f * v) / 255.0f
                val g = (luma - 0.344136f * u - 0.714136f * v) / 255.0f
                val b = (luma + 1.772f * u) / 255.0f
                val offset = y * size + x
                plane[offset] = (r - 0.485f) / 0.229f
                plane[area + offset] = (g - 0.456f) / 0.224f
                plane[2 * area + offset] = (b - 0.406f) / 0.225f
            }
        }
        return plane
    }

    companion object {
        /** 模型原生输入边长（14 的倍数），用满它才有足够细的深度结构。 */
        const val INPUT_SIZE = 518
        private const val MODEL_ASSET = "depth_anything_v2_small_int8.onnx"
    }
}
