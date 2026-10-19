package org.megavideoprotect.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.io.File
import java.security.KeyStore
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import org.json.JSONArray
import org.json.JSONObject

/** How one local item maps to the server's versioned record. */
data class SyncV4Mapping(
    val recordId: String,
    val localId: Long,
    val path: String,
    val signature: String,
    val version: Int,
    val packageName: String?,
)

/**
 * Everything the protocol 4 client must remember between launches: who is
 * signed in, which vault on the server it is, where the record cursor points,
 * and the local-id ↔ record mapping that makes rename/move/tag changes
 * analysable without re-hashing the vault.
 *
 * Sealed with a non-exportable per-install Android Keystore key, like the v3
 * ledger, because it holds a device token and the vault's package secret.
 */
class SyncV4StateStore(context: Context, vaultRoot: String) {
    private val namespace = run {
        val root = File(vaultRoot).canonicalFile
        sha256(root.path.toByteArray(Charsets.UTF_8))
    }
    private val prefs = context.getSharedPreferences("sync_v4", Context.MODE_PRIVATE)
    private val alias = "mvp-sync-v4"

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(
                KeyGenParameterSpec.Builder(
                    alias,
                    KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                )
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setRandomizedEncryptionRequired(true)
                    .build(),
            )
        }.generateKey()
    }

    fun load(): SyncV4Link? {
        val sealed = prefs.getString(namespace, null) ?: return null
        val bytes = Base64.decode(sealed, Base64.NO_WRAP)
        if (bytes.size < 28) return null
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(0, 12)))
        cipher.updateAAD(namespace.toByteArray(Charsets.UTF_8))
        val plain = String(cipher.doFinal(bytes.copyOfRange(12, bytes.size)), Charsets.UTF_8)
        return runCatching { SyncV4Link.fromJson(JSONObject(plain)) }.getOrNull()
    }

    fun save(link: SyncV4Link) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        cipher.updateAAD(namespace.toByteArray(Charsets.UTF_8))
        val sealed = cipher.iv + cipher.doFinal(link.toJson().toString().toByteArray(Charsets.UTF_8))
        prefs.edit().putString(namespace, Base64.encodeToString(sealed, Base64.NO_WRAP)).apply()
    }

    fun clear() {
        prefs.edit().remove(namespace).apply()
    }
}

/** The signed-in link to one server vault. */
data class SyncV4Link(
    val server: String,
    val email: String,
    val token: String,
    val vaultId: String,
    val vaultName: String,
    val packageSecret: String,
    val cursor: Long,
    val mappings: List<SyncV4Mapping>,
) {
    fun toJson(): JSONObject {
        val records = JSONArray()
        mappings.forEach { item ->
            records.put(
                JSONObject()
                    .put("record", item.recordId)
                    .put("local", item.localId)
                    .put("path", item.path)
                    .put("signature", item.signature)
                    .put("version", item.version)
                    .put("package", item.packageName ?: JSONObject.NULL),
            )
        }
        return JSONObject()
            .put("server", server)
            .put("email", email)
            .put("token", token)
            .put("vault", vaultId)
            .put("vaultName", vaultName)
            .put("packageSecret", packageSecret)
            .put("cursor", cursor)
            .put("records", records)
    }

    companion object {
        fun fromJson(value: JSONObject): SyncV4Link? {
            val server = value.optString("server")
            val token = value.optString("token")
            val vault = value.optString("vault")
            if (server.isEmpty() || token.isEmpty() || vault.isEmpty()) return null
            val listed = ArrayList<SyncV4Mapping>()
            val array = value.optJSONArray("records") ?: JSONArray()
            for (index in 0 until array.length()) {
                val entry = array.optJSONObject(index) ?: continue
                listed.add(
                    SyncV4Mapping(
                        recordId = entry.optString("record"),
                        localId = entry.optLong("local"),
                        path = entry.optString("path"),
                        signature = entry.optString("signature"),
                        version = entry.optInt("version"),
                        packageName = entry.optString("package").takeIf { it.isNotEmpty() },
                    ),
                )
            }
            return SyncV4Link(
                server = server,
                email = value.optString("email"),
                token = token,
                vaultId = vault,
                vaultName = value.optString("vaultName"),
                packageSecret = value.optString("packageSecret"),
                cursor = value.optLong("cursor"),
                mappings = listed,
            )
        }
    }
}

/**
 * What the app shows about one local file: the same fingerprint the v3 engine
 * used — content identity, not mtime — plus the folder path the server stores.
 */
data class SyncV4LocalFile(val id: Long, val name: String, val path: String, val signature: String)

object SyncV4Local {
    /** Folder chain as "/"-joined segments, matching what the desktop writes. */
    fun folderPath(id: Long, tree: Map<Long, Pair<Long, String>>): String {
        var cursor = id
        val seen = HashSet<Long>()
        val names = ArrayList<String>()
        while (cursor != 0L) {
            if (!seen.add(cursor) || seen.size > 64) break
            val folder = tree[cursor] ?: break
            names.add(folder.second)
            cursor = folder.first
        }
        return names.asReversed().joinToString("/")
    }

    fun files(): List<SyncV4LocalFile> {
        val folders = Json.folders(CoreBridge.nativeListFolders())
        val tree = folders.associate { it.id to (it.parentId to it.name) }
        return Json.videos(CoreBridge.nativeListVideos()).map { file ->
            val folder = folderPath(file.folderId, tree)
            val path = if (folder.isEmpty()) file.name else "$folder/${file.name}"
            val signature = sha256(
                JSONArray().put(file.id).put(file.size).put(file.importedAt)
                    .put(path).put(JSONArray(file.tags.sorted()))
                    .toString().toByteArray(Charsets.UTF_8),
            )
            SyncV4LocalFile(file.id, file.name, path, signature)
        }
    }

    /** A record id the server accepts: 32 lowercase hex characters. */
    fun newRecordId(): String = sha256(
        (System.nanoTime().toString() + Math.random()).toByteArray(Charsets.UTF_8),
    ).substring(0, 32)

    fun hashFile(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
    }
}
