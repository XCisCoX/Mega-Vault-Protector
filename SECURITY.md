# Security Policy

Mega Vault Protect stores private files in encrypted vaults on Windows, Linux, and Android. Reports about unauthorized access, secret disclosure, data integrity, and unsafe file handling are welcome.

## Supported versions

The project does not currently publish a version-specific security support schedule or a long-term support policy.

Please check whether a problem affects the latest published release and, when practical, the current default branch. Report vulnerabilities in older versions too, especially if newer versions may also be affected. Maintainers determine which versions need fixes during triage; fixes and backports are not guaranteed for every release.

Development builds may contain incomplete changes. Do not use your only copy of important data to reproduce a problem.

## Reporting a vulnerability

Please avoid disclosing an unpatched vulnerability in a public issue, discussion, or pull request.

1. Open the repository's [Security page](https://github.com/XCisCoX/Mega-Vault-Protector/security).
2. If **Report a vulnerability** is available, use it to submit a private report.
3. If private reporting is unavailable, check the maintainer's [GitHub profile](https://github.com/XCisCoX) for a published private contact method.
4. If neither route is available, open a minimal [issue](https://github.com/XCisCoX/Mega-Vault-Protector/issues/new) requesting a private security-reporting channel. Do not include vulnerability details, an exploit, or sensitive evidence in that issue.

Private vulnerability reporting availability depends on the repository's GitHub settings. This policy does not imply that it is enabled or that a dedicated security email exists.

For ordinary bugs that do not have a security impact, follow [CONTRIBUTING.md](CONTRIBUTING.md).

### What to include

A useful private report includes:

- The affected release or commit, operating system, and architecture.
- The affected component or task.
- A description of the security impact and any prerequisites.
- Reproduction steps using a disposable vault and synthetic files.
- Expected and observed behavior.
- A minimal proof of concept, redacted logs, or a suggested fix, if available.
- Whether the report has been shared elsewhere.

Incomplete reports are welcome. State uncertainties rather than making unsupported claims. Screenshots and recordings are optional.

Never include real vault passwords, encryption keys, private documents, personal records, or active sharing-session QR codes. If a reproduction needs a password or key, generate one exclusively for the test vault and clearly identify it as test data.

## Areas of interest

Examples of security-sensitive behavior include:

- Vault creation, unlocking, password derivation, and password changes.
- Encryption, key wrapping, authenticated package reads, and database integrity.
- Plaintext exposure through logs, caches, temporary files, thumbnails, clipboard operations, or memory handling.
- Malformed-file processing, decoding, and native-library or JNI boundaries.
- Sharing-session authentication, TLS certificate verification, and remote permissions.
- Unauthorized modification or deletion of another device's files.
- Build, dependency, signing, and release-pipeline integrity.

These examples guide reporting; they do not establish that these components have been audited or are free of vulnerabilities. The README describes the current security model and platform-specific behavior, including the Android PDF renderer's plaintext cache requirement. Report unexpected exposure or behavior that exceeds those documented requirements.

### Existing plaintext note staging

Desktop note saves currently stage the UTF-8 note body in `<system temporary directory>/mvp-notes/<sanitized note filename>` before importing it into the encrypted vault. `MainWindow::saveOpenText` in `qt-app/src/main_window.cpp` obtains that root with `std::filesystem::temp_directory_path()` and uses `noteFileName` to sanitize the title. This is a file-import staging step, not an in-memory-only save or an encrypted cache.

The file exists during writing and import. The code attempts to remove it after import returns (whether successful or an error result), and after a short write. Removal errors are not checked, and an exception or process crash before removal can bypass cleanup. Ordinary deletion is not secure erasure. The plaintext body and filename may therefore be exposed to software or users with access to the temporary location, and residual data may remain. If checking for leftovers, close the application first, preserve needed data, and inspect the `mvp-notes` directory under the system temporary directory; do not assume it is empty after a failed save. This existing exception does not authorize new plaintext staging elsewhere.

### Cloud server trust boundary

`MegaVaultCloud` is a trusted vault endpoint, not a server that stores only opaque ciphertext. The first successful cloud sign-in unlocks the vault on the server; the server retains the vault password in a `std::string` in process memory while it runs to check later sign-ins. Session expiration does not lock that server-side vault or discard the retained password. Trust the host and its administrators with the password, keys, and decrypted content, and protect process memory and crash dumps accordingly.

Authenticated cloud clients can delete files and folders from the shared vault. LAN peer-sharing read-only permissions do not apply to cloud deletion. Keep independent backups and see [docs/cloud-ssl.md](docs/cloud-ssl.md) for TLS, fingerprint verification, and safe proxy configuration.

Cloud clients also keep decrypted local copies: the desktop writes them under Qt's application cache location in `cloud/`, while Android uses `cacheDir/cloud/`. Partial downloads use a `.part` suffix. These files are not encrypted vault packages; the downloaders do not delete each completed file when its viewer closes. Desktop cloud opening hands the cached file to the system's associated application, which may create additional copies or history. Do not extend the local-vault in-memory playback claim to cloud caches.

## Responsible testing

Test only on systems, devices, vaults, and networks you own or have explicit permission to assess. Use sample data and backups. Avoid disrupting other users, accessing their information, or testing against another person's shared vault without permission.

When you demonstrate a problem, collect only the evidence needed to reproduce it. Do not retain or distribute other people's private data. Coordinate further testing with the maintainer when it could cause disruption or data loss.

## How reports are handled

Maintainers aim to review private reports, reproduce the problem, assess the affected versions and impact, and coordinate a fix. They may request additional information or suggest a safe mitigation.

Response and resolution times depend on maintainer availability and the complexity of the issue. There is no guaranteed acknowledgement, fix, or release deadline. Any estimate should be communicated through the private reporting channel and updated when circumstances change.

When a fix is available, reporters may be invited to verify it. Maintainers and reporters should coordinate public disclosure so users have an opportunity to obtain a fix or mitigation. A security advisory or release note may explain affected versions, impact, and recommended actions without revealing private report contents unnecessarily.

Reports will be handled as privately as reasonably possible, but absolute confidentiality cannot be guaranteed. Necessary information may be shared with people helping resolve the issue or with affected dependency maintainers. Reporter credit should be agreed with the reporter before publication; anonymous reporting is welcome.

## Security updates and mitigations

Check [GitHub Releases](https://github.com/XCisCoX/Mega-Vault-Protector/releases) and the repository's [Security page](https://github.com/XCisCoX/Mega-Vault-Protector/security) for published fixes and advisories. Follow the instructions accompanying an update, especially when storage compatibility or migration is involved.

Keep backups before testing or installing changes that affect vault storage. Current published Android release APKs use the same project release-signing key and can update an installation signed with that key without uninstalling. Older CI APKs from before October 14, 2026 used changing debug keys; switching from one of those installations to the release key can require a one-time uninstall and reinstall. Local or fork debug builds may also have a different signer. **Before any uninstall, preserve and verify a separate copy of the vault and any data you need: uninstalling can remove the application's local vault.** Do not uninstall as a routine troubleshooting or update step. See [Android signing](docs/ANDROID-BUILD.md#signing-android-releases) for the signing workflow.

If a vulnerability affects sharing, disabling the affected sharing session may reduce exposure while a fix is prepared. Specific mitigations should be confirmed for the reported issue rather than assumed to solve every problem.

## Policy maintenance

The project maintainer is responsible for keeping this policy, reporting routes, and security-update guidance current. Update this document when a dedicated reporting contact, version-support schedule, or security-response process is established.
