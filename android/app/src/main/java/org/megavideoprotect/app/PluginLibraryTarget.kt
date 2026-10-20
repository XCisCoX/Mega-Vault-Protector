package org.megavideoprotect.app

/** A provider folder is navigation state, never a native vault folder or file ID. */
data class PluginLibraryTarget(
    val query: String,
    val item: PluginItem? = null,
    val category: String? = null,
) {
    val title: String get() = item?.title ?: when (category) {
        "track" -> "Songs"
        "artist" -> "Artists"
        "album" -> "Albums"
        else -> "Search for “$query”"
    }
    fun visibleRows(rows: List<PluginItem>): List<PluginItem> =
        if (category == null) rows else rows.filter { it.kind == category }
}
