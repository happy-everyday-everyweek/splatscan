package com.splatscan.app.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PerfGovernorTest {

    private fun snapshot(
        fps: Float = 30f,
        availableMb: Int = 1200,
        totalMb: Int = 4096,
        thermal: Float = 0.2f,
        charging: Boolean = false,
        stepMillis: Float = 0f,
    ) = TelemetrySnapshot(
        device = DeviceTelemetry(
            renderFps = fps,
            availableMemoryMb = availableMb,
            totalMemoryMb = totalMb,
            thermalPressure = thermal,
            batteryPercent = 70,
            charging = charging,
            drainPercentPerHour = 4f,
            stepMillis = stepMillis,
        ),
        tracking = TrackingTelemetry(100, 0, 0.8f, 0.6f, 1f),
        model = ModelTelemetry(10_000, 5, 0.4f, memoryCapped = false),
    )

    @Test
    fun `low memory device gets a smaller ceiling than high memory device`() {
        val small = PerfGovernor.memoryCeilingGaussians(2048)
        val large = PerfGovernor.memoryCeilingGaussians(12288)
        assertTrue("small=$small large=$large", small < large)
    }

    @Test
    fun `slow frame rate reduces load`() {
        val start = PerfGovernor.initial(4096, charging = false)
        val next = PerfGovernor.next(start, snapshot(fps = 8f), dtSeconds = 1f)
        assertTrue(next.iterationsPerBatch <= start.iterationsPerBatch)
        assertTrue(next.keyframeIntervalMs >= start.keyframeIntervalMs)
    }

    @Test
    fun `higher frame rate allows more work`() {
        val start = PerfGovernor.initial(8192, charging = true)
        val next = PerfGovernor.next(start, snapshot(fps = 45f, totalMb = 8192, charging = true), 1f)
        assertTrue(next.iterationsPerBatch >= start.iterationsPerBatch)
    }

    @Test
    fun `heat forces immediate reduction`() {
        val start = PerfGovernor.initial(8192, charging = true)
        val next = PerfGovernor.next(start, snapshot(fps = 40f, thermal = 0.95f, charging = true), 1f)
        assertEquals(1, next.iterationsPerBatch)
        // 初始分辨率已经贴着下限，过热时只会保持最低规格而不会更低
        assertTrue(next.trainingImageLongSide <= start.trainingImageLongSide)
        assertTrue(next.keyframeIntervalMs >= start.keyframeIntervalMs)
    }

    @Test
    fun `spec always stays inside supported bounds`() {
        var spec = PerfGovernor.initial(2048, charging = false)
        repeat(200) { step ->
            val fps = if (step % 3 == 0) 5f else 60f
            spec = PerfGovernor.next(
                spec,
                snapshot(fps = fps, availableMb = 10, totalMb = 2048, thermal = if (step % 5 == 0) 1f else 0f),
                0.25f,
            )
            assertTrue(spec.iterationsPerBatch in 1..24)
            assertTrue(spec.keyframeIntervalMs in 80..500)
            assertTrue(spec.trainingImageLongSide in 96..512)
            assertTrue(spec.maxGaussians <= PerfGovernor.memoryCeilingGaussians(2048))
        }
    }

    @Test
    fun `spare capacity raises quality even when the frame rate is unmeasured`() {
        // Vulkan 直出时没有位图预览，renderFps 会一直是 0。这时必须靠训练耗时判断余量，
        // 否则分辨率会永远停在初始值——这正是「有余量也不提升质量」的原因。
        val start = PerfGovernor.initial(12_288, charging = false)
        var spec = start
        repeat(8) {
            spec = PerfGovernor.next(
                spec,
                snapshot(fps = 0f, availableMb = 4000, totalMb = 12_288, stepMillis = 8f),
                0.25f,
            )
        }
        assertTrue(
            "side=${spec.trainingImageLongSide}",
            spec.trainingImageLongSide > start.trainingImageLongSide,
        )
    }

    @Test
    fun `heavy training steps pull quality back down`() {
        val start = PerfGovernor.initial(12_288, charging = false).copy(trainingImageLongSide = 384)
        val next = PerfGovernor.next(
            start,
            snapshot(fps = 0f, totalMb = 12_288, stepMillis = 400f),
            0.25f,
        )
        assertTrue("side=${next.trainingImageLongSide}", next.trainingImageLongSide < start.trainingImageLongSide)
    }

    @Test
    fun `memory estimate follows the documented per gaussian cost`() {
        // 每高斯 2048 字节，10 万高斯 = 2048 * 100000 / 1048576 ≈ 195 MiB
        assertEquals(195, PerfGovernor.estimateModelMemoryMb(100_000))
    }
}