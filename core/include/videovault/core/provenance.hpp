#pragma once

#include <cstdint>
#include <string>

namespace videovault::core {

// Provenance recorded for a library item that came from a music provider.
//
// It is stored inside the encrypted vault database (schema migration 7), so it
// inherits the vault's protection model: no provider identity, source URL or
// listening context is written to a plaintext file, to the registry, or to
// QSettings. Rows are scoped to one vault, so two vaults opened on the same
// machine never share a provider catalogue.
//
// `provider_id` + `provider_track_id` are the identity a provider track is
// resolved by: a vault item with the same pair is the offline counterpart of
// that track and playback uses it instead of the network. File names are never
// used as identity.
struct TrackProvenance {
    // Filled by the vault when the row is read back.
    std::int64_t video_id{0};
    // Stable provider key, e.g. "org.megavault.monochrome".
    std::string provider_id;
    // The provider's own track id, kept as text (never round-tripped through a
    // floating-point number).
    std::string provider_track_id;
    // Canonical source URL, for the user's reference and for re-resolution.
    std::string source_url;
    std::string title;
    std::string artist;
    std::string album;
    std::uint64_t duration_ms{0U};
    // Container/codec the bytes are stored as, e.g. "flac".
    std::string codec;
    // Provider-reported quality when it reports one ("lossless", "128 kbps").
    std::string quality;
    // Lower-case hex SHA-256 of the artwork bytes the provider supplied, empty
    // when the cover is unknown. The artwork itself is never stored here.
    std::string artwork_sha256;
};

} // namespace videovault::core
