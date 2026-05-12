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
import io.ktor.http.HttpStatusCode
import io.ktor.serialization.kotlinx.json.json
import io.ktor.server.application.call
import io.ktor.server.application.install
import io.ktor.server.engine.ApplicationEngine
import io.ktor.server.engine.embeddedServer
import io.ktor.server.netty.Netty
import io.ktor.server.plugins.contentnegotiation.ContentNegotiation
import io.ktor.server.request.receive
import io.ktor.server.response.respond
import io.ktor.server.response.respondText
import io.ktor.server.routing.get
import io.ktor.server.routing.post
import io.ktor.server.routing.routing
import kotlinx.coroutines.runBlocking

class MirroringService : Service() {
    companion object {
        private const val NOTIFICATION_ID = 18294
        private const val TAG = "MirroringService"
    }

    private var server: ApplicationEngine? = null
    private val adbWifiManager by lazy { AdbWifiManager(this) }
    private val clientStore by lazy { PairedClientStore(this) }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i(TAG, "Starting MirroringService")
        startForeground(NOTIFICATION_ID, NotificationHelper.createNotification(this))
        startKtorServer()
        return START_STICKY
    }

    private fun startKtorServer() {
        if (server != null) return

        server = embeddedServer(Netty, port = 18294) {
            install(ContentNegotiation) {
                json()
            }
            routing {
                get("/ping") {
                    call.respondText("pong")
                }
                
                post("/connect") {
                    val request = call.receive<ConnectRequest>()
                    
                    val isAuthorized = clientStore.isClientPaired(request.clientId)
                    
                    if (!isAuthorized) {
                        call.respond(HttpStatusCode.Forbidden)
                        return@post
                    }
                    
                    // Auto-pair if no one is paired yet
                    if (clientStore.getPairedClient() == null) {
                        clientStore.savePairedClient(request.clientId, request.clientName)
                    }

                    // Activate ADB WiFi
                    val adbPort = (5555..5595).random()
                    val success = adbWifiManager.enableAdbWifi() && adbWifiManager.enableAdbTcpIp(adbPort)
                    
                    val ips = NetworkScanner.getAllLocalIps(this@MirroringService)
                    
                    call.respond(ConnectResponse(
                        success = success,
                        ips = ips,
                        adbPort = adbPort,
                        deviceName = Build.MODEL
                    ))
                }
                
                get("/status") {
                    call.respond(StatusResponse(
                        adbWifiEnabled = adbWifiManager.isAdbWifiEnabled(),
                        hasPermission = adbWifiManager.hasSecureSettingsPermission(),
                        deviceName = Build.MODEL
                    ))
                }
            }
        }
        
        try {
            server?.start(wait = false)
            Log.i(TAG, "Ktor Server started on port 18294")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start Ktor server on port 18294. Port might be in use.", e)
            // Cleanup server instance
            server = null
        }
    }

    override fun onDestroy() {
        server?.stop(1000, 2000)
        super.onDestroy()
    }
}