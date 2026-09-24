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
    ) = TelemetrySnapshot(
        device = DeviceTelemetry(
            renderFps = fps,
            availableMemoryMb = availableMb,
            totalMemoryMb = totalMb,
            thermalPressure = thermal,
            batteryPercent = 70,
            charging = charging,
            drainPercentPerHour = 4f,
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
        assertEquals(2, next.iterationsPerBatch)
        assertTrue(next.trainingImageLongSide < start.trainingImageLongSide)
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
            assertTrue(spec.iterationsPerBatch in 2..32)
            assertTrue(spec.keyframeIntervalMs in 80..500)
            assertTrue(spec.trainingImageLongSide in 320..1280)
            assertTrue(spec.maxGaussians <= PerfGovernor.memoryCeilingGaussians(2048))
        }
    }

    @Test
    fun `memory estimate follows the documented per gaussian cost`() {
        // 每高斯 2048 字节，10 万高斯 = 2048 * 100000 / 1048576 ≈ 195 MiB
        assertEquals(195, PerfGovernor.estimateModelMemoryMb(100_000))
    }
}