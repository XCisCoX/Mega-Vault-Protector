package org.megavideoprotect.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp

/** Process/session-only identity: never in a manifest, preferences, URL log or saved instance state. */
@Composable
internal fun ProviderIdentitySettings(session: PluginSession, onRetry: () -> Unit) {
    if (session.manifest.protocol != "soundcloud-tracks-v1") return
    Column(Modifier.fillMaxWidth().padding(12.dp)) {
        OutlinedTextField(value = session.clientId, onValueChange = session::updateClientId,
            label = { Text("Your SoundCloud API client ID · this session only") },
            visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false), singleLine = true, modifier = Modifier.fillMaxWidth())
        Text("Supply your own authorized API identity. No credentials are scraped. Cleared when the provider closes or vault locks.", color = Mvp.description)
        TextButton(onClick = onRetry, enabled = session.clientId.isNotBlank() && !session.catalogueBusy) { Text("Apply and retry") }
    }
}
