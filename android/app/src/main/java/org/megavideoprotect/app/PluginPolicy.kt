package org.megavideoprotect.app

import org.json.JSONArray
import org.json.JSONObject
import java.net.URI
import java.security.MessageDigest
import java.io.File

private fun JSONArray.strings(): List<String> = (0 until length()).map { getString(it) }

data class PluginCommand(val id: String, val title: String, val inputLabel: String)
data class ExtensionBooleanSetting(val id: String, val title: String, val defaultValue: Boolean)
data class PluginManifest(val id: String, val name: String, val version: String, val description: String,
    val baseUrl: String, val allowedHosts: Set<String>, val sourceNotice: String,
    val protocol: String = "", val commands: List<PluginCommand> = emptyList(), val script: String = "",
    val settings: List<ExtensionBooleanSetting> = emptyList()) {
    fun allowsHost(host: String): Boolean = allowedHosts.any { approved ->
        if (!approved.startsWith("*.")) host == approved else {
            val suffix = approved.substring(1)
            host.endsWith(suffix) && host.removeSuffix(suffix).let { it.isNotEmpty() && '.' !in it }
        }
    }
    fun checkUrl(value: String): URI {
        val uri = URI(value)
        require(uri.scheme == "https" && uri.host != null && allowsHost(uri.host) && uri.rawUserInfo == null &&
            uri.port in listOf(-1, 443) && uri.fragment == null) { "Only HTTPS URLs on approved plugin hosts are allowed" }
        return uri
    }
    companion object {
        fun parse(bytes: ByteArray): PluginManifest {
            require(bytes.size in 1..(512*1024)) { "Manifest must be at most 512 KiB" }
            val o = JSONObject(bytes.toString(Charsets.UTF_8))
            require(o.get("schemaVersion") is Int && o.getInt("schemaVersion") == 1) { "Unsupported manifest schema" }
            val id = o.getString("id")
            require(Regex("[a-z][a-z0-9]*(?:\\.[a-z][a-z0-9-]*){1,7}").matches(id) && id.length <= 120) { "Invalid plugin ID" }
            val version = o.getString("version")
            require(Regex("[0-9]+\\.[0-9]+\\.[0-9]+(?:-[A-Za-z0-9.-]+)?").matches(version) && version.length <= 64) { "Invalid plugin version" }
            val platforms = o.getJSONArray("platforms").strings()
            require("android" in platforms && platforms.all { it in setOf("android","windows","linux") }) { "Plugin does not support Android" }
            val protocol = o.getString("protocol")
            val definition = PluginProviderRegistry.protocol(protocol)
            val permissions = o.getJSONArray("permissions").strings()
            require(permissions.size == permissions.distinct().size && permissions.toSet() == definition.permissions) { "Unsupported extension permissions" }
            require(listOf("entrypoint","code","library","executable").none { o.has(it) }) { "External code and native libraries are unsupported" }
            fun field(key: String, max: Int): String = o.getString(key).also {
                require(it.isNotBlank() && it.length <= max && it.none { c -> c < ' ' }) { "Invalid $key" }
            }
            val baseUrl: String
            val hosts: List<String>
            val commands: List<PluginCommand>
            val script: String
            if (definition.music) {
                require(!o.has("script") && !o.has("commands")) { "Music providers are data-only descriptors" }
                val network = o.getJSONObject("network")
                hosts = network.getJSONArray("allowedHosts").strings()
                require(hosts.isNotEmpty() && hosts.size <= 16 && hosts.distinct().size == hosts.size)
                require(hosts.all { it.length <= 253 && Regex("[a-z0-9]+(?:[.-][a-z0-9]+)*\\.[a-z]{2,}").matches(it.removePrefix("*.")) }) { "Invalid approved host" }
                baseUrl = network.getString("baseUrl")
                commands = emptyList(); script = ""
            } else {
                require(!o.has("network")) { "Sandbox actions have no network access" }
                baseUrl = ""; hosts = emptyList()
                script = o.getString("script")
                require(script.isNotBlank() && script.toByteArray(Charsets.UTF_8).size <= 256*1024) { "Sandbox script must be at most 256 KiB" }
                val a = o.getJSONArray("commands")
                require(a.length() in 1..32) { "An action extension needs 1–32 commands" }
                commands = (0 until a.length()).map { i ->
                    val c=a.getJSONObject(i); val cid=c.getString("id")
                    require(Regex("[a-z][a-z0-9_-]{0,63}").matches(cid)) { "Invalid command ID" }
                    val title=c.getString("title"); val label=c.getString("inputLabel")
                    require(title.isNotBlank() && title.length<=120 && label.isNotBlank() && label.length<=120)
                    PluginCommand(cid,title,label)
                }
                require(commands.map { it.id }.distinct().size==commands.size) { "Duplicate command ID" }
            }
            val notice = if (definition.music) field("sourceNotice",2000) else o.optString("sourceNotice", "Runs locally in an isolated sandbox without network or vault access.").take(2000)
            val rawSettings = o.optJSONArray("settings") ?: JSONArray().also { require(!o.has("settings")) { "Settings must be an array" } }
            require(rawSettings.length() <= 16) { "At most 16 extension settings are supported" }
            val settings = (0 until rawSettings.length()).map { i ->
                val setting=rawSettings.getJSONObject(i); val sid=setting.getString("id");val title=setting.getString("title")
                require(sid.length<=64 && Regex("[a-z][a-z0-9]*(?:-[a-z0-9]+)*").matches(sid)) { "Invalid setting ID" }
                require(title.isNotBlank() && title.length<=80 && title.none { it<' ' }) { "Invalid setting title" }
                require(setting.getString("type")=="boolean" && setting.get("default") is Boolean) { "Only boolean extension settings are supported" }
                ExtensionBooleanSetting(sid,title,setting.getBoolean("default"))
            }
            require(settings.map {it.id}.distinct().size==settings.size) { "Duplicate extension setting ID" }
            val result = PluginManifest(id,field("name",100),version,field("description",2000),baseUrl,hosts.toSet(),notice,protocol,commands,script,settings)
            if (definition.music) {
                val base = result.checkUrl(result.baseUrl)
                require(base.host in result.allowedHosts && base.rawQuery == null && base.rawPath in listOf("", "/")) { "Provider base URL must be an origin" }
            }
            return result
        }
    }
}

data class PluginOptions(val enabled: Boolean = false, val autoSave: Boolean = false, val folderId: Long = 0, val memoryMiB: Int = 32) {
    init { require(memoryMiB in listOf(32,64,128) && folderId >= 0) }
    companion object {
        fun key(plugin: String, vault: String): String {
            val canonical = File(vault).canonicalPath
            return MessageDigest.getInstance("SHA-256").digest((plugin + "\u0000" + canonical).toByteArray())
                .joinToString("") { "%02x".format(it) }
        }
    }
}

data class PluginItem(val kind: String, val id: String, val title: String, val subtitle: String = "", val artwork: String = "", val playable: Boolean = true)
object PluginCatalog {
    private fun id(o: JSONObject, key: String): String? {
        val value = if (o.has(key)) o.opt(key) else o.opt("id")
        // A JSON double has already lost precision. Never stringify or repair it.
        if (value !is String && value !is Long && value !is Int) return null
        return value.toString().takeIf { Regex("[0-9]{17,20}").matches(it) }
    }
    private fun artists(o: JSONObject): String {
        val names = o.optJSONArray("artistNames")
        if (names != null && names.length() > 0) return (0 until names.length()).map { names.optString(it) }.filter { it.isNotBlank() }.joinToString(", ").take(300)
        val nested = o.optJSONArray("artists") ?: return ""
        return (0 until nested.length()).mapNotNull { nested.optJSONObject(it) }
            .map { it.optString("name", it.optString("displayName")) }.joinToString(", ").take(300)
    }
    fun track(o: JSONObject): PluginItem? = id(o,"trackId")?.let { PluginItem("track",it,o.optString("title","Unknown title").take(300),artists(o),o.optString("artwork",o.optString("cover")).take(2048),o.optBoolean("playable",true)) }
    private fun album(o: JSONObject): PluginItem? = id(o,"releaseId")?.let { PluginItem("album",it,o.optString("title","Unknown album").take(300),artists(o),o.optString("artwork",o.optString("cover")).take(2048)) }
    private fun artist(o: JSONObject): PluginItem? = id(o,"artistId")?.let { PluginItem("artist",it,o.optString("name",o.optString("displayName","Unknown artist")).take(300),artwork=o.optString("avatar").take(2048)) }
    fun rows(o: JSONObject, detail: String? = null): List<PluginItem> {
        val result = ArrayList<PluginItem>()
        fun add(key: String, parse: (JSONObject) -> PluginItem?) {
            val a = o.optJSONArray(key) ?: return
            for (i in 0 until minOf(a.length(),500)) a.optJSONObject(i)?.let(parse)?.let { result.add(it) }
        }
        when (detail) {
            "album" -> add("tracks", ::track)
            "artist" -> {
                add("topTracks", ::track)
                if (result.isEmpty()) {
                    for (key in listOf("albums","singles","compilations")) {
                        val a = o.optJSONArray(key) ?: continue
                        for (i in 0 until minOf(a.length(),100)) {
                            val t = a.optJSONObject(i)?.optJSONArray("tracks") ?: continue
                            for (j in 0 until minOf(t.length(),20)) t.optJSONObject(j)?.let(::track)?.let { result.add(it) }
                        }
                    }
                }
                add("albums", ::album); add("singles", ::album); add("compilations", ::album)
            }
            else -> { add("tracks",::track); add("releases",::album); add("artists",::artist) }
        }
        return result.distinctBy { it.kind + it.id }.take(500)
    }
}
object PluginAudio {
    fun isFlac(mime: String, bytes: ByteArray): Boolean =
        mime.substringBefore(';').trim().lowercase() in setOf("audio/flac","audio/x-flac","application/octet-stream") &&
            bytes.size >= 4 && bytes[0] == 102.toByte() && bytes[1] == 76.toByte() && bytes[2] == 97.toByte() && bytes[3] == 67.toByte()
    fun fileName(title: String, artist: String): String = ("$artist - $title".replace(Regex("[\\p{Cntrl}<>:\"/\\\\|?*]"), "_").trim(' ','.').take(110).ifBlank { "Music" }) + ".flac"
}
