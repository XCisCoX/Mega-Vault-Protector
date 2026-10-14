package org.megavideoprotect.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import org.json.JSONObject

/** Per-vault, private encrypted preferences; pending encrypted MVPK lives in no-backup storage. */
class AndroidSyncStore(context: Context, vaultRoot: String) : SyncStateStore {
    private val namespace = run {
        val root = File(vaultRoot).canonicalFile
        val ready = File(root, "vault.ready")
        check(ready.isFile && ready.length() == 24L) { "Local vault identity unavailable" }
        SyncPolicy.vaultNamespace(root.path, ready.readBytes())
    }
    private val prefs = context.getSharedPreferences("sync_v2", Context.MODE_PRIVATE)
    private val alias = "mvp-sync-v2-$namespace"
    override val directory = File(context.noBackupFilesDir, "sync-v2/$namespace").apply { check(isDirectory || mkdirs()) }
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        check(!prefs.contains(namespace)) { "Sync Keystore key missing; old state cannot be decrypted. Do not reset pending work." }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true).build())
        }.generateKey()
    }
    override fun load(): JSONObject {
        val sealed = prefs.getString(namespace, null) ?: return JSONObject()
        val bytes = Base64.decode(sealed, Base64.NO_WRAP)
        require(bytes.size >= 28)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(0, 12)))
        cipher.updateAAD(namespace.toByteArray(Charsets.UTF_8))
        return JSONObject(String(cipher.doFinal(bytes.copyOfRange(12, bytes.size)), Charsets.UTF_8))
    }
    override fun save(state: JSONObject) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        cipher.updateAAD(namespace.toByteArray(Charsets.UTF_8))
        val bytes = cipher.iv + cipher.doFinal(state.toString().toByteArray(Charsets.UTF_8))
        check(prefs.edit().putString(namespace, Base64.encodeToString(bytes, Base64.NO_WRAP)).commit()) { "Cannot durably save sync state" }
    }
}

class NativeSyncVault : SyncVault {
    override fun isUnlocked() = !RemoteVault.connected() && CoreBridge.nativeIsUnlocked()
    override fun files(): List<SyncLocalFile> {
        check(isUnlocked()) { "Unlock a local vault" }
        val folders = Json.folders(CoreBridge.nativeListFolders())
        val tree = folders.associate { it.id to (it.parentId to it.name) }
        return Json.videos(CoreBridge.nativeListVideos()).map { file ->
            val folder = SyncPolicy.folderPath(file.folderId, tree)
            val path = if (folder.isEmpty()) file.name else "$folder/${file.name}"
            // Core media bytes are immutable for a local ID. Rename, move and tags do not change that ID.
            // Package hash is separately persisted with this complete local metadata fingerprint.
            val signature = sha256(org.json.JSONArray().put(file.id).put(file.size).put(file.importedAt)
                .put(path).put(org.json.JSONArray(file.tags.sorted())).toString().toByteArray(Charsets.UTF_8))
            SyncLocalFile(file.id, path, signature)
        }
    }
    override fun export(file: SyncLocalFile, target: File, key: String) {
        check(isUnlocked())
        val path = file.path.split('/').joinToString("/") { packageSegment(it) }
        require(path.toByteArray(Charsets.UTF_8).size <= 1024) { "Package path too long" }
        val directories = path.split('/').dropLast(1).runningFold("") { parent, name -> if (parent.isEmpty()) name else "$parent/$name" }.drop(1)
        val result = CoreBridge.Result.parse(CoreBridge.nativeExportPackage(target.absolutePath, key, longArrayOf(file.id), arrayOf(path), directories.toTypedArray(), false))
        check(result.ok) { result.error.ifBlank { result.detail } }
    }
    override fun import(source: File, key: String): Int {
        check(isUnlocked())
        // JNI currently returns count only, never an imported ID. Engine reconciles before/after IDs,
        // and asks for explicit mapping on dedup. Native import can transiently decrypt locally;
        // the core owns its temporary-directory policy, not this encrypted sync cache.
        val raw = CoreBridge.nativeImportPackage(source.absolutePath, key, 0L)
        val result = CoreBridge.Result.parse(raw)
        check(result.ok) { result.error.ifBlank { result.detail } }
        return JSONObject(raw).getInt("count")
    }
}
