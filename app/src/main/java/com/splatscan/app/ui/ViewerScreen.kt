package com.splatscan.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.splatscan.app.R
import com.splatscan.app.data.ScanRecord
import com.splatscan.app.data.ScanStore
import com.splatscan.app.session.ScanSession
import com.splatscan.app.ui.theme.ViewerBackground

/**
 * 模型查看器：载入一条扫描记录，用轨道相机看它。
 *
 * 手势与常见三维查看器一致：单指拖动旋转、双指捏合缩放。
 * 渲染仍由原生层完成，Vulkan 可用时直出 Surface，否则用 CPU 位图预览。
 */
@Composable
fun ViewerScreen(
    session: ScanSession,
    record: ScanRecord,
    store: ScanStore,
    modifier: Modifier = Modifier,
    onBack: () -> Unit,
) {
    var loaded by remember(record.id) { mutableStateOf(true) }

    LaunchedEffect(record.id) {
        loaded = session.openModel(store.modelFile(record.id).absolutePath)
    }

    // 查看器页没有扫描页的采集循环，位图预览必须在这里自己刷新：
    // Vulkan 直出时无所谓，回退到 CPU 预览时没有这个循环就是一整片黑。
    LaunchedEffect(record.id) {
        while (true) {
            session.refreshViewerPreview()
            kotlinx.coroutines.delay(120L)
        }
    }

    DisposableEffect(record.id) {
        onDispose { session.closeModel() }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(ViewerBackground),
    ) {
        ModelView(session = session, modifier = Modifier.fillMaxSize())

        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(record.id) {
                    detectTransformGestures { _, pan, zoom, _ ->
                        session.orbitBy(pan.x, pan.y, zoom)
                    }
                },
        )

        Row(
            modifier = Modifier
                .align(Alignment.TopStart)
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(horizontal = 12.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ViewerCapsule(text = stringResource(R.string.action_back), onClick = onBack)
            Text(
                text = record.name,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurface,
            )
        }

        Text(
            text = if (loaded) {
                stringResource(R.string.viewer_hint)
            } else {
                stringResource(R.string.viewer_empty)
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(16.dp),
        )
    }
}

@Composable
private fun ViewerCapsule(text: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .clickable(onClick = onClick)
            .padding(horizontal = 18.dp, vertical = 10.dp),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}