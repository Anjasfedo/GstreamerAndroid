package com.example.gstreamerandroid

import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.gstreamerandroid.ui.theme.GstreamerAndroidTheme
import org.freedesktop.gstreamer.GStreamer

class MainActivity : ComponentActivity() {

    private external fun nativeGetGStreamerInfo(): String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        try {
            GStreamer.init(this)
        } catch (e: Exception) {
            Toast.makeText(this, e.message, Toast.LENGTH_LONG).show()
            finish()
            return
        }

        val gstVersion = nativeGetGStreamerInfo()

        setContent {
            GstreamerAndroidTheme {
                Text(
                    text = "GStreamer version:\n$gstVersion",
                    modifier = Modifier.padding(16.dp),
                    fontSize = 18.sp,
                    color = MaterialTheme.colorScheme.onBackground
                )
            }
        }
    }

    companion object {
        init {
            System.loadLibrary("c++_shared")
            System.loadLibrary("gstreamer_android")
            System.loadLibrary("tutorial-4")
        }
    }
}