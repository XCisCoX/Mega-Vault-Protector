package org.megavideoprotect.app

import java.net.IDN
import java.net.InetAddress

enum class ProxyKind { HTTP, SOCKS5 }

/** Intentionally not a data class: generated toString must never expose credentials. */
class ProxyProfile(
    val id: String,
    val kind: ProxyKind,
    val host: String,
    val port: Int,
    val username: String = "",
    val password: String = "",
) {
    init {
        require(id.isNotBlank()) { "Missing proxy ID" }
        require(port in 1..65535) { "Port must be between 1 and 65535" }
        require(host.isNotBlank() && host == host.trim() && host.length <= 253 &&
            host.none { it.isWhitespace() || it in "/\\@?#%[]" }) { "Enter a server hostname or IP, without a scheme or port" }
        if (':' in host) {
            require(host.all { it in "0123456789abcdefABCDEF:." } && InetAddress.getByName(host).address.size == 16) { "Invalid IPv6 server" }
        } else {
            val ascii = IDN.toASCII(host, IDN.USE_STD3_ASCII_RULES)
            require(ascii.split('.').all { it.isNotEmpty() && it.length <= 63 && !it.startsWith('-') && !it.endsWith('-') }) { "Invalid server hostname" }
            if (ascii.all { it.isDigit() || it == '.' }) {
                require(ascii.split('.').size == 4 && ascii.split('.').all { it.toIntOrNull() in 0..255 }) { "Invalid IPv4 server" }
            }
        }
        require(':' !in username && username.none { it == '\r' || it == '\n' } && password.none { it == '\r' || it == '\n' }) { "Invalid proxy credentials" }
        require(password.isEmpty() || username.isNotEmpty()) { "Enter a username for this password" }
        if (kind == ProxyKind.SOCKS5) require(username.toByteArray().size <= 255 && password.toByteArray().size <= 255) { "SOCKS5 credentials exceed 255 bytes" }
    }
    override fun toString() = "ProxyProfile($kind, $host:$port)"
}
