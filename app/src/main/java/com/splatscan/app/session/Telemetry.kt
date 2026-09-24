package com.splatscan.app.session

/**
 * 一次遥测采样。所有字段都来自系统的可测读数，界面与调节器只读这个快照，
 * 不直接访问系统 API，因此这一层可以脱离设备做单元测试。
 */
data class TelemetrySnapshot(
    val device: DeviceTelemetry,
    val tracking: TrackingTelemetry,
    val model: ModelTelemetry,
) {
    companion object {
        val Empty = TelemetrySnapshot(
            device = DeviceTelemetry(
                renderFps = 0f,
                availableMemoryMb = 0,
                totalMemoryMb = 0,
                thermalPressure = 0f,
                batteryPercent = -1,
                charging = false,
                drainPercentPerHour = 0f,
            ),
            tracking = TrackingTelemetry(
                framesAccepted = 0,
                framesRejected = 0,
                poseStability = 0f,
                coverage = 0f,
                reprojectionErrorPx = 0f,
            ),
            model = ModelTelemetry(
                gaussianCount = 0,
                iterationRounds = 0,
                reconstructionResidual = 1f,
                memoryCapped = false,
            ),
        )
    }
}

data class DeviceTelemetry(
    val renderFps: Float,
    val availableMemoryMb: Int,
    val totalMemoryMb: Int,
    /** 0 表示不热，1 表示已经到系统给出的最高热压力。 */
    val thermalPressure: Float,
    val batteryPercent: Int,
    val charging: Boolean,
    val drainPercentPerHour: Float,
)

data class TrackingTelemetry(
    val framesAccepted: Int,
    val framesRejected: Int,
    /** 0..1，位姿解的稳定程度。 */
    val poseStability: Float,
    /** 0..1，已覆盖视角的充分程度。 */
    val coverage: Float,
    val reprojectionErrorPx: Float,
)

data class ModelTelemetry(
    val gaussianCount: Int,
    val iterationRounds: Int,
    /** 0..1，重建残差，越小越好。 */
    val reconstructionResidual: Float,
    /** 是否因为内存限制压住了模型规模。 */
    val memoryCapped: Boolean,
)
