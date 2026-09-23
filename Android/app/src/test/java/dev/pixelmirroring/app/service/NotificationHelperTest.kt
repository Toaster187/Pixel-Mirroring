package dev.pixelmirroring.app.service

import android.app.Notification
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36])
class NotificationHelperTest {
    private val context: Context get() = ApplicationProvider.getApplicationContext()

    @Test
    fun activeConnectionRequestsLocalLiveStatus() {
        val notification = NotificationHelper.createNotification(context, "PC verbunden", true)
        assertTrue(notification.extras.getBoolean("android.requestPromotedOngoing"))
        assertEquals("PC", notification.shortCriticalText)
        assertTrue(notification.flags and Notification.FLAG_ONGOING_EVENT != 0)
        assertTrue(notification.flags and Notification.FLAG_LOCAL_ONLY != 0)
        assertTrue(notification.flags and Notification.FLAG_ONLY_ALERT_ONCE != 0)
    }

    @Test
    fun disconnectedStateDropsPromotion() {
        NotificationHelper.createNotification(context, "PC verbunden", true)
        val idle = NotificationHelper.createNotification(context)
        assertFalse(idle.extras.getBoolean("android.requestPromotedOngoing"))
        assertEquals(null, idle.shortCriticalText)
    }

    @Test
    @Config(sdk = [30, 35])
    fun olderAndroidKeepsStandardNotification() {
        val notification = NotificationHelper.createNotification(context, "PC verbunden", true)
        assertFalse(notification.extras.getBoolean("android.requestPromotedOngoing"))
        assertEquals("PC verbunden", notification.extras.getCharSequence(Notification.EXTRA_TEXT))
        assertTrue(notification.flags and Notification.FLAG_LOCAL_ONLY != 0)
    }
}
