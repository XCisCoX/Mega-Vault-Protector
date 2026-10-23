#include "videovault/app/soundcloud_page.hpp"
#include "videovault/app/soundcloud_transport.hpp"
#include "videovault/app/soundcloud_hls_source.hpp"
#include "videovault/app/music_resolver.hpp"
#include "videovault/app/music_window.hpp"
#include "videovault/app/plugin_audio.hpp"
#include "videovault/app/plugin_host.hpp"
#include "videovault/app/remote_stream_source.hpp"
#include "videovault/core/vault.hpp"
#include <QComboBox>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QHBoxLayout>
#include <QImageReader>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <algorithm>

namespace videovault::app {
namespace {
QString timeText(qint64 ms) {
    const auto seconds = std::max<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
QString pathId(const QString& id) { return QString::fromUtf8(QUrl::toPercentEncoding(id)); }
RemoteStreamSource::Limits sourceLimits() {
    RemoteStreamSource::Limits limits; limits.buffer_bytes = 4 << 20;
    limits.allowed_hosts = QStringList{QStringLiteral("*.sndcdn.com"), QStringLiteral("api-v2.soundcloud.com"), QStringLiteral("api.soundcloud.com")};
    return limits;
}
}
SoundCloudPage::SoundCloudPage(PluginHost* host, QWidget* parent)
    : QWidget(parent), host_(host), transport_(std::make_shared<SoundCloudTransport>()) {
    setObjectName(QStringLiteral("soundCloudPage"));
    browseCancel_ = std::make_shared<std::atomic<bool>>(false);
    playCancel_ = std::make_shared<std::atomic<bool>>(false);
    importCancel_ = std::make_shared<std::atomic<bool>>(false);
    auto* column = new QVBoxLayout(this); column->setContentsMargins(16, 12, 16, 12);
    auto* header = new QHBoxLayout;
    back_ = new QPushButton(tr("Back"), this); header->addWidget(back_);
    title_ = new QLabel(tr("SoundCloud"), this); title_->setTextFormat(Qt::PlainText); header->addWidget(title_);
    kind_ = new QComboBox(this); kind_->setObjectName("soundCloudSearchKind");
    kind_->addItem(tr("Tracks"), "tracks"); kind_->addItem(tr("Artists"), "users"); kind_->addItem(tr("Playlists"), "playlists"); header->addWidget(kind_);
    query_ = new QLineEdit(this); query_->setObjectName("soundCloudQuery");
    query_->setPlaceholderText(tr("Search or paste a SoundCloud track, artist or playlist link")); header->addWidget(query_, 1);
    search_ = new QPushButton(tr("Search"), this); header->addWidget(search_);
    keyButton_ = new QPushButton(tr("API identity…"), this); header->addWidget(keyButton_); column->addLayout(header);
    notice_ = new QLabel(this); notice_->setWordWrap(true); notice_->setTextFormat(Qt::PlainText); column->addWidget(notice_);
    results_ = new QListWidget(this); results_->setObjectName("soundCloudResults"); results_->setIconSize(QSize(48,48)); column->addWidget(results_, 1);
    more_ = new QPushButton(tr("Load more"), this); more_->hide(); column->addWidget(more_);
    nowPlaying_ = new QLabel(this); nowPlaying_->setTextFormat(Qt::PlainText); column->addWidget(nowPlaying_);
    auto* bar = new QHBoxLayout;
    playButton_ = new QPushButton(tr("Play"), this); bar->addWidget(playButton_);
    seek_ = new QSlider(Qt::Horizontal, this); seek_->setRange(0,0); bar->addWidget(seek_, 1);
    clock_ = new QLabel("0:00 / 0:00", this); bar->addWidget(clock_);
    downloadButton_ = new QPushButton(tr("Save to vault"), this); bar->addWidget(downloadButton_); column->addLayout(bar);
    status_ = new QLabel(this); status_->setObjectName("soundCloudStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); column->addWidget(status_);
    player_ = new PluginAudioPlayer(this);
    connect(search_, &QPushButton::clicked, this, [this] { runSearch(); });
    connect(query_, &QLineEdit::returnPressed, this, [this] { runSearch(); });
    connect(keyButton_, &QPushButton::clicked, this, [this] { promptForKey(); });
    connect(back_, &QPushButton::clicked, this, [this] {
        if (!history_.empty()) { const auto old = history_.back(); history_.pop_back(); browse(old.url, old.kind, old.context); }
        else { setVault({}); emit closed(); }
    });
    connect(more_, &QPushButton::clicked, this, [this] { if (!next_.isEmpty()) browse(next_, browseKind_, context_, true); });
    connect(results_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { playRow(item); });
    connect(playButton_, &QPushButton::clicked, this, [this] {
        if (player_->state() == QMediaPlayer::PlayingState) player_->pause();
        else if (stream_ || hls_) player_->play();
        updateTransport();
    });
    connect(seek_, &QSlider::sliderReleased, this, [this] { if (!playingHls_) player_->setPosition(seek_->value()); });
    connect(player_, &PluginAudioPlayer::positionChanged, this, [this](qint64 ms) {
        if (!seek_->isSliderDown()) seek_->setValue(int(ms));
        clock_->setText(timeText(ms) + " / " + timeText(playing_.duration_ms > 0 ? playing_.duration_ms : player_->duration()));
    });
    connect(player_, &PluginAudioPlayer::durationChanged, this, [this](qint64 ms) { seek_->setRange(0, int(std::min<qint64>(INT_MAX, ms))); });
    connect(player_, &PluginAudioPlayer::stateChanged, this, [this] { updateTransport(); });
    connect(downloadButton_, &QPushButton::clicked, this, [this] { startImport(); });
    importPoll_ = new QTimer(this); importPoll_->setInterval(100);
    connect(importPoll_, &QTimer::timeout, this, [this] {
        if (importSource_ && importSource_->failed()) {
            const auto error = importSource_->errorString(); cancelImport(); status_->setText(error); refreshImportButton(); return;
        }
        if (!import_ || !import_->finished()) return;
        const bool success = import_->committed(); const auto error = import_->errorString();
        if (success) importedId_ = playingId_;
        cancelImport();
        status_->setText(success ? tr("Saved in the encrypted vault.") : error);
        refreshImportButton(); if (success) emit imported();
    });
    connect(host_, &PluginHost::changed, this, [this] {
        if (!host_->enabled(QStringLiteral("org.megavault.soundcloud"))) setVault({});
    });
    reloadKeyState(); refreshImportButton();
}
SoundCloudPage::~SoundCloudPage() { setVault({}); }
void SoundCloudPage::setVault(const std::shared_ptr<core::Vault>& vault) {
    if (browseCancel_) *browseCancel_ = true;
    stopPlayback(); cancelImport();
    vault_ = vault; resolver_ = vault ? std::make_unique<MusicResolver>(vault) : nullptr;
    covers_.clear(); coverBusy_ = false; results_->clear(); history_.clear(); next_ = QUrl(); currentBrowse_ = QUrl();
    query_->clear(); context_.clear(); title_->setText(tr("SoundCloud")); status_->clear(); nowPlaying_->clear(); more_->hide();
    if (auto* editor = findChild<QWidget*>("soundCloudIdentityEditor")) delete editor;
    reloadKeyState(); refreshImportButton();
}
QString SoundCloudPage::apiKey() const {
    return QSettings().value(host_->settingKey("org.megavault.soundcloud") + "apiKey").toString();
}
void SoundCloudPage::reloadKeyState() {
    const bool ready = vault_ && !apiKey().isEmpty(); query_->setEnabled(ready); search_->setEnabled(ready); kind_->setEnabled(ready); keyButton_->setEnabled(bool(vault_));
    notice_->setText(tr("Use your own authorized SoundCloud application client identity. No web-player credentials are scraped. API access is subject to SoundCloud approval; access refusals are not bypassed. Artist-enabled original downloads only; HLS playback is memory-only and cannot currently be saved."));
}
void SoundCloudPage::promptForKey() {
    if (!vault_ || findChild<QWidget*>("soundCloudIdentityEditor")) return;
    // An inline host-owned editor, not a separate native dialog/window.
    auto* editor = new QWidget(this); editor->setObjectName("soundCloudIdentityEditor"); auto* row = new QHBoxLayout(editor);
    auto* entry = new QLineEdit(editor); entry->setPlaceholderText(tr("Your authorized application's client_id")); entry->setText(apiKey());
    auto* save = new QPushButton(tr("Save identity"), editor); auto* cancel = new QPushButton(tr("Cancel"), editor);
    row->addWidget(entry, 1); row->addWidget(save); row->addWidget(cancel); qobject_cast<QVBoxLayout*>(layout())->insertWidget(1, editor);
    connect(cancel, &QPushButton::clicked, editor, &QObject::deleteLater);
    connect(save, &QPushButton::clicked, this, [this, editor, entry] {
        const auto key = entry->text().trimmed().left(512);
        if (browseCancel_) *browseCancel_ = true; stopPlayback(); cancelImport(); results_->clear(); covers_.clear();
        QSettings().setValue(host_->settingKey("org.megavault.soundcloud") + "apiKey", key); editor->deleteLater(); reloadKeyState();
    });
    entry->setFocus();
}
QUrl SoundCloudPage::apiUrl(const QString& path) const { return SoundCloudProvider::withClientId(QUrl("https://api-v2.soundcloud.com" + path), apiKey()); }
void SoundCloudPage::request(QUrl url, int limit, Cancel cancel, std::function<void(QByteArray)> complete) {
    if (!vault_ || *cancel) return;
    auto* watcher = new QFutureWatcher<SoundCloudTransport::Result>(this);
    connect(watcher, &QFutureWatcher<SoundCloudTransport::Result>::finished, this, [this, watcher, cancel, complete] {
        auto result = watcher->result(); watcher->deleteLater();
        if (*cancel || !vault_) return;
        if (!result.error.isEmpty()) { status_->setText(result.error); return; }
        complete(std::move(result.body));
    });
    auto transport = transport_;
    const bool resolveOriginal = url.path().startsWith("/tracks/") && url.path().endsWith("/download");
    watcher->setFuture(QtConcurrent::run([transport, url, limit, cancel, resolveOriginal] { return transport->get(url, limit, *cancel, resolveOriginal); }));
}
void SoundCloudPage::searchFor(const QString& term) { query_->setText(term); runSearch(); }
void SoundCloudPage::openLink(const QString& link) { query_->setText(link); runSearch(); }
void SoundCloudPage::runSearch() {
    if (!vault_) return;
    if (apiKey().isEmpty()) { promptForKey(); return; }
    const auto text = query_->text().trimmed(); if (text.isEmpty()) return;
    history_.clear();
    if (SoundCloudProvider::looks_like_link(text)) { resolveLink(text); return; }
    const auto kind = kind_->currentData().toString();
    const auto path = kind == "users" ? SoundCloudProvider::searchUsersPath(text,25) : kind == "playlists" ? SoundCloudProvider::searchPlaylistsPath(text,25) : SoundCloudProvider::searchTracksPath(text,25);
    browse(apiUrl(path), kind, tr("Results for %1").arg(text));
}
void SoundCloudPage::resolveLink(const QString& link) { browse(apiUrl(SoundCloudProvider::resolvePath(link)), "resolve", tr("SoundCloud link")); }
void SoundCloudPage::browse(QUrl url, QString kind, QString context, bool append) {
    if (!vault_) return;
    // next_href is untrusted, and may never redirect catalogue browsing to a CDN.
    if (url.host() != "api-v2.soundcloud.com" && url.host() != "api.soundcloud.com") { status_->setText(tr("Invalid catalogue continuation host.")); return; }
    if (browseCancel_) *browseCancel_ = true;
    browseCancel_ = std::make_shared<std::atomic<bool>>(false); covers_.clear(); coverBusy_ = false;
    if (!append) { results_->clear(); currentBrowse_ = url; }
    browseKind_ = kind; context_ = context; next_ = QUrl(); more_->hide(); status_->setText(tr("Loading SoundCloud…")); title_->setText(context);
    request(SoundCloudProvider::withClientId(url,apiKey()), 4 << 20, browseCancel_, [this, kind, context, append](QByteArray body) {
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray())) { status_->setText(tr("Invalid SoundCloud catalogue response.")); return; }
        auto object = doc.isArray() ? QJsonObject{{"collection",doc.array()}} : doc.object();
        if (kind == "resolve" || kind == "playlist") { showObject(object); return; }
        if (kind == "tracks") showTracks(SoundCloudProvider::parse_track_collection(object), context, append);
        else {
            if (!append) results_->clear();
            for (const auto& value : object.value("collection").toArray()) {
                if (results_->count() >= 500) break;
                const auto raw = value.toObject(); QString id, name, art, subtitle;
                if (kind == "users") {
                    SoundCloudProvider::Artist artist; if (!SoundCloudProvider::parse_artist(raw,&artist)) continue;
                    id = artist.id; name = artist.name; art = artist.artwork_url; subtitle = tr("Artist · %1 tracks").arg(artist.track_count);
                } else {
                    SoundCloudProvider::Playlist playlist; if (!SoundCloudProvider::parse_playlist(raw,&playlist)) continue;
                    id = playlist.id; name = playlist.title; art = playlist.artwork_url; subtitle = tr("Playlist · %1 · %2 tracks").arg(playlist.artist).arg(playlist.track_count);
                }
                auto* item = new QListWidgetItem(name + "\n" + subtitle, results_); item->setData(Qt::UserRole,id); item->setData(Qt::UserRole+8,kind);
                requestCover(art,id);
            }
            status_->setText(tr("%1 — %2 results").arg(context).arg(results_->count()));
        }
        const QUrl continuation(object.value("next_href").toString());
        if (!continuation.isEmpty() && SoundCloudTransport::permits(continuation) && results_->count() < 500) { next_ = continuation; more_->show(); }
    });
}
void SoundCloudPage::showObject(const QJsonObject& object) {
    const auto kind = object.value("kind").toString();
    if (kind == "user") {
        SoundCloudProvider::Artist artist;
        if (SoundCloudProvider::parse_artist(object,&artist)) { browse(apiUrl(SoundCloudProvider::userTracksPath(artist.id,25)), "tracks", artist.name); return; }
    } else if (kind == "playlist" || object.contains("tracks")) {
        SoundCloudProvider::Playlist playlist;
        if (SoundCloudProvider::parse_playlist(object,&playlist)) { title_->setText(playlist.title); showTracks(playlist.tracks, playlist.title); return; }
    } else {
        SoundCloudProvider::Track track;
        if (SoundCloudProvider::parse_track(object,&track)) { showTracks({track},track.title); return; }
    }
    status_->setText(tr("The link is not an available SoundCloud track, artist or playlist."));
}
void SoundCloudPage::showTracks(const QVector<SoundCloudProvider::Track>& tracks, const QString& context, bool append) {
    if (!append) results_->clear();
    for (const auto& track : tracks) {
        if (results_->count() >= 500) break;
        auto text = track.title + "\n" + track.artist + " · " + timeText(track.duration_ms);
        const auto rights = SoundCloudProvider::playback_rights(track);
        if (rights != SoundCloudProvider::Rights::None) text += " · " + SoundCloudProvider::rights_message(rights);
        const auto local = resolver_ ? resolver_->find("org.megavault.soundcloud",track.id.toStdString()) : std::nullopt;
        if (local) text += tr(" · In vault");
        auto* item = new QListWidgetItem(text,results_); item->setData(Qt::UserRole,track.id); item->setData(Qt::UserRole+1,track.title);
        item->setData(Qt::UserRole+7,local ? qlonglong(local->video_id) : 0); item->setData(Qt::UserRole+8,"tracks");
        requestCover(track.artwork_url,track.id);
    }
    status_->setText(results_->count() >= 500 ? tr("Showing the first 500 tracks; refine the search for more.") : tr("%1 — %2 tracks (double-click to play)").arg(context).arg(results_->count()));
}
void SoundCloudPage::requestCover(const QString& url, const QString& id) {
    if (url.isEmpty() || !SoundCloudTransport::permits(QUrl(url)) || covers_.size() >= 500) return;
    covers_.emplace_back(url,id); loadNextCover();
}
void SoundCloudPage::loadNextCover() {
    if (coverBusy_ || covers_.empty() || !vault_) return;
    coverBusy_ = true; const auto entry = covers_.front(); covers_.pop_front(); const auto cancel = browseCancel_; auto transport = transport_;
    auto* watcher = new QFutureWatcher<SoundCloudTransport::Result>(this);
    connect(watcher,&QFutureWatcher<SoundCloudTransport::Result>::finished,this,[this,watcher,entry,cancel] {
        auto result = watcher->result(); watcher->deleteLater(); if (*cancel || !vault_) return;
        coverBusy_ = false;
        if (result.error.isEmpty()) {
            QBuffer buffer(&result.body); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
            const auto size = reader.size();
            if (size.isValid() && size.width() <= 4096 && size.height() <= 4096) {
                reader.setScaledSize(size.scaled(48,48,Qt::KeepAspectRatio)); const auto image = reader.read();
                if (!image.isNull()) for (int row=0;row<results_->count();++row) if (results_->item(row)->data(Qt::UserRole).toString() == entry.second) results_->item(row)->setIcon(QIcon(QPixmap::fromImage(image)));
            }
        }
        loadNextCover();
    });
    watcher->setFuture(QtConcurrent::run([transport,entry,cancel] { return transport->get(QUrl(entry.first),512 << 10,*cancel); }));
}
void SoundCloudPage::playRow(QListWidgetItem* item) {
    if (!item || !vault_) return;
    const auto kind = item->data(Qt::UserRole+8).toString(); const auto id = item->data(Qt::UserRole).toString();
    if (kind != "tracks") {
        if (!currentBrowse_.isEmpty() && history_.size() < 32) history_.push_back({currentBrowse_,browseKind_,context_});
        if (kind == "users") browse(apiUrl(SoundCloudProvider::userTracksPath(id,25)),"tracks",item->text().section('\n',0,0));
        else browse(apiUrl(SoundCloudProvider::playlistPath(id)),"playlist",item->text().section('\n',0,0));
        return;
    }
    stopPlayback(); cancelImport();
    playingId_ = id; playing_.id = id;
    if (const auto local = resolver_ ? resolver_->find("org.megavault.soundcloud",id.toStdString()) : std::nullopt) {
        playing_.title = item->data(Qt::UserRole+1).toString(); playLocalCopy(local->video_id, playing_.title); return;
    }
    resolveTrack(id);
}
void SoundCloudPage::playLocalCopy(qint64 id, const QString& title) {
    if (!localPlayer_) { localPlayer_ = MusicWindow::shared(this); localPlayer_->setParent(this); if (auto* column = qobject_cast<QVBoxLayout*>(layout())) column->addWidget(localPlayer_); }
    localPlayer_->playQueue(vault_,std::vector<MusicTrack>{{id,title}},0); nowPlaying_->setText(title + tr(" (offline)")); status_->setText(tr("Playing the encrypted vault copy.")); refreshImportButton();
}
void SoundCloudPage::resolveTrack(const QString& id) {
    playCancel_ = std::make_shared<std::atomic<bool>>(false); status_->setText(tr("Checking SoundCloud playback permissions…"));
    request(apiUrl("/tracks/"+pathId(id)),4 << 20,playCancel_,[this,id](QByteArray bytes) {
        const auto raw = QJsonDocument::fromJson(bytes).object(); SoundCloudProvider::Track fresh;
        if (!SoundCloudProvider::parse_track(raw,&fresh) || fresh.id != id) { status_->setText(tr("Invalid track response.")); return; }
        playing_ = fresh; const auto rights = SoundCloudProvider::playback_rights(playing_);
        if (rights != SoundCloudProvider::Rights::None) { status_->setText(SoundCloudProvider::rights_message(rights)); refreshImportButton(); return; }
        bool hasHls = false; auto endpoint = SoundCloudProvider::progressive_stream_url(playing_,raw,&hasHls);
        playingHls_ = endpoint.isEmpty(); if (playingHls_) endpoint = SoundCloudProvider::hls_stream_url(raw);
        if (endpoint.isEmpty()) { status_->setText(tr("SoundCloud returned no supported stream.")); return; }
        // Client identity/track authorization are sent only to an approved API endpoint.
        if (endpoint.host() != "api-v2.soundcloud.com" && endpoint.host() != "api.soundcloud.com") { status_->setText(tr("Unapproved transcoding endpoint.")); return; }
        endpoint = SoundCloudProvider::streaming_url_for_transcoding(endpoint,apiKey());
        if (!playing_.track_authorization.isEmpty()) { QUrlQuery query(endpoint); query.addQueryItem("track_authorization",playing_.track_authorization); endpoint.setQuery(query); }
        request(endpoint,1 << 20,playCancel_,[this](QByteArray bytes) {
            const QUrl url(QJsonDocument::fromJson(bytes).object().value("url").toString());
            if (!SoundCloudTransport::permits(url)) { status_->setText(tr("Invalid or unapproved stream URL.")); return; }
            nowPlaying_->setText(playing_.artist + " — " + playing_.title);
            if (playingHls_) {
                hls_ = std::make_unique<SoundCloudHlsSource>(url); hls_->start(); player_->setSource(hls_.get());
                status_->setText(tr("HLS: memory-only format-aware playback; seeking and offline remux/import are not supported."));
            } else {
                stream_ = new RemoteStreamSource(url,sourceLimits(),this); stream_->start(); player_->setSource(stream_);
                stream_->set_failure_callback([this](const QString& error) { status_->setText(error); }); status_->setText(tr("Buffering progressive audio…"));
            }
            seek_->setEnabled(!playingHls_); player_->play(); refreshImportButton();
        });
    });
}
void SoundCloudPage::stopPlayback() {
    if (playCancel_) *playCancel_ = true;
    if (stream_) stream_->cancel(); if (hls_) hls_->cancel();
    player_->stop();
    // The player borrows ByteSource even while its asynchronous open job is
    // pending. Cancel wakes those reads; drain/destroy open-job results before
    // releasing their source, including a page destruction during probing.
    for (auto* open : player_->findChildren<QFutureWatcherBase*>()) {
        open->waitForFinished();
        delete open;
    }
    if (stream_) { stream_->deleteLater(); stream_ = nullptr; } hls_.reset();
    // The bar is process-wide: closing it stops playback without destroying it.
    if (localPlayer_) { localPlayer_->close(); localPlayer_ = nullptr; }
    playingId_.clear(); importedId_.clear(); playing_ = {}; playingHls_ = false;
    seek_->setRange(0,0); seek_->setEnabled(true); clock_->setText("0:00 / 0:00"); nowPlaying_->clear(); refreshImportButton();
}
void SoundCloudPage::updateTransport() {
    playButton_->setText(player_->state() == QMediaPlayer::PlayingState ? tr("Pause") : tr("Play"));
    if (hls_ && !hls_->errorString().isEmpty()) status_->setText(hls_->errorString());
    else if (player_->error() != QMediaPlayer::NoError) status_->setText(player_->errorString());
}
void SoundCloudPage::refreshImportButton() {
    const bool local = resolver_ && !playingId_.isEmpty() && resolver_->has_local_copy("org.megavault.soundcloud",playingId_.toStdString());
    const auto rights = SoundCloudProvider::download_rights(playing_);
    downloadButton_->setText(local || !importedId_.isEmpty() ? tr("In vault") : tr("Save to vault"));
    downloadButton_->setEnabled(vault_ && !playingId_.isEmpty() && !local && importedId_.isEmpty() && !import_ && !playingHls_ && rights == SoundCloudProvider::Rights::None);
    downloadButton_->setToolTip(playingHls_ ? tr("HLS plays in memory, but this build cannot remux it into encrypted staging with a known final size; HLS offline import is not supported.") : SoundCloudProvider::rights_message(rights));
}
void SoundCloudPage::startImport() {
    if (!vault_ || import_ || playingId_.isEmpty() || playingHls_) { refreshImportButton(); return; }
    const auto rights = SoundCloudProvider::download_rights(playing_);
    if (rights != SoundCloudProvider::Rights::None) { status_->setText(SoundCloudProvider::rights_message(rights)); return; }
    if (resolver_ && resolver_->has_local_copy("org.megavault.soundcloud",playingId_.toStdString())) { refreshImportButton(); return; }
    *importCancel_ = true; importCancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto id = playingId_; const auto pinnedVault = vault_;
    // Recheck rights immediately before resolving the artist-authorized original download.
    request(apiUrl("/tracks/"+pathId(id)),4 << 20,importCancel_,[this,id,pinnedVault](QByteArray body) {
        SoundCloudProvider::Track fresh;
        if (!SoundCloudProvider::parse_track(QJsonDocument::fromJson(body).object(),&fresh) || fresh.id != id) { status_->setText(tr("Cannot verify download permissions.")); return; }
        const auto rights = SoundCloudProvider::download_rights(fresh);
        if (rights != SoundCloudProvider::Rights::None) { status_->setText(SoundCloudProvider::rights_message(rights)); return; }
        request(SoundCloudProvider::download_url(id,apiKey()),1 << 20,importCancel_,[this,id,pinnedVault,fresh](QByteArray body) {
            if (vault_ != pinnedVault || id != playingId_) return;
            const auto raw = QJsonDocument::fromJson(body).object();
            QUrl url(raw.value("redirectUri").toString()); if (url.isEmpty()) url = QUrl(raw.value("url").toString());
            if (url.isEmpty()) url = QUrl(QString::fromUtf8(body).trimmed());
            if (!SoundCloudTransport::permits(url)) { status_->setText(tr("Original download unavailable: SoundCloud returned no permitted file URL. Stream URLs are never substituted.")); return; }
            importSource_ = new RemoteStreamSource(url,sourceLimits(),this);
            core::TrackProvenance provenance; provenance.provider_id = "org.megavault.soundcloud"; provenance.provider_track_id = id.toStdString();
            provenance.source_url = fresh.permalink_url.toStdString(); provenance.title = fresh.title.toStdString(); provenance.artist = fresh.artist.toStdString(); provenance.duration_ms = std::uint64_t(std::max<qint64>(0,fresh.duration_ms));
            // Original format need not match the playback transcoding; do not label it MP3.
            QString name = fresh.title.left(100); name.replace('/','_'); name.replace('\\','_'); name += ".audio";
            import_ = std::make_unique<RemoteStreamImport>(*importSource_,pinnedVault,name,0,provenance,false);
            importSource_->start(); importPoll_->start(); status_->setText(tr("Encrypting the artist-authorized original download…")); refreshImportButton();
        });
    });
}
void SoundCloudPage::cancelImport() {
    if (importCancel_) *importCancel_ = true;
    importPoll_->stop(); if (import_) import_->cancel(); import_.reset();
    if (importSource_) { importSource_->cancel(); importSource_->deleteLater(); importSource_ = nullptr; }
}
} // namespace videovault::app
