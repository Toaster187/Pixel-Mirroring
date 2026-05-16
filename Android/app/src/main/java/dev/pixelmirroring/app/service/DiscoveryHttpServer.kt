package dev.pixelmirroring.app.service

import android.util.Log
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.Closeable
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class DiscoveryHttpServer(
    private val port: Int,
    private val requestHandler: (HttpRequest) -> HttpResponse
) : Closeable {

    companion object {
        private const val TAG = "DiscoveryHttpServer"
        private const val SOCKET_BACKLOG = 8
        private const val SOCKET_TIMEOUT_MS = 5000
        private const val MAX_HEADER_BYTES = 16 * 1024
        private const val MAX_BODY_BYTES = 64 * 1024
    }

    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    private var workerPool: ExecutorService? = null
    @Volatile
    private var running = false

    fun start() {
        if (running) {
            return
        }

        val socket = ServerSocket()
        socket.reuseAddress = true
        socket.bind(InetSocketAddress(port), SOCKET_BACKLOG)
        serverSocket = socket

        workerPool = Executors.newCachedThreadPool()
        running = true

        acceptThread = Thread({
            // Ugg wait for tiny HTTP knocks.
            while (running) {
                try {
                    val client = socket.accept()
                    workerPool?.execute { handleClient(client) }
                } catch (_: SocketException) {
                    if (running) {
                        Log.w(TAG, "Socket went weird while waiting for request")
                    }
                    break
                } catch (e: Exception) {
                    if (running) {
                        Log.e(TAG, "Accept loop broke", e)
                    }
                }
            }
        }, "pm-discovery-accept").apply {
            isDaemon = true
            start()
        }
    }

    override fun close() {
        running = false
        serverSocket?.close()
        acceptThread?.join(1000)
        workerPool?.shutdownNow()
        workerPool?.awaitTermination(1, TimeUnit.SECONDS)
        acceptThread = null
        workerPool = null
        serverSocket = null
    }

    private fun handleClient(socket: Socket) {
        socket.use { client ->
            client.soTimeout = SOCKET_TIMEOUT_MS

            try {
                val input = BufferedInputStream(client.getInputStream())
                val output = BufferedOutputStream(client.getOutputStream())
                val request = parseRequest(input)
                val response = if (request != null) {
                    requestHandler(request)
                } else {
                    HttpResponse(
                        statusCode = 400,
                        contentType = "text/plain; charset=utf-8",
                        body = "bad request"
                    )
                }

                writeResponse(output, response)
                output.flush()
            } catch (e: Exception) {
                Log.w(TAG, "Request handling failed", e)
            }
        }
    }

    private fun parseRequest(input: InputStream): HttpRequest? {
        val headerBytes = ByteArrayOutput()
        val marker = byteArrayOf('\r'.code.toByte(), '\n'.code.toByte(), '\r'.code.toByte(), '\n'.code.toByte())

        while (headerBytes.size < MAX_HEADER_BYTES) {
            val next = input.read()
            if (next == -1) {
                return null
            }

            headerBytes.write(next)
            if (headerBytes.endsWith(marker)) {
                break
            }
        }

        if (!headerBytes.endsWith(marker)) {
            return null
        }

        val headerText = headerBytes.toByteArray().toString(StandardCharsets.UTF_8)
        val lines = headerText.split("\r\n")
        if (lines.isEmpty()) {
            return null
        }

        val requestLineParts = lines.first().split(" ")
        if (requestLineParts.size < 2) {
            return null
        }

        val headers = linkedMapOf<String, String>()
        for (line in lines.drop(1)) {
            if (line.isEmpty()) {
                break
            }

            val colonIndex = line.indexOf(':')
            if (colonIndex <= 0) {
                continue
            }

            val name = line.substring(0, colonIndex).trim().lowercase(Locale.US)
            val value = line.substring(colonIndex + 1).trim()
            headers[name] = value
        }

        val contentLength = headers["content-length"]?.toIntOrNull()?.coerceAtLeast(0) ?: 0
        if (contentLength > MAX_BODY_BYTES) {
            return null
        }

        val bodyBytes = ByteArray(contentLength)
        var offset = 0
        while (offset < contentLength) {
            val read = input.read(bodyBytes, offset, contentLength - offset)
            if (read == -1) {
                return null
            }
            offset += read
        }

        return HttpRequest(
            method = requestLineParts[0].uppercase(Locale.US),
            path = requestLineParts[1],
            headers = headers,
            body = bodyBytes.toString(StandardCharsets.UTF_8)
        )
    }

    private fun writeResponse(output: BufferedOutputStream, response: HttpResponse) {
        val bodyBytes = response.body.toByteArray(StandardCharsets.UTF_8)
        val statusText = response.statusText.ifBlank { defaultStatusText(response.statusCode) }
        val header = buildString {
            append("HTTP/1.1 ${response.statusCode} $statusText\r\n")
            append("Content-Type: ${response.contentType}\r\n")
            append("Content-Length: ${bodyBytes.size}\r\n")
            append("Connection: close\r\n")
            append("\r\n")
        }

        output.write(header.toByteArray(StandardCharsets.UTF_8))
        output.write(bodyBytes)
    }

    private fun defaultStatusText(statusCode: Int): String = when (statusCode) {
        200 -> "OK"
        400 -> "Bad Request"
        403 -> "Forbidden"
        404 -> "Not Found"
        405 -> "Method Not Allowed"
        500 -> "Internal Server Error"
        else -> "OK"
    }

    private class ByteArrayOutput {
        private val buffer = ArrayList<Byte>(256)

        val size: Int
            get() = buffer.size

        fun write(value: Int) {
            buffer.add(value.toByte())
        }

        fun toByteArray(): ByteArray = buffer.toByteArray()

        fun endsWith(suffix: ByteArray): Boolean {
            if (buffer.size < suffix.size) {
                return false
            }

            val start = buffer.size - suffix.size
            for (i in suffix.indices) {
                if (buffer[start + i] != suffix[i]) {
                    return false
                }
            }

            return true
        }
    }
}

data class HttpRequest(
    val method: String,
    val path: String,
    val headers: Map<String, String>,
    val body: String
)

data class HttpResponse(
    val statusCode: Int,
    val contentType: String,
    val body: String,
    val statusText: String = ""
)
