package com.splatscan.app.ui

import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.TextureView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.foundation.Image
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.splatscan.app.R
import com.splatscan.app.camera.CameraController
import com.splatscan.app.camera.ImuSampler
import com.splatscan.app.session.LogPrimaryKind
import com.splatscan.app.session.ScanPhase
import com.splatscan.app.session.ScanSession
import com.splatscan.app.session.ScanUiLogic
import com.splatscan.app.ui.theme.ScanRingColor
import com.splatscan.app.ui.theme.ViewerBackground
import kotlin.math.max

@Composable
fun ScanRoute(
    session: ScanSession,
    modifier: Modifier = Modifier,
    onOpenHistory: () -> Unit = {},
) {
    val context = LocalContext.current
    val camera = remember { CameraController(context) }
    val imu = remember { ImuSampler(context) }

    LaunchedEffect(camera, imu, session) {
        camera.onFrame = { packet -> session.onCameraFrame(packet) }
        imu.onSample = { ax, ay, az, gx, gy, gz, timestamp ->
            session.onImu(ax, ay, az, gx, gy, gz, timestamp)
        }
        imu.start()
    }

    DisposableEffect(camera, imu) {
        onDispose {
            camera.stop()
            imu.stop()
        }
    }

    var previewBuffer by remember { mutableStateOf(Size(1080, 1920)) }

    LaunchedEffect(camera) {
        camera.onAnalysisSize = { size -> previewBuffer = size }
    }

    ScanScreen(
        session = session,
        modifier = modifier,
        onOpenHistory = onOpenHistory,
        previewBuffer = previewBuffer,
        onPreviewSurface = { surface ->
            // 不做「已在运行就忽略」的判断：捕获会话必须跟着预览表面走，
            // 否则全屏预览切成小窗后，相机还绑在已销毁的表面上，小窗会一直黑。
            camera.start(surface)
        },
    )
}

@Composable
private fun ScanScreen(
    session: ScanSession,
    modifier: Modifier = Modifier,
    onOpenHistory: () -> Unit,
    previewBuffer: Size,
    onPreviewSurface: (Surface) -> Unit,
) {
    var logExpanded by remember { mutableStateOf(false) }
    val showsModel = ScanUiLogic.mainWindowShowsModel(session.phase)

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(ViewerBackground),
    ) {
        if (showsModel) {
            ModelView(session = session, modifier = Modifier.fillMaxSize())
            if (ScanUiLogic.showsScanRing(session.phase)) {
                ScanRingOverlay(
                    ringX = session.ringX,
                    ringY = session.ringY,
                    modifier = Modifier.fillMaxSize(),
                )
            }
        }

        if (ScanUiLogic.showsCameraWindow(session.phase)) {
            Row(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .statusBarsPadding()
                    .padding(top = 12.dp, end = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CameraWindow(
                    modifier = Modifier.size(width = 104.dp, height = 140.dp),
                    previewBuffer = previewBuffer,
                    onPreviewSurface = onPreviewSurface,
                )
            }
        } else {
            CameraWindow(
                modifier = Modifier.fillMaxSize(),
                previewBuffer = previewBuffer,
                onPreviewSurface = onPreviewSurface,
            )
            CapsuleButton(
                text = stringResource(R.string.history_title),
                onClick = onOpenHistory,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .statusBarsPadding()
                    .padding(top = 12.dp, end = 12.dp),
            )
        }

        session.hint?.let { hint ->
            HintCapsule(
                text = hint,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .statusBarsPadding()
                    .padding(top = 12.dp, start = 12.dp),
            )
        }

        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .navigationBarsPadding()
                .padding(horizontal = 12.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (logExpanded) {
                LogPanel(session = session)
            }
            ControlBar(
                session = session,
                logExpanded = logExpanded,
                onToggleLog = { logExpanded = !logExpanded },
            )
        }
    }
}

@Composable
private fun CameraWindow(
    modifier: Modifier,
    previewBuffer: Size,
    onPreviewSurface: (Surface) -> Unit,
) {
    val currentBuffer = rememberUpdatedState(previewBuffer)
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(16.dp))
            .background(ViewerBackground),
    ) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { context ->
                TextureView(context).apply {
                    val applyTransform = {
                        val buffer = currentBuffer.value
                        setTransform(previewTransform(width, height, buffer.width, buffer.height))
                    }
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(
                            texture: SurfaceTexture,
                            width: Int,
                            height: Int,
                        ) {
                            applyTransform()
                            onPreviewSurface(Surface(texture))
                        }

                        override fun onSurfaceTextureSizeChanged(
                            texture: SurfaceTexture,
                            width: Int,
                            height: Int,
                        ) {
                            applyTransform()
                        }

                        override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean =
                            true

                        override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {
                        }
                    }
                }
            },
            update = { view ->
                val buffer = currentBuffer.value
                view.setTransform(
                    previewTransform(view.width, view.height, buffer.width, buffer.height),
                )
            },
        )
    }
}

/**
 * 预览的绘制矩阵：相机缓冲是横向的，而界面锁竖屏，所以要顺时针转 90°，
 * 再按等比缩放裁切铺满。SurfaceView 无法对内容做变换，只能这么做才不变形。
 */
private fun previewTransform(
    viewWidth: Int,
    viewHeight: Int,
    bufferWidth: Int,
    bufferHeight: Int,
): Matrix {
    val matrix = Matrix()
    if (viewWidth <= 0 || viewHeight <= 0 || bufferWidth <= 0 || bufferHeight <= 0) return matrix
    val centerX = viewWidth / 2f
    val centerY = viewHeight / 2f
    val viewRect = RectF(0f, 0f, viewWidth.toFloat(), viewHeight.toFloat())
    // 转过 90° 后，内容的宽度对应缓冲的高度。
    val bufferRect = RectF(0f, 0f, bufferHeight.toFloat(), bufferWidth.toFloat())
    bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
    matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
    val scale = max(viewHeight.toFloat() / bufferHeight, viewWidth.toFloat() / bufferWidth)
    matrix.postScale(scale, scale, centerX, centerY)
    matrix.postRotate(90f, centerX, centerY)
    return matrix
}

@Composable
fun ModelView(session: ScanSession, modifier: Modifier = Modifier) {
    val tick = session.previewTick
    val vulkanActive = session.nativeStatus.vulkanActive

    Box(modifier = modifier.background(ViewerBackground)) {
        // 表面始终提供给原生渲染器；Vulkan 不可用时下面用位图预览盖住。
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { context ->
                SurfaceView(context).apply {
                    holder.addCallback(
                        object : SurfaceHolder.Callback {
                            override fun surfaceCreated(holder: SurfaceHolder) {
                                session.attachRenderSurface(holder.surface)
                            }

                            override fun surfaceChanged(
                                holder: SurfaceHolder,
                                format: Int,
                                width: Int,
                                height: Int,
                            ) {
                            }

                            override fun surfaceDestroyed(holder: SurfaceHolder) {
                                session.detachRenderSurface()
                            }
                        },
                    )
                }
            },
        )

        if (!vulkanActive && tick > 0) {
            Image(
                bitmap = remember(tick) { session.preview.asImageBitmap() },
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Fit,
            )
        }
    }
}

/** 主窗口上的白色空心圆环，用来指示此刻正在扫描的位置。 */
@Composable
private fun ScanRingOverlay(ringX: Float, ringY: Float, modifier: Modifier) {
    Canvas(modifier = modifier) {
        val center = Offset(
            size.width * ringX.coerceIn(0.05f, 0.95f),
            size.height * ringY.coerceIn(0.05f, 0.95f),
        )
        val radius = size.minDimension * 0.09f
        drawCircle(
            color = ScanRingColor,
            radius = radius,
            center = center,
            style = Stroke(width = 3.dp.toPx()),
        )
        drawCircle(
            color = ScanRingColor.copy(alpha = 0.35f),
            radius = radius * 0.18f,
            center = center,
            style = Stroke(width = 1.5f.dp.toPx()),
        )
    }
}

@Composable
private fun HintCapsule(text: String, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(50))
            .background(MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.85f))
            .padding(horizontal = 14.dp, vertical = 8.dp),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

@Composable
private fun ControlBar(
    session: ScanSession,
    logExpanded: Boolean,
    onToggleLog: () -> Unit,
) {
    val controls = ScanUiLogic.controls(session.phase)
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (controls.start) {
            CapsuleButton(
                text = stringResource(R.string.action_start),
                emphasized = true,
                onClick = session::start,
            )
        }
        LogBar(
            session = session,
            expanded = logExpanded,
            onToggle = onToggleLog,
            modifier = Modifier.weight(1f),
        )
        if (controls.pause) {
            CapsuleButton(text = stringResource(R.string.action_pause), onClick = session::pause)
        }
        if (controls.resume) {
            CapsuleButton(text = stringResource(R.string.action_resume), onClick = session::resume)
        }
        if (controls.stop) {
            CapsuleButton(text = stringResource(R.string.action_stop), onClick = session::stop)
        }
    }
}

@Composable
private fun CapsuleButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    emphasized: Boolean = false,
) {
    val container = if (emphasized) {
        MaterialTheme.colorScheme.primary
    } else {
        MaterialTheme.colorScheme.surfaceContainerHigh
    }
    val content = if (emphasized) {
        MaterialTheme.colorScheme.onPrimary
    } else {
        MaterialTheme.colorScheme.onSurface
    }
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(50))
            .background(container)
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 12.dp),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelLarge,
            color = content,
        )
    }
}

@Composable
private fun LogBar(
    session: ScanSession,
    expanded: Boolean,
    onToggle: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val kind = ScanUiLogic.logPrimaryKind(session.phase)
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(50))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .clickable(onClick = onToggle)
            .padding(horizontal = 18.dp, vertical = 10.dp),
    ) {
        when (kind) {
            LogPrimaryKind.Accuracy -> AccuracyLine(session)
            LogPrimaryKind.Status -> StatusLine(session)
        }
    }
}

@Composable
private fun AccuracyLine(session: ScanSession) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            text = stringResource(R.string.log_bar_title),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        val accuracy = ScanUiLogic.accuracyText(session.accuracy)
        Text(
            text = if (accuracy == null) {
                stringResource(R.string.log_accuracy_calibrating)
            } else {
                stringResource(R.string.log_accuracy, session.accuracy)
            },
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Text(
            text = "${session.model.gaussianCount} 点 · ${session.telemetryFps()} fps",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun StatusLine(session: ScanSession) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            text = statusLabel(session.phase),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Text(
            text = session.telemetrySummary(),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun LogPanel(session: ScanSession) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(200.dp)
            .clip(RoundedCornerShape(20.dp))
            .background(MaterialTheme.colorScheme.surfaceContainer)
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = stringResource(R.string.debug_vision_pose),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f),
                )
                androidx.compose.material3.Switch(
                    checked = session.visionPoseEnabled,
                    onCheckedChange = { session.changeVisionPose(it) },
                )
            }
            session.logs.forEach { line ->
                Text(
                    text = line,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun statusLabel(phase: ScanPhase): String = when (phase) {
    ScanPhase.Idle -> stringResource(R.string.state_idle)
    ScanPhase.Scanning -> stringResource(R.string.state_scanning)
    ScanPhase.Paused -> stringResource(R.string.state_paused)
    ScanPhase.Finalizing -> stringResource(R.string.state_finalizing)
}