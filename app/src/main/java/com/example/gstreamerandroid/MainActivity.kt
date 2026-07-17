package com.example.gstreamerandroid

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.Keep
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
import io.sentry.Sentry
import io.sentry.android.core.SentryAndroid
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : ComponentActivity() {

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

    private var uiMessage by mutableStateOf("Initializing...")
    private var uiPosition by mutableIntStateOf(0)
    private var uiDuration by mutableIntStateOf(0)
    private var mediaWidth by mutableIntStateOf(0)
    private var mediaHeight by mutableIntStateOf(0)
    private var isGStreamerInitialized by mutableStateOf(false)
    private var isPlaying by mutableStateOf(false)
    private var isDraggingSlider by mutableStateOf(false)
    private var currentUri by mutableStateOf<String?>(null)

    data class CameraDevice(val name: String, val info: String)
    private var cameraList by mutableStateOf(listOf<CameraDevice>())
    private var hasScanned by mutableStateOf(false)

    private lateinit var usbManager: UsbManager

    companion object {
        private const val PERMISSION_REQUEST_CAMERA = 1001

        @JvmStatic private external fun nativeClassInit(): Boolean

        init {
            System.loadLibrary("c++_shared")
            System.loadLibrary("gstreamer_android")
            System.loadLibrary("tutorial-4")
            nativeClassInit()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        SentryAndroid.init(this) { options ->
            options.dsn = "http://7176336d82c242249a607875b922be3d@103.197.190.23:9020/2"
            options.isDebug = true
            options.environment = "development"
        }
        Sentry.addBreadcrumb("App started with Sentry enabled")

        // Request CAMERA permission if not already granted
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), PERMISSION_REQUEST_CAMERA)
        }

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

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSION_REQUEST_CAMERA) {
            val granted = grantResults.isNotEmpty() &&
                    grantResults[0] == PackageManager.PERMISSION_GRANTED
            Sentry.addBreadcrumb("CAMERA permission ${if (granted) "granted" else "denied"}")
            if (granted) {
                Toast.makeText(this, "Camera permission granted", Toast.LENGTH_SHORT).show()
            } else {
                Toast.makeText(this, "Camera permission denied – cannot use camera", Toast.LENGTH_LONG).show()
            }
        }
    }

    @Keep
    private fun setMessage(message: String) {
        Sentry.captureMessage("Native: $message")
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

    private fun buildUSBDetail(device: UsbDevice): String {
        val sb = StringBuilder()
        sb.append("DeviceName: ${device.deviceName}\n")
        sb.append("VID: ${device.vendorId}, PID: ${device.productId}\n")
        sb.append("Class: ${device.deviceClass}, Subclass: ${device.deviceSubclass}, Protocol: ${device.deviceProtocol}\n")
        sb.append("Interfaces:\n")
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            sb.append("  - id: ${iface.id}, class: ${iface.interfaceClass}, " +
                    "subclass: ${iface.interfaceSubclass}, protocol: ${iface.interfaceProtocol}\n")
        }
        return sb.toString()
    }

    private fun scanUSBDevices() {
        cameraList = emptyList()
        hasScanned = true

        val devices = usbManager.deviceList.values
        Log.d("USB_SCAN", "Number of USB devices: ${devices.size}")

        val jsonArray = JSONArray()
        for (device in devices) {
            val detail = buildUSBDetail(device)
            cameraList = cameraList + CameraDevice(device.deviceName ?: "Unknown", detail)

            val jsonDevice = JSONObject().apply {
                put("name", device.deviceName)
                put("vid", device.vendorId)
                put("pid", device.productId)
                put("class", device.deviceClass)
                put("subclass", device.deviceSubclass)
                put("protocol", device.deviceProtocol)
            }
            val ifacesArr = JSONArray()
            for (i in 0 until device.interfaceCount) {
                val iface = device.getInterface(i)
                ifacesArr.put(JSONObject().apply {
                    put("id", iface.id)
                    put("class", iface.interfaceClass)
                    put("subclass", iface.interfaceSubclass)
                    put("protocol", iface.interfaceProtocol)
                })
            }
            jsonDevice.put("interfaces", ifacesArr)
            jsonArray.put(jsonDevice)
        }

        if (devices.isEmpty()) {
            cameraList = cameraList + CameraDevice("No USB devices", "")
        }

        Sentry.captureMessage("USB scan result: ${devices.size} devices, data: $jsonArray")
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
                    modifier = Modifier.weight(1f).padding(start = 16.dp),
                    enabled = isGStreamerInitialized && uiDuration > 0
                )
            }

            // Play / Pause / Open File
            Row(
                horizontalArrangement = Arrangement.Center,
                modifier = Modifier.fillMaxWidth().padding(16.dp)
            ) {
                Button(
                    onClick = { isPlaying = true; nativePlay() },
                    enabled = isGStreamerInitialized && currentUri != null && !isPlaying
                ) { Text("Play") }

                Spacer(Modifier.width(16.dp))

                Button(
                    onClick = { isPlaying = false; nativePause() },
                    enabled = isGStreamerInitialized && currentUri != null && isPlaying
                ) { Text("Pause") }

                Spacer(Modifier.width(16.dp))

                Button(
                    onClick = { openVideoLauncher.launch(arrayOf("video/*")) },
                    enabled = isGStreamerInitialized
                ) { Text("Open File") }
            }

            HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

            // Camera Section
            Button(
                onClick = { scanUSBDevices() },
                enabled = isGStreamerInitialized,
                modifier = Modifier.padding(horizontal = 16.dp)
            ) {
                Text("📷 Scan USB Devices")
            }

            Spacer(Modifier.height(8.dp))

            Button(
                onClick = {
                    val camIndex = 1
                    Sentry.addBreadcrumb("Starting camera with index $camIndex")
                    Sentry.captureMessage("Camera button pressed, index=$camIndex")
                    Toast.makeText(this@MainActivity, "Starting camera index $camIndex", Toast.LENGTH_SHORT).show()
                    nativePlayCamera(camIndex)
                },
                enabled = isGStreamerInitialized,
                modifier = Modifier.padding(horizontal = 16.dp)
            ) {
                Text("▶ Start Camera (index 1)")
            }

            Spacer(Modifier.height(8.dp))

            if (cameraList.isNotEmpty()) {
                LazyColumn(modifier = Modifier.fillMaxWidth().weight(0.5f)) {
                    items(cameraList) { cam ->
                        Card(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 16.dp, vertical = 4.dp),
                            elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
                        ) {
                            Column(modifier = Modifier.padding(12.dp)) {
                                Text(
                                    text = cam.name,
                                    style = MaterialTheme.typography.titleSmall
                                )
                                Text(
                                    text = cam.info,
                                    style = MaterialTheme.typography.bodySmall,
                                    modifier = Modifier.padding(top = 4.dp)
                                )
                            }
                        }
                    }
                }
            } else if (hasScanned) {
                Text(
                    "No USB devices found",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
        }
    }
}