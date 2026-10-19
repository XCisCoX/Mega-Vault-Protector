package org.megavideoprotect.app
import java.io.File

/** Optional LIVE network probe. Audio stays in RAM and is erased, never written. */
fun main(args: Array<String>) {
    val m=PluginManifest.parse(File(args[0]).readBytes())
    val h=PluginHttp(m)
    try {
        val search=h.json("/search?q=Ludwig%20van%20Beethoven")
        val rows=PluginCatalog.rows(search)
        check(rows.any {it.kind=="track"} && rows.any {it.kind=="artist"})
        println("LIVE search: ${rows.size} normalized catalogue rows")
        val release=h.json("/releases/166472736192020480")
        check(PluginCatalog.rows(release,"album").any{it.kind=="track"})
        val artist=h.json("/artists/155479365088448512")
        check(PluginCatalog.rows(artist,"artist").isNotEmpty())
        println("LIVE release and artist detail: normalized successfully")
        rows.firstOrNull {it.artwork.isNotBlank()}?.let { item ->
            val image=h.image(item.artwork)
            try { check(image.isNotEmpty());println("LIVE approved-host artwork: ${image.size} bytes") } finally {image.fill(0)}
        }
        val audio=h.audio("/track/282968934603341824",32*1024*1024)
        try {check(PluginAudio.isFlac("application/octet-stream",audio));println("LIVE audio: ${audio.size} bytes verified FLAC signature in RAM; no device decoding claimed")}
        finally {audio.fill(0)}
    } finally {h.close()}
}
