#pragma once

#include <QPushButton>
#include <QStringList>
#include <QVector>

class QLineEdit;
class QListWidget;
class QLabel;

namespace videovault::app {

struct TagFilterOption {
    QString name;
    quint64 count;
};

// Case-insensitive tag identities; selectedTags() returns available display names
// in the order supplied to setTags(). Unknown selections are ignored.
class TagFilterButton : public QPushButton {
    Q_OBJECT
public:
    explicit TagFilterButton(QWidget* parent = nullptr);
    void setTags(const QVector<TagFilterOption>& tags);
    QStringList selectedTags() const;
    void setSelectedTags(const QStringList& tags);
    QSize sizeHint() const override;

signals:
    void selectionChanged(const QStringList& tags);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showPopup();
    void rebuildRows();
    void refreshSelection(const QStringList& previous);
    void filterRows();
    void toggleRow(int row);
    void positionPopup();

    QVector<TagFilterOption> tags_;
    QStringList selected_;
    QWidget* popup_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* empty_ = nullptr;
    QPushButton* clear_ = nullptr;
};

} // namespace videovault::app
