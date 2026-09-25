package com.splatscan.app.format

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

class PlyCodecTest {

    private fun sampleCloud(): GaussianCloud {
        val cloud = GaussianCloud(4)
        cloud.add(1f, 2f, 3f, -1f, -2f, -3f, 1f, 0f, 0f, 0f, 0.5f, 0.1f, 0.2f, 0.3f)
        cloud.add(-1f, 0.5f, 0.25f, -2f, -1.5f, -1f, 0.707f, 0.707f, 0f, 0f, -0.5f, 0.4f, 0.5f, 0.6f)
        return cloud
    }

    @Test
    fun `round trip keeps gaussian count`() {
        val cloud = sampleCloud()
        val bytes = ByteArrayOutputStream().also { PlyCodec.write(cloud, it) }.toByteArray()
        val restored = PlyCodec.read(ByteArrayInputStream(bytes))
        assertEquals(cloud.count, restored.count)
    }

    @Test
    fun `round trip keeps values within float precision`() {
        val cloud = sampleCloud()
        val bytes = ByteArrayOutputStream().also { PlyCodec.write(cloud, it) }.toByteArray()
        val restored = PlyCodec.read(ByteArrayInputStream(bytes))
        for (i in 0 until cloud.count) {
            assertEquals(cloud.positions[i * 3], restored.positions[i * 3], 1e-5f)
            assertEquals(cloud.scales[i * 3 + 1], restored.scales[i * 3 + 1], 1e-5f)
            assertEquals(cloud.colors[i * 3 + 2], restored.colors[i * 3 + 2], 1e-5f)
            assertEquals(cloud.opacities[i], restored.opacities[i], 1e-5f)
            assertEquals(cloud.rotations[i * 4 + 3], restored.rotations[i * 4 + 3], 1e-5f)
        }
    }

    @Test
    fun `header is a standard binary ply`() {
        val bytes = ByteArrayOutputStream().also { PlyCodec.write(sampleCloud(), it) }.toByteArray()
        val header = String(bytes, 0, 200, Charsets.US_ASCII)
        assertTrue(header.startsWith("ply\n"))
        assertTrue(header.contains("format binary_little_endian 1.0"))
        assertTrue(header.contains("element vertex 2"))
    }
}