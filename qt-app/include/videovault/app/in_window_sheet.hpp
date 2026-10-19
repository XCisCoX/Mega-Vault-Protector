#pragma once

#include <QEventLoop>
#include <QMouseEvent>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace videovault::app {

// A prompt drawn on top of the window that owns `host`. It is a child of that
// window, so it cannot appear as its own Windows dialog.
class InWindowSheet final : public QWidget {
public:
    explicit InWindowSheet(QWidget* host);

    void setTitle(const QString& title);
    void setMessage(const QString& message, bool problem = false);
    [[nodiscard]] QVBoxLayout* body() const noexcept { return body_; }
    [[nodiscard]] QHBoxLayout* buttons() const noexcept { return buttons_; }
    QLineEdit* addTextEntry(int echoMode, const QString& initial, const QString& placeholder);
    QPushButton* addButton(const QString& label, int result, bool primary);
    // Blocks until a button, Escape, or a click outside the card finishes it.
    int run();
    void finish(int result);
    [[nodiscard]] QString text() const;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    // The dimmed backdrop is painted, not styled: a type selector cannot match this
    // class (it has no Q_OBJECT, so Qt knows it as a plain QWidget).
    void paintEvent(QPaintEvent* event) override;

private:
    QFrame* card_{nullptr};
    QLabel* title_{nullptr};
    QLabel* message_{nullptr};
    QVBoxLayout* body_{nullptr};
    QHBoxLayout* buttons_{nullptr};
    QLineEdit* entry_{nullptr};
    QEventLoop loop_;
    int result_{0};
};

} // namespace videovault::app
