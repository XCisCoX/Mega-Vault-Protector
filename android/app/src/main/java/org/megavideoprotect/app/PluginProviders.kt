package org.megavideoprotect.app

import org.json.JSONObject
import java.net.URLEncoder

/** Host-approved adapters. Installing a manifest never registers native code or classes. */
interface PluginProviderAdapter {
    fun cataloguePath(query: String?, item: PluginItem?): String
    fun normalize(payload: JSONObject, detail: String?): List<PluginItem>
    fun audioPath(item: PluginItem): String
}
data class PluginProtocol(val id: String, val permissions: Set<String>, val music: Boolean)
object PluginProviderRegistry {
    private val definitions = mapOf(
        "monochrome-tracks-v1" to PluginProtocol("monochrome-tracks-v1",setOf("network","audio-playback","vault-import"),true),
        "megavault-actions-v1" to PluginProtocol("megavault-actions-v1",emptySet(),false),
    )
    fun protocol(id: String): PluginProtocol = definitions[id] ?: error("Unsupported extension protocol")
    fun adapter(id: String): PluginProviderAdapter = when (id) {
        "monochrome-tracks-v1" -> MonochromeTracksProvider()
        else -> error("This extension is not a music provider")
    }
}
object BundledPluginRegistry {
    val assets = listOf("monochrome/plugin.json", "text-tools/plugin.json")
}
class MonochromeTracksProvider : PluginProviderAdapter {
    override fun cataloguePath(query: String?, item: PluginItem?): String = when(item?.kind) {
        "artist" -> "/artists/${item.id}"
        "album" -> "/releases/${item.id}"
        else -> "/search?q=" + URLEncoder.encode(query.orEmpty().take(240),"UTF-8")
    }
    override fun normalize(payload: JSONObject, detail: String?) = PluginCatalog.rows(payload,detail)
    override fun audioPath(item: PluginItem) = "/track/${item.id}"
}
