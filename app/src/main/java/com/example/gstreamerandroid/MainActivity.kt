package com.example.gstreamerandroid

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.Keep
import androidx.annotation.RequiresApi
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.example.gstreamerandroid.ui.theme.GstreamerAndroidTheme
import org.freedesktop.gstreamer.GStreamer
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.*
import androidx.compose.foundation.clickable

class MainActivity : ComponentActivity() {

    // -- native methods --
    private external fun nativeInit()
    private external fun nativeFinalize()
    private external fun nativeSetUri(uri: String)
    private external fun nativePlay()
    private external fun nativePause()
    private external fun nativeSetPosition(milliseconds: Int)
    private external fun nativeSurfaceInit(surface: Any)
    private external fun nativeSurfaceFinalize()
    private external fun nativeGetVersion(): String
    private external fun nativeStartCameraDiscovery()

    private external fun nativePlayCamera(cameraIndex: Int)

    @Keep
    private var native_custom_data: Long = 0

    // UI states
    private var uiMessage by mutableStateOf("Initializing...")
    private var uiPosition by mutableIntStateOf(0)
    private var uiDuration by mutableIntStateOf(0)
    private var mediaWidth by mutableIntStateOf(0)
    private var mediaHeight by mutableIntStateOf(0)
    private var isGStreamerInitialized by mutableStateOf(false)
    private var isPlaying by mutableStateOf(false)
    private var isDraggingSlider by mutableStateOf(false)
    private var currentUri by mutableStateOf<String?>(null)

    // Camera list (will hold both USB and Camera2 entries)
    data class CameraDevice(val name: String, val path: String)
    private var cameraList by mutableStateOf(listOf<CameraDevice>())
    private var hasScanned by mutableStateOf(false)

    private lateinit var usbManager: UsbManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        try {
            GStreamer.init(this)
        } catch (e: Exception) {
            Toast.makeText(this, e.message, Toast.LENGTH_LONG).show()
            finish()
            return
        }

        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager

        setContent {
            GstreamerAndroidTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    MediaPlayerScreen(modifier = Modifier.padding(innerPadding))
                }
            }
        }

        nativeInit()
    }

    override fun onDestroy() {
        nativeFinalize()
        super.onDestroy()
    }

    @Keep
    private fun setMessage(message: String) {
        runOnUiThread { uiMessage = message }
    }

    @Keep
    private fun onGStreamerInitialized() {
        runOnUiThread {
            uiMessage = "GStreamer ${nativeGetVersion()} ready"
            isGStreamerInitialized = true
        }
    }

    @Keep
    private fun setCurrentPosition(position: Int, duration: Int) {
        if (isDraggingSlider) return
        runOnUiThread {
            uiPosition = position
            uiDuration = duration
        }
    }

    @Keep
    private fun onMediaSizeChanged(width: Int, height: Int) {
        runOnUiThread {
            mediaWidth = width
            mediaHeight = height
        }
    }

    @Keep
    private fun onCameraDeviceFound(name: String, path: String) {
        runOnUiThread {
            cameraList = cameraList + CameraDevice(name, path)
        }
    }

    private fun copyUriToCache(context: Context, contentUri: Uri): String? {
        return try {
            val inputStream = context.contentResolver.openInputStream(contentUri) ?: return null
            val cacheFile = File(context.cacheDir, "video_${System.currentTimeMillis()}.mp4")
            FileOutputStream(cacheFile).use { outputStream ->
                inputStream.copyTo(outputStream)
            }
            inputStream.close()
            cacheFile.absolutePath.let { "file://$it" }
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    private fun playUri(fileUri: String) {
        currentUri = fileUri
        nativeSetUri(fileUri)
        nativePlay()
        isPlaying = true
    }

    private fun formatTime(millis: Int): String {
        val df = SimpleDateFormat("HH:mm:ss", Locale.US)
        df.timeZone = TimeZone.getTimeZone("UTC")
        return df.format(Date(millis.toLong()))
    }

    /**
     * Lists all USB devices and Camera2 cameras directly in the app.
     * No USB permission required – just enumerates what the system sees.
     */
    @RequiresApi(Build.VERSION_CODES.LOLLIPOP)
    private fun scanUSBDevicesAndCamera2() {
        cameraList = emptyList()
        hasScanned = true

        // 1. Show all USB devices
        val usbSection = mutableListOf<CameraDevice>()
        for (device in usbManager.deviceList.values) {
            val name = device.deviceName ?: "Unknown"
            val vid = device.vendorId
            val pid = device.productId
            val interfaces = buildString {
                for (i in 0 until device.interfaceCount) {
                    if (i > 0) append(", ")
                    append("class=${device.getInterface(i).interfaceClass}")
                }
            }
            val detail = "VID:$vid PID:$pid interfaces: $interfaces"
            usbSection.add(CameraDevice(name = name, path = detail))
        }

        if (usbSection.isNotEmpty()) {
            cameraList = cameraList + CameraDevice(name = "--- USB Devices ---", path = "")
            cameraList = cameraList + usbSection
        } else {
            cameraList = cameraList + CameraDevice(name = "No USB devices found", path = "")
        }

        // 2. Show Camera2 devices (works for many UVC cameras)
        try {
            val cameraManager = getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val cameraIds = cameraManager.cameraIdList
            if (cameraIds.isNotEmpty()) {
                cameraList = cameraList + CameraDevice(name = "--- Camera2 Devices ---", path = "")
                for (id in cameraIds) {
                    val chars = cameraManager.getCameraCharacteristics(id)
                    val facing = chars.get(CameraCharacteristics.LENS_FACING)
                    val isExternal = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                        chars.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL) ==
                                CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL
                    } else false
                    val desc = when {
                        isExternal -> "External USB"
                        facing == CameraCharacteristics.LENS_FACING_FRONT -> "Front"
                        facing == CameraCharacteristics.LENS_FACING_BACK -> "Back"
                        else -> "Other"
                    }
                    cameraList = cameraList + CameraDevice(name = "$desc camera", path = id)
                }
            } else {
                cameraList = cameraList + CameraDevice(name = "No Camera2 devices", path = "")
            }
        } catch (e: Exception) {
            cameraList = cameraList + CameraDevice(name = "Camera2 error: ${e.message}", path = "")
        }

        // 3. Also call native GStreamer device monitor (likely empty due to SELinux)
        nativeStartCameraDiscovery()
    }

    @Composable
    fun MediaPlayerScreen(modifier: Modifier = Modifier) {
        val openVideoLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument()
        ) { uri: Uri? ->
            uri?.let {
                val fileUri = copyUriToCache(this@MainActivity, it)
                if (fileUri != null) {
                    playUri(fileUri)
                } else {
                    setMessage("Error: could not open file")
                }
            }
        }

        Column(
            modifier = modifier.fillMaxSize(),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Video surface
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f),
                contentAlignment = Alignment.Center
            ) {
                AndroidView(
                    factory = { context ->
                        SurfaceView(context).apply {
                            holder.addCallback(object : SurfaceHolder.Callback {
                                override fun surfaceCreated(holder: SurfaceHolder) {}
                                override fun surfaceChanged(
                                    holder: SurfaceHolder,
                                    format: Int,
                                    width: Int,
                                    height: Int
                                ) {
                                    nativeSurfaceInit(holder.surface)
                                }
                                override fun surfaceDestroyed(holder: SurfaceHolder) {
                                    nativeSurfaceFinalize()
                                }
                            })
                        }
                    },
                    modifier = Modifier
                        .fillMaxWidth()
                        .aspectRatio(
                            if (mediaWidth > 0 && mediaHeight > 0)
                                mediaWidth.toFloat() / mediaHeight.toFloat()
                            else 16f / 9f
                        )
                )
            }

            Text(
                text = uiMessage,
                modifier = Modifier.padding(8.dp)
            )

            // Seek bar
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(text = "${formatTime(uiPosition)} / ${formatTime(uiDuration)}")
                Slider(
                    value = if (uiDuration > 0) uiPosition.toFloat() else 0f,
                    onValueChange = { value ->
                        isDraggingSlider = true
                        uiPosition = value.toInt()
                    },
                    onValueChangeFinished = {
                        isDraggingSlider = false
                        nativeSetPosition(uiPosition)
                        if (isPlaying) nativePlay()
                    },
                    valueRange = 0f..(if (uiDuration > 0) uiDuration.toFloat() else 100f),
                    modifier = Modifier
                        .weight(1f)
                        .padding(start = 16.dp),
                    enabled = isGStreamerInitialized && uiDuration > 0
                )
            }

            // Play / Pause / Open File buttons
            Row(
                horizontalArrangement = Arrangement.Center,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp)
            ) {
                Button(
                    onClick = {
                        isPlaying = true
                        nativePlay()
                    },
                    enabled = isGStreamerInitialized && currentUri != null && !isPlaying
                ) { Text("Play") }

                Spacer(modifier = Modifier.width(16.dp))

                Button(
                    onClick = {
                        isPlaying = false
                        nativePause()
                    },
                    enabled = isGStreamerInitialized && currentUri != null && isPlaying
                ) { Text("Pause") }

                Spacer(modifier = Modifier.width(16.dp))

                Button(
                    onClick = {
                        openVideoLauncher.launch(arrayOf("video/*"))
                    },
                    enabled = isGStreamerInitialized
                ) {
                    Text("Open File")
                }
            }

            HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

            // Camera discovery button
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Button(
                    onClick = {
                        scanUSBDevicesAndCamera2()
                    },
                    enabled = isGStreamerInitialized
                ) {
                    Text("📷")
                    Spacer(Modifier.width(8.dp))
                    Text("Scan Cameras")
                }
            }

            // Display results
            if (cameraList.isNotEmpty()) {
                LazyColumn(modifier = Modifier.fillMaxWidth().weight(0.5f)) {
                    items(cameraList) { cam ->
                        if (cam.path.isEmpty()) {
                            // section header
                            Text(
                                text = cam.name,
                                style = MaterialTheme.typography.titleSmall,
                                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)
                            )
                        } else {
                            ListItem(
                                headlineContent = { Text(cam.name) },
                                supportingContent = { Text(cam.path) },
                                modifier = Modifier.clickable {
                                    // If the camera is an external USB, try to play it via ahcsrc
                                    if (cam.name.contains("External USB")) {
                                        // Use camera ID from path (it's the Camera2 ID)
                                        val id = cam.path.toIntOrNull() ?: 0
                                        nativePlayCamera(id)
                                    }
                                }
                            )
                        }
                    }
                }
            } else if (hasScanned) {
                Text(
                    "No cameras or USB devices found",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
        }
    }

    companion object {
        @JvmStatic private external fun nativeClassInit(): Boolean

        init {
            System.loadLibrary("c++_shared")
            System.loadLibrary("gstreamer_android")
            System.loadLibrary("tutorial-4")
            nativeClassInit()
        }
    }
}