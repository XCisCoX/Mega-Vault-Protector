package org.megavideoprotect.app

import android.Manifest
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.provider.MediaStore
import android.net.Uri
import android.util.Size
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

enum class DevicePick { Gallery, Music }

private data class PhoneFile(
    val uri: Uri,
    val name: String,
    val detail: String,
    val added: Long,
)

@Composable
fun DeviceLibraryScreen(
    kind: DevicePick,
    onClose: () -> Unit,
    onBrowse: () -> Unit,
    onImport: (List<Uri>) -> Unit,
) {
    val context = LocalContext.current
    val permissions = remember(kind) { mediaPermissions(kind) }
    var allowed by remember(kind) { mutableStateOf(hasMediaAccess(context, permissions)) }
    var files by remember(kind) { mutableStateOf<List<PhoneFile>>(emptyList()) }
    var query by remember(kind) { mutableStateOf("") }
    var picked by remember(kind) { mutableStateOf(setOf<Uri>()) }
    val ask = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        allowed = hasMediaAccess(context, permissions)
    }
    LaunchedEffect(kind, allowed) {
        files = if (allowed) {
            withContext(Dispatchers.IO) {
                if (kind == DevicePick.Music) loadMusic(context) else loadGallery(context)
            }
        } else {
            emptyList()
        }
    }
    val shown = remember(files, query) {
        if (query.isBlank()) files
        else files.filter {
            it.name.contains(query, ignoreCase = true) || it.detail.contains(query, ignoreCase = true)
        }
    }
    Column(
        Modifier
            .fillMaxSize()
            .background(Mvp.window)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.size(40.dp).clip(CircleShape).clickable(onClick = onClose),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    painterResource(R.drawable.ic_ab_back),
                    contentDescription = "Back",
                    tint = Color.White,
                    modifier = Modifier.size(22.dp),
                )
            }
            Text(
                if (kind == DevicePick.Music) "Music" else "Gallery",
                color = Mvp.title,
                fontSize = 20.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f).padding(start = 4.dp),
            )
            Text(
                "Browse",
                color = Mvp.accent,
                fontSize = 15.sp,
                modifier = Modifier.clickable(onClick = onBrowse).padding(8.dp),
            )
        }
        BasicTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 4.dp)
                .clip(RoundedCornerShape(20.dp))
                .background(Mvp.glass)
                .padding(horizontal = 14.dp, vertical = 10.dp),
            singleLine = true,
            textStyle = TextStyle(color = Mvp.title, fontSize = 15.sp),
            cursorBrush = SolidColor(Mvp.accent),
            decorationBox = { inner ->
                Box {
                    if (query.isEmpty()) Text("Search", color = Mvp.placeholder, fontSize = 15.sp)
                    inner()
                }
            },
        )
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when {
                !allowed -> Column(
                    Modifier.fillMaxSize().padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(
                        if (kind == DevicePick.Music) {
                            "Music on this phone is not visible yet."
                        } else {
                            "Photos and videos on this phone are not visible yet."
                        },
                        color = Mvp.text,
                        fontSize = 16.sp,
                    )
                    MvpButton("Allow access", onClick = { ask.launch(permissions) }, primary = true, modifier = Modifier.fillMaxWidth())
                    MvpButton("Browse storage", onClick = onBrowse, modifier = Modifier.fillMaxWidth())
                }
                shown.isEmpty() -> Text(
                    "Nothing here.",
                    color = Mvp.description,
                    fontSize = 15.sp,
                    modifier = Modifier.padding(24.dp),
                )
                kind == DevicePick.Music -> LazyColumn(
                    contentPadding = PaddingValues(bottom = 12.dp),
                ) {
                    items(shown, key = { it.uri.toString() }) { file ->
                        val on = file.uri in picked
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable {
                                    picked = if (on) picked - file.uri else picked + file.uri
                                }
                                .padding(horizontal = 16.dp, vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            PickMark(on)
                            Column(Modifier.weight(1f)) {
                                Text(file.name, color = Mvp.text, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                if (file.detail.isNotBlank()) {
                                    Text(file.detail, color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                }
                            }
                        }
                    }
                }
                else -> LazyVerticalGrid(
                    columns = GridCells.Fixed(3),
                    contentPadding = PaddingValues(1.dp),
                    horizontalArrangement = Arrangement.spacedBy(1.dp),
                    verticalArrangement = Arrangement.spacedBy(1.dp),
                ) {
                    items(shown, key = { it.uri.toString() }) { file ->
                        GalleryCell(file, file.uri in picked) {
                            picked = if (file.uri in picked) picked - file.uri else picked + file.uri
                        }
                    }
                }
            }
        }
        if (picked.isNotEmpty()) {
            MvpButton(
                "Add ${picked.size}",
                onClick = { onImport(picked.toList()) },
                primary = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            )
        }
    }
}

@Composable
private fun PickMark(on: Boolean) {
    Box(
        Modifier
            .size(22.dp)
            .clip(CircleShape)
            .background(if (on) Mvp.primary else Color(0x33FFFFFF)),
        contentAlignment = Alignment.Center,
    ) {
        if (on) {
            Icon(
                painterResource(R.drawable.checkbig),
                contentDescription = null,
                tint = Color.White,
                modifier = Modifier.size(14.dp),
            )
        }
    }
}

@Composable
private fun GalleryCell(file: PhoneFile, on: Boolean, onClick: () -> Unit) {
    val context = LocalContext.current
    var thumb by remember(file.uri) { mutableStateOf<android.graphics.Bitmap?>(null) }
    LaunchedEffect(file.uri) {
        thumb = withContext(Dispatchers.IO) { loadThumb(context, file.uri) }
    }
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .background(Color(0xFF161618))
            .clickable(onClick = onClick),
    ) {
        val image = thumb
        if (image != null) {
            Image(
                image.asImageBitmap(),
                contentDescription = file.name,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        }
        Box(Modifier.align(Alignment.TopEnd).padding(6.dp)) { PickMark(on) }
    }
}

private fun mediaPermissions(kind: DevicePick): Array<String> =
    if (Build.VERSION.SDK_INT >= 33) {
        if (kind == DevicePick.Music) {
            arrayOf(Manifest.permission.READ_MEDIA_AUDIO)
        } else {
            arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO)
        }
    } else {
        arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
    }

private fun hasMediaAccess(context: Context, permissions: Array<String>): Boolean =
    permissions.all {
        ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
    }

private fun loadMusic(context: Context): List<PhoneFile> {
    val uri = MediaStore.Audio.Media.EXTERNAL_CONTENT_URI
    val cols = arrayOf(
        MediaStore.Audio.Media._ID,
        MediaStore.Audio.Media.DISPLAY_NAME,
        MediaStore.Audio.Media.ARTIST,
        MediaStore.Audio.Media.DURATION,
        MediaStore.Audio.Media.DATE_ADDED,
    )
    return queryStore(context, uri, cols, MediaStore.Audio.Media.DATE_ADDED + " DESC") { cursor ->
        val id = cursor.getLong(0)
        val name = cursor.getString(1) ?: return@queryStore null
        val artist = cursor.getString(2)?.takeIf { it.isNotBlank() && !it.equals("<unknown>", true) }.orEmpty()
        val duration = cursor.getLong(3)
        val added = cursor.getLong(4)
        val clock = if (duration > 0L) {
            val sec = duration / 1000
            "%d:%02d".format(sec / 60, sec % 60)
        } else ""
        val detail = listOf(artist, clock).filter { it.isNotBlank() }.joinToString("  ·  ")
        PhoneFile(ContentUris.withAppendedId(uri, id), name, detail, added)
    }
}

private fun loadGallery(context: Context): List<PhoneFile> {
    val images = MediaStore.Images.Media.EXTERNAL_CONTENT_URI
    val videos = MediaStore.Video.Media.EXTERNAL_CONTENT_URI
    val imageCols = arrayOf(
        MediaStore.Images.Media._ID,
        MediaStore.Images.Media.DISPLAY_NAME,
        MediaStore.Images.Media.DATE_ADDED,
    )
    val videoCols = arrayOf(
        MediaStore.Video.Media._ID,
        MediaStore.Video.Media.DISPLAY_NAME,
        MediaStore.Video.Media.DURATION,
        MediaStore.Video.Media.DATE_ADDED,
    )
    val photos = queryStore(context, images, imageCols, MediaStore.Images.Media.DATE_ADDED + " DESC") { cursor ->
        val id = cursor.getLong(0)
        val name = cursor.getString(1) ?: return@queryStore null
        PhoneFile(ContentUris.withAppendedId(images, id), name, "", cursor.getLong(2))
    }
    val clips = queryStore(context, videos, videoCols, MediaStore.Video.Media.DATE_ADDED + " DESC") { cursor ->
        val id = cursor.getLong(0)
        val name = cursor.getString(1) ?: return@queryStore null
        val duration = cursor.getLong(2)
        val clock = if (duration > 0L) {
            val sec = duration / 1000
            "%d:%02d".format(sec / 60, sec % 60)
        } else ""
        PhoneFile(ContentUris.withAppendedId(videos, id), name, clock, cursor.getLong(3))
    }
    return (photos + clips).sortedByDescending { it.added }
}

private fun queryStore(
    context: Context,
    uri: Uri,
    columns: Array<String>,
    sort: String,
    read: (android.database.Cursor) -> PhoneFile?,
): List<PhoneFile> {
    val found = ArrayList<PhoneFile>()
    context.contentResolver.query(uri, columns, null, null, sort)?.use { cursor ->
        while (cursor.moveToNext()) {
            read(cursor)?.let { found.add(it) }
        }
    }
    return found
}

private fun loadThumb(context: Context, uri: Uri): android.graphics.Bitmap? {
    if (Build.VERSION.SDK_INT < 29) return null
    return runCatching {
        context.contentResolver.loadThumbnail(uri, Size(240, 240), null)
    }.getOrNull()
}
