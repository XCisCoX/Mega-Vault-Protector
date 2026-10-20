package org.megavideoprotect.app

import android.graphics.Bitmap
import android.graphics.Color as AndroidColor
import android.graphics.pdf.PdfRenderer
import android.os.ParcelFileDescriptor
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.max

/**
 * Reads a PDF stored in the vault. The plaintext is written to a private cache
 * file only while the page is open, then deleted. Android's renderer cannot
 * open a password-protected PDF, so those stay readable on Windows.
 */
@Composable
fun PdfScreen(
    videoId: Long,
    name: String,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    var renderer by remember(videoId) { mutableStateOf<PdfRenderer?>(null) }
    var pageCount by remember(videoId) { mutableIntStateOf(0) }
    var error by remember(videoId) { mutableStateOf<String?>(null) }
    var widthPx by remember(videoId) { mutableIntStateOf(0) }
    var scale by remember(videoId) { mutableFloatStateOf(1f) }
    var pan by remember(videoId) { mutableStateOf(Offset.Zero) }
    val scroll = rememberScrollState()
    val scope = rememberCoroutineScope()
    val gate = remember(videoId) { Mutex() }
    var cacheFile by remember(videoId) { mutableStateOf<File?>(null) }
    var descriptor by remember(videoId) { mutableStateOf<ParcelFileDescriptor?>(null) }

    LaunchedEffect(videoId) {
        renderer?.close()
        renderer = null
        error = null
        val opened = withContext(Dispatchers.IO) {
            runCatching { openVaultPdf(context.cacheDir, videoId) }
        }
        opened.onSuccess { pdf ->
            cacheFile = pdf.file
            descriptor = pdf.descriptor
            pageCount = pdf.renderer.pageCount
            renderer = pdf.renderer
        }.onFailure {
            error = if (it is SecurityException) {
                "This PDF is password protected. Open it on Windows."
            } else {
                "This PDF could not be opened."
            }
        }
    }
    DisposableEffect(videoId) {
        onDispose {
            renderer?.close()
            descriptor?.close()
            cacheFile?.delete()
        }
    }

    Column(Modifier.fillMaxSize().background(Mvp.window).statusBarsPadding()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                painterResource(R.drawable.ic_lucide_arrow_left),
                contentDescription = "Back",
                tint = Mvp.accent,
                modifier = Modifier.clickable(onClick = onBack).padding(8.dp).size(24.dp),
            )
            Text(
                name,
                color = Mvp.text,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            )
            if (scale > 1.05f) {
                Text(
                    "Fit",
                    color = Mvp.accent,
                    fontSize = 15.sp,
                    modifier = Modifier.clickable {
                        scale = 1f
                        pan = Offset.Zero
                    }.padding(end = 12.dp),
                )
            }
            if (pageCount > 0) {
                Text("$pageCount", color = Mvp.description, fontSize = 13.sp, modifier = Modifier.padding(end = 8.dp))
            }
        }
        when {
            error != null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(error.orEmpty(), color = Mvp.description, modifier = Modifier.padding(24.dp))
            }
            renderer == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = Mvp.accent)
            }
            else -> Column(
                Modifier
                    .fillMaxSize()
                    .onSizeChanged { widthPx = it.width }
                    .pdfGestures(
                        scale = { scale },
                        onScale = { scale = it },
                        pan = { pan },
                        onPan = { pan = it },
                        onScroll = { dy -> scope.launch { scroll.scrollBy(dy) } },
                    )
                    .graphicsLayer {
                        scaleX = scale
                        scaleY = scale
                        translationX = pan.x
                        translationY = pan.y
                    }
                    .verticalScroll(scroll, enabled = false)
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                repeat(pageCount) { index ->
                    PdfPageImage(renderer!!, gate, index, widthPx)
                }
            }
        }
    }
}

private fun Modifier.pdfGestures(
    scale: () -> Float,
    onScale: (Float) -> Unit,
    pan: () -> Offset,
    onPan: (Offset) -> Unit,
    onScroll: (Float) -> Unit,
): Modifier = pointerInput(Unit) {
    var lastUp = 0L
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        var last = down.position
        var moved = 0f
        var zooming = false
        while (true) {
            val event = awaitPointerEvent()
            val pressed = event.changes.filter { it.pressed }
            if (pressed.size >= 2) {
                zooming = true
                val zoom = event.calculateZoom()
                val drag = event.calculatePan()
                val next = (scale() * zoom).coerceIn(1f, 5f)
                onScale(next)
                if (next <= 1.02f) {
                    onPan(Offset.Zero)
                } else {
                    val maxX = size.width * (next - 1f) / 2f
                    val maxY = size.height * (next - 1f) / 2f
                    val current = pan()
                    onPan(
                        Offset(
                            (current.x + drag.x).coerceIn(-maxX, maxX),
                            (current.y + drag.y).coerceIn(-maxY, maxY),
                        ),
                    )
                }
                event.changes.forEach { if (it.positionChanged()) it.consume() }
                continue
            }
            val change = event.changes.firstOrNull { it.id == down.id }
            if (change == null || !change.pressed) {
                if (!zooming && moved < 12f) {
                    val now = System.currentTimeMillis()
                    if (now - lastUp < 280L) {
                        if (scale() > 1.05f) {
                            onScale(1f)
                            onPan(Offset.Zero)
                        } else {
                            onScale(2.5f)
                        }
                        lastUp = 0L
                    } else {
                        lastUp = now
                    }
                }
                break
            }
            val delta = change.position - last
            last = change.position
            moved += abs(delta.x) + abs(delta.y)
            if (moved < 8f) continue
            if (scale() > 1.05f) {
                val next = scale()
                val maxX = size.width * (next - 1f) / 2f
                val maxY = size.height * (next - 1f) / 2f
                val current = pan()
                onPan(
                    Offset(
                        (current.x + delta.x).coerceIn(-maxX, maxX),
                        (current.y + delta.y).coerceIn(-maxY, maxY),
                    ),
                )
                change.consume()
            } else if (abs(delta.y) >= abs(delta.x)) {
                onScroll(-delta.y)
                change.consume()
            }
        }
    }
}

@Composable
private fun PdfPageImage(
    renderer: PdfRenderer,
    gate: Mutex,
    index: Int,
    widthPx: Int,
) {
    var bitmap by remember(index, widthPx) { mutableStateOf<Bitmap?>(null) }
    LaunchedEffect(index, widthPx) {
        if (widthPx < 32) return@LaunchedEffect
        val rendered = withContext(Dispatchers.Default) {
            gate.withLock {
                renderer.openPage(index).use { page ->
                    val width = widthPx.coerceAtMost(1600)
                    val height = max(1, page.height * width / max(1, page.width))
                    val image = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                    image.eraseColor(AndroidColor.WHITE)
                    page.render(image, null, null, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY)
                    image
                }
            }
        }
        bitmap = rendered
    }
    val image = bitmap
    if (image == null) {
        Box(
            Modifier.fillMaxWidth().padding(vertical = 48.dp),
            contentAlignment = Alignment.Center,
        ) {
            CircularProgressIndicator(color = Mvp.accent, strokeWidth = 2.dp)
        }
    } else {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = "Page ${index + 1}",
            modifier = Modifier.fillMaxWidth(),
            contentScale = ContentScale.FillWidth,
        )
    }
}

private class OpenedPdf(
    val renderer: PdfRenderer,
    val descriptor: ParcelFileDescriptor,
    val file: File,
)

private fun openVaultPdf(cacheDir: File, id: Long): OpenedPdf {
    val bytes = if (RemoteVault.connected()) {
        RemoteVault.readAll(id, 80 * 1024 * 1024)
    } else {
        val size = CoreBridge.nativeVideoSize(id)
        if (size <= 0L || size > 80L * 1024 * 1024) {
            throw IllegalStateException("size")
        }
        val all = ByteArray(size.toInt())
        var offset = 0
        while (offset < all.size) {
            val want = minOf(1 shl 20, all.size - offset)
            val chunk = CoreBridge.nativeReadRange(id, offset.toLong(), want)
                ?: throw IllegalStateException("read")
            if (chunk.isEmpty()) break
            val count = minOf(chunk.size, all.size - offset)
            System.arraycopy(chunk, 0, all, offset, count)
            offset += count
        }
        if (offset <= 0) throw IllegalStateException("empty") else all.copyOf(offset)
    } ?: throw IllegalStateException("empty")
    val file = File(cacheDir, "mvp-pdf-$id.pdf")
    file.writeBytes(bytes)
    val descriptor = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    return OpenedPdf(PdfRenderer(descriptor), descriptor, file)
}
