# Cloud TLS

Create the vault in Mega Vault Protect first. `cloud/config.json` then holds only that folder, the port, the bind address, and `plain`:

```
{
  "vault": "D:/vaults/my-vault",
  "port": 8766,
  "bind": "0.0.0.0",
  "plain": false
}
```

No password and no user goes in the config. Everyone who signs in types the vault password in their own app, and that is what opens the vault.

## Built-in certificate

Leave `"plain": false`. When `MegaVaultCloud` starts it prints a public key:

```
Public key:
9f2c...e41b
```

That value is the SHA-256 fingerprint of the certificate the server generated for this run. Paste it into Cloud login on the phone and on the PC, next to the address `host:port`, and then type the vault password. The password only goes out after the certificate matches, and the vault opens with the first sign-in that presents it.

Each launch creates a new certificate, so copy the new public key after every restart. The printed vault path tells you which vault the key belongs to.

## Your own certificate

To keep one certificate, or to use a certificate from a real authority, terminate TLS in a proxy and let the cloud process speak plain HTTP on the machine only.

1. In `cloud/config.json` set `"plain": true` and `"bind": "127.0.0.1"`.
2. Start `MegaVaultCloud`. It listens on port 8766 without TLS.
3. Point Caddy (or nginx) at `127.0.0.1:8766` and give Caddy the certificate.

Caddy example:

```
your.example.com {
    reverse_proxy 127.0.0.1:8766
}
```

4. Fingerprint the certificate clients will see:

```
openssl x509 -in cert.pem -outform der | openssl dgst -sha256
```

5. Cloud login uses `your.example.com:443`, that public key, and the vault password.

Do not set `"plain": true` on a port that other machines can reach. Plain mode has no encryption of its own.

## What a signed-in device sees

Every client shows the vault the way it shows a local one: folders, media, playback, and a green tick on the files it has synced. The cloud extras are who else is signed in, the sync speed (slow, normal, fast) and that a delete lands in the shared vault.
