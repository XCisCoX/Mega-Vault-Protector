#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QWidget>
#include <atomic>
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
    void stopAndClear();
  signals:
    void closed();
    void imported();

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
    QBuffer *buffer_;
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
} // namespace videovault::app
