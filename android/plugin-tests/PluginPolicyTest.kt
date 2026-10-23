package org.megavideoprotect.app
import org.json.JSONObject
import java.io.File

private fun rejects(block: () -> Unit) { check(runCatching(block).isFailure) }
fun main(args: Array<String>) {
    val raw = File(args[0]).readBytes()
    val manifest = PluginManifest.parse(raw)
    check(manifest.id == "org.megavault.monochrome")
    check(PluginProviderRegistry.adapter(manifest.protocol).cataloguePath("A B",null) == "/search?q=A+B")
    fun changed(block: (JSONObject) -> Unit) = JSONObject(raw.toString(Charsets.UTF_8)).also(block).toString().toByteArray()
    rejects { PluginManifest.parse(changed { it.put("schemaVersion", 2) }) }
    rejects { PluginManifest.parse(changed { it.put("schemaVersion", 1.5) }) }
    rejects { PluginManifest.parse(changed { it.getJSONObject("network").put("baseUrl", "https://tracks.monochrome.st?secret=x") }) }
    rejects { PluginManifest.parse(changed { it.getJSONObject("network").put("baseUrl", "https://user:pass@tracks.monochrome.st") }) }
    rejects { PluginManifest.parse(changed { it.getJSONObject("network").put("allowedHosts", listOf("127.0.0.1")) }) }
    rejects { PluginManifest.parse(changed { it.getJSONObject("network").put("allowedHosts", listOf("localhost")) }) }
    rejects { PluginManifest.parse(changed { it.put("script", "evil.js") }) }
    rejects { PluginManifest.parse(changed { it.put("id", "../../evil") }) }
    rejects { PluginManifest.parse(changed { it.put("version", "latest") }) }
    rejects { PluginManifest.parse(changed { it.put("platforms", listOf("linux")) }) }
    rejects { PluginManifest.parse(changed { it.put("protocol", "javascript") }) }
    rejects { PluginManifest.parse(changed { it.put("permissions", listOf("shell")) }) }
    rejects { PluginManifest.parse(changed { it.getJSONObject("network").put("baseUrl", "http://tracks.monochrome.st") }) }
    rejects { PluginManifest.parse(ByteArray(65537)) }
    rejects { manifest.checkUrl("https://evil.example/audio") }
    rejects { manifest.checkUrl("https://tracks.monochrome.st@evil.example/audio") }
    rejects { manifest.checkUrl("https://tracks.monochrome.st:8443/audio") }
    check(PluginOptions().autoSave == false)
    check(PluginOptions().memoryMiB == 32)
    rejects { PluginOptions(memoryMiB = 1024) }
    check(PluginOptions.key("a", "/vault/A") != PluginOptions.key("a", "/vault/B"))
    val nested = JSONObject("""{"id":"168671849784291328","title":"Fixture","artists":[{"name":"Composer","artistId":"155479365088448512"}],"duration":120000}""")
    check(PluginCatalog.track(nested)!!.subtitle == "Composer")
    check(PluginCatalog.track(nested)!!.id == "168671849784291328")
    check(PluginCatalog.track(JSONObject("""{"id":"168671849784291328","title":"Fixture","artistNames":["Singer"]}"""))!!.subtitle == "Singer")
    check(PluginAudio.isFlac("application/octet-stream", "fLaCfixture".toByteArray()))
    check(!PluginAudio.isFlac("application/json", "fLaCfixture".toByteArray()))
    check(!PluginAudio.isFlac("audio/flac", "<html>".toByteArray()))
    val actions=JSONObject().put("schemaVersion",1).put("id","org.megavault.fixture-tools").put("name","Fixture tools").put("version","1.0.0")
        .put("description","Fixture sandbox commands").put("protocol","megavault-actions-v1").put("platforms",listOf("android"))
        .put("permissions",emptyList<String>()).put("commands",listOf(mapOf("id" to "statistics","title" to "Statistics","inputLabel" to "Text")))
        .put("script","globalThis.extension={commands:{statistics(input){return {title:'Stats',text:input.text}}}};")
    actions.put("settings",listOf(mapOf("id" to "remove-empty-lines","title" to "Remove empty lines","type" to "boolean","default" to true)))
    val actionManifest=PluginManifest.parse(actions.toString().toByteArray())
    check(actionManifest.settings.single().defaultValue)
    rejects { PluginManifest.parse(JSONObject(actions.toString()).put("settings",listOf(mapOf("id" to "../bad","title" to "Bad","type" to "boolean","default" to false))).toString().toByteArray()) }
    rejects { PluginManifest.parse(JSONObject(actions.toString()).put("settings",listOf(mapOf("id" to "bad","title" to "Bad","type" to "string","default" to false))).toString().toByteArray()) }
    check(PluginOptions.key("a@1.0.0","/vault/A")!=PluginOptions.key("a@2.0.0","/vault/A"))
    check(actionManifest.commands.single().id=="statistics")
    check(!PluginProviderRegistry.protocol(actionManifest.protocol).music)
    rejects { PluginManifest.parse(JSONObject(actions.toString()).put("script","x".repeat(262145)).toString().toByteArray()) }
    rejects { PluginManifest.parse(JSONObject(actions.toString()).put("permissions",listOf("network")).toString().toByteArray()) }
    rejects { PluginManifest.parse(JSONObject(actions.toString()).put("commands",emptyList<String>()).toString().toByteArray()) }
    httpTest(args)
    println("PluginPolicyTest: validation, scoped defaults, nested artists and FLAC MIME passed")
}
