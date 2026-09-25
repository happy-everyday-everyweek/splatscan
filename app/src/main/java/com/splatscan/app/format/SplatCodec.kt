package com.splatscan.app.format

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.exp
import kotlin.math.roundToInt

/**
 * .splat 格式读写：每个高斯 32 字节，网页查看器与移动端工具普遍支持。
 *
 * 位置三个 float，线性尺度三个 float，颜色四个字节（RGB + 不透明度），
 * 四元数四个字节（q * 128 + 128）。内部存储的对数尺度在这里换成线性尺度。
 */
object SplatCodec {

    private const val BYTES_PER_GAUSSIAN = 32
    private const val RECORD_BYTES = 32

    fun write(cloud: GaussianCloud, output: OutputStream, gaussianCount: Int = cloud.count) {
        val stream = BufferedOutputStream(output, 1 shl 16)
        val buffer = ByteBuffer.allocate(RECORD_BYTES).order(ByteOrder.LITTLE_ENDIAN)
        for (i in 0 until gaussianCount) {
            val p = i * 3
            val q = i * 4
            buffer.clear()
            buffer.putFloat(cloud.positions[p])
            buffer.putFloat(cloud.positions[p + 1])
            buffer.putFloat(cloud.positions[p + 2])
            buffer.putFloat(exp(cloud.scales[p]))
            buffer.putFloat(exp(cloud.scales[p + 1]))
            buffer.putFloat(exp(cloud.scales[p + 2]))
            buffer.put(toByte((0.5f + GaussianCloud.SH_C0 * cloud.colors[p]) * 255f))
            buffer.put(toByte((0.5f + GaussianCloud.SH_C0 * cloud.colors[p + 1]) * 255f))
            buffer.put(toByte((0.5f + GaussianCloud.SH_C0 * cloud.colors[p + 2]) * 255f))
            buffer.put(toByte(GaussianCloud.sigmoid(cloud.opacities[i]) * 255f))
            buffer.put(toQuatByte(cloud.rotations[q]))
            buffer.put(toQuatByte(cloud.rotations[q + 1]))
            buffer.put(toQuatByte(cloud.rotations[q + 2]))
            buffer.put(toQuatByte(cloud.rotations[q + 3]))
            stream.write(buffer.array(), 0, RECORD_BYTES)
        }
        stream.flush()
    }

    fun read(input: InputStream, maxGaussians: Int = Int.MAX_VALUE): GaussianCloud {
        val stream = BufferedInputStream(input, 1 shl 16)
        val cloud = GaussianCloud(1024)
        val buffer = ByteArray(BYTES_PER_GAUSSIAN)
        val bb = ByteBuffer.allocate(BYTES_PER_GAUSSIAN).order(ByteOrder.LITTLE_ENDIAN)
        var index = 0
        while (index < maxGaussians && readFully(stream, buffer)) {
            bb.clear()
            bb.put(buffer)
            bb.flip()
            val px = bb.float
            val py = bb.float
            val pz = bb.float
            val sx = bb.float
            val sy = bb.float
            val sz = bb.float
            val r = (bb.get().toInt() and 0xFF) / 255f
            val g = (bb.get().toInt() and 0xFF) / 255f
            val b = (bb.get().toInt() and 0xFF) / 255f
            val a = (bb.get().toInt() and 0xFF) / 255f
            val qw = fromQuatByte(bb.get())
            val qx = fromQuatByte(bb.get())
            val qy = fromQuatByte(bb.get())
            val qz = fromQuatByte(bb.get())
            cloud.add(
                px, py, pz,
                ln(sx), ln(sy), ln(sz),
                qw, qx, qy, qz,
                GaussianCloud.logit(a),
                (r - 0.5f) / GaussianCloud.SH_C0,
                (g - 0.5f) / GaussianCloud.SH_C0,
                (b - 0.5f) / GaussianCloud.SH_C0,
            )
            index++
        }
        return cloud
    }

    private fun ln(value: Float): Float = kotlin.math.ln(value.coerceAtLeast(1e-6f))

    private fun toByte(value: Float): Byte = value.roundToInt().coerceIn(0, 255).toByte()

    private fun toQuatByte(value: Float): Byte = (value * 128f + 128f).roundToInt().coerceIn(0, 255).toByte()

    private fun fromQuatByte(value: Byte): Float = ((value.toInt() and 0xFF) - 128) / 128f

    private fun readFully(stream: InputStream, buffer: ByteArray): Boolean {
        var offset = 0
        while (offset < buffer.size) {
            val read = stream.read(buffer, offset, buffer.size - offset)
            if (read < 0) return false
            offset += read
        }
        return true
    }
}