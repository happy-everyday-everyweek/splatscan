package com.splatscan.app.format

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import kotlin.math.abs

class SplatCodecTest {

    private fun sampleCloud(): GaussianCloud {
        val cloud = GaussianCloud(4)
        cloud.add(0.5f, -0.25f, 1.5f, -1f, -1f, -1f, 1f, 0f, 0f, 0f, 1.0f, 0.25f, 0.5f, 0.75f)
        return cloud
    }

    @Test
    fun `record size is thirty two bytes`() {
        val cloud = sampleCloud()
        val bytes = ByteArrayOutputStream().also { SplatCodec.write(cloud, it) }.toByteArray()
        assertEquals(32, bytes.size)
    }

    @Test
    fun `round trip restores position and linear scale`() {
        val cloud = sampleCloud()
        val bytes = ByteArrayOutputStream().also { SplatCodec.write(cloud, it) }.toByteArray()
        val restored = SplatCodec.read(ByteArrayInputStream(bytes), maxGaussians = 1)
        assertEquals(1, restored.count)
        assertEquals(0.5f, restored.positions[0], 1e-4f)
        assertEquals(-0.25f, restored.positions[1], 1e-4f)
        // 写入的是线性尺度 exp(-1)，读回来仍是对数，所以应接近 -1
        assertEquals(-1f, restored.scales[0], 1e-4f)
        assertTrue(abs(restored.opacities[0] - 1f) < 0.05f)
    }

    @Test
    fun `truncated stream stops instead of producing garbage`() {
        val cloud = sampleCloud()
        val bytes = ByteArrayOutputStream().also { SplatCodec.write(cloud, it) }.toByteArray()
        val truncated = ByteArrayInputStream(bytes.copyOf(20))
        val restored = SplatCodec.read(truncated)
        assertEquals(0, restored.count)
    }
}