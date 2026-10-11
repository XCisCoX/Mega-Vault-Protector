#pragma once

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QEvent;
class QKeyEvent;
class QObject;

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

signals:
    void closeRequested();
    void previousRequested();
    void nextRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
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
    [[nodiscard]] int pageWidth() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::int64_t id_{0};
};

} // namespace videovault::app
