#include "videovault/app/in_window_sheet.hpp"
#include "videovault/app/icons.hpp"
#include "videovault/app/pdf_window.hpp"
#include "videovault/app/theme.hpp"

#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QPainterPath>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#endif

namespace videovault::app {
namespace {

// Every surface in the viewer is derived from the live palette. The reader used
// to be a fixed dark surface (#000000 backdrop, white labels, light-only icons),
// which is what made it look wrong in the light theme while the library around
// it was pale.
QColor pdfBackdrop() {
    return isDarkTheme() ? QColor(12, 12, 14) : QColor(236, 236, 240);
}
QColor pdfChrome() {
    return isDarkTheme() ? QColor(20, 20, 22) : QColor(250, 250, 252);
}
QColor pdfInk() {
    return QApplication::palette().color(QPalette::WindowText);
}
QColor pdfSecondary() {
    return QApplication::palette().color(QPalette::PlaceholderText);
}
QColor pdfHairline() {
    QColor color = QApplication::palette().color(QPalette::Midlight);
    color.setAlpha(isDarkTheme() ? 90 : 170);
    return color;
}
QColor pdfPageBorder() {
    return isDarkTheme() ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 34);
}
QColor pdfPageShadow() {
    QColor color(0, 0, 0);
    color.setAlpha(isDarkTheme() ? 120 : 34);
    return color;
}
// Space around each page that carries the drop shadow.
constexpr int kPageMargin = 8;

enum class CaptionGlyph { Minimize, Maximize, Restore, Close };

class CaptionButton final : public QAbstractButton {
public:
    explicit CaptionButton(const CaptionGlyph glyph, QWidget* parent = nullptr)
        : QAbstractButton(parent), glyph_(glyph) {
        setFixedSize(46, 32);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
    }

    void setGlyph(const CaptionGlyph glyph) {
        glyph_ = glyph;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            QColor hover = isDarkTheme() ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 20);
            if (glyph_ == CaptionGlyph::Close) {
                hover = QColor(232, 17, 35);
            }
            painter.fillRect(rect(), hover);
        }
        painter.setPen(QPen(glyph_ == CaptionGlyph::Close && underMouse()
            ? QColor(Qt::white) : pdfInk(), 1.2));
        const QPointF center = rect().center();
        switch (glyph_) {
        case CaptionGlyph::Minimize:
            painter.drawLine(QPointF(center.x() - 5, center.y()), QPointF(center.x() + 5, center.y()));
            break;
        case CaptionGlyph::Maximize:
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 5, 10, 10), 1.5, 1.5);
            break;
        case CaptionGlyph::Restore:
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 2, 8, 8), 1.2, 1.2);
            painter.drawLine(QPointF(center.x() - 2, center.y() - 4), QPointF(center.x() + 5, center.y() - 4));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 4), QPointF(center.x() + 5, center.y() + 3));
            break;
        case CaptionGlyph::Close:
            painter.drawLine(QPointF(center.x() - 5, center.y() - 5), QPointF(center.x() + 5, center.y() + 5));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 5), QPointF(center.x() - 5, center.y() + 5));
            break;
        }
    }

private:
    CaptionGlyph glyph_;
};

class PageCanvas final : public QWidget {
public:
    explicit PageCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    QImage image;
    int renderedWidth{0};
    int rotation{0};
    bool failed{false};

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF page = QRectF(rect()).adjusted(kPageMargin, kPageMargin,
            -kPageMargin, -kPageMargin);
        // A soft shadow under each sheet separates the page from the backdrop the
        // way every other reader does; the old flat white rectangle on black gave
        // no edge at all in the dark theme and none in the light one either.
        for (int step = 4; step >= 1; --step) {
            QColor shade = pdfPageShadow();
            shade.setAlpha(std::max(6, shade.alpha() / (step + 1)));
            painter.setPen(Qt::NoPen);
            painter.setBrush(shade);
            painter.drawRoundedRect(page.adjusted(-step, -step + 2, step, step + 2), 6, 6);
        }
        QPainterPath clip;
        clip.addRoundedRect(page, 3, 3);
        painter.setClipPath(clip);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRect(page);
        if (!image.isNull()) {
            if (rotation % 180 == 0) {
                painter.drawImage(page, image);
            } else {
                // The frame already carries the rotated aspect; draw the bitmap
                // turned so its long edge follows the frame's long edge.
                painter.save();
                painter.translate(page.center());
                painter.rotate(rotation);
                const qreal turnedW = page.height();
                const qreal turnedH = page.width();
                painter.drawImage(QRectF(-turnedW / 2.0, -turnedH / 2.0, turnedW, turnedH), image);
                painter.restore();
            }
        } else if (failed) {
            painter.setPen(pdfSecondary());
            QFont font = painter.font();
            font.setPixelSize(13);
            painter.setFont(font);
            painter.drawText(page, Qt::AlignCenter, QStringLiteral("This page could not be rendered."));
        } else {
            painter.setPen(pdfSecondary());
            QFont font = painter.font();
            font.setPixelSize(13);
            painter.setFont(font);
            painter.drawText(page, Qt::AlignCenter, QStringLiteral("Rendering…"));
        }
        painter.setClipping(false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(pdfPageBorder(), 1));
        painter.drawRoundedRect(page.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
};

#ifdef Q_OS_WIN
struct DocState {
    std::atomic<bool> alive{true};
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream{nullptr};
    winrt::Windows::Data::Pdf::PdfDocument document{nullptr};
};

void ensureMta() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        // The worker was already initialized as STA. Agile PDF objects still work.
        if (error.code() != static_cast<winrt::hresult>(0x80010106)) {
            throw;
        }
    }
}

bool hasPdfHeader(const std::vector<unsigned char>& bytes) {
    const auto limit = std::min<std::size_t>(bytes.size(), 1024U);
    const char mark[] = "%PDF";
    for (std::size_t i = 0; i + 4 <= limit; ++i) {
        if (std::memcmp(bytes.data() + i, mark, 4) == 0) {
            return true;
        }
    }
    return false;
}

bool passwordRejected(const winrt::hresult code, const QString& message) {
    const auto value = static_cast<std::uint32_t>(static_cast<int32_t>(code));
    // LoadFromStreamAsync reports a missing or wrong PDF password with these
    // Win32 codes. A file that merely contains the letters "/Encrypt" is not
    // one of these failures.
    return value == 0x8007052BU || value == 0x8007052EU || value == 0x80070056U
        || message.contains(QStringLiteral("password"), Qt::CaseInsensitive);
}

QImage renderPage(const std::shared_ptr<DocState>& state, const int index, const int pixelWidth) {
    try {
        ensureMta();
        if (state == nullptr || !state->alive.load() || state->document == nullptr || pixelWidth < 1) {
            return {};
        }
        const auto document = state->document;
        auto page = document.GetPage(static_cast<std::uint32_t>(index));
        const auto size = page.Size();
        if (size.Width < 1.f || size.Height < 1.f) {
            return {};
        }
        const auto destW = static_cast<std::uint32_t>(pixelWidth);
        const auto destH = std::max(1U, static_cast<std::uint32_t>(size.Height * static_cast<float>(destW) / size.Width));
        winrt::Windows::Data::Pdf::PdfPageRenderOptions options;
        options.DestinationWidth(destW);
        options.DestinationHeight(destH);
        options.IsIgnoringHighContrast(true);
        winrt::Windows::UI::Color white{};
        white.A = 255;
        white.R = 255;
        white.G = 255;
        white.B = 255;
        options.BackgroundColor(white);
        options.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::JpegEncoderId());
        winrt::Windows::Storage::Streams::InMemoryRandomAccessStream output;
        page.RenderToStreamAsync(output, options).get();
        const auto encodedSize = static_cast<std::uint32_t>(output.Size());
        if (encodedSize == 0U) {
            return {};
        }
        winrt::Windows::Storage::Streams::DataReader reader(output.GetInputStreamAt(0));
        reader.LoadAsync(encodedSize).get();
        std::vector<unsigned char> encoded(encodedSize);
        reader.ReadBytes(winrt::array_view<std::uint8_t>(encoded.data(), encodedSize));
        QImage image;
        if (!image.loadFromData(encoded.data(), static_cast<int>(encoded.size()), "JPEG")) {
            return {};
        }
        return image;
    } catch (const winrt::hresult_error&) {
        return {};
    } catch (const std::exception&) {
        return {};
    }
}

struct OpenOutcome {
    std::shared_ptr<DocState> doc;
    std::vector<QSizeF> sizes;
    QString error;
    bool needsPassword{false};
    bool badPassword{false};
};

OpenOutcome openPdf(const std::vector<unsigned char>& bytes, const QString& password) {
    OpenOutcome outcome;
    if (bytes.size() > 0xFFFFFFFFU) {
        outcome.error = QStringLiteral("This PDF is too large to open.");
        return outcome;
    }
    if (!hasPdfHeader(bytes)) {
        outcome.error = QStringLiteral("This file is not a PDF.");
        return outcome;
    }
    try {
        ensureMta();
        auto state = std::make_shared<DocState>();
        state->stream = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream();
        {
            winrt::Windows::Storage::Streams::DataWriter writer(state->stream);
            writer.WriteBytes(winrt::array_view<const std::uint8_t>(
                bytes.data(), static_cast<std::uint32_t>(bytes.size())));
            writer.StoreAsync().get();
            writer.DetachStream();
        }
        state->stream.Seek(0);
        if (password.isEmpty()) {
            state->document = winrt::Windows::Data::Pdf::PdfDocument::LoadFromStreamAsync(state->stream).get();
        } else {
            state->document = winrt::Windows::Data::Pdf::PdfDocument::LoadFromStreamAsync(
                state->stream, winrt::hstring(password.toStdWString())).get();
        }
        const auto count = state->document.PageCount();
        if (count == 0U) {
            outcome.error = QStringLiteral("This PDF has no pages.");
            return outcome;
        }
        outcome.sizes.reserve(count);
        for (std::uint32_t i = 0U; i < count; ++i) {
            const auto size = state->document.GetPage(i).Size();
            outcome.sizes.emplace_back(
                std::max(1.0, static_cast<double>(size.Width)),
                std::max(1.0, static_cast<double>(size.Height)));
        }
        outcome.doc = std::move(state);
    } catch (const winrt::hresult_error& error) {
        const QString message = QString::fromWCharArray(error.message().c_str());
        if (passwordRejected(error.code(), message)) {
            if (password.isEmpty()) {
                outcome.needsPassword = true;
            } else {
                outcome.badPassword = true;
                outcome.error = QStringLiteral("That password did not open this PDF.");
            }
            return outcome;
        }
        outcome.error = message.isEmpty()
            ? QStringLiteral("This PDF could not be opened.")
            : message;
    }
    return outcome;
}
#else
struct DocState {
    std::atomic<bool> alive{true};
};
#endif

struct LoadResult {
    int generation{0};
    bool encrypted{false};
    bool badPassword{false};
    QString error;
    std::vector<QSizeF> sizes;
    std::shared_ptr<DocState> doc;
    std::shared_ptr<std::vector<unsigned char>> bytes;
};

struct RenderResult {
    int generation{0};
    int index{-1};
    int width{0};
    QImage image;
};

} // namespace

struct PdfWindow::Impl {
    QWidget* caption{nullptr};
    QLabel* captionTitle{nullptr};
    CaptionButton* captionMax{nullptr};
    QPushButton* pop{nullptr};
    QLabel* title{nullptr};
    QLabel* pageLabel{nullptr};      // header: "12 pages"
    QLabel* zoomLabel{nullptr};
    QLabel* message{nullptr};
    QWidget* header{nullptr};
    QWidget* footer{nullptr};
    QLineEdit* pageField{nullptr};   // footer: current page, editable
    QLabel* pageTotal{nullptr};      // footer: "/ 12"
    QPushButton* previous{nullptr};
    QPushButton* next{nullptr};
    QPushButton* fitWidth{nullptr};
    QPushButton* fitPage{nullptr};
    QPushButton* zoomOut{nullptr};
    QPushButton* zoomIn{nullptr};
    QPushButton* rotate{nullptr};
    PdfFit fit{PdfFit::Width};
    int rotation{0};
    QString titleText;
    // restyle() writes stylesheets, which can emit another PaletteChange; without
    // these two the reader recursed into itself until the stack died.
    bool restyling{false};
    QPalette appliedPalette;
    // Icon-only buttons carry their glyph name so a palette change can re-tint
    // them; icons::lucideSolid bakes the colour into the pixmap.
    std::vector<std::pair<QPushButton*, QString>> iconButtons;
    QStackedWidget* stack{nullptr};
    QScrollArea* scroll{nullptr};
    QWidget* host{nullptr};
    QVBoxLayout* pagesLayout{nullptr};
    QFutureWatcher<LoadResult>* loadWatcher{nullptr};
    QFutureWatcher<RenderResult>* renderWatcher{nullptr};
    std::vector<PageCanvas*> pages;
    std::vector<QSizeF> sizes;
    std::shared_ptr<DocState> doc;
    std::shared_ptr<std::vector<unsigned char>> pending;
    double zoom{1.0};
    int generation{0};
    bool rendering{false};
    qreal devicePixelRatio{1.0};
    bool dragging{false};
    QPoint dragOrigin;
    int dragScrollX{0};
    int dragScrollY{0};
};

PdfWindow::PdfWindow(QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {
    setFocusPolicy(Qt::StrongFocus);
    setObjectName(QStringLiteral("pdfWindow"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Frameless caption, shown only while the reader is a separate window.
    impl_->caption = new QWidget(this);
    impl_->caption->setFixedHeight(32);
    impl_->caption->setObjectName(QStringLiteral("pdfCaption"));
    impl_->caption->setAttribute(Qt::WA_StyledBackground, true);
    auto* captionLayout = new QHBoxLayout(impl_->caption);
    captionLayout->setContentsMargins(14, 0, 0, 0);
    captionLayout->setSpacing(0);
    impl_->captionTitle = new QLabel(QStringLiteral("PDF"), impl_->caption);
    impl_->captionTitle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    captionLayout->addWidget(impl_->captionTitle, 1);
    auto* minimize = new CaptionButton(CaptionGlyph::Minimize, impl_->caption);
    impl_->captionMax = new CaptionButton(CaptionGlyph::Maximize, impl_->caption);
    auto* closeCaption = new CaptionButton(CaptionGlyph::Close, impl_->caption);
    captionLayout->addWidget(minimize);
    captionLayout->addWidget(impl_->captionMax);
    captionLayout->addWidget(closeCaption);
    connect(minimize, &QAbstractButton::clicked, this, [this] { showMinimized(); });
    connect(impl_->captionMax, &QAbstractButton::clicked, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(closeCaption, &QAbstractButton::clicked, this, &PdfWindow::closeRequested);
    impl_->caption->hide();
    root->addWidget(impl_->caption);

    // Header: back, document name, page count, pop out/dock.
    impl_->header = new QWidget(this);
    impl_->header->setObjectName(QStringLiteral("pdfHeader"));
    impl_->header->setAttribute(Qt::WA_StyledBackground, true);
    auto* headerLayout = new QHBoxLayout(impl_->header);
    headerLayout->setContentsMargins(10, 7, 10, 7);
    headerLayout->setSpacing(10);
    auto* back = new QPushButton(QStringLiteral("Back"), impl_->header);
    back->setProperty("quiet", true);
    back->setCursor(Qt::PointingHandCursor);
    back->setFocusPolicy(Qt::NoFocus);
    impl_->title = new QLabel(impl_->header);
    impl_->title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    impl_->title->setMinimumWidth(0);
    impl_->pageLabel = new QLabel(impl_->header);
    impl_->pop = new QPushButton(QStringLiteral("Pop out"), impl_->header);
    impl_->pop->setProperty("quiet", true);
    impl_->pop->setCursor(Qt::PointingHandCursor);
    impl_->pop->setFocusPolicy(Qt::NoFocus);
    impl_->pop->setToolTip(QStringLiteral("Open in a window you can move and resize"));
    headerLayout->addWidget(back);
    headerLayout->addWidget(impl_->title, 1);
    headerLayout->addWidget(impl_->pageLabel);
    headerLayout->addWidget(impl_->pop);
    root->addWidget(impl_->header);

    impl_->stack = new QStackedWidget(this);
    auto* messagePage = new QWidget(impl_->stack);
    auto* messageLayout = new QVBoxLayout(messagePage);
    impl_->message = new QLabel(messagePage);
    impl_->message->setAlignment(Qt::AlignCenter);
    impl_->message->setWordWrap(true);
    impl_->message->setText(QStringLiteral(
        "No document is open.\nPick a PDF from the library to read it here."));
    messageLayout->addWidget(impl_->message);
    impl_->stack->addWidget(messagePage);

    impl_->scroll = new QScrollArea(impl_->stack);
    impl_->scroll->setObjectName(QStringLiteral("pdfScroll"));
    impl_->scroll->setFrameShape(QFrame::NoFrame);
    impl_->scroll->setWidgetResizable(true);
    impl_->host = new QWidget(impl_->scroll);
    impl_->host->setObjectName(QStringLiteral("pdfHost"));
    impl_->host->setAttribute(Qt::WA_StyledBackground, true);
    impl_->pagesLayout = new QVBoxLayout(impl_->host);
    impl_->pagesLayout->setContentsMargins(24, 20, 24, 28);
    impl_->pagesLayout->setSpacing(20);
    impl_->pagesLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    impl_->host->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    impl_->scroll->setWidget(impl_->host);
    impl_->scroll->viewport()->setCursor(Qt::OpenHandCursor);
    impl_->host->setCursor(Qt::OpenHandCursor);
    impl_->scroll->viewport()->installEventFilter(this);
    impl_->host->installEventFilter(this);
    impl_->stack->addWidget(impl_->scroll);
    root->addWidget(impl_->stack, 1);

    // Footer: page navigation, zoom and fit modes on one line, hint on the right.
    impl_->footer = new QWidget(this);
    impl_->footer->setObjectName(QStringLiteral("pdfFooter"));
    impl_->footer->setAttribute(Qt::WA_StyledBackground, true);
    auto* footerLayout = new QHBoxLayout(impl_->footer);
    footerLayout->setContentsMargins(10, 6, 12, 8);
    footerLayout->setSpacing(6);
    impl_->previous = new QPushButton(impl_->footer);
    impl_->next = new QPushButton(impl_->footer);
    impl_->pageField = new QLineEdit(impl_->footer);
    impl_->pageField->setObjectName(QStringLiteral("pdfPageField"));
    impl_->pageField->setAlignment(Qt::AlignCenter);
    impl_->pageField->setFixedWidth(46);
    impl_->pageField->setMaxLength(5);
    impl_->pageField->setValidator(new QIntValidator(1, 99999, impl_->pageField));
    impl_->pageField->setToolTip(QStringLiteral("Type a page number and press Enter"));
    impl_->pageField->setAccessibleName(QStringLiteral("Current page"));
    impl_->pageTotal = new QLabel(impl_->footer);
    auto* zoomOut = new QPushButton(impl_->footer);
    impl_->zoomOut = zoomOut;
    impl_->zoomLabel = new QLabel(QStringLiteral("Fit width"), impl_->footer);
    impl_->zoomLabel->setAlignment(Qt::AlignCenter);
    impl_->zoomLabel->setMinimumWidth(64);
    auto* zoomIn = new QPushButton(impl_->footer);
    impl_->zoomIn = zoomIn;
    auto* fit = new QPushButton(impl_->footer);
    impl_->fitWidth = fit;
    impl_->fitPage = new QPushButton(impl_->footer);
    auto* rotate = new QPushButton(impl_->footer);
    impl_->rotate = rotate;
    auto* hint = new QLabel(QStringLiteral("Ctrl + wheel zooms · drag to pan"), impl_->footer);
    hint->setObjectName(QStringLiteral("pdfHint"));
    hint->setVisible(false);
    for (auto* button : {zoomOut, zoomIn, fit, impl_->fitPage, impl_->previous,
             impl_->next, rotate}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
    }
    zoomOut->setToolTip(QStringLiteral("Zoom out"));
    zoomIn->setToolTip(QStringLiteral("Zoom in"));
    fit->setToolTip(QStringLiteral("Fit the page width to the window"));
    impl_->fitPage->setToolTip(QStringLiteral("Fit the whole page in the window"));
    rotate->setToolTip(QStringLiteral("Rotate the page 90 degrees"));
    impl_->previous->setToolTip(QStringLiteral("Previous page"));
    impl_->next->setToolTip(QStringLiteral("Next page"));
    impl_->previous->setAccessibleName(QStringLiteral("Previous page"));
    impl_->next->setAccessibleName(QStringLiteral("Next page"));
    for (auto* button : {zoomOut, zoomIn, fit, impl_->fitPage, impl_->previous,
             impl_->next, rotate}) {
        button->setProperty("quiet", true);
    }
    fit->setCheckable(true);
    impl_->fitPage->setCheckable(true);
    fit->setChecked(true);

    struct ViewerControl {
        QPushButton* button;
        const char* icon;
        const char* tooltip;
        bool glyphOnly;
    };
    for (const auto& control : {
             ViewerControl{back, "arrow-left", "Back to the library", false},
             ViewerControl{impl_->pop, "external-link", "Open in a window you can move and resize", false},
             ViewerControl{impl_->previous, "chevron-left", "Previous page", true},
             ViewerControl{impl_->next, "chevron-right", "Next page", true},
             ViewerControl{zoomOut, "minus", "Zoom out", true},
             ViewerControl{zoomIn, "plus", "Zoom in", true},
             ViewerControl{fit, "scan", "Fit the page width to the window", true},
             ViewerControl{impl_->fitPage, "maximize", "Fit the whole page in the window", true},
             ViewerControl{rotate, "rotate-ccw", "Rotate the page 90 degrees", true}}) {
        auto* button = control.button;
        button->setToolTip(QString::fromLatin1(control.tooltip));
        button->setAccessibleName(QString::fromLatin1(control.tooltip));
        button->setIconSize(QSize(16, 16));
        button->setIcon(icons::lucideSolid(QString::fromLatin1(control.icon), pdfSecondary()));
        impl_->iconButtons.push_back({button, QString::fromLatin1(control.icon)});
        if (control.glyphOnly) {
            button->setText(QString());
            button->setStyleSheet(QStringLiteral(
                "QPushButton { padding: 0px; min-width: 30px; min-height: 28px; }"));
        }
    }

    auto* divider = new QWidget(impl_->footer);
    divider->setObjectName(QStringLiteral("pdfDivider"));
    divider->setFixedWidth(1);
    divider->setFixedHeight(18);

    footerLayout->addWidget(impl_->previous);
    footerLayout->addWidget(impl_->pageField);
    footerLayout->addWidget(impl_->pageTotal);
    footerLayout->addWidget(impl_->next);
    footerLayout->addSpacing(4);
    footerLayout->addWidget(divider);
    footerLayout->addSpacing(4);
    footerLayout->addWidget(zoomOut);
    footerLayout->addWidget(impl_->zoomLabel);
    footerLayout->addWidget(zoomIn);
    footerLayout->addWidget(fit);
    footerLayout->addWidget(impl_->fitPage);
    footerLayout->addWidget(rotate);
    footerLayout->addStretch(1);
    footerLayout->addWidget(hint);
    root->addWidget(impl_->footer);
    restyle();

    // Nothing to navigate or zoom until a document opens: the transport and the
    // page field start disabled instead of looking broken.
    setControlsEnabled(false);
    impl_->loadWatcher = new QFutureWatcher<LoadResult>(this);
    impl_->renderWatcher = new QFutureWatcher<RenderResult>(this);
    connect(impl_->loadWatcher, &QFutureWatcherBase::finished, this, [this] { applyLoad(); });
    connect(impl_->renderWatcher, &QFutureWatcherBase::finished, this, [this] { applyRender(); });
    connect(impl_->scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        updatePageLabel();
        pump();
    });
    connect(back, &QPushButton::clicked, this, &PdfWindow::closeRequested);
    connect(impl_->pop, &QPushButton::clicked, this, [this] {
        if (detached_) {
            emit dockRequested();
        } else {
            emit detachRequested();
        }
    });
    connect(impl_->previous, &QPushButton::clicked, this, &PdfWindow::previousRequested);
    connect(impl_->next, &QPushButton::clicked, this, &PdfWindow::nextRequested);
    connect(zoomOut, &QPushButton::clicked, this, [this] { zoomBy(0.8); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { zoomBy(1.25); });
    connect(fit, &QPushButton::clicked, this, [this] { setFitMode(PdfFit::Width); });
    connect(impl_->fitPage, &QPushButton::clicked, this, [this] { setFitMode(PdfFit::Page); });
    connect(rotate, &QPushButton::clicked, this, [this] {
        const auto anchor = scrollAnchor();
        impl_->rotation = (impl_->rotation + 90) % 360;
        for (auto* page : impl_->pages) {
            page->rotation = impl_->rotation;
            page->update();
        }
        relayout();
        restoreScrollAnchor(anchor.first, anchor.second);
        pump();
    });
    connect(impl_->pageField, &QLineEdit::returnPressed, this, [this] {
        goToPage(impl_->pageField->text().toInt() - 1);
        setFocus();
    });
    setNeighbors(false, false);
}

PdfWindow::~PdfWindow() {
    if (impl_ != nullptr && impl_->doc != nullptr) {
        impl_->doc->alive.store(false);
    }
}

void PdfWindow::openDocument(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::int64_t id,
    const QString& title) {
    id_ = id;
    const QString shown = title.isEmpty() ? QStringLiteral("PDF") : title;
    impl_->titleText = shown;
    impl_->title->setText(impl_->title->fontMetrics().elidedText(
        shown, Qt::ElideMiddle, std::max(0, impl_->title->width() - 4)));
    impl_->title->setToolTip(shown);
    impl_->captionTitle->setText(shown);
    setWindowTitle(shown);
    impl_->pageLabel->clear();
    impl_->pageTotal->clear();
    impl_->pageField->clear();
    impl_->rotation = 0;
    impl_->fit = PdfFit::Width;
    impl_->zoom = 1.0;
    if (impl_->fitWidth != nullptr && impl_->fitPage != nullptr) {
        const QSignalBlocker blockWidth(impl_->fitWidth);
        const QSignalBlocker blockPage(impl_->fitPage);
        impl_->fitWidth->setChecked(true);
        impl_->fitPage->setChecked(false);
    }
    impl_->zoomLabel->setText(QStringLiteral("Fit width"));
    impl_->devicePixelRatio = devicePixelRatioF();
    ++impl_->generation;
    impl_->rendering = false;
    impl_->pending.reset();
    if (impl_->doc != nullptr) {
        impl_->doc->alive.store(false);
        impl_->doc.reset();
    }
    for (auto* page : impl_->pages) {
        page->deleteLater();
    }
    impl_->pages.clear();
    impl_->sizes.clear();
    impl_->message->setText(QStringLiteral("Opening…"));
    impl_->stack->setCurrentIndex(0);
    setControlsEnabled(false);
    if (vault == nullptr) {
        impl_->message->setText(QStringLiteral("The vault is locked."));
        return;
    }
    const int generation = impl_->generation;
    impl_->loadWatcher->setFuture(QtConcurrent::run([vault, id, generation] {
        LoadResult result;
        result.generation = generation;
        std::vector<unsigned char> plain;
        std::uint64_t offset = 0;
        constexpr std::size_t kChunk = 1024U * 1024U;
        QString decryptError;
        for (;;) {
            auto chunk = vault->read_video_range(id, offset, kChunk);
            if (!chunk) {
                decryptError = QString::fromUtf8(
                    videovault::core::user_message(chunk.error().code).data());
                break;
            }
            if (chunk.value().empty()) {
                break;
            }
            plain.insert(plain.end(), chunk.value().begin(), chunk.value().end());
            offset += static_cast<std::uint64_t>(chunk.value().size());
            if (chunk.value().size() < kChunk) {
                break;
            }
            if (plain.size() > 256U * 1024U * 1024U) {
                decryptError = QStringLiteral("This PDF is too large to open.");
                plain.clear();
                break;
            }
        }
        if (!decryptError.isEmpty()) {
            result.error = decryptError;
            return result;
        }
        if (plain.empty()) {
            result.error = QStringLiteral("This PDF could not be decrypted.");
            return result;
        }
        result.bytes = std::make_shared<std::vector<unsigned char>>(std::move(plain));
#ifdef Q_OS_WIN
        auto opened = openPdf(*result.bytes, {});
        result.encrypted = opened.needsPassword;
        result.doc = std::move(opened.doc);
        result.sizes = std::move(opened.sizes);
        result.error = std::move(opened.error);
        result.badPassword = opened.badPassword;
        if (result.doc != nullptr || result.encrypted) {
            if (result.doc != nullptr) {
                result.bytes.reset();
            }
        }
#else
        result.error = QStringLiteral("Open this PDF on Windows to read it.");
#endif
        return result;
    }));
}

void PdfWindow::setControlsEnabled(const bool enabled) {
    for (auto* button : {impl_->previous, impl_->next, impl_->zoomOut, impl_->zoomIn,
             impl_->fitWidth, impl_->fitPage, impl_->rotate}) {
        if (button != nullptr) {
            button->setEnabled(enabled);
        }
    }
    // The page group only means something with a document: hide it rather than
    // leave an empty field and a stray zoom label on an idle reader.
    if (impl_->pageField != nullptr) {
        impl_->pageField->setVisible(enabled);
    }
    if (impl_->pageTotal != nullptr) {
        impl_->pageTotal->setVisible(enabled);
    }
    if (impl_->zoomLabel != nullptr) {
        impl_->zoomLabel->setVisible(enabled);
    }
}

void PdfWindow::setNeighbors(const bool previous, const bool next) {
    impl_->previous->setEnabled(previous);
    impl_->next->setEnabled(next);
}

void PdfWindow::applyLoad() {
    const auto result = impl_->loadWatcher->result();
    if (result.generation != impl_->generation) {
        return;
    }
    if (result.encrypted && result.doc == nullptr && result.error.isEmpty()) {
        impl_->pending = result.bytes;
        askPassword(false);
        return;
    }
    if (result.badPassword) {
        impl_->pending = result.bytes != nullptr ? result.bytes : impl_->pending;
        askPassword(true);
        return;
    }
    if (result.doc == nullptr || result.sizes.empty()) {
        impl_->message->setText(result.error.isEmpty()
            ? QStringLiteral("This PDF could not be opened.")
            : result.error);
        impl_->stack->setCurrentIndex(0);
        return;
    }
    impl_->pending.reset();
    impl_->doc = result.doc;
    impl_->sizes = result.sizes;
    impl_->pages.clear();
    impl_->pages.reserve(result.sizes.size());
    for (std::size_t i = 0U; i < result.sizes.size(); ++i) {
        auto* page = new PageCanvas(impl_->host);
        impl_->pagesLayout->addWidget(page, 0, Qt::AlignHCenter);
        impl_->pages.push_back(page);
    }
    impl_->stack->setCurrentWidget(impl_->scroll);
    setControlsEnabled(true);
    for (auto* page : impl_->pages) {
        page->rotation = impl_->rotation;
    }
    relayout();
    updatePageLabel();
    QTimer::singleShot(0, this, [this] { pump(); });
    setFocus();
}

void PdfWindow::askPassword(const bool rejected) {
    if (impl_->pending == nullptr) {
        impl_->message->setText(QStringLiteral("This PDF is password protected."));
        impl_->stack->setCurrentIndex(0);
        return;
    }
    InWindowSheet dialog(this);
    dialog.setTitle(QStringLiteral("PDF password"));
    dialog.setMessage(rejected
        ? QStringLiteral("That password did not open this PDF. Try again.")
        : QStringLiteral("This PDF is password protected."));
    dialog.addTextEntry(QLineEdit::Password, QString(), QStringLiteral("Password"));
    dialog.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    dialog.addButton(QStringLiteral("Open"), 1, true, QStringLiteral("folder-open"));
    if (dialog.run() != 1) {
        impl_->pending.reset();
        impl_->message->setText(QStringLiteral("This PDF is password protected."));
        impl_->stack->setCurrentIndex(0);
        return;
    }
    QString password = dialog.text();
    const int generation = impl_->generation;
    const auto bytes = impl_->pending;
    impl_->message->setText(QStringLiteral("Opening…"));
    impl_->stack->setCurrentIndex(0);
    impl_->loadWatcher->setFuture(QtConcurrent::run([bytes, password = std::move(password), generation] {
        LoadResult result;
        result.generation = generation;
        result.bytes = bytes;
        result.encrypted = true;
#ifdef Q_OS_WIN
        auto opened = openPdf(*bytes, password);
        result.doc = std::move(opened.doc);
        result.sizes = std::move(opened.sizes);
        result.error = std::move(opened.error);
        result.encrypted = opened.needsPassword;
        result.badPassword = opened.badPassword;
        if (result.doc != nullptr) {
            result.bytes.reset();
        }
#else
        Q_UNUSED(password)
        result.error = QStringLiteral("Open this PDF on Windows to read it.");
#endif
        return result;
    }));
}

void PdfWindow::relayout() {
    if (impl_ == nullptr || impl_->scroll == nullptr || impl_->pagesLayout == nullptr
        || impl_->pages.size() != impl_->sizes.size()) {
        return;
    }
    const int width = pageWidth();
    for (std::size_t i = 0U; i < impl_->pages.size(); ++i) {
        const auto& size = impl_->sizes[i];
        const double aspect = size.height() > 1.0 ? size.width() / size.height() : 0.77;
        const bool sideways = impl_->rotation == 90 || impl_->rotation == 270;
        const double shown = sideways ? 1.0 / aspect : aspect;
        const int height = std::max(40, static_cast<int>(static_cast<double>(width) / shown));
        impl_->pages[i]->rotation = impl_->rotation;
        impl_->pages[i]->setFixedSize(width + 2 * kPageMargin, height + 2 * kPageMargin);
    }
    //impl_->host->setMinimumWidth(width + 2 * kPageMargin + 48);
    int contentHeight = impl_->pagesLayout->contentsMargins().top()
        + impl_->pagesLayout->contentsMargins().bottom();
    if (!impl_->pages.empty()) {
        contentHeight += impl_->pagesLayout->spacing() * static_cast<int>(impl_->pages.size() - 1U);
    }
    for (auto* page : impl_->pages) {
        contentHeight += page->height();
    }
    //impl_->host->setMinimumHeight(contentHeight);
    // Page positions are read by the anchor helpers, so settle the layout now.
    impl_->pagesLayout->activate();

    if (impl_->fit == PdfFit::Custom) {
        double points = 595.0;
        if (!impl_->sizes.empty()) {
            const auto index = std::min<std::size_t>(
                static_cast<std::size_t>(scrollAnchor().first), impl_->sizes.size() - 1U);
            const auto& size = impl_->sizes[index];
            points = (impl_->rotation == 90 || impl_->rotation == 270)
                ? std::max(1.0, size.height()) : std::max(1.0, size.width());
        }
        impl_->zoomLabel->setText(QStringLiteral("%1%").arg(qRound(100.0 * width / points)));
    } else {
        impl_->zoomLabel->setText(impl_->fit == PdfFit::Page
            ? QStringLiteral("Fit page") : QStringLiteral("Fit width"));
    }
}

int PdfWindow::pageWidth() const {
    if (impl_ == nullptr || impl_->scroll == nullptr) {
        return 120;
    }
    const int viewportW = impl_->scroll->viewport()->width();
    const int viewportH = impl_->scroll->viewport()->height();
    const int usableW = std::max(120, viewportW - 48 - 2 * kPageMargin);
    if (impl_->fit == PdfFit::Page && !impl_->sizes.empty()) {
        const auto index = std::min<std::size_t>(
            static_cast<std::size_t>(scrollAnchor().first), impl_->sizes.size() - 1U);
        const auto& size = impl_->sizes[index];
        double aspect = size.height() > 1.0 ? size.width() / size.height() : 0.77;
        if (impl_->rotation == 90 || impl_->rotation == 270) {
            aspect = 1.0 / aspect;
        }
        // The tallest width that still leaves the whole page on screen.
        const int usableH = std::max(120, viewportH - 40 - 2 * kPageMargin);
        return std::clamp(std::min(usableW, static_cast<int>(usableH * aspect)), 120, 2400);
    }
    return std::clamp(static_cast<int>(static_cast<double>(usableW) * impl_->zoom), 120, 2400);
}

std::pair<int, double> PdfWindow::scrollAnchor() const {
    if (impl_ == nullptr || impl_->scroll == nullptr || impl_->pages.empty()) {
        return {0, 0.0};
    }
    const int top = impl_->scroll->verticalScrollBar()->value();
    int page = 0;
    for (int i = 0; i < static_cast<int>(impl_->pages.size()); ++i) {
        if (impl_->pages[static_cast<std::size_t>(i)]->y() <= top) {
            page = i;
        } else {
            break;
        }
    }
    auto* current = impl_->pages[static_cast<std::size_t>(page)];
    const double fraction = current->height() > 0
        ? std::clamp(static_cast<double>(top - current->y()) / current->height(), 0.0, 1.0)
        : 0.0;
    return {page, fraction};
}

void PdfWindow::restoreScrollAnchor(const int page, const double fraction) {
    if (impl_->pages.empty()) {
        return;
    }
    const int index = std::clamp(page, 0, static_cast<int>(impl_->pages.size()) - 1);
    auto* target = impl_->pages[static_cast<std::size_t>(index)];
    impl_->scroll->verticalScrollBar()->setValue(
        target->y() + static_cast<int>(fraction * target->height()));
    updatePageLabel();
}

void PdfWindow::zoomBy(const double factor) {
    // Zoom is expressed against the fit-width base, whichever mode is active, so
    // zooming out of Fit page continues from the size actually on screen.
    const int viewportW = impl_->scroll->viewport()->width();
    const int usableW = std::max(1, viewportW - 48 - 2 * kPageMargin);
    const double current = static_cast<double>(pageWidth()) / usableW;
    setZoom(std::clamp(current * factor, 0.5, 4.0));
}

void PdfWindow::setZoom(const double zoom) {
    const auto anchor = scrollAnchor();
    impl_->zoom = zoom;
    impl_->fit = PdfFit::Custom;
    if (impl_->fitWidth != nullptr && impl_->fitPage != nullptr) {
        const QSignalBlocker blockWidth(impl_->fitWidth);
        const QSignalBlocker blockPage(impl_->fitPage);
        impl_->fitWidth->setChecked(false);
        impl_->fitPage->setChecked(false);
    }
    for (auto* page : impl_->pages) {
        page->renderedWidth = 0;
    }
    relayout();
    restoreScrollAnchor(anchor.first, anchor.second);
    pump();
}

void PdfWindow::setFitMode(const PdfFit mode) {
    const auto anchor = scrollAnchor();
    impl_->fit = mode;
    impl_->zoom = 1.0;
    if (impl_->fitWidth != nullptr && impl_->fitPage != nullptr) {
        const QSignalBlocker blockWidth(impl_->fitWidth);
        const QSignalBlocker blockPage(impl_->fitPage);
        impl_->fitWidth->setChecked(mode == PdfFit::Width);
        impl_->fitPage->setChecked(mode == PdfFit::Page);
    }
    for (auto* page : impl_->pages) {
        page->renderedWidth = 0;
    }
    relayout();
    restoreScrollAnchor(anchor.first, anchor.second);
    pump();
}

void PdfWindow::goToPage(int index) {
    if (impl_->pages.empty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(impl_->pages.size()) - 1);
    auto* page = impl_->pages[static_cast<std::size_t>(index)];
    impl_->scroll->verticalScrollBar()->setValue(
        std::max(0, page->y() - impl_->pagesLayout->spacing()));
    updatePageLabel();
    pump();
}

void PdfWindow::updatePageLabel() {
    if (impl_->pages.empty()) {
        impl_->pageLabel->clear();
        impl_->pageTotal->clear();
        return;
    }
    const int mark = impl_->scroll->verticalScrollBar()->value()
        + impl_->scroll->viewport()->height() / 3;
    int current = 0;
    for (int i = 0; i < static_cast<int>(impl_->pages.size()); ++i) {
        if (impl_->pages[static_cast<std::size_t>(i)]->y() <= mark) {
            current = i;
        }
    }
    impl_->pageLabel->setText(QStringLiteral("%1 page%2")
        .arg(impl_->pages.size())
        .arg(impl_->pages.size() == 1U ? QString() : QStringLiteral("s")));
    impl_->pageTotal->setText(QStringLiteral("/ %1").arg(impl_->pages.size()));
    // The field is the first focusable widget in the footer, so it can hold focus
    // the moment the reader opens; only a user edit may suppress the sync.
    if (!impl_->pageField->hasFocus() || !impl_->pageField->isModified()) {
        const QSignalBlocker block(impl_->pageField);
        impl_->pageField->setText(QString::number(current + 1));
        impl_->pageField->setModified(false);
    }
}

// One place that paints every surface from the palette. Called once when the
// window is built and again on every palette change, so the reader follows the
// Light/Dark toggle without being reopened.
void PdfWindow::restyle() {
    // Called from the constructor and on every palette change; a resize or a
    // palette event can arrive while the footer is still being built, so every
    // surface is checked rather than assumed.
    if (impl_ == nullptr || impl_->header == nullptr || impl_->footer == nullptr
        || impl_->scroll == nullptr || impl_->host == nullptr || impl_->title == nullptr
        || impl_->pageLabel == nullptr || impl_->pageTotal == nullptr
        || impl_->zoomLabel == nullptr || impl_->message == nullptr
        || impl_->pageField == nullptr || impl_->restyling) {
        return;
    }
    impl_->restyling = true;
    const QColor backdrop = pdfBackdrop();
    const QColor chrome = pdfChrome();
    const QColor ink = pdfInk();
    const QColor secondary = pdfSecondary();
    const QColor hairline = pdfHairline();
    setStyleSheet(QStringLiteral("QWidget#pdfWindow { background: %1; }").arg(backdrop.name()));
    if (impl_->caption != nullptr) {
        impl_->caption->setStyleSheet(QStringLiteral(
            "QWidget#pdfCaption { background: %1; border: none; }").arg(chrome.name()));
        impl_->captionTitle->setStyleSheet(QStringLiteral(
            "color: %1; background: transparent;").arg(secondary.name()));
    }
    impl_->header->setStyleSheet(QStringLiteral(
        "QWidget#pdfHeader { background: %1; border-bottom: 1px solid %2; }")
        .arg(chrome.name(), hairline.name()));
    impl_->footer->setStyleSheet(QStringLiteral(
        "QWidget#pdfFooter { background: %1; border-top: 1px solid %2; }")
        .arg(chrome.name(), hairline.name()));
    impl_->scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#pdfScroll { background: %1; border: none; }"
        "QScrollArea#pdfScroll > QWidget > QWidget { background: %1; }").arg(backdrop.name()));
    impl_->host->setStyleSheet(QStringLiteral("QWidget#pdfHost { background: %1; }")
        .arg(backdrop.name()));
    impl_->title->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 15px; font-weight: 600; background: transparent;")
        .arg(ink.name()));
    impl_->pageLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
        .arg(secondary.name()));
    impl_->pageTotal->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
        .arg(secondary.name()));
    impl_->zoomLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
        .arg(ink.name()));
    impl_->message->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 15px; background: transparent;").arg(secondary.name()));
    impl_->pageField->setStyleSheet(QStringLiteral(
        "QLineEdit#pdfPageField { color: %1; background: %2; border: 1px solid %3;"
        " border-radius: 7px; padding: 3px 4px; min-height: 20px; }"
        "QLineEdit#pdfPageField:focus { border-color: %4; }")
        .arg(ink.name(), backdrop.name(), hairline.name(),
            QApplication::palette().color(QPalette::Highlight).name()));
    if (auto* divider = impl_->footer->findChild<QWidget*>(QStringLiteral("pdfDivider"))) {
        divider->setStyleSheet(QStringLiteral("background: %1;").arg(hairline.name()));
    }
    if (auto* hint = impl_->footer->findChild<QLabel*>(QStringLiteral("pdfHint"))) {
        hint->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
            .arg(secondary.name()));
    }
    for (auto& entry : impl_->iconButtons) {
        if (entry.first != nullptr && !entry.second.isEmpty()) {
            entry.first->setIcon(icons::lucideSolid(entry.second, secondary));
        }
    }
    for (auto* page : impl_->pages) {
        page->update();
    }
    impl_->appliedPalette = palette();
    impl_->restyling = false;
}

void PdfWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (impl_ == nullptr || impl_->scroll == nullptr || impl_->footer == nullptr
        || impl_->title == nullptr || impl_->pagesLayout == nullptr) {
        return;
    }
    // The title elides against the width the layout actually gave it, and the
    // keyboard hint only shows where there is room for it.
    if (impl_->title != nullptr) {
        const int available = std::max(0, impl_->title->width() - 4);
        impl_->title->setText(impl_->title->fontMetrics().elidedText(
            impl_->titleText, Qt::ElideMiddle, available));
        impl_->title->setToolTip(impl_->titleText);
    }
    if (auto* hint = impl_->footer->findChild<QLabel*>(QStringLiteral("pdfHint"))) {
        hint->setVisible(width() >= 820);
    }
    // Both fit modes depend on the viewport, so a resize always re-derives the
    // page width; page positions change, so keep the reader where they were.
    const auto anchor = scrollAnchor();
    relayout();
    restoreScrollAnchor(anchor.first, anchor.second);
    pump();
}

void PdfWindow::pump() {
    if (impl_->rendering || impl_->doc == nullptr || impl_->pages.empty()) {
        return;
    }
    const int width = pageWidth();
    if (width < 80) {
        return;
    }
    const int top = impl_->scroll->verticalScrollBar()->value();
    const int bottom = top + impl_->scroll->viewport()->height();
    int next = -1;
    for (int i = 0; i < static_cast<int>(impl_->pages.size()); ++i) {
        auto* page = impl_->pages[static_cast<std::size_t>(i)];
        const int y = page->y();
        if (y + page->height() < top - 400) {
            continue;
        }
        if (y > bottom + 400) {
            break;
        }
        if (page->image.isNull() || std::abs(page->renderedWidth - width) > 24) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        return;
    }
    impl_->rendering = true;
    const int generation = impl_->generation;
    const auto state = impl_->doc;
    const qreal ratio = std::max<qreal>(1.0, impl_->devicePixelRatio);
    const int pixels = std::clamp(static_cast<int>(width * ratio), width, 2048);
    impl_->renderWatcher->setFuture(QtConcurrent::run([state, next, width, pixels, generation] {
        RenderResult result;
        result.generation = generation;
        result.index = next;
        result.width = width;
#ifdef Q_OS_WIN
        result.image = renderPage(state, next, pixels);
#else
        Q_UNUSED(state)
        Q_UNUSED(pixels)
#endif
        return result;
    }));
}

void PdfWindow::applyRender() {
    impl_->rendering = false;
    const auto result = impl_->renderWatcher->result();
    if (result.generation == impl_->generation
        && result.index >= 0
        && result.index < static_cast<int>(impl_->pages.size())
        && !result.image.isNull()) {
        auto* page = impl_->pages[static_cast<std::size_t>(result.index)];
        page->image = result.image;
        page->renderedWidth = result.width;
        page->update();
    }
    pump();
}

void PdfWindow::updatePopButton() {
    if (impl_ == nullptr || impl_->pop == nullptr) {
        return;
    }
    impl_->pop->setText(detached_ ? QStringLiteral("Dock") : QStringLiteral("Pop out"));
    impl_->pop->setToolTip(detached_
        ? QStringLiteral("Put the PDF back in the library")
        : QStringLiteral("Open in a window you can move and resize"));
}

void PdfWindow::prepareDock() {
    if (isMaximized()) {
        showNormal();
    }
    detached_ = false;
    frameReady_ = false;
    setMinimumSize(0, 0);
    setWindowFlags(Qt::Widget);
    if (impl_->caption != nullptr) {
        impl_->caption->hide();
    }
    updatePopButton();
}

void PdfWindow::detach() {
    if (isMaximized()) {
        showNormal();
    }
    detached_ = true;
    setParent(nullptr);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint
        | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    setMinimumSize(640, 480);
    resize(960, 720);
    if (impl_->caption != nullptr) {
        impl_->caption->show();
    }
    updatePopButton();
    if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect area = screen->availableGeometry();
        move(area.center() - QPoint(width() / 2, height() / 2));
    }
    show();
    raise();
    activateWindow();
}

void PdfWindow::applyDetachedFrame() {
#ifdef Q_OS_WIN
    if (!detached_ || frameReady_) {
        return;
    }
    frameReady_ = true;
    HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    const MARGINS shadow{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    const int corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(this)
#endif
}

void PdfWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (detached_) {
        applyDetachedFrame();
    }
}

void PdfWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange && impl_ != nullptr
        && impl_->header != nullptr && !impl_->restyling
        && palette() != impl_->appliedPalette) {
        restyle();
    }
    if (event->type() == QEvent::WindowStateChange && impl_->captionMax != nullptr) {
        impl_->captionMax->setGlyph(isMaximized() ? CaptionGlyph::Restore : CaptionGlyph::Maximize);
    }
    QWidget::changeEvent(event);
}

bool PdfWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    if (detached_ && eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
            if (IsZoomed(msg->hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                const int pad = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                params->rgrc[0].left += pad;
                params->rgrc[0].top += pad;
                params->rgrc[0].right -= pad;
                params->rgrc[0].bottom -= pad;
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST && impl_->caption != nullptr && impl_->caption->isVisible()) {
            const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            const QPoint pos = mapFromGlobal(global);
            const bool zoomed = IsZoomed(msg->hwnd);
            const QPoint inCaption = impl_->caption->mapFromGlobal(global);
            if (impl_->caption->rect().contains(inCaption)) {
                QWidget* child = impl_->caption->childAt(inCaption);
                if (qobject_cast<QAbstractButton*>(child) != nullptr) {
                    *result = HTCLIENT;
                } else if (!zoomed && inCaption.y() < 4) {
                    *result = HTTOP;
                } else {
                    *result = HTCAPTION;
                }
                return true;
            }
            if (!zoomed) {
                constexpr int border = 6;
                const bool left = pos.x() < border;
                const bool right = pos.x() >= width() - border;
                const bool bottom = pos.y() >= height() - border;
                if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
                if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
                if (left) { *result = HTLEFT; return true; }
                if (right) { *result = HTRIGHT; return true; }
                if (bottom) { *result = HTBOTTOM; return true; }
            }
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void PdfWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit closeRequested();
        return;
    }
    if ((event->key() == Qt::Key_Left) && impl_->previous->isEnabled()) {
        emit previousRequested();
        return;
    }
    if ((event->key() == Qt::Key_Right) && impl_->next->isEnabled()) {
        emit nextRequested();
        return;
    }
    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
        zoomBy(1.25);
        return;
    }
    if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Underscore) {
        zoomBy(0.8);
        return;
    }
    if (event->key() == Qt::Key_0) {
        setZoom(1.0);
        return;
    }
    if (event->key() == Qt::Key_Home) {
        goToPage(0);
        return;
    }
    if (event->key() == Qt::Key_End) {
        goToPage(static_cast<int>(impl_->pages.size()) - 1);
        return;
    }
    if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier) {
        setFitMode(PdfFit::Page);
        return;
    }
    if (event->key() == Qt::Key_0 && event->modifiers().testFlag(Qt::ControlModifier)) {
        setFitMode(PdfFit::Width);
        return;
    }
    if (event->key() == Qt::Key_1 && event->modifiers().testFlag(Qt::ControlModifier)) {
        setZoom(1.0);
        return;
    }
    if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_PageUp) {
        auto* bar = impl_->scroll->verticalScrollBar();
        const int step = std::max(40, impl_->scroll->viewport()->height() * 9 / 10);
        bar->setValue(bar->value() + (event->key() == Qt::Key_PageDown ? step : -step));
        return;
    }
    QWidget::keyPressEvent(event);
}

bool PdfWindow::eventFilter(QObject* watched, QEvent* event) {
    if (impl_ == nullptr || impl_->scroll == nullptr) {
        return QWidget::eventFilter(watched, event);
    }
    auto* viewport = impl_->scroll->viewport();
    const bool onDocument = watched == viewport || watched == impl_->host;
    if (onDocument && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            impl_->dragging = true;
            impl_->dragOrigin = mouse->globalPos();
            impl_->dragScrollX = impl_->scroll->horizontalScrollBar()->value();
            impl_->dragScrollY = impl_->scroll->verticalScrollBar()->value();
            viewport->setCursor(Qt::ClosedHandCursor);
            impl_->host->setCursor(Qt::ClosedHandCursor);
            viewport->grabMouse();
            return true;
        }
    }
    if (impl_->dragging && (event->type() == QEvent::MouseMove
            || event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint delta = mouse->globalPos() - impl_->dragOrigin;
        impl_->scroll->horizontalScrollBar()->setValue(impl_->dragScrollX - delta.x());
        impl_->scroll->verticalScrollBar()->setValue(impl_->dragScrollY - delta.y());
        if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
            impl_->dragging = false;
            viewport->releaseMouse();
            viewport->setCursor(Qt::OpenHandCursor);
            impl_->host->setCursor(Qt::OpenHandCursor);
        }
        return true;
    }
    if (watched == viewport) {
        if (event->type() == QEvent::Resize) {
            if (std::abs(impl_->zoom - 1.0) < 0.02) {
                relayout();
            }
            pump();
        } else if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
                const int delta = wheel->angleDelta().y();
                if (delta > 0) {
                    zoomBy(1.1);
                } else if (delta < 0) {
                    zoomBy(1.0 / 1.1);
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QImage renderPdfFirstPage(const std::vector<unsigned char>& bytes, const int maxEdge) {
#ifdef Q_OS_WIN
    if (maxEdge < 16) {
        return {};
    }
    auto opened = openPdf(bytes, {});
    if (opened.doc == nullptr || opened.sizes.empty()) {
        return {};
    }
    const auto& size = opened.sizes.front();
    const double longEdge = std::max(size.width(), size.height());
    const int pixels = longEdge > 1.0
        ? static_cast<int>(maxEdge * size.width() / longEdge)
        : maxEdge;
    return renderPage(opened.doc, 0, std::max(16, pixels));
#else
    Q_UNUSED(bytes)
    Q_UNUSED(maxEdge)
    return {};
#endif
}

} // namespace videovault::app
