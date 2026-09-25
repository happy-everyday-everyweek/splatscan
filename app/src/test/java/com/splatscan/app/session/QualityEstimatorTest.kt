package com.splatscan.app.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class QualityEstimatorTest {

    private fun snapshot(
        accepted: Int = 100,
        rejected: Int = 0,
        stability: Float = 0.9f,
        coverage: Float = 0.9f,
        residual: Float = 0.1f,
        memoryCapped: Boolean = false,
        thermal: Float = 0f,
        battery: Int = 80,
        charging: Boolean = false,
    ) = TelemetrySnapshot(
        device = DeviceTelemetry(
            renderFps = 30f,
            availableMemoryMb = 800,
            totalMemoryMb = 4096,
            thermalPressure = thermal,
            batteryPercent = battery,
            charging = charging,
            drainPercentPerHour = 5f,
        ),
        tracking = TrackingTelemetry(
            framesAccepted = accepted,
            framesRejected = rejected,
            poseStability = stability,
            coverage = coverage,
            reprojectionErrorPx = 1f,
        ),
        model = ModelTelemetry(
            gaussianCount = 50_000,
            iterationRounds = 10,
            reconstructionResidual = residual,
            memoryCapped = memoryCapped,
        ),
    )

    @Test
    fun `unknown before any frames are processed`() {
        assertEquals(
            QualityEstimator.UNKNOWN,
            QualityEstimator.estimate(snapshot(accepted = 0, rejected = 2)),
        )
    }

    @Test
    fun `frames rejected by weak tracking still produce a real low score`() {
        // 跟踪差会让绝大多数帧被拒。这时用户应该看到真实的低分，而不是一直显示「--」：
        // 闸门看的是已处理帧数，而不是已接受帧数。
        val failed = QualityEstimator.estimate(
            snapshot(accepted = 1, rejected = 60, stability = 0f),
        )
        assertTrue("expected a real score but was $failed", failed != QualityEstimator.UNKNOWN)
        assertTrue("failed=$failed", failed < QualityEstimator.estimate(snapshot()))
    }

    @Test
    fun `good scan scores high`() {
        val score = QualityEstimator.estimate(snapshot())
        assertTrue("expected high score but was $score", score >= 80)
    }

    @Test
    fun `poor tracking scores lower than good tracking`() {
        val good = QualityEstimator.estimate(snapshot())
        val poor = QualityEstimator.estimate(snapshot(stability = 0.2f, coverage = 0.25f, residual = 0.8f))
        assertTrue("poor=$poor good=$good", poor < good)
    }

    @Test
    fun `memory cap and heat reduce the score`() {
        val clean = QualityEstimator.estimate(snapshot())
        val pressured = QualityEstimator.estimate(
            snapshot(memoryCapped = true, thermal = 0.95f, battery = 10, charging = false),
        )
        assertTrue("pressured=$pressured clean=$clean", pressured < clean)
    }

    @Test
    fun `score stays within range`() {
        val worst = QualityEstimator.estimate(
            snapshot(stability = -5f, coverage = -1f, residual = 9f, accepted = 8, rejected = 1000),
        )
        val best = QualityEstimator.estimate(snapshot(stability = 5f, coverage = 5f, residual = -3f))
        assertTrue(worst in 0..100)
        assertTrue(best in 0..100)
    }
}