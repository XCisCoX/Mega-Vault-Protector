"""Real production Kotlin orchestration/source, fake Android/JNI/transport ports.

Does not claim device decoding, real HTTP ranges, or native encryption. The JNI
library is compiled/linked separately with Gradle externalNativeBuildDebug.
Run --scratch <Hermes scratch path> --java <jdk/bin/java.exe>.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import urllib.request


def compiler(cache):
    def jar(group, module, version):
        return next((cache / group / module / version).glob('*/*.jar'))
    deps = [jar('org.jetbrains.kotlin', n, '2.1.20') for n in
            ('kotlin-compiler-embeddable', 'kotlin-stdlib', 'kotlin-script-runtime', 'kotlin-daemon-embeddable')]
    deps += [jar('org.jetbrains.kotlin', 'kotlin-reflect', '2.0.21'),
             jar('org.jetbrains.intellij.deps', 'trove4j', '1.0.20200330'),
             jar('org.jetbrains.kotlinx', 'kotlinx-coroutines-core-jvm', '1.8.0'),
             jar('org.jetbrains', 'annotations', '23.0.0')]
    return deps


def run(java, jars, runtime, sources, output, main, args=()):
    subprocess.run([str(java), '-cp', os.pathsep.join(map(str, jars)), 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
                    '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', os.pathsep.join(map(str, runtime)),
                    '-d', str(output), *map(str, sources)], check=True)
    subprocess.run([str(java), '-cp', os.pathsep.join(map(str, [output, *runtime])), main, *map(str, args)], check=True, timeout=90)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--scratch', type=Path, required=True)
    p.add_argument('--java', type=Path, required=True)
    p.add_argument('--cache', type=Path, default=Path.home()/'.gradle/caches/modules-2/files-2.1')
    args = p.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    source = root/'app/src/main/java/org/megavideoprotect/app'
    jars = compiler(args.cache)
    runtime = [j for j in jars if any(x in j.name for x in ('kotlin-stdlib', 'coroutines-core', 'annotations'))]
    test = args.scratch/'kotlinx-coroutines-test-jvm-1.8.0.jar'
    if not test.exists():
        urllib.request.urlretrieve('https://repo.maven.apache.org/maven2/org/jetbrains/kotlinx/kotlinx-coroutines-test-jvm/1.8.0/kotlinx-coroutines-test-jvm-1.8.0.jar', test)
    spec = importlib.util.spec_from_file_location('ports', root/'tests/plugin_session_harness.py')
    ports = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ports)
    stubs = dict(ports.STUBS)
    stubs['Uri.kt'] = 'package android.net\nclass Uri(val text: String) { companion object { fun parse(s: String)=Uri(s) } }'
    stubs['Media.kt'] = stubs['Media.kt'].replace('const val USAGE_MEDIA=1;', 'const val RESULT_END_OF_INPUT=-1; const val USAGE_MEDIA=1;')
    stubs['DataSource.kt'] = '''package androidx.media3.datasource
class DataSpec(val uri: android.net.Uri, val position: Long=0, val length: Long=-1)
interface DataSource {
 fun open(spec: DataSpec): Long; fun read(bytes: ByteArray, offset: Int, length: Int): Int; fun close(); fun getUri(): android.net.Uri?
 fun interface Factory { fun createDataSource(): DataSource }
}
abstract class BaseDataSource(network: Boolean): DataSource {
 fun transferInitializing(s: DataSpec) {}; fun transferStarted(s: DataSpec) {}; fun transferEnded() {}; fun bytesTransferred(n: Int) {}
}
'''
    stubs['Source.kt'] = '''package androidx.media3.exoplayer.source
class ProgressiveMediaSource(val item: androidx.media3.common.MediaItem, val factory: androidx.media3.datasource.DataSource.Factory) {
 class Factory(val factory: androidx.media3.datasource.DataSource.Factory) { fun createMediaSource(i: androidx.media3.common.MediaItem)=ProgressiveMediaSource(i,factory) }
}'''
    stubs['Exo.kt'] = stubs['Exo.kt'].replace('fun build()=ExoPlayer()', 'fun setLoadControl(c: DefaultLoadControl)=this; fun build()=ExoPlayer()')
    stubs['Exo.kt'] = stubs['Exo.kt'].replace('var currentMediaItem: MediaItem?=null', 'var currentMediaItem: MediaItem?=null; var source: androidx.media3.exoplayer.source.ProgressiveMediaSource?=null')
    stubs['Exo.kt'] = stubs['Exo.kt'].replace('{currentMediaItem=s.item}', '{currentMediaItem=s.item; source=s}')
    stubs['Exo.kt'] += '''
class DefaultLoadControl { class Builder { fun setTargetBufferBytes(n: Int)=this; fun setPrioritizeTimeOverSizeThresholds(b: Boolean)=this; fun build()=DefaultLoadControl() } }
'''
    stubs['Ports.kt'] = (root/'tests/stream_ports.kt.txt').read_text(encoding='utf-8')
    stubs['Harness.kt'] = (root/'tests/stream_checks.kt.txt').read_text(encoding='utf-8')
    sources = []
    for name, text in stubs.items():
        path = args.scratch/name
        path.write_text(text, encoding='utf-8')
        sources.append(path)
    run(args.java, jars, [*runtime, test], [*sources, source/'PluginSession.kt', source/'ProviderDataSource.kt'],
        args.scratch/'stream-classes', 'org.megavideoprotect.app.HarnessKt')

    # Real manifest/adapters/rights/host policy, no Android dependencies.
    json = args.scratch/'json-20240303.jar'
    if not json.exists():
        urllib.request.urlretrieve('https://repo.maven.apache.org/maven2/org/json/json/20240303/json-20240303.jar', json)
    proxy = args.scratch/'Proxy.kt'
    proxy.write_text('package org.megavideoprotect.app\nobject AppProxy { fun open(url: java.net.URL): java.net.URLConnection = url.openConnection() }', encoding='utf-8')
    run(args.java, jars, [*runtime, json], [source/n for n in ('PluginPolicy.kt','PluginProviders.kt','SoundCloudTracksProvider.kt','PluginHttp.kt')] +
        [root/'plugin-tests/SoundCloudPolicyTest.kt', proxy], args.scratch/'policy-classes', 'org.megavideoprotect.app.SoundCloudPolicyTestKt', [root.parent/'plugins/soundcloud/plugin.json'])


if __name__ == '__main__':
    main()
