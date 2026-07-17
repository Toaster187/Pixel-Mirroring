package dev.pixelmirroring.app.data

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

private val Context.dataStore by preferencesDataStore(name = "paired_client")

data class PairedClient(val id: String, val name: String)

class PairedClientStore(private val context: Context) {

    companion object {
        val CLIENT_ID_KEY = stringPreferencesKey("client_id")
        val CLIENT_NAME_KEY = stringPreferencesKey("client_name")
        val SESSION_ACTIVE_KEY = booleanPreferencesKey("session_active")
    }

    suspend fun savePairedClient(clientId: String, clientName: String) {
        context.dataStore.edit { preferences ->
            preferences[CLIENT_ID_KEY] = clientId
            preferences[CLIENT_NAME_KEY] = clientName
        }
    }

    suspend fun getPairedClient(): PairedClient? {
        val preferences = context.dataStore.data.first()
        val id = preferences[CLIENT_ID_KEY]
        val name = preferences[CLIENT_NAME_KEY]
        return if (id != null && name != null) PairedClient(id, name) else null
    }

    suspend fun removePairedClient() {
        context.dataStore.edit { preferences ->
            preferences.remove(CLIENT_ID_KEY)
            preferences.remove(CLIENT_NAME_KEY)
        }
    }

    suspend fun isClientPaired(clientId: String): Boolean {
        // Fürs erste lassen wir jede Verbindung zu, solange noch kein Client gekoppelt ist.
        // Wenn einer gekoppelt ist, muss die ID übereinstimmen.
        val paired = getPairedClient()
        return paired == null || paired.id == clientId
    }

    suspend fun setSessionActive(active: Boolean) {
        context.dataStore.edit { preferences ->
            preferences[SESSION_ACTIVE_KEY] = active
        }
    }

    suspend fun isSessionActive(): Boolean {
        val preferences = context.dataStore.data.first()
        return preferences[SESSION_ACTIVE_KEY] ?: false
    }
}
