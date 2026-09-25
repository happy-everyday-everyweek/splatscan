package com.splatscan.app.format

/**
 * 内存中的高斯集合。字段与训练态一一对应，不做二次转换：
 * 位置、对数尺度、四元数旋转、不透明度 logit、球谐 DC 项（三通道）。
 *
 * 第一版只保留球谐 0 阶（颜色），高阶项留给后续版本，因为端上内存是硬约束。
 */
class GaussianCloud(initialCapacity: Int = 1024) {

    var count: Int = 0
        private set

    var positions: FloatArray = FloatArray(initialCapacity * 3)
        private set

    var scales: FloatArray = FloatArray(initialCapacity * 3)
        private set

    var rotations: FloatArray = FloatArray(initialCapacity * 4)
        private set

    var opacities: FloatArray = FloatArray(initialCapacity)
        private set

    var colors: FloatArray = FloatArray(initialCapacity * 3)
        private set

    val capacity: Int
        get() = opacities.size

    fun add(
        px: Float,
        py: Float,
        pz: Float,
        sx: Float,
        sy: Float,
        sz: Float,
        qw: Float,
        qx: Float,
        qy: Float,
        qz: Float,
        opacityLogit: Float,
        r: Float,
        g: Float,
        b: Float,
    ): Int {
        ensureCapacity(count + 1)
        val index = count
        val p = index * 3
        val q = index * 4
        positions[p] = px
        positions[p + 1] = py
        positions[p + 2] = pz
        scales[p] = sx
        scales[p + 1] = sy
        scales[p + 2] = sz
        rotations[q] = qw
        rotations[q + 1] = qx
        rotations[q + 2] = qy
        rotations[q + 3] = qz
        opacities[index] = opacityLogit
        colors[p] = r
        colors[p + 1] = g
        colors[p + 2] = b
        count = index + 1
        return index
    }

    fun ensureCapacity(required: Int) {
        if (required <= capacity) return
        var next = capacity.coerceAtLeast(1)
        while (next < required) next *= 2
        positions = positions.copyOf(next * 3)
        scales = scales.copyOf(next * 3)
        rotations = rotations.copyOf(next * 4)
        opacities = opacities.copyOf(next)
        colors = colors.copyOf(next * 3)
    }

    companion object {
        const val SH_C0: Float = 0.28209479177387814f

        fun sigmoid(value: Float): Float = 1f / (1f + kotlin.math.exp(-value))

        fun logit(value: Float): Float {
            val clamped = value.coerceIn(1e-4f, 1f - 1e-4f)
            return kotlin.math.ln(clamped / (1f - clamped))
        }
    }
}