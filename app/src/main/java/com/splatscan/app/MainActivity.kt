package com.splatscan.app

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.splatscan.app.data.ScanStore
import com.splatscan.app.session.ScanSession
import com.splatscan.app.ui.HistoryScreen
import com.splatscan.app.ui.ScanRoute
import com.splatscan.app.ui.ViewerScreen
import com.splatscan.app.ui.theme.SplatScanTheme
import kotlinx.coroutines.delay
import java.io.File

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            SplatScanTheme {
                SplatScanApp()
            }
        }
    }
}

private enum class Screen { Scan, History, Viewer }

@Composable
fun SplatScanApp() {
    val context = LocalContext.current
    var granted by remember { mutableStateOf(hasCameraPermission(context)) }
    var screen by remember { mutableStateOf(Screen.Scan) }
    var viewing by remember { mutableStateOf<com.splatscan.app.data.ScanRecord?>(null) }
    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { result -> granted = result }

    LaunchedEffect(Unit) {
        if (!granted) launcher.launch(Manifest.permission.CAMERA)
    }

    if (!granted) {
        PermissionScreen(onGrant = { launcher.launch(Manifest.permission.CAMERA) })
        return
    }

    val store = remember { ScanStore(File(context.filesDir, "scans")) }
    val session = remember { ScanSession(context, store) }

    DisposableEffect(session) {
        session.ensureEngine()
        onDispose { session.release() }
    }

    LaunchedEffect(session) {
        while (true) {
            session.tick()
            delay(TICK_INTERVAL_MS)
        }
    }

    when (screen) {
        Screen.Scan -> ScanRoute(
            session = session,
            modifier = Modifier.fillMaxSize(),
            onOpenHistory = { screen = Screen.History },
        )

        Screen.History -> HistoryScreen(
            store = store,
            modifier = Modifier.fillMaxSize(),
            onBack = { screen = Screen.Scan },
            onOpen = { record ->
                viewing = record
                screen = Screen.Viewer
            },
        )

        Screen.Viewer -> viewing?.let { record ->
            ViewerScreen(
                session = session,
                record = record,
                store = store,
                modifier = Modifier.fillMaxSize(),
                onBack = { screen = Screen.History },
            )
        } ?: run { screen = Screen.History }
    }
}

@Composable
private fun PermissionScreen(onGrant: () -> Unit) {
    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(32.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
            horizontalAlignment = Alignment.Start,
        ) {
            Text(
                text = stringResource(R.string.permission_title),
                style = MaterialTheme.typography.titleLarge,
            )
            Text(
                text = stringResource(R.string.permission_body),
                style = MaterialTheme.typography.labelLarge,
            )
            Button(onClick = onGrant) {
                Text(text = stringResource(R.string.permission_grant))
            }
        }
    }
}

private fun hasCameraPermission(context: Context): Boolean =
    context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

private const val TICK_INTERVAL_MS = 250L