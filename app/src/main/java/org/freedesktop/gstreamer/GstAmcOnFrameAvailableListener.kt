package org.freedesktop.gstreamer

import android.graphics.SurfaceTexture

class GstAmcOnFrameAvailableListener : SurfaceTexture.OnFrameAvailableListener {
    @get:Synchronized
    @set:Synchronized
    var context: Long = 0
    @Synchronized
    override fun onFrameAvailable(surfaceTexture: SurfaceTexture) {
        native_onFrameAvailable(context, surfaceTexture)
    }

    companion object {
        @JvmStatic
        private external fun native_onFrameAvailable(context: Long, surfaceTexture: SurfaceTexture)
    }
}