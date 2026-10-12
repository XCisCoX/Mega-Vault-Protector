#pragma once

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QEvent;
class QKeyEvent;
class QObject;
class QShowEvent;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

// Renders the first page of a PDF for the library. A protected or unreadable
// file returns a null image; the library then keeps the plain document tile.
QImage renderPdfFirstPage(const std::vector<unsigned char>& bytes, int maxEdge);

// Reads a PDF from the vault and shows it: continuous pages, zoom, fit, and a
// page strip. The plaintext stays in memory for as long as the document is open.
class PdfWindow final : public QWidget {
    Q_OBJECT

public:
    explicit PdfWindow(QWidget* parent = nullptr);
    ~PdfWindow() override;

    void openDocument(
        const std::shared_ptr<videovault::core::Vault>& vault,
        std::int64_t id,
        const QString& title);
    void setNeighbors(bool previous, bool next);
    [[nodiscard]] std::int64_t documentId() const noexcept { return id_; }
    // The reader can sit in the library or in its own window, same as the video player.
    void prepareDock();
    void detach();
    [[nodiscard]] bool isDetached() const noexcept { return detached_; }

signals:
    void closeRequested();
    void previousRequested();
    void nextRequested();
    void detachRequested();
    void dockRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyLoad();
    void askPassword(bool rejected);
    void relayout();
    void zoomBy(double factor);
    void setZoom(double zoom);
    void updatePageLabel();
    void pump();
    void applyRender();
    void applyDetachedFrame();
    void updatePopButton();
    [[nodiscard]] int pageWidth() const;

    bool detached_{false};
    bool frameReady_{false};

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::int64_t id_{0};
};

} // namespace videovault::app
