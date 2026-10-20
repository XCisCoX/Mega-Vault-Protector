#pragma once

#include <QEventLoop>
#include <functional>
#include <QMouseEvent>
#include <QPixmap>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
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
    // `icon` is a Lucide name (see docs/icon-strategy.md); empty keeps the button
    // text-only, which is the right default for anything without an obvious glyph.
    QPushButton* addButton(const QString& label, const int result, const bool primary,
        const QString& icon = QString());
    // Blocks until a button, Escape, or a click outside the card finishes it.
    int run();
    void finish(int result);
    [[nodiscard]] QString text() const;
    // Adds a row of widgets (e.g. radio buttons) to the card body.
    void addRow(QWidget* widget);

    // Test-only: when set, run() shows the sheet and hands it to the driver
    // instead of spinning a nested event loop, so headless tests can click it.
    static void setTestDriver(std::function<void(InWindowSheet*)> driver);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    // The dimmed backdrop is painted, not styled: a type selector cannot match this
    // class (it has no Q_OBJECT, so Qt knows it as a plain QWidget).
    void paintEvent(QPaintEvent* event) override;

private:
    void captureBackdrop();
    bool fitting_{false};
    bool fitPending_{false};
    static std::function<void(InWindowSheet*)> testDriver_;
    // The card has a fixed width, so its height has to come from the layout's
    // heightForWidth at that width: a word-wrapped message is otherwise cut off.
    void fitCard();
    QFrame* card_{nullptr};
    QLabel* title_{nullptr};
    QLabel* message_{nullptr};
    QWidget* bodyHost_{nullptr};
    QScrollArea* scroll_{nullptr};
    QVBoxLayout* body_{nullptr};
    QHBoxLayout* buttons_{nullptr};
    QLineEdit* entry_{nullptr};
    QEventLoop loop_;
    int result_{0};
    QPixmap backdrop_;
};

} // namespace videovault::app
