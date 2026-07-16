package org.freedesktop.gstreamer

import android.content.Context
import android.content.res.AssetManager
import java.io.*

object GStreamer {
    @JvmStatic
    @Throws(Exception::class)
    private external fun nativeInit(context: Context)

    @JvmStatic
    @Throws(Exception::class)
    fun init(context: Context) {
        copyCaCertificates(context)
        copyFonts(context)
        nativeInit(context)
    }

    private fun copyFonts(context: Context) {
        val assetManager = context.assets
        val filesDir = context.filesDir
        val fontsFCDir = File(filesDir, "fontconfig")
        val fontsDir = File(fontsFCDir, "fonts")
        val fontsCfg = File(fontsFCDir, "fonts.conf")
        fontsDir.mkdirs()
        try {
            copyFile(assetManager, "fontconfig/fonts.conf", fontsCfg)
            for (filename in assetManager.list("fontconfig/fonts/truetype")!!) {
                val font = File(fontsDir, filename)
                copyFile(assetManager, "fontconfig/fonts/truetype/$filename", font)
            }
        } catch (e: IOException) {
            e.printStackTrace()
        }
    }

    private fun copyCaCertificates(context: Context) {
        val assetManager = context.assets
        val filesDir = context.filesDir
        val sslDir = File(filesDir, "ssl")
        val certsDir = File(sslDir, "certs")
        val certs = File(certsDir, "ca-certificates.crt")
        certsDir.mkdirs()
        try {
            copyFile(assetManager, "ssl/certs/ca-certificates.crt", certs)
        } catch (e: IOException) {
            e.printStackTrace()
        }
    }

    @Throws(IOException::class)
    private fun copyFile(assetManager: AssetManager, assetPath: String, outFile: File) {
        var `in`: InputStream? = null
        var out: OutputStream? = null
        var exception: IOException? = null
        if (outFile.exists()) outFile.delete()
        try {
            `in` = assetManager.open(assetPath)
            out = FileOutputStream(outFile)
            val buffer = ByteArray(1024)
            var read: Int
            while (`in`.read(buffer).also { read = it } != -1) {
                out.write(buffer, 0, read)
            }
            out.flush()
        } catch (e: IOException) {
            exception = e
        } finally {
            if (`in` != null) try {
                `in`.close()
            } catch (e: IOException) {
                if (exception == null) exception = e
            }
            if (out != null) try {
                out.close()
            } catch (e: IOException) {
                if (exception == null) exception = e
            }
            if (exception != null) throw exception
        }
    }
}