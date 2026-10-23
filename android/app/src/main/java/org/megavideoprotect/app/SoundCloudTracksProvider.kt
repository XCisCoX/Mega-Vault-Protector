package org.megavideoprotect.app

import org.json.JSONObject
import java.net.URI
import java.net.URLEncoder

/** No web-page scraping, embedded API identity, OAuth bypass or restricted-track fallback. */
class SoundCloudTracksProvider : PluginProviderAdapter {
    override fun cataloguePath(query: String?, item: PluginItem?): String = when (item?.kind) {
        "artist" -> "/users/${id(item.id)}/tracks?limit=100"
        "album" -> "/playlists/${id(item.id)}"
        else -> {
            val q = query.orEmpty().trim().take(2048)
            if (q.startsWith("https://")) {
                val uri = URI(q)
                require(uri.host in setOf("soundcloud.com", "www.soundcloud.com", "m.soundcloud.com") && uri.userInfo == null) { "Enter a SoundCloud URL" }
                "/resolve?url=${encode(q)}"
            } else "/search?q=${encode(q.take(240))}&limit=50"
        }
    }
    override fun audioPath(item: PluginItem) = "/tracks/${id(item.id)}"
    override fun normalize(payload: JSONObject, detail: String?): List<PluginItem> {
        val collection = payload.optJSONArray("collection") ?: payload.optJSONArray("tracks")
        val objects = if (collection == null) listOf(payload) else (0 until minOf(500, collection.length())).mapNotNull { collection.optJSONObject(it) }
        return objects.mapNotNull { wrapped ->
            val o = wrapped.optJSONObject("track") ?: wrapped
            val raw = o.opt("id")
            if (raw !is String && raw !is Long && raw !is Int) return@mapNotNull null
            val key = raw.toString().takeIf { Regex("[0-9]{1,20}").matches(it) } ?: return@mapNotNull null
            val kind = when (o.optString("kind")) { "user" -> "artist"; "playlist" -> "album"; else -> "track" }
            if (kind == "track" && !o.has("title")) return@mapNotNull null
            val user = o.optJSONObject("user")
            PluginItem(kind, key, o.optString(if (kind == "artist") "username" else "title", "Untitled").take(300),
                user?.optString("username").orEmpty().take(300), o.optString("artwork_url", user?.optString("avatar_url").orEmpty()).take(2048),
                kind != "track" || playbackDenial(o).isEmpty())
        }.distinctBy { it.kind + it.id }
    }
    override fun resolve(http: PluginHttp, item: PluginItem, clientId: String, download: Boolean): ProviderStream {
        val o = http.jsonUrl(withClientId(http.absolute(audioPath(item)), clientId))
        val denial = if (download) downloadDenial(o) else playbackDenial(o)
        require(denial.isEmpty()) { denial }
        val canonical = o.optString("permalink_url").takeIf { runCatching { URI(it).host in setOf("soundcloud.com", "www.soundcloud.com") }.getOrDefault(false) }.orEmpty().substringBefore('?')
        if (download) {
            // The original-file endpoint enforces artist permission server-side too.
            val resolved = http.jsonUrl(withClientId(http.absolute("/tracks/${id(item.id)}/download"), clientId))
            val url = resolved.optString("redirectUri", resolved.optString("url"))
            require(url.isNotBlank()) { "SoundCloud did not authorize a download URL" }
            val codec = o.optString("original_format", "mp3").lowercase()
            require(codec in setOf("mp3", "flac", "wav", "m4a", "aac", "ogg", "opus")) { "This authorized original-file format is not supported by the audio player." }
            return ProviderStream(url, codec, "", true, canonical)
        }
        val a = o.optJSONObject("media")?.optJSONArray("transcodings") ?: error("No supported SoundCloud stream")
        val entries = (0 until a.length()).mapNotNull { a.optJSONObject(it) }
        val chosen = entries.firstOrNull { it.optJSONObject("format")?.optString("protocol") == "progressive" }
            ?: entries.firstOrNull { it.optJSONObject("format")?.optString("protocol") == "hls" }
            ?: error("No supported SoundCloud stream")
        val format = chosen.getJSONObject("format")
        var path = withClientId(chosen.getString("url"), clientId)
        val authorization = o.optString("track_authorization")
        if (authorization.isNotEmpty()) path += "&track_authorization=${encode(authorization)}"
        val url = http.jsonUrl(path).getString("url")
        val mime = format.optString("mime_type")
        val codec = when { "mpeg" in mime -> "mp3"; "mp4" in mime -> "m4a"; else -> "ogg" }
        return ProviderStream(url, codec, mime, downloadDenial(o).isEmpty(), canonical, format.optString("protocol") == "hls")
    }
    companion object {
        private fun id(value: String): String = value.also { require(Regex("[0-9]{1,20}").matches(it)) }
        private fun encode(value: String) = URLEncoder.encode(value, "UTF-8")
        fun withClientId(url: String, key: String): String {
            require(key.isNotBlank() && key.length <= 256 && key.none { it < ' ' }) { "Enter your own SoundCloud API client ID in this provider's session settings. No identity is bundled or scraped." }
            val uri = URI(url)
            // Never attach API identity to artwork/CDN URLs.
            require(uri.host in setOf("api-v2.soundcloud.com", "api.soundcloud.com")) { "Unapproved API identity destination" }
            if (uri.rawQuery.orEmpty().split('&').any { it.substringBefore('=') == "client_id" }) return url
            return url + (if (uri.rawQuery == null) "?" else "&") + "client_id=${encode(key)}"
        }
        fun playbackDenial(o: JSONObject): String = when {
            o.optString("policy").uppercase() == "BLOCK" || o.optString("access").lowercase() == "block" -> "SoundCloud blocked this track."
            o.optBoolean("geo_blocked", false) -> "SoundCloud does not allow this track in your region."
            o.optString("policy").uppercase() in setOf("SNIP", "PREVIEW") || o.optString("access").lowercase().let { it == "preview" || it.startsWith("snip") } -> "Preview-only tracks cannot be played or saved as full tracks."
            !o.optBoolean("streamable", false) -> "SoundCloud does not authorize streaming this track."
            else -> ""
        }
        fun downloadDenial(o: JSONObject): String = playbackDenial(o).ifEmpty {
            if (!o.optBoolean("downloadable", false)) "The artist has not authorized downloads; streaming only." else ""
        }
    }
}
