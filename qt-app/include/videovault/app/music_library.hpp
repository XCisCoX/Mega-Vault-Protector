#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

class MusicWindow;

// The library's music page: every audio item in the vault, listed like a music
// app (title, artist, size) instead of a folder of files. Playing a row hands a
// queue to the existing MusicWindow bar, which sits at the bottom of the page.
class MusicLibraryPage final : public QWidget {
    Q_OBJECT

public:
    explicit MusicLibraryPage(QWidget* parent = nullptr);
    void setVault(const std::shared_ptr<videovault::core::Vault>& vault);
    void refresh();
    // Provider search is only offered when a music provider is installed and
    // enabled; the local library works without one.
    void setProviderAvailable(bool available);

signals:
    void closed();
    void providerRequested();

private:
    struct Track {
        std::int64_t id{0};
        QString title;
        QString artist;
        qint64 size{0};
    };

    void rebuild();
    void applyFilter(const QString& text);
    void playRow(int row);
    void ensurePlayer();
    [[nodiscard]] std::vector<Track> playableOrder() const;

    std::shared_ptr<videovault::core::Vault> vault_;
    std::vector<Track> tracks_;
    std::vector<Track> shown_;

    QLineEdit* search_{nullptr};
    QListWidget* list_{nullptr};
    QLabel* count_{nullptr};
    QLabel* empty_{nullptr};
    QPushButton* playAll_{nullptr};
    QPushButton* provider_{nullptr};
    MusicWindow* player_{nullptr};
};

} // namespace videovault::app
