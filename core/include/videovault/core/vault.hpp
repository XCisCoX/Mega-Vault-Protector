#pragma once

#include "videovault/core/secret.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace videovault::core {

enum class VaultErrorCode {
    WrongPassword,
    DatabaseCorrupt,
    UnsupportedPackageVersion,
    AuthenticationFailed,
    PackageMissing,
    PackageModified,
    UnsupportedVideoFormat,
    ImportCancelled,
    DiskFull,
    PermissionDenied,
    CryptoFailure,
    DatabaseFailure,
    InvalidArgument,
    VaultAlreadyExists,
    VaultNotFound,
    MetadataCorrupt,
    UnsupportedVaultVersion
};

struct VaultError {
    VaultErrorCode code{VaultErrorCode::DatabaseFailure};
    std::string technical_detail;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T&& value) : storage_(std::move(value)) {}
    Result(const T& value) requires std::is_copy_constructible_v<T> : storage_(value) {}
    Result(VaultError error) : storage_(std::move(error)) {}

    explicit operator bool() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] bool has_value() const noexcept { return static_cast<bool>(*this); }

    T& value() & { return std::get<T>(storage_); }
    const T& value() const& { return std::get<T>(storage_); }
    T&& value() && { return std::get<T>(std::move(storage_)); }

    VaultError& error() & { return std::get<VaultError>(storage_); }
    const VaultError& error() const& { return std::get<VaultError>(storage_); }

private:
    std::variant<T, VaultError> storage_;
};

struct Argon2Parameters {
    std::uint32_t memory_kib{256U * 1024U};
    std::uint32_t iterations{3U};
    std::uint32_t parallelism{1U};
};

// Gallery entry for an imported video. package_sha256 is the SHA-256 of the
// complete encrypted package file and also encodes its storage path
// (vault-data/<h0h1>/<h2h3>/<64 lowercase hex>.vvp).
struct VideoInfo {
    std::int64_t id{0};
    std::string display_name;
    std::uint64_t original_size{0U};
    std::uint64_t package_size{0U};
    std::array<unsigned char, 32> package_sha256{};
    std::string package_relative_path;
    std::uint64_t imported_at{0U};
    // 0 is the library root. A video lives in one folder.
    std::int64_t folder_id{0};
    // Phase 7: tag names attached to this video (case-insensitively sorted).
    std::vector<std::string> tags;
};

// A virtual folder. parent_id 0 is the library root. Removing a folder
// deletes the files and folders inside it.
struct FolderInfo {
    std::int64_t id{0};
    std::int64_t parent_id{0};
    std::string name;
    std::uint64_t created_at{0U};
};

// Phase 7: a tag with the number of videos it is attached to.
struct TagInfo {
    std::int64_t id{0};
    std::string name;
    std::int64_t video_count{0};
};

// Phase 5: container metadata probed through FFmpeg.
struct MediaInfo {
    std::int64_t video_id{0};
    std::uint64_t duration_ms{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t rotation_degrees{0U};
    std::string codec_name;
    // Empty when the container has no tag. The gallery then uses the file name.
    std::string title;
    std::string artist;
};

// A decrypted JPEG thumbnail ready for display.
struct ThumbnailInfo {
    std::int64_t video_id{0};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::string mime;
    std::vector<unsigned char> bytes;
};

class Vault final {
public:
    Vault() noexcept;
    ~Vault();

    Vault(Vault&&) noexcept;
    Vault& operator=(Vault&&) noexcept;

    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    [[nodiscard]] static bool exists(const std::filesystem::path& root) noexcept;

    // True when this process could lock secret pages with mlock(). When false,
    // Secret buffers are still wiped on release but may be swappable; the UI
    // must not claim otherwise.
    [[nodiscard]] static bool secure_memory_available() noexcept;

    // Secure-memory entry points. Prefer these: the password bytes go straight
    // into a locked, self-wiping buffer and are never copied into a std::string.
    [[nodiscard]] static Result<Vault> create(
        const std::filesystem::path& root,
        const Secret& password,
        Argon2Parameters parameters = {});

    [[nodiscard]] static Result<Vault> open(
        const std::filesystem::path& root,
        const Secret& password);

    [[nodiscard]] static Result<bool> validate_password(
        const std::filesystem::path& root,
        const Secret& password);

    // Compatibility overloads for callers that already hold the password as
    // text. They copy it into a Secret internally, but the caller's own bytes
    // are outside the core's control (see REMAINING_RISKS.md).
    [[nodiscard]] static Result<Vault> create(
        const std::filesystem::path& root,
        std::string_view password,
        Argon2Parameters parameters = {});

    [[nodiscard]] static Result<Vault> open(
        const std::filesystem::path& root,
        std::string_view password);

    [[nodiscard]] static Result<bool> validate_password(
        const std::filesystem::path& root,
        std::string_view password);

    [[nodiscard]] bool is_unlocked() const noexcept;

    // Hard lock: revokes the session, closes the encrypted database, wipes the
    // master key and clears every capability handed out during the unlock.
    void lock() noexcept;

    // Increases on every completed lock(). An asynchronous result created under
    // an older generation belongs to a vault that no longer exists; callers
    // must discard it instead of reopening a closed view.
    [[nodiscard]] std::uint64_t lock_generation() const noexcept;

    [[nodiscard]] const std::filesystem::path& root_path() const noexcept;
    [[nodiscard]] Argon2Parameters kdf_parameters() const noexcept;

    // Phase 3: import and gallery.
    // Imports an opaque file as an encrypted package and records it in the
    // vault database. The same bytes are stored once: a second copy, even
    // under another name, returns the existing id and adds nothing.
    // `progress`, when given, receives a monotonic 0.0..1.0 fraction of the
    // file processed, called from the importing thread.
    // folder_id 0 stores the new video in the library root. An existing
    // copy of the same file keeps the folder it already has.
    // already_present, when given, is set true when the bytes were already
    // in the vault and false when a new item was stored.
    [[nodiscard]] Result<std::int64_t> import_file(
        const std::filesystem::path& source_path,
        const std::function<void(double)>& progress = {},
        std::int64_t folder_id = 0,
        bool* already_present = nullptr);

    // Phase 8: text records. Imports content supplied in memory instead
    // of a file on disk, so the app can author an item that lives in the library
    // exactly like an imported file: the same package format, folders, tags,
    // search, sharing and password-change behavior. The caller decides the
    // content format; the core stores it as opaque bytes. Identical content
    // returns the item already in the vault, exactly as import_file does.
    // `already_present`, when given, is set to true in that case so a caller can
    // tell the user instead of leaving them wondering where the new item went.
    // `display_name` follows the folder/file naming rule (1–120 characters).
    [[nodiscard]] Result<std::int64_t> import_bytes(
        std::span<const unsigned char> bytes,
        std::string_view display_name,
        std::int64_t folder_id = 0,
        bool* already_present = nullptr);

    // Replaces an existing item's content in place. A new package is sealed and
    // the row, its wrapped key and the content hash are updated in one
    // transaction, so the item keeps its id, folder, tags and shares. The
    // previous package file is removed after the commit. `display_name` renames
    // the item. Unknown ids return InvalidArgument.
    [[nodiscard]] Result<bool> replace_video_content(
        std::int64_t video_id,
        std::span<const unsigned char> bytes,
        std::string_view display_name);

    [[nodiscard]] Result<std::vector<VideoInfo>> list_videos() const;

    // Plaintext byte length for one id. Playback must never list/probe the
    // whole library just to implement AVSEEK_SIZE for the selected item.
    [[nodiscard]] Result<std::uint64_t> video_size(std::int64_t video_id) const;

    // Resolves the stored package file for a video and verifies it exists.
    [[nodiscard]] Result<std::filesystem::path> package_path(
        std::int64_t video_id) const;

    // Decrypts a video package fully into memory. Suitable for verification
    // and export of reasonably sized files; playback uses a streaming reader
    // in a later phase.
    [[nodiscard]] Result<std::vector<unsigned char>> read_video_bytes(
        std::int64_t video_id) const;

    // Reads up to `size` plaintext bytes starting at `offset` from a video
    // package (fewer at EOF; empty when offset is at or past the end). Every
    // touched chunk is authenticated before its bytes are returned.
    [[nodiscard]] Result<std::vector<unsigned char>> read_video_range(
        std::int64_t video_id,
        std::uint64_t offset,
        std::size_t size) const;

    // Phase 4: vault administration.
    // Removes a video's gallery row (cascading its wrapped key) and its
    // stored package file. Returns InvalidArgument for an unknown id.
    [[nodiscard]] Result<bool> remove_video(std::int64_t video_id);

    [[nodiscard]] Result<std::vector<FolderInfo>> list_folders() const;

    [[nodiscard]] Result<std::int64_t> create_folder(
        std::int64_t parent_id,
        std::string_view name);

    [[nodiscard]] Result<bool> rename_folder(
        std::int64_t folder_id,
        std::string_view name);

    // Deletes the folder, nested folders, and the encrypted packages inside.
    [[nodiscard]] Result<bool> remove_folder(std::int64_t folder_id);

    [[nodiscard]] Result<bool> move_folder(
        std::int64_t folder_id,
        std::int64_t new_parent_id);

    [[nodiscard]] Result<bool> rename_video(
        std::int64_t video_id,
        std::string_view name);

    [[nodiscard]] Result<bool> move_video(
        std::int64_t video_id,
        std::int64_t folder_id);

    // Re-keys the vault with a new password: new salt and Argon2id parameters,
    // SQLCipher rekey, a fresh sealed password verifier, and re-wrapped file
    // keys. The vault stays unlocked under the new credentials on success.
    // Both passwords are held in secure memory for the duration of the change
    // only; neither is retained afterwards.
    [[nodiscard]] Result<bool> change_password(
        const Secret& current_password,
        const Secret& new_password,
        Argon2Parameters parameters = {});

    [[nodiscard]] Result<bool> change_password(
        std::string_view current_password,
        std::string_view new_password,
        Argon2Parameters parameters = {});

    // Returns the stored thumbnail, decrypting it for display.
    [[nodiscard]] Result<ThumbnailInfo> thumbnail(
        std::int64_t video_id) const;

    // Thumbnail for the gallery and the player. For a song, a stored music-note
    // placeholder is replaced the first time a real embedded cover can be read,
    // and that cover stays in the vault.
    [[nodiscard]] Result<ThumbnailInfo> display_thumbnail(
        std::int64_t video_id,
        std::uint32_t max_dimension = 320U) const;

    // Phase 5: media metadata and encrypted thumbnails.
    // Probes an imported video's container through FFmpeg (streaming over the
    // encrypted package; nothing is written to disk).
    [[nodiscard]] Result<MediaInfo> media_info(std::int64_t video_id) const;

    // Decodes a representative frame, scales it to fit `max_dimension`, and
    // stores the JPEG encrypted in the vault database. Returns the plaintext
    // thumbnail for display. A PDF is skipped here; the library renders one
    // page and stores it with store_thumbnail.
    [[nodiscard]] Result<ThumbnailInfo> generate_thumbnail(
        std::int64_t video_id,
        std::uint32_t max_dimension = 320U) const;

    // Encrypts a JPEG the caller already rendered and keeps it in the
    // thumbnails table, replacing any older picture for this item.
    [[nodiscard]] Result<ThumbnailInfo> store_thumbnail(
        std::int64_t video_id,
        const std::vector<unsigned char>& jpeg,
        std::uint32_t width,
        std::uint32_t height) const;

    // Phase 7: tags. Tag names are trimmed, non-empty, at most 64 bytes, and
    // matched case-insensitively (re-adding "family" to "Family" is a no-op
    // that returns the same tag id). Attaching a tag creates it on demand.
    [[nodiscard]] Result<std::int64_t> add_tag(
        std::int64_t video_id,
        std::string_view tag_name);

    // Creates a tag without attaching it to any video (normalized like
    // add_tag; reuses an existing case-insensitive match).
    [[nodiscard]] Result<std::int64_t> create_tag(std::string_view tag_name);

    [[nodiscard]] Result<bool> remove_tag(
        std::int64_t video_id,
        std::int64_t tag_id);

    [[nodiscard]] Result<std::vector<TagInfo>> tags_for_video(
        std::int64_t video_id) const;

    // All tags with their video counts, sorted case-insensitively.
    [[nodiscard]] Result<std::vector<TagInfo>> list_tags() const;

    // Renames a tag (normalized like add_tag; case-insensitively unique).
    // Fails with InvalidArgument for an unknown tag or a name collision.
    [[nodiscard]] Result<std::int64_t> rename_tag(
        std::int64_t tag_id,
        std::string_view new_name);

    // Deletes a tag and detaches it from every video.
    [[nodiscard]] Result<bool> delete_tag(std::int64_t tag_id);

    // One file inside a vault package. relative_path uses '/' separators and
    // is relative to the package root ("Holiday/clip.mp4").
    struct PackageItem {
        std::int64_t video_id{0};
        std::string relative_path;
    };

    // Writes a password-protected vault package. The file starts with the
    // MVPK marker and is encrypted with its own password. It is not a RAR,
    // ZIP, or 7-Zip archive, so archive tools cannot open it.
    // `directories` are folder paths to keep even when they hold no files.
    // `progress` receives 0.0..1.0 from the calling thread.
    [[nodiscard]] Result<bool> export_package(
        const std::filesystem::path& destination,
        const Secret& password,
        const std::vector<PackageItem>& files,
        const std::vector<std::string>& directories,
        bool compress,
        const std::function<void(double)>& progress = {});

    [[nodiscard]] Result<bool> export_package(
        const std::filesystem::path& destination,
        std::string_view password,
        const std::vector<PackageItem>& files,
        const std::vector<std::string>& directories,
        bool compress,
        const std::function<void(double)>& progress = {});

    // Reads a vault package and copies its files into `folder_id` (0 is the
    // library root), recreating folders. The password is the one chosen when
    // the package was written, not the vault password.
    // `on_file` / `on_folder` run for items that were newly stored.
    [[nodiscard]] Result<int> import_package(
        const std::filesystem::path& source,
        const Secret& password,
        std::int64_t folder_id,
        const std::function<void(double)>& progress = {},
        const std::function<void(std::int64_t id, const std::string& name, std::uint64_t size, std::int64_t parent)>& on_file = {},
        const std::function<void(std::int64_t id, std::int64_t parent, const std::string& name)>& on_folder = {});

    [[nodiscard]] Result<int> import_package(
        const std::filesystem::path& source,
        std::string_view password,
        std::int64_t folder_id,
        const std::function<void(double)>& progress = {},
        const std::function<void(std::int64_t id, const std::string& name, std::uint64_t size, std::int64_t parent)>& on_file = {},
        const std::function<void(std::int64_t id, std::int64_t parent, const std::string& name)>& on_folder = {});

    // Restores a video to plaintext: streams the decrypted bytes to
    // `target_dir / <original name>` in chunks (no whole-file buffer) and
    // returns the written path. Overwrites an existing file of the same name.
    // `progress`, when given, receives a monotonic 0.0..1.0 fraction of the
    // package processed, called from the restoring thread.
    [[nodiscard]] Result<std::filesystem::path> restore_video(
        std::int64_t video_id,
        const std::filesystem::path& target_dir,
        const std::function<void(double)>& progress = {});

private:
    class Impl;
    explicit Vault(std::filesystem::path root, std::unique_ptr<Impl> impl) noexcept;

    // Guards impl_ and all operations on it. Held by unique_ptr so the mutex
    // address is stable across Vault moves. Vault methods are safe to call
    // from a worker thread while the owner may lock() concurrently.
    std::unique_ptr<std::mutex> mutex_;

    // Lock-free mirror of "impl_ != nullptr" so UI event paths (e.g. an
    // application-level event filter) can ask whether the vault is unlocked
    // without blocking on mutex_ — which a worker import holds for the whole
    // duration of each file. Blocking there would freeze the UI thread.
    std::atomic<bool> unlocked_{false};

    // Incremented by every lock() so asynchronous callers can tell that the
    // vault they were reading belongs to a session that has ended.
    std::atomic<std::uint64_t> lock_generation_{0};

    std::filesystem::path root_;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view user_message(VaultErrorCode code) noexcept;

} // namespace videovault::core
