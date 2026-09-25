package com.splatscan.app.session

import com.splatscan.app.session.QualityEstimator.UNKNOWN

/** 一次扫描所处的阶段。界面上的按钮与日志内容都由它决定。 */
enum class ScanPhase {
    Idle,
    Scanning,
    Paused,
    Finalizing,
}

/** 控制栏里哪些按钮可见。 */
data class ControlVisibility(
    val start: Boolean,
    val pause: Boolean,
    val resume: Boolean,
    val stop: Boolean,
)

/** 日志栏收起时展示哪一类内容。 */
enum class LogPrimaryKind {
    /** 采集时优先展示精度概览。 */
    Accuracy,

    /** 未开始与暂停时展示普通状态。 */
    Status,
}

/**
 * 界面状态的纯逻辑。放在这里而不是 Composable 里，是为了能脱离设备做单元测试：
 * 「哪个阶段显示哪组按钮、日志栏显示哪一类内容」这类规则不应该靠真机截图验证。
 */
object ScanUiLogic {

    fun controls(phase: ScanPhase): ControlVisibility = when (phase) {
        ScanPhase.Idle -> ControlVisibility(start = true, pause = false, resume = false, stop = false)
        ScanPhase.Scanning -> ControlVisibility(start = false, pause = true, resume = false, stop = true)
        ScanPhase.Paused -> ControlVisibility(start = false, pause = false, resume = true, stop = true)
        ScanPhase.Finalizing -> ControlVisibility(start = false, pause = false, resume = false, stop = false)
    }

    fun logPrimaryKind(phase: ScanPhase): LogPrimaryKind = when (phase) {
        ScanPhase.Scanning -> LogPrimaryKind.Accuracy
        ScanPhase.Idle, ScanPhase.Paused, ScanPhase.Finalizing -> LogPrimaryKind.Status
    }

    /** 未开始时主窗口显示摄像头；开始之后主窗口显示模型。 */
    fun mainWindowShowsModel(phase: ScanPhase): Boolean = phase != ScanPhase.Idle

    /** 只有开始之后才出现摄像头小窗口。 */
    fun showsCameraWindow(phase: ScanPhase): Boolean = phase != ScanPhase.Idle

    /** 采集与暂停时，主窗口上才需要那个标记当前位置的圆环。 */
    fun showsScanRing(phase: ScanPhase): Boolean =
        phase == ScanPhase.Scanning || phase == ScanPhase.Paused

    fun accuracyText(accuracy: Int): String? = when {
        accuracy == UNKNOWN -> null
        else -> accuracy.coerceIn(0, 100).toString()
    }
}