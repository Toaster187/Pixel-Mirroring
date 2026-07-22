package dev.pixelmirroring.app.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.MediaScannerConnection
import android.util.Log

class MediaScannerReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val path = intent.getStringExtra("path")
        if (path != null) {
            Log.d("MediaScannerReceiver", "Scanning path: $path")
            MediaScannerConnection.scanFile(context, arrayOf(path), null) { _, uri ->
                Log.d("MediaScannerReceiver", "Scanned to URI: $uri")
            }
        }
    }
}
