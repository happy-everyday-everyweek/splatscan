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

    /**
     * 训练分辨率是画质的第一决定因素，上限必须留得足够高，让有余量的设备自己爬上去。
     * 之前写死 192，等于给所有设备都封了顶，性能再空也不会更清晰。
     */
    private const val MAX_IMAGE_SIDE = 512

    private const val MIN_ITERATIONS = 1
    private const val MAX_ITERATIONS = 24

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

        // 负载信号：上一批训练实际花掉的时间占这批预算的比例。
        // 只看 renderFps 是不够的：Vulkan 直出时没有位图预览，那个读数会一直是 0，
        // 而 0 落不进任何分支，结果分辨率与迭代在整个扫描里一动不动。
        val budgetMillis = (dtSeconds * 1000f).coerceAtLeast(1f)
        val load = if (snapshot.device.stepMillis > 0f) {
            snapshot.device.stepMillis / budgetMillis
        } else {
            -1f
        }
        val fpsUsable = fps > 0.5f
        val starved = (fpsUsable && fps < MIN_FPS) || load > 0.9f
        val headroom = !starved && heat < 0.55f && memoryHeadroom > 96 &&
            ((fpsUsable && fps > TARGET_FPS) || (!fpsUsable && load in 0f..0.6f))

        var keyframe = previous.keyframeIntervalMs
        var iterations = previous.iterationsPerBatch
        var maxGaussians = previous.maxGaussians
        var imageSide = previous.trainingImageLongSide

        if (starved) {
            // 先收迭代，再收分辨率：分辨率是画质的第一来源，留到最后动。
            iterations = (iterations - 2).coerceAtLeast(MIN_ITERATIONS)
            imageSide = (imageSide - 32).coerceAtLeast(MIN_IMAGE_SIDE)
            keyframe = (keyframe + 40).coerceAtMost(500)
        } else if (headroom) {
            // 有余量就往前试探：先把分辨率抬上去，到顶再加迭代与采样密度。
            if (imageSide < MAX_IMAGE_SIDE) {
                imageSide = (imageSide + 32).coerceAtMost(MAX_IMAGE_SIDE)
            } else if (iterations < MAX_ITERATIONS) {
                iterations = (iterations + 2).coerceAtMost(MAX_ITERATIONS)
            }
            if (keyframe > 120) keyframe -= 10
            if (maxGaussians < memoryCeiling) {
                maxGaussians = (maxGaussians + maxGaussians / 8 + 4_000).coerceAtMost(memoryCeiling)
            }
        } else if (fpsUsable && fps in MIN_FPS..TARGET_FPS) {
            iterations = (iterations - 1).coerceAtLeast(MIN_ITERATIONS)
        }

        // 内存：这是硬上限，只收不放。
        if (memoryHeadroom < 64) {
            maxGaussians = (maxGaussians * 3 / 4).coerceAtLeast(MIN_GAUSSIANS)
            imageSide = (imageSide - 32).coerceAtLeast(MIN_IMAGE_SIDE)
        }

        // 过热：立即收缩，不等掉帧。
        if (heat > 0.75f) {
            iterations = MIN_ITERATIONS
            keyframe = (keyframe + 80).coerceAtMost(500)
            imageSide = (imageSide - 16).coerceAtLeast(MIN_IMAGE_SIDE)
        }

        // 供电充裕时允许更高的上限，但依然受内存与温度约束。
        if (snapshot.device.charging && heat < 0.6f && !starved) {
            imageSide = (imageSide + 16).coerceAtMost(MAX_IMAGE_SIDE)
            maxGaussians = (maxGaussians + maxGaussians / 8).coerceAtMost(memoryCeiling)
        }

        maxGaussians = maxGaussians.coerceIn(MIN_GAUSSIANS, memoryCeiling.coerceAtMost(MAX_GAUSSIANS))
        imageSide = imageSide.coerceIn(MIN_IMAGE_SIDE, MAX_IMAGE_SIDE)
        keyframe = keyframe.coerceIn(80, 500)
        iterations = iterations.coerceIn(MIN_ITERATIONS, MAX_ITERATIONS)

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
