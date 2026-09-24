package com.splatscan.app.session

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.PowerManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlin.math.max
import kotlin.math.min

/**
 * 采集遥测的薄层：只负责从系统读数，然后交给纯逻辑层。
 * 这里不做任何判断，判断都在 [PerfGovernor] 与 [QualityEstimator] 里。
 */
class TelemetrySource(private val context: Context) {

    private val activityManager: ActivityManager? =
        context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
    private val powerManager: PowerManager? =
        context.getSystemService(Context.POWER_SERVICE) as? PowerManager

    private var batteryStartPercent: Int = -1
    private var batteryStartNanos: Long = 0L

    var renderFps: Float by mutableStateOf(0f)
        private set

    private var lastFrameNanos: Long = 0L
    private var fpsAccumulator: Float = 0f

    /** 每渲染一帧调用一次，用于连续测帧率。 */
    fun onFrameRendered() {
        val now = System.nanoTime()
        if (lastFrameNanos != 0L) {
            val delta = (now - lastFrameNanos) / 1_000_000_000f
            if (delta > 0f && delta < 1f) {
                val instant = 1f / delta
                fpsAccumulator = if (fpsAccumulator == 0f) instant else fpsAccumulator * 0.9f + instant * 0.1f
                renderFps = fpsAccumulator
            }
        }
        lastFrameNanos = now
    }

    fun snapshot(tracking: TrackingTelemetry, model: ModelTelemetry): TelemetrySnapshot {
        val memory = readMemory()
        val battery = readBattery()
        return TelemetrySnapshot(
            device = DeviceTelemetry(
                renderFps = renderFps,
                availableMemoryMb = memory.first,
                totalMemoryMb = memory.second,
                thermalPressure = readThermalPressure(),
                batteryPercent = battery.first,
                charging = battery.second,
                drainPercentPerHour = battery.third,
            ),
            tracking = tracking,
            model = model,
        )
    }

    private fun readMemory(): Pair<Int, Int> {
        val info = ActivityManager.MemoryInfo()
        activityManager?.getMemoryInfo(info)
        val availableMb = (info.availMem / (1024L * 1024L)).toInt()
        val totalMb = (info.totalMem / (1024L * 1024L)).toInt()
        return availableMb to totalMb
    }

    private fun readThermalPressure(): Float {
        val manager = powerManager ?: return 0f
        val status = manager.currentThermalStatus
        val maxStatus = PowerManager.THERMAL_STATUS_SHUTDOWN
        if (maxStatus <= 0) return 0f
        return (status.toFloat() / maxStatus.toFloat()).coerceIn(0f, 1f)
    }

    private fun readBattery(): Triple<Int, Boolean, Float> {
        val intent: Intent? = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val percent = if (level >= 0 && scale > 0) level * 100 / scale else -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL

        val now = System.nanoTime()
        if (batteryStartPercent < 0 || charging) {
            batteryStartPercent = percent
            batteryStartNanos = now
            return Triple(percent, charging, 0f)
        }
        val elapsedHours = (now - batteryStartNanos) / 3.6e12
        val drain = if (elapsedHours > 0.01) {
            ((batteryStartPercent - percent) / elapsedHours).toFloat()
        } else {
            0f
        }
        return Triple(percent, charging, max(0f, drain))
    }

    fun normalizedMemoryPressure(availableMb: Int, requiredMb: Int): Float =
        if (availableMb <= 0) 1f else min(1f, requiredMb.toFloat() / availableMb.toFloat())
}