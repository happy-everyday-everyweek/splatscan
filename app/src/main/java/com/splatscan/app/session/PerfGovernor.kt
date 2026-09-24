package com.splatscan.app.session

/**
 * 一次扫描的运行规格。由 [PerfGovernor] 连续调节，不由用户选择，也没有档位概念。
 */
data class ScanSpec(
    val keyframeIntervalMs: Int,
    val iterationsPerBatch: Int,
    val maxGaussians: Int,
    val trainingImageLongSide: Int,
)

/**
 * 自适应调节器：只做一件事——根据最近的遥测把负载连续地推回设备可持续的水平。
 *
 * 它没有「低/中/高」档，也没有面向用户的阈值；所有调节都是小幅度的步进，
 * 因此 2GB 的老设备和 12GB 的新设备走的是同一套逻辑，只是收敛到不同工作点。
 */
object PerfGovernor {

    private const val TARGET_FPS = 24f
    private const val MIN_FPS = 12f

    /** 每个高斯的训练态开销，用于把内存换算成规模上限。 */
    private const val BYTES_PER_GAUSSIAN_TRAINING = 2048

    /** 只允许模型占用可用内存的一部分，其余留给相机、渲染与系统。 */
    private const val MEMORY_SHARE = 0.35f

    private const val MIN_GAUSSIANS = 20_000
    private const val MAX_GAUSSIANS = 400_000

    private const val MIN_IMAGE_SIDE = 96
    private const val MAX_IMAGE_SIDE = 192

    fun initial(totalMemoryMb: Int, charging: Boolean): ScanSpec {
        val memoryCeiling = memoryCeilingGaussians(totalMemoryMb)
        return ScanSpec(
            keyframeIntervalMs = 220,
            iterationsPerBatch = if (charging) 4 else 3,
            maxGaussians = (memoryCeiling / 2).coerceAtLeast(MIN_GAUSSIANS),
            trainingImageLongSide = 96,
        )
    }

    fun next(previous: ScanSpec, snapshot: TelemetrySnapshot, dtSeconds: Float): ScanSpec {
        if (dtSeconds <= 0f) return previous

        val fps = snapshot.device.renderFps
        val heat = snapshot.device.thermalPressure.coerceIn(0f, 1f)
        val memoryCeiling = memoryCeilingGaussians(snapshot.device.totalMemoryMb)
        val memoryHeadroom = snapshot.device.availableMemoryMb -
            estimateModelMemoryMb(previous.maxGaussians)

        var keyframe = previous.keyframeIntervalMs
        var iterations = previous.iterationsPerBatch
        var maxGaussians = previous.maxGaussians
        var imageSide = previous.trainingImageLongSide

        // 掉帧：先降迭代，再降抽帧密度，最后降训练分辨率。
        if (fps in 0.1f..MIN_FPS) {
            iterations = (iterations - 2).coerceAtLeast(1)
            keyframe = (keyframe + 40).coerceAtMost(500)
            imageSide = (imageSide - 16).coerceAtLeast(MIN_IMAGE_SIDE)
        } else if (fps in MIN_FPS..TARGET_FPS) {
            iterations = (iterations - 1).coerceAtLeast(1)
        } else if (fps > TARGET_FPS * 1.35f) {
            // 有余量就往前试探，每次只走一小步。
            iterations = (iterations + 1).coerceAtMost(8)
            if (keyframe > 120) keyframe -= 10
        }

        // 内存：这是硬上限，只收不放。
        if (memoryHeadroom < 64) {
            maxGaussians = (maxGaussians * 3 / 4).coerceAtLeast(MIN_GAUSSIANS)
            imageSide = (imageSide - 32).coerceAtLeast(MIN_IMAGE_SIDE)
        }

        // 过热：立即收缩，不等掉帧。
        if (heat > 0.75f) {
            iterations = 1
            keyframe = (keyframe + 80).coerceAtMost(500)
            imageSide = (imageSide - 16).coerceAtLeast(MIN_IMAGE_SIDE)
        } else if (heat < 0.4f && fps > TARGET_FPS) {
            iterations = (iterations + 1).coerceAtMost(8)
            imageSide = (imageSide + 16).coerceAtMost(MAX_IMAGE_SIDE)
        }

        // 供电充裕时允许更高的上限，但依然受内存与温度约束。
        if (snapshot.device.charging && heat < 0.6f) {
            imageSide = (imageSide + 16).coerceAtMost(MAX_IMAGE_SIDE)
            maxGaussians = (maxGaussians + maxGaussians / 8).coerceAtMost(memoryCeiling)
        }

        maxGaussians = maxGaussians.coerceIn(MIN_GAUSSIANS, memoryCeiling.coerceAtMost(MAX_GAUSSIANS))
        imageSide = imageSide.coerceIn(MIN_IMAGE_SIDE, MAX_IMAGE_SIDE)
        keyframe = keyframe.coerceIn(80, 500)
        iterations = iterations.coerceIn(1, 8)

        return ScanSpec(
            keyframeIntervalMs = keyframe,
            iterationsPerBatch = iterations,
            maxGaussians = maxGaussians,
            trainingImageLongSide = imageSide,
        )
    }

    fun memoryCeilingGaussians(totalMemoryMb: Int): Int {
        if (totalMemoryMb <= 0) return MIN_GAUSSIANS
        val bytes = totalMemoryMb.toLong() * 1024L * 1024L * MEMORY_SHARE.toDouble()
        val count = (bytes / BYTES_PER_GAUSSIAN_TRAINING).toInt()
        return count.coerceIn(MIN_GAUSSIANS, MAX_GAUSSIANS)
    }

    fun estimateModelMemoryMb(gaussianCount: Int): Int =
        (gaussianCount.toLong() * BYTES_PER_GAUSSIAN_TRAINING / (1024L * 1024L)).toInt()
}
