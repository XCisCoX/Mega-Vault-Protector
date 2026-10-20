package org.megavideoprotect.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class ProxySnapshot(val enabled: Boolean = false, val selected: String? = null, val profiles: List<ProxyProfile> = emptyList())

/** One encrypted, atomic no-backup file. No plaintext proxy credentials enter preferences or logs. */
private class ProxyStore(context: Context) {
    private val file = AtomicFile(File(context.noBackupFilesDir, "proxy-settings.aesgcm"))
    private val alias = "mvp.proxy.settings.v1"
    private val aad = "MegaVaultProtect/proxy-settings/v1".toByteArray(Charsets.UTF_8)
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true).build())
        }.generateKey()
    }
    fun load(): ProxySnapshot {
        if (!file.baseFile.exists()) return ProxySnapshot()
        val bytes = file.readFully()
        require(bytes.size >= 29 && bytes[0] == 1.toByte()) { "Invalid proxy settings" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(1, 13))); cipher.updateAAD(aad)
        val plain = cipher.doFinal(bytes, 13, bytes.size - 13)
        try {
            val json = JSONObject(plain.toString(Charsets.UTF_8)); val list = json.getJSONArray("profiles")
            val profiles = (0 until list.length()).map { i -> list.getJSONObject(i).let {
                ProxyProfile(it.getString("id"), ProxyKind.valueOf(it.getString("kind")), it.getString("host"), it.getInt("port"), it.getString("username"), it.getString("password"))
            } }
            require(profiles.map { it.id }.distinct().size == profiles.size)
            return ProxySnapshot(json.getBoolean("enabled"), json.optString("selected").takeIf { it.isNotEmpty() }, profiles)
        } finally { plain.fill(0) }
    }
    fun save(value: ProxySnapshot) {
        val list = JSONArray()
        value.profiles.forEach { list.put(JSONObject().put("id", it.id).put("kind", it.kind.name).put("host", it.host)
            .put("port", it.port).put("username", it.username).put("password", it.password)) }
        val plain = JSONObject().put("enabled", value.enabled).put("selected", value.selected ?: "").put("profiles", list).toString().toByteArray(Charsets.UTF_8)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key()); cipher.updateAAD(aad)
        val encrypted = try { cipher.doFinal(plain) } finally { plain.fill(0) }
        require(cipher.iv.size == 12)
        val out = file.startWrite()
        try { out.write(byteArrayOf(1)); out.write(cipher.iv); out.write(encrypted); file.finishWrite(out) }
        catch (t: Throwable) { file.failWrite(out); throw t }
    }
}

/** A settings failure blocks network access; it never turns a requested proxy into a direct route. */
object AppProxy {
    @Volatile private var initialized = false
    @Volatile private var blocked = true
    @Volatile private var current = ProxySnapshot()
    private var store: ProxyStore? = null
    val settingsUnavailable: Boolean get() = blocked
    val snapshot: ProxySnapshot get() = current
    @Synchronized fun initialize(context: Context) {
        if (initialized) return
        store = ProxyStore(context.applicationContext)
        try { current = store!!.load(); blocked = false } catch (_: Exception) { blocked = true }
        initialized = true
    }
    @Synchronized fun save(value: ProxySnapshot) {
        check(initialized && !blocked) { "Proxy settings are unavailable. Network access is blocked." }
        require(value.profiles.map { it.id }.distinct().size == value.profiles.size)
        require(value.selected == null || value.profiles.any { it.id == value.selected })
        store!!.save(value) // Publish only after durable encryption succeeded.
        current = value
    }
    fun open(url: URL): HttpURLConnection {
        if (!initialized || blocked) throw IOException("Proxy settings unavailable; network access blocked")
        val value = current
        val route = if (value.enabled) value.profiles.firstOrNull { it.id == value.selected }
            ?: throw IOException("Use proxy is enabled; select or add a proxy") else null
        return ProxyTransport.open(url, route)
    }
}
