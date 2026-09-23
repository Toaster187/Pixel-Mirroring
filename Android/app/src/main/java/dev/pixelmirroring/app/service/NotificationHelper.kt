package dev.pixelmirroring.app.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.core.app.NotificationCompat
import dev.pixelmirroring.app.MainActivity

object NotificationHelper {
    private const val CHANNEL_ID = "pixel_mirroring_service_channel"
    private const val CHANNEL_NAME = "Pixel Mirroring Service"

    fun createNotification(
        context: Context,
        contentText: String = "Bereit für Verbindung | Tippen für Einstellungen",
        sessionActive: Boolean = false
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

        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            // .setSmallIcon(R.mipmap.ic_launcher) // We use standard launcher icon or fallback
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setContentTitle("Pixel Mirroring")
            .setContentText(contentText)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setLocalOnly(true)
            .build()

        // Promotion is a request: Android and the user's notification preferences
        // decide whether to show a status chip. Idle discovery must never be promoted.
        if (Build.VERSION.SDK_INT >= 36 && sessionActive) {
            return Notification.Builder.recoverBuilder(context, notification)
                // The public setter arrived in 36.1. Its documented extra also
                // works there when compiled against 36, without a new dependency.
                .addExtras(Bundle().apply { putBoolean("android.requestPromotedOngoing", true) })
                .setShortCriticalText("PC")
                .build()
        }
        return notification
    }
}
