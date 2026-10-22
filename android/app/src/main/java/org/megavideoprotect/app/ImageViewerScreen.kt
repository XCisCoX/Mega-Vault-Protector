package org.megavideoprotect.app

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.rememberTransformableState
import androidx.compose.foundation.gestures.transformable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.animation.core.VectorConverter
import kotlinx.coroutines.launch
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer

/**
 * Image extensions the bundled core can actually decode (FFmpeg's image2 demuxer
 * plus the compiled decoders). Anything here opens in the image viewer: the media
 * player has no stream to play for a single frame, so an image "playing" was a
 * dead 0-length video.
 */
private val IMAGE_EXTENSIONS =
    setOf("png", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff")

fun isImageName(name: String): Boolean =
    name.substringAfterLast('.', "").lowercase() in IMAGE_EXTENSIONS

private val AUDIO_EXTENSIONS = setOf(
    "mp3", "flac", "wav", "wave", "ogg", "oga", "opus", "m4a", "aac",
    "wma", "aiff", "aif", "alac", "mp2", "mka",
)

fun isAudioName(name: String): Boolean =
    name.substringAfterLast('.', "").lowercase() in AUDIO_EXTENSIONS

fun isPdfName(name: String): Boolean =
    name.substringAfterLast('.', "").lowercase() == "pdf"

/**
 * Full-screen photo viewer, modelled on Telegram's media viewer: the picture
 * sits on a black stage (a photo viewer is a dark room in every theme), the
 * chrome follows the app palette and hides on a tap, pinch zooms around the
 * fingers, a double-tap zooms on the tapped point, and panning stops at the
 * image edges instead of drifting into empty space.
 */
@Composable
fun ImageViewerScreen(
    videoId: Long,
    name: String,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var bitmap by remember(videoId) { mutableStateOf<Bitmap?>(null) }
    var failed by remember(videoId) { mutableStateOf(false) }
    var rotation by remember(videoId) { mutableStateOf(0f) }
    var chrome by remember { mutableStateOf(true) }
    // The fitted picture size and the viewport, shared with the zoom pill so its
    // buttons clamp panning exactly like the gestures do.
    var stageImage by remember { mutableStateOf(Size.Zero) }
    var stageBox by remember { mutableStateOf(Size.Zero) }
    val scale = remember(videoId) { Animatable(1f) }
    val offset = remember(videoId) { Animatable(Offset.Zero, Offset.VectorConverter) }
    val appear = remember(videoId) { Animatable(0f) }

    LaunchedEffect(videoId) {
        bitmap = null
        failed = false
        val metrics = context.resources.displayMetrics
        val maxDimension = maxOf(metrics.widthPixels, metrics.heightPixels).coerceIn(1024, 2560)
        val decoded = withContext(Dispatchers.Default) {
            runCatching {
                // The phone's decoder understands the JPEGs the gallery exported.
                // FFmpeg's content probe was opening some of them as an empty
                // PNG and reporting that the stored file could not be decoded.
                decodeVaultImage(videoId, maxDimension)
                    ?: CoreBridge.nativeDecodeFrame(videoId, 0, maxDimension)?.let(::rgbaToBitmap)
            }.getOrNull()
        }
        bitmap = decoded
        failed = decoded == null
        if (decoded != null) appear.animateTo(1f, tween(240))
    }

    fun clamped(candidate: Offset, current: Float): Offset {
        if (current <= 1.001f) return Offset.Zero
        if (stageImage == Size.Zero || stageBox == Size.Zero) return Offset.Zero
        val maxX = ((stageImage.width * current - stageBox.width) / 2f).coerceAtLeast(0f)
        val maxY = ((stageImage.height * current - stageBox.height) / 2f).coerceAtLeast(0f)
        return Offset(
            candidate.x.coerceIn(-maxX, maxX),
            candidate.y.coerceIn(-maxY, maxY),
        )
    }

    Box(Modifier.fillMaxSize().background(Color.Black).safeDrawingPadding()) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val boxWidth = constraints.maxWidth.toFloat()
            val boxHeight = constraints.maxHeight.toFloat()
            val image = bitmap
            // The size a Fit image occupies, so panning stops at the picture's
            // own edges rather than the letterbox around it.
            val fitted = if (image != null && image.width > 0 && image.height > 0) {
                val aspect = image.width.toFloat() / image.height.toFloat()
                if (boxWidth / boxHeight > aspect) {
                    Size(boxHeight * aspect, boxHeight)
                } else {
                    Size(boxWidth, boxWidth / aspect)
                }
            } else {
                Size.Zero
            }
            SideEffect {
                stageImage = fitted
                stageBox = Size(boxWidth, boxHeight)
            }

            Box(
                Modifier
                    .fillMaxSize()
                    .pointerInput(videoId) {
                        detectTapGestures(
                            onTap = { chrome = !chrome },
                            onDoubleTap = { point ->
                                val target = if (scale.value > 1.01f) 1f else 2.5f
                                val from = scale.value
                                val next = if (target == 1f) {
                                    Offset.Zero
                                } else {
                                    clamped(point - (point - offset.value) * (target / from), target)
                                }
                                scope.launch {
                                    launch { scale.animateTo(target, tween(220)) }
                                    offset.animateTo(next, tween(220))
                                }
                            },
                        )
                    }
                    .pointerInput(videoId, fitted) {
                        detectTransformGestures { centroid, pan, zoom, _ ->
                            val from = scale.value
                            val next = (from * zoom).coerceIn(1f, 6f)
                            // Zoom around the centroid: the point between the
                            // fingers stays under them.
                            val moved = centroid - (centroid - offset.value) * (next / from) + pan
                            scope.launch {
                                scale.snapTo(next)
                                offset.snapTo(clamped(moved, next))
                            }
                        }
                    },
            ) {
                when {
                    image != null -> Image(
                        bitmap = image.asImageBitmap(),
                        contentDescription = name,
                        modifier = Modifier
                            .fillMaxSize()
                            .graphicsLayer(
                                scaleX = scale.value,
                                scaleY = scale.value,
                                translationX = offset.value.x,
                                translationY = offset.value.y,
                                rotationZ = rotation,
                                alpha = appear.value,
                            ),
                        contentScale = ContentScale.Fit,
                    )
                    failed -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text(
                            "Could not decode this image",
                            color = Mvp.errorText,
                            fontSize = 14.sp,
                            textAlign = TextAlign.Center,
                        )
                    }
                    else -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            CircularProgressIndicator(color = Mvp.accent, strokeWidth = 3.dp)
                            Text(
                                "Decrypting…",
                                color = Mvp.description,
                                fontSize = 12.sp,
                                modifier = Modifier.padding(top = 10.dp),
                            )
                        }
                    }
                }
            }
        }

        // Chrome: a scrimmed top bar and a small zoom pill, both fading out of the
        // way on a tap so the picture is the whole screen.
        AnimatedVisibility(
            visible = chrome,
            enter = fadeIn(tween(160)),
            exit = fadeOut(tween(160)),
            modifier = Modifier.align(Alignment.TopCenter),
        ) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(Mvp.headerBg.copy(alpha = 0.92f))
                    .padding(horizontal = 6.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Box(
                    Modifier.size(40.dp).clickable(onClick = onBack),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        painterResource(R.drawable.ic_lucide_arrow_left),
                        contentDescription = "Back",
                        tint = Mvp.title,
                        modifier = Modifier.size(24.dp),
                    )
                }
                Text(
                    name,
                    color = Mvp.title,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                bitmap?.let {
                    Text(
                        "${it.width} × ${it.height}",
                        color = Mvp.description,
                        fontSize = 11.sp,
                        modifier = Modifier.padding(end = 8.dp),
                    )
                }
            }
        }

        AnimatedVisibility(
            visible = chrome,
            enter = fadeIn(tween(160)),
            exit = fadeOut(tween(160)),
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 18.dp),
        ) {
            Row(
                Modifier
                    .background(Mvp.tileBg.copy(alpha = 0.94f), RoundedCornerShape(22.dp))
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                ControlChip("−") {
                    val next = (scale.value * 0.8f).coerceIn(1f, 6f)
                    scope.launch {
                        scale.animateTo(next, tween(160))
                        offset.animateTo(clamped(offset.value, next), tween(160))
                    }
                }
                Text(
                    "${(scale.value * 100).toInt()}%",
                    color = Mvp.tileText,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(horizontal = 8.dp),
                )
                ControlChip("+") {
                    val next = (scale.value * 1.25f).coerceIn(1f, 6f)
                    scope.launch {
                        scale.animateTo(next, tween(160))
                        offset.animateTo(clamped(offset.value, next), tween(160))
                    }
                }
                ControlChip("Fit") {
                    scope.launch {
                        scale.animateTo(1f, tween(180))
                        offset.animateTo(Offset.Zero, tween(180))
                    }
                }
                ControlChip("⟲") {
                    rotation = (rotation + 90f) % 360f
                    scope.launch {
                        scale.snapTo(1f)
                        offset.snapTo(Offset.Zero)
                    }
                }
            }
        }
    }
}

/** One rounded action inside the photo pill. */
@Composable
private fun ControlChip(label: String, onClick: () -> Unit) {
    Box(
        Modifier
            .clip(RoundedCornerShape(16.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 7.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = Mvp.tileText, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
    }
}

/**
 * The bridge returns raw RGBA plus an 8-byte little-endian width/height prefix
 * (jni_bridge.cpp, decode_frame_at). BitmapFactory only understands *encoded*
 * image data, which is why decoding always failed before — rebuild the bitmap
 * from the pixels instead.
 */
/** Decrypts a stored picture and decodes it with the platform image decoder. */
fun decodeVaultImage(id: Long, maxSide: Int): Bitmap? {
    val encoded = if (RemoteVault.connected()) {
        RemoteVault.readAll(id, 40 * 1024 * 1024)
    } else {
        val size = CoreBridge.nativeVideoSize(id)
        if (size <= 0L || size > 40L * 1024 * 1024) return null
        val all = ByteArray(size.toInt())
        var offset = 0
        while (offset < all.size) {
            val want = minOf(1 shl 20, all.size - offset)
            val chunk = CoreBridge.nativeReadRange(id, offset.toLong(), want) ?: return null
            if (chunk.isEmpty()) break
            val count = minOf(chunk.size, all.size - offset)
            System.arraycopy(chunk, 0, all, offset, count)
            offset += count
        }
        if (offset <= 0) null else all.copyOf(offset)
    } ?: return null
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(encoded, 0, encoded.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
    var sample = 1
    val limit = maxSide.coerceAtLeast(1)
    while (bounds.outWidth / sample > limit || bounds.outHeight / sample > limit) {
        sample *= 2
    }
    val options = BitmapFactory.Options().apply { inSampleSize = sample }
    return BitmapFactory.decodeByteArray(encoded, 0, encoded.size, options)
}

internal fun rgbaToBitmap(bytes: ByteArray): Bitmap? {
    if (bytes.size < 8) return null
    fun u32(at: Int): Int =
        (bytes[at].toInt() and 0xFF) or
            ((bytes[at + 1].toInt() and 0xFF) shl 8) or
            ((bytes[at + 2].toInt() and 0xFF) shl 16) or
            ((bytes[at + 3].toInt() and 0xFF) shl 24)

    val width = u32(0)
    val height = u32(4)
    if (width <= 0 || height <= 0) return null
    val pixelBytes = width.toLong() * height.toLong() * 4L
    if (bytes.size - 8 < pixelBytes) return null
    return runCatching {
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(bytes, 8, pixelBytes.toInt()))
        bitmap
    }.getOrNull()
}
