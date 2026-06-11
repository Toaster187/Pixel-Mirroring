package dev.pixelmirroring.app.service

import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log
import dev.pixelmirroring.app.data.PairedClientStore
import dev.pixelmirroring.app.network.ConnectRequest
import dev.pixelmirroring.app.network.ConnectResponse
import dev.pixelmirroring.app.network.NetworkScanner
import dev.pixelmirroring.app.network.StatusResponse
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.Json
import java.util.concurrent.atomic.AtomicBoolean

class MirroringService : Service() {
    companion object {
        private const val NOTIFICATION_ID = 18294
        private const val TAG = "MirroringService"
    }

    private val json = Json { ignoreUnknownKeys = true }
    private var server: DiscoveryHttpServer? = null
    private val adbWifiManager by lazy { AdbWifiManager(this) }
    private val clientStore by lazy { PairedClientStore(this) }
    private val pairingMutex = Mutex()
    private val isScreenOn = AtomicBoolean(true)
    private var receiverRegistered = false

    private var wifiLock: android.net.wifi.WifiManager.WifiLock? = null
    private var wakeLock: android.os.PowerManager.WakeLock? = null
    private var networkCallback: android.net.ConnectivityManager.NetworkCallback? = null

    private val screenStateReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: android.content.Context?, intent: android.content.Intent?) {
            if (intent?.action == android.content.Intent.ACTION_SCREEN_OFF) {
                isScreenOn.set(false)
                Log.i(TAG, "Screen went OFF")
            } else if (intent?.action == android.content.Intent.ACTION_SCREEN_ON) {
                isScreenOn.set(true)
                Log.i(TAG, "Screen went ON")
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val powerManager = getSystemService(android.content.Context.POWER_SERVICE) as android.os.PowerManager
        isScreenOn.set(powerManager.isInteractive)
        
        // Acquire Partial WakeLock to prevent CPU sleep
        try {
            wakeLock = powerManager.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "PixelMirroring::WakeLock").apply {
                acquire()
            }
            Log.i(TAG, "CPU WakeLock acquired")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to acquire CPU WakeLock", e)
        }

        // Acquire Low-Latency WifiLock to keep WiFi active and fast
        try {
            val wifiManager = applicationContext.getSystemService(android.content.Context.WIFI_SERVICE) as android.net.wifi.WifiManager
            wifiLock = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                wifiManager.createWifiLock(android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "PixelMirroring::WifiLock")
            } else {
                @Suppress("DEPRECATION")
                wifiManager.createWifiLock(android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF, "PixelMirroring::WifiLock")
            }.apply {
                acquire()
            }
            Log.i(TAG, "Low-Latency WiFi Lock acquired")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to acquire WiFi Lock", e)
        }

        // Monitor Network Connectivity Changes to rebind Discovery Server
        try {
            val connectivityManager = getSystemService(android.content.Context.CONNECTIVITY_SERVICE) as android.net.ConnectivityManager
            val networkRequest = android.net.NetworkRequest.Builder()
                .addCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build()
            
            networkCallback = object : android.net.ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: android.net.Network) {
                    Log.i(TAG, "Network became available, rebinding discovery server...")
                    rebindDiscoveryServer()
                }
                override fun onLost(network: android.net.Network) {
                    Log.i(TAG, "Network connection lost")
                }
            }
            connectivityManager.registerNetworkCallback(networkRequest, networkCallback!!)
            Log.i(TAG, "NetworkCallback registered")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to register NetworkCallback", e)
        }

        val filter = android.content.IntentFilter().apply {
            addAction(android.content.Intent.ACTION_SCREEN_OFF)
            addAction(android.content.Intent.ACTION_SCREEN_ON)
        }
        registerReceiver(screenStateReceiver, filter)
        receiverRegistered = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i(TAG, "Starting MirroringService")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID, 
                NotificationHelper.createNotification(this),
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            )
        } else {
            startForeground(NOTIFICATION_ID, NotificationHelper.createNotification(this))
        }
        startDiscoveryServer()
        return START_STICKY
    }

    private fun startDiscoveryServer() {
        if (server != null) return

        var retryCount = 0
        while (retryCount < 5) {
            try {
                server = DiscoveryHttpServer(port = 18294, requestHandler = ::handleRequest).also {
                    it.start()
                }
                Log.i(TAG, "Discovery server started on port 18294")
                break
            } catch (e: Exception) {
                retryCount++
                Log.e(TAG, "Failed to start discovery server on port 18294. Retry $retryCount/5", e)
                server?.close()
                server = null
                if (retryCount >= 5) {
                    server = null
                } else {
                    try {
                        Thread.sleep(2000)
                    } catch (ie: InterruptedException) {
                        Thread.currentThread().interrupt()
                        Log.w(TAG, "Thread interrupted during server start retry", ie)
                    }
                }
            }
        }
    }

    private fun rebindDiscoveryServer() {
        synchronized(this) {
            server?.close()
            server = null
            startDiscoveryServer()
        }
    }

    private fun handleRequest(request: HttpRequest): HttpResponse {
        return try {
            when {
            request.method == "GET" && request.path == "/ping" -> {
                HttpResponse(
                    statusCode = 200,
                    contentType = "text/plain; charset=utf-8",
                    body = "pong"
                )
            }

            request.method == "GET" && request.path == "/status" -> {
                val response = StatusResponse(
                    adbWifiEnabled = adbWifiManager.isAdbWifiEnabled(),
                    hasPermission = adbWifiManager.hasSecureSettingsPermission(),
                    deviceName = Build.MODEL
                )
                jsonResponse(response)
            }

            request.method == "GET" && request.path == "/screen" -> {
                HttpResponse(
                    statusCode = 200,
                    contentType = "application/json; charset=utf-8",
                    body = "{\"screenOn\":${isScreenOn.get()}}"
                )
            }

            request.method == "POST" && request.path == "/connect" -> {
                val connectRequest = json.decodeFromString<ConnectRequest>(request.body)

                val isAuthorized = runBlocking {
                    pairingMutex.withLock {
                        val alreadyAuthorized = clientStore.isClientPaired(connectRequest.clientId)
                        if (alreadyAuthorized && clientStore.getPairedClient() == null) {
                            // Ugg first friend gets paired.
                            clientStore.savePairedClient(connectRequest.clientId, connectRequest.clientName)
                        }
                        alreadyAuthorized
                    }
                }

                if (!isAuthorized) {
                    return HttpResponse(
                        statusCode = 403,
                        contentType = "text/plain; charset=utf-8",
                        body = ""
                    )
                }

                var adbPort = (5555..5595).random()
                val success = runBlocking {
                    val ok = adbWifiManager.enableAdbWifi() && adbWifiManager.enableAdbTcpIp(adbPort)
                    val dynamicPort = adbWifiManager.getDynamicAdbPort()
                    if (dynamicPort != -1) {
                        adbPort = dynamicPort
                    }
                    ok
                }

                val response = ConnectResponse(
                    success = success,
                    ips = NetworkScanner.getAllLocalIps(this),
                    adbPort = adbPort,
                    deviceName = Build.MODEL
                )
                jsonResponse(response)
            }

            request.path == "/ping" || request.path == "/status" || request.path == "/connect" || request.path == "/screen" -> {
                HttpResponse(
                    statusCode = 405,
                    contentType = "text/plain; charset=utf-8",
                    body = "method not allowed"
                )
            }

            else -> {
                HttpResponse(
                    statusCode = 404,
                    contentType = "text/plain; charset=utf-8",
                    body = "not found"
                )
            }
        }
    } catch (e: Exception) {
        Log.w(TAG, "Failed to handle request ${request.method} ${request.path}", e)
        HttpResponse(
            statusCode = 400,
            contentType = "text/plain; charset=utf-8",
            body = "bad request"
        )
    }
}

    private fun jsonResponse(payload: Any): HttpResponse {
        val body = when (payload) {
            is ConnectResponse -> json.encodeToString(ConnectResponse.serializer(), payload)
            is StatusResponse -> json.encodeToString(StatusResponse.serializer(), payload)
            else -> error("Unsupported payload")
        }

        return HttpResponse(
            statusCode = 200,
            contentType = "application/json; charset=utf-8",
            body = body
        )
    }

    override fun onDestroy() {
        if (receiverRegistered) {
            unregisterReceiver(screenStateReceiver)
            receiverRegistered = false
        }
        
        networkCallback?.let {
            try {
                val connectivityManager = getSystemService(android.content.Context.CONNECTIVITY_SERVICE) as android.net.ConnectivityManager
                connectivityManager.unregisterNetworkCallback(it)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to unregister NetworkCallback", e)
            }
        }
        networkCallback = null

        wifiLock?.let {
            try {
                if (it.isHeld) it.release()
            } catch (e: Exception) {
                Log.w(TAG, "Failed to release WiFi Lock", e)
            }
        }
        wifiLock = null

        wakeLock?.let {
            try {
                if (it.isHeld) it.release()
            } catch (e: Exception) {
                Log.w(TAG, "Failed to release CPU WakeLock", e)
            }
        }
        wakeLock = null

        server?.close()
        server = null
        super.onDestroy()
    }
}
