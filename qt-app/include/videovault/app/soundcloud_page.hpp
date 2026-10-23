#pragma once
#include "videovault/app/soundcloud_provider.hpp"
#include <QWidget>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <memory>
#include <deque>
#include <vector>
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QComboBox;
class QTimer;
namespace videovault::core { class Vault; }
namespace videovault::app {
class PluginHost;
class PluginAudioPlayer;
class RemoteStreamSource;
class RemoteStreamImport;
class SoundCloudHlsSource;
class SoundCloudTransport;
class MusicWindow;
class MusicResolver;
class SoundCloudPage final : public QWidget {
    Q_OBJECT
public:
    explicit SoundCloudPage(PluginHost*, QWidget* parent = nullptr);
    ~SoundCloudPage() override;
    void setVault(const std::shared_ptr<videovault::core::Vault>&);
    void searchFor(const QString&);
    void openLink(const QString&);
signals:
    void closed();
    void imported();
private:
    using Cancel = std::shared_ptr<std::atomic<bool>>;
    void reloadKeyState();
    void promptForKey();
    void runSearch();
    void browse(QUrl, QString, QString, bool append = false);
    void resolveLink(const QString&);
    void showObject(const QJsonObject&);
    void showTracks(const QVector<SoundCloudProvider::Track>&, const QString&, bool append = false);
    void playRow(QListWidgetItem*);
    void resolveTrack(const QString&);
    void stopPlayback();
    void playLocalCopy(qint64, const QString&);
    void updateTransport();
    void requestCover(const QString&, const QString&);
    void loadNextCover();
    void startImport();
    void cancelImport();
    void refreshImportButton();
    void request(QUrl, int, Cancel, std::function<void(QByteArray)>);
    QString apiKey() const;
    QUrl apiUrl(const QString&) const;
    PluginHost* host_;
    std::shared_ptr<videovault::core::Vault> vault_;
    std::shared_ptr<SoundCloudTransport> transport_;
    Cancel browseCancel_, playCancel_, importCancel_;
    QPushButton *back_, *search_, *keyButton_, *more_, *playButton_, *downloadButton_;
    QLabel *title_, *notice_, *nowPlaying_, *clock_, *status_;
    QLineEdit* query_;
    QComboBox* kind_;
    QListWidget* results_;
    QSlider* seek_;
    PluginAudioPlayer* player_;
    MusicWindow* localPlayer_{nullptr};
    std::unique_ptr<MusicResolver> resolver_;
    RemoteStreamSource* stream_{nullptr};
    std::unique_ptr<SoundCloudHlsSource> hls_;
    std::unique_ptr<RemoteStreamImport> import_;
    RemoteStreamSource* importSource_{nullptr};
    QTimer* importPoll_;
    QString playingId_, importedId_, context_, browseKind_;
    SoundCloudProvider::Track playing_;
    bool playingHls_{false};
    QUrl next_, currentBrowse_;
    struct Location { QUrl url; QString kind; QString context; };
    std::vector<Location> history_;
    std::deque<std::pair<QString, QString>> covers_;
    bool coverBusy_{false};
};
} // namespace videovault::app
