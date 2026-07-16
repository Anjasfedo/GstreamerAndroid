package com.example.gstreamerandroid

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.annotation.Keep
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import org.freedesktop.gstreamer.GStreamer
import java.text.SimpleDateFormat
import java.util.*
import com.example.gstreamerandroid.ui.theme.GstreamerAndroidTheme

class MainActivity : ComponentActivity() {

    private external fun nativeInit()
    private external fun nativeFinalize()
    private external fun nativeSetUri(uri: String)
    private external fun nativePlay()
    private external fun nativePause()
    private external fun nativeSetPosition(milliseconds: Int)
    private external fun nativeSurfaceInit(surface: Any)
    private external fun nativeSurfaceFinalize()

    @Keep
    private var native_custom_data: Long = 0

    private val defaultMediaUri = "https://gstreamer.freedesktop.org/data/media/sintel_trailer-368p.ogv"

    private var uiMessage by mutableStateOf("Initializing...")
    private var uiPosition by mutableIntStateOf(0)
    private var uiDuration by mutableIntStateOf(0)
    private var mediaWidth by mutableIntStateOf(0)
    private var mediaHeight by mutableIntStateOf(0)
    private var isGStreamerInitialized by mutableStateOf(false)
    private var isPlaying by mutableStateOf(false)
    private var isDraggingSlider by mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        try {
            GStreamer.init(this)
        } catch (e: Exception) {
            Toast.makeText(this, e.message, Toast.LENGTH_LONG).show()
            finish()
            return
        }

        setContent {
            GstreamerAndroidTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    MediaPlayerScreen(
                        modifier = Modifier.padding(innerPadding)
                    )
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
        runOnUiThread {
            uiMessage = message
        }
    }

    @Keep
    private fun onGStreamerInitialized() {
        runOnUiThread {
            isGStreamerInitialized = true
            nativeSetUri(defaultMediaUri)
            nativeSetPosition(uiPosition)
            if (isPlaying) {
                nativePlay()
            } else {
                nativePause()
            }
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

    private fun formatTime(millis: Int): String {
        val df = SimpleDateFormat("HH:mm:ss", Locale.US)
        df.timeZone = TimeZone.getTimeZone("UTC")
        return df.format(Date(millis.toLong()))
    }

    @Composable
    fun MediaPlayerScreen(modifier: Modifier = Modifier) {
        Column(
            modifier = modifier.fillMaxSize(),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {

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
                            if (mediaWidth > 0 && mediaHeight > 0) {
                                mediaWidth.toFloat() / mediaHeight.toFloat()
                            } else {
                                16f / 9f
                            }
                        )
                )
            }

            Text(
                text = uiMessage,
                modifier = Modifier.padding(8.dp)
            )

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
                    enabled = isGStreamerInitialized
                )
            }

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
                    enabled = isGStreamerInitialized && !isPlaying
                ) {
                    Text("Play")
                }

                Spacer(modifier = Modifier.width(16.dp))

                Button(
                    onClick = {
                        isPlaying = false
                        nativePause()
                    },
                    enabled = isGStreamerInitialized && isPlaying
                ) {
                    Text("Pause")
                }
            }
        }
    }

    companion object {
        @JvmStatic
        private external fun nativeClassInit(): Boolean

        init {
            System.loadLibrary("gstreamer_android")
            System.loadLibrary("tutorial-4")
            nativeClassInit()
        }
    }
}