package dev.pixelmirroring.app.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

class BootReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "BootReceiver"
    }

    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action == Intent.ACTION_BOOT_COMPLETED) {
            val serviceIntent = Intent(context, MirroringService::class.java)
            try {
                context.startForegroundService(serviceIntent)
                Log.i(TAG, "Started MirroringService from BootReceiver")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start MirroringService from BootReceiver", e)
            }
        }
    }
}