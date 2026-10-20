package org.megavideoprotect.app

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * In-app reader for plain text. The file stays encrypted; a capped copy is
 * decrypted into memory and shown in a monospace page.
 */
@Composable
fun TextScreen(
    videoId: Long,
    name: String,
    onBack: () -> Unit,
) {
    var body by remember(videoId) { mutableStateOf<String?>(null) }
    var failed by remember(videoId) { mutableStateOf(false) }
    LaunchedEffect(videoId) {
        body = null
        failed = false
        val loaded = withContext(Dispatchers.IO) { readVaultText(videoId) }
        if (loaded == null) failed = true else body = loaded
    }
    Column(Modifier.fillMaxSize().background(Mvp.window)) {
        Row(
            Modifier
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(horizontal = 8.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                painterResource(R.drawable.ic_ab_back),
                contentDescription = "Back",
                tint = Mvp.accent,
                modifier = Modifier.clickable(onClick = onBack).padding(8.dp).size(24.dp),
            )
            Text(
                name,
                color = Mvp.title,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 4.dp).weight(1f),
            )
        }
        when {
            failed -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Could not open this file.", color = Mvp.description, fontSize = 14.sp)
            }
            body == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = Mvp.accent, strokeWidth = 2.dp)
            }
            else -> Text(
                body.orEmpty(),
                color = Mvp.text,
                fontSize = 13.sp,
                lineHeight = 20.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 18.dp, vertical = 12.dp),
            )
        }
    }
}

private const val TEXT_CAP = 2 * 1024 * 1024

private fun readVaultText(id: Long): String? {
    val data = java.io.ByteArrayOutputStream()
    var offset = 0L
    var truncated = false
    while (data.size() < TEXT_CAP) {
        val room = TEXT_CAP - data.size()
        val want = minOf(256 * 1024, room)
        val chunk = runCatching { CoreBridge.nativeReadRange(id, offset, want) }.getOrNull() ?: return null
        if (chunk.isEmpty()) break
        val take = minOf(chunk.size, room)
        data.write(chunk, 0, take)
        offset += take
        if (take < chunk.size) {
            truncated = true
            break
        }
        if (chunk.size < want) break
    }
    var text = data.toByteArray().toString(Charsets.UTF_8)
    if (text.startsWith("\uFEFF")) text = text.substring(1)
    if (truncated) text += "\n\n— This file is longer than the reader shows. —"
    if (text.isEmpty()) text = "This file is empty."
    return text
}
