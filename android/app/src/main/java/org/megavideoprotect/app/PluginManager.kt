package org.megavideoprotect.app

import android.content.Context
import android.net.Uri
import java.io.File

/** JSON descriptors only: native music adapters or bounded sandbox source inline, never dex/libraries/ZIPs. */
class PluginManager(private val context: Context, private val vaultRoot: String) {
    private val prefs = context.getSharedPreferences("portable_plugins", Context.MODE_PRIVATE)
    private val directory = File(context.noBackupFilesDir, "plugins").apply { mkdirs() }
    private val bundled by lazy {
        BundledPluginRegistry.assets.map { path -> context.assets.open(path).use { PluginManifest.parse(it.readBytes()) } }
    }
    fun isBundled(manifest: PluginManifest): Boolean = bundled.any { it.id == manifest.id }
    fun manifests(): List<PluginManifest> {
        val installed = directory.listFiles().orEmpty().filter { it.extension == "json" && it.length() <= 512*1024 }
            .mapNotNull { runCatching { PluginManifest.parse(it.readBytes()) }.getOrNull() }
        return bundled + installed.filter { item -> bundled.none { it.id == item.id } }.distinctBy { it.id }
    }
    fun install(uri: Uri): PluginManifest {
        val bytes = context.contentResolver.openInputStream(uri)?.use { input ->
            val buffer = ByteArray(512*1024+1)
            var count = 0
            while (count < buffer.size) {
                val n = input.read(buffer, count, buffer.size - count)
                if (n < 0) break
                count += n
            }
            require(count <= 512*1024) { "Manifest exceeds 512 KiB" }
            buffer.copyOf(count)
        } ?: error("Cannot read the selected manifest")
        val manifest = PluginManifest.parse(bytes)
        require(manifests().none { it.id == manifest.id }) { "This plugin ID is already installed (including bundled plugins)" }
        // The validated ID is a filename, never a path supplied by the document provider.
        val file = File(directory, "${manifest.id}.json")
        file.outputStream().use { it.write(bytes) }
        require(PluginManifest.parse(file.readBytes()) == manifest) { "Manifest verification failed" }
        return manifest
    }
    private fun key(manifest: PluginManifest) = PluginOptions.key(manifest.id + "@" + manifest.version, vaultRoot)
    fun options(manifest: PluginManifest): PluginOptions {
        val k = key(manifest)
        return PluginOptions(
            enabled = prefs.getBoolean("$k.enabled", false),
            autoSave = prefs.getBoolean("$k.autoSave", false),
            folderId = prefs.getLong("$k.folder", 0).coerceAtLeast(0),
            memoryMiB = prefs.getInt("$k.memory", 32).takeIf { it in listOf(32,64,128) } ?: 32,
        )
    }
    fun save(manifest: PluginManifest, value: PluginOptions) {
        val k = key(manifest)
        check(prefs.edit().putBoolean("$k.enabled", value.enabled)
            .putBoolean("$k.autoSave", value.autoSave).putLong("$k.folder", value.folderId)
            .putInt("$k.memory", value.memoryMiB).commit()) { "Cannot save plugin settings" }
    }
    fun booleanSettings(manifest: PluginManifest): Map<String,Boolean> {
        val k=key(manifest)
        return manifest.settings.associate { it.id to prefs.getBoolean("$k.setting.${it.id}",it.defaultValue) }
    }
    fun saveBooleanSettings(manifest: PluginManifest, values: Map<String,Boolean>) {
        require(values.keys == manifest.settings.map {it.id}.toSet()) { "Unrecognized extension setting" }
        val k=key(manifest);val edit=prefs.edit()
        values.forEach { (id,value) -> edit.putBoolean("$k.setting.$id",value) }
        check(edit.commit()) { "Cannot save extension settings" }
    }
    fun remove(manifest: PluginManifest) {
        require(!isBundled(manifest)) { "Bundled extensions cannot be removed" }
        check(File(directory, "${manifest.id}.json").delete()) { "Cannot remove plugin" }
        save(manifest, PluginOptions())
    }
}
