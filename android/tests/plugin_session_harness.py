"""Execute real PluginSession.kt with JVM ports for Android/ExoPlayer/JNI.

The ports are explicitly fake: this tests session orchestration, not decoding,
Android rendering, HTTP FLAC validation, or native encryption. Requires the
portable Kotlin compiler jars + JDK used by android-proxy-tools. All generated
classes and downloaded coroutine-test dependency stay in Hermes scratch.
"""
import argparse
import os
from pathlib import Path
import subprocess
import urllib.request

STUBS = {
'Context.kt': '''package android.content
open class Context { val filesDir = java.io.File("vault"); fun startService(i: Intent) {} }
class Intent(c: Context, k: Class<*>) { fun setAction(s: String) = this }
''',
'Uri.kt': '''package android.net
class Uri { companion object { fun parse(s: String) = Uri() } }
''',
'Bitmap.kt': '''package android.graphics
class Bitmap { fun eraseColor(c: Int) {} }
object Color { const val TRANSPARENT = 0 }
''',
'Compose.kt': '''package androidx.compose.runtime
import kotlin.reflect.KProperty
class State<T>(var value: T)
fun <T> mutableStateOf(v: T) = State(v)
operator fun <T> State<T>.getValue(o: Any?, p: KProperty<*>) = value
operator fun <T> State<T>.setValue(o: Any?, p: KProperty<*>, v: T) { value = v }
''',
'UnstableApi.kt': '''package androidx.media3.common.util
@RequiresOptIn annotation class UnstableApi
''',
'Media.kt': '''package androidx.media3.common
interface Player {
 companion object { const val REPEAT_MODE_OFF=0; const val REPEAT_MODE_ONE=1; const val REPEAT_MODE_ALL=2; const val STATE_ENDED=4 }
 interface Listener {
  fun onIsPlayingChanged(v: Boolean) {}
  fun onPlayerError(e: PlaybackException) {}
  fun onPlaybackStateChanged(s: Int) {}
 }
}
class PlaybackException: Exception()
object C { const val USAGE_MEDIA=1; const val AUDIO_CONTENT_TYPE_MUSIC=2 }
class AudioAttributes { class Builder { fun setUsage(v: Int)=this; fun setContentType(v: Int)=this; fun build()=AudioAttributes() } }
class MediaMetadata { class Builder { fun setTitle(s: String)=this; fun setArtist(s: String)=this; fun build()=MediaMetadata() } }
class MediaItem(val id: String) {
 class Builder { var id=""; fun setMediaId(s: String)=apply {id=s}; fun setUri(v: android.net.Uri)=this; fun setMimeType(s: String)=this; fun setMediaMetadata(m: MediaMetadata)=this; fun build()=MediaItem(id) }
}
''',
'DataSource.kt': '''package androidx.media3.datasource
interface DataSource { fun interface Factory { fun createDataSource(): DataSource } }
class ByteArrayDataSource(val bytes: ByteArray): DataSource
''',
'Source.kt': '''package androidx.media3.exoplayer.source
class ProgressiveMediaSource(val item: androidx.media3.common.MediaItem) {
 class Factory(val factory: androidx.media3.datasource.DataSource.Factory) { fun createMediaSource(i: androidx.media3.common.MediaItem)=ProgressiveMediaSource(i) }
}
''',
'Exo.kt': '''package androidx.media3.exoplayer
import androidx.media3.common.*
class ExoPlayer {
 class Builder(c: android.content.Context) { fun build()=ExoPlayer() }
 val listeners=mutableListOf<Player.Listener>()
 var isPlaying=false; var repeatMode=0; var currentPosition=0L; var playbackState=1
 var playerError: PlaybackException? = null
 var currentMediaItem: MediaItem?=null
 fun setAudioAttributes(a: AudioAttributes, b: Boolean) {}
 fun setHandleAudioBecomingNoisy(v: Boolean) {}
 fun addListener(l: Player.Listener) {listeners+=l}
 fun stop() {isPlaying=false; playbackState=1; currentPosition=0; listeners.toList().forEach {it.onIsPlayingChanged(false)}}
 fun clearMediaItems() {currentMediaItem=null}
 fun release() {}
 fun setMediaSource(s: androidx.media3.exoplayer.source.ProgressiveMediaSource) {currentMediaItem=s.item}
 fun prepare() {playbackState=3; playerError=null}
 fun play() {isPlaying=true; listeners.toList().forEach {it.onIsPlayingChanged(true)}}
 fun pause() {isPlaying=false; listeners.toList().forEach {it.onIsPlayingChanged(false)}}
 fun seekTo(p: Long) {currentPosition=p; if(playbackState==4) playbackState=3}
 fun end() {isPlaying=false; playbackState=4; listeners.toList().forEach {it.onPlaybackStateChanged(4)}}
}
''',
'Ports.kt': '''package org.megavideoprotect.app
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
class ExtensionActionSession {fun close() {}}
data class PluginManifest(val protocol: String="fake", val name: String="Test provider")
data class PluginOptions(val enabled: Boolean=true, val memoryMiB: Int=32, val autoSave: Boolean=false, val folderId: Long=0)
data class PluginItem(val kind: String, val id: String, val title: String=id, val subtitle: String="", val playable: Boolean=true)
class PluginLimitException(message: String): Exception(message)
class Gate(val result: String, val fail: Boolean=false) { val started=CountDownLatch(1); val release=CountDownLatch(1); @Volatile var closed=false }
object Fixtures {
 val gates=ConcurrentHashMap<String,Gate>()
 val buffers=ConcurrentHashMap<String,ByteArray>()
 fun request(path: String): String { val gate=gates[path] ?: return path.substringAfterLast('/'); gate.started.countDown(); check(gate.release.await(30,TimeUnit.SECONDS)); if(gate.fail) throw PluginLimitException("stale error"); return gate.result }
}
class PluginHttp(m: PluginManifest) {
 var gate: Gate?=null
 fun json(path: String): String {gate=Fixtures.gates[path]; return Fixtures.request(path)}
 fun audio(path: String, cap: Int): ByteArray {gate=Fixtures.gates[path]; val result=Fixtures.request(path); return result.toByteArray().also {Fixtures.buffers[path]=it}}
 fun image(url: String)=byteArrayOf(1)
 fun close() {gate?.closed=true} // deliberately non-cooperative IO to exercise generations
}
class FakeProvider {
 fun cataloguePath(q: String?, i: PluginItem?)="catalogue/"+(i?.id ?: q)
 fun normalize(data: String, kind: String?)=data.split(',').map {PluginItem("track",it)}
 fun audioPath(i: PluginItem)="audio/"+i.id
}
object PluginProviderRegistry {fun adapter(id: String)=FakeProvider()}
object MusicHub {const val active=false}
class MusicPlaybackService {companion object {const val ACTION_STOP="stop"}}
object RemoteVault {fun connected()=false}
object VaultLocation {fun saved(c: android.content.Context): String?="vault"}
data class Folder(val id: Long)
object Json {fun folders(s: String)=emptyList<Folder>()}
object PluginAudio {fun fileName(a: String,b: String)=a+".flac"}
object GalleryCache {fun clear() {}}
object CoreBridge {
 var imports=0
 var importGate: Gate?=null
 fun nativeIsUnlocked()=true
 fun nativeListFolders()="[]"
 fun nativeImportBytesForVault(b: ByteArray,n: String,f: Long,r: String): String {importGate?.let {it.started.countDown(); check(it.release.await(30,TimeUnit.SECONDS))}; check(b.any {it.toInt()!=0}); imports++; return "ok"}
 class Result(val ok: Boolean=true,val error: String="") {companion object {fun parse(s: String)=Result()}}
}
''',
'Harness.kt': '''@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class, kotlinx.coroutines.DelicateCoroutinesApi::class)
package org.megavideoprotect.app
import kotlinx.coroutines.*
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.test.resetMain
import androidx.media3.common.Player
suspend fun until(label: String, test: () -> Boolean) { withTimeout(3000) {while(!test()) delay(5)} }
fun track(id: String)=PluginItem("track",id)
fun session(autoSave: Boolean=false)=PluginSession(android.content.Context(),PluginManifest(),"vault",PluginOptions(autoSave=autoSave))
suspend fun loaded(s: PluginSession) {until("audio") {!s.playbackBusy}; check(s.player.currentMediaItem != null); check(s.canDownload)}
fun checkThat(v: Boolean, label: String) {check(v) {label}}
fun main() {
 val main=newSingleThreadContext("session-main")
 kotlinx.coroutines.Dispatchers.setMain(main)
 try {runBlocking(main) {
  val a=track("a"); val b=track("b"); val c=track("c")
  val s=session()
  s.play(a,listOf(a,PluginItem("album","folder"),b,b,PluginItem("track","bad",playable=false),c))
  checkThat(s.selected==a,"selected set before IO"); loaded(s)
  checkThat(s.playableRows==listOf(a,b,c),"playable queue filters/deduplicates")
  val aBytes=Fixtures.buffers.getValue("audio/a")
  s.next(); loaded(s); checkThat(s.selected==b,"next provider track")
  checkThat(aBytes.all {it.toInt()==0},"replaced RAM erased")
  s.previous(); loaded(s); checkThat(s.selected==a,"previous provider track")
  s.previous(); checkThat(s.selected==a,"previous boundary no-op")
  s.setRepeat(Player.REPEAT_MODE_ALL); s.previous(); loaded(s); checkThat(s.selected==c,"repeat list wraps backwards")
  s.next(); loaded(s); checkThat(s.selected==a,"repeat list wraps forwards")
  s.setRepeat(Player.REPEAT_MODE_ONE); checkThat(s.player.repeatMode==Player.REPEAT_MODE_ONE,"native one-track repeat")
  s.setRepeat(Player.REPEAT_MODE_OFF); s.player.end(); loaded(s); checkThat(s.selected==b,"ended advances queue")
  s.next(); loaded(s); s.player.end(); checkThat(s.selected==c && !s.player.isPlaying,"off stops at end")
  s.toggle(); checkThat(s.player.isPlaying,"play restarts ended track")
  s.setShuffle(true); val visited=mutableSetOf(s.selected!!.id)
  repeat(2) {s.next(); loaded(s); checkThat(visited.add(s.selected!!.id),"shuffle does not repeat early")}
  checkThat(visited==setOf("a","b","c"),"shuffle covers full queue")
  s.reversePlaylist(); checkThat(s.playableRows==listOf(c,b,a),"reverse list")
  s.setShuffle(false)
  s.download(); until("save") {!s.saveBusy}; checkThat(s.saved && !s.canDownload && CoreBridge.imports==1,"download exact RAM to vault port")
  s.browse("ready"); until("catalogue") {!s.catalogueBusy}; checkThat(s.rows.single().id=="ready","catalogue result")
  checkThat(s.playableRows==listOf(c,b,a),"browsing retains playback snapshot")
  val old=Gate("old",fail=true); Fixtures.gates["catalogue/old"]=old
  s.browse("old"); checkThat(s.rows.isEmpty() && s.catalogueBusy,"browse immediately clears stale rows")
  until("old start") {old.started.count==0L}
  val fresh=Gate("fresh"); Fixtures.gates["catalogue/fresh"]=fresh
  s.browse("fresh"); until("fresh start") {fresh.started.count==0L}
  old.release.countDown(); delay(50)
  checkThat(s.catalogueBusy && s.catalogueMessage.isEmpty(),"stale error/finally cannot change newer request")
  fresh.release.countDown(); until("fresh done") {!s.catalogueBusy}
  checkThat(s.rows.single().id=="fresh","newer catalogue wins")
  val late=Gate("late"); Fixtures.gates["catalogue/timeout"]=late
  val slowAudio=Gate("slow"); Fixtures.gates["audio/slow"]=slowAudio
  s.browse("timeout"); s.play(track("slow"),listOf(track("slow")))
  until("requests") {late.started.count==0L && slowAudio.started.count==0L}
  delay(15_100)
  checkThat(!s.catalogueBusy && s.catalogueMessage.contains("15 seconds"),"15 second catalogue deadline")
  checkThat(late.closed && !slowAudio.closed && s.playbackBusy,"deadline cancels catalogue only")
  late.release.countDown(); slowAudio.release.countDown(); loaded(s)
  checkThat(s.rows.isEmpty() && s.catalogueMessage.contains("15 seconds"),"late timed-out response cannot populate rows")
  val staleAudio=Gate("stale"); Fixtures.gates["audio/stale"]=staleAudio
  s.play(track("stale")); until("stale audio") {staleAudio.started.count==0L}
  s.play(a,listOf(a,b)); loaded(s); staleAudio.release.countDown(); delay(50)
  checkThat(s.selected==a && s.player.currentMediaItem!!.id=="a" && !s.playbackBusy,"late audio cannot replace selected track")
  checkThat(Fixtures.buffers.getValue("audio/stale").all {it.toInt()==0},"cancelled loaded RAM erased")
  val import=Gate("import"); CoreBridge.importGate=import
  s.download(); until("import") {import.started.count==0L}
  s.next(); checkThat(s.selected==a,"import pins track bytes")
  val pinned=Fixtures.buffers.getValue("audio/a"); s.close()
  checkThat(pinned.any {it.toInt()!=0},"close retains bytes until running import returns")
  import.release.countDown(); until("import finished") {!s.saveBusy}
  checkThat(pinned.all {it.toInt()==0},"closed-session import finally erases pinned RAM")
  s.next(); s.previous(); s.toggle(); s.browse("closed"); checkThat(s.selected==null && !s.busy,"closed session actions are inert")
  checkThat(CoreBridge.imports==2,"only explicit imports")
  CoreBridge.importGate=null
  val auto=session(true); auto.play(a,listOf(a)); until("autosave") {!auto.saveBusy && auto.saved}
  checkThat(CoreBridge.imports==3,"autosave starts on isPlaying only")
  auto.close()
  val stoppable=session(); stoppable.play(a,listOf(a)); loaded(stoppable)
  val stoppedBytes=Fixtures.buffers.getValue("audio/a")
  stoppable.stopPlayback()
  checkThat(stoppable.selected==null && !stoppable.playbackBusy && stoppable.playableRows.isEmpty(),"stop clears playback only")
  checkThat(stoppedBytes.all {it.toInt()==0},"stop erases audio RAM")
  stoppable.browse("after-stop"); until("after-stop catalogue") {!stoppable.catalogueBusy}
  checkThat(stoppable.rows.single().id=="after-stop","stop retains working provider folder")
  stoppable.close()
  println("PASS PluginSession JVM orchestration: queue, boundaries, repeat, shuffle, reverse, RAM wipe, save pinning, independent browsing, stale responses, real 15s deadline, close, auto-save")
 }} finally {kotlinx.coroutines.Dispatchers.resetMain(); main.close()}
}
''',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    test_jar = args.scratch / 'kotlinx-coroutines-test-jvm-1.8.0.jar'
    if not test_jar.exists():
        urllib.request.urlretrieve('https://repo.maven.apache.org/maven2/org/jetbrains/kotlinx/kotlinx-coroutines-test-jvm/1.8.0/kotlinx-coroutines-test-jvm-1.8.0.jar', test_jar)
    sources = []
    for name, text in STUBS.items():
        path = args.scratch / name
        path.write_text(text, encoding='utf-8')
        sources.append(str(path))
    production = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app/PluginSession.kt'
    java = next(args.tools.glob('*/bin/java.exe'))
    compiler_cp = os.pathsep.join(str(p) for p in args.tools.glob('*.jar'))
    runtime_cp = os.pathsep.join(str(p) for p in [args.tools/'kotlin-stdlib.jar', args.tools/'coroutines.jar', args.tools/'annotations.jar', test_jar])
    out = args.scratch / 'classes'
    subprocess.run([str(java), '-cp', compiler_cp, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler', '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', runtime_cp, '-d', str(out), *sources, str(production)], check=True)
    subprocess.run([str(java), '-cp', str(out)+os.pathsep+runtime_cp, 'org.megavideoprotect.app.HarnessKt'], check=True, timeout=60)

if __name__ == '__main__':
    main()
