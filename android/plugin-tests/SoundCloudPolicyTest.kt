package org.megavideoprotect.app

import org.json.JSONObject
import java.io.File

private fun rejects(block: () -> Unit) { check(runCatching(block).isFailure) }
fun main(args: Array<String>) {
    val m=PluginManifest.parse(File(args[0]).readBytes())
    check(m.protocol=="soundcloud-tracks-v1" && PluginProviderRegistry.protocol(m.protocol).music)
    check(m.checkUrl("https://cf-media.sndcdn.com/fixture").host=="cf-media.sndcdn.com")
    for(url in listOf("https://sndcdn.com/a", "https://nested.cf-media.sndcdn.com/a", "https://evil-sndcdn.com/a", "http://cf-media.sndcdn.com/a", "https://cf-media.sndcdn.com:8443/a", "https://api-v2.soundcloud.com@evil.example/a")) rejects {m.checkUrl(url)}
    val provider=SoundCloudTracksProvider()
    check(provider.cataloguePath("A & B",null).contains("A+%26+B"))
    check(provider.cataloguePath(null,PluginItem("artist","42","Artist"))=="/users/42/tracks?limit=100")
    check(provider.cataloguePath(null,PluginItem("album","42","Playlist"))=="/playlists/42")
    check(provider.cataloguePath("https://soundcloud.com/artist/track",null).startsWith("/resolve?url="))
    rejects {provider.cataloguePath("https://evil.example/track",null)}
    val track=JSONObject("""{"kind":"track","id":123456789012345678,"title":"Fixture","streamable":true,"downloadable":false,"user":{"username":"Artist"}}""")
    check(provider.normalize(track,null).single().id=="123456789012345678")
    check(SoundCloudTracksProvider.playbackDenial(track).isEmpty())
    check(SoundCloudTracksProvider.downloadDenial(track).isNotEmpty())
    track.put("downloadable",true); check(SoundCloudTracksProvider.downloadDenial(track).isEmpty())
    for(policy in listOf("SNIP","PREVIEW","BLOCK")) {
        track.put("policy",policy)
        check(SoundCloudTracksProvider.downloadDenial(track).isNotEmpty())
        check(SoundCloudTracksProvider.playbackDenial(track).isNotEmpty())
    }
    track.remove("policy"); track.put("access","preview"); check(SoundCloudTracksProvider.downloadDenial(track).isNotEmpty())
    track.remove("access"); track.put("geo_blocked",true); check(SoundCloudTracksProvider.downloadDenial(track).isNotEmpty())
    track.put("geo_blocked",false); track.put("streamable",false); check(SoundCloudTracksProvider.playbackDenial(track).isNotEmpty())
    check(provider.normalize(JSONObject("""{"tracks":[{"track":{"id":"123","title":"Nested","streamable":true}},{}]}"""),"album").single().id=="123")
    check(provider.normalize(JSONObject("""{"id":1.2345678e18,"title":"Lossy ID"}"""),null).isEmpty())
    rejects {SoundCloudTracksProvider.withClientId("https://api-v2.soundcloud.com/search", "")}
    rejects {SoundCloudTracksProvider.withClientId("https://cf-media.sndcdn.com/audio", "fixture-client")}
    val keyed=SoundCloudTracksProvider.withClientId("https://api-v2.soundcloud.com/search?q=a", "fixture-client")
    check(keyed.endsWith("&client_id=fixture-client"))
    check(SoundCloudTracksProvider.withClientId(keyed,"fixture-client")==keyed)
    val client=PluginHttp(m)
    rejects {client.jsonUrl("https://127.0.0.1/a")}
    client.close(); rejects {client.stream("https://api-v2.soundcloud.com/track")}
    println("PASS production SoundCloud provider: bundled manifest; wildcard/URL boundary; stable IDs; track/user/playlist browse; preview/geo/stream/download rights; explicit identity gate and API-only attachment; cancellation")
}
