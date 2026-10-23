#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QWidget>
#include <atomic>
#include "videovault/core/provenance.hpp"
#include <functional>
#include <memory>
class QNetworkAccessManager;
class QNetworkReply;
class QMediaPlayer;
class QBuffer;
class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QSlider;
namespace videovault::core {
class Vault;
}
namespace videovault::app {
class PluginAudioPlayer;
class ByteSource;
class RemoteStreamSource;
class RemoteStreamImport;
struct PluginCommand { QString id, title, inputLabel; };
struct PluginSetting { QString id, title; bool defaultValue=false; };
struct PluginManifest {
    QString id, name, version, description, sourceNotice;
    QString protocol, script;
    QVector<PluginCommand> commands;
    QVector<PluginSetting> settings;
    bool builtin = false;
    QUrl baseUrl;
    QStringList hosts;
    static bool parse(const QByteArray &, PluginManifest *, QString *error);
    bool allows(const QUrl &) const;
};
QString pluginArtistNames(const QJsonObject &);
// True for every music-provider protocol the host implements. Keeping the test
// in one place is what lets a second provider (SoundCloud) reuse the host's
// network, playback and import pipeline without touching each call site.
bool pluginProtocolIsMusic(const QString &protocol);
bool pluginFlac(const QByteArray &);
class PluginHost final : public QObject {
    Q_OBJECT
  public:
    explicit PluginHost(QString directory = {}, QObject *parent = nullptr);
    const QVector<PluginManifest> &manifests() const { return manifests_; }
    bool install(const QByteArray &, QString *error);
    bool remove(const QString& id, QString* error);
    bool enabled(const QString &) const;
    bool setEnabled(const QString &, bool);
    QString settingKey(const QString &id, const QString &vaultIdentity = {}) const;
    QWidget *manager(const std::shared_ptr<core::Vault> &, QWidget *parent = nullptr);
  signals:
    void changed(const QString& pluginId);
    void openRequested(const QString& pluginId);

  private:
    QString directory_;
    QVector<PluginManifest> manifests_;
};
class MusicPluginPage final : public QWidget {
    Q_OBJECT
  public:
    explicit MusicPluginPage(PluginHost *, QWidget *parent = nullptr);
    ~MusicPluginPage() override;
    void setVault(std::shared_ptr<core::Vault>);
    // Embedded mode drops the page header (back button and title) so the
    // surface can live inside the library page, above the folder's own items.
    void setEmbedded(bool embedded);
    // The library's search offers "Search <provider> for ..." and hands the
    // term here, so the provider opens already showing those results.
    void searchFor(const QString &term);
    // Hands one catalogue entry (a song or an album) to the page's own player,
    // so the library can drive playback without a second playback path.
    void activateItem(const QJsonObject &item);
    // Asks the provider for one release's track list (album view). The answer
    // arrives as albumTracksReady; browsing itself stays in the library.
    void requestAlbumTracks(const QString &releaseId);
    void stopAndClear();
  signals:
    void closed();
    void imported();
    // A search answer, so a library folder can list what the provider has.
    void catalogueReady(const QString &term, const QJsonObject &results);
    // A release's tracks, for the library's album folder.
    void albumTracksReady(const QString &releaseId, const QJsonArray &tracks);
    void albumTracksFailed(const QString &releaseId, const QString &message);
    // The search could not be answered (offline, blocked, provider error), so
    // the folder can say why instead of waiting forever.
    void catalogueFailed(const QString &term, const QString &message);

  private:
    void refreshProviders();
    void search();
    void activate(QListWidgetItem *);
    void showResults(const QJsonObject &, bool detail = false);
    void applyFilter();
    void request(QUrl, qint64, std::function<void(QByteArray)>, int redirects = 0);
    // Shared tail for both network backends: applies limits, redirects and
    // error reporting, then hands the body to the caller.
    void completeRequest(const QUrl &, QByteArray, int status, const QUrl &redirect,
                         const QString &error, qint64 limit,
                         const std::function<void(QByteArray)> &, int redirects, quint64 generation);
    void thumbnailNext();
    void saveAudio();
    QString vaultIdentity() const;
    void clearPlayback(bool clearCatalogue);
    PluginManifest manifest() const;
    PluginHost *host_;
    std::shared_ptr<core::Vault> vault_, pinnedVault_;
    QNetworkAccessManager *network_;
    PluginAudioPlayer *player_;
    std::unique_ptr<ByteSource> playbackSource_;
    std::unique_ptr<RemoteStreamImport> streamImport_;
    core::TrackProvenance pinnedProvenance_;
    QComboBox *providers_;
    QLineEdit *query_;
    QListWidget *results_;
    QLabel *notice_;
    QLabel *status_;
    QLabel *nowPlaying_;
    QPushButton *play_;
    QPushButton *download_;
    QPushButton *back_{nullptr};
    QLabel *title_{nullptr};
    QSlider *seek_;
    bool embedded_{false};
    // Bumped whenever the provider answers; a search that never answers is
    // reported by its own deadline instead of leaving the row at "Waiting".
    int searchToken_{0};
    int filter_ = 0;
    quint64 generation_ = 0;
    bool autoSave_ = false, saved_ = false, importBusy_ = false;
    bool autoSaveAttempted_ = false;
    qint64 pinnedFolder_ = 0;
    QString pinnedPlugin_, pinnedName_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    QSet<QNetworkReply *> replies_;
    QVector<QPair<QUrl, int>> artwork_;
    bool thumbnailBusy_ = false;
    QHash<QNetworkReply *, std::shared_ptr<QByteArray>> requestBuffers_;
};
// Progressive row downloads: bounded network/import queues, encrypted staging
// only, provenance resolution before any network request, two jobs at a time.
class PluginTrackDownloader final : public QObject {
    Q_OBJECT
  public:
    // Row states, mirrored by the library's row buttons.
    enum State { Idle = 0, Queued = 1, Fetching = 2, Encrypting = 3, Saved = 4, Failed = 5 };
    explicit PluginTrackDownloader(PluginHost *, QObject *parent = nullptr);
    ~PluginTrackDownloader() override;
    void setVault(std::shared_ptr<core::Vault>);
    // Queues one catalogue track (needs _id/trackId and a title).
    void enqueue(const QJsonObject &track);
    int state(const QString &trackId) const { return states_.value(trackId, Idle); }
    // Tracks currently fetching or encrypting, and rows waiting for a slot.
    int active() const { return active_; }
    int waiting() const { return queue_.size(); }
    // Test seam: replaces only the HTTP fetch. The FLAC check, the encrypted
    // import and every state transition stay the production code path.
    using Fetcher = std::function<void(const QString &trackId,
        std::function<void(qint64, qint64)> progress,
        std::function<void(QByteArray, QString)> done)>;
    void setFetcher(Fetcher fetcher) { fetcher_ = std::move(fetcher); }
  signals:
    // state is a State; percent is -1 while the size is unknown.
    void stateChanged(const QString &trackId, int state, int percent);
    void finished(const QString &trackId, bool ok, const QString &message);

  private:
    struct Job {
        QString id;
        QString name;
        QJsonObject item;
        PluginManifest provider;
    };
    void pump();
    void start(const Job &);
    PluginManifest manifest() const;
    PluginHost *host_;
    QNetworkAccessManager *network_;
    std::shared_ptr<core::Vault> vault_;
    QVector<Job> queue_;
    QHash<QString, int> states_;
    QHash<QString, int> percent_;
    Fetcher fetcher_;
    struct ActiveDownload;
    QVector<std::shared_ptr<ActiveDownload>> downloads_;
    quint64 generation_{0};
    int active_{0};
};
} // namespace videovault::app
