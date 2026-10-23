#pragma once

#include <QWidget>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

class MusicWindow;

// The library's music page: every audio item in the vault, listed like a music
// app (title, artist, size) instead of a folder of files. Playing a row hands a
// queue to the borrowed, MainWindow-owned persistent footer.
class MusicLibraryPage final : public QWidget {
    Q_OBJECT

public:
    explicit MusicLibraryPage(QWidget* parent = nullptr);
    void setVault(const std::shared_ptr<videovault::core::Vault>& vault);
    void refresh();
    // Borrowed: never reparents, embeds, closes or deletes the central footer.
    void setPlayer(MusicWindow* player);

signals:
    void closed();

private:
    struct Track {
        std::int64_t id{0};
        QString title;
        QString artist;
        qint64 size{0};
        // Import time, so "Recently added" is a real ordering rather than an
        // alphabetical accident.
        qint64 added{0};
        // Set when the item came from a music provider: the provider's display
        // name, its album and the fact that this is the offline copy of a remote
        // track. Empty for items imported as plain files.
        QString provider;
        QString album;
        QString remoteId;
    };

    // The page is sectioned the way a music app is: everything, by artist, by
    // import time, and the provider-backed offline copies.
    enum Section { SongsSection = 0, ArtistsSection = 1, RecentSection = 2,
        OfflineSection = 3, AlbumsSection = 4, PlaylistsSection = 5 };

    struct Playlist {
        std::int64_t id{0};
        QString name;
        std::vector<std::int64_t> trackIds;
    };
    // Playlist editing works the way the core API does: read the playlist, submit
    // the complete new track sequence. Every mutation reloads and says what
    // happened, so a refused edit is never silent.
    void createPlaylist(std::int64_t initialTrack = 0);
    void renamePlaylist(Playlist playlist);
    void deletePlaylist(Playlist playlist);
    void changePlaylistTracks(std::int64_t id, std::vector<std::int64_t> ids);
    void addPlaylistTracks();
    void editSelectedTrack(int direction); // 0 removes, -1/+1 moves in stored order.
    void updateActions();
    void showError(const QString& message);
    void runPlaylistEdit(std::function<QString()> work);
    [[nodiscard]] const Playlist* playlistById(std::int64_t id) const;
    void openPlaylist(std::int64_t id);
    void rebuild();
    void applyFilter(const QString& text);
    void playRow(int row);
    void ensurePlayer();
    // Album art is decrypted one row at a time so a large library never pays for
    // N package opens just to paint a list.
    void startCoverLoads();
    void loadNextCover();

    std::shared_ptr<videovault::core::Vault> vault_;
    std::vector<Track> tracks_;
    std::vector<Track> shown_;
    // Encrypted playlists, read from the vault on every rebuild.
    std::vector<Playlist> playlists_;
    // 0 lists the playlists themselves; otherwise the list shows one playlist's
    // tracks in their stored order.
    std::int64_t openPlaylistId_{0};

    QLineEdit* search_{nullptr};
    QListWidget* list_{nullptr};
    QLabel* count_{nullptr};
    QLabel* empty_{nullptr};
    QPushButton* playAll_{nullptr};
    QPushButton* newPlaylist_{nullptr};
    QPushButton* openPlaylist_{nullptr};
    QPushButton* renamePlaylist_{nullptr};
    QPushButton* deletePlaylist_{nullptr};
    QPushButton* addTracks_{nullptr};
    QPushButton* removeTrack_{nullptr};
    QPushButton* moveUp_{nullptr};
    QPushButton* moveDown_{nullptr};
    QLabel* playlistTitle_{nullptr};
    QLabel* status_{nullptr};
    QComboBox* section_{nullptr};
    QComboBox* group_{nullptr};
    QPushButton* playlistBack_{nullptr};
    void rebuildGroups();
    QTimer* coverPump_{nullptr};
    int coverCursor_{0};
    int coverGeneration_{0};
    bool coverBusy_{false};
    int currentSection_{SongsSection};
    QPointer<MusicWindow> player_;
    bool playlistEditBusy_{false};
    std::uint64_t vaultGeneration_{0};
};

} // namespace videovault::app
