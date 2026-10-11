#include "videovault/app/pdf_window.hpp"

#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#endif

namespace videovault::app {
namespace {

class PageCanvas final : public QWidget {
public:
    explicit PageCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    QImage image;
    int renderedWidth{0};

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.fillRect(rect(), Qt::white);
        if (!image.isNull()) {
            painter.drawImage(rect(), image);
        }
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

bool looksEncrypted(const std::vector<unsigned char>& bytes) {
    const char mark[] = "/Encrypt";
    const auto markSize = sizeof(mark) - 1U;
    if (bytes.size() < markSize) {
        return false;
    }
    const auto scan = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t i = begin; i + markSize <= end; ++i) {
            if (std::memcmp(bytes.data() + i, mark, markSize) == 0) {
                return true;
            }
        }
        return false;
    };
    const auto head = std::min<std::size_t>(bytes.size(), 65536U);
    const auto tail = std::min<std::size_t>(bytes.size(), 262144U);
    return scan(0U, head) || scan(bytes.size() - tail, bytes.size());
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
        options.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId());
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
        if (!image.loadFromData(encoded.data(), static_cast<int>(encoded.size()), "PNG")) {
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
        outcome.badPassword = !password.isEmpty();
        outcome.error = outcome.badPassword
            ? QStringLiteral("That password did not open this PDF.")
            : QString::fromWCharArray(error.message().c_str());
        if (outcome.error.isEmpty()) {
            outcome.error = QStringLiteral("This PDF could not be opened.");
        }
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
    QLabel* title{nullptr};
    QLabel* pageLabel{nullptr};
    QLabel* zoomLabel{nullptr};
    QLabel* message{nullptr};
    QPushButton* previous{nullptr};
    QPushButton* next{nullptr};
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
    setStyleSheet(QStringLiteral("PdfWindow { background: #000000; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 8, 12, 8);
    headerLayout->setSpacing(8);
    auto* back = new QPushButton(QStringLiteral("Back"), header);
    back->setProperty("quiet", true);
    back->setCursor(Qt::PointingHandCursor);
    impl_->title = new QLabel(header);
    impl_->title->setStyleSheet(QStringLiteral("color: white; font-size: 15px; background: transparent;"));
    impl_->pageLabel = new QLabel(header);
    impl_->pageLabel->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
    headerLayout->addWidget(back);
    headerLayout->addWidget(impl_->title, 1);
    headerLayout->addWidget(impl_->pageLabel);
    root->addWidget(header);

    impl_->stack = new QStackedWidget(this);
    auto* messagePage = new QWidget(impl_->stack);
    auto* messageLayout = new QVBoxLayout(messagePage);
    impl_->message = new QLabel(messagePage);
    impl_->message->setAlignment(Qt::AlignCenter);
    impl_->message->setWordWrap(true);
    impl_->message->setStyleSheet(QStringLiteral("color: #aeaeb2; font-size: 15px; background: transparent;"));
    messageLayout->addWidget(impl_->message);
    impl_->stack->addWidget(messagePage);

    impl_->scroll = new QScrollArea(impl_->stack);
    impl_->scroll->setFrameShape(QFrame::NoFrame);
    impl_->scroll->setWidgetResizable(true);
    impl_->scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: #000000; border: none; }"
        "QScrollArea > QWidget > QWidget { background: #000000; }"));
    impl_->host = new QWidget(impl_->scroll);
    impl_->host->setStyleSheet(QStringLiteral("background: #000000;"));
    impl_->pagesLayout = new QVBoxLayout(impl_->host);
    impl_->pagesLayout->setContentsMargins(24, 16, 24, 24);
    impl_->pagesLayout->setSpacing(16);
    impl_->pagesLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    impl_->host->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    impl_->scroll->setWidget(impl_->host);
    impl_->scroll->viewport()->setCursor(Qt::OpenHandCursor);
    impl_->host->setCursor(Qt::OpenHandCursor);
    impl_->scroll->viewport()->installEventFilter(this);
    impl_->host->installEventFilter(this);
    impl_->stack->addWidget(impl_->scroll);
    root->addWidget(impl_->stack, 1);

    auto* footer = new QWidget(this);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 12, 10);
    footerLayout->setSpacing(8);
    auto* zoomOut = new QPushButton(QStringLiteral("−"), footer);
    impl_->zoomLabel = new QLabel(QStringLiteral("Fit"), footer);
    impl_->zoomLabel->setAlignment(Qt::AlignCenter);
    impl_->zoomLabel->setMinimumWidth(52);
    impl_->zoomLabel->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    auto* zoomIn = new QPushButton(QStringLiteral("+"), footer);
    auto* fit = new QPushButton(QStringLiteral("Fit width"), footer);
    impl_->previous = new QPushButton(QStringLiteral("Previous"), footer);
    impl_->next = new QPushButton(QStringLiteral("Next"), footer);
    for (auto* button : {zoomOut, zoomIn, fit, impl_->previous, impl_->next}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
    }
    zoomOut->setProperty("quiet", true);
    zoomIn->setProperty("quiet", true);
    fit->setProperty("quiet", true);
    impl_->previous->setProperty("quiet", true);
    impl_->next->setProperty("quiet", true);
    footerLayout->addWidget(zoomOut);
    footerLayout->addWidget(impl_->zoomLabel);
    footerLayout->addWidget(zoomIn);
    footerLayout->addWidget(fit);
    footerLayout->addStretch(1);
    footerLayout->addWidget(impl_->previous);
    footerLayout->addWidget(impl_->next);
    root->addWidget(footer);

    impl_->loadWatcher = new QFutureWatcher<LoadResult>(this);
    impl_->renderWatcher = new QFutureWatcher<RenderResult>(this);
    connect(impl_->loadWatcher, &QFutureWatcherBase::finished, this, [this] { applyLoad(); });
    connect(impl_->renderWatcher, &QFutureWatcherBase::finished, this, [this] { applyRender(); });
    connect(impl_->scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        updatePageLabel();
        pump();
    });
    connect(back, &QPushButton::clicked, this, &PdfWindow::closeRequested);
    connect(impl_->previous, &QPushButton::clicked, this, &PdfWindow::previousRequested);
    connect(impl_->next, &QPushButton::clicked, this, &PdfWindow::nextRequested);
    connect(zoomOut, &QPushButton::clicked, this, [this] { zoomBy(0.8); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { zoomBy(1.25); });
    connect(fit, &QPushButton::clicked, this, [this] { setZoom(1.0); });
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
    impl_->title->setText(title.isEmpty() ? QStringLiteral("PDF") : title);
    impl_->pageLabel->clear();
    impl_->zoom = 1.0;
    impl_->zoomLabel->setText(QStringLiteral("Fit"));
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
        result.encrypted = looksEncrypted(*result.bytes);
        if (!result.encrypted) {
            auto opened = openPdf(*result.bytes, {});
            result.doc = std::move(opened.doc);
            result.sizes = std::move(opened.sizes);
            result.error = std::move(opened.error);
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
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PDF password"));
    dialog.setLabelText(rejected
        ? QStringLiteral("That password did not open this PDF. Try again.")
        : QStringLiteral("This PDF is password protected."));
    dialog.setTextEchoMode(QLineEdit::Password);
    dialog.setOkButtonText(QStringLiteral("Open"));
    if (dialog.exec() != QDialog::Accepted) {
        impl_->pending.reset();
        impl_->message->setText(QStringLiteral("This PDF is password protected."));
        impl_->stack->setCurrentIndex(0);
        return;
    }
    QString password = dialog.textValue();
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
    const int width = pageWidth();
    if (width < 80 || impl_->pages.size() != impl_->sizes.size()) {
        return;
    }
    for (std::size_t i = 0U; i < impl_->pages.size(); ++i) {
        const auto& size = impl_->sizes[i];
        const double aspect = size.height() > 1.0 ? size.width() / size.height() : 0.77;
        const int height = std::max(40, static_cast<int>(static_cast<double>(width) / aspect));
        impl_->pages[i]->setFixedSize(width, height);
    }
    impl_->host->setMinimumWidth(width + 48);
    int contentHeight = impl_->pagesLayout->contentsMargins().top()
        + impl_->pagesLayout->contentsMargins().bottom();
    if (!impl_->pages.empty()) {
        contentHeight += impl_->pagesLayout->spacing() * static_cast<int>(impl_->pages.size() - 1U);
    }
    for (auto* page : impl_->pages) {
        contentHeight += page->height();
    }
    impl_->host->setMinimumHeight(contentHeight);
    impl_->zoomLabel->setText(std::abs(impl_->zoom - 1.0) < 0.02
        ? QStringLiteral("Fit")
        : QStringLiteral("%1%").arg(static_cast<int>(impl_->zoom * 100.0)));
}

int PdfWindow::pageWidth() const {
    const int viewport = impl_->scroll->viewport()->width();
    const int fitted = std::max(120, viewport - 48);
    return std::clamp(static_cast<int>(static_cast<double>(fitted) * impl_->zoom), 120, 2400);
}

void PdfWindow::zoomBy(const double factor) {
    setZoom(std::clamp(impl_->zoom * factor, 0.5, 4.0));
}

void PdfWindow::setZoom(const double zoom) {
    impl_->zoom = zoom;
    for (auto* page : impl_->pages) {
        page->renderedWidth = 0;
    }
    relayout();
    pump();
}

void PdfWindow::updatePageLabel() {
    if (impl_->pages.empty()) {
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
    impl_->pageLabel->setText(QStringLiteral("%1 / %2")
        .arg(current + 1)
        .arg(impl_->pages.size()));
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
    const int pixels = std::clamp(static_cast<int>(width * ratio), width, 4096);
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
    if (maxEdge < 16 || looksEncrypted(bytes)) {
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
