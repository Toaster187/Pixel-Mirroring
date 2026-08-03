package dev.pixelmirroring.app.service

import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkRequest
import android.net.NetworkCapabilities
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import java.net.InetAddress
import dev.pixelmirroring.app.data.PairedClientStore
import dev.pixelmirroring.app.network.ConnectRequest
import dev.pixelmirroring.app.network.ConnectResponse
import dev.pixelmirroring.app.network.NetworkScanner
import dev.pixelmirroring.app.network.StatusResponse
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.Json
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

class MirroringService : Service() {
    companion object {
        private const val NOTIFICATION_ID = 18294
        private const val TAG = "MirroringService"
        private const val SESSION_IDLE_TIMEOUT_MS = 60_000L
        private const val WATCHDOG_INTERVAL_MS = 5_000L
        private const val NETWORK_SETTLE_MS = 1_500L
    }

    private val json = Json { ignoreUnknownKeys = true }
    private val servers = mutableListOf<DiscoveryHttpServer>()
    private val serversMutex = Mutex()
    private val adbWifiManager by lazy { AdbWifiManager(this) }
    private val clientStore by lazy { PairedClientStore(this) }
    private val pairingMutex = Mutex()
    private val isScreenOn = AtomicBoolean(true)
    private var receiverRegistered = false

    // Session tracking, so the watchdog can tell when the PC stopped calling in.
    private val sessionActive = AtomicBoolean(false)
    private val lastSeenElapsedMs = AtomicLong(0L)
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    private var discoveryRestartJob: Job? = null

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
        
        val filter = android.content.IntentFilter().apply {
            addAction(android.content.Intent.ACTION_SCREEN_OFF)
            addAction(android.content.Intent.ACTION_SCREEN_ON)
        }
        registerReceiver(screenStateReceiver, filter)
        receiverRegistered = true

        startSessionWatchdog()
        registerNetworkCallback()
    }

    private fun registerNetworkCallback() {
        val connectivityManager = getSystemService(android.content.Context.CONNECTIVITY_SERVICE) as ConnectivityManager

        // An empty request would deliver callbacks for EVERY transport — mobile data,
        // VPN, Bluetooth. Only the local WLAN matters here, because that is the only
        // one the PC can reach us on. Filtering also keeps the callback rate down,
        // which matters for battery.
        val request = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .build()

        networkCallback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                Log.i(TAG, "WLAN available, scheduling discovery restart")
                scheduleDiscoveryRestart()
            }

            override fun onLost(network: Network) {
                Log.i(TAG, "WLAN lost, scheduling discovery restart")
                scheduleDiscoveryRestart()
            }
        }

        connectivityManager.registerNetworkCallback(request, networkCallback!!)
    }

    // A WLAN hiccup fires several callbacks in a row. Restarting the servers on each
    // one tore the listener down exactly while the PC was trying to connect, so wait
    // for the callbacks to settle first.
    private fun scheduleDiscoveryRestart() {
        if (!serviceScope.isActive) return
        discoveryRestartJob?.cancel()
        discoveryRestartJob = serviceScope.launch {
            delay(NETWORK_SETTLE_MS)
            startDiscoveryServer()
        }
    }

    private fun startSessionWatchdog() {
        serviceScope.launch {
            // If the service crashed or the device rebooted mid-session, adopt the
            // stored session so the watchdog still times it out and disables ADB.
            if (clientStore.isSessionActive()) {
                sessionActive.set(true)
                lastSeenElapsedMs.set(SystemClock.elapsedRealtime())
            }
            while (isActive) {
                delay(WATCHDOG_INTERVAL_MS)
                if (sessionActive.get() &&
                    SystemClock.elapsedRealtime() - lastSeenElapsedMs.get() > SESSION_IDLE_TIMEOUT_MS
                ) {
                    Log.i(TAG, "Session idle for over ${SESSION_IDLE_TIMEOUT_MS}ms, closing the ADB door.")
                    endSession()
                }
            }
        }
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
        serviceScope.launch {
            serversMutex.withLock {
                if (!isActive) return@withLock
                servers.forEach { it.close() }
                servers.clear()

                val ips = NetworkScanner.getAllLocalIps(this@MirroringService)
                if (ips.isEmpty()) {
                    Log.w(TAG, "No local IPs found to bind discovery server.")
                    try {
                        val fallbackServer = DiscoveryHttpServer(host = InetAddress.getByName("127.0.0.1"), port = 18294, requestHandler = ::handleRequest)
                        fallbackServer.start()
                        servers.add(fallbackServer)
                        Log.i(TAG, "Discovery server started on fallback 127.0.0.1:18294")
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed to start fallback discovery server", e)
                    }
                    return@withLock
                }

                for (ip in ips) {
                    var retryCount = 0
                    while (retryCount < 5 && isActive) {
                        try {
                            val server = DiscoveryHttpServer(host = InetAddress.getByName(ip), port = 18294, requestHandler = ::handleRequest)
                            server.start()
                            servers.add(server)
                            Log.i(TAG, "Discovery server started on $ip:18294")
                            break
                        } catch (e: Exception) {
                            retryCount++
                            Log.e(TAG, "Failed to start discovery server on $ip:18294. Retry $retryCount/5", e)
                            if (retryCount >= 5) {
                                Log.e(TAG, "Given up starting discovery server on $ip:18294")
                            } else {
                                delay(2000)
                            }
                        }
                    }
                }
            }
        }
    }

    private suspend fun authorizeClient(clientId: String, clientName: String): Boolean {
        return pairingMutex.withLock {
            val alreadyAuthorized = clientStore.isClientPaired(clientId)
            if (alreadyAuthorized && clientStore.getPairedClient() == null) {
                // Trust on first use: the first client to connect becomes the paired one.
                clientStore.savePairedClient(clientId, clientName)
            }
            alreadyAuthorized
        }
    }

    private fun refreshSession() {
        lastSeenElapsedMs.set(SystemClock.elapsedRealtime())
        if (sessionActive.compareAndSet(false, true)) {
            runBlocking { clientStore.setSessionActive(true) }
            updateNotification("PC verbunden – ADB aktiviert")
            Log.i(TAG, "PC session started.")
        }
    }

    private fun endSession() {
        // The PC stopped calling in — close the ADB attack surface again.
        adbWifiManager.disableAdbTcpIp()
        adbWifiManager.disableAdbWifi()
        adbWifiManager.setAdbEnabled(false)
        sessionActive.set(false)
        runBlocking { clientStore.setSessionActive(false) }
        updateNotification("ADB deaktiviert – Bereit für Verbindung")
        Log.i(TAG, "PC session ended, ADB disabled.")
    }

    private fun updateNotification(text: String) {
        val notificationManager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.notify(NOTIFICATION_ID, NotificationHelper.createNotification(this, text))
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
                    authorizeClient(connectRequest.clientId, connectRequest.clientName)
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
                    val ok = adbWifiManager.enableAdbWifi() &&
                        adbWifiManager.enableAdbTcpIp(adbPort)
                    val dynamicPort = adbWifiManager.getDynamicAdbPort()
                    if (dynamicPort != -1) {
                        adbPort = dynamicPort
                    }
                    ok
                }

                if (success) {
                    refreshSession()
                }

                val response = ConnectResponse(
                    success = success,
                    ips = NetworkScanner.getAllLocalIps(this),
                    adbPort = adbPort,
                    deviceName = Build.MODEL
                )
                jsonResponse(response)
            }

            request.method == "POST" && request.path == "/heartbeat" -> {
                val heartbeatRequest = json.decodeFromString<ConnectRequest>(request.body)

                val isAuthorized = runBlocking {
                    authorizeClient(heartbeatRequest.clientId, heartbeatRequest.clientName)
                }

                if (!isAuthorized) {
                    HttpResponse(
                        statusCode = 403,
                        contentType = "text/plain; charset=utf-8",
                        body = ""
                    )
                } else {
                    refreshSession()
                    HttpResponse(
                        statusCode = 200,
                        contentType = "application/json; charset=utf-8",
                        body = "{\"ok\":true}"
                    )
                }
            }

            request.path == "/ping" || request.path == "/status" || request.path == "/connect" ||
                request.path == "/screen" || request.path == "/heartbeat" -> {
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
        serviceScope.cancel()
        if (sessionActive.get()) {
            endSession()
        }
        if (receiverRegistered) {
            unregisterReceiver(screenStateReceiver)
            receiverRegistered = false
        }
        networkCallback?.let {
            val connectivityManager = getSystemService(android.content.Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            try {
                connectivityManager.unregisterNetworkCallback(it)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to unregister network callback", e)
            }
        }
        // serviceScope.cancel() was already called above, so startDiscoveryServer coroutines will stop.
        // We can close the servers immediately to unblock any pending accept() calls.
        servers.toList().forEach {
            try {
                it.close()
            } catch (e: Exception) {
                Log.w(TAG, "Error closing discovery server", e)
            }
        }
        servers.clear()
        super.onDestroy()
    }
}
