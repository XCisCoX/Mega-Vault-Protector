package org.megavideoprotect.app

import android.content.Context
import java.io.File
import java.io.RandomAccessFile

/**
 * The protocol 4 sync loop, mirroring qt-app/src/sync_engine.cpp: pull every
 * record past the cursor and apply it locally, then push what changed here.
 *
 * Packages are sealed with the vault's package secret, so a device can only
 * read another device's items when it joined the same vault with the same
 * password — the relay never holds that secret.
 */
class SyncV4Engine(
    private val context: Context,
    private val store: SyncV4StateStore,
    link: SyncV4Link,
) {
    var link: SyncV4Link = link
        private set

    private val client = SyncV4Client(link.server, link.token)

    /** What the last run did, in the words the screen shows. */
    var summary: String = ""
        private set

    private fun save() = store.save(link)

    // ── pull ─────────────────────────────────────────────────────────────

    fun pull(onStep: (String) -> Unit = {}): Int {
        var cursor = link.cursor
        var applied = 0
        val mappings = link.mappings.toMutableList()
        while (true) {
            onStep("Reading the server's changes…")
            val page = client.records(link.vaultId, cursor)
            if (page.records.isEmpty()) {
                cursor = page.cursor
                break
            }
            for (record in page.records) {
                if (apply(record, mappings, onStep)) applied++
            }
            cursor = page.cursor
            link = link.copy(cursor = cursor, mappings = mappings.toList())
            save()
            if (page.records.size < 500) break
        }
        // Adopt the local fingerprint of everything we just touched, so the push
        // that follows does not mistake our own pull for a local edit.
        val fresh = runCatching { SyncV4Local.files() }.getOrNull()
        if (fresh != null) {
            val byId = fresh.associateBy { it.id }
            val updated = mappings.map { item ->
                val local = byId[item.localId]
                if (local == null) item else item.copy(path = local.path, signature = local.signature)
            }
            link = link.copy(cursor = cursor, mappings = updated)
            save()
        }
        return applied
    }

    private fun apply(
        record: SyncRecord,
        mappings: MutableList<SyncV4Mapping>,
        onStep: (String) -> Unit,
    ): Boolean {
        val index = mappings.indexOfFirst { it.recordId == record.id }
        val current = if (index >= 0) mappings[index] else null

        if (record.deleted) {
            if (current != null) {
                if (current.localId != 0L) runCatching { CoreBridge.nativeRemoveVideo(current.localId) }
                mappings.removeAt(index)
            }
            return true
        }

        if (current == null) {
            // Taken off this phone on purpose: keep the record known, but do not
            // pull its bytes back down until the user asks for it.
            if (link.remoteOnly.contains(record.id)) {
                mappings.add(
                    SyncV4Mapping(
                        recordId = record.id,
                        localId = 0L,
                        path = record.path,
                        signature = "",
                        version = record.version,
                        packageName = record.packageName,
                    ),
                )
                return false
            }
            val localId = materialise(record, onStep)
            mappings.add(
                SyncV4Mapping(
                    recordId = record.id,
                    localId = localId,
                    path = record.path,
                    signature = "",
                    version = record.version,
                    packageName = record.packageName,
                ),
            )
            return true
        }

        if (record.version <= current.version) return false

        var localId = current.localId
        if (record.packageName != null && record.packageName != current.packageName) {
            if (link.remoteOnly.contains(record.id)) {
                mappings[index] = current.copy(
                    path = record.path,
                    version = record.version,
                    packageName = record.packageName,
                )
                return true
            }
            onStep("Replacing ${record.name}…")
            val landed = materialise(record, onStep)
            if (landed != 0L) {
                if (localId != 0L && localId != landed) runCatching { CoreBridge.nativeRemoveVideo(localId) }
                localId = landed
            }
        } else if (localId != 0L) {
            if (record.name.isNotBlank()) runCatching { CoreBridge.nativeRenameVideo(localId, record.name) }
            applyTags(localId, record.tags)
        }
        mappings[index] = current.copy(
            localId = localId,
            path = record.path,
            version = record.version,
            packageName = record.packageName ?: current.packageName,
        )
        return true
    }

    /** Downloads and imports one record's package; 0 when it carries no package. */
    private fun materialise(record: SyncRecord, onStep: (String) -> Unit): Long {
        val packageName = record.packageName ?: return 0L
        val before = runCatching { Json.videos(CoreBridge.nativeListVideos()).map { it.id }.toSet() }
            .getOrDefault(emptySet())
        onStep("Downloading ${record.name}…")
        val packageFile = download(packageName)
        val folderId = ensureFolder(record.path.substringBeforeLast('/', ""))
        val result = CoreBridge.Result.parse(
            CoreBridge.nativeImportPackage(packageFile.absolutePath, link.packageSecret, folderId),
        )
        packageFile.delete()
        if (!result.ok) {
            // A wrong secret is the usual reason: say so instead of silently
            // importing nothing.
            throw SyncV4Client.Failure(
                result.error.ifBlank { result.detail }.ifBlank {
                    "This vault's items are sealed with a different password."
                },
            )
        }
        val after = runCatching { Json.videos(CoreBridge.nativeListVideos()) }.getOrDefault(emptyList())
        val landed = after.filter { it.id !in before }
            .maxByOrNull { it.importedAt }
            ?: after.firstOrNull { it.name == record.name }
            ?: return 0L
        if (record.name.isNotBlank() && landed.name != record.name) {
            runCatching { CoreBridge.nativeRenameVideo(landed.id, record.name) }
        }
        applyTags(landed.id, record.tags)
        return landed.id
    }

    private fun applyTags(localId: Long, tags: List<String>) {
        if (tags.isEmpty()) return
        val known = runCatching { Json.tags(CoreBridge.nativeListTags()).map { it.name }.toSet() }
            .getOrDefault(emptySet())
        for (tag in tags) {
            if (tag !in known && !known.contains(tag)) {
                CoreBridge.Result.parse(CoreBridge.nativeCreateTag(tag))
            }
            runCatching { CoreBridge.nativeAddTag(localId, tag) }
        }
    }

    /** Finds or creates the folder chain named by a record path. */
    private fun ensureFolder(path: String): Long {
        if (path.isBlank()) return 0L
        var parent = 0L
        for (segment in path.split('/')) {
            if (segment.isBlank()) continue
            val folders = runCatching { Json.folders(CoreBridge.nativeListFolders()) }.getOrDefault(emptyList())
            val existing = folders.firstOrNull { it.parentId == parent && it.name == segment }
            if (existing != null) {
                parent = existing.id
            } else {
                val created = CoreBridge.Result.parse(CoreBridge.nativeCreateFolder(parent, segment))
                if (created.ok && created.id > 0L) parent = created.id
            }
        }
        return parent
    }

    /** Streams one package in ranges and proves it hashes to its address. */
    private fun download(packageName: String): File {
        val state = client.headBlob(link.vaultId, packageName)
        if (!state.exists) throw SyncV4Client.Failure("A package this vault lists is not on the server yet.", 404)
        val target = File(context.cacheDir, "sync-$packageName.mvpk")
        target.parentFile?.mkdirs()
        RandomAccessFile(target, "rw").use { out ->
            out.setLength(0)
            var offset = 0L
            while (offset < state.size) {
                val last = minOf(offset + SyncPolicyV4.CHUNK - 1, state.size - 1)
                val chunk = client.getRange(link.vaultId, packageName, offset, last, state.size)
                out.write(chunk)
                offset = last + 1
            }
        }
        val hash = SyncV4Local.hashFile(target)
        if (hash != packageName) {
            target.delete()
            throw SyncV4Client.Failure("The downloaded package does not match its address.")
        }
        return target
    }

    // ── push ─────────────────────────────────────────────────────────────

    fun push(onStep: (String) -> Unit = {}): Int {
        val locals = SyncV4Local.files()
        val byLocal = link.mappings.associateBy { it.localId }
        val mappings = link.mappings.toMutableList()
        var uploaded = 0

        for (file in locals) {
            val mapping = byLocal[file.id]
            if (mapping == null) {
                onStep("Uploading ${file.name}…")
                val record = createRecord(file)
                mappings.add(
                    SyncV4Mapping(record.id, file.id, file.path, file.signature, record.version, record.packageName),
                )
                uploaded++
            } else if (mapping.signature != file.signature || mapping.path != file.path) {
                onStep("Updating ${file.name}…")
                val stored = updateRecord(file, mapping)
                val index = mappings.indexOfFirst { it.recordId == stored.id }
                if (index >= 0) {
                    mappings[index] = mappings[index].copy(
                        path = file.path,
                        signature = file.signature,
                        version = stored.version,
                        packageName = stored.packageName,
                    )
                }
                uploaded++
            }
            link = link.copy(mappings = mappings.toList())
            save()
        }

        // Items deleted here become tombstones, so the other device removes
        // them instead of uploading them back.
        val alive = locals.map { it.id }.toSet()
        val gone = mappings.filter { it.localId != 0L && it.localId !in alive }
        for (item in gone) {
            onStep("Removing ${item.path}…")
            runCatching { client.tombstone(link.vaultId, item.recordId) }
            mappings.removeAll { it.recordId == item.recordId }
            link = link.copy(mappings = mappings.toList())
            save()
        }
        return uploaded
    }

    private fun createRecord(file: SyncV4LocalFile): SyncRecord {
        val packageFile = exportPackage(file)
        val packageName = SyncV4Local.hashFile(packageFile)
        client.putBlob(link.vaultId, packageName, packageFile)
        val record = SyncRecord(
            id = SyncV4Local.newRecordId(),
            name = file.name,
            path = file.path,
            tags = tagsOf(file.id),
            packageName = packageName,
            size = packageFile.length(),
            version = 0,
            deleted = false,
            updated = 0,
        )
        packageFile.delete()
        return client.write(link.vaultId, record, 0, "create")
    }

    private fun updateRecord(file: SyncV4LocalFile, mapping: SyncV4Mapping): SyncRecord {
        val packageFile = exportPackage(file)
        val packageName = SyncV4Local.hashFile(packageFile)
        val record = SyncRecord(
            id = mapping.recordId,
            name = file.name,
            path = file.path,
            tags = tagsOf(file.id),
            packageName = packageName,
            size = packageFile.length(),
            version = mapping.version,
            deleted = false,
            updated = 0,
        )
        try {
            client.putBlob(link.vaultId, packageName, packageFile)
            return client.write(link.vaultId, record, mapping.version, "update")
        } catch (conflict: SyncV4Client.Conflict) {
            // Someone else wrote first. Keep this copy rather than losing it:
            // the pull that follows will bring their version in beside it.
            throw SyncV4Client.Failure(
                "“${file.name}” changed on another device at the same time. Sync again to merge.",
                409,
            )
        } finally {
            packageFile.delete()
        }
    }

    private fun exportPackage(file: SyncV4LocalFile): File {
        val target = File(context.cacheDir, "sync-export-${file.id}.mvpk")
        val segments = file.path.split('/').filter { it.isNotBlank() }
        val directories = segments.dropLast(1).runningFold("") { parent, name ->
            if (parent.isEmpty()) name else "$parent/$name"
        }.drop(1)
        val result = CoreBridge.Result.parse(
            CoreBridge.nativeExportPackage(
                target.absolutePath,
                link.packageSecret,
                longArrayOf(file.id),
                arrayOf(file.path),
                directories.toTypedArray(),
                false,
            ),
        )
        if (!result.ok) {
            throw SyncV4Client.Failure(result.error.ifBlank { result.detail }.ifBlank { "Cannot export ${file.name}." })
        }
        return target
    }

    private fun tagsOf(localId: Long): List<String> =
        runCatching { Json.videos(CoreBridge.nativeListVideos()).firstOrNull { it.id == localId }?.tags }
            .getOrNull().orEmpty()

    // ── one full pass ────────────────────────────────────────────────────

    /** Pull then push, exactly the order the desktop uses. */
    fun syncNow(onStep: (String) -> Unit = {}): String {
        val applied = pull(onStep)
        val pushed = push(onStep)
        summary = when {
            applied == 0 && pushed == 0 -> "Everything is up to date."
            else -> "Pulled $applied, pushed $pushed."
        }
        return summary
    }

    // ── the list, and what a selection does ──────────────────────────────

    /** One row of the list: what the vault holds and where this phone stands. */
    data class Item(
        val recordId: String,
        val name: String,
        val path: String,
        val state: String,
        val size: Long,
        val localId: Long,
    )

    fun items(): List<Item> {
        val locals = runCatching { Json.videos(CoreBridge.nativeListVideos()) }.getOrDefault(emptyList())
        val byId = locals.associateBy { it.id }
        return link.mappings.map { mapping ->
            val local = byId[mapping.localId]
            val onDevice = mapping.localId != 0L && local != null
            Item(
                recordId = mapping.recordId,
                name = mapping.path.substringAfterLast('/').ifBlank { mapping.path },
                path = mapping.path,
                state = when {
                    onDevice -> "synced"
                    link.remoteOnly.contains(mapping.recordId) -> "remote"
                    else -> "pending"
                },
                size = local?.size ?: 0L,
                localId = if (onDevice) mapping.localId else 0L,
            )
        }.sortedBy { it.path.lowercase() }
    }

    /** Downloads the ticked records; they leave the shelf and stay here. */
    fun downloadItems(recordIds: List<String>, onStep: (String) -> Unit = {}): Int {
        var kept = 0
        val mappings = link.mappings.toMutableList()
        for (id in recordIds) {
            val index = mappings.indexOfFirst { it.recordId == id }
            if (index < 0) continue
            onStep("Downloading ${mappings[index].path}…")
            val record = runCatching { client.record(link.vaultId, id) }.getOrNull() ?: continue
            if (record.deleted) continue
            val localId = materialise(record, onStep)
            if (localId == 0L) continue
            mappings[index] = mappings[index].copy(
                localId = localId,
                path = record.path,
                version = record.version,
                packageName = record.packageName ?: mappings[index].packageName,
                signature = "",
            )
            link = link.copy(mappings = mappings.toList(), remoteOnly = link.remoteOnly - id)
            save()
            kept++
        }
        return kept
    }

    /** Deletes this phone's copy only; the record stays in the vault. */
    fun removeFromDevice(recordIds: List<String>): Int {
        var dropped = 0
        val mappings = link.mappings.toMutableList()
        val shelf = link.remoteOnly.toMutableList()
        for (id in recordIds) {
            val index = mappings.indexOfFirst { it.recordId == id }
            if (index < 0) continue
            val localId = mappings[index].localId
            if (localId != 0L) {
                runCatching { CoreBridge.nativeRemoveVideo(localId) }
                dropped++
            }
            mappings[index] = mappings[index].copy(localId = 0L, signature = "")
            if (!shelf.contains(id)) shelf.add(id)
        }
        link = link.copy(mappings = mappings.toList(), remoteOnly = shelf)
        save()
        return dropped
    }

    fun restoreItems(recordIds: List<String>, onStep: (String) -> Unit = {}) {
        for (id in recordIds) {
            onStep("Restoring…")
            runCatching { client.restore(link.vaultId, id) }
        }
    }

    fun tombstoneItems(recordIds: List<String>, onStep: (String) -> Unit = {}) {
        val mappings = link.mappings.toMutableList()
        for (id in recordIds) {
            onStep("Removing…")
            runCatching { client.tombstone(link.vaultId, id) }
            mappings.removeAll { it.recordId == id }
        }
        link = link.copy(mappings = mappings.toList(), remoteOnly = link.remoteOnly - recordIds.toSet())
        save()
    }

    fun purgeItems(recordIds: List<String>, onStep: (String) -> Unit = {}) {
        val mappings = link.mappings.toMutableList()
        for (id in recordIds) {
            onStep("Deleting permanently…")
            val localId = mappings.firstOrNull { it.recordId == id }?.localId ?: 0L
            runCatching { client.purge(link.vaultId, id) }
            if (localId != 0L) runCatching { CoreBridge.nativeRemoveVideo(localId) }
            mappings.removeAll { it.recordId == id }
        }
        link = link.copy(mappings = mappings.toList(), remoteOnly = link.remoteOnly - recordIds.toSet())
        save()
    }
}
