#pragma once

#include "videovault/app/record_document.hpp"

#include <QByteArray>
#include <QSet>
#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>

class QKeyEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

// The editor for one record file: an ordered list of records, each a title plus a
// nested body of typed values.
//
// Nothing here talks to Windows: prompts are drawn inside the window that owns this
// page through InWindowSheet, which is the same prompt the library uses to rename an
// item. Content transitions (read, save, migrate) run off the GUI thread and come
// back through a watcher.
class RecordWindow final : public QWidget {
    Q_OBJECT

public:
    explicit RecordWindow(QWidget* parent = nullptr);

    void openRecord(std::shared_ptr<core::Vault> vault, std::int64_t recordId,
        const QString& itemName);

signals:
    void closeRequested();
    void saved();

protected:
    void changeEvent(QEvent* event) override;
    // Escape clears the filter when there is one, otherwise it closes the page.
    void keyPressEvent(QKeyEvent* event) override;

private:
    // A position inside the document: the record, then the member/element index at
    // each level. Resolved on demand, so a rebuild cannot invalidate it.
    struct NodePath {
        int entry{-1};
        QVector<int> steps;
    };

    // What a prompt came back with.
    struct NodeDraft {
        ValueType type{ValueType::String};
        QString key;
        QString value;
    };

    void buildInterface();
    void applyStyle();

    void load();
    void fill(const RecordDocument& document);
    void showLegacy(const QByteArray& bytes, int version);
    void showUnreadable(const QByteArray& bytes);
    void migrate();

    void rebuild();
    void buildEntry(int entryIndex, QVBoxLayout* layout);
    void buildChildren(QWidget* host, QVBoxLayout* layout, const NodePath& path,
        const RecordNode& node, int depth, const QString& collapseKey, bool showAll);
    void buildRow(QWidget* parent, QVBoxLayout* layout, const NodePath& path,
        const RecordNode& node, int depth, const QString& collapseKey, const QString& key,
        bool showAll);

    RecordNode* resolve(const NodePath& path);
    RecordNode* resolveParent(const NodePath& path, QString* key);
    QString pathText(const NodePath& path) const;

    // Prompts, all drawn in-window.
    bool promptNode(const QString& heading, bool withKey, NodeDraft* draft);
    bool promptText(const QString& heading, const QString& message, const QString& initial,
        QString* out);
    bool confirm(const QString& heading, const QString& message, const QString& action);

    void addChild(const NodePath& parent);
    void editNode(const NodePath& path);
    void renameNode(const NodePath& path);
    void removeNode(const NodePath& path);
    void copyNode(const NodePath& path);

    void addEntry();
    void renameEntry(int entryIndex);
    void removeEntry(int entryIndex);
    void moveEntry(int entryIndex, int delta);

    void markChanged();
    void refreshDirty();
    void save();
    void revert();
    // True when the document differs from what was last read or saved.
    [[nodiscard]] bool isDirty() const;
    // Closes, asking first if there are unsaved edits.
    void requestClose();
    // Reorders the records by title; the old order comes back through Revert.
    void sortEntries(bool ascending);
    void writeContent(const QByteArray& bytes, const QString& success,
        const std::function<void()>& after);

    void setBusy(bool busy);
    void setStatus(const QString& message, bool problem);
    void updateEmptyState();
    void copyToClipboard(const QString& text, bool secret, const QString& note);

    std::shared_ptr<core::Vault> vault_;
    std::int64_t recordId_{0};
    QString itemName_;
    RecordDocument document_;
    QByteArray loaded_;
    QByteArray legacy_;
    bool editable_{false};
    QSet<QString> collapsed_;
    QSet<QString> revealed_;

    QTimer* clipboardTimer_{nullptr};
    QString clipboardValue_;
    QString rowHoverStyle_;
    // setStyleSheet() posts another StyleChange; without these two the handler
    // below re-entered itself until the stack died (creating a record in light
    // theme crashed the app).
    bool restyling_{false};
    QColor styledWindowColor_;
    QString disclosureColor_;
    int actionColumn_{0};

    QLineEdit* search_{nullptr};
    QString query_;
    QLabel* fileName_{nullptr};
    QLabel* countLabel_{nullptr};
    QLabel* status_{nullptr};
    QLabel* errorHint_{nullptr};
    QLabel* legacyLabel_{nullptr};
    QPushButton* addRecordButton_{nullptr};
    QPushButton* saveButton_{nullptr};
    QPushButton* revertButton_{nullptr};
    QPushButton* migrateButton_{nullptr};
    QWidget* legacyBar_{nullptr};
    QScrollArea* scroll_{nullptr};
    QWidget* host_{nullptr};
    QVBoxLayout* hostLayout_{nullptr};
    QPlainTextEdit* raw_{nullptr};
};

} // namespace videovault::app
