package com.splatscan.app.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ScanUiLogicTest {

    @Test
    fun `idle shows only start`() {
        val controls = ScanUiLogic.controls(ScanPhase.Idle)
        assertTrue(controls.start)
        assertFalse(controls.pause)
        assertFalse(controls.resume)
        assertFalse(controls.stop)
    }

    @Test
    fun `scanning shows pause and stop`() {
        val controls = ScanUiLogic.controls(ScanPhase.Scanning)
        assertFalse(controls.start)
        assertTrue(controls.pause)
        assertFalse(controls.resume)
        assertTrue(controls.stop)
    }

    @Test
    fun `paused shows resume and stop`() {
        val controls = ScanUiLogic.controls(ScanPhase.Paused)
        assertFalse(controls.start)
        assertFalse(controls.pause)
        assertTrue(controls.resume)
        assertTrue(controls.stop)
    }

    @Test
    fun `log bar leads with accuracy while scanning and with status otherwise`() {
        assertEquals(LogPrimaryKind.Accuracy, ScanUiLogic.logPrimaryKind(ScanPhase.Scanning))
        assertEquals(LogPrimaryKind.Status, ScanUiLogic.logPrimaryKind(ScanPhase.Idle))
        assertEquals(LogPrimaryKind.Status, ScanUiLogic.logPrimaryKind(ScanPhase.Paused))
        assertEquals(LogPrimaryKind.Status, ScanUiLogic.logPrimaryKind(ScanPhase.Finalizing))
    }

    @Test
    fun `main window shows camera before start and model afterwards`() {
        assertFalse(ScanUiLogic.mainWindowShowsModel(ScanPhase.Idle))
        assertTrue(ScanUiLogic.mainWindowShowsModel(ScanPhase.Scanning))
        assertTrue(ScanUiLogic.mainWindowShowsModel(ScanPhase.Paused))
    }

    @Test
    fun `camera window only exists after start`() {
        assertFalse(ScanUiLogic.showsCameraWindow(ScanPhase.Idle))
        assertTrue(ScanUiLogic.showsCameraWindow(ScanPhase.Scanning))
        assertTrue(ScanUiLogic.showsCameraWindow(ScanPhase.Paused))
    }

    @Test
    fun `ring is shown only while the model window is the subject`() {
        assertFalse(ScanUiLogic.showsScanRing(ScanPhase.Idle))
        assertTrue(ScanUiLogic.showsScanRing(ScanPhase.Scanning))
        assertTrue(ScanUiLogic.showsScanRing(ScanPhase.Paused))
    }

    @Test
    fun `unknown accuracy renders as nothing rather than a fake number`() {
        assertNull(ScanUiLogic.accuracyText(QualityEstimator.UNKNOWN))
        assertEquals("0", ScanUiLogic.accuracyText(0))
        assertEquals("100", ScanUiLogic.accuracyText(120))
    }
}