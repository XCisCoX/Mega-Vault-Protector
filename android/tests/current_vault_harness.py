"""Compile real VaultIdentity/PluginManager/policy on JVM with in-memory Android ports.

Exercises namespace selection and actual manager reads/writes, not Android storage,
Compose rendering, QR transport, JNI opening or activity lifecycle instrumentation.
"""
import argparse
import os
from pathlib import Path
import subprocess
import urllib.request

PORTS = {
    'Context.kt': '''package android.content
import java.io.File
class Context(val noBackupFilesDir: File) {
    companion object { const val MODE_PRIVATE = 0 }
    private val prefs = mutableMapOf<String, Preferences>()
    fun getSharedPreferences(name: String, mode: Int) = prefs.getOrPut(name) { Preferences() }
    val assets = Assets()
    val contentResolver = Resolver()
}
class Preferences {
    private val values = mutableMapOf<String, Any>()
    fun getBoolean(key: String, default: Boolean) = values[key] as? Boolean ?: default
    fun getLong(key: String, default: Long) = values[key] as? Long ?: default
    fun getInt(key: String, default: Int) = values[key] as? Int ?: default
    fun edit() = Editor(values)
}
class Editor(private val values: MutableMap<String, Any>) {
    private val pending = mutableMapOf<String, Any>()
    fun putBoolean(key: String, value: Boolean) = apply { pending[key] = value }
    fun putLong(key: String, value: Long) = apply { pending[key] = value }
    fun putInt(key: String, value: Int) = apply { pending[key] = value }
    fun commit(): Boolean { values.putAll(pending); return true }
}
class Assets { fun open(path: String): java.io.InputStream = error("Unused asset port") }
class Resolver { fun openInputStream(uri: android.net.Uri): java.io.InputStream? = error("Unused document port") }
''',
    'Uri.kt': 'package android.net\nclass Uri\n',
    'CurrentVaultBehavior.kt': '''package org.megavideoprotect.app
import android.content.Context
import java.io.File

fun main(args: Array<String>) {
    val context = Context(File(args.single()).apply { mkdirs() })
    val a = File(context.noBackupFilesDir, "vault-A").path
    val b = File(context.noBackupFilesDir, "vault-B").path
    val plugin = PluginManifest("org.example.fixture", "Fixture", "1.0.0", "Fixture", "", emptySet(), "",
        settings = listOf(ExtensionBooleanSetting("trim", "Trim", false)))
    var identity = VaultIdentity().openedLocal(a)
    fun manager() = PluginManager(context, checkNotNull(identity.activeLocalRoot))
    val approvedA = PluginOptions(enabled = true, autoSave = true, folderId = 73, memoryMiB = 64)
    manager().save(plugin, approvedA)
    manager().saveBooleanSettings(plugin, mapOf("trim" to true))
    check(manager().options(plugin) == approvedA)

    // A picker preference is not an unlock: changing it must not select B yet.
    var pickerPreference = b
    check(identity.activeLocalRoot == a)
    check(manager().options(plugin).enabled)
    // Only the successful native-open/create callback publishes the new root.
    identity = identity.openedLocal(pickerPreference)
    check(identity.activeLocalRoot == b)
    check(manager().options(plugin) == PluginOptions()) { "B inherited A consent/destination" }
    check(manager().booleanSettings(plugin) == mapOf("trim" to false))
    val approvedB = PluginOptions(enabled = true, folderId = 8, memoryMiB = 128)
    manager().save(plugin, approvedB)
    manager().saveBooleanSettings(plugin, mapOf("trim" to false))
    pickerPreference = a
    check(identity.activeLocalRoot == b)
    check(manager().options(plugin) == approvedB) { "Unlocked B followed picker preference" }

    // Activity recreation reuses the process session identity, not startup prefs.
    val retainedSession = identity
    identity = retainedSession
    check(identity.activeLocalRoot == b && pickerPreference == a)
    check(manager().options(plugin) == approvedB)
    check(!manager().options(plugin.copy(version = "2.0.0")).enabled)

    identity = identity.connectedRemote()
    check(identity.localRoot == b) { "QR connection overwrote last local root" }
    check(identity.activeLocalRoot == null) { "Remote exposed a local namespace" }
    check(VaultIdentity().connectedRemote().activeLocalRoot == null)
    identity = identity.openedLocal(a)
    check(manager().options(plugin) == approvedA) { "B changed A settings" }
    check(manager().booleanSettings(plugin) == mapOf("trim" to true))
    identity = identity.openedLocal(b)
    check(manager().options(plugin) == approvedB)
    check(manager().booleanSettings(plugin) == mapOf("trim" to false))
    println("CurrentVaultBehavior: A -> B consent/options isolation, picker drift, retained identity, remote exclusion, version isolation, A/B reopen passed")
}
''',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', required=True, type=Path)
    parser.add_argument('--scratch', required=True, type=Path)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    sources = []
    for name, text in PORTS.items():
        target = args.scratch / name
        target.write_text(text, encoding='utf-8')
        sources.append(target)
    json = args.scratch / 'json.jar'
    if not json.exists():
        urllib.request.urlretrieve('https://repo.maven.apache.org/maven2/org/json/json/20240303/json-20240303.jar', json)
    app = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app'
    sources += [app / name for name in ('VaultIdentity.kt', 'PluginManager.kt', 'PluginPolicy.kt', 'PluginProviders.kt')]
    java = next(args.tools.glob('*/bin/java.exe'))
    compiler_cp = os.pathsep.join(str(p) for p in args.tools.glob('*.jar'))
    runtime_cp = os.pathsep.join(str(p) for p in (args.tools / 'kotlin-stdlib.jar', args.tools / 'annotations.jar', json))
    classes = args.scratch / 'classes'
    subprocess.run([str(java), '-cp', compiler_cp, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
                    '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', runtime_cp,
                    '-d', str(classes), *(str(p) for p in sources)], check=True)
    subprocess.run([str(java), '-cp', str(classes) + os.pathsep + runtime_cp,
                    'org.megavideoprotect.app.CurrentVaultBehaviorKt', str(args.scratch / 'state')], check=True)


if __name__ == '__main__':
    main()
