#pragma once

#include "videovault/app/record_document.hpp"

#include <QFrame>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace videovault::core {
class Vault;
struct VaultError;
} // namespace videovault::core

namespace videovault::app {

// One key/value row: a name, a value, and a persisted "secret" flag.
//
// Deliberately not a QObject: the window wires it up with callbacks, so no
// extra moc pass is needed for a helper type. Masking never loses data -- the
// real value lives in the model, and the editor only holds dots while masked,
// so a masked row can be saved or copied without leaking or corrupting.
class RecordFieldCard final : public QFrame {
public:
    explicit RecordFieldCard(QWidget* parent = nullptr);

    void setField(const RecordField& field);
    RecordField field() const { return field_; }
    void setEditable(bool editable);
    bool matches(const QString& needle) const;
    void focusName();

    std::function<void()> onChanged;
    std::function<void(RecordFieldCard*)> onRemove;
    std::function<void(const QString& name, const QString& value, bool secret)> onCopy;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyMask();
    void refreshKind();
    void adjustValueHeight();
    void notifyChanged();

    QLineEdit* name_{nullptr};
    QLabel* badge_{nullptr};
    QPlainTextEdit* value_{nullptr};
    QPushButton* reveal_{nullptr};
    QPushButton* secret_{nullptr};
    QPushButton* copy_{nullptr};
    QPushButton* remove_{nullptr};

    RecordField field_;
    bool revealed_{false};
    bool updating_{false};
    bool editable_{true};
};

// The record editor. Loads and saves through the core's ordinary item API, so
// a record behaves exactly like an imported file everywhere else in the app.
class RecordWindow final : public QWidget {
    Q_OBJECT
public:
    explicit RecordWindow(QWidget* parent = nullptr);

    // Loads the record off the UI thread and populates the editor.
    void openRecord(std::shared_ptr<core::Vault> vault, std::int64_t record_id,
        const QString& title);

signals:
    void closeRequested();
    // The stored content changed: the gallery may need to repaint (the display
    // name can change here).
    void saved();

private:
    void buildInterface();
    void load();
    void fill(const RecordDocument& document);
    void updateEmptyState();
    void applySearch();
    void addField(const RecordField& field, bool focus);
    void removeCard(RecordFieldCard* card);
    void markChanged();
    void refreshDirty();
    void setEditable(bool editable);
    RecordDocument collect() const;
    void save();
    void revert();
    void copyValue(const QString& name, const QString& value, bool secret);
    void setStatus(const QString& message, bool problem);
    void setBusy(bool busy);
    void updateCount();

    std::shared_ptr<core::Vault> vault_;
    std::int64_t recordId_{0};
    QByteArray loaded_;      // bytes as stored, for change detection
    bool editable_{true};    // false when the content is not a record we can write

    QLineEdit* titleEdit_{nullptr};
    QLineEdit* searchEdit_{nullptr};
    QScrollArea* scroll_{nullptr};
    QWidget* cardHost_{nullptr};
    QVBoxLayout* cardLayout_{nullptr};
    QLabel* emptyHint_{nullptr};
    QLabel* status_{nullptr};
    QLabel* countLabel_{nullptr};
    QPushButton* addButton_{nullptr};
    QPushButton* saveButton_{nullptr};
    QPushButton* revertButton_{nullptr};
    QTimer* clipboardTimer_{nullptr};
    QString clipboardValue_;
    std::vector<RecordFieldCard*> cards_;
};

} // namespace videovault::app
