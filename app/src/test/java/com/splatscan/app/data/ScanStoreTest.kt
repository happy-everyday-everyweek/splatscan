package com.splatscan.app.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ScanStoreTest {

    @get:Rule
    val folder = TemporaryFolder()

    private fun store() = ScanStore(folder.newFolder("scans"))

    private fun record(id: String, created: Long = 1_000L, name: String = id) = ScanRecord(
        id = id,
        name = name,
        createdAtMillis = created,
        durationMillis = 65_000L,
        gaussianCount = 42_000,
        meanAccuracy = 77,
        modelFileName = "model.ply",
    )

    @Test
    fun `save then list returns the record`() {
        val store = store()
        store.save(record("a"))
        val list = store.list()
        assertEquals(1, list.size)
        assertEquals("a", list[0].id)
        assertEquals(42_000, list[0].gaussianCount)
        assertEquals(77, list[0].meanAccuracy)
    }

    @Test
    fun `list is ordered newest first`() {
        val store = store()
        store.save(record("old", created = 1_000L))
        store.save(record("new", created = 5_000L))
        assertEquals(listOf("new", "old"), store.list().map { it.id })
    }

    @Test
    fun `rename keeps the model file name`() {
        val store = store()
        store.save(record("a"))
        val renamed = store.rename("a", "客厅")
        assertEquals("客厅", renamed?.name)
        assertEquals("model.ply", renamed?.modelFileName)
        assertEquals("客厅", store.list()[0].name)
    }

    @Test
    fun `rename of missing record returns null`() {
        assertNull(store().rename("nope", "x"))
    }

    @Test
    fun `delete removes the whole record directory`() {
        val store = store()
        store.save(record("a"))
        val dir = store.recordDir("a")
        assertTrue(dir.isDirectory)
        assertTrue(store.delete("a"))
        assertFalse(dir.exists())
        assertTrue(store.list().isEmpty())
    }

    @Test
    fun `meta survives a round trip through text`() {
        val original = record("a", name = "书桌")
        val parsed = ScanRecord.parse(original.toMetaText(), "fallback")
        assertEquals(original.copy(modelFileName = parsed!!.modelFileName), parsed)
    }

    @Test
    fun `duration is rendered as minutes and seconds`() {
        assertEquals("1:05", record("a").durationText)
    }
}