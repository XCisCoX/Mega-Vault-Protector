#pragma once

#include <QWidget>

#include <memory>
#include <utility>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

template <typename T>
class QFutureWatcher;

namespace videovault::core {
class Vault;
template <typename T>
class Result;
} // namespace videovault::core

namespace videovault::app {

// In-window page. The host owns navigation, updating and application theming.
class SettingsDialog final : public QWidget {
    Q_OBJECT
public:
    explicit SettingsDialog(
        const std::shared_ptr<videovault::core::Vault>& vault,
        QWidget* parent = nullptr);
    void reject();
    void setUpdateStatus(const QString& message, bool busy = false);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void settingsChanged();
    void closed();
    void updateCheckRequested();
    void updatePreferencesChanged();
    void appearanceChanged();

private:
    void reloadTags();
    void beginPasswordChange();
    void finishPasswordChange();
    void addTag();
    void renameSelectedTag();
    void removeSelectedTag();
    void applyCacheSelection();
    void beginRegenerateMissing();
    void finishRegenerateMissing();
    void setStatus(const QString& message, bool error = false);
    std::shared_ptr<videovault::core::Vault> vault_;

    // Security section.
    QLineEdit* currentPassword_{nullptr};
    QLineEdit* newPassword_{nullptr};
    QLineEdit* confirmPassword_{nullptr};
    QComboBox* securityProfile_{nullptr};
    QPushButton* changePasswordButton_{nullptr};
    QFutureWatcher<std::shared_ptr<videovault::core::Result<bool>>>* passwordWatcher_{nullptr};

    // Tags section.
    QListWidget* tagList_{nullptr};
    QLineEdit* newTagEdit_{nullptr};
    QPushButton* addTagButton_{nullptr};
    QPushButton* renameTagButton_{nullptr};
    QPushButton* removeTagButton_{nullptr};

    // Playback section.
    QComboBox* cacheCombo_{nullptr};

    // Rebuilds stored thumbnails that import never managed to create.
    QPushButton* regenerateButton_{nullptr};
    QFutureWatcher<std::shared_ptr<std::pair<int, int>>>* regenerateWatcher_{nullptr};

    QLabel* status_{nullptr};
    QPushButton* checkUpdatesButton_{nullptr};
    QLabel* updateStatus_{nullptr};
};

} // namespace videovault::app
