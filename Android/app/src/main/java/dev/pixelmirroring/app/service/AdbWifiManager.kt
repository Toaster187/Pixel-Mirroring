package dev.pixelmirroring.app.service

import android.content.Context
import android.provider.Settings
import android.util.Log

class AdbWifiManager(private val context: Context) {

    companion object {
        private const val TAG = "AdbWifiManager"
        private const val ADB_WIFI_ENABLED = "adb_wifi_enabled"
        private const val ADB_TCP_PORT = "adb_tcp_port"
    }

    /**
     * Aktiviert ADB over WiFi über Settings.Global.
     * Setzt zwingend die Berechtigung WRITE_SECURE_SETTINGS voraus (erteilt via ADB).
     */
    suspend fun enableAdbWifi(): Boolean {
        return try {
            // First turn it off to ensure daemon restarts when we turn it back on
            Settings.Global.putInt(
                context.contentResolver,
                ADB_WIFI_ENABLED,
                0
            )
            kotlinx.coroutines.delay(100)
            Settings.Global.putInt(
                context.contentResolver,
                ADB_WIFI_ENABLED,
                1
            )
            Log.i(TAG, "ADB over WiFi activated successfully.")
            true
        } catch (e: SecurityException) {
            Log.e(TAG, "Failed to enable ADB over WiFi: Missing WRITE_SECURE_SETTINGS permission", e)
            false
        } catch (e: Exception) {
            Log.e(TAG, "Exception while toggling ADB WiFi", e)
            false
        }
    }

    /**
     * Startet das ADB TCP/IP Protokoll auf einem festen Port (Standard 5555).
     */
    suspend fun enableAdbTcpIp(port: Int = 5555): Boolean {
        return try {
            // Set to -1 first to trigger change observer
            Settings.Global.putString(
                context.contentResolver,
                ADB_TCP_PORT,
                "-1"
            )
            kotlinx.coroutines.delay(100)
            Settings.Global.putString(
                context.contentResolver,
                ADB_TCP_PORT,
                port.toString()
            )
            Log.i(TAG, "ADB TCP/IP activated on port $port.")
            true
        } catch (e: SecurityException) {
            Log.e(TAG, "Failed to set ADB TCP port: Missing WRITE_SECURE_SETTINGS permission", e)
            false
        } catch (e: Exception) {
            Log.e(TAG, "Exception while toggling ADB TCP port", e)
            false
        }
    }

    /**
     * Prüft ob ADB over WiFi aktuell aktiv ist.
     */
    fun isAdbWifiEnabled(): Boolean {
        return try {
            Settings.Global.getInt(
                context.contentResolver,
                ADB_WIFI_ENABLED,
                0
            ) == 1
        } catch (e: Settings.SettingNotFoundException) {
            false
        }
    }

    /**
     * Prüft ob die App die nötige Berechtigung WRITE_SECURE_SETTINGS besitzt.
     */
    fun hasSecureSettingsPermission(): Boolean {
        return context.checkSelfPermission(android.Manifest.permission.WRITE_SECURE_SETTINGS) == 
               android.content.pm.PackageManager.PERMISSION_GRANTED
    }
}
