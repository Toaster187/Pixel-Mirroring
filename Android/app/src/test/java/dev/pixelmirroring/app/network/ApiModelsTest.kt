package dev.pixelmirroring.app.network

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Test

class ApiModelsTest {

    @Test
    fun testConnectRequestSerialization() {
        val original = ConnectRequest(
            clientId = "test-client-id-123",
            clientName = "DESKTOP-TEST"
        )

        val jsonString = Json.encodeToString(original)
        val decoded = Json.decodeFromString<ConnectRequest>(jsonString)

        assertEquals(original, decoded)
        assertEquals("test-client-id-123", decoded.clientId)
        assertEquals("DESKTOP-TEST", decoded.clientName)
    }

    @Test
    fun testConnectResponseSerialization() {
        val original = ConnectResponse(
            success = true,
            ips = listOf("192.168.1.100", "10.0.0.5"),
            adbPort = 5555,
            deviceName = "Pixel 9 Pro"
        )

        val jsonString = Json.encodeToString(original)
        val decoded = Json.decodeFromString<ConnectResponse>(jsonString)

        assertEquals(original, decoded)
        assertEquals(true, decoded.success)
        assertEquals(listOf("192.168.1.100", "10.0.0.5"), decoded.ips)
        assertEquals(5555, decoded.adbPort)
        assertEquals("Pixel 9 Pro", decoded.deviceName)
    }

    @Test
    fun testStatusResponseSerialization() {
        val original = StatusResponse(
            adbWifiEnabled = true,
            hasPermission = false,
            deviceName = "Pixel 9 Pro"
        )

        val jsonString = Json.encodeToString(original)
        val decoded = Json.decodeFromString<StatusResponse>(jsonString)

        assertEquals(original, decoded)
        assertEquals(true, decoded.adbWifiEnabled)
        assertEquals(false, decoded.hasPermission)
        assertEquals("Pixel 9 Pro", decoded.deviceName)
    }
}
