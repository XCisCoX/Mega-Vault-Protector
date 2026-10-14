# Cloud TLS: legacy service replaced

The former `MegaVaultCloud` `/v1/login` service decrypted files on the server and required sending it the vault password. That is not end-to-end encrypted sync and does not implement issue #3. Do not deploy it for the new sync workflow.

Use the encrypted package relay described in [self-hosted-sync.md](self-hosted-sync.md). The new service accepts per-device pairing credentials, never a vault password or the separate shared package recovery secret. Clients keep ordinary local vaults for offline viewing and editing.

For deployment and certificate setup, see the new guide. For protocol details and security limits, see [sync-protocol.md](sync-protocol.md) and [sync-architecture.md](sync-architecture.md).

Existing PC/LAN sharing is a separate, intentionally trusted-PC workflow and remains available; it is not the encrypted relay.
