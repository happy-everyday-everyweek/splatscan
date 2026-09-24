package com.splatscan.app.camera

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager

/**
 * 惯性测量：加速度计与陀螺仪。
 *
 * 旋转由陀螺提供、重力方向由加速度计估计、短时位移由比力积分给出，
 * 三者一起交给原生核做位姿推进。传感器不存在时安静降级，不阻塞扫描。
 */
class ImuSampler(context: Context) {

    private val sensorManager =
        context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val accelerometer = sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val gyroscope = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

    var onSample: ((ax: Float, ay: Float, az: Float, gx: Float, gy: Float, gz: Float, timestampNs: Long) -> Unit)? =
        null

    private var acceleration = FloatArray(3)
    private var gyro = FloatArray(3)

    val isAvailable: Boolean
        get() = accelerometer != null || gyroscope != null

    private val listener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            when (event.sensor.type) {
                Sensor.TYPE_ACCELEROMETER -> {
                    acceleration[0] = event.values[0]
                    acceleration[1] = event.values[1]
                    acceleration[2] = event.values[2]
                }

                Sensor.TYPE_GYROSCOPE -> {
                    gyro[0] = event.values[0]
                    gyro[1] = event.values[1]
                    gyro[2] = event.values[2]
                }
            }
            onSample?.invoke(
                acceleration[0], acceleration[1], acceleration[2],
                gyro[0], gyro[1], gyro[2],
                event.timestamp,
            )
        }

        override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) = Unit
    }

    fun start() {
        val manager = sensorManager ?: return
        accelerometer?.let {
            manager.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME)
        }
        gyroscope?.let {
            manager.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME)
        }
    }

    fun stop() {
        sensorManager?.unregisterListener(listener)
    }
}