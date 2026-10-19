// Development-only stand-ins for the music player and the PDF reader.
//
// The real MusicWindow (src/music_window.cpp) and PdfWindow (src/pdf_window.cpp)
// integrate with Windows through C++/WinRT: SystemMediaTransportControls for the
// system media session and Windows.Data.Pdf for rendering. Both need MSVC plus
// the Windows SDK; MinGW cannot compile those translation units at all, so the
// whole desktop app could not be built or launched on a MinGW-only machine and
// the rest of the UI (library, file explorer, vault, sync) had no way to run.
//
// This file is compiled only when MVP_MINGW_APP_FALLBACK=ON and only for a
// non-MSVC Windows build (see qt-app/CMakeLists.txt). It implements the two
// classes exactly as declared in videovault/app/music_window.hpp and
// videovault/app/pdf_window.hpp - constructor, public API, QWidget overrides,
// and the declared private plumbing - so main_window.cpp compiles and links
// unchanged.
//
// Every operation here is a deliberate, visible "not available" answer:
//   * no media is decoded, no audio device is opened, no PDF is parsed;
//   * renderPdfFirstPage() always returns a null QImage, so the library keeps
//     its plain document tile instead of a preview;
//   * no vault handle is dereferenced: nothing is read, decrypted, copied, or
//     written to disk, and no success state is ever reported.
//
// These stubs are a diagnostic surface, not a feature. Do not ship them: the
// MSVC build always compiles the real windows.

#include "videovault/app/music_window.hpp"
#include "videovault/app/pdf_window.hpp"

#include "videovault/app/media_decoder.hpp"

#include <QCloseEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

namespace videovault::app {

namespace {

constexpr const char* kPdfNotice =
    "This development build has no PDF renderer. The reader needs the Windows "
    "SDK (MSVC) build, so nothing was opened, read, or decrypted.";

// Card geometry: the wrapped labels are pinned to the inner card width so the
// layout always reserves the height their text actually needs.
constexpr int kPdfCardWidth = 560;
constexpr int kPdfTextWidth = kPdfCardWidth - 48;

// Font sizes are set through QFont (not the stylesheet) so that the measuring
// code below and the painted text always agree.
QLabel* makeNotice(
    const QString& text,
    QWidget* parent,
    const char* colorStyle = R"(color: #d8b25a; background: transparent;)",
    const int pixelSize = 13,
    const bool bold = false) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPixelSize(pixelSize);
    font.setBold(bold);
    label->setFont(font);
    // Word-wrapped labels only get their true height from a QBoxLayout when the
    // policy advertises height-for-width (QLabel does not set this by default).
    QSizePolicy policy = label->sizePolicy();
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
    label->setStyleSheet(QString::fromUtf8(colorStyle));
    return label;
}

// A word-wrapped QLabel has no useful sizeHint width, so a plain vertical
// layout tends to give it a box shorter than its wrapped text. Pin the width
// and reserve exactly the height QFontMetrics computes for that width.
void fitWrapped(QLabel* label, const int width) {
    if (label == nullptr) {
        return;
    }
    label->setWordWrap(true);
    label->setMinimumWidth(width);
    label->setMaximumWidth(width);
    const QFontMetrics metrics(label->font());
    const QRect box = metrics.boundingRect(
        QRect(0, 0, width, 100000), Qt::TextWordWrap, label->text());
    label->setMinimumHeight(box.height() + 2);
}

QPushButton* makeButton(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { color: #e8cf94; background: #26221a;"
        " border: 1px solid #4a3c22; border-radius: 4px; padding: 5px 12px; }"
        "QPushButton:hover { background: #322b1f; }"
        "QPushButton:disabled { color: #6b6152; border-color: #332c20; }"));
    return button;
}

} // namespace

// The helper types music_window.cpp keeps to itself (TimeStretch, GlyphButton,
// the popups) live in that file's anonymous namespace. MusicWindow only holds
// them through pointers, but ~MusicWindow() must instantiate the destructor of
// std::unique_ptr<TimeStretch>, which needs a complete type. This empty
// definition satisfies that without pulling in the real audio engine.
class TimeStretch {};

// Declared as a nested private type in music_window.hpp purely as a
// unique_ptr target. The fallback has no system media session, so it stays
// empty and is never allocated.
struct MusicWindow::SystemMediaSession {};

MusicWindow::MusicWindow(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mvpMusicFallbackBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral(
        "QWidget#mvpMusicFallbackBar { background: #17171c;"
        " border-top: 1px solid #4a3c22; }"));

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(16, 10, 12, 10);
    row->setSpacing(12);

    auto* badge = makeNotice(QStringLiteral("MUSIC"), this,
        R"(color: #17171c; background: #d8b25a; border-radius: 3px; padding: 2px 7px;)",
        11, true);
    auto* text = makeNotice(
        tr("Music playback is not available in this development build - "
           "the player needs the Windows SDK (MSVC) build."), this);
    auto* dismiss = makeButton(tr("Dismiss"), this);
    dismiss->setToolTip(tr("Close this placeholder bar."));
    connect(dismiss, &QPushButton::clicked, this, &QWidget::close);

    row->addWidget(badge, 0, Qt::AlignVCenter);
    row->addWidget(text, 1);
    row->addWidget(dismiss, 0, Qt::AlignVCenter);
}

MusicWindow::~MusicWindow() = default;

// Nothing is queued, decoded, or played. The bar only reports that state.
void MusicWindow::playQueue(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::vector<MusicTrack>& tracks,
    const int index) {
    Q_UNUSED(vault);
    Q_UNUSED(tracks);
    Q_UNUSED(index);
}

void MusicWindow::pauseForVideo() {}

void MusicWindow::showWhileMinimized() {
    show();
    raise();
}

void MusicWindow::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
}

void MusicWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

void MusicWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

void MusicWindow::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
}

void MusicWindow::mouseMoveEvent(QMouseEvent* event) {
    QWidget::mouseMoveEvent(event);
}

void MusicWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    QWidget::mouseDoubleClickEvent(event);
}

void MusicWindow::enterEvent(QEvent* event) {
    QWidget::enterEvent(event);
}

void MusicWindow::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
}

bool MusicWindow::eventFilter(QObject* watched, QEvent* event) {
    return QWidget::eventFilter(watched, event);
}

// Declared private plumbing. Every entry point the header exposes is defined so
// the type is complete, and each one is a no-op: there is no decoder, no sink,
// no clock, no playlist and no system media session behind this widget.
void MusicWindow::showTrack() {}
void MusicWindow::startWorker() {}
void MusicWindow::stopWorker() {}
bool MusicWindow::advance(int) { return false; }
void MusicWindow::playIndex(int) {}
void MusicWindow::togglePlay() {}
void MusicWindow::seekTo(std::int64_t) {}
void MusicWindow::refreshClock() {}
void MusicWindow::onTrackEnded() {}
void MusicWindow::ensureOutput() {}
void MusicWindow::applySeekClear() {}
void MusicWindow::layoutControls() {}
void MusicWindow::applyName() {}
void MusicWindow::syncTransport() {}
void MusicWindow::setSpeedMilli(int) {}
void MusicWindow::toggleSpeed() {}
void MusicWindow::setOrder(int) {}
void MusicWindow::setVolumePercent(int) {}
void MusicWindow::toggleMute() {}
void MusicWindow::showOnly(QWidget*, QWidget*, bool) {}
void MusicWindow::hidePopups() {}
void MusicWindow::armHide() {}
bool MusicWindow::cursorOnChrome() const { return false; }
bool MusicWindow::nextAvailable() const { return false; }
bool MusicWindow::previousAvailable() const { return false; }
void MusicWindow::refillBag() {}
void MusicWindow::loadPlayback() {}
void MusicWindow::savePlayback() {}
void MusicWindow::applySong() {}
void MusicWindow::updateTray() {}
void MusicWindow::showTrayCard() {}
void MusicWindow::syncSystemMedia(bool) {}
void MusicWindow::clearSystemMedia() {}
void MusicWindow::publishTrackInfo(
    std::int64_t,
    const QString&,
    const QString&,
    const QString&,
    const QImage&) {}
SongTags MusicWindow::songFor(int) const { return {}; }
std::int64_t MusicWindow::positionMs() const { return 0; }
QString MusicWindow::formatTime(std::int64_t) const { return QStringLiteral("--:--"); }
void MusicWindow::seekBy(std::int64_t) {}
void MusicWindow::requestCovers() {}

// ---------------------------------------------------------------------------
// PDF reader
// ---------------------------------------------------------------------------

// The real reader keeps its document, page cache and widgets here. The fallback
// keeps only the text it displays: no document handle, no plaintext.
struct PdfWindow::Impl {
    QString title;
    QLabel* heading{nullptr};
    QLabel* detail{nullptr};
    QPushButton* dockButton{nullptr};
    bool previous{false};
    bool next{false};
};

PdfWindow::PdfWindow(QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("mvpPdfFallback"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral(
        "QWidget#mvpPdfFallback { background: #101014; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("mvpPdfFallbackCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFixedWidth(kPdfCardWidth);
    card->setStyleSheet(QStringLiteral(
        "QFrame#mvpPdfFallbackCard { background: #17171c;"
        " border: 1px dashed #4a3c22; border-radius: 8px; }"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 22, 24, 22);
    cardLayout->setSpacing(12);

    impl_->heading = makeNotice(
        tr("PDF viewer is not available in this development build"), card,
        R"(color: #f0e2bd; background: transparent;)", 17, true);
    fitWrapped(impl_->heading, kPdfTextWidth);
    impl_->detail = makeNotice(QString::fromUtf8(kPdfNotice), card);
    fitWrapped(impl_->detail, kPdfTextWidth);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(10);
    auto* back = makeButton(tr("Back to library"), card);
    back->setToolTip(tr("Leave this placeholder and return to the gallery."));
    impl_->dockButton = makeButton(tr("Pop out"), card);
    buttons->addWidget(back);
    buttons->addWidget(impl_->dockButton);
    buttons->addStretch(1);

    cardLayout->addWidget(impl_->heading);
    cardLayout->addWidget(impl_->detail);
    cardLayout->addLayout(buttons);

    root->addStretch(1);
    root->addWidget(card, 0, Qt::AlignHCenter);
    root->addStretch(1);

    connect(back, &QPushButton::clicked, this, &PdfWindow::closeRequested);
    connect(impl_->dockButton, &QPushButton::clicked, this, [this] {
        if (detached_) {
            emit dockRequested();
        } else {
            emit detachRequested();
        }
    });
    updatePopButton();
}

PdfWindow::~PdfWindow() = default;

void PdfWindow::openDocument(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::int64_t id,
    const QString& title) {
    // The vault handle is intentionally never dereferenced: no ciphertext is
    // read and no plaintext is produced, so there is nothing to show or deny.
    Q_UNUSED(vault);
    id_ = id;
    impl_->title = title.isEmpty() ? QStringLiteral("PDF") : title;
    impl_->detail->setText(tr("\"%1\" was not opened. %2")
        .arg(impl_->title, QString::fromUtf8(kPdfNotice)));
    fitWrapped(impl_->detail, kPdfTextWidth);
}

void PdfWindow::setNeighbors(const bool previous, const bool next) {
    impl_->previous = previous;
    impl_->next = next;
}

void PdfWindow::prepareDock() {
    if (isMaximized()) {
        showNormal();
    }
    detached_ = false;
    setMinimumSize(0, 0);
    setWindowFlags(Qt::Widget);
    updatePopButton();
}

void PdfWindow::detach() {
    if (isMaximized()) {
        showNormal();
    }
    detached_ = true;
    setParent(nullptr);
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
        | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint
        | Qt::WindowCloseButtonHint);
    setMinimumSize(420, 300);
    resize(720, 520);
    updatePopButton();
    show();
    raise();
    activateWindow();
}

void PdfWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit closeRequested();
        return;
    }
    // Navigation stays live even though the document cannot be shown, so the
    // library's previous/next controls never dead-end here.
    if (event->key() == Qt::Key_Left && impl_->previous) {
        emit previousRequested();
        return;
    }
    if (event->key() == Qt::Key_Right && impl_->next) {
        emit nextRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PdfWindow::showEvent(QShowEvent* event) {
    relayout();
    QWidget::showEvent(event);
    setFocus();
}

void PdfWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
}

bool PdfWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
    // No custom window messages are handled: the fallback has no caption strip
    // and no page surface.
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
}

bool PdfWindow::eventFilter(QObject* watched, QEvent* event) {
    return QWidget::eventFilter(watched, event);
}

void PdfWindow::applyLoad() {}
void PdfWindow::askPassword(bool) {}
void PdfWindow::relayout() {
    // Re-measure the notice text once the widget is polished, so the last line
    // is never clipped at a different font/margin than construction assumed.
    if (impl_ == nullptr) {
        return;
    }
    fitWrapped(impl_->heading, kPdfTextWidth);
    fitWrapped(impl_->detail, kPdfTextWidth);
}
void PdfWindow::zoomBy(double) {}
void PdfWindow::setZoom(double) {}
void PdfWindow::updatePageLabel() {}
void PdfWindow::pump() {}
void PdfWindow::applyRender() {}
void PdfWindow::applyDetachedFrame() {}

void PdfWindow::updatePopButton() {
    if (impl_ != nullptr && impl_->dockButton != nullptr) {
        impl_->dockButton->setText(detached_ ? tr("Dock") : tr("Pop out"));
    }
}

int PdfWindow::pageWidth() const {
    return 0;
}

// No renderer: the gallery keeps its plain document tile. Never returns a
// placeholder image that could look like a successful open.
QImage renderPdfFirstPage(const std::vector<unsigned char>& bytes, const int maxEdge) {
    Q_UNUSED(bytes);
    Q_UNUSED(maxEdge);
    return {};
}

} // namespace videovault::app
