#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QTimer;

namespace videovault::app {

struct CloudRow {
    std::int64_t id{0};
    std::int64_t parent{0};
    QString name;
    std::uint64_t size{0};
    bool folder{false};
};

// The cloud vault, inside this window. Files download at the chosen speed
// and pick up a green tick once the bytes are stored on this PC.
class CloudBrowser final : public QWidget {
public:
    explicit CloudBrowser(QWidget* parent = nullptr);

    void openSession(
        const QString& address,
        const QString& fingerprint,
        const QString& password);
    void setOnLeave(std::function<void()> callback);

private:
    void applyLibrary(const QByteArray& videos, const QByteArray& folders, const QString& sessions);
    void showFolder(std::int64_t folderId);
    void refreshRemote();
    void pump();
    void markSynced(std::int64_t id);
    [[nodiscard]] bool isSynced(const CloudRow& row) const;
    [[nodiscard]] QString cacheFile(std::int64_t id) const;

    QString host_;
    quint16 port_{0};
    QString fingerprint_;
    QString model_;
    QByteArray token_;
    QString speed_{QStringLiteral("normal")};
    std::vector<CloudRow> rows_;
    std::int64_t folderId_{0};
    bool refreshing_{false};
    bool downloading_{false};
    std::int64_t priorityId_{-1};
    QSet<qint64> failed_;
    std::function<void()> onLeave_;

    QLabel* people_{nullptr};
    QLabel* status_{nullptr};
    QComboBox* speedBox_{nullptr};
    QListWidget* list_{nullptr};
    QTimer* timer_{nullptr};
};

} // namespace videovault::app
