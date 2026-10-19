# Cloud TLS: the legacy service is replaced

The former `MegaVaultCloud` `/v1/login` service decrypted files on the server
and required the vault password to be sent to it. That is not end-to-end
encrypted sync and does not implement this design. Do not deploy it.

The relay described in [self-hosted-sync.md](self-hosted-sync.md) has no login,
no password field and no server-side decryption. It authenticates one thing —
the library key, presented as a bearer token — and stores only that key's
SHA-256.

TLS is still mandatory, because the key travels in a request header on every
call:

- **Public deployment.** Caddy obtains an ACME certificate for the public
  hostname and terminates TLS; the relay is never published and only Caddy
  holds a host port. See [../sync_server/deployment/README.md](../sync_server/deployment/README.md).
- **The Caddy-to-relay hop.** The relay refuses a plaintext bind on a
  non-loopback address, so even that internal hop is TLS, using a self-signed
  pair that lives in `deployment/tls/` and that no client ever sees.
- **Direct TLS.** `serve --cert … --key …` terminates TLS in the server itself
  for a deployment without a proxy. Keep the private key outside this
  repository.

On the device, use a URL the device already trusts. The setup is a URL plus the
library key: there is no certificate pin to maintain, so a private CA must be
installed in the device's trust store rather than worked around by disabling
verification. Never point a client at `http://` (loopback development is the
only exception, and then only on the same host), and never disable certificate
checks to make a connection work.

Existing PC/LAN sharing is a separate, intentionally trusted-PC workflow and
remains available; it is not the encrypted relay and has nothing to do with
this certificate setup.
