package org.megavideoprotect.app

import org.junit.Assert.*
import org.junit.Test

class PluginLibraryTargetTest {
    private val song = PluginItem("track", "168671849784291328", "One song")
    private val artist = PluginItem("artist", "155479365088448512", "One artist")
    private val album = PluginItem("album", "166472736192020480", "One album")
    private val rows = listOf(song, artist, album)

    @Test fun rootShowsSongsArtistsAndAlbumsWithoutConvertingIds() {
        val target = PluginLibraryTarget("a search")
        assertEquals(rows, target.visibleRows(rows))
        assertEquals("168671849784291328", target.visibleRows(rows).first().id)
    }

    @Test fun eachCategoryUsesTheSameResultSetWithoutMutatingIt() {
        assertEquals(listOf(song), PluginLibraryTarget("q", category = "track").visibleRows(rows))
        assertEquals(listOf(artist), PluginLibraryTarget("q", category = "artist").visibleRows(rows))
        assertEquals(listOf(album), PluginLibraryTarget("q", category = "album").visibleRows(rows))
        assertEquals(3, rows.size)
    }

    @Test fun albumAndArtistFoldersRetainTheOriginalSearchForBackNavigation() {
        val root = PluginLibraryTarget("my original search")
        val category = root.copy(category = "artist")
        val detail = PluginLibraryTarget(category.query, item = artist)
        assertEquals("my original search", detail.query)
        assertEquals("One artist", detail.title)
        assertEquals("Artists", category.title)
        assertNull(root.category)
        assertNull(root.item)
    }

    @Test fun emptyCategoryIsAnEmptyResultNotAVaultId() {
        assertTrue(PluginLibraryTarget("q", category = "album").visibleRows(listOf(song)).isEmpty())
    }
}
