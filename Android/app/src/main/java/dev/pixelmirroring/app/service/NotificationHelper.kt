package dev.pixelmirroring.app.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import dev.pixelmirroring.app.MainActivity

object NotificationHelper {
    private const val CHANNEL_ID = "pixel_mirroring_service_channel"
    private const val CHANNEL_NAME = "Pixel Mirroring Service"

    fun createNotification(
        context: Context,
        contentText: String = "Bereit für Verbindung | Tap für Einstellungen"
    ): Notification {
        val notificationManager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Läuft im Hintergrund für eingehende Mirroring-Anfragen"
            }
            notificationManager.createNotificationChannel(channel)
        }

        val pendingIntent = PendingIntent.getActivity(
            context,
            0,
            Intent(context, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(context, CHANNEL_ID)
            // .setSmallIcon(R.mipmap.ic_launcher) // We use standard launcher icon or fallback
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setContentTitle("Pixel Mirroring")
            .setContentText(contentText)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }
}
