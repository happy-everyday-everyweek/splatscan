package com.splatscan.app.session

/**
 * 精度指标：把多个来源的质量信号混合成一个 0~100 的概览分。
 *
 * 它是纯函数，不依赖设备与时间，便于单元测试与调参。
 * 数据不足时返回 [UNKNOWN]，界面显示为「精度 --」而不是假的低分。
 */
object QualityEstimator {

    const val UNKNOWN: Int = -1

    private const val MIN_FRAMES_FOR_ESTIMATE = 3

    /** 权重之和为 1。 */
    private const val WEIGHT_TRACKING = 0.35f
    private const val WEIGHT_COVERAGE = 0.30f
    private const val WEIGHT_RESIDUAL = 0.35f

    fun estimate(snapshot: TelemetrySnapshot): Int {
        val accepted = snapshot.tracking.framesAccepted
        val total = accepted + snapshot.tracking.framesRejected
        // 闸门看「已处理的帧数」而不是「已接受的帧数」：跟踪差会让帧被拒，
        // 但用户仍应看到一个真实的低分，而不是一直显示「--」。
        if (total < MIN_FRAMES_FOR_ESTIMATE) return UNKNOWN

        val acceptance = if (total <= 0) 0f else accepted.toFloat() / total.toFloat()
        val trackingScore = clamp01(snapshot.tracking.poseStability) * 0.7f + acceptance * 0.3f
        val coverageScore = clamp01(snapshot.tracking.coverage)
        val residualScore = 1f - clamp01(snapshot.model.reconstructionResidual)

        val base = trackingScore * WEIGHT_TRACKING +
            coverageScore * WEIGHT_COVERAGE +
            residualScore * WEIGHT_RESIDUAL

        return (base * pressurePenalty(snapshot) * 100f).toInt().coerceIn(0, 100)
    }

    /**
     * 设备压力不直接计入质量，但它会压住模型规模、进而压住最终效果，
     * 所以用乘法惩罚而不是扣分，保持量纲简单。
     */
    private fun pressurePenalty(snapshot: TelemetrySnapshot): Float {
        var penalty = 1f
        if (snapshot.model.memoryCapped) penalty *= 0.85f
        val heat = clamp01(snapshot.device.thermalPressure)
        if (heat > 0.6f) penalty *= 1f - (heat - 0.6f) * 0.25f
        val battery = snapshot.device.batteryPercent
        if (!snapshot.device.charging && battery in 0..15) penalty *= 0.9f
        return penalty
    }

    private fun clamp01(value: Float): Float = when {
        value.isNaN() -> 0f
        value < 0f -> 0f
        value > 1f -> 1f
        else -> value
    }
}
