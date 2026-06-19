package dev.pixelmirroring.app.network

import android.content.Context
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkStatic
import io.mockk.unmockkStatic
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import java.net.NetworkInterface
import java.net.SocketException

class NetworkScannerTest {

    @Before
    fun setUp() {
        mockkStatic(NetworkInterface::class)
    }

    @After
    fun tearDown() {
        unmockkStatic(NetworkInterface::class)
    }

    @Test
    fun `getAllLocalIps returns empty list when NetworkInterface throws exception`() {
        val context = mockk<Context>()
        every { NetworkInterface.getNetworkInterfaces() } throws SocketException("Mock exception")

        val ips = NetworkScanner.getAllLocalIps(context)

        assertEquals(0, ips.size)
    }
}
