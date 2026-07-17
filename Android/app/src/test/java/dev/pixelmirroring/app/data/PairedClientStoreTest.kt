package dev.pixelmirroring.app.data

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PairedClientStoreTest {

    private lateinit var context: Context
    private lateinit var store: PairedClientStore

    @Before
    fun setup() {
        context = ApplicationProvider.getApplicationContext()
        store = PairedClientStore(context)
    }

    @After
    fun teardown() = runTest {
        store.removePairedClient()
    }

    @Test
    fun isClientPaired_whenNoClientPaired_returnsTrueForAnyId() = runTest {
        // Assert: When no client is paired, any ID is accepted
        assertTrue(store.isClientPaired("any_id_1"))
        assertTrue(store.isClientPaired("any_id_2"))
    }

    @Test
    fun isClientPaired_whenClientPaired_returnsTrueOnlyForMatchingId() = runTest {
        // Arrange
        val pairedId = "test_client_id"
        val clientName = "Test Client"
        store.savePairedClient(pairedId, clientName)

        // Act & Assert
        assertTrue(store.isClientPaired(pairedId))
        assertFalse(store.isClientPaired("different_id"))
    }

    @Test
    fun saveAndGetPairedClient_worksCorrectly() = runTest {
        // Arrange
        val id = "client_123"
        val name = "My PC"

        // Act
        store.savePairedClient(id, name)
        val client = store.getPairedClient()

        // Assert
        assertEquals(id, client?.id)
        assertEquals(name, client?.name)
    }

    @Test
    fun removePairedClient_clearsData() = runTest {
        // Arrange
        store.savePairedClient("id", "name")

        // Act
        store.removePairedClient()
        val client = store.getPairedClient()

        // Assert
        assertNull(client)
    }
}
