#include "videovault/app/app_update.hpp"
#include "videovault/app/batch_worker.hpp"
#include "videovault/app/sync_engine.hpp"
#include "videovault/app/sync_page.hpp"
#include "videovault/app/glass.hpp"
#include "videovault/app/main_window.hpp"

#include "videovault/app/icons.hpp"
#include "videovault/app/music_window.hpp"
#include "videovault/app/peer_browser.hpp"
#include "videovault/app/peer_link.hpp"
#include "videovault/app/in_window_sheet.hpp"
#include "videovault/app/pdf_window.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/app/record_document.hpp"
#include "videovault/app/record_window.hpp"
#include "videovault/app/settings_dialog.hpp"
#include "videovault/app/plugin_host.hpp"
#include "videovault/app/plugin_tools.hpp"
#include "videovault/app/tag_filter_button.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/app/secure_input.hpp"
#include "videovault/app/share_server.hpp"
#include "videovault/core/vault.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <QtConcurrent/QtConcurrentRun>

#include <QAction>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QBuffer>
#include <QApplication>
#include <QImage>
#include <QGuiApplication>
#include <QScreen>
#include <QSignalBlocker>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QComboBox>
#include <QCursor>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFormLayout>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLineEdit>
#include <QDateTime>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QLinearGradient>
#include <QPen>
#include <QPixmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QProgressBar>
#include <QPointer>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QScrollBar>
#include <QSet>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QThread>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTimer>
#include <QRandomGenerator>
#include <QAbstractSlider>
#include <QUrl>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <deque>
#include <QThreadPool>
#include <string>
#include <utility>

namespace videovault::app {

struct FileIdentity {
    QString label;
    // 0 cannot be opened, 1 video or picture, 2 audio, 3 PDF, 4 plain text.
    int route{0};
};

struct MainWindow::GalleryCache {
    std::unordered_map<std::int64_t, QPixmap> thumbs;
    std::deque<std::int64_t> thumbOrder;
    std::unordered_map<std::int64_t, QListWidgetItem*> iconRows;
    std::unordered_map<std::int64_t, QTreeWidgetItem*> treeRows;
    std::unordered_map<std::int64_t, core::MediaInfo> media;
    std::unordered_map<std::int64_t, FileIdentity> kinds;
    // PDFs whose first page was already read or saved this session.
    std::unordered_set<std::int64_t> pdfThumbs;
};

namespace {

constexpr int kSetupPage = 0;
constexpr int kLoginPage = 1;
constexpr int kUnlockedPage = 2;
// Gallery item kinds (Qt::UserRole + 12).
constexpr int kKindPhone = 3;
// The library's search offers "Search <provider> for ..." as an entry that
// opens the music provider with those results.
constexpr int kKindPluginSearch = 4;
constexpr int kKindPluginBack = 5;
// A song from the provider: the library's own music row plus a Download button.
constexpr int kKindPluginEntry = 7;
// An album from the provider: a folder row that opens like any other folder.
constexpr int kKindPluginAlbum = 8;
// Provider rows carry three extra roles: the row's download state (0 = the row
// is not a track), the artist · duration line, and the provider track id.
constexpr int kRowStateRole = Qt::UserRole + 18;
constexpr int kRowSubtitleRole = Qt::UserRole + 19;
constexpr int kRowTrackIdRole = Qt::UserRole + 20;
// The Download button the provider folder pins over each track row's right
// edge, and the room the row delegate leaves for it.
constexpr int kRowButtonWidth = 104;
constexpr int kRowButtonHeight = 24;
constexpr int kRowButtonReserve = kRowButtonWidth + 22;
// Set on a gallery row while it is cut (Ctrl+X). The delegate fades the row.
constexpr int kCutDimRole = Qt::UserRole + 17;
// 0 means the vault stays unlocked until Lock is pressed. Five minutes is the
// default, matching the previous fixed timer.
int autoLockMilliseconds() {
    const int minutes = QSettings().value(QStringLiteral("security/autoLockMinutes"), 5).toInt();
    if (minutes <= 0) {
        return 0;
    }
    return minutes * 60 * 1000;
}

std::filesystem::path pathFromText(const QString& text) {
    return std::filesystem::path(text.trimmed().toStdWString());
}

QString textFromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

// The sync keys come from the vault password, so they are derived while it is
// still in the worker's hand; only the derived keys leave this function, never
// the password. A failure here leaves sync locked and does not block the vault.
sync::Keys deriveSyncKeys(const std::filesystem::path& root, const core::Secret& password) {
    try {
        const QString folder = textFromPath(root);
        // password.view() is read straight out of locked memory: no QString or
        // QByteArray copy of the vault password is created here.
        return sync::deriveKeysFromBytes(SyncEngine::stateDirectoryFor(folder), password.view());
    } catch (...) {
        return {};
    }
}

QPixmap scaledIcon(const char* resource, const int side) {
    QPixmap source(QString::fromLatin1(resource));
    if (source.isNull() || side <= 0) {
        return {};
    }
    return source.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// The update glyph: the download arrow the phone shows in its library header.
// Drawn here (like the cloud browser's green tick) so the button needs no asset.
// What the background update check found, handed back through a QFutureWatcher.
struct UpdateCheckOutcome {
    QString version;
    QString url;
    QString error;
    qint64 size{0};
};

// What a finished download/unpack produced: the folder to install from, or why
// it did not get that far.
struct UpdateInstallOutcome {
    QString payload;
    QString error;
};

QPixmap tintedIcon(const QPixmap& source, const QColor& color) {
    if (source.isNull()) {
        return {};
    }
    QPixmap plate(source.size());
    plate.fill(Qt::transparent);
    QPainter painter(&plate);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(plate.rect(), color);
    return plate;
}


QPixmap musicPlaceholder(const int side) {
    // A track row's art plate is painted from the palette, so a song on a light
    // row does not carry a black square into the light theme.
    const int s = std::max(24, side);
    QPixmap plate(s, s);
    const bool dark = isDarkTheme();
    plate.fill(dark ? QColor(0x24, 0x24, 0x26) : QColor(0xEA, 0xEA, 0xEF));
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(dark ? QColor(0xA8, 0xA8, 0xAE) : QColor(0x8E, 0x8E, 0x93));
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(std::max(12, s / 3));
    painter.setFont(font);
    painter.drawText(plate.rect(), Qt::AlignCenter, QStringLiteral("♪"));
    return plate;
}

QPixmap pdfDocumentPixmap(const int side) {
    const int s = std::max(64, side);
    QPixmap plate(s, s);
    plate.fill(QColor(0x1C, 0x1C, 0x1E));
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPixmap document = scaledIcon(":/icons/media_doc_blue.png", (s * 56) / 100);
    if (!document.isNull()) {
        painter.drawPixmap((s - document.width()) / 2, (s - document.height()) / 2, document);
    }
    painter.setPen(Qt::white);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(std::max(13, s / 18));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.drawText(QRect(0, s / 2 - s / 16, s, s / 5), Qt::AlignCenter, QStringLiteral("PDF"));
    return plate;
}

QPixmap textDocumentPixmap(const QString& label, const int side) {
    const int s = std::max(64, side);
    QPixmap plate(s, s);
    plate.fill(QColor(0x1C, 0x1C, 0x1E));
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QPixmap document = scaledIcon(":/icons/media_doc_blue.png", (s * 56) / 100);
    if (!document.isNull()) {
        painter.drawPixmap((s - document.width()) / 2, (s - document.height()) / 2, document);
    }
    painter.setPen(Qt::white);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(std::max(11, s / 22));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    QString badge = label.toUpper();
    if (badge.size() > 5) {
        badge = badge.left(5);
    }
    painter.drawText(QRect(0, s / 2 - s / 16, s, s / 5), Qt::AlignCenter, badge);
    return plate;
}

bool bytesAt(const unsigned char* data, const std::size_t size, const std::size_t offset, const char* text) {
    const auto length = std::strlen(text);
    return offset + length <= size && std::memcmp(data + offset, text, length) == 0;
}

QString compactSize(const std::uint64_t bytes) {
    if (bytes < 1024U) {
        return QStringLiteral("%1B").arg(bytes);
    }
    const double value = static_cast<double>(bytes);
    if (bytes < 1024U * 1024U) {
        return QStringLiteral("%1KB").arg(value / 1024.0, 0, 'f', value < 10.0 * 1024.0 ? 1 : 0);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        const double mb = value / (1024.0 * 1024.0);
        return mb >= 10.0
            ? QStringLiteral("%1MB").arg(static_cast<int>(mb + 0.5))
            : QStringLiteral("%1MB").arg(mb, 0, 'f', 1);
    }
    return QStringLiteral("%1GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

QColor colorForKind(const QString& label) {
    static const QColor palette[] = {
        QColor(0xC4, 0x4B, 0x4B), QColor(0x3E, 0x7C, 0xB8), QColor(0x3F, 0x8F, 0x6B),
        QColor(0xC4, 0x84, 0x2A), QColor(0x7A, 0x5A, 0xC4), QColor(0x2F, 0x8A, 0x8A),
        QColor(0xB0, 0x4E, 0x7A), QColor(0x5C, 0x6B, 0xC4), QColor(0x8A, 0x6A, 0x3B),
        QColor(0x3D, 0x7A, 0x4A), QColor(0xA1, 0x4E, 0x3C), QColor(0x4A, 0x6A, 0x8A),
    };
    return palette[qHash(label) % (sizeof(palette) / sizeof(palette[0]))];
}

QPixmap kindBadge(const QString& label, const QString& sizeText, const int side) {
    const int s = std::max(64, side);
    QPixmap plate(s, s);
    plate.fill(colorForKind(label));
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::white);
    QFont kindFont(QStringLiteral("Segoe UI"));
    kindFont.setPixelSize(std::max(16, s / 8));
    kindFont.setWeight(QFont::DemiBold);
    painter.setFont(kindFont);
    painter.drawText(QRect(8, s / 2 - s / 5, s - 16, s / 5), Qt::AlignCenter,
        QStringLiteral("[ %1 ]").arg(label));
    QFont sizeFont(QStringLiteral("Segoe UI"));
    sizeFont.setPixelSize(std::max(14, s / 10));
    painter.setFont(sizeFont);
    painter.drawText(QRect(8, s / 2 + 4, s - 16, s / 6), Qt::AlignCenter, sizeText);
    return plate;
}

// A record has no thumbnail and, in the icon grid, no caption either (the name
// lives in the tooltip). So the plate has to identify it by itself: the record's
// name is drawn on the tile, coloured per record, with the kind and size around
// it. Otherwise every record would look identical.
QPixmap recordBadge(const QString& kindLabel, const QString& name,
    const QString& sizeText, const int side) {
    const int s = std::max(64, side);
    QPixmap plate(s, s);
    plate.fill(colorForKind(name));
    QPainter painter(&plate);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::white);

    QFont smallFont(QStringLiteral("Segoe UI"));
    smallFont.setPixelSize(std::max(12, s / 12));
    painter.setFont(smallFont);
    painter.drawText(QRect(8, 6, s - 16, s / 8), Qt::AlignCenter, kindLabel);
    painter.drawText(QRect(8, s - 6 - s / 8, s - 16, s / 8), Qt::AlignCenter, sizeText);

    QFont nameFont(QStringLiteral("Segoe UI"));
    nameFont.setPixelSize(std::max(16, s / 8));
    nameFont.setWeight(QFont::DemiBold);
    painter.setFont(nameFont);
    const QFontMetrics nameMetrics(nameFont);
    const QString elided = nameMetrics.elidedText(
        name.isEmpty() ? QStringLiteral("Untitled") : name, Qt::ElideRight,
        std::max(16, s - 16));
    painter.drawText(QRect(8, s / 2 - s / 4, s - 16, s / 2),
        Qt::AlignCenter | Qt::TextWordWrap, elided);
    return plate;
}

QByteArray headBytes(const unsigned char* data, const std::size_t size) {
    // The sniffing callers pass the first bytes of an item's plaintext; clamp
    // so a hostile length can never overflow the QByteArray view.
    const auto head = static_cast<int>(std::min<std::size_t>(size, 4096U));
    return QByteArray::fromRawData(reinterpret_cast<const char*>(data), head);
}

bool looksLikeText(const unsigned char* data, std::size_t size);

FileIdentity identityFromBytes(const unsigned char* data, const std::size_t size) {
    auto named = [](const char* label, const int route) {
        FileIdentity identity;
        identity.label = QString::fromLatin1(label);
        identity.route = route;
        return identity;
    };
    if (size == 0U) {
        return named("File", 0);
    }
    // Text records carry their maker in the first bytes of the plaintext, so a
    // record is recognised by content like any other item, never by its name.
    // Route 5, not 4: a record is text, but it opens in the record editor
    // instead of the text sheet, so the two have to stay distinguishable.
    if (looksLikeRecord(headBytes(data, size))) {
        return named("Record", 5);
    }
    if (bytesAt(data, size, 0, "%PDF")) {
        return named("Pdf", 3);
    }
    if (size >= 3U && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return named("Jpeg", 1);
    }
    if (bytesAt(data, size, 0, "\x89PNG")) {
        return named("Png", 1);
    }
    if (bytesAt(data, size, 0, "GIF87a") || bytesAt(data, size, 0, "GIF89a")) {
        return named("Gif", 1);
    }
    if (bytesAt(data, size, 0, "BM")) {
        return named("Bmp", 1);
    }
    if (bytesAt(data, size, 0, "RIFF") && bytesAt(data, size, 8, "WEBP")) {
        return named("Webp", 1);
    }
    if (bytesAt(data, size, 0, "RIFF") && bytesAt(data, size, 8, "WAVE")) {
        return named("Wav", 2);
    }
    if (bytesAt(data, size, 0, "RIFF") && bytesAt(data, size, 8, "AVI ")) {
        return named("Avi", 1);
    }
    if (bytesAt(data, size, 0, "fLaC") || bytesAt(data, size, 0, "ID3") || bytesAt(data, size, 0, "OggS")) {
        return named(bytesAt(data, size, 0, "fLaC") ? "Flac" : (bytesAt(data, size, 0, "ID3") ? "Mp3" : "Ogg"), 2);
    }
    if (size >= 2U && data[0] == 0xFF && (data[1] == 0xFB || data[1] == 0xF3 || data[1] == 0xF2 || data[1] == 0xF1 || data[1] == 0xF9)) {
        return named(data[1] == 0xF1 || data[1] == 0xF9 ? "Aac" : "Mp3", 2);
    }
    if (size >= 12U && bytesAt(data, size, 4, "ftyp")) {
        const auto audioBrand = [](const unsigned char* brand) {
            return std::memcmp(brand, "M4A ", 4) == 0 || std::memcmp(brand, "M4B ", 4) == 0
                || std::memcmp(brand, "M4P ", 4) == 0 || std::memcmp(brand, "F4A ", 4) == 0
                || std::memcmp(brand, "F4B ", 4) == 0;
        };
        if (audioBrand(data + 8)) {
            return named("M4a", 2);
        }
        const std::size_t brandEnd = std::min(size, static_cast<std::size_t>(80));
        for (std::size_t i = 16; i + 4 <= brandEnd; i += 4) {
            if (audioBrand(data + i)) {
                return named("M4a", 2);
            }
        }
        const QString brand = QString::fromLatin1(reinterpret_cast<const char*>(data + 8), 4).trimmed();
        FileIdentity identity;
        identity.label = brand.isEmpty() ? QStringLiteral("Mp4") : brand.left(1).toUpper() + brand.mid(1).toLower();
        identity.route = 1;
        return identity;
    }
    if (size >= 4U && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) {
        return named("Mkv", 1);
    }
    if (bytesAt(data, size, 0, "Rar!")) {
        return named("Rar", 0);
    }
    if (size >= 4U && data[0] == 'P' && data[1] == 'K' && (data[2] == 3 || data[2] == 5 || data[2] == 7)) {
        return named("Zip", 0);
    }
    if (size >= 6U && data[0] == 0x37 && data[1] == 0x7A && data[2] == 0xBC && data[3] == 0xAF && data[4] == 0x27 && data[5] == 0x1C) {
        return named("7z", 0);
    }
    if (size >= 2U && data[0] == 0x1F && data[1] == 0x8B) {
        return named("Gz", 0);
    }
    if (bytesAt(data, size, 0, "BZh")) {
        return named("Bz2", 0);
    }
    if (size >= 6U && data[0] == 0xFD && bytesAt(data, size, 1, "7zXZ")) {
        return named("Xz", 0);
    }
    if (size >= 4U && data[0] == 0x30 && data[1] == 0x26 && data[2] == 0xB2 && data[3] == 0x75) {
        return named("Asf", 1);
    }
    if (bytesAt(data, size, 0, "FLV")) {
        return named("Flv", 1);
    }
    if (size >= 8U) {
        const char* atom = reinterpret_cast<const char*>(data + 4);
        if (std::memcmp(atom, "moov", 4) == 0 || std::memcmp(atom, "mdat", 4) == 0
            || std::memcmp(atom, "wide", 4) == 0 || std::memcmp(atom, "free", 4) == 0) {
            return named("Mov", 1);
        }
    }
    if (bytesAt(data, size, 257, "ustar")) {
        return named("Tar", 0);
    }
    if (size >= 2U && data[0] == 'M' && data[1] == 'Z') {
        return named("Exe", 0);
    }
    if (size >= 4U && data[0] == 0x7F && bytesAt(data, size, 1, "ELF")) {
        return named("Elf", 0);
    }
    if (bytesAt(data, size, 0, "SQLite format 3")) {
        return named("Sqlite", 0);
    }
    if (size >= 8U && data[0] == 0xD0 && data[1] == 0xCF && data[2] == 0x11 && data[3] == 0xE0) {
        return named("Doc", 0);
    }
    if (bytesAt(data, size, 0, "MSCF")) {
        return named("Cab", 0);
    }
    if (looksLikeText(data, size)) {
        return named("Text", 4);
    }
    return named("File", 0);
}

bool looksLikeText(const unsigned char* data, const std::size_t size) {
    if (data == nullptr || size < 4U) {
        return false;
    }
    std::size_t controls = 0U;
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char byte = data[i];
        if (byte == 0U) {
            return false;
        }
        if (byte < 32U && byte != 9U && byte != 10U && byte != 13U) {
            ++controls;
        }
    }
    return controls * 20U < size;
}

bool isTextFileName(const QString& name) {
    const QString suffix = QFileInfo(name).suffix().toLower();
    static const QStringList kText = {
        QStringLiteral("txt"), QStringLiteral("text"), QStringLiteral("log"),
        QStringLiteral("json"), QStringLiteral("jsonl"), QStringLiteral("md"),
        QStringLiteral("markdown"), QStringLiteral("csv"), QStringLiteral("tsv"),
        QStringLiteral("xml"), QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("css"), QStringLiteral("js"), QStringLiteral("mjs"),
        QStringLiteral("ts"), QStringLiteral("yaml"), QStringLiteral("yml"),
        QStringLiteral("ini"), QStringLiteral("cfg"), QStringLiteral("conf"),
        QStringLiteral("toml"), QStringLiteral("svg"), QStringLiteral("sh"),
        QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("ps1"),
        QStringLiteral("py"), QStringLiteral("srt"), QStringLiteral("vtt"),
    };
    return kText.contains(suffix);
}

QString noteFileName(QString name) {
    name = name.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    if (name.isEmpty() || name == QLatin1String("..")) {
        name = QStringLiteral("Note.txt");
    }
    if (QFileInfo(name).suffix().isEmpty()) {
        name += QStringLiteral(".txt");
    }
    return name;
}

struct LoadedText {
    QString body;
    bool truncated{false};
};

struct TextSaveOutcome {
    bool ok{false};
    bool already{false};
    std::int64_t id{0};
    qlonglong size{0};
    QString error;
};

FileIdentity identityFromName(const QString& name);
bool isAudioFileName(const QString& name);

FileIdentity identityForVideo(
    const std::shared_ptr<core::Vault>& vault,
    const std::int64_t video_id,
    const QString& name) {
    FileIdentity identity;
    identity.label = QStringLiteral("File");
    auto bytes = vault->read_video_range(video_id, 0U, 560U);
    if (bytes && !bytes.value().empty()) {
        identity = identityFromBytes(bytes.value().data(), bytes.value().size());
    }
    // An .m4a (and the other audio names) often carries a generic MP4 brand.
    // The extension is what puts it in the music player.
    if (isAudioFileName(name)) {
        return identityFromName(name);
    }
    // Magic wins when it recognizes the bytes. The name is only the fallback.
    if (identity.label == QStringLiteral("File")) {
        return identityFromName(name);
    }
    return identity;
}

QPair<QString, QString> songLabel(const QString& fileName) {
    const QFileInfo info(fileName);
    QString base = info.completeBaseName().trimmed();
    if (base.isEmpty()) {
        base = fileName.trimmed();
    }
    const QString original = base;
    const QRegularExpression leading(QStringLiteral("^\\s*(?:\\d{1,3}\\s*[.\\-_]\\s+)+"));
    base.remove(leading);
    base = base.trimmed();
    if (base.isEmpty()) {
        base = original;
    }
    const QStringList separators{
        QStringLiteral(" — "), QStringLiteral(" – "), QStringLiteral(" - ")};
    for (const QString& separator : separators) {
        const int at = base.indexOf(separator);
        if (at > 0 && at + separator.size() < base.size()) {
            return {base.left(at).trimmed(), base.mid(at + separator.size()).trimmed()};
        }
    }
    return {{}, base};
}

// One provider catalogue array, whichever key this provider uses for it.
QJsonArray pluginArray(const QJsonObject& catalogue, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonArray array = catalogue.value(key).toArray();
        if (!array.isEmpty()) {
            return array;
        }
    }
    return {};
}

// "3:07" for a provider track: milliseconds when the provider says so, and
// seconds when the value is too small to be milliseconds.
QString pluginDurationText(const QJsonObject& item) {
    qint64 ms = 0;
    for (const QString& key : {QStringLiteral("durationMs"), QStringLiteral("duration_ms"),
             QStringLiteral("durationMS")}) {
        const QJsonValue value = item.value(key);
        if (value.isDouble() && value.toDouble() > 0) {
            ms = static_cast<qint64>(value.toDouble());
            break;
        }
    }
    if (ms <= 0) {
        const QJsonValue value = item.value(QStringLiteral("duration"));
        if (value.isDouble() && value.toDouble() > 0) {
            const double raw = value.toDouble();
            ms = static_cast<qint64>(raw > 36000.0 ? raw : raw * 1000.0);
        }
    }
    if (ms <= 0) {
        return {};
    }
    const qint64 seconds = ms / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// The row button's four looks: ready, busy, saved and retry. Small and quiet so
// a list of songs still reads as a list, not as a wall of buttons.
QString pluginRowButtonStyle(const QString& look, const bool dark) {
    const QString muted = dark ? QStringLiteral("#a8a8ae") : QStringLiteral("#63636a");
    const QString accent = QStringLiteral("#3390ec");
    QString border = dark ? QStringLiteral("rgba(255,255,255,64)") : QStringLiteral("rgba(0,0,0,48)");
    QString fill = dark ? QStringLiteral("rgba(255,255,255,14)") : QStringLiteral("rgba(0,0,0,8)");
    QString ink = muted;
    if (look == QLatin1String("ready") || look == QLatin1String("retry")) {
        border = accent;
        fill = dark ? QStringLiteral("rgba(51,144,236,56)") : QStringLiteral("rgba(51,144,236,34)");
        ink = dark ? QStringLiteral("#9ccbf7") : QStringLiteral("#1f6fbf");
    } else if (look == QLatin1String("saved")) {
        border = dark ? QStringLiteral("rgba(94,196,132,150)") : QStringLiteral("rgba(38,138,84,150)");
        fill = QStringLiteral("transparent");
        ink = dark ? QStringLiteral("#7fd6a2") : QStringLiteral("#1f7a4d");
    }
    return QStringLiteral(
        "QToolButton { border: 1px solid %1; border-radius: %2px; background: %3; color: %4;"
        " font-family: \"Segoe UI\"; font-size: 11px; padding: 0 6px; }"
        "QToolButton:hover { background: %5; }"
        "QToolButton:disabled { color: %4; }")
        .arg(border)
        .arg(kRowButtonHeight / 2)
        .arg(fill, ink)
        .arg(dark ? QStringLiteral("rgba(255,255,255,26)") : QStringLiteral("rgba(0,0,0,16)"));
}

bool isPdfFileName(const QString& name) {
    return QFileInfo(name).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
}

bool isAudioFileName(const QString& name) {
    const QString suffix = QFileInfo(name).suffix().toLower();
    static const QStringList kAudio = {
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("wav"),
        QStringLiteral("wave"), QStringLiteral("ogg"), QStringLiteral("oga"),
        QStringLiteral("opus"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("wma"), QStringLiteral("aiff"), QStringLiteral("aif"),
        QStringLiteral("alac"), QStringLiteral("mp2"), QStringLiteral("mka"),
    };
    return kAudio.contains(suffix);
}

FileIdentity identityFromName(const QString& name) {
    const QString suffix = QFileInfo(name).suffix().toLower();
    FileIdentity identity;
    if (suffix.isEmpty()) {
        identity.label = QStringLiteral("File");
        return identity;
    }
    identity.label = suffix;
    identity.label[0] = identity.label.at(0).toUpper();
    if (suffix == QLatin1String("pdf")) {
        identity.route = 3;
        return identity;
    }
    if (isTextFileName(name)) {
        identity.route = 4;
        return identity;
    }
    if (isAudioFileName(name)) {
        identity.route = 2;
        return identity;
    }
    static const QStringList kPlayable = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"),
        QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("wmv"),
        QStringLiteral("webm"), QStringLiteral("ts"), QStringLiteral("mts"),
        QStringLiteral("m2ts"), QStringLiteral("flv"), QStringLiteral("3gp"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("vob"),
        QStringLiteral("asf"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("png"), QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("gif"), QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("tif"), QStringLiteral("tiff"),
    };
    identity.route = kPlayable.contains(suffix) ? 1 : 0;
    return identity;
}

// Gallery items are recreated from scratch on every refreshGallery() (search
// keystrokes, tag filters, deletes...), so raw QTreeWidgetItem* /
// QListWidgetItem* captured by in-flight async watchers dangle as soon as the
// tree is cleared. Resolve items by their stored video id at completion time
// instead of keeping pointers across the async boundary.
QTreeWidgetItem* findTreeItemById(QTreeWidget* tree, const std::int64_t video_id) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* item = tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toLongLong() == video_id) {
            return item;
        }
    }
    return nullptr;
}

QListWidgetItem* findListItemById(QListWidget* list, const std::int64_t video_id) {
    for (int i = 0; i < list->count(); ++i) {
        auto* item = list->item(i);
        if (item->data(Qt::UserRole).toLongLong() == video_id) {
            return item;
        }
    }
    return nullptr;
}

const QPixmap& folderPixmap() {
    static const QPixmap pix = [] {
        // Drawn as vector paths, not the old bitmap resource: tinting and
        // stretching a small PNG left visibly aliased edges at row size.
        constexpr int size = 256;
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal width = size * 0.84;
        const qreal height = size * 0.62;
        const qreal left = (size - width) / 2.0;
        const qreal top = (size - height) / 2.0 + size * 0.06;
        const qreal radius = size * 0.055;
        QPainterPath tab;
        tab.addRoundedRect(QRectF(left, top - size * 0.11, width * 0.44, size * 0.22),
            radius, radius);
        painter.fillPath(tab, QColor(0x2F, 0x74, 0xC0));
        QPainterPath body;
        body.addRoundedRect(QRectF(left, top, width, height), radius, radius);
        QLinearGradient shade(0, top, 0, top + height);
        shade.setColorAt(0.0, QColor(0x63, 0xB4, 0xF7));
        shade.setColorAt(1.0, QColor(0x33, 0x84, 0xD2));
        painter.fillPath(body, shade);
        painter.setPen(QPen(QColor(255, 255, 255, 30), size * 0.008));
        painter.drawPath(body);
        painter.end();
        return image;
    }();
    return pix;
}

// The swapped phone's library uses the same folder as the phone app.
const QPixmap& phoneFolderPixmap() {
    return folderPixmap();
}

void updateIconItemText(QListWidgetItem* item);

QString vaultErrorText(const core::VaultError& error) {
    if (!error.technical_detail.empty()) {
        return QString::fromStdString(error.technical_detail);
    }
    return QString::fromUtf8(core::user_message(error.code).data());
}

qlonglong pixelCount(const std::uint32_t width, const std::uint32_t height) {
    const auto product =
        static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height);
    constexpr auto limit = static_cast<unsigned long long>(std::numeric_limits<qlonglong>::max());
    if (product > limit) {
        return std::numeric_limits<qlonglong>::max();
    }
    return static_cast<qlonglong>(product);
}

void addLibraryEntry(
    QListWidget* list,
    QTreeWidget* tree,
    const qlonglong role_id,
    const int kind,
    const QString& name,
    const QString& meta,
    const QPixmap& pixmap = folderPixmap()) {
    auto* iconItem = new QListWidgetItem(list);
    iconItem->setData(Qt::UserRole, role_id);
    iconItem->setData(Qt::UserRole + 1, name);
    iconItem->setData(Qt::UserRole + 3, name);
    iconItem->setData(Qt::UserRole + 5, meta);
    iconItem->setData(Qt::UserRole + 12, kind);
    iconItem->setData(Qt::UserRole + 4, pixmap);
    iconItem->setIcon(QIcon(pixmap));
    iconItem->setToolTip(name);
    updateIconItemText(iconItem);

    auto* treeItem = new QTreeWidgetItem(tree);
    treeItem->setText(0, name);
    treeItem->setText(1, meta);
    treeItem->setIcon(0, QIcon(pixmap));
    treeItem->setData(0, Qt::UserRole, role_id);
    treeItem->setData(0, Qt::UserRole + 12, kind);
    treeItem->setToolTip(0, name);
}

// Icon/list cards show the video name plus, in large-icon (IconMode) view, the
// tags underneath so tag-based searches are visible at a glance. The compact
// List mode keeps the single-line name. The display text is derived from data
// roles so it can be rebuilt when the user switches view modes.
void updateIconItemText(QListWidgetItem* item) {
    const QString name = item->data(Qt::UserRole + 1).toString();
    // Icon mode is a picture grid. The name stays in the tooltip, not under the tile.
    if (item->listWidget() != nullptr
        && item->listWidget()->viewMode() == QListView::IconMode) {
        item->setText(QString());
        return;
    }
    item->setText(name);
}

void layoutInstagramGrid(QListWidget* list) {
    if (list == nullptr || list->viewMode() != QListView::IconMode) {
        return;
    }
    // Phone tiles are about a third of a narrow screen. On the desktop that
    // same size means more columns, not three giant squares.
    const int width = std::max(1, list->viewport()->width());
    const int columns = std::max(5, width / 124);
    const int cell = std::max(72, width / columns);
    if (list->gridSize() == QSize(cell, cell)) {
        return;
    }
    list->setGridSize(QSize(cell, cell));
    list->setIconSize(QSize(cell, cell));
}

// Square cover tiles, a hairline gap, and a small duration on the picture.
class InstagramDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override {
        const auto* view = qobject_cast<const QListView*>(option.widget);
        if (view != nullptr && view->viewMode() == QListView::ListMode) {
            painter->save();
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            if (index.data(kCutDimRole).toBool()) {
                painter->setOpacity(0.45);
            }
            const QRect row = option.rect;
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            if (selected) {
                painter->fillRect(row, QColor(38, 114, 200, 70));
            } else if (hovered) {
                painter->fillRect(row, isDarkTheme() ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 12));
            }
            const QRect thumb(row.left() + 14, row.top() + (row.height() - 44) / 2, 44, 44);
            painter->fillRect(thumb, isDarkTheme() ? QColor(28, 28, 30) : option.palette.color(QPalette::AlternateBase));
            const QPixmap source = index.data(Qt::UserRole + 4).value<QPixmap>();
            const int rowKind = index.data(Qt::UserRole + 12).toInt();
            const bool folderRow = rowKind == 1 || rowKind == 2 || rowKind == kKindPhone;
            const bool kindTile = !index.data(Qt::UserRole + 15).toString().isEmpty()
                && index.data(Qt::UserRole + 16).toInt() == 0;
            if (!source.isNull()) {
                if (folderRow || kindTile) {
                    const QPixmap scaled = source.scaled(
                        thumb.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    painter->drawPixmap(
                        thumb.x() + (thumb.width() - scaled.width()) / 2,
                        thumb.y() + (thumb.height() - scaled.height()) / 2,
                        scaled);
                } else {
                    const QPixmap scaled = source.scaled(
                        thumb.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    const int cropX = std::max(0, (scaled.width() - thumb.width()) / 2);
                    const int cropY = std::max(0, (scaled.height() - thumb.height()) / 2);
                    painter->drawPixmap(thumb, scaled, QRect(cropX, cropY, thumb.width(), thumb.height()));
                }
            } else if (isPdfFileName(index.data(Qt::UserRole + 1).toString())) {
                painter->setPen(QColor(142, 142, 147));
                QFont badge(QStringLiteral("Segoe UI"));
                badge.setPixelSize(11);
                badge.setWeight(QFont::DemiBold);
                painter->setFont(badge);
                painter->drawText(thumb, Qt::AlignCenter, QStringLiteral("PDF"));
            }
            const int textLeft = thumb.right() + 12;
            const QString name = index.data(Qt::UserRole + 1).toString();
            const QString sizeText = index.data(Qt::UserRole + 5).toString();
            const QString tags = index.data(Qt::UserRole + 2).toString();
            // A provider track is the library's normal music row: the title on
            // top and the artist · duration line under it. The row's Download
            // button is a widget pinned over the right edge, so the text stops
            // short of it instead of running underneath.
            if (index.data(kRowStateRole).toInt() != 0) {
                const QString subtitle = index.data(kRowSubtitleRole).toString();
                const int textWidth =
                    std::max(24, row.right() - 16 - kRowButtonReserve - textLeft);
                QFont titleFont(QStringLiteral("Segoe UI"));
                titleFont.setPointSize(10);
                const QFontMetrics titleMetrics(titleFont);
                painter->setFont(titleFont);
                painter->setPen(option.palette.color(QPalette::Text));
                painter->drawText(QRect(textLeft, row.top() + 12, textWidth, 18),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    titleMetrics.elidedText(name, Qt::ElideRight, textWidth));
                if (!subtitle.isEmpty()) {
                    QFont detailFont(QStringLiteral("Segoe UI"));
                    detailFont.setPointSize(9);
                    const QFontMetrics detailMetrics(detailFont);
                    painter->setFont(detailFont);
                    painter->setPen(isDarkTheme() ? QColor(168, 168, 174) : QColor(99, 99, 104));
                    painter->drawText(QRect(textLeft, row.top() + 31, textWidth, 16),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        detailMetrics.elidedText(subtitle, Qt::ElideRight, textWidth));
                }
                painter->setPen(isDarkTheme() ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 20));
                painter->drawLine(row.left() + 14, row.bottom(), row.right() - 14, row.bottom());
                painter->restore();
                return;
            }
            // One line per row: icon, name, then the meta pinned right. Folders
            // carry no meta at all - the stacked "Folder" caption is what made
            // list rows look like a broken table.
            const QString meta = folderRow
                ? tags
                : (tags.isEmpty() ? sizeText : QStringLiteral("%1  ·  %2").arg(sizeText, tags));
            QFont nameFont(QStringLiteral("Segoe UI"));
            nameFont.setPointSize(10);
            QFont metaFont(QStringLiteral("Segoe UI"));
            metaFont.setPointSize(9);
            const QFontMetrics nameMetrics(nameFont);
            const QFontMetrics metaMetrics(metaFont);
            const int metaWidth = meta.isEmpty()
                ? 0
                : std::min(metaMetrics.horizontalAdvance(meta) + 12, row.width() / 2);
            const int nameRight = std::max(textLeft + 24, row.right() - 16 - metaWidth);
            painter->setFont(nameFont);
            painter->setPen(option.palette.color(QPalette::Text));
            painter->drawText(QRect(textLeft, row.top(), nameRight - textLeft, row.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                nameMetrics.elidedText(name, Qt::ElideRight, nameRight - textLeft));
            if (!meta.isEmpty()) {
                painter->setFont(metaFont);
                painter->setPen(isDarkTheme() ? QColor(168, 168, 174) : QColor(99, 99, 104));
                painter->drawText(QRect(nameRight, row.top(), metaWidth, row.height()),
                    Qt::AlignRight | Qt::AlignVCenter,
                    metaMetrics.elidedText(meta, Qt::ElideLeft, metaWidth));
            }
            painter->setPen(isDarkTheme() ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 20));
            painter->drawLine(row.left() + 14, row.bottom(), row.right() - 14, row.bottom());
            painter->restore();
            return;
        }
        if (view == nullptr || view->viewMode() != QListView::IconMode) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (index.data(kCutDimRole).toBool()) {
            painter->setOpacity(0.45);
        }
        const QRect tile = option.rect.adjusted(1, 1, -2, -2);
        painter->fillRect(tile, isDarkTheme() ? QColor(22, 22, 24) : option.palette.color(QPalette::AlternateBase));
        const QPixmap source = index.data(Qt::UserRole + 4).value<QPixmap>();
        const int tileKind = index.data(Qt::UserRole + 12).toInt();
        const bool folderTile = tileKind == 1 || tileKind == 2 || tileKind == kKindPhone;
        const bool kindTile = !index.data(Qt::UserRole + 15).toString().isEmpty()
            && index.data(Qt::UserRole + 16).toInt() == 0;
        if (!source.isNull()) {
            if (folderTile) {
                const int pad = std::max(10, tile.width() / 7);
                const QRect box = tile.adjusted(pad, pad, -pad, -pad);
                const QPixmap scaled = source.scaled(
                    box.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawPixmap(
                    box.x() + (box.width() - scaled.width()) / 2,
                    box.y() + (box.height() - scaled.height()) / 2,
                    scaled);
            } else if (kindTile) {
                const QPixmap scaled = source.scaled(
                    tile.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawPixmap(
                    tile.x() + (tile.width() - scaled.width()) / 2,
                    tile.y() + (tile.height() - scaled.height()) / 2,
                    scaled);
            } else {
                const QPixmap scaled = source.scaled(
                    tile.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                const int cropX = std::max(0, (scaled.width() - tile.width()) / 2);
                const int cropY = std::max(0, (scaled.height() - tile.height()) / 2);
                painter->drawPixmap(
                    tile, scaled, QRect(cropX, cropY, tile.width(), tile.height()));
            }
        } else if (isPdfFileName(index.data(Qt::UserRole + 1).toString())) {
            painter->setPen(QColor(174, 174, 178));
            QFont badge(QStringLiteral("Segoe UI"));
            badge.setPixelSize(std::max(12, tile.height() / 8));
            badge.setWeight(QFont::DemiBold);
            painter->setFont(badge);
            painter->drawText(tile, Qt::AlignCenter, QStringLiteral("PDF"));
        }
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (hovered && !selected) {
            painter->fillRect(tile, QColor(255, 255, 255, 28));
        }
        if (selected) {
            painter->fillRect(tile, QColor(0, 0, 0, 70));
            painter->setPen(QPen(QColor(38, 114, 200), 3));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(tile.adjusted(1, 1, -2, -2));
        }
        if (folderTile) {
            const QString name = index.data(Qt::UserRole + 1).toString();
            const QRect shade(tile.left(), tile.bottom() - 36, tile.width(), 36);
            QLinearGradient fade(shade.topLeft(), shade.bottomLeft());
            fade.setColorAt(0.0, QColor(0, 0, 0, 0));
            fade.setColorAt(1.0, QColor(0, 0, 0, 190));
            painter->fillRect(shade, fade);
            QFont titleFont(QStringLiteral("Segoe UI"));
            titleFont.setPixelSize(12);
            titleFont.setWeight(QFont::DemiBold);
            painter->setFont(titleFont);
            painter->setPen(Qt::white);
            const QFontMetrics titleMetrics(titleFont);
            painter->drawText(
                QRect(tile.left() + 6, tile.bottom() - titleMetrics.height() - 6, tile.width() - 12, titleMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(name, Qt::ElideRight, tile.width() - 12));
            painter->restore();
            return;
        }
        if (kindTile) {
            painter->restore();
            return;
        }
        const QString fileName = index.data(Qt::UserRole + 1).toString();
        const int route = index.data(Qt::UserRole + 16).toInt();
        const bool knownAudio = !index.data(Qt::UserRole + 15).toString().isEmpty() && route == 2;
        if (knownAudio || (!index.data(Qt::UserRole + 15).isValid() && isAudioFileName(fileName))) {
            QString artist = index.data(Qt::UserRole + 13).toString();
            QString title = index.data(Qt::UserRole + 14).toString();
            if (title.isEmpty()) {
                const auto parsed = songLabel(fileName);
                artist = parsed.first;
                title = parsed.second;
            }
            const QRect shade(tile.left(), tile.bottom() - 46, tile.width(), 46);
            QLinearGradient fade(shade.topLeft(), shade.bottomLeft());
            fade.setColorAt(0.0, QColor(0, 0, 0, 0));
            fade.setColorAt(1.0, QColor(0, 0, 0, 190));
            painter->fillRect(shade, fade);
            const QRect text = tile.adjusted(6, 0, -6, -5);
            QFont titleFont(QStringLiteral("Segoe UI"));
            titleFont.setPixelSize(12);
            titleFont.setWeight(QFont::DemiBold);
            painter->setFont(titleFont);
            const QFontMetrics titleMetrics(titleFont);
            painter->setPen(Qt::white);
            painter->drawText(
                QRect(text.left(), text.bottom() - (artist.isEmpty() ? titleMetrics.height() : titleMetrics.height() + 14),
                    text.width(), titleMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(title, Qt::ElideRight, text.width()));
            if (!artist.isEmpty()) {
                QFont artistFont(QStringLiteral("Segoe UI"));
                artistFont.setPixelSize(11);
                painter->setFont(artistFont);
                painter->setPen(QColor(220, 220, 224));
                const QFontMetrics artistMetrics(artistFont);
                painter->drawText(
                    QRect(text.left(), text.bottom() - artistMetrics.height(), text.width(), artistMetrics.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    artistMetrics.elidedText(artist, Qt::ElideRight, text.width()));
            }
            painter->restore();
            return;
        }
        const bool pdfFile = route == 3 || isPdfFileName(fileName);
        const bool textFile = route == 4 || isTextFileName(fileName);
        if (pdfFile || textFile) {
            QString title = fileName;
            if (pdfFile) {
                const int dot = title.lastIndexOf(QLatin1Char('.'));
                if (dot > 0) {
                    title = title.left(dot);
                }
            }
            const QRect shade(tile.left(), tile.bottom() - 36, tile.width(), 36);
            QLinearGradient fade(shade.topLeft(), shade.bottomLeft());
            fade.setColorAt(0.0, QColor(0, 0, 0, 0));
            fade.setColorAt(1.0, QColor(0, 0, 0, 190));
            painter->fillRect(shade, fade);
            QFont titleFont(QStringLiteral("Segoe UI"));
            titleFont.setPixelSize(12);
            titleFont.setWeight(QFont::DemiBold);
            painter->setFont(titleFont);
            painter->setPen(Qt::white);
            const QFontMetrics titleMetrics(titleFont);
            painter->drawText(
                QRect(tile.left() + 6, tile.bottom() - titleMetrics.height() - 6, tile.width() - 12, titleMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(title, Qt::ElideRight, tile.width() - 12));
            painter->restore();
            return;
        }
        const QString duration = index.data(Qt::UserRole + 3).toString();
        if (!duration.isEmpty()) {
            QFont font = painter->font();
            font.setPixelSize(12);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            const QRect text = tile.adjusted(8, 0, -8, -6);
            painter->setPen(QColor(0, 0, 0, 180));
            painter->drawText(text.translated(0, 1), Qt::AlignLeft | Qt::AlignBottom, duration);
            painter->setPen(Qt::white);
            painter->drawText(text, Qt::AlignLeft | Qt::AlignBottom, duration);
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto* view = qobject_cast<const QListView*>(option.widget);
        if (view != nullptr && view->viewMode() == QListView::ListMode) {
            return QSize(std::max(1, option.rect.width()), 62);
        }
        if (view != nullptr && view->viewMode() == QListView::IconMode
            && view->gridSize().isValid()) {
            return view->gridSize();
        }
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

class IconGrid final : public QListWidget {
public:
    using QListWidget::QListWidget;

    // Called as a drag starts from this view, so the gallery can record the
    // rows being dragged before any drop target is known.
    std::function<void()> onDragStart;

    void setChromeInset(const int top, const int bottom) {
        if (chromeTop_ == top && chromeBottom_ == bottom) {
            return;
        }
        chromeTop_ = top;
        chromeBottom_ = bottom;
        setViewportMargins(0, top, 0, bottom);
    }

protected:
    void startDrag(Qt::DropActions actions) override {
        if (onDragStart) {
            onDragStart();
        }
        QListWidget::startDrag(actions);
    }

    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        layoutInstagramGrid(this);
    }

private:
    int chromeTop_{-1};
    int chromeBottom_{-1};
};

// The details tree needs the same drag-start hook as IconGrid. Qt hands the
// drop target only mime data, so the rows are recorded by the source view.
class GalleryTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

    std::function<void()> onDragStart;

protected:
    void startDrag(Qt::DropActions actions) override {
        if (onDragStart) {
            onDragStart();
        }
        QTreeWidget::startDrag(actions);
    }
};

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
    void enterEvent(QEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool close = glyph_ == CaptionGlyph::Close;
        if (underMouse()) {
            painter.fillRect(rect(), close ? QColor(232, 17, 35)
                : (isDarkTheme() ? QColor(255, 255, 255, 24) : QColor(0, 0, 0, 18)));
        }
        painter.setPen(QPen(close && underMouse() ? QColor(Qt::white) : palette().color(QPalette::WindowText), 1.0));
        painter.setBrush(Qt::NoBrush);
        const QPointF center(width() / 2.0, height() / 2.0);
        switch (glyph_) {
        case CaptionGlyph::Minimize:
            painter.drawLine(QPointF(center.x() - 5, center.y()), QPointF(center.x() + 5, center.y()));
            break;
        case CaptionGlyph::Maximize:
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 5, 10, 10), 1.5, 1.5);
            break;
        case CaptionGlyph::Restore: {
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 2, 8, 8), 1.2, 1.2);
            painter.drawLine(QPointF(center.x() - 2, center.y() - 4), QPointF(center.x() + 5, center.y() - 4));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 4), QPointF(center.x() + 5, center.y() + 3));
            break;
        }
        case CaptionGlyph::Close:
            painter.drawLine(QPointF(center.x() - 5, center.y() - 5), QPointF(center.x() + 5, center.y() + 5));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 5), QPointF(center.x() - 5, center.y() + 5));
            break;
        }
    }

private:
    CaptionGlyph glyph_;
};

QLabel* heading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageTitle"));
    // A wrapped title is what lets the auth card shrink with the window instead
    // of forcing a card (and a window) wider than a small screen.
    label->setWordWrap(true);
    return label;
}

QLabel* description(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("description"));
    label->setWordWrap(true);
    return label;
}

// Height a labelled control needs once the stylesheet's border, padding and the
// platform font are accounted for. The literal 38 this replaces was measured on
// one font: at larger text sizes the glyphs, the caret and the bottom border no
// longer fit, which read as "the path field and Browse are cut off".
int authControlHeight(const QFontMetrics& metrics) {
    return std::max(38, metrics.height() + 20);
}

// The storage-path row: text field plus its Browse action. Both keep the same
// height, the row keeps that height as a floor, and vertical policies are Fixed
// so a window dragged short makes the card scroll rather than shave the row.
void configureAuthRow(QWidget* row, QLineEdit* field, QPushButton* browse) {
    if (row == nullptr || field == nullptr || browse == nullptr) {
        return;
    }
    const int height = authControlHeight(field->fontMetrics());
    field->setMinimumHeight(height);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    browse->setMinimumHeight(height);
    browse->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    row->setMinimumHeight(height);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QLabel* errorLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(QStringLiteral("errorLabel"));
    label->setWordWrap(true);
    label->hide();
    return label;
}

QFrame* cardFor(QWidget* page) {
    auto* card = new QFrame(page);
    card->setObjectName(QStringLiteral("card"));
    card->setMaximumWidth(620);
    return card;
}

void placeCard(QWidget* page, QFrame* card) {
    // A short window scrolls the card instead of squeezing it: a compressed card
    // is what cut the bottom half off the path and password fields.
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* area = new QScrollArea(page);
    area->setObjectName(QStringLiteral("authScroll"));
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->viewport()->setAutoFillBackground(false);
    area->setStyleSheet(QStringLiteral(
        "QScrollArea#authScroll { background: transparent; border: none; }"));
    auto* host = new QWidget;
    host->setObjectName(QStringLiteral("authHost"));
    host->setStyleSheet(QStringLiteral("QWidget#authHost { background: transparent; }"));
    auto* inner = new QVBoxLayout(host);
    inner->setContentsMargins(48, 48, 48, 48);
    inner->addStretch();
    inner->addWidget(card, 0, Qt::AlignHCenter);
    inner->addStretch();
    area->setWidget(host);
    outer->addWidget(area);
    // The scroll area's own size hint is small, which let the window shrink to
    // below the card once the auth page was shown. Floor the window at the
    // card's MINIMUM size, not its preferred size: "sizeHint + 192" produced an
    // 812x520 slab that would not fit a small laptop screen, and a frame dragged
    // below the floor clipped the card's controls. Height beyond that scrolls.
    // QSize::grownBy is unavailable in the Windows build's Qt 5.12.
    const QSize cardMinimum = card->minimumSizeHint();
    page->setMinimumSize(QSize(cardMinimum.width() + 96, cardMinimum.height() + 48));
}

core::Argon2Parameters selectedParameters(const int profile) {
    core::Argon2Parameters parameters;
    if (profile == 1) {
        parameters.memory_kib = 512U * 1024U;
        parameters.iterations = 4U;
        parameters.parallelism = 2U;
    }
    return parameters;
}

// Right-click sheet. Same dark rounded card as Settings, not the system menu.
class CommandPopup final : public QDialog {
public:
    explicit CommandPopup(QWidget* parent)
        : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
        // The popup is its own top-level window, so it does not inherit the themed
        // palette of the window that opened it: in light theme it painted the
        // application's dark palette, i.e. a black card with black text.
        const QPalette themed = parent != nullptr ? parent->palette() : QApplication::palette();
        setPalette(themed);
        card_ = themed.color(QPalette::Base);
        border_ = card_.lightness() < 128 ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 20);
        setAttribute(Qt::WA_TranslucentBackground);
        setObjectName(QStringLiteral("commandPopup"));
        setMinimumWidth(210);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(2);
        // Hug the commands: an unconstrained popup used to open as a large empty
        // card, which is what a black rectangle over the library turned out to be.
        layout->setSizeConstraint(QLayout::SetFixedSize);
        setAutoFillBackground(false);
        // Authored in the dark palette: the theme manager rewrites widget sheets
        // for light mode, and a hand-computed light colour would be rewritten too
        // (that is how a light card kept white labels).
        setStyleSheet(QStringLiteral(
            "QDialog#commandPopup { background: transparent; border: none; }"
            "QPushButton#menuCommand, QPushButton#menuDanger {"
            "  color: #f2f2f7; background: transparent; border: none;"
            "  border-radius: 10px; text-align: left; padding: 0 16px;"
            "  min-height: 36px; font-size: 10pt; }"
            "QPushButton#menuCommand:hover { background: #2672c8; color: #ffffff; }"
            "QPushButton#menuDanger { color: #ff453a; }"
            "QPushButton#menuDanger:hover { background: rgba(255, 69, 58, 42); }"));
    }

    QPushButton* addCommand(const QString& text, const int result, const bool danger = false) {
        auto* layout = static_cast<QVBoxLayout*>(this->layout());
        if (danger && layout->count() > 0) {
            auto* line = new QWidget(this);
            line->setFixedHeight(1);
            line->setAttribute(Qt::WA_StyledBackground, true);
            line->setStyleSheet(QStringLiteral("background: palette(mid); margin: 4px 8px;"));
            layout->addSpacing(4);
            layout->addWidget(line);
            layout->addSpacing(4);
        }
        auto* button = new QPushButton(text, this);
        button->setObjectName(danger
            ? QStringLiteral("menuDanger")
            : QStringLiteral("menuCommand"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoDefault(false);
        button->setDefault(false);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, result] { done(result); });
        return button;
    }

    [[nodiscard]] bool empty() const {
        return layout() == nullptr || layout()->count() == 0;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(border_));
        painter.setBrush(card_);
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 14, 14);
    }

private:
    QColor card_;
    QColor border_;
};

void fadeInWidget(QWidget* widget) {
    if (widget == nullptr || !widget->isVisible()) {
        return;
    }
    auto* effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(0.0);
    widget->setGraphicsEffect(effect);
    auto* fade = new QPropertyAnimation(effect, "opacity", widget);
    fade->setDuration(220);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(fade, &QPropertyAnimation::finished, widget, [widget, effect] {
        QTimer::singleShot(0, widget, [widget, effect] {
            if (widget->graphicsEffect() == effect) {
                widget->setGraphicsEffect(nullptr);
            }
        });
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void fadeStack(QStackedWidget* stack, QWidget* next) {
    if (stack == nullptr || next == nullptr) {
        return;
    }
    if (stack->indexOf(next) < 0) {
        stack->addWidget(next);
    }
    const bool changed = stack->currentWidget() != next;
    stack->setCurrentWidget(next);
    if (!changed || !stack->isVisible()) {
        return;
    }
    auto* veil = new QWidget(stack);
    veil->setAttribute(Qt::WA_TransparentForMouseEvents);
    veil->setGeometry(stack->rect());
    veil->setStyleSheet(QStringLiteral("background-color: #000000;"));
    auto* effect = new QGraphicsOpacityEffect(veil);
    effect->setOpacity(0.72);
    veil->setGraphicsEffect(effect);
    veil->raise();
    veil->show();
    auto* fade = new QPropertyAnimation(effect, "opacity", veil);
    fade->setDuration(240);
    fade->setStartValue(0.72);
    fade->setEndValue(0.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(fade, &QPropertyAnimation::finished, veil, &QObject::deleteLater);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

void slideInBar(QWidget* bar) {
    if (bar == nullptr) {
        return;
    }
    const int full = std::max(bar->sizeHint().height(), 48);
    bar->setMinimumHeight(0);
    bar->setMaximumHeight(0);
    auto* slide = new QPropertyAnimation(bar, "maximumHeight", bar);
    slide->setDuration(240);
    slide->setStartValue(0);
    slide->setEndValue(full);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(slide, &QPropertyAnimation::valueChanged, bar, [bar](const QVariant& value) {
        bar->setMinimumHeight(value.toInt());
    });
    QObject::connect(slide, &QPropertyAnimation::finished, bar, [bar, full] {
        bar->setFixedHeight(full);
    });
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

class TextPage final : public QWidget {
public:
    explicit TextPage(QWidget* parent = nullptr)
        : QWidget(parent) {
        setStyleSheet(QStringLiteral("background: #101014;"));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(18, 14, 18, 16);
        column->setSpacing(10);
        auto* bar = new QHBoxLayout();
        back_ = new QPushButton(QStringLiteral("Back"), this);
        icons::withText(back_, QStringLiteral("arrow-left"));
        back_->setCursor(Qt::PointingHandCursor);
        back_->setStyleSheet(QStringLiteral(
            "QPushButton { color: #2672c8; background: transparent; border: none;"
            " font: 600 13px \"Segoe UI\"; padding: 4px 2px; }"));
        title_ = new QLabel(this);
        title_->setStyleSheet(QStringLiteral(
            "color: white; background: transparent; font: 600 15px \"Segoe UI\";"));
        bar->addWidget(back_);
        bar->addSpacing(8);
        bar->addWidget(title_, 1);
        save_ = new QPushButton(QStringLiteral("Save"), this);
        icons::withText(save_, QStringLiteral("check"));
        save_->setCursor(Qt::PointingHandCursor);
        save_->setStyleSheet(QStringLiteral(
            "QPushButton { color: white; background: #2672c8; border: none; border-radius: 8px;"
            " font: 600 13px \"Segoe UI\"; padding: 6px 14px; }"
            "QPushButton:disabled { background: #2c2c2e; color: #8e8e93; }"));
        bar->addWidget(save_);
        column->addLayout(bar);
        editor_ = new QPlainTextEdit(this);
        editor_->setReadOnly(true);
        editor_->setUndoRedoEnabled(true);
        editor_->setPlaceholderText(QStringLiteral("Start writing…"));
        editor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        editor_->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background: #161618; color: #f2f2f7; border: none; border-radius: 12px;"
            " font: 13px \"Cascadia Mono\", Consolas, monospace; padding: 16px;"
            " selection-background-color: #2672c8; }"));
        column->addWidget(editor_, 1);
    }

    QPushButton* backButton() const { return back_; }

    QPushButton* saveButton() const { return save_; }

    int bump() { return ++generation_; }

    int generation() const { return generation_; }

    void showDocument(const QString& title, const QString& body, const bool editable) {
        title_->setText(title);
        saved_ = body;
        editor_->setReadOnly(!editable);
        editor_->setUndoRedoEnabled(editable);
        save_->setVisible(editable);
        save_->setEnabled(editable);
        editor_->setPlainText(body);
        editor_->moveCursor(QTextCursor::Start);
    }

    QString documentText() const { return editor_->toPlainText(); }

    bool dirty() const { return editor_->toPlainText() != saved_; }

    void markSaved() { saved_ = editor_->toPlainText(); }

    void setBusy(const bool busy) {
        save_->setEnabled(!busy && save_->isVisible());
        editor_->setReadOnly(busy || !save_->isVisible());
    }

private:
    QPushButton* back_{nullptr};
    QPushButton* save_{nullptr};
    QLabel* title_{nullptr};
    QPlainTextEdit* editor_{nullptr};
    QString saved_;
    int generation_{0};
};

void placePopup(QDialog* popup, const QPoint& globalPosition) {
    popup->adjustSize();
    QPoint position = globalPosition;
    if (QScreen* screen = QGuiApplication::screenAt(globalPosition)) {
        const QRect area = screen->availableGeometry();
        if (position.x() + popup->width() > area.right()) {
            position.setX(std::max(area.left(), area.right() - popup->width() - 4));
        }
        if (position.y() + popup->height() > area.bottom()) {
            position.setY(std::max(area.top(), area.bottom() - popup->height() - 4));
        }
    }
    popup->move(position);
}

} // namespace

struct VaultOperationResult {
    bool succeeded{false};
    // The sync keys, derived inside the worker while the password is still in
    // hand. Empty when the derivation failed; the vault still opens, sync then
    // says it is locked.
    sync::Keys keys;
    // The vault was already open for a swap. The password checked out, so the
    // same vault stays in place instead of being opened a second time.
    bool resumed{false};
    std::unique_ptr<core::Vault> vault;
    core::VaultError error;
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), autoLockTimer_(new QTimer(this)), galleryCache_(std::make_unique<GalleryCache>()) {
    galleryPool_ = new QThreadPool(this);
    galleryPool_->setMaxThreadCount(2);
    setWindowTitle(QStringLiteral("Mega Vault Protect"));
#ifdef Q_OS_WIN
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint
        | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
#endif
    resize(1080, 700);
    setMinimumSize(780, 520);
    setAcceptDrops(true); // drag & drop video import

    autoLockTimer_->setSingleShot(true);
    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(180);
    connect(searchTimer_, &QTimer::timeout, this, [this] { refreshGallery(); });
    updateCheckTimer_ = new QTimer(this);
    updateCheckTimer_->setInterval(6 * 60 * 60 * 1000);
    connect(updateCheckTimer_, &QTimer::timeout, this, [this] { beginUpdateCheck(); });
    connect(autoLockTimer_, &QTimer::timeout, this, [this] {
        // Never auto-lock in the middle of an import/restore/remove batch:
        // lock() blocks on the vault mutex a worker currently holds and the
        // batch would fail with "vault is locked".
        // A running share keeps its own vault, so locking this window does
        // not cut the phone off. Skip only while a batch holds the UI vault.
        if (!batchBusy_) {
            // Idle lock is not a user gesture: no dialog, always hard lock.
            interactiveLock_ = false;
            lockVault();
        }
    });
    // Batch worker thread for imports/restores: the UI thread never blocks;
    // progress and completion arrive as queued signals.
    batchThread_ = new QThread(this);
    batchWorker_ = new BatchWorker();
    batchWorker_->moveToThread(batchThread_);
    connect(batchThread_, &QThread::finished, batchWorker_, &QObject::deleteLater);
    connect(batchWorker_, &BatchWorker::progress, this,
        [this](const qlonglong done, const qlonglong total) {
            if (progressBar_ != nullptr && total > 0) {
                progressBar_->setRange(0, 1000);
                progressBar_->setValue(static_cast<int>((done * 1000) / total));
            }
            if (peerCopyActive_ && total > 0) {
                for (auto& job : transfers_) {
                    if (job.state == PeerTransfer::Running) {
                        job.percent = static_cast<int>((done * 100) / total);
                        break;
                    }
                }
                refreshTransfers();
            }
        });
    connect(batchWorker_, &BatchWorker::fileFinished, this,
        [this](const int done, const int total) {
            showStatus(QStringLiteral("%1 %2 / %3…")
                .arg(batchLabel_, QString::number(done), QString::number(total)), false);
        });
    connect(batchWorker_, &BatchWorker::videoArrived, this,
        [this](const qlonglong id, const QString& name, const qlonglong size,
            const qlonglong folderId, const qlonglong importedAt) {
            revealVideo(id, name, size, folderId, importedAt);
        });
    connect(batchWorker_, &BatchWorker::folderArrived, this,
        [this](const qlonglong id, const qlonglong parentId, const QString& name) {
            revealFolder(id, parentId, name);
        });
    connect(batchWorker_, &BatchWorker::finished, this,
        [this](const bool ok, const QString& message, const int count) {
            finishBatch(ok, message, count);
        });
    batchThread_->start();
    importServer_ = new QLocalServer(this);
    connect(importServer_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = importServer_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                const QString text = QString::fromUtf8(socket->readAll());
                socket->deleteLater();
                QStringList paths;
                const auto lines = text.split(QLatin1Char('\n'), QString::SkipEmptyParts);
                for (const QString& line : lines) {
                    if (line != QStringLiteral("ACTIVATE")) {
                        paths.push_back(line);
                    }
                }
                if (paths.isEmpty()) {
                    showNormal();
                    raise();
                    activateWindow();
                } else {
                    acceptExternalPaths(paths);
                }
            });
        }
    });
    const QString inbox = QStringLiteral("MegaVaultProtect.inbox");
    QLocalServer::removeServer(inbox);
    importServer_->listen(inbox);
    qApp->installEventFilter(this);

    buildInterface();

    QSettings settings;
    const auto remembered = pathFromText(settings.value(QStringLiteral("vault/location")).toString());
    if (!remembered.empty() && core::Vault::exists(remembered)) {
        showLogin(remembered);
    } else {
        showSetup();
    }
}

MainWindow::~MainWindow() {
    if (pluginMusic_) pluginMusic_->setVault({});
    if (extensionTools_) extensionTools_->stopAndClear();
    closeSync();
    qApp->removeEventFilter(this);
    if (shareServer_) {
        shareServer_->setOnRunning({});
        shareServer_.reset();
    }
    // The player is an unparented top-level window (so it can be covered by
    // the main window), so close it explicitly to stop its worker thread
    // before the vault goes away.
    if (playerWindow_ != nullptr) {
        playerWindow_->close();
    }
    if (pdfWindow_ != nullptr) {
        pdfWindow_->close();
    }
    if (musicWindow_ != nullptr) {
        musicWindow_->close();
    }
    // Stop the batch worker before the vault goes away: quit the event loop,
    // then wait for the current batch item to finish (bounded).
    if (batchThread_ != nullptr) {
        batchThread_->quit();
        batchThread_->wait(5000);
    }
    if (adminWatcher_ != nullptr) {
        adminWatcher_->waitForFinished();
    }
    if (tagEditorWatcher_ != nullptr) {
        tagEditorWatcher_->waitForFinished();
    }
    if (vault_) {
        vault_->lock();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (handleExplorerDrag(watched, event)) {
        return true;
    }
    if (galleryStack_ != nullptr && watched == galleryStack_->parentWidget()
        && event->type() == QEvent::Resize) {
        layoutLibraryChrome();
    }
    if (event->type() == QEvent::Resize
        && ((iconList_ != nullptr && watched == iconList_->viewport())
            || (detailsTree_ != nullptr && watched == detailsTree_->viewport()))) {
        decorateVisible();
    }
    if (event->type() == QEvent::KeyPress && vault_ && vault_->is_unlocked()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && key->modifiers() == Qt::NoModifier) {
            QWidget* focus = QApplication::focusWidget();
            bool inGallery = false;
            for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
                if (qobject_cast<QLineEdit*>(widget) != nullptr
                    || qobject_cast<QComboBox*>(widget) != nullptr
                    || qobject_cast<QPushButton*>(widget) != nullptr) {
                    inGallery = false;
                    break;
                }
                if (widget == detailsTree_ || widget == iconList_) {
                    inGallery = true;
                    break;
                }
            }
            if (inGallery) {
                const int kind = selectedGalleryKind();
                if (pluginFolder_) {
                    // Enter is the keyboard's double-click, including inside the
                    // provider folder's albums and song rows.
                    int entry = -1;
                    QListWidgetItem* current = iconList_ != nullptr ? iconList_->currentItem() : nullptr;
                    if (current != nullptr && current->isSelected()) {
                        entry = current->data(Qt::UserRole + 21).toInt();
                    }
                    if (handlePluginRow(kind, entry)) {
                        return true;
                    }
                }
                if (kind == 1 || kind == 2 || kind == kKindPhone) {
                    activateCurrentItem();
                    return true;
                }
                const std::int64_t videoId = selectedVideoId();
                if (videoId >= 0) {
                    beginPlayback(videoId);
                    return true;
                }
            }
        }
        if (galleryHasFocus()
            && key->key() == Qt::Key_F2 && key->modifiers() == Qt::NoModifier) {
            const int kind = selectedGalleryKind();
            if (kind == 1) {
                const qlonglong role = galleryStack_->currentWidget() == detailsTree_
                    ? detailsTree_->currentItem()->data(0, Qt::UserRole).toLongLong()
                    : iconList_->currentItem()->data(Qt::UserRole).toLongLong();
                beginRenameFolder(-role);
                return true;
            }
            const std::int64_t videoId = selectedVideoId();
            if (videoId > 0) {
                beginRenameVideo(videoId);
                return true;
            }
        }
        if (galleryHasFocus()
            && key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier) {
            std::vector<std::int64_t> folder_ids;
            const auto take = [&folder_ids](const int kind, const qlonglong role) {
                if (kind == 1 && role < 0) {
                    folder_ids.push_back(-role);
                }
            };
            if (galleryStack_->currentWidget() == detailsTree_) {
                for (auto* item : detailsTree_->selectedItems()) {
                    take(item->data(0, Qt::UserRole + 12).toInt(),
                        item->data(0, Qt::UserRole).toLongLong());
                }
            } else {
                for (auto* item : iconList_->selectedItems()) {
                    take(item->data(Qt::UserRole + 12).toInt(),
                        item->data(Qt::UserRole).toLongLong());
                }
            }
            const auto videos = selectedVideoIds();
            if (!folder_ids.empty() && videos.empty() && folder_ids.size() == 1U) {
                beginRemoveFolder(folder_ids.front());
                return true;
            }
            std::unordered_set<qlonglong> inside_files;
            if (!folder_ids.empty()) {
                InWindowSheet confirm(this);
                confirm.setTitle(QStringLiteral("Remove"));
                confirm.setMessage(videos.empty()
                    ? QStringLiteral("Delete %1 folder(s) and everything inside them? This cannot be undone.")
                        .arg(folder_ids.size())
                    : QStringLiteral("Delete the selected folders, the files inside them, and the selected items? This cannot be undone."));
                confirm.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
                confirm.addButton(QStringLiteral("Delete"), 1, true, QStringLiteral("trash"));
                if (confirm.run() != 1) {
                    return true;
                }
                if (!videos.empty()) {
                    auto listed = vault_->list_videos();
                    if (listed) {
                        std::vector<std::int64_t> tree(folder_ids.begin(), folder_ids.end());
                        for (std::size_t index = 0; index < tree.size(); ++index) {
                            for (const auto& folder : folders_) {
                                if (folder.parent_id == tree[index]) {
                                    tree.push_back(folder.id);
                                }
                            }
                        }
                        for (const auto& video : listed.value()) {
                            if (std::find(tree.begin(), tree.end(), video.folder_id) != tree.end()) {
                                inside_files.insert(static_cast<qlonglong>(video.id));
                            }
                        }
                    }
                }
                for (const auto folder_id : folder_ids) {
                    auto removed = vault_->remove_folder(folder_id);
                    if (!removed) {
                        setError(galleryStatus_, vaultErrorText(removed.error()));
                        galleryStatus_->setVisible(true);
                        refreshGallery();
                        return true;
                    }
                }
                if (currentFolderId_ != 0) {
                    bool still = false;
                    for (const auto& folder : folders_) {
                        if (folder.id == currentFolderId_) {
                            still = true;
                        }
                    }
                    for (const auto folder_id : folder_ids) {
                        if (folder_id == currentFolderId_) {
                            still = false;
                        }
                    }
                    if (!still) {
                        currentFolderId_ = 0;
                    }
                }
            }
            if (folder_ids.empty() && videos.empty()) {
                return false;
            }
            if (!videos.empty()) {
                // Files that lived in a folder just deleted are already gone.
                if (galleryStack_->currentWidget() == detailsTree_) {
                    for (auto* item : detailsTree_->selectedItems()) {
                        if (item->data(0, Qt::UserRole + 12).toInt() != 0) {
                            continue;
                        }
                        if (inside_files.count(item->data(0, Qt::UserRole).toLongLong()) != 0) {
                            item->setSelected(false);
                        }
                    }
                } else if (iconList_ != nullptr) {
                    for (auto* item : iconList_->selectedItems()) {
                        if (item->data(Qt::UserRole + 12).toInt() != 0) {
                            continue;
                        }
                        if (inside_files.count(item->data(Qt::UserRole).toLongLong()) != 0) {
                            item->setSelected(false);
                        }
                    }
                }
                if (!selectedVideoIds().empty()) {
                    beginRemoveSelected(folder_ids.empty());
                } else {
                    refreshGallery();
                }
            } else {
                refreshGallery();
            }
            return true;
        }
        if (galleryHasFocus()
            && key->key() == Qt::Key_N
            && key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            beginNewFolder();
            return true;
        }
        if (galleryHasFocus() && key->modifiers() == Qt::ControlModifier) {
            if (key->key() == Qt::Key_X) {
                cutSelection();
                return true;
            }
            if (key->key() == Qt::Key_V) {
                pasteCutSelection();
                return true;
            }
            if (key->key() == Qt::Key_C) {
                // The vault stores one copy of each file by content hash, so a
                // duplicate cannot exist: say so instead of pretending to copy.
                showStatus(QStringLiteral(
                    "Files are stored once by content — there is no copy to paste. "
                    "Use Cut (Ctrl+X) to move an item."), false);
                return true;
            }
        }
        // Escape drops a pending cut, the way Explorer cancels a cut.
        if (galleryHasFocus() && key->key() == Qt::Key_Escape && !cutItems_.empty()) {
            clearCut();
            showStatus(QStringLiteral("Cut cancelled."), false);
            return true;
        }
        if (galleryHasFocus() && key->key() == Qt::Key_Left
            && key->modifiers() == Qt::AltModifier) {
            goUpFolder();
            return true;
        }
        if (galleryHasFocus() && key->key() == Qt::Key_F5
            && key->modifiers() == Qt::NoModifier) {
            refreshGallery();
            return true;
        }
        if (key->key() == Qt::Key_Backspace && key->modifiers() == Qt::NoModifier) {
            QWidget* focus = QApplication::focusWidget();
            for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
                if (qobject_cast<QLineEdit*>(widget) != nullptr) {
                    break;
                }
                if (widget == detailsTree_ || widget == iconList_) {
                    if (currentFolderId_ != 0) {
                        std::int64_t parent = 0;
                        for (const auto& folder : folders_) {
                            if (folder.id == currentFolderId_) {
                                parent = folder.parent_id;
                                break;
                            }
                        }
                        openFolder(parent);
                    }
                    return true;
                }
            }
        }
    }
    if (vault_ && vault_->is_unlocked()) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
            resetAutoLock();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange) {
        // Overlay widgets paint themselves from a stylesheet, so a theme switch
        // has to repaint the provider row buttons as well.
        syncPluginRowButtons();
        // Row art is a pixmap painted with the palette; rebuild the folder so a
        // song's plate follows the new theme instead of keeping the old one.
        if (pluginFolder_) rebuildPluginFolder();
    }
    if (event->type() == QEvent::WindowStateChange) {
        if (captionBar_ != nullptr) {
            captionBar_->setVisible(!isFullScreen());
        }
        if (captionMax_ != nullptr) {
            static_cast<CaptionButton*>(captionMax_)->setGlyph(
                isMaximized() ? CaptionGlyph::Restore : CaptionGlyph::Maximize);
        }
        if (isMinimized() && musicWindow_ != nullptr) {
            musicWindow_->showWhileMinimized();
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    layoutInstagramGrid(iconList_);
#ifdef Q_OS_WIN
    if (!frameReady_) {
        frameReady_ = true;
        HWND hwnd = reinterpret_cast<HWND>(winId());
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        style |= WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        const MARGINS shadow{0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd, &shadow);
        const int corner = 2; // DWMWCP_ROUND, Windows 11
        DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
            // The system hands a maximized window a rect that spills past the
            // monitor. Inset it by the frame so it sits on the work area
            // instead of sliding under the taskbar or leaving a gap.
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
        if (msg->message == WM_NCHITTEST) {
            const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            const QPoint pos = mapFromGlobal(global);
            const bool zoomed = IsZoomed(msg->hwnd);
            // Buttons win over the resize border, or the close control cannot be clicked.
            if (captionBar_ != nullptr) {
                const QPoint inCaption = captionBar_->mapFromGlobal(global);
                if (captionBar_->rect().contains(inCaption)) {
                    QWidget* child = captionBar_->childAt(inCaption);
                    if (qobject_cast<QAbstractButton*>(child) != nullptr) {
                        *result = HTCLIENT;
                    } else if (!zoomed && inCaption.y() < 4) {
                        *result = HTTOP;
                    } else {
                        *result = HTCAPTION;
                    }
                    return true;
                }
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
        if (msg->message == WM_GETMINMAXINFO) {
            auto* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
            info->ptMinTrackSize.x = minimumWidth();
            info->ptMinTrackSize.y = minimumHeight();
            return false;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::buildInterface() {
    pluginHost_ = new PluginHost({}, this);
    connect(pluginHost_, &PluginHost::changed, this, [this] {
        bool music = false, tools = false;
        for (const auto& manifest : pluginHost_->manifests()) {
            music |= manifest.protocol=="monochrome-tracks-v1" && pluginHost_->enabled(manifest.id);
            tools |= manifest.protocol=="megavault-actions-v1" && pluginHost_->enabled(manifest.id);
        }
        if (pluginMusicButton_) pluginMusicButton_->setVisible(music);
        if (extensionToolsButton_) extensionToolsButton_->setVisible(tools);
        if (!music && pluginMusic_ && contentStack_->currentWidget() == pluginMusic_) contentStack_->setCurrentWidget(pages_);
        if (!tools && extensionTools_ && contentStack_->currentWidget() == extensionTools_) contentStack_->setCurrentWidget(pages_);
    });
    connect(pluginHost_, &PluginHost::openRequested, this, [this](const QString& id) {
        for(const auto& m:pluginHost_->manifests())if(m.id==id) {
            if(m.protocol=="monochrome-tracks-v1")openPluginMusic();else openExtensionTools(id);
            return;
        }
    });
    auto* shell = new QWidget(this);
    auto* shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

#ifdef Q_OS_WIN
    captionBar_ = new QWidget(shell);
    captionBar_->setFixedHeight(32);
    captionBar_->setObjectName(QStringLiteral("captionBar"));
    captionBar_->setAttribute(Qt::WA_StyledBackground, true);
    captionBar_->setStyleSheet(QStringLiteral(
        "QWidget#captionBar { background: #000000; border: none; }"));
    auto* captionLayout = new QHBoxLayout(captionBar_);
    captionLayout->setContentsMargins(14, 0, 0, 0);
    captionLayout->setSpacing(0);
    auto* captionTitle = new QLabel(QStringLiteral("Mega Vault Protect"), captionBar_);
    captionTitle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QFont captionFont(QStringLiteral("Segoe UI"));
    captionFont.setPointSize(9);
    captionTitle->setFont(captionFont);
    captionTitle->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 210); background: transparent;"));
    captionLayout->addWidget(captionTitle);
    captionLayout->addStretch(1);
    auto* minimize = new CaptionButton(CaptionGlyph::Minimize, captionBar_);
    captionMax_ = new CaptionButton(CaptionGlyph::Maximize, captionBar_);
    auto* closeButton = new CaptionButton(CaptionGlyph::Close, captionBar_);
    captionLayout->addWidget(minimize);
    captionLayout->addWidget(captionMax_);
    captionLayout->addWidget(closeButton);
    connect(minimize, &QAbstractButton::clicked, this, [this] { showMinimized(); });
    connect(captionMax_, &QAbstractButton::clicked, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(closeButton, &QAbstractButton::clicked, this, &QWidget::close);
    shellLayout->addWidget(captionBar_);
#endif

    contentStack_ = new QStackedWidget(shell);
    pages_ = new QStackedWidget(contentStack_);
    setupPage_ = buildSetupPage();
    loginPage_ = buildLoginPage();
    unlockedPage_ = buildUnlockedPage();
    pages_->addWidget(setupPage_);
    pages_->addWidget(loginPage_);
    pages_->addWidget(unlockedPage_);
    contentStack_->addWidget(pages_);
    shellLayout->addWidget(contentStack_, 1);
    setCentralWidget(shell);
}

QWidget* MainWindow::buildSetupPage() {
    auto* page = new QWidget(this);
    auto* card = cardFor(page);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(42, 38, 42, 40);
    layout->setSpacing(14);
    layout->addWidget(heading(QStringLiteral("Create your encrypted vault"), card));
    layout->addWidget(description(
        QStringLiteral("Choose a private storage location and a strong password. "
                       "The password is never stored."), card));

    auto* locationHost = new QWidget(card);
    auto* locationRow = new QHBoxLayout(locationHost);
    locationRow->setContentsMargins(0, 0, 0, 0);
    setupLocation_ = new QLineEdit(locationHost);
    setupLocation_->setPlaceholderText(QStringLiteral("Vault storage folder"));
    setupLocation_->setTextMargins(10, 0, 8, 0);
    setupLocation_->setCursorPosition(0);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), locationHost);
    icons::withText(browse, QStringLiteral("folder-open"));
    locationRow->addWidget(setupLocation_, 1);
    locationRow->addWidget(browse);
    configureAuthRow(locationHost, setupLocation_, browse);
    layout->addWidget(locationHost);

    setupPassword_ = new QLineEdit(card);
    setupPassword_->setEchoMode(QLineEdit::Password);
    setupPassword_->setPlaceholderText(QStringLiteral("Password"));
    setupConfirmation_ = new QLineEdit(card);
    setupConfirmation_->setEchoMode(QLineEdit::Password);
    setupConfirmation_->setPlaceholderText(QStringLiteral("Confirm password"));
    layout->addWidget(setupPassword_);
    layout->addWidget(setupConfirmation_);

    securityProfile_ = new QComboBox(card);
    securityProfile_->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    securityProfile_->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    layout->addWidget(description(QStringLiteral("Encryption strength"), card));
    layout->addWidget(securityProfile_);

    setupError_ = errorLabel(card);
    layout->addWidget(setupError_);

    createButton_ = new QPushButton(QStringLiteral("Create vault"), card);
    icons::withText(createButton_, QStringLiteral("plus"));
    createButton_->setProperty("primary", true);
    auto* existingButton = new QPushButton(QStringLiteral("Open an existing vault"), card);
    icons::withText(existingButton, QStringLiteral("folder-open"));
    layout->addWidget(createButton_);
    layout->addWidget(existingButton);

    connect(browse, &QPushButton::clicked, this, [this] { selectSetupLocation(); });
    connect(createButton_, &QPushButton::clicked, this, [this] { beginCreate(); });
    connect(existingButton, &QPushButton::clicked, this, [this] { showLogin({}); });
    connect(setupConfirmation_, &QLineEdit::returnPressed, this, [this] { beginCreate(); });

    placeCard(page, card);
    return page;
}

QWidget* MainWindow::buildLoginPage() {
    auto* page = new QWidget(this);
    auto* card = cardFor(page);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(42, 38, 42, 40);
    layout->setSpacing(14);
    layout->addWidget(heading(QStringLiteral("Unlock Mega Vault Protect"), card));
    layout->addWidget(description(
        QStringLiteral("Enter the vault password to unlock the encrypted database."), card));

    loginLocationRow_ = new QWidget(card);
    auto* locationRow = new QHBoxLayout(loginLocationRow_);
    locationRow->setContentsMargins(0, 0, 0, 0);
    loginLocation_ = new QLineEdit(loginLocationRow_);
    loginLocation_->setPlaceholderText(QStringLiteral("Vault storage folder"));
    loginLocation_->setTextMargins(10, 0, 8, 0);
    loginLocation_->setCursorPosition(0);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), loginLocationRow_);
    icons::withText(browse, QStringLiteral("folder-open"));
    locationRow->addWidget(loginLocation_, 1);
    locationRow->addWidget(browse);
    configureAuthRow(loginLocationRow_, loginLocation_, browse);
    // The remembered vault path is longer than this field, and a line edit that
    // keeps the caret at the end shows only "...\scratch\ux-vault", which reads
    // as a clipped control. Show the start of the path and keep the whole value
    // in a tooltip.
    QObject::connect(loginLocation_, &QLineEdit::textChanged, loginLocation_, [this] {
        loginLocation_->setCursorPosition(0);
        loginLocation_->setToolTip(loginLocation_->text());
    });
    layout->addWidget(loginLocationRow_);

    cloudFields_ = new QWidget(card);
    auto* cloudLayout = new QVBoxLayout(cloudFields_);
    cloudLayout->setContentsMargins(0, 0, 0, 0);
    cloudLayout->setSpacing(8);
    cloudAddress_ = new QLineEdit(cloudFields_);
    cloudAddress_->setPlaceholderText(QStringLiteral("Cloud address (host:port)"));
    cloudFingerprint_ = new QLineEdit(cloudFields_);
    cloudFingerprint_->setPlaceholderText(QStringLiteral("Server public key"));
    cloudLayout->addWidget(cloudAddress_);
    cloudLayout->addWidget(cloudFingerprint_);
    cloudFields_->hide();
    layout->addWidget(cloudFields_);

#ifdef Q_OS_ANDROID
    // Android has no desktop file dialogs and no user-visible filesystem;
    // the vault lives in the app-private sandbox (no storage permission
    // needed, wiped with the app). The Browse buttons become no-ops because
    // QFileDialog returns an empty path on Android.
    const QString sandbox =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    setupLocation_->setText(QDir::toNativeSeparators(sandbox));
    loginLocation_->setText(QDir::toNativeSeparators(sandbox));
#endif

    loginPassword_ = new QLineEdit(card);
    loginPassword_->setEchoMode(QLineEdit::Password);
    loginPassword_->setPlaceholderText(QStringLiteral("Password"));
    layout->addWidget(loginPassword_);
    // Every auth field keeps a font-derived floor. A squeezed card must scroll
    // (placeCard puts the card in a scroll area) instead of compressing a field
    // below the height its border, padding and glyphs need.
    for (QLineEdit* field : {loginLocation_, loginPassword_, setupLocation_,
             setupPassword_, setupConfirmation_}) {
        if (field != nullptr) {
            field->setMinimumHeight(authControlHeight(field->fontMetrics()));
            field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
    }

    loginError_ = errorLabel(card);
    layout->addWidget(loginError_);

    unlockButton_ = new QPushButton(QStringLiteral("Unlock vault"), card);
    icons::withText(unlockButton_, QStringLiteral("lock-open"));
    unlockButton_->setProperty("primary", true);
    auto* newVaultButton = new QPushButton(QStringLiteral("Create a new vault"), card);
    icons::withText(newVaultButton, QStringLiteral("plus"));
    layout->addWidget(unlockButton_);
    layout->addWidget(newVaultButton);

    // Explicit "keep sharing" state. It has to be impossible to mistake for a
    // real lock: the vault keys are still in memory and the phone still reads
    // them, so the banner says exactly that and offers the hard lock.
    privacyBanner_ = new QWidget(card);
    auto* privacyLayout = new QVBoxLayout(privacyBanner_);
    privacyLayout->setContentsMargins(0, 0, 0, 0);
    privacyLayout->setSpacing(6);
    privacyBannerText_ = new QLabel(privacyBanner_);
    privacyBannerText_->setWordWrap(true);
    privacyBannerText_->setProperty("problem", true);
    privacyLockButton_ = new QPushButton(
        QStringLiteral("Lock the vault now and stop sharing"), privacyBanner_);
    icons::withText(privacyLockButton_, QStringLiteral("lock"));
    privacyLockButton_->setProperty("primary", true);
    privacyLayout->addWidget(privacyBannerText_);
    privacyLayout->addWidget(privacyLockButton_);
    privacyBanner_->hide();
    layout->addWidget(privacyBanner_);
    connect(privacyLockButton_, &QPushButton::clicked, this, [this] {
        privacyScreenActive_ = false;
        lockVault();
    });

    connect(browse, &QPushButton::clicked, this, [this] { selectLoginLocation(); });
    connect(unlockButton_, &QPushButton::clicked, this, [this] { beginOpen(); });
    connect(loginPassword_, &QLineEdit::returnPressed, this, [this] { beginOpen(); });

    connect(newVaultButton, &QPushButton::clicked, this, [this] { showSetup(); });

    placeCard(page, card);
    return page;
}

QWidget* MainWindow::buildUnlockedPage() {
    auto* page = new QWidget(this);
    page->installEventFilter(this);

    // Translucent command bar. Icon and list views scroll underneath it.
    libraryTop_ = new FrostedBar(FrostedBar::Band::Top, page);
    auto* toolbar = libraryTop_;
    auto* toolbarLayout = new QVBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(16, 10, 12, 10);
    toolbarLayout->setSpacing(8);

    viewModeCombo_ = new QComboBox(toolbar);
    viewModeCombo_->addItem(QStringLiteral("Details"));
    viewModeCombo_->addItem(QStringLiteral("Icons"));
    viewModeCombo_->addItem(QStringLiteral("List"));
    viewModeCombo_->hide();
    tagFilterButton_ = new TagFilterButton(toolbar);
    tagFilterButton_->setMinimumWidth(150);
    searchEdit_ = new QLineEdit(toolbar);
    searchEdit_->setPlaceholderText(QStringLiteral("Search tags or names…"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(220);
    const auto makeIconButton = [](QWidget* parent, const QString& icon, const QString& tip,
                                   const bool filled) {
        auto* button = new QPushButton(parent);
        if (filled) {
            button->setIcon(icons::lucideSolid(icon, QColor(0xff, 0xff, 0xff)));
        } else {
            icons::apply(button, icon, icons::Tone::Normal);
        }
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(36, 36);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tip);
        button->setStyleSheet(filled
            ? QStringLiteral(
                "QPushButton { background: #2672c8; border: none; border-radius: 18px; padding: 0; min-height: 36px; max-height: 36px; }"
                "QPushButton:hover { background: #2168b8; }"
                "QPushButton:disabled { background: rgba(38, 114, 200, 90); }")
            : QStringLiteral(
                "QPushButton { background: transparent; border: none; border-radius: 18px; padding: 0; min-height: 36px; max-height: 36px; }"
                "QPushButton:hover { background: rgba(255, 255, 255, 28); }"
                "QPushButton:disabled { background: transparent; }"
                "QPushButton[sharing=\"true\"] { background: rgba(38, 114, 200, 90); }"));
        return button;
    };
    // Offered only once GitHub has a newer release, the way the phone puts a
    // download icon in its library header.
    updateButton_ = makeIconButton(toolbar, QStringLiteral("download"),
        QStringLiteral("Check for a newer version"), false);
    updateButton_->hide();
    importButton_ = makeIconButton(toolbar, QStringLiteral("plus"),
        QStringLiteral("Add files to this vault"), true);
    // Folder import uses the same button. Both stay in step while a batch runs.
    importFolderButton_ = importButton_;
    shareButton_ = makeIconButton(toolbar, QStringLiteral("wifi"),
        QStringLiteral("Share this vault on Wi-Fi over an encrypted connection. "
                       "The phone must enter the vault password. Locking this PC does not stop sharing."),
        false);
    settingsButton_ = makeIconButton(toolbar, QStringLiteral("settings"),
        QStringLiteral("Change the password, manage tags, and adjust player settings"), false);
    auto* lockButton = makeIconButton(toolbar, QStringLiteral("lock"),
        QStringLiteral("Lock this PC. A phone that already entered the password keeps access until you stop sharing."),
        false);

    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);
    auto* libraryTitle = new QLabel(QStringLiteral("Library"), toolbar);
    QFont libraryFont(QStringLiteral("Segoe UI"));
    libraryFont.setPointSize(16);
    libraryTitle->setFont(libraryFont);
    libraryTitle->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    titleRow->addWidget(libraryTitle);
    titleRow->addStretch(1);
    titleRow->addWidget(updateButton_);
    titleRow->addWidget(importButton_);
    titleRow->addWidget(shareButton_);
    auto* syncButton = new QPushButton(QStringLiteral("Sync"), toolbar);
    icons::withText(syncButton, QStringLiteral("refresh-cw"));
    syncButton->setObjectName(QStringLiteral("syncSection"));
    syncButton->setCursor(Qt::PointingHandCursor);
    syncButton->setFocusPolicy(Qt::NoFocus);
    // A section entry, drawn like the other text actions in the chrome, not a
    // boxed button between the round icons.
    syncButton->setStyleSheet(QStringLiteral(
        "QPushButton { color: #2672c8; background: transparent; border: none;"
        " padding: 0 10px; min-height: 32px; max-height: 32px;"
        " font: 600 13px \"Segoe UI\"; }"
        "QPushButton:hover { color: #5aa6f0; background: rgba(255, 255, 255, 18);"
        " border-radius: 16px; }"));
    syncButton->setToolTip(QStringLiteral("Encrypted sync: sign in to your account, pick a vault, "
                                         "and let it sync lazily in both directions"));
    connect(syncButton, &QPushButton::clicked, this, &MainWindow::openSync);
    titleRow->addWidget(syncButton);
    pluginMusicButton_ = new QPushButton(QStringLiteral("Music"), toolbar);
    pluginMusicButton_->setObjectName(QStringLiteral("musicSection"));
    pluginMusicButton_->setStyleSheet(syncButton->styleSheet());
    pluginMusicButton_->setCursor(Qt::PointingHandCursor);
    pluginMusicButton_->setToolTip(QStringLiteral("Music from your enabled plugin providers"));
    bool musicEnabled = false;
    for (const auto& manifest : pluginHost_->manifests()) musicEnabled |= manifest.protocol=="monochrome-tracks-v1" && pluginHost_->enabled(manifest.id);
    pluginMusicButton_->setVisible(musicEnabled);
    connect(pluginMusicButton_, &QPushButton::clicked, this, [this] { openPluginMusic(); });
    titleRow->addWidget(pluginMusicButton_);
    extensionToolsButton_ = new QPushButton(QStringLiteral("Tools"), toolbar);
    extensionToolsButton_->setObjectName(QStringLiteral("toolsSection"));
    extensionToolsButton_->setStyleSheet(syncButton->styleSheet());
    extensionToolsButton_->setCursor(Qt::PointingHandCursor);
    bool toolsEnabled = false;
    for(const auto& manifest:pluginHost_->manifests()) toolsEnabled |= manifest.protocol=="megavault-actions-v1" && pluginHost_->enabled(manifest.id);
    extensionToolsButton_->setVisible(toolsEnabled);
    connect(extensionToolsButton_,&QPushButton::clicked,this,[this]{openExtensionTools();});
    titleRow->addWidget(extensionToolsButton_);
    titleRow->addWidget(settingsButton_);
    titleRow->addWidget(lockButton);
    toolbarLayout->addLayout(titleRow);

    searchEdit_->setPlaceholderText(QStringLiteral("Search names or tags"));
    searchEdit_->setToolTip(QStringLiteral("Filter the library by file name or tag"));
    searchEdit_->setMinimumWidth(160);
    auto* modeHost = new QWidget(toolbar);
    modeHost->setObjectName(QStringLiteral("viewModes"));
    modeHost->setStyleSheet(QStringLiteral(
        "QWidget#viewModes { background: rgba(255, 255, 255, 14);"
        " border: 1px solid rgba(255, 255, 255, 28); border-radius: 18px; }"));
    auto* modeLayout = new QHBoxLayout(modeHost);
    modeLayout->setContentsMargins(2, 2, 2, 2);
    modeLayout->setSpacing(0);
    auto* detailsMode = new QPushButton(modeHost);
    auto* iconsMode = new QPushButton(modeHost);
    auto* listMode = new QPushButton(modeHost);
    icons::apply(detailsMode, QStringLiteral("rows-3"));
    detailsMode->setToolTip(QStringLiteral("Details"));
    icons::apply(iconsMode, QStringLiteral("layout-grid"));
    iconsMode->setToolTip(QStringLiteral("Icons"));
    icons::apply(listMode, QStringLiteral("list"));
    listMode->setToolTip(QStringLiteral("List"));
    viewModeGroup_ = new QButtonGroup(toolbar);
    viewModeGroup_->setExclusive(true);
    const QString modeStyle = QStringLiteral(
        "QPushButton { min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px;"
        " padding: 0; border-radius: 16px; background: transparent; border: none; }"
        "QPushButton:checked { background: rgba(38, 114, 200, 90); }"
        "QPushButton:hover { background: rgba(255, 255, 255, 18); }"
        "QPushButton:checked:hover { background: rgba(38, 114, 200, 120); }");
    int modeId = 0;
    for (auto* button : {detailsMode, iconsMode, listMode}) {
        button->setIconSize(QSize(18, 18));
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(modeStyle);
        viewModeGroup_->addButton(button, modeId);
        modeLayout->addWidget(button);
        ++modeId;
    }
    sortButton_ = new QPushButton(toolbar);
    sortButton_->setFocusPolicy(Qt::NoFocus);
    sortButton_->setCursor(Qt::PointingHandCursor);
    sortButton_->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 32px; padding: 0 12px; border-radius: 16px;"
        " background: transparent; border: none; color: #2672c8; }"));
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);
    filterRow->addWidget(searchEdit_, 1);
    filterRow->addWidget(sortButton_);
    filterRow->addWidget(modeHost);
    filterRow->addWidget(tagFilterButton_);
    toolbarLayout->addLayout(filterRow);

    crumbHost_ = new QWidget(toolbar);
    crumbHost_->setStyleSheet(QStringLiteral("background: transparent;"));
    // The bar's height is computed from this row's size hint (layoutLibraryChrome). A
    // rebuild empties the row while it recreates the crumbs, and a hint measured in that
    // window came back degenerate: the bar was then resized below its children's minimums
    // and the path bar was clipped. The floor keeps the row's contribution stable.
    crumbHost_->setMinimumHeight(40);
    crumbLayout_ = new QHBoxLayout(crumbHost_);
    crumbLayout_->setContentsMargins(0, 0, 0, 0);
    crumbLayout_->setSpacing(2);
    toolbarLayout->addWidget(crumbHost_);
    rebuildBreadcrumb();

    // Explorer-style gallery views fill the window.
    auto* galleryTree = new GalleryTree(page);
    galleryTree->onDragStart = [this] { captureDragPayload(true); };
    detailsTree_ = galleryTree;
    detailsTree_->setObjectName(QStringLiteral("gallery"));
    detailsTree_->setColumnCount(7);
    detailsTree_->setHeaderLabels({
        QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Duration"),
        QStringLiteral("Resolution"), QStringLiteral("Codec"), QStringLiteral("Tags"),
        QStringLiteral("Imported")});
    detailsTree_->setRootIsDecorated(false);
    detailsTree_->setAlternatingRowColors(true);
    detailsTree_->setUniformRowHeights(true);
    detailsTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    detailsTree_->setAcceptDrops(true);
    detailsTree_->viewport()->setAcceptDrops(true);
    detailsTree_->setDragEnabled(true);
    detailsTree_->setDragDropMode(QAbstractItemView::DragDrop);
    detailsTree_->setDefaultDropAction(Qt::TargetMoveAction);
    detailsTree_->viewport()->installEventFilter(this);
    detailsTree_->installEventFilter(this);
    detailsTree_->header()->setStretchLastSection(true);
    detailsTree_->setSortingEnabled(false);
    detailsTree_->header()->setSectionsClickable(true);
    detailsTree_->header()->setSortIndicatorShown(true);
    connect(detailsTree_->header(), &QHeaderView::sectionClicked, this, [this](const int section) {
        if (section >= 0 && section <= 6) {
            applyGallerySort(section);
        }
    });
    iconList_ = new IconGrid(page);
    static_cast<IconGrid*>(iconList_)->onDragStart = [this] { captureDragPayload(false); };
    iconList_->setObjectName(QStringLiteral("gallery"));
    iconList_->setItemDelegate(new InstagramDelegate(iconList_));
    // The provider folder's Download buttons are widgets over rows: keep them on
    // their rows when the list scrolls.
    connect(iconList_->verticalScrollBar(), &QScrollBar::valueChanged, this,
        [this] { syncPluginRowButtons(); });
    iconList_->setViewMode(QListView::IconMode);
    iconList_->setSpacing(0);
    iconList_->setWordWrap(false);
    iconList_->setResizeMode(QListView::Adjust);
    iconList_->setMovement(QListView::Static);
    iconList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    iconList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    iconList_->setAcceptDrops(true);
    iconList_->viewport()->setAcceptDrops(true);
    iconList_->setDragEnabled(true);
    iconList_->setDragDropMode(QAbstractItemView::DragDrop);
    iconList_->setDefaultDropAction(Qt::TargetMoveAction);
    iconList_->viewport()->installEventFilter(this);
    iconList_->installEventFilter(this);
    connect(iconList_->verticalScrollBar(), &QAbstractSlider::valueChanged, this, [this](int) {
        decorateVisible();
    });
    connect(detailsTree_->verticalScrollBar(), &QAbstractSlider::valueChanged, this, [this](int) {
        decorateVisible();
    });

    galleryStack_ = new QStackedWidget(page);
    galleryStack_->addWidget(detailsTree_);
    galleryStack_->addWidget(iconList_);
    // The footer follows the selection: four selected files report their own
    // combined size instead of the whole folder's.
    connect(iconList_, &QListWidget::itemSelectionChanged, this, [this] {
        updateSelectionFooter();
    });
    connect(detailsTree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateSelectionFooter();
    });

    // Explorer-style folder pane. It sits beside both gallery views and is laid
    // out by layoutLibraryChrome, because the library page uses floating bars
    // instead of a layout. Its width collapses to a thin strip.
    folderPane_ = new QWidget(page);
    folderPane_->setObjectName(QStringLiteral("folderPane"));
    // Plain QWidget needs this for the stylesheet background and divider to paint.
    folderPane_->setAttribute(Qt::WA_StyledBackground, true);
    // Palette roles: the pane and its tree have to read correctly in both themes.
    folderPane_->setStyleSheet(QStringLiteral(
        "QWidget#folderPane { background: palette(window);"
        " border-right: 1px solid palette(midlight); }"
        "QTreeWidget#folderTree { background: transparent; border: none; color: palette(text); }"
        "QTreeWidget#folderTree::item { min-height: 26px; border-radius: 6px; }"
        "QTreeWidget#folderTree::item:hover { background: palette(alternate-base); }"
        "QTreeWidget#folderTree::item:selected { background: palette(highlight);"
        " color: palette(highlighted-text); }"
        "QLabel#folderPaneTitle { color: palette(mid); font-size: 9pt;"
        " font-weight: 600; background: transparent; }"
        "QPushButton#folderPaneToggle { background: transparent; border: none; padding: 0; }"
        "QPushButton#folderPaneToggle:hover { background: palette(alternate-base);"
        " border-radius: 6px; }"));
    auto* paneLayout = new QVBoxLayout(folderPane_);
    paneLayout->setContentsMargins(6, 6, 6, 6);
    paneLayout->setSpacing(4);
    auto* paneHeader = new QHBoxLayout();
    paneHeader->setContentsMargins(6, 0, 0, 0);
    paneHeader->setSpacing(2);
    folderPaneLabel_ = new QLabel(QStringLiteral("Folders"), folderPane_);
    folderPaneLabel_->setObjectName(QStringLiteral("folderPaneTitle"));
    folderPaneToggle_ = new QPushButton(folderPane_);
    // The glyph is set from the collapsed state elsewhere; start collapsed-to-open.
    icons::apply(folderPaneToggle_, QStringLiteral("chevron-left"));
    folderPaneToggle_->setIconSize(QSize(14, 14));
    folderPaneToggle_->setObjectName(QStringLiteral("folderPaneToggle"));
    folderPaneToggle_->setAccessibleName(QStringLiteral("Collapse the folder pane"));
    folderPaneToggle_->setToolTip(QStringLiteral("Collapse the folder pane"));
    folderPaneToggle_->setFixedSize(22, 22);
    // icons::apply re-tints on a theme change, so the chevron follows both themes.
    icons::apply(folderPaneToggle_, QStringLiteral("chevron-left"));
    folderPaneToggle_->setFocusPolicy(Qt::NoFocus);
    folderPaneToggle_->setCursor(Qt::PointingHandCursor);
    paneHeader->addWidget(folderPaneLabel_, 1);
    paneHeader->addWidget(folderPaneToggle_, 0, Qt::AlignRight);
    folderTree_ = new QTreeWidget(folderPane_);
    folderTree_->setObjectName(QStringLiteral("folderTree"));
    folderTree_->setHeaderHidden(true);
    folderTree_->setRootIsDecorated(true);
    folderTree_->setIndentation(14);
    folderTree_->setUniformRowHeights(true);
    folderTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    folderTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    folderTree_->setAcceptDrops(true);
    folderTree_->viewport()->setAcceptDrops(true);
    folderTree_->installEventFilter(this);
    folderTree_->viewport()->installEventFilter(this);
    paneLayout->addLayout(paneHeader);
    paneLayout->addWidget(folderTree_, 1);
    folderPane_->hide();
    connect(folderPaneToggle_, &QPushButton::clicked, this, [this] {
        setFolderPaneCollapsed(!folderPaneCollapsed_);
    });
    connect(folderTree_, &QTreeWidget::itemClicked, this,
        [this](QTreeWidgetItem* item, int) {
            if (item == nullptr) {
                return;
            }
            const auto id = item->data(0, Qt::UserRole).toLongLong();
            if (id != currentFolderId_) {
                openFolder(id);
            }
        });
    // The set of open folders is replayed onto the rebuilt tree after a refresh.
    connect(folderTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (item != nullptr) {
            expandedFolders_.insert(item->data(0, Qt::UserRole).toLongLong());
        }
    });
    connect(folderTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (item != nullptr) {
            expandedFolders_.erase(item->data(0, Qt::UserRole).toLongLong());
        }
    });
    // Hovering a closed folder mid-drag opens it, the way Explorer does.
    dragExpandTimer_ = new QTimer(this);
    dragExpandTimer_->setSingleShot(true);
    connect(dragExpandTimer_, &QTimer::timeout, this, [this] {
        const std::int64_t id = dragExpandFolder_;
        dragExpandFolder_ = 0;
        if (id == 0 || folderTree_ == nullptr) {
            return;
        }
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findItem =
            [&findItem, id](QTreeWidgetItem* node) -> QTreeWidgetItem* {
            for (int index = 0; index < node->childCount(); ++index) {
                auto* child = node->child(index);
                if (child->data(0, Qt::UserRole).toLongLong() == id) {
                    return child;
                }
                if (auto* hit = findItem(child)) {
                    return hit;
                }
            }
            return nullptr;
        };
        if (auto* item = findItem(folderTree_->invisibleRootItem())) {
            expandedFolders_.insert(id);
            item->setExpanded(true);
        }
    });

    libraryBottom_ = new FrostedBar(FrostedBar::Band::Bottom, page);
    auto* statusBar = libraryBottom_;
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 4, 12, 4);
    statusLayout->setSpacing(12);
    unlockedLocation_ = new QLabel(statusBar);
    unlockedLocation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unlockedLocation_->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    statusCountLabel_ = new QLabel(statusBar);
    statusCountLabel_->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    transfersButton_ = new QPushButton(QStringLiteral("Transfers"), statusBar);
    icons::withText(transfersButton_, QStringLiteral("arrow-up-down"));
    transfersButton_->setFlat(true);
    transfersButton_->setCursor(Qt::PointingHandCursor);
    transfersButton_->setStyleSheet(QStringLiteral(
        "QPushButton { color: #2672c8; background: transparent; font-weight: 600; }"
        "QPushButton:hover { color: #5aa6f0; }"));
    transfersButton_->hide();
    connect(transfersButton_, &QPushButton::clicked, this, [this] { showTransfers(); });
    progressBar_ = new QProgressBar(statusBar);
    progressBar_->setFixedWidth(160);
    progressBar_->setFixedHeight(8);
    progressBar_->setTextVisible(false);
    progressBar_->hide();
    galleryStatus_ = new QLabel(statusBar);
    galleryStatus_->setObjectName(QStringLiteral("statusNote"));
    galleryStatus_->hide();
    emptyState_ = new QLabel(page);
    emptyState_->setAlignment(Qt::AlignCenter);
    emptyState_->setWordWrap(true);
    emptyState_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyState_->setStyleSheet(QStringLiteral(
        "color: #8e8e93; font-size: 15px; background: transparent;"));
    emptyState_->hide();
    statusLayout->addWidget(unlockedLocation_);
    statusLayout->addStretch(1);
    statusLayout->addWidget(transfersButton_);
    statusLayout->addWidget(progressBar_);
    statusLayout->addWidget(statusCountLabel_);
    statusLayout->addWidget(galleryStatus_);

    connect(lockButton, &QPushButton::clicked, this, [this] { lockVault(); });
    connect(updateButton_, &QPushButton::clicked, this, [this] { beginUpdateInstall(); });
    connect(importButton_, &QPushButton::clicked, this, [this] {
        CommandPopup menu(this);
        menu.addCommand(QStringLiteral("Import files…"), 1);
        menu.addCommand(QStringLiteral("Import folder…"), 2);
        menu.addCommand(QStringLiteral("New note"), 3);
        menu.addCommand(QStringLiteral("New record"), 4);
        menu.addCommand(QStringLiteral("New from template…"), 5);
        menu.addCommand(QStringLiteral("New folder"), 6);
        menu.addCommand(QStringLiteral("Import package…"), 7);
        menu.addCommand(QStringLiteral("Export package…"), 8);
        const QPoint origin = importButton_->mapToGlobal(QPoint(0, importButton_->height()));
        if (menu.empty()) { return; }
        placePopup(&menu, origin);
        switch (menu.exec()) {
        case 1:
            beginImport();
            break;
        case 2:
            beginImportFolder();
            break;
        case 3:
            beginNewNote();
            break;
        case 4:
            beginNewRecord();
            break;
        case 5:
            beginNewRecordFromTemplate();
            break;
        case 6:
            beginNewFolder();
            break;
        case 7:
            beginImportPackage();
            break;
        case 8:
            beginExportPackage(0);
            break;
        default:
            break;
        }
    });
    connect(shareButton_, &QPushButton::clicked, this, [this] { openShare(); });
    connect(settingsButton_, &QPushButton::clicked, this, [this] { openSettings(); });
    connect(viewModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](const int index) { setViewMode(index); });
    connect(viewModeGroup_, QOverload<int>::of(&QButtonGroup::buttonClicked),
        this, [this](const int index) {
            if (viewModeCombo_->currentIndex() != index) {
                viewModeCombo_->setCurrentIndex(index);
            }
        });
    connect(sortButton_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        menu.setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        menu.setAttribute(Qt::WA_TranslucentBackground);
        const QStringList labels{
            QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Duration"),
            QStringLiteral("Resolution"), QStringLiteral("Codec"), QStringLiteral("Tags"),
            QStringLiteral("Imported")};
        for (int i = 0; i < labels.size(); ++i) {
            QString text = labels.at(i);
            if (i == sortKey_) {
                text += sortAscending_ ? QStringLiteral("  ↑") : QStringLiteral("  ↓");
            }
            QAction* action = menu.addAction(text);
            action->setData(i);
        }
        QAction* chosen = menu.exec(sortButton_->mapToGlobal(QPoint(0, sortButton_->height())));
        if (chosen != nullptr) {
            applyGallerySort(chosen->data().toInt());
        }
    });
    connect(tagFilterButton_, &TagFilterButton::selectionChanged,
        this, [this](const QStringList& tags) {
            tagFilterNames_ = tags;
            // Coalesce rapid checkmark changes just like gallery text search.
            searchTimer_->start();
        });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        searchText_ = text.trimmed();
        searchTimer_->start();
    });
    connect(detailsTree_, &QTreeWidget::itemDoubleClicked,
        this, [this](QTreeWidgetItem* item, int) {
            const int kind = item->data(0, Qt::UserRole + 12).toInt();
            const auto id = item->data(0, Qt::UserRole).toLongLong();
                        if (pluginFolder_) {
                handlePluginRow(kind, item->data(0, Qt::UserRole + 21).toInt());
                return;
            }
            if (kind == 1) {
                openFolder(-id);
            } else if (kind == 2) {
                openFolder(id);
            } else if (kind == kKindPhone) {
                openPeerBrowser();
            } else if (kind == kKindPluginSearch) {
                enterPluginFolder(searchText_);
            } else {
                beginPlayback(id);
            }
        });
    connect(iconList_, &QListWidget::itemDoubleClicked,
        this, [this](QListWidgetItem* item) {
            const int kind = item->data(Qt::UserRole + 12).toInt();
            const auto id = item->data(Qt::UserRole).toLongLong();
                        if (pluginFolder_) {
                handlePluginRow(kind, item->data(Qt::UserRole + 21).toInt());
                return;
            }
            if (kind == 1) {
                openFolder(-id);
            } else if (kind == 2) {
                openFolder(id);
            } else if (kind == kKindPhone) {
                openPeerBrowser();
            } else if (kind == kKindPluginSearch) {
                enterPluginFolder(searchText_);
            } else {
                beginPlayback(id);
            }
        });
    detailsTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    iconList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(detailsTree_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position) {
            showGalleryContextMenu(detailsTree_->viewport()->mapToGlobal(position));
        });
    connect(iconList_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position) {
            showGalleryContextMenu(iconList_->viewport()->mapToGlobal(position));
        });

    QSettings settings;
    sortKey_ = settings.value(QStringLiteral("gallery/sortKey"), 0).toInt();
    sortAscending_ = settings.value(QStringLiteral("gallery/sortAscending"), true).toBool();
    if (sortKey_ < 0 || sortKey_ > 6) {
        sortKey_ = 0;
    }
    updateSortButton();
    const int saved_view = settings.value(QStringLiteral("gallery/viewMode"), 1).toInt();
    viewModeCombo_->setCurrentIndex(saved_view);
    setViewMode(saved_view);
    layoutLibraryChrome();
    return page;
}

void MainWindow::layoutLibraryChrome() {
    QWidget* page = galleryStack_ != nullptr ? galleryStack_->parentWidget() : nullptr;
    if (page == nullptr || libraryTop_ == nullptr || libraryBottom_ == nullptr) {
        return;
    }
    // The bar's height is computed from a size *hint*, and a hint taken while one of its rows is
    // momentarily empty comes back short. Qt then clips every row in the bar rather than laying it
    // out smaller, so the title, the search field and the path collapse together, and the bar stays
    // that way until some later pass happens to re-lay it out -- which is why a second drag can put
    // it back. A drag rewrites the crumbs' stylesheets on every move, a rebuild empties the crumb
    // row, and either can catch the hint in that window.
    //
    // So the barrier is removed from the hint: the bar is never allowed below what its own layout
    // requires, measured after activating it. Rows that are hidden do not count, so the bar still
    // shrinks when a row legitimately goes away.
    int required = 0;
    if (auto* layout = libraryTop_->layout()) {
        layout->activate();
        required = layout->sizeHint().height();
        libraryTop_->setMinimumHeight(required);
    }
    const int topHeight = std::max({required, libraryTop_->sizeHint().height(), 44});
    const int bottomHeight = std::max(libraryBottom_->sizeHint().height(), 28);
    const bool details = galleryStack_->currentWidget() == detailsTree_;
    const bool paneVisible = folderPane_ != nullptr && folderPane_->isVisibleTo(page);
    const int paneWidth = paneVisible ? (folderPaneCollapsed_ ? 34 : 208) : 0;
    const int galleryWidth = std::max(0, page->width() - paneWidth);
    if (paneVisible) {
        // Keep the pane off the window border: flush at x=0 clipped the
        // "Folders" header and let its labels paint over the window edge.
        const int paneInset = folderPaneCollapsed_ ? 4 : 8;
        folderPane_->setGeometry(paneInset, topHeight,
            std::max(0, paneWidth - paneInset),
            std::max(0, page->height() - topHeight - bottomHeight));
    }
    if (details) {
        libraryTop_->setGeometry(0, 0, page->width(), topHeight);
        libraryBottom_->setGeometry(0, std::max(0, page->height() - bottomHeight), page->width(), bottomHeight);
        galleryStack_->setGeometry(
            paneWidth, topHeight, galleryWidth, std::max(0, page->height() - topHeight - bottomHeight));
    } else {
        galleryStack_->setGeometry(paneWidth, 0, galleryWidth, page->height());
        libraryTop_->setGeometry(0, 0, page->width(), topHeight);
        libraryBottom_->setGeometry(0, std::max(0, page->height() - bottomHeight), page->width(), bottomHeight);
        galleryStack_->lower();
    }
    libraryTop_->raise();
    libraryBottom_->raise();
    if (emptyState_ != nullptr && emptyState_->isVisible() && galleryStack_ != nullptr) {
        const QRect area = galleryStack_->geometry();
        emptyState_->setGeometry(area.adjusted(64, 36, -64, -36));
        emptyState_->raise();
    }
    // Keep the first and last rows fully visible. The bars float over the
    // gallery, so without this inset the top icons sit halfway underneath.
    if (iconList_ != nullptr) {
        static_cast<IconGrid*>(iconList_)->setChromeInset(
            details ? 0 : topHeight, details ? 0 : bottomHeight);
    }
    auto* area = qobject_cast<QAbstractScrollArea*>(galleryStack_->currentWidget());
    libraryTop_->follow(area);
    libraryBottom_->follow(area);
    // Row buttons sit over rows, so they follow every layout change.
    syncPluginRowButtons();
}

void MainWindow::setViewMode(const int index) {
    switch (index) {
    case 0: // Details
        galleryStack_->setCurrentWidget(detailsTree_);
        break;
    case 1: // Icons: three square columns, picture only.
        iconList_->setViewMode(QListView::IconMode);
        iconList_->setSpacing(0);
        iconList_->setWordWrap(false);
        galleryStack_->setCurrentWidget(iconList_);
        layoutInstagramGrid(iconList_);
        break;
    default: // List
        iconList_->setViewMode(QListView::ListMode);
        iconList_->setIconSize(QSize(44, 44));
        iconList_->setGridSize(QSize());
        iconList_->setSpacing(0);
        iconList_->setUniformItemSizes(true);
        galleryStack_->setCurrentWidget(iconList_);
        break;
    }
    if (index == 1) {
        iconList_->setUniformItemSizes(false);
    }
    if (sortButton_ != nullptr) {
        sortButton_->setVisible(index != 0);
    }
    if (viewModeGroup_ != nullptr) {
        if (QAbstractButton* button = viewModeGroup_->button(index)) {
            button->setChecked(true);
        }
    }
    // Large-icon cards carry a name + tags caption; rebuild the captions so
    // they match the mode that was just selected.
    for (int i = 0; i < iconList_->count(); ++i) {
        updateIconItemText(iconList_->item(i));
    }
    // Relayout from the top so icons never start half-clipped after a mode
    // switch (Qt IconMode keeps the old scroll offset and item layout).
    iconList_->scrollToTop();
    iconList_->doItemsLayout();
    layoutLibraryChrome();
    decorateVisible();
    if (isVisible() && galleryStack_ != nullptr) {
        fadeInWidget(galleryStack_->currentWidget());
    }
    QSettings settings;
    settings.setValue(QStringLiteral("gallery/viewMode"), index);
}

void MainWindow::updateSortButton() {
    if (sortButton_ == nullptr) {
        return;
    }
    const QStringList labels{
        QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Duration"),
        QStringLiteral("Resolution"), QStringLiteral("Codec"), QStringLiteral("Tags"),
        QStringLiteral("Imported")};
    const int key = std::clamp(sortKey_, 0, labels.size() - 1);
    sortButton_->setText(QStringLiteral("%1  %2")
        .arg(sortAscending_ ? QStringLiteral("↑") : QStringLiteral("↓"), labels.at(key)));
}

void MainWindow::applyGallerySort(const int key) {
    if (sortKey_ == key) {
        sortAscending_ = !sortAscending_;
    } else {
        sortKey_ = key;
        sortAscending_ = true;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("gallery/sortKey"), sortKey_);
    settings.setValue(QStringLiteral("gallery/sortAscending"), sortAscending_);
    updateSortButton();
    sortGalleryItems();
}

void MainWindow::showGallerySortIndicator() {
    if (detailsTree_ == nullptr) {
        return;
    }
    auto* header = detailsTree_->header();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    const int column = std::clamp(sortKey_, 0, std::max(0, detailsTree_->columnCount() - 1));
    header->blockSignals(true);
    header->setSortIndicator(column, sortAscending_ ? Qt::AscendingOrder : Qt::DescendingOrder);
    header->blockSignals(false);
}

void MainWindow::scheduleGallerySort() {
    if (gallerySortQueued_) {
        return;
    }
    gallerySortQueued_ = true;
    QTimer::singleShot(400, this, [this] {
        gallerySortQueued_ = false;
        sortGalleryItems();
    });
}

void MainWindow::sortGalleryItems() {
    if (iconList_ == nullptr || detailsTree_ == nullptr) {
        return;
    }
    if (sortingGallery_) {
        gallerySortAgain_ = true;
        return;
    }
    sortingGallery_ = true;
    const bool iconUpdates = iconList_->updatesEnabled();
    const bool treeUpdates = detailsTree_->updatesEnabled();
    iconList_->setUpdatesEnabled(false);
    detailsTree_->setUpdatesEnabled(false);
    detailsTree_->setSortingEnabled(false);

    {
        QSignalBlocker blockList(iconList_);
        QSignalBlocker blockListSelection(iconList_->selectionModel());
        QSignalBlocker blockTree(detailsTree_);
        QSignalBlocker blockTreeSelection(detailsTree_->selectionModel());

        QList<qlonglong> selected;
        for (auto* item : iconList_->selectedItems()) {
            if (item != nullptr) {
                selected.append(item->data(Qt::UserRole).toLongLong());
            }
        }
        const qlonglong currentId = iconList_->currentItem() == nullptr
            ? -1
            : iconList_->currentItem()->data(Qt::UserRole).toLongLong();
        QList<QListWidgetItem*> items;
        items.reserve(iconList_->count());
        for (int i = 0; i < iconList_->count(); ++i) {
            items.append(iconList_->item(i));
        }

        const int key = sortKey_;
        const bool ascending = sortAscending_;
        std::stable_sort(items.begin(), items.end(), [key, ascending](QListWidgetItem* left, QListWidgetItem* right) {
            if (left == nullptr || right == nullptr) {
                return left == nullptr && right != nullptr;
            }
            auto rank = [](QListWidgetItem* item) {
                const int kind = item->data(Qt::UserRole + 12).toInt();
                if (kind == 2 || kind == kKindPhone) {
                    return 0;
                }
                return kind == 1 ? 1 : 2;
            };
            const int leftRank = rank(left);
            const int rightRank = rank(right);
            if (leftRank != rightRank) {
                return leftRank < rightRank;
            }
            auto textCompare = [](const QString& a, const QString& b) {
                return a.compare(b, Qt::CaseInsensitive);
            };
            auto numberCompare = [](const qlonglong a, const qlonglong b) {
                return a < b ? -1 : (a > b ? 1 : 0);
            };
            int compared = 0;
            // Duration, resolution, and codec fill in as probes finish. A zero
            // or blank value is "not known yet" and stays after known rows, in
            // both directions, so rows do not leap through the list.
            bool placedUnknown = false;
            switch (key) {
            case 1:
                compared = numberCompare(
                    left->data(Qt::UserRole + 6).toLongLong(),
                    right->data(Qt::UserRole + 6).toLongLong());
                break;
            case 2: {
                const qlonglong ld = left->data(Qt::UserRole + 7).toLongLong();
                const qlonglong rd = right->data(Qt::UserRole + 7).toLongLong();
                if (ld <= 0 || rd <= 0) {
                    compared = (ld <= 0 && rd <= 0) ? 0 : (ld <= 0 ? 1 : -1);
                    placedUnknown = true;
                } else {
                    compared = numberCompare(ld, rd);
                }
                break;
            }
            case 3: {
                const qlonglong lp = left->data(Qt::UserRole + 8).toLongLong();
                const qlonglong rp = right->data(Qt::UserRole + 8).toLongLong();
                if (lp <= 0 || rp <= 0) {
                    compared = (lp <= 0 && rp <= 0) ? 0 : (lp <= 0 ? 1 : -1);
                    placedUnknown = true;
                } else {
                    compared = numberCompare(lp, rp);
                }
                break;
            }
            case 4: {
                const QString lc = left->data(Qt::UserRole + 11).toString();
                const QString rc = right->data(Qt::UserRole + 11).toString();
                if (lc.isEmpty() || rc.isEmpty()) {
                    compared = (lc.isEmpty() && rc.isEmpty()) ? 0 : (lc.isEmpty() ? 1 : -1);
                    placedUnknown = true;
                } else {
                    compared = textCompare(lc, rc);
                }
                break;
            }
            case 5:
                compared = textCompare(
                    left->data(Qt::UserRole + 2).toString(),
                    right->data(Qt::UserRole + 2).toString());
                break;
            case 6:
                compared = numberCompare(
                    left->data(Qt::UserRole + 9).toLongLong(),
                    right->data(Qt::UserRole + 9).toLongLong());
                break;
            default:
                compared = textCompare(
                    left->data(Qt::UserRole + 1).toString(),
                    right->data(Qt::UserRole + 1).toString());
                break;
            }
            if (compared == 0) {
                compared = numberCompare(
                    left->data(Qt::UserRole).toLongLong(),
                    right->data(Qt::UserRole).toLongLong());
            } else if (placedUnknown) {
                return compared < 0;
            }
            if (!ascending) {
                compared = -compared;
            }
            return compared < 0;
        });

        bool orderChanged = false;
        for (int i = 0; i < items.size(); ++i) {
            if (iconList_->item(i) != items.at(i)) {
                orderChanged = true;
                break;
            }
        }
        if (orderChanged) {
            iconList_->setCurrentItem(nullptr);
            detailsTree_->setCurrentItem(nullptr);
            while (iconList_->count() > 0) {
                iconList_->takeItem(iconList_->count() - 1);
            }

        struct RowKey {
            int kind{0};
            qlonglong id{0};
            bool operator==(const RowKey& other) const {
                return kind == other.kind && id == other.id;
            }
        };
        struct RowKeyHash {
            std::size_t operator()(const RowKey& key) const {
                return std::hash<qlonglong>{}(key.id) ^ (static_cast<std::size_t>(key.kind) << 1);
            }
        };
        std::unordered_map<RowKey, QTreeWidgetItem*, RowKeyHash> treeById;
        QList<QTreeWidgetItem*> treeExtras;
        treeById.reserve(static_cast<std::size_t>(std::max(0, detailsTree_->topLevelItemCount())));
        while (detailsTree_->topLevelItemCount() > 0) {
            auto* treeItem = detailsTree_->takeTopLevelItem(detailsTree_->topLevelItemCount() - 1);
            if (treeItem == nullptr) {
                continue;
            }
            const RowKey row{
                treeItem->data(0, Qt::UserRole + 12).toInt(),
                treeItem->data(0, Qt::UserRole).toLongLong()};
            const auto inserted = treeById.emplace(row, treeItem);
            if (!inserted.second) {
                treeExtras.append(treeItem);
            }
        }

        QListWidgetItem* currentItem = nullptr;
        QTreeWidgetItem* currentTree = nullptr;
        for (auto* item : items) {
            if (item == nullptr) {
                continue;
            }
            iconList_->addItem(item);
            const qlonglong id = item->data(Qt::UserRole).toLongLong();
            QTreeWidgetItem* treeItem = nullptr;
            const auto found = treeById.find(RowKey{item->data(Qt::UserRole + 12).toInt(), id});
            if (found != treeById.end()) {
                treeItem = found->second;
                treeById.erase(found);
                detailsTree_->addTopLevelItem(treeItem);
            }
            if (selected.contains(id)) {
                item->setSelected(true);
                if (treeItem != nullptr) {
                    treeItem->setSelected(true);
                }
            }
            if (id == currentId) {
                currentItem = item;
                currentTree = treeItem;
            }
        }
        for (auto& entry : treeById) {
            detailsTree_->addTopLevelItem(entry.second);
        }
        for (auto* extra : treeExtras) {
            detailsTree_->addTopLevelItem(extra);
        }
        if (currentItem != nullptr) {
            iconList_->setCurrentItem(currentItem);
        }
        if (currentTree != nullptr) {
            detailsTree_->setCurrentItem(currentTree);
        }
        }
    }

    iconList_->setUpdatesEnabled(iconUpdates);
    detailsTree_->setUpdatesEnabled(treeUpdates);
    showGallerySortIndicator();
    sortingGallery_ = false;
    if (gallerySortAgain_) {
        gallerySortAgain_ = false;
        sortGalleryItems();
    }
}

void MainWindow::refreshTagFilter() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto generation = ++tagGeneration_;
    const auto vault = vault_;
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<core::Result<std::vector<core::TagInfo>>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, generation, vault, watcher] {
        const auto outcome = *watcher->result();
        watcher->deleteLater();
        if (generation != tagGeneration_ || vault_ != vault) {
            return;
        }
        // A transient read failure must not silently discard the selection.
        if (!outcome) return;
        QVector<TagFilterOption> options;
        options.reserve(static_cast<int>(outcome.value().size()));
        for (const auto& tag : outcome.value()) {
            options.push_back({QString::fromStdString(tag.name),
                static_cast<quint64>(tag.video_count)});
        }
        tagFilterButton_->setTags(options);
    });
    watcher->setFuture(QtConcurrent::run([vault] {
        return std::make_shared<core::Result<std::vector<core::TagInfo>>>(
            vault->list_tags());
    }));
}

void MainWindow::beginEditTags(const std::vector<std::int64_t>& video_ids) {
    if (!vault_ || !vault_->is_unlocked() || video_ids.empty()) {
        return;
    }
    const auto vault = vault_;
    if (tagEditorWatcher_ != nullptr) {
        tagEditorWatcher_->waitForFinished();
    }
    tagEditorWatcher_ = new QFutureWatcher<std::shared_ptr<TagEditorData>>(this);
    connect(tagEditorWatcher_, &QFutureWatcherBase::finished, this,
        [this] { finishEditTags(); });
    tagEditorWatcher_->setFuture(QtConcurrent::run([vault, video_ids] {
        auto data = std::make_shared<TagEditorData>();
        data->video_ids = video_ids;
        auto all = vault->list_tags();
        if (all) {
            data->all_tags = all.value();
        }
        // A tag starts checked only when EVERY selected video carries it, so
        // the dialog applies one exact tag set to all of them: checking adds
        // the tag to every video, unchecking removes it from every video.
        // Tags held by only some videos are left alone unless the user
        // explicitly checks or unchecks them.
        QSet<QString> shared;
        std::vector<core::TagInfo> first_video_tags;
        bool first = true;
        for (const auto id : video_ids) {
            auto own = vault->tags_for_video(id);
            if (!own) {
                continue;
            }
            QSet<QString> names;
            for (const auto& tag : own.value()) {
                names.insert(QString::fromStdString(tag.name));
            }
            if (first) {
                shared = names;
                first_video_tags = own.value();
                first = false;
            } else {
                shared.intersect(names);
            }
        }
        for (const auto& tag : first_video_tags) {
            if (shared.contains(QString::fromStdString(tag.name))) {
                data->video_tags.push_back(tag);
            }
        }
        return data;
    }));
}

void MainWindow::finishEditTags() {
    auto* completed = tagEditorWatcher_;
    tagEditorWatcher_ = nullptr;
    const auto data = completed->result();
    completed->deleteLater();
    if (!data || !vault_) {
        return;
    }

    InWindowSheet dialog(this);
    dialog.setTitle(QStringLiteral("Edit tags"));
    auto* layout = dialog.body();

    if (data->video_ids.size() > 1U) {
        auto* hint = new QLabel(
            QStringLiteral("Applying to %1 selected items — checked tags are set on all of them.")
                .arg(data->video_ids.size()));
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }

    auto* list = new QListWidget(&dialog);
    QSet<QString> applied;
    for (const auto& tag : data->video_tags) {
        applied.insert(QString::fromStdString(tag.name));
    }
    for (const auto& tag : data->all_tags) {
        auto* item = new QListWidgetItem(QString::fromStdString(tag.name), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(
            applied.contains(QString::fromStdString(tag.name))
                ? Qt::Checked : Qt::Unchecked);
    }

    auto* inputRow = new QHBoxLayout();
    auto* input = new QLineEdit();
    input->setPlaceholderText(QStringLiteral("New tag…"));
    auto* addButton = new QPushButton(QStringLiteral("Add"));
    icons::withText(addButton, QStringLiteral("plus"));
    inputRow->addWidget(input, 1);
    inputRow->addWidget(addButton);

    list->setMinimumHeight(220);
    layout->addWidget(list, 1);
    layout->addLayout(inputRow);
    dialog.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    dialog.addButton(QStringLiteral("Save"), 1, true, QStringLiteral("check"));

    connect(addButton, &QPushButton::clicked, &dialog, [list, input] {
        const QString name = input->text().trimmed();
        if (name.isEmpty()) {
            return;
        }
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().compare(name, Qt::CaseInsensitive) == 0) {
                list->item(i)->setCheckState(Qt::Checked);
                list->setCurrentRow(i);
                input->clear();
                return;
            }
        }
        auto* item = new QListWidgetItem(name, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        list->setCurrentItem(item);
        input->clear();
    });
    if (dialog.run() != 1) {
        return;
    }

    QSet<QString> desired;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked) {
            desired.insert(list->item(i)->text());
        }
    }
    if (desired == applied) {
        return; // nothing changed
    }

    const auto vault = vault_;
    const auto video_ids = data->video_ids;
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] {
        auto* completed = adminWatcher_;
        adminWatcher_ = nullptr;
        const auto outcome = *completed->result();
        completed->deleteLater();
        importButton_->setEnabled(true);
        importFolderButton_->setEnabled(true);
        settingsButton_->setEnabled(true);
        if (!outcome) {
            setError(galleryStatus_,
                QString::fromUtf8(core::user_message(outcome.error().code).data()));
            galleryStatus_->setVisible(true);
            return;
        }
        refreshTagFilter();
        refreshGallery();
    });
    adminWatcher_->setFuture(QtConcurrent::run([vault, video_ids, applied, desired] {
        for (const auto& name : desired) {
            if (!applied.contains(name)) {
                for (const auto id : video_ids) {
                    (void)vault->add_tag(id, name.toUtf8().constData());
                }
            }
        }
        auto tags = vault->list_tags();
        if (tags) {
            for (const auto& tag : tags.value()) {
                const QString name = QString::fromStdString(tag.name);
                if (applied.contains(name) && !desired.contains(name)) {
                    for (const auto id : video_ids) {
                        (void)vault->remove_tag(id, tag.id);
                    }
                }
            }
        }
        return std::make_shared<core::Result<bool>>(true);
    }));
}

void MainWindow::showGalleryContextMenu(const QPoint& global_position) {
    qlonglong role = -1;
    int kind = 0;
    bool on_item = false;
    if (galleryStack_->currentWidget() == detailsTree_) {
        auto* item = detailsTree_->itemAt(
            detailsTree_->viewport()->mapFromGlobal(global_position));
        if (item != nullptr) {
            on_item = true;
            role = item->data(0, Qt::UserRole).toLongLong();
            kind = item->data(0, Qt::UserRole + 12).toInt();
            if (!item->isSelected()) {
                detailsTree_->clearSelection();
                item->setSelected(true);
                detailsTree_->setCurrentItem(item);
            }
        }
    } else {
        auto* item = iconList_->itemAt(
            iconList_->viewport()->mapFromGlobal(global_position));
        if (item != nullptr) {
            on_item = true;
            role = item->data(Qt::UserRole).toLongLong();
            kind = item->data(Qt::UserRole + 12).toInt();
            if (!item->isSelected()) {
                iconList_->clearSelection();
                item->setSelected(true);
                iconList_->setCurrentItem(item);
            }
        }
    }

    CommandPopup menu(this);
    // Provider rows are not vault items: rename/move/delete would act on a
    // synthetic id. Their own rows carry open and download instead.
    if (pluginFolder_ && (kind == kKindPluginBack || kind == kKindPluginAlbum
            || kind == kKindPluginEntry)) {
        return;
    }
    if (on_item && kind == kKindPhone) {
        menu.addCommand(QStringLiteral("Open"), 1);
        menu.addCommand(QStringLiteral("Disconnect phone"), 2, true);
        if (menu.empty()) { return; }
        if (menu.empty()) { return; }
    placePopup(&menu, global_position);
        switch (menu.exec()) {
        case 1:
            openPeerBrowser();
            break;
        case 2:
            if (shareServer_) {
                shareServer_->unlinkPeer();
            }
            break;
        default:
            break;
        }
        return;
    }
    if (!on_item || kind == 2) {
        menu.addCommand(QStringLiteral("New folder"), 1);
        menu.addCommand(QStringLiteral("New note"), 2);
        menu.addCommand(QStringLiteral("New record"), 3);
        menu.addCommand(QStringLiteral("New from template…"), 4);
        menu.addCommand(QStringLiteral("Import package…"), 5);
        if (!cutItems_.empty()) {
            menu.addCommand(QStringLiteral("Paste"), 6);
        }
        if (menu.empty()) { return; }
        if (menu.empty()) { return; }
    placePopup(&menu, global_position);
        switch (menu.exec()) {
        case 1:
            beginNewFolder();
            break;
        case 2:
            beginNewNote();
            break;
        case 3:
            beginNewRecord();
            break;
        case 4:
            beginNewRecordFromTemplate();
            break;
        case 5:
            beginImportPackage();
            break;
        case 6:
            pasteCutSelection();
            break;
        default:
            break;
        }
        return;
    }
    if (kind == 1) {
        const qlonglong folder_id = -role;
        menu.addCommand(QStringLiteral("Open"), 1);
        menu.addCommand(QStringLiteral("Rename"), 2);
        menu.addCommand(QStringLiteral("Cut"), 6);
        if (!cutItems_.empty()) {
            menu.addCommand(QStringLiteral("Paste"), 7);
        }
        menu.addCommand(QStringLiteral("Move to…"), 3);
        menu.addCommand(QStringLiteral("Export as package…"), 5);
        menu.addCommand(QStringLiteral("Remove folder"), 4, true);
        if (menu.empty()) { return; }
        if (menu.empty()) { return; }
    placePopup(&menu, global_position);
        switch (menu.exec()) {
        case 1:
            openFolder(folder_id);
            break;
        case 2:
            beginRenameFolder(folder_id);
            break;
        case 6:
            cutSelection();
            break;
        case 7: {
            // Paste into the folder that was right-clicked, as Explorer does.
            const auto rows = cutItems_;
            if (moveRowsToFolder(rows, folder_id, cutFolderId_)) {
                clearCut();
            }
            break;
        }
        case 3:
            beginMoveSelection(folder_id);
            break;
        case 4:
            beginRemoveFolder(folder_id);
            break;
        case 5:
            beginExportPackage(folder_id);
            break;
        default:
            break;
        }
        return;
    }

    const std::int64_t video_id = role;
    const int count = std::max(1, static_cast<int>(selectedVideoIds().size()));
    const QString countText = count > 1
        ? QStringLiteral(" %1 items").arg(count)
        : QString();

    menu.addCommand(QStringLiteral("Open"), 1);
    menu.addCommand(count > 1
        ? QStringLiteral("Edit tags")
        : QStringLiteral("Edit tags…"), 2);
    if (count == 1) {
        menu.addCommand(QStringLiteral("Rename"), 3);
    }
    menu.addCommand(count > 1
        ? QStringLiteral("Move") + countText + QStringLiteral("…")
        : QStringLiteral("Move to…"), 4);
    menu.addCommand(QStringLiteral("Cut"), 8);
    if (!cutItems_.empty()) {
        menu.addCommand(QStringLiteral("Paste"), 9);
    }
    menu.addCommand(count > 1
        ? QStringLiteral("Restore") + countText
        : QStringLiteral("Restore to folder…"), 5);
    menu.addCommand(QStringLiteral("Export as package…"), 7);
    menu.addCommand(count > 1
        ? QStringLiteral("Remove") + countText
        : QStringLiteral("Remove"), 6, true);
    if (menu.empty()) { return; }
    placePopup(&menu, global_position);
    switch (menu.exec()) {
    case 1:
        beginPlayback(video_id);
        break;
    case 2: {
        auto ids = selectedVideoIds();
        if (ids.empty()) {
            ids.push_back(video_id);
        }
        beginEditTags(ids);
        break;
    }
    case 3:
        beginRenameVideo(video_id);
        break;
    case 4:
        beginMoveSelection(0);
        break;
    case 8:
        cutSelection();
        break;
    case 9:
        pasteCutSelection();
        break;
    case 5:
        beginRestoreSelected();
        break;
    case 7:
        beginExportPackage(0);
        break;
    case 6:
        beginRemoveSelected();
        break;
    default:
        break;
    }
}

void MainWindow::selectSetupLocation() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose vault storage location"), setupLocation_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) {
        setupLocation_->setText(QDir::toNativeSeparators(selected));
    }
}

void MainWindow::selectLoginLocation() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose existing vault"), loginLocation_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) {
        loginLocation_->setText(QDir::toNativeSeparators(selected));
    }
}

void MainWindow::beginCreate() {
    setError(setupError_, {});
    const auto root = pathFromText(setupLocation_->text());
    if (root.empty()) {
        setError(setupError_, QStringLiteral("Choose a vault storage location."));
        return;
    }
    if (setupPassword_->text().isEmpty()) {
        setError(setupError_, QStringLiteral("Enter a password."));
        return;
    }
    if (setupPassword_->text() != setupConfirmation_->text()) {
        setError(setupError_, QStringLiteral("The password confirmation does not match."));
        return;
    }
    // The field is emptied here, before Argon2id runs, and the bytes live in a
    // locked buffer that wipes itself when the worker drops it.
    setupConfirmation_->clear();
    auto password = std::make_shared<core::Secret>();
    if (auto captured = takeSecretFromLineEdit(setupPassword_)) {
        *password = std::move(*captured);
    } else {
        setError(setupError_, QStringLiteral(
            "This password could not be held in protected memory. Try a shorter one."));
        return;
    }
    const auto parameters = selectedParameters(securityProfile_->currentIndex());
    setBusy(true);
    watcher_ = new QFutureWatcher<std::shared_ptr<VaultOperationResult>>(this);
    connect(watcher_, &QFutureWatcherBase::finished, this, [this] { finishOperation(); });
    watcher_->setFuture(QtConcurrent::run([root, password, parameters] {
        auto outcome = std::make_shared<VaultOperationResult>();
        auto result = core::Vault::create(root, *password, parameters);
        if (result) {
            outcome->keys = deriveSyncKeys(root, *password);
            outcome->succeeded = true;
            outcome->vault = std::make_unique<core::Vault>(std::move(result.value()));
        } else {
            outcome->error = result.error();
        }
        password->wipe();
        return outcome;
    }));
}

void MainWindow::beginCloudLogin() {
    loginPassword_->clear();
    setError(loginError_, QStringLiteral("Legacy remote vault-password login is disabled. Unlock a local vault, then use Sync setup."));
}

void MainWindow::beginOpen() {
    if (cloudLogin_) {
        beginCloudLogin();
        return;
    }
    setError(loginError_, {});
    auto root = pathFromText(loginLocation_->text());
    if (root.empty()) {
        setError(loginError_, QStringLiteral("Choose a vault storage location."));
        return;
    }
    if (loginPassword_->text().isEmpty()) {
        setError(loginError_, QStringLiteral("Enter the vault password."));
        return;
    }

    // A swap keeps the vault open behind the lock screen. Check the password
    // and come back to that same vault; opening a second one would drop the share.
    const bool resumeShare = vault_ && vault_->is_unlocked();
    if (resumeShare) {
        // Compare directory identity, not spelling: the Windows picker can
        // change case, separators or aliases without choosing a different vault.
        std::error_code error;
        if (!std::filesystem::equivalent(root, vault_->root_path(), error) || error) {
            setError(loginError_, QStringLiteral("Stop sharing before opening another vault."));
            return;
        }
        // Validate the retained vault, never a path alias that could be retargeted.
        root = vault_->root_path();
    }
    auto password = std::make_shared<core::Secret>();
    if (auto captured = takeSecretFromLineEdit(loginPassword_)) {
        *password = std::move(*captured);
    } else {
        setError(loginError_, QStringLiteral(
            "This password could not be held in protected memory. Try a shorter one."));
        return;
    }
    setBusy(true);
    watcher_ = new QFutureWatcher<std::shared_ptr<VaultOperationResult>>(this);
    connect(watcher_, &QFutureWatcherBase::finished, this, [this] { finishOperation(); });
    watcher_->setFuture(QtConcurrent::run([root, password, resumeShare] {
        auto outcome = std::make_shared<VaultOperationResult>();
        if (resumeShare) {
            auto checked = core::Vault::validate_password(root, *password);
            if (!checked) {
                outcome->error = checked.error();
            } else if (!checked.value()) {
                outcome->error = core::VaultError{core::VaultErrorCode::WrongPassword, {}};
            } else {
                outcome->keys = deriveSyncKeys(root, *password);
                outcome->succeeded = true;
                outcome->resumed = true;
            }
            password->wipe();
            return outcome;
        }
        auto result = core::Vault::open(root, *password);
        if (result) {
            outcome->keys = deriveSyncKeys(root, *password);
            outcome->succeeded = true;
            outcome->vault = std::make_unique<core::Vault>(std::move(result.value()));
        } else {
            outcome->error = result.error();
        }
        password->wipe();
        return outcome;
    }));
}

void MainWindow::finishOperation() {
    auto* completedWatcher = watcher_;
    watcher_ = nullptr;
    const auto outcome = completedWatcher->result();
    completedWatcher->deleteLater();
    setupPassword_->clear();
    setupConfirmation_->clear();
    loginPassword_->clear();
    setBusy(false);

    if (!outcome || !outcome->succeeded) {
        const auto error = outcome
            ? outcome->error
            : core::VaultError{core::VaultErrorCode::DatabaseFailure, "worker returned no result"};
        const QString message = QString::fromUtf8(core::user_message(error.code).data());
        setError(pages_->currentIndex() == kSetupPage ? setupError_ : loginError_, message);
        return;
    }

    if (!outcome->resumed) {
        vault_ = std::shared_ptr<core::Vault>(std::move(outcome->vault));
        if (shareServer_) {
            shareServer_->setLocalVault(vault_);
        }
    }
    // A fresh set of sync keys for this unlock; the old material is wiped.
    syncKeys_.wipe();
    syncKeys_ = outcome->keys;
    QSettings settings;
    settings.setValue(QStringLiteral("vault/location"), textFromPath(vault_->root_path()));
    showUnlocked();
}

void MainWindow::beginImport() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});
    const QStringList selected = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import files"), {},
        QStringLiteral("All files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    std::vector<std::filesystem::path> sources;
    sources.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& entry : selected) {
        sources.push_back(pathFromText(entry));
    }
    beginImportMany(std::move(sources), currentFolderId_);
}

void MainWindow::beginImportFolder() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    setError(galleryStatus_, {});
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Import a folder"), {},
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }
    beginImportTree({pathFromText(selected)}, currentFolderId_);
}

void MainWindow::beginImportTree(
    std::vector<std::filesystem::path> sources, const std::int64_t folder_id) {
    if (sources.empty()) {
        return;
    }
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        pendingExternal_.insert(pendingExternal_.end(), sources.begin(), sources.end());
        return;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Importing");
    setError(galleryStatus_, {});
    const auto vault = vault_;
    const auto shared_sources =
        std::make_shared<std::vector<std::filesystem::path>>(std::move(sources));
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Importing…"), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    const auto folderId = folder_id;
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, sources = std::move(shared_sources), folderId] {
            worker->importTree(sources, vault, folderId);
        },
        Qt::QueuedConnection);
}

void MainWindow::acceptExternalPaths(const QStringList& paths) {
    showNormal();
    raise();
    activateWindow();
    for (const QString& path : paths) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        // The shell file association hands .mvpk packages to this app; they are
        // imported as packages, not as files for the library.
        if (path.endsWith(QStringLiteral(".mvpk"), Qt::CaseInsensitive)) {
            pendingPackageExternal_.push_back(path);
            continue;
        }
        pendingExternal_.push_back(pathFromText(path));
    }
    if (pendingExternal_.empty() && pendingPackageExternal_.isEmpty()) {
        return;
    }
    if (!vault_ || !vault_->is_unlocked()) {
        if (loginError_ != nullptr) {
            setError(loginError_, QStringLiteral("Unlock the vault to import what you sent."));
        }
        return;
    }
    flushExternalImports();
}

void MainWindow::flushExternalImports() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    if (!pendingPackageExternal_.isEmpty()) {
        // A package import holds the batch slot, so media that arrived with it
        // waits for finishBatch() to call back into here.
        const QString package = pendingPackageExternal_.takeFirst();
        if (importPackagePath(package)) {
            return;
        }
        // Dismissing the password prompt drops the rest of the packages, but
        // still lets the media queue through.
        pendingPackageExternal_.clear();
    }
    if (pendingExternal_.empty()) {
        return;
    }
    auto paths = std::move(pendingExternal_);
    pendingExternal_.clear();
    beginImportTree(std::move(paths), currentFolderId_);
}

void MainWindow::beginImportMany(
    std::vector<std::filesystem::path> sources, const std::int64_t folder_id) {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Importing");
    setError(galleryStatus_, {});
    const auto vault = vault_;
    const auto shared_sources =
        std::make_shared<std::vector<std::filesystem::path>>(std::move(sources));
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Importing…"), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    // Run on the batch worker thread; progress/completion arrive as queued
    // signals so the UI stays fully responsive during the import.
    const auto folderId = folder_id;
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, sources = std::move(shared_sources), folderId] {
            worker->importFiles(sources, vault, folderId);
        },
        Qt::QueuedConnection);
}

void MainWindow::finishBatch(const bool ok, const QString& message, const int count) {
    Q_UNUSED(count)
    const bool wasPeer = peerCopyActive_;
    batchBusy_ = false;
    peerCopyActive_ = false;
    importButton_->setEnabled(true);
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
    if (wasPeer) {
        notePeerCopyFinished(ok, message);
        if (ok && message != QStringLiteral("Canceled.")) {
            showStatus(message, false);
        } else if (!ok && message != QStringLiteral("Canceled.")) {
            setError(galleryStatus_, message);
        }
        pumpPeerCopies();
        flushExternalImports();
        return;
    }
    progressBar_->hide();
    if (!ok) {
        setError(galleryStatus_, message);
        pumpPeerCopies();
        flushExternalImports();
        return;
    }
    showStatus(message, false);
    // Imports appear one file at a time. Rebuilding the library here would
    // throw away the scroll position and redraw every row that is already shown.
    if (batchLabel_ != QStringLiteral("Importing")
        && batchLabel_ != QStringLiteral("Exporting")) {
        refreshTagFilter();
        refreshGallery();
    }
    pumpPeerCopies();
    flushExternalImports();
}

void MainWindow::openSync() {
    if (!vault_ || !vault_->is_unlocked() || pages_->currentWidget() != unlockedPage_) return;
    if (!syncEngine_) {
        syncEngine_ = new SyncEngine(vault_, syncKeys_, {}, this);
        connect(syncEngine_, &SyncEngine::libraryChanged, this, [this] { refreshGallery(); refreshTagFilter(); });
        connect(syncEngine_, &SyncEngine::changed, this, [this] {
            if (syncEngine_ && syncEngine_->failed()) showStatus(syncEngine_->status(), true);
        });
    }
    if (!syncPage_) {
        // The sync surface is a page in the content stack, like the player and
        // the text reader: same window, same chrome, no second frame.
        syncPage_ = new SyncPage(syncEngine_, vault_, contentStack_);
        connect(syncPage_, &SyncPage::closed, this, [this] {
            if (contentStack_ != nullptr && syncPage_ != nullptr
                && contentStack_->currentWidget() == syncPage_) {
                fadeStack(contentStack_, pages_);
            }
        });
        connect(syncPage_, &QObject::destroyed, this, [this] { syncPage_ = nullptr; });
    }
    fadeStack(contentStack_, syncPage_);
}
void MainWindow::closeSync() {
    if (contentStack_ != nullptr && syncPage_ != nullptr
        && contentStack_->currentWidget() == syncPage_ && pages_ != nullptr) {
        contentStack_->setCurrentWidget(pages_);
    }
    if (syncPage_) { contentStack_->removeWidget(syncPage_); delete syncPage_; syncPage_ = nullptr; }
    if (syncEngine_) { syncEngine_->stop(); delete syncEngine_; syncEngine_ = nullptr; }
}

void MainWindow::openPluginMusic(const QString& query) {
    if (!vault_ || !vault_->is_unlocked()) return;
    bool enabled = false;
    for (const auto& manifest : pluginHost_->manifests()) enabled |= manifest.protocol=="monochrome-tracks-v1" && pluginHost_->enabled(manifest.id);
    if (!enabled) return;
    if (!pluginMusic_) {
        pluginMusic_ = new MusicPluginPage(pluginHost_, contentStack_);
        contentStack_->addWidget(pluginMusic_);
        connect(pluginMusic_, &MusicPluginPage::closed, this, [this] { contentStack_->setCurrentWidget(pages_); });
        connect(pluginMusic_, &MusicPluginPage::imported, this, [this] { refreshGallery(); });
    connect(pluginMusic_, &MusicPluginPage::catalogueFailed, this,
        [this](const QString& term, const QString& message) {
            if (!pluginFolder_ || term != pluginQuery_) {
                return;
            }
            if (pluginDeadline_ != nullptr) {
                pluginDeadline_->stop();
            }

            pluginCatalogue_ = QJsonObject();
            rebuildPluginFolder(QStringLiteral("Could not reach the provider · ") + message);
        });
    }
    pluginMusic_->setVault(vault_);
    contentStack_->setCurrentWidget(pluginMusic_);
    if (!query.isEmpty()) {
        pluginMusic_->searchFor(query);
    }
}

void MainWindow::ensurePluginPage() {
    if (pluginMusic_ != nullptr) {
        return;
    }
    pluginMusic_ = new MusicPluginPage(pluginHost_, contentStack_);
    contentStack_->addWidget(pluginMusic_);
    connect(pluginMusic_, &MusicPluginPage::closed, this, [this] {
        if (pluginFolder_) {
            leavePluginFolder();
        } else {
            contentStack_->setCurrentWidget(pages_);
        }
    });
    connect(pluginMusic_, &MusicPluginPage::imported, this, [this] { refreshGallery(); });
    connect(pluginMusic_, &MusicPluginPage::catalogueReady, this,
        [this](const QString& term, const QJsonObject& results) {
            if (!pluginFolder_ || term != pluginQuery_) {
                return;
            }
            if (pluginDeadline_ != nullptr) {
                pluginDeadline_->stop();
            }

            pluginCatalogue_ = results;
            rebuildPluginFolder();
        });
    // The album folder's track list. Opening it is a library folder open, not a
    // jump to the music page: the rows land in the same gallery.
    connect(pluginMusic_, &MusicPluginPage::albumTracksReady, this,
        [this](const QString& releaseId, const QJsonArray& tracks) {
            if (!pluginFolder_ || !pluginAlbumPending_ || releaseId != pluginAlbumId_) {
                return;
            }
            pluginAlbumPending_ = false;
            pluginAlbumTracks_ = tracks;
            if (pluginDeadline_ != nullptr) {
                pluginDeadline_->stop();
            }
            rebuildPluginFolder();
        });
    connect(pluginMusic_, &MusicPluginPage::albumTracksFailed, this,
        [this](const QString& releaseId, const QString& message) {
            if (!pluginFolder_ || !pluginAlbumPending_ || releaseId != pluginAlbumId_) {
                return;
            }
            pluginAlbumPending_ = false;
            pluginAlbumProblem_ = message;
            if (pluginDeadline_ != nullptr) {
                pluginDeadline_->stop();
            }
            rebuildPluginFolder();
        });
}

// A provider entry is a folder like any other, so the library's own chrome,
// rows and Back behaviour carry it: no separate browsing surface. The root is
// one mixed list - albums as folder rows, songs as music rows - and opening an
// album is just another folder inside the library.
void MainWindow::enterPluginFolder(const QString& term) {
    if (!vault_ || !vault_->is_unlocked() || iconList_ == nullptr) {
        return;
    }
    ensurePluginPage();
    pluginFolder_ = true;
    pluginQuery_ = term;
    pluginCatalogue_ = QJsonObject();
    pluginRows_.clear();
    pluginTrackItems_.clear();
    pluginAlbumId_.clear();
    pluginAlbumName_.clear();
    pluginAlbumTracks_ = QJsonArray();
    pluginAlbumProblem_.clear();
    pluginAlbumPending_ = false;
    // Rows, not tiles: this folder lists music the way the library lists it,
    // and every song row carries its own Download button.
    enterPluginListView();
    pluginMusic_->setVault(vault_);
    iconList_->clear();
    detailsTree_->clear();

    rebuildPluginFolder();
    armPluginDeadline(QStringLiteral("no answer within 12 s"));
    pluginMusic_->searchFor(term);
}

void MainWindow::leavePluginFolder() {
    if (pluginDeadline_ != nullptr) {
        pluginDeadline_->stop();
    }
    pluginFolder_ = false;
    pluginCatalogue_ = QJsonObject();
    pluginRows_.clear();
    pluginTrackItems_.clear();
    pluginAlbumId_.clear();
    pluginAlbumName_.clear();
    pluginAlbumTracks_ = QJsonArray();
    pluginAlbumProblem_.clear();
    pluginAlbumPending_ = false;
    syncPluginRowButtons();
    restorePluginViewMode();
    refreshGallery();
}

// The folder's own deadline: a provider that never answers must not leave the
// row at "Waiting" for the rest of the session. It covers the search and the
// album track list that replaces it once an album is opened.
void MainWindow::armPluginDeadline(const QString& message) {
    if (pluginDeadline_ == nullptr) {
        pluginDeadline_ = new QTimer(this);
        pluginDeadline_->setSingleShot(true);
    }
    pluginDeadline_->stop();
    pluginDeadline_->disconnect();
    connect(pluginDeadline_, &QTimer::timeout, this, [this, message] {
        if (!pluginFolder_) {
            return;
        }
        if (pluginAlbumPending_) {
            pluginAlbumPending_ = false;
            pluginAlbumProblem_ = message;
            rebuildPluginFolder();
        } else if (pluginCatalogue_.isEmpty()) {
            rebuildPluginFolder(message);
        }
    });
    pluginDeadline_->start(12000);
}

void MainWindow::openPluginAlbum(const int entry) {
    if (entry < 0 || entry >= static_cast<int>(pluginAlbumItems_.size())) {
        return;
    }
    const QJsonObject album = pluginAlbumItems_[static_cast<std::size_t>(entry)];
    const QString id = album.value(QStringLiteral("_id")).toString();
    if (id.isEmpty()) {
        return;
    }
    pluginAlbumId_ = id;
    pluginAlbumName_ = album.value(QStringLiteral("title")).toString();
    if (pluginAlbumName_.isEmpty()) {
        pluginAlbumName_ = QStringLiteral("this album");
    }
    pluginAlbumTracks_ = QJsonArray();
    pluginAlbumProblem_.clear();
    pluginAlbumPending_ = true;
    rebuildPluginFolder();
    ensurePluginPage();
    pluginMusic_->setVault(vault_);
    pluginMusic_->requestAlbumTracks(id);
    armPluginDeadline(QStringLiteral("no answer within 12 s"));
}

// One place for row activation, so double-click and Enter behave the same.
bool MainWindow::handlePluginRow(const int kind, const int entry) {
    if (!pluginFolder_) {
        return false;
    }
    if (kind == kKindPluginBack) {
        if (pluginAlbumId_.isEmpty()) {
            leavePluginFolder();
        } else {
            // Back from an album returns to the provider's mixed root list.
            pluginAlbumId_.clear();
            pluginAlbumName_.clear();
            pluginAlbumTracks_ = QJsonArray();
            pluginAlbumProblem_.clear();
            pluginAlbumPending_ = false;
            if (pluginDeadline_ != nullptr) {
                pluginDeadline_->stop();
            }
            rebuildPluginFolder();
        }
        return true;
    }
    if (kind == kKindPluginAlbum) {
        openPluginAlbum(entry);
        return true;
    }
    if (kind == kKindPluginEntry) {
        if (entry >= 0 && entry < static_cast<int>(pluginRows_.size())) {
            ensurePluginPage();
            pluginMusic_->activateItem(pluginRows_[static_cast<std::size_t>(entry)]);
        }
        return true;
    }
    return false;
}

// The provider folder is a list of rows even when the rest of the library is
// showing tiles. The saved view mode is restored on the way out.
void MainWindow::enterPluginListView() {
    if (galleryStack_ == nullptr || iconList_ == nullptr) {
        return;
    }
    if (!pluginViewModeSaved_) {
        pluginViewModeSaved_ = true;
        pluginSavedViewMode_ = viewModeCombo_ != nullptr ? viewModeCombo_->currentIndex() : 1;
    }
    if (viewModeCombo_ != nullptr) {
        QSignalBlocker blocker(viewModeCombo_);
        viewModeCombo_->setCurrentIndex(2);
        // The folder is a list of rows; a tile mode would hide the row buttons.
        viewModeCombo_->setEnabled(false);
    }
    if (viewModeGroup_ != nullptr) {
        if (QAbstractButton* button = viewModeGroup_->button(2)) {
            button->setChecked(true);
        }
        for (QAbstractButton* button : viewModeGroup_->buttons()) {
            button->setEnabled(false);
        }
    }
    iconList_->setViewMode(QListView::ListMode);
    iconList_->setIconSize(QSize(44, 44));
    iconList_->setGridSize(QSize());
    iconList_->setSpacing(0);
    iconList_->setUniformItemSizes(true);
    galleryStack_->setCurrentWidget(iconList_);
    layoutLibraryChrome();
}

void MainWindow::restorePluginViewMode() {
    if (!pluginViewModeSaved_) {
        return;
    }
    pluginViewModeSaved_ = false;
    if (viewModeCombo_ != nullptr) {
        viewModeCombo_->setEnabled(true);
    }
    if (viewModeGroup_ != nullptr) {
        for (QAbstractButton* button : viewModeGroup_->buttons()) {
            button->setEnabled(true);
        }
    }
    setViewMode(pluginSavedViewMode_);
}

// 1 ready, 2 busy (queued, downloading or encrypting), 3 saved, 4 failed.
int MainWindow::pluginRowState(const QString& trackId) const {
    if (pluginDownloads_ == nullptr) {
        return 1;
    }
    switch (pluginDownloads_->state(trackId)) {
    case PluginTrackDownloader::Saved:
        return 3;
    case PluginTrackDownloader::Failed:
        return 4;
    case PluginTrackDownloader::Queued:
    case PluginTrackDownloader::Fetching:
    case PluginTrackDownloader::Encrypting:
        return 2;
    default:
        return 1;
    }
}

void MainWindow::applyPluginRowState(const QString& trackId) {
    const int state = pluginRowState(trackId);
    bool changed = false;
    if (iconList_ != nullptr) {
        for (int i = 0; i < iconList_->count(); ++i) {
            QListWidgetItem* item = iconList_->item(i);
            if (item->data(kRowTrackIdRole).toString() != trackId) {
                continue;
            }
            if (item->data(kRowStateRole).toInt() != state) {
                item->setData(kRowStateRole, state);
                changed = true;
            }
        }
    }
    if (detailsTree_ != nullptr) {
        for (int i = 0; i < detailsTree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = detailsTree_->topLevelItem(i);
            if (item->data(0, kRowTrackIdRole).toString() != trackId) {
                continue;
            }
            if (item->data(0, kRowStateRole).toInt() != state) {
                item->setData(0, kRowStateRole, state);
                changed = true;
            }
        }
    }
    if (changed && iconList_ != nullptr) {
        iconList_->viewport()->update();
    }
    syncPluginRowButtons();
}

void MainWindow::ensurePluginDownloads() {
    if (pluginDownloads_ != nullptr) {
        return;
    }
    pluginDownloads_ = new PluginTrackDownloader(pluginHost_, this);
    connect(pluginDownloads_, &PluginTrackDownloader::stateChanged, this,
        [this](const QString& id, int, int percent) {
            pluginRowPercent_[id] = percent;
            applyPluginRowState(id);
        });
    connect(pluginDownloads_, &PluginTrackDownloader::finished, this,
        [this](const QString& id, const bool ok, const QString& message) {
            applyPluginRowState(id);
            if (ok) {
                // The vault changed; the library lists it again after Back.
                refreshGallery();
            } else if (!message.isEmpty()) {
                showStatus(QStringLiteral("Download failed: ") + message, true);
            }
        });
}

void MainWindow::startPluginDownload(const QString& trackId) {
    if (!pluginFolder_ || trackId.isEmpty() || !vault_ || !vault_->is_unlocked()
        || pluginHost_ == nullptr) {
        return;
    }
    const auto track = pluginTrackItems_.constFind(trackId);
    if (track == pluginTrackItems_.constEnd()) {
        return;
    }
    ensurePluginDownloads();
    pluginDownloads_->setVault(vault_);
    pluginDownloads_->enqueue(track.value());
    applyPluginRowState(trackId);
}

// The Download button is a widget over the row, so it follows the row's own
// rect. Rows that scrolled away -- or a view mode that has no rows -- release
// their button instead of leaving it floating over the wrong track.
void MainWindow::syncPluginRowButtons() {
    if (iconList_ == nullptr) {
        return;
    }
    struct Slot {
        QString trackId;
        QRect rect;
    };
    std::vector<Slot> rows;
    const bool listMode = pluginFolder_ && galleryStack_ != nullptr
        && galleryStack_->currentWidget() == iconList_
        && iconList_->viewMode() == QListView::ListMode;
    if (listMode) {
        const QRect area = iconList_->viewport()->rect();
        for (int i = 0; i < iconList_->count(); ++i) {
            QListWidgetItem* item = iconList_->item(i);
            const QString trackId = item->data(kRowTrackIdRole).toString();
            if (trackId.isEmpty()) {
                continue;
            }
            const QRect rect = iconList_->visualItemRect(item);
            if (!rect.isValid() || rect.height() < kRowButtonHeight || !rect.intersects(area)) {
                continue;
            }
            Slot slot;
            slot.trackId = trackId;
            slot.rect = rect;
            rows.push_back(slot);
        }
    }
    while (static_cast<int>(pluginRowButtons_.size()) < static_cast<int>(rows.size())) {
        auto* button = new QToolButton(iconList_->viewport());
        button->setObjectName(QStringLiteral("pluginRowDownload"));
        button->setCursor(Qt::PointingHandCursor);
        // Focus stays in the gallery, so Enter keeps activating the selected row.
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(kRowButtonWidth, kRowButtonHeight);
        button->hide();
        connect(button, &QToolButton::clicked, this, [this, button] {
            startPluginDownload(button->property("trackId").toString());
        });
        pluginRowButtons_.push_back(button);
    }
    const bool dark = isDarkTheme();
    for (int i = 0; i < static_cast<int>(pluginRowButtons_.size()); ++i) {
        QToolButton* button = pluginRowButtons_[static_cast<std::size_t>(i)];
        if (i >= static_cast<int>(rows.size())) {
            button->hide();
            continue;
        }
        const Slot& slot = rows[static_cast<std::size_t>(i)];
        const int state = pluginDownloads_ != nullptr
            ? pluginDownloads_->state(slot.trackId)
            : PluginTrackDownloader::Idle;
        const int percent = pluginRowPercent_.value(slot.trackId, -1);
        QString text;
        QString look;
        switch (state) {
        case PluginTrackDownloader::Saved:
            text = QStringLiteral("Saved");
            look = QStringLiteral("saved");
            break;
        case PluginTrackDownloader::Failed:
            text = QStringLiteral("Retry");
            look = QStringLiteral("retry");
            break;
        case PluginTrackDownloader::Fetching:
            text = percent >= 0 ? QStringLiteral("Downloading… %1%").arg(percent)
                                : QStringLiteral("Downloading…");
            look = QStringLiteral("busy");
            break;
        case PluginTrackDownloader::Queued:
            text = QStringLiteral("Queued");
            look = QStringLiteral("busy");
            break;
        case PluginTrackDownloader::Encrypting:
            text = QStringLiteral("Saving…");
            look = QStringLiteral("busy");
            break;
        default:
            text = QStringLiteral("Download");
            look = QStringLiteral("ready");
            break;
        }
        if (button->property("trackId").toString() != slot.trackId) {
            button->setProperty("trackId", slot.trackId);
        }
        if (button->text() != text) {
            button->setText(text);
        }
        button->setEnabled(look == QStringLiteral("ready") || look == QStringLiteral("retry"));
        if (button->property("rowLook").toString() != look) {
            button->setProperty("rowLook", look);
            button->setStyleSheet(pluginRowButtonStyle(look, dark));
        }
        // A theme switch changes only the colours: the look name stays the same,
        // so the stylesheet has to be keyed on the theme as well.
        if (button->property("rowDark").isValid() != true || button->property("rowDark").toBool() != dark) {
            button->setProperty("rowDark", dark);
            button->setStyleSheet(pluginRowButtonStyle(look, dark));
        }
        button->setGeometry(slot.rect.right() - kRowButtonReserve + 8,
            slot.rect.center().y() - kRowButtonHeight / 2, kRowButtonWidth, kRowButtonHeight);
        button->show();
        button->raise();
    }
}

void MainWindow::updatePluginFolderStatus(const int songs, const int albums) {
    if (statusCountLabel_ == nullptr) {
        return;
    }
    if (pluginAlbumId_.isEmpty()) {
        setFooterSummary(QStringLiteral("%1 items · %2 songs · %3 albums")
            .arg(songs + albums)
            .arg(songs)
            .arg(albums));
    } else {
        setFooterSummary(
            QStringLiteral("%1 tracks · %2").arg(songs).arg(pluginAlbumName_));
    }
    if (emptyState_ != nullptr) {
        if (songs + albums == 0) {
            emptyState_->setText(pluginAlbumId_.isEmpty()
                ? QStringLiteral("Nothing here for this search.")
                : QStringLiteral("This album listed no tracks."));
            emptyState_->show();
            layoutLibraryChrome();
        } else {
            emptyState_->hide();
        }
    }
}

void MainWindow::rebuildPluginFolder(const QString& problem) {
    if (!pluginFolder_ || iconList_ == nullptr || detailsTree_ == nullptr) {
        return;
    }
    iconList_->clear();
    detailsTree_->clear();
    pluginRows_.clear();
    pluginTrackItems_.clear();
    pluginAlbumItems_.clear();
    constexpr qlonglong kSynthetic = -910000;

    auto addRow = [&](const int kind, const qlonglong id, const QString& label,
                      const QString& note, const int entry,
                      const QPixmap& pixmap = QPixmap(folderPixmap())) {
        addLibraryEntry(iconList_, detailsTree_, id, kind, label, note, pixmap);
        QListWidgetItem* iconRow = iconList_->item(iconList_->count() - 1);
        QTreeWidgetItem* treeRow = detailsTree_->topLevelItem(detailsTree_->topLevelItemCount() - 1);
        if (iconRow != nullptr) {
            iconRow->setData(Qt::UserRole + 12, kind);
            iconRow->setData(Qt::UserRole + 21, entry);
        }
        if (treeRow != nullptr) {
            treeRow->setData(0, Qt::UserRole + 12, kind);
            treeRow->setData(0, Qt::UserRole + 21, entry);
        }
    };

    // A provider song is the library's normal music row: the title, then the
    // artist · duration line, with its Download button on the right.
    auto addTrackRow = [&](const QJsonObject& value) {
        QJsonObject item = value;
        QString id = item.value(QStringLiteral("trackId")).toString(
            item.value(QStringLiteral("_id")).toString());
        if (id.isEmpty()) {
            id = item.value(QStringLiteral("id")).toString();
        }
        if (id.isEmpty()) {
            return;
        }
        item.insert(QStringLiteral("_id"), id);
        item.insert(QStringLiteral("_kind"), 1);
        QString title = item.value(QStringLiteral("title")).toString();
        if (title.isEmpty()) {
            title = item.value(QStringLiteral("name")).toString();
        }
        if (title.isEmpty()) {
            title = QStringLiteral("Untitled");
        }
        const QString artists = pluginArtistNames(item);
        const QString duration = pluginDurationText(item);
        QString detail = artists;
        if (!duration.isEmpty()) {
            detail = artists.isEmpty() ? duration
                                       : artists + QStringLiteral(" · ") + duration;
        }
        if (detail.isEmpty()) {
            detail = QStringLiteral("Song");
        }
        pluginRows_.push_back(item);
        pluginTrackItems_.insert(id, item);
        const int state = pluginRowState(id);
        addRow(kKindPluginEntry, kSynthetic - 100 - static_cast<qlonglong>(pluginRows_.size()),
               title, detail, static_cast<int>(pluginRows_.size() - 1), musicPlaceholder(96));
        QListWidgetItem* iconRow = iconList_->item(iconList_->count() - 1);
        if (iconRow != nullptr) {
            iconRow->setData(kRowSubtitleRole, detail);
            iconRow->setData(kRowTrackIdRole, id);
            iconRow->setData(kRowStateRole, state);
            iconRow->setToolTip(detail.isEmpty() ? title : title + QStringLiteral("\n") + detail);
        }
        QTreeWidgetItem* treeRow = detailsTree_->topLevelItem(detailsTree_->topLevelItemCount() - 1);
        if (treeRow != nullptr) {
            treeRow->setText(2, duration);
            treeRow->setData(0, kRowSubtitleRole, detail);
            treeRow->setData(0, kRowTrackIdRole, id);
            treeRow->setData(0, kRowStateRole, state);
        }
    };

    QString provider = QStringLiteral("the provider");
    for (const auto& manifest : pluginHost_->manifests()) {
        if (manifest.protocol == "monochrome-tracks-v1" && pluginHost_->enabled(manifest.id)) {
            provider = manifest.name;
            break;
        }
    }

    const bool albumOpen = !pluginAlbumId_.isEmpty();
    addRow(kKindPluginBack, kSynthetic,
           albumOpen ? QStringLiteral("\u2039 Back to ") + provider
                     : QStringLiteral("\u2039 Back to library"),
           QStringLiteral("Folder"), -1);

    if (!albumOpen) {
        if (pluginCatalogue_.isEmpty()) {
            addRow(kKindPluginEntry, kSynthetic - 1,
                   problem.isEmpty()
                       ? QStringLiteral("Searching %1 for \u201C%2\u201D\u2026")
                             .arg(provider, pluginQuery_)
                       : QStringLiteral("Could not reach %1 · %2").arg(provider, problem),
                   QStringLiteral("Waiting"), -1);
            updatePluginFolderStatus(0, 0);
            iconList_->doItemsLayout();
            syncPluginRowButtons();
            return;
        }
        // Albums, then songs, in one list: an album is a folder inside the
        // provider, a song is a track that saves from its own row.
        const QJsonArray albums = pluginArray(pluginCatalogue_,
            {QStringLiteral("releases"), QStringLiteral("albums"), QStringLiteral("singles"),
                QStringLiteral("compilations")});
        for (const auto& value : albums) {
            QJsonObject item = value.toObject();
            QString id = item.value(QStringLiteral("releaseId")).toString(
                item.value(QStringLiteral("id")).toString());
            if (id.isEmpty()) {
                continue;
            }
            item.insert(QStringLiteral("_id"), id);
            item.insert(QStringLiteral("_kind"), 3);
            QString title = item.value(QStringLiteral("title")).toString();
            if (title.isEmpty()) {
                title = item.value(QStringLiteral("name")).toString();
            }
            if (title.isEmpty()) {
                title = QStringLiteral("Untitled album");
            }
            const QString artists = pluginArtistNames(item);
            pluginAlbumItems_.push_back(item);
            addRow(kKindPluginAlbum,
                   kSynthetic - 300 - static_cast<qlonglong>(pluginAlbumItems_.size()), title,
                   artists.isEmpty() ? QStringLiteral("Album")
                                     : artists + QStringLiteral(" · Album"),
                   static_cast<int>(pluginAlbumItems_.size() - 1));
        }
        const QJsonArray songs = pluginArray(pluginCatalogue_,
            {QStringLiteral("tracks"), QStringLiteral("topTracks")});
        for (const auto& value : songs) {
            addTrackRow(value.toObject());
        }
        updatePluginFolderStatus(static_cast<int>(pluginRows_.size()),
                                 static_cast<int>(pluginAlbumItems_.size()));
        iconList_->doItemsLayout();
        syncPluginRowButtons();
        return;
    }

    // An album folder: only its tracks, in the same music rows as the root.
    if (pluginAlbumPending_) {
        addRow(kKindPluginEntry, kSynthetic - 2,
               QStringLiteral("Loading %1\u2026").arg(pluginAlbumName_),
               QStringLiteral("Waiting"), -1);
    } else if (pluginAlbumTracks_.isEmpty()) {
        addRow(kKindPluginEntry, kSynthetic - 2,
               pluginAlbumProblem_.isEmpty()
                   ? QStringLiteral("%1 listed no tracks").arg(pluginAlbumName_)
                   : QStringLiteral("%1 · %2").arg(pluginAlbumName_, pluginAlbumProblem_),
               QStringLiteral("Album"), -1);
    } else {
        for (const auto& value : pluginAlbumTracks_) {
            addTrackRow(value.toObject());
        }
    }
    updatePluginFolderStatus(static_cast<int>(pluginRows_.size()), 0);
    iconList_->doItemsLayout();
    syncPluginRowButtons();
}

// Searching the library also asks the enabled provider whether it has
// anything under that name, as one extra entry above the results.
void MainWindow::offerPluginSearchRow() {
    if (searchText_.isEmpty() || iconList_ == nullptr || detailsTree_ == nullptr
        || pluginHost_ == nullptr || !vault_ || !vault_->is_unlocked()) {
        return;
    }
    QString name;
    for (const auto& manifest : pluginHost_->manifests()) {
        if (manifest.protocol == "monochrome-tracks-v1" && pluginHost_->enabled(manifest.id)) {
            name = manifest.name;
            break;
        }
    }
    if (name.isEmpty()) {
        return;
    }
    const QString label = QStringLiteral("Search %1 for “%2”").arg(name, searchText_);
    constexpr qlonglong kPluginSearchId = -900001;
    addLibraryEntry(iconList_, detailsTree_, kPluginSearchId, kKindPluginSearch, label,
                    QStringLiteral("Plugin"));
    if (iconList_->count() > 0) {
        iconList_->insertItem(0, iconList_->takeItem(iconList_->count() - 1));
        iconList_->item(0)->setData(Qt::UserRole + 12, kKindPluginSearch);
    }
    if (detailsTree_->topLevelItemCount() > 0) {
        detailsTree_->insertTopLevelItem(
            0, detailsTree_->takeTopLevelItem(detailsTree_->topLevelItemCount() - 1));
        detailsTree_->topLevelItem(0)->setData(0, Qt::UserRole + 12, kKindPluginSearch);
    }
}

void MainWindow::openExtensionTools(const QString& id) {
    if(!vault_ || !vault_->is_unlocked())return;
    bool enabled=false;
    for(const auto& m:pluginHost_->manifests())enabled |= m.protocol=="megavault-actions-v1" && pluginHost_->enabled(m.id);
    if(!enabled)return;
    if(!extensionTools_) {
        extensionTools_=new ExtensionToolsPage(pluginHost_,contentStack_);contentStack_->addWidget(extensionTools_);
        connect(extensionTools_,&ExtensionToolsPage::closed,this,[this]{contentStack_->setCurrentWidget(pages_);});
    }
    if(!id.isEmpty())extensionTools_->selectExtension(id);
    contentStack_->setCurrentWidget(extensionTools_);
}

void MainWindow::openSettings() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});
    if (!settingsPage_) {
        auto* page = new SettingsDialog(vault_, contentStack_, pluginHost_);
        settingsPage_ = page;
        contentStack_->addWidget(page);
        connect(page, &SettingsDialog::settingsChanged, this, [this] {
            refreshTagFilter();
            refreshGallery();
            resetAutoLock();
        });
        connect(page, &SettingsDialog::appearanceChanged, this, [this] {
            setThemeMode(savedThemeMode());
            update();
        });
        connect(page, &SettingsDialog::updateCheckRequested, this, [this] { beginUpdateCheck(true); });
        // A template is a record document, so the record editor is the editor for it.
        connect(page, &SettingsDialog::openTemplateRequested, this,
            [this](qlonglong id, const QString& name) {
                beginRecord(static_cast<std::int64_t>(id), name);
            });
        connect(page, &SettingsDialog::updatePreferencesChanged, this, [this] { configureUpdateChecks(); });
        connect(page, &SettingsDialog::passwordChanged, this, [this] {
            // The sync key comes from the vault password, so every engine and
            // key derived from the old one is now stale. Drop them; the next
            // unlock derives from the new password and the re-sealed state.
            if (syncPage_ != nullptr) {
                contentStack_->removeWidget(syncPage_);
                syncPage_->deleteLater();
                syncPage_ = nullptr;
            }
            if (syncEngine_ != nullptr) {
                syncEngine_->stop();
                syncEngine_->deleteLater();
                syncEngine_ = nullptr;
            }
            syncKeys_.wipe();
            setError(galleryStatus_, QStringLiteral(
                "Password changed. Sync is locked until you unlock the vault again; "
                "items already shared stay readable to your other devices."));
        });
        connect(page, &SettingsDialog::closed, this, [this, page] {
            if (settingsPage_ != page) return;
            settingsPage_ = nullptr;
            contentStack_->setCurrentWidget(pages_);
            contentStack_->removeWidget(page);
            page->deleteLater();
            restoreGalleryFocus();
            decorateVisible();
        });
        connect(page, &QObject::destroyed, this, [this, page] {
            if (settingsPage_ == page) settingsPage_ = nullptr;
        });
    }
    settingsPage_->setUpdateStatus(updateStatus_.isEmpty()
        ? QStringLiteral("Installed version %1").arg(installedVersion()) : updateStatus_,
        updateChecking_ || updateInstalling_);
    contentStack_->setCurrentWidget(settingsPage_);
    settingsPage_->setFocus();
}

void MainWindow::configureUpdateChecks() {
    const QSettings preferences;
    const bool automatic = preferences.value(QStringLiteral("updates/checkAutomatically"), true).toBool();
    const bool beta = preferences.value(QStringLiteral("updates/includeBeta"), false).toBool();
    if (beta != updateChannelBeta_ && !updateVersion_.isEmpty()) {
        updateVersion_.clear();
        updateUrl_.clear();
        updatePayload_.clear();
        updateButton_->hide();
    }
    if (automatic && appUpdateSupported()) {
        updateCheckTimer_->start();
        beginUpdateCheck();
    } else {
        updateCheckTimer_->stop();
    }
}

void MainWindow::beginUpdateCheck(const bool manual) {
    if (updateChecking_ || updateInstalling_ || updateButton_ == nullptr) return;
    if (!appUpdateSupported()) {
        updateStatus_ = QStringLiteral("In-app updates are not available on this platform.");
        if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
        return;
    }
    QSettings preferences;
    const bool beta = preferences.value(QStringLiteral("updates/includeBeta"), false).toBool();
    if (!manual) {
        if (!preferences.value(QStringLiteral("updates/checkAutomatically"), true).toBool()) return;
        const QDateTime last = preferences.value(QStringLiteral("updates/lastCheckUtc")).toDateTime();
        const qint64 elapsed = last.secsTo(QDateTime::currentDateTimeUtc());
        if (last.isValid() && elapsed >= 0 && elapsed < 6 * 60 * 60
            && preferences.value(QStringLiteral("updates/lastCheckBeta"), false).toBool() == beta) return;
    }
    preferences.setValue(QStringLiteral("updates/lastCheckUtc"), QDateTime::currentDateTimeUtc());
    preferences.setValue(QStringLiteral("updates/lastCheckBeta"), beta);
    updateChecking_ = true;
    updateChannelBeta_ = beta;
    updateStatus_ = QStringLiteral("Checking for updates…");
    if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_, true);
    auto* watcher = new QFutureWatcher<UpdateCheckOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, beta] {
        const UpdateCheckOutcome outcome = watcher->result();
        watcher->deleteLater();
        updateChecking_ = false;
        const QSettings preferences;
        if (beta != preferences.value(QStringLiteral("updates/includeBeta"), false).toBool()) {
            updateStatus_ = QStringLiteral("Update channel changed. Check again for the selected channel.");
            if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
            configureUpdateChecks();
            return;
        }
        if (!outcome.error.isEmpty()) {
            updateStatus_ = outcome.error;
            if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
            return;
        }
        if (outcome.version.isEmpty()) {
            updateStatus_ = QStringLiteral("You’re up to date · Version %1").arg(installedVersion());
            updateVersion_.clear();
            updateUrl_.clear();
            updatePayload_.clear();
            updateButton_->hide();
            if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
            return;
        }
        if (updateVersion_ != outcome.version) updatePayload_.clear();
        updateVersion_ = outcome.version;
        updateUrl_ = outcome.url;
        updateSize_ = outcome.size;
        updateStatus_ = QStringLiteral("Version %1 is available.").arg(updateVersion_);
        if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
        updateButton_->setToolTip(
            QStringLiteral("Download and install version %1").arg(updateVersion_));
        updateButton_->show();
        if (preferences.value(QStringLiteral("updates/checkAutomatically"), true).toBool()
            && preferences.value(QStringLiteral("updates/downloadAutomatically"), false).toBool()) {
            beginUpdateInstall(true);
        }
    });
    watcher->setFuture(QtConcurrent::run([beta] {
        UpdateCheckOutcome outcome;
        AppRelease release;
        if (!fetchLatestRelease(installedVersion(), &release, &outcome.error, beta)) {
            return outcome;
        }
        outcome.version = release.version;
        outcome.url = release.url.toString();
        outcome.size = release.size;
        return outcome;
    }));
}

void MainWindow::beginUpdateInstall(const bool automatic) {
    if (updateInstalling_ || updateVersion_.isEmpty()) {
        return;
    }
    if (!updatePayload_.isEmpty()) {
        if (!automatic) offerUpdateRestart();
        return;
    }
    updateInstalling_ = true;
    updateButton_->setEnabled(false);
    if (!batchBusy_) {
        progressBar_->setRange(0, 1000);
        progressBar_->setValue(0);
        progressBar_->show();
    }
    updateStatus_ = QStringLiteral("Downloading update…");
    if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_, true);
    showStatus(updateStatus_, false);

    AppRelease release;
    release.version = updateVersion_;
    release.url = QUrl(updateUrl_);
    release.size = updateSize_;
    // The worker reports progress with queued calls, which a guard keeps from
    // reaching a window that is already gone.
    const QPointer<MainWindow> guard(this);
    auto* watcher = new QFutureWatcher<UpdateInstallOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, release, automatic] {
        const UpdateInstallOutcome outcome = watcher->result();
        watcher->deleteLater();
        updateInstalling_ = false;
        if (!batchBusy_) progressBar_->hide();
        if (updateButton_ != nullptr) {
            updateButton_->setEnabled(true);
        }
        if (!outcome.error.isEmpty()) {
            updateStatus_ = outcome.error;
            if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
            showStatus(outcome.error, true);
            return;
        }
        if (updateVersion_ != release.version) {
            updateStatus_ = QStringLiteral("Update channel changed; the old download will not be installed.");
            if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
            return;
        }
        updatePayload_ = outcome.payload;
        updateStatus_ = QStringLiteral("Version %1 is ready. Use the update button to restart and install.").arg(release.version);
        if (settingsPage_) settingsPage_->setUpdateStatus(updateStatus_);
        showStatus(updateStatus_, false);
        // Background downloads never quit an open vault without consent.
        if (!automatic) offerUpdateRestart();
    });
    watcher->setFuture(QtConcurrent::run([release, guard] {
        UpdateInstallOutcome outcome;
        const QString folder = updateFolder(release.version);
        QDir().mkpath(folder);
        const QString package = folder + QLatin1Char('/') + QFileInfo(release.url.path()).fileName();
        const auto progress = [guard](const qint64 done, const qint64 total) {
            if (guard != nullptr) {
                QMetaObject::invokeMethod(guard, [guard, done, total] {
                    if (guard != nullptr) {
                        guard->setUpdateProgress(done, total);
                    }
                }, Qt::QueuedConnection);
            }
            return true;
        };
        if (!downloadRelease(release, package, progress, &outcome.error)) {
            return outcome;
        }
        stageRelease(package, &outcome.payload, &outcome.error);
        return outcome;
    }));
}

void MainWindow::setUpdateProgress(const qint64 done, const qint64 total) {
    if (progressBar_ == nullptr || batchBusy_) {
        return;
    }
    if (total > 0) {
        progressBar_->setRange(0, 1000);
        progressBar_->setValue(static_cast<int>((done * 1000) / total));
    } else {
        // GitHub reported no length: show the bar working instead of a number.
        progressBar_->setRange(0, 0);
    }
}

void MainWindow::offerUpdateRestart() {
    if (updatePayload_.isEmpty()) {
        return;
    }
    AppRelease release;
    release.version = updateVersion_;
    release.url = QUrl(updateUrl_);
    release.size = updateSize_;

    InWindowSheet confirm(this);
    confirm.setTitle(QStringLiteral("Update ready"));
    confirm.setMessage(QStringLiteral("Version %1 has been downloaded. "
        "MegaVaultProtect closes, replaces its files and starts again.").arg(release.version));
    confirm.addButton(QStringLiteral("Later"), 0, false);
    confirm.addButton(QStringLiteral("Restart and install"), 1, true, QStringLiteral("refresh-cw"));
    if (confirm.run() != 1) {
        showStatus(QStringLiteral("Version %1 is ready. Use the update button when you want to install.").arg(release.version), false);
        return;
    }
    if (batchBusy_) {
        showStatus(QStringLiteral("An import is still running. Try again when it finishes."), true);
        return;
    }
    QString error;
    if (!applyRelease(release, updatePayload_, &error)) {
        showStatus(error, true);
        return;
    }
    showStatus(QStringLiteral("Installing version %1…").arg(release.version), false);
    // The helper waits for this process to exit before it swaps the files.
    QTimer::singleShot(300, qApp, [] { QCoreApplication::quit(); });
}

void MainWindow::beginRestoreSelected() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Restoring");
    setError(galleryStatus_, {});
    const auto ids = selectedVideoIds();
    if (ids.empty()) {
        batchBusy_ = false;
        setError(galleryStatus_, QStringLiteral("Select one or more items to restore."));
        return;
    }
    const QString target = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Restore selected items"), {},
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (target.isEmpty()) {
        batchBusy_ = false;
        return;
    }
    const auto vault = vault_;
    const auto shared_ids = std::make_shared<std::vector<std::int64_t>>(ids);
    const auto shared_dir = std::make_shared<std::filesystem::path>(pathFromText(target));
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Restoring…"), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    // Run on the batch worker thread; progress/completion arrive as queued
    // signals so the UI stays fully responsive during the restore.
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, ids = std::move(shared_ids), dir = std::move(shared_dir)] {
            worker->restoreVideos(ids, dir, vault);
        },
        Qt::QueuedConnection);
}

namespace {

QString packageSegment(QString name) {
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\x00-\x1F\x7F])")),
        QStringLiteral("_"));
    name = name.trimmed();
    while (!name.isEmpty()
        && (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.')))) {
        name.chop(1);
    }
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        name = QStringLiteral("_");
    }
    const QString upper = name.toUpper();
    if (upper == QStringLiteral("CON") || upper == QStringLiteral("PRN")
        || upper == QStringLiteral("AUX") || upper == QStringLiteral("NUL")
        || QRegularExpression(QStringLiteral("^(COM|LPT)[1-9]$")).match(upper).hasMatch()) {
        name += QLatin1Char('_');
    }
    if (name.size() > 120) {
        name.truncate(120);
        while (!name.isEmpty()
            && (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.')))) {
            name.chop(1);
        }
        if (name.isEmpty()) {
            name = QStringLiteral("_");
        }
    }
    return name;
}

std::string claimPackagePath(std::unordered_set<std::string>& used, const QString& relative) {
    QString candidate = relative;
    const int slash = relative.lastIndexOf(QLatin1Char('/'));
    const int dot = relative.lastIndexOf(QLatin1Char('.'));
    const bool hasExtension = dot > slash;
    const QString stem = hasExtension ? relative.left(dot) : relative;
    const QString extension = hasExtension ? relative.mid(dot) : QString();
    for (int suffix = 2; suffix < 500; ++suffix) {
        const std::string key = candidate.toUtf8().toStdString();
        if (used.count(key) == 0U) {
            used.insert(key);
            return key;
        }
        candidate = stem + QStringLiteral(" (") + QString::number(suffix) + QLatin1Char(')') + extension;
    }
    const std::string fallback = candidate.toUtf8().toStdString();
    used.insert(fallback);
    return fallback;
}

struct PackagePassword {
    bool accepted{false};
    QString password;
    bool compress{true};
};

PackagePassword askPackagePassword(QWidget* parent, const bool exporting) {
    InWindowSheet dialog(parent);
    dialog.setTitle(exporting
        ? QStringLiteral("Export package")
        : QStringLiteral("Import package"));
    dialog.setMessage(exporting
        ? QStringLiteral("This password locks the package. It is separate from the vault password.")
        : QStringLiteral("Enter the password that was set when this package was created."));
    auto* password = new QLineEdit();
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(QStringLiteral("Package password"));
    dialog.body()->addWidget(password);
    QLineEdit* confirm = nullptr;
    QCheckBox* compress = nullptr;
    if (exporting) {
        confirm = new QLineEdit();
        confirm->setEchoMode(QLineEdit::Password);
        confirm->setPlaceholderText(QStringLiteral("Confirm password"));
        dialog.body()->addWidget(confirm);
        compress = new QCheckBox(QStringLiteral("Compress the files"));
        compress->setChecked(true);
        dialog.body()->addWidget(compress);
    }
    dialog.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    auto* ok = new QPushButton(exporting
        ? QStringLiteral("Export")
        : QStringLiteral("Import"));
    ok->setObjectName(QStringLiteral("inWindowPrimary"));
    ok->setCursor(Qt::PointingHandCursor);
    ok->setFocusPolicy(Qt::NoFocus);
    dialog.buttons()->addWidget(ok);
    QObject::connect(password, &QLineEdit::returnPressed, ok, &QPushButton::click);
    QObject::connect(ok, &QPushButton::clicked, &dialog, [&dialog, password, confirm] {
        if (password->text().isEmpty()) {
            dialog.setMessage(QStringLiteral("Enter a password."), true);
            return;
        }
        if (confirm != nullptr && password->text() != confirm->text()) {
            dialog.setMessage(QStringLiteral("The password confirmation does not match."), true);
            return;
        }
        dialog.finish(1);
    });
    password->setFocus();
    PackagePassword result;
    if (dialog.run() != 1) {
        return result;
    }
    result.accepted = true;
    result.password = password->text();
    result.compress = compress == nullptr || compress->isChecked();
    password->clear();
    if (confirm != nullptr) {
        confirm->clear();
    }
    return result;
}

} // namespace

void MainWindow::beginExportPackage(const std::int64_t only_folder_id) {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    auto listed_folders = vault_->list_folders();
    if (!listed_folders) {
        setError(galleryStatus_, vaultErrorText(listed_folders.error()));
        galleryStatus_->setVisible(true);
        return;
    }
    folders_ = std::move(listed_folders.value());
    auto listed_videos = vault_->list_videos();
    if (!listed_videos) {
        setError(galleryStatus_, vaultErrorText(listed_videos.error()));
        galleryStatus_->setVisible(true);
        return;
    }
    const auto& videos = listed_videos.value();

    struct Picked {
        int kind{0};
        qlonglong role{0};
    };
    std::vector<Picked> picked;
    if (only_folder_id <= 0) {
        const auto take = [&picked](const int kind, const qlonglong role) {
            if ((kind == 0 && role > 0) || (kind == 1 && role < 0)) {
                picked.push_back(Picked{kind, role});
            }
        };
        if (galleryStack_->currentWidget() == detailsTree_) {
            for (auto* item : detailsTree_->selectedItems()) {
                take(item->data(0, Qt::UserRole + 12).toInt(),
                    item->data(0, Qt::UserRole).toLongLong());
            }
        } else {
            for (auto* item : iconList_->selectedItems()) {
                take(item->data(Qt::UserRole + 12).toInt(),
                    item->data(Qt::UserRole).toLongLong());
            }
        }
    }

    const auto parentOf = [this](const std::int64_t id) {
        for (const auto& folder : folders_) {
            if (folder.id == id) {
                return folder.parent_id;
            }
        }
        return std::int64_t{0};
    };
    const auto inside = [&parentOf](const std::int64_t ancestor, std::int64_t node) {
        for (int guard = 0; guard < 64 && node != 0; ++guard) {
            const auto parent = parentOf(node);
            if (parent == ancestor) {
                return true;
            }
            if (parent == node) {
                return false;
            }
            node = parent;
        }
        return false;
    };

    auto files = std::make_shared<std::vector<core::Vault::PackageItem>>();
    auto directories = std::make_shared<std::vector<std::string>>();
    std::unordered_set<std::string> used;
    QString problem;
    QString suggestion = QStringLiteral("package");
    std::function<bool(std::int64_t, const QString&)> addFolder;
    addFolder = [&](const std::int64_t folderId, const QString& relative) -> bool {
        if (relative.toUtf8().size() > 1024) {
            problem = QStringLiteral("A folder path in this package is too long.");
            return false;
        }
        const auto dir = claimPackagePath(used, relative);
        directories->push_back(dir);
        for (const auto& folder : folders_) {
            if (folder.parent_id != folderId) {
                continue;
            }
            const QString child = QString::fromUtf8(dir.c_str())
                + QLatin1Char('/')
                + packageSegment(QString::fromStdString(folder.name));
            if (!addFolder(folder.id, child)) {
                return false;
            }
        }
        for (const auto& video : videos) {
            if (video.folder_id != folderId) {
                continue;
            }
            const QString filePath = QString::fromUtf8(dir.c_str())
                + QLatin1Char('/')
                + packageSegment(QString::fromStdString(video.display_name));
            if (filePath.toUtf8().size() > 1024) {
                problem = QStringLiteral("A file path in this package is too long.");
                return false;
            }
            core::Vault::PackageItem item;
            item.video_id = video.id;
            item.relative_path = claimPackagePath(used, filePath);
            files->push_back(std::move(item));
        }
        return true;
    };

    if (only_folder_id > 0) {
        QString folderName;
        for (const auto& folder : folders_) {
            if (folder.id == only_folder_id) {
                folderName = QString::fromStdString(folder.name);
                break;
            }
        }
        if (folderName.isEmpty()) {
            setError(galleryStatus_, QStringLiteral("That folder is no longer in the library."));
            galleryStatus_->setVisible(true);
            return;
        }
        suggestion = packageSegment(folderName);
        if (!addFolder(only_folder_id, suggestion)) {
            setError(galleryStatus_, problem);
            galleryStatus_->setVisible(true);
            return;
        }
    } else {
        std::vector<std::int64_t> chosenFolders;
        for (const auto& item : picked) {
            if (item.kind == 1) {
                chosenFolders.push_back(-item.role);
            }
        }
        bool named = false;
        for (const auto folderId : chosenFolders) {
            bool nested = false;
            for (const auto other : chosenFolders) {
                if (other != folderId && inside(other, folderId)) {
                    nested = true;
                    break;
                }
            }
            if (nested) {
                continue;
            }
            QString folderName;
            for (const auto& folder : folders_) {
                if (folder.id == folderId) {
                    folderName = QString::fromStdString(folder.name);
                    break;
                }
            }
            if (folderName.isEmpty()) {
                continue;
            }
            if (!named) {
                suggestion = packageSegment(folderName);
                named = true;
            }
            if (!addFolder(folderId, packageSegment(folderName))) {
                setError(galleryStatus_, problem);
                galleryStatus_->setVisible(true);
                return;
            }
        }
        for (const auto& item : picked) {
            if (item.kind != 0) {
                continue;
            }
            const core::VideoInfo* video = nullptr;
            for (const auto& row : videos) {
                if (row.id == item.role) {
                    video = &row;
                    break;
                }
            }
            if (video == nullptr) {
                continue;
            }
            const QString filePath = packageSegment(QString::fromStdString(video->display_name));
            if (!named) {
                suggestion = filePath;
                const int dot = suggestion.lastIndexOf(QLatin1Char('.'));
                if (dot > 0) {
                    suggestion.truncate(dot);
                }
                named = true;
            }
            core::Vault::PackageItem entry;
            entry.video_id = video->id;
            entry.relative_path = claimPackagePath(used, filePath);
            files->push_back(std::move(entry));
        }
    }

    if (!problem.isEmpty()) {
        setError(galleryStatus_, problem);
        galleryStatus_->setVisible(true);
        return;
    }
    if (files->empty() && directories->empty()) {
        setError(galleryStatus_, QStringLiteral("Select items or a folder to export."));
        galleryStatus_->setVisible(true);
        return;
    }

    PackagePassword prompt = askPackagePassword(this, true);
    if (!prompt.accepted) {
        return;
    }
    QString destination = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export package"),
        suggestion + QStringLiteral(".mvpk"),
        QStringLiteral("Mega Vault package (*.mvpk)"));
    if (destination.isEmpty()) {
        return;
    }
    if (!destination.endsWith(QStringLiteral(".mvpk"), Qt::CaseInsensitive)) {
        destination += QStringLiteral(".mvpk");
    }

    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Exporting");
    setError(galleryStatus_, {});
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Exporting package…"), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    const auto vault = vault_;
    const auto path = std::make_shared<std::filesystem::path>(pathFromText(destination));
    auto password = std::make_shared<core::Secret>();
    if (auto captured = secretFromQString(prompt.password)) {
        *password = std::move(*captured);
    }
    wipeString(prompt.password);
    if (password->empty()) {
        setError(galleryStatus_, QStringLiteral("The package password is not valid."));
        // Undo the busy state: the batch never started.
        batchBusy_ = false;
        importButton_->setEnabled(true);
        importFolderButton_->setEnabled(true);
        settingsButton_->setEnabled(true);
        progressBar_->hide();
        return;
    }
    const bool compress = prompt.compress;
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, path, password, files, directories, compress] {
            worker->exportPackage(vault, path, password, files, directories, compress);
        },
        Qt::QueuedConnection);
}

void MainWindow::beginImportPackage() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    const QString selected = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import package"), {},
        QStringLiteral("Mega Vault package (*.mvpk)"));
    if (selected.isEmpty()) {
        return;
    }
    importPackagePath(selected);
}

bool MainWindow::importPackagePath(const QString& selected) {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return false;
    }
    PackagePassword prompt = askPackagePassword(this, false);
    if (!prompt.accepted) {
        return false;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Importing");
    setError(galleryStatus_, {});
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Importing package…"), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    const auto vault = vault_;
    const auto path = std::make_shared<std::filesystem::path>(pathFromText(selected));
    auto password = std::make_shared<core::Secret>();
    if (auto captured = secretFromQString(prompt.password)) {
        *password = std::move(*captured);
    }
    wipeString(prompt.password);
    if (password->empty()) {
        setError(galleryStatus_, QStringLiteral("The package password is not valid."));
        // Undo the busy state: the batch never started.
        batchBusy_ = false;
        importButton_->setEnabled(true);
        importFolderButton_->setEnabled(true);
        settingsButton_->setEnabled(true);
        progressBar_->hide();
        return false;
    }
    const auto folderId = currentFolderId_;
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, path, password, folderId] {
            worker->importPackage(vault, path, password, folderId);
        },
        Qt::QueuedConnection);
    return true;
}

std::vector<std::int64_t> MainWindow::selectedVideoIds() const {
    std::vector<std::int64_t> ids;
    QList<QTreeWidgetItem*> tree_selection;
    QList<QListWidgetItem*> list_selection;
    if (galleryStack_->currentWidget() == detailsTree_) {
        tree_selection = detailsTree_->selectedItems();
    } else {
        list_selection = iconList_->selectedItems();
    }
    ids.reserve(static_cast<std::size_t>(
        tree_selection.size() + list_selection.size()));
    for (auto* item : tree_selection) {
        if (item->data(0, Qt::UserRole + 12).toInt() != 0) {
            continue;
        }
        const auto id = item->data(0, Qt::UserRole).toLongLong();
        if (id > 0) {
            ids.push_back(id);
        }
    }
    for (auto* item : list_selection) {
        if (item->data(Qt::UserRole + 12).toInt() != 0) {
            continue;
        }
        const auto id = item->data(Qt::UserRole).toLongLong();
        if (id > 0) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool MainWindow::galleryHasFocus() const {
    QWidget* focus = QApplication::focusWidget();
    for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
        if (qobject_cast<QLineEdit*>(widget) != nullptr
            || qobject_cast<QComboBox*>(widget) != nullptr
            || qobject_cast<QAbstractButton*>(widget) != nullptr) {
            return false;
        }
        if (widget == detailsTree_ || widget == iconList_) {
            return true;
        }
    }
    return false;
}

void MainWindow::highlightDropFolder(const std::int64_t folder_id, const bool on) {
    dropHighlight_ = on ? folder_id : -1;
    const QBrush mark(QColor(38, 114, 200, 110));
    if (detailsTree_ != nullptr) {
        for (int index = 0; index < detailsTree_->topLevelItemCount(); ++index) {
            auto* item = detailsTree_->topLevelItem(index);
            const int kind = item->data(0, Qt::UserRole + 12).toInt();
            const auto role = item->data(0, Qt::UserRole).toLongLong();
            const qlonglong id = kind == 1 ? -role : role;
            const bool hit = on && (kind == 1 || kind == 2) && id == folder_id;
            item->setBackground(0, hit ? mark : QBrush());
        }
    }
    if (iconList_ != nullptr) {
        for (int index = 0; index < iconList_->count(); ++index) {
            auto* item = iconList_->item(index);
            const int kind = item->data(Qt::UserRole + 12).toInt();
            const auto role = item->data(Qt::UserRole).toLongLong();
            const qlonglong id = kind == 1 ? -role : role;
            const bool hit = on && (kind == 1 || kind == 2) && id == folder_id;
            item->setBackground(hit ? mark : QBrush());
        }
    }
    if (folderTree_ != nullptr) {
        // The pane uses its own background for "current folder", so restore that
        // rather than clearing to default.
        std::function<void(QTreeWidgetItem*)> paint = [&](QTreeWidgetItem* node) {
            for (int index = 0; index < node->childCount(); ++index) {
                auto* item = node->child(index);
                const auto id = item->data(0, Qt::UserRole).toLongLong();
                const bool hit = on && id == folder_id;
                const bool current = id == currentFolderId_;
                item->setBackground(0, hit ? mark : (current ? QBrush(QColor(38, 114, 200, 70)) : QBrush()));
                paint(item);
            }
        };
        paint(folderTree_->invisibleRootItem());
    }
    if (crumbHost_ != nullptr) {
        const auto buttons = crumbHost_->findChildren<QPushButton*>();
        for (auto* button : buttons) {
            const bool hit = on && button->property("mvpDropFolder").isValid()
                && button->property("mvpDropFolder").toLongLong() == folder_id;
            if (button->property("dropTarget").toBool() == hit) {
                continue;
            }
            // Property plus repolish: the stylesheet is never replaced mid-drag, so the
            // crumb keeps the height the path bar's row was laid out with.
            button->setProperty("dropTarget", hit);
            button->style()->unpolish(button);
            button->style()->polish(button);
            button->update();
        }
    }
}

std::vector<MainWindow::GalleryRow> MainWindow::selectedRows() const {
    std::vector<GalleryRow> rows;
    const auto take = [&rows](const int kind, const qlonglong role) {
        if (kind == 0 && role > 0) {
            rows.emplace_back(0, role);
        } else if (kind == 1 && role < 0) {
            rows.emplace_back(1, role);
        }
    };
    if (galleryStack_ != nullptr && galleryStack_->currentWidget() == detailsTree_) {
        for (auto* item : detailsTree_->selectedItems()) {
            take(item->data(0, Qt::UserRole + 12).toInt(),
                item->data(0, Qt::UserRole).toLongLong());
        }
    } else if (iconList_ != nullptr) {
        for (auto* item : iconList_->selectedItems()) {
            take(item->data(Qt::UserRole + 12).toInt(),
                item->data(Qt::UserRole).toLongLong());
        }
    }
    return rows;
}

void MainWindow::captureDragPayload(const bool from_details) {
    dragPayload_.clear();
    const auto take = [this](const int kind, const qlonglong role) {
        if (kind == 0 && role > 0) {
            dragPayload_.emplace_back(0, role);
        } else if (kind == 1 && role < 0) {
            dragPayload_.emplace_back(1, role);
        }
    };
    if (from_details) {
        if (detailsTree_ != nullptr) {
            for (auto* item : detailsTree_->selectedItems()) {
                take(item->data(0, Qt::UserRole + 12).toInt(),
                    item->data(0, Qt::UserRole).toLongLong());
            }
        }
    } else if (iconList_ != nullptr) {
        for (auto* item : iconList_->selectedItems()) {
            take(item->data(Qt::UserRole + 12).toInt(),
                item->data(Qt::UserRole).toLongLong());
        }
    }
    dragPayloadValid_ = !dragPayload_.empty();
}

void MainWindow::clearDragPayload() {
    dragPayload_.clear();
    dragPayloadValid_ = false;
}

void MainWindow::moveItemsToFolder(const std::int64_t destination) {
    // What the drag carried, captured in startDrag. The selection can have
    // changed by the time the drop lands, so it is only a fallback. The rows
    // came from this view, which lists the open folder.
    std::vector<GalleryRow> rows = dragPayloadValid_ ? dragPayload_ : selectedRows();
    clearDragPayload();
    (void)moveRowsToFolder(rows, destination, currentFolderId_);
}

bool MainWindow::moveRowsToFolder(const std::vector<GalleryRow>& rows,
    const std::int64_t destination, const std::int64_t sourceFolder) {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_ || rows.empty()) {
        return false;
    }
    std::vector<std::int64_t> videos;
    std::vector<std::int64_t> folders;
    for (const auto& row : rows) {
        if (row.first == 0 && row.second > 0) {
            videos.push_back(row.second);
        } else if (row.first == 1 && row.second < 0) {
            folders.push_back(-row.second);
        }
    }
    if (videos.empty() && folders.empty()) {
        return false;
    }
    auto parentOf = [this](const std::int64_t folder_id, std::int64_t* parent) {
        for (const auto& folder : folders_) {
            if (folder.id == folder_id) {
                *parent = folder.parent_id;
                return true;
            }
        }
        return false;
    };
    auto contains = [this](const std::int64_t ancestor, std::int64_t node) {
        for (int guard = 0; guard < 64 && node != 0; ++guard) {
            if (node == ancestor) {
                return true;
            }
            std::int64_t parent = 0;
            bool found = false;
            for (const auto& folder : folders_) {
                if (folder.id == node) {
                    parent = folder.parent_id;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
            node = parent;
        }
        return false;
    };
    // A file lives in exactly one folder, so a drop back onto the folder it is
    // already in is a no-op rather than an error. The rows came from
    // sourceFolder, so the move only has work to do when that differs.
    const bool videosStay = destination == sourceFolder;
    // Check every folder before touching the vault: one impossible target must
    // not leave a half-applied batch.
    QString problem;
    for (const auto folder_id : folders) {
        if (folder_id == destination || contains(folder_id, destination)) {
            problem = QStringLiteral("A folder can't be moved into itself.");
            break;
        }
    }
    if (!problem.isEmpty()) {
        setError(galleryStatus_, problem);
        galleryStatus_->setVisible(true);
        return false;
    }
    std::vector<GalleryRow> moved;
    moved.reserve(rows.size());
    for (const auto folder_id : folders) {
        std::int64_t parent = -1;
        if (parentOf(folder_id, &parent) && parent == destination) {
            continue;
        }
        auto result = vault_->move_folder(folder_id, destination);
        if (!result) {
            problem = vaultErrorText(result.error());
            continue;
        }
        moved.emplace_back(1, -folder_id);
    }
    for (const auto video_id : videos) {
        if (videosStay) {
            continue;
        }
        auto result = vault_->move_video(video_id, destination);
        if (!result) {
            problem = vaultErrorText(result.error());
            continue;
        }
        moved.emplace_back(0, video_id);
    }
    if (!problem.isEmpty()) {
        setError(galleryStatus_, problem);
        galleryStatus_->setVisible(true);
    } else {
        setError(galleryStatus_, {});
    }
    // Explorer leaves the moved rows selected when the destination is the folder
    // that is on screen; the paint pass re-selects them once the rows exist.
    pendingSelection_ = destination == currentFolderId_ ? moved : std::vector<GalleryRow>{};
    // A cut whose rows just moved by drag or paste has been spent.
    if (problem.isEmpty() && !cutItems_.empty()) {
        for (const auto& row : moved) {
            const bool wasCut = std::any_of(cutItems_.begin(), cutItems_.end(),
                [&row](const GalleryRow& cut) {
                    return cut.first == row.first && cut.second == row.second;
                });
            if (wasCut) {
                clearCut();
                break;
            }
        }
    }
    refreshGallery();
    return problem.isEmpty();
}

void MainWindow::selectGalleryRow(const int kind, const qlonglong role) {
    if (galleryStack_ == nullptr) {
        return;
    }
    if (galleryStack_->currentWidget() == detailsTree_ && detailsTree_ != nullptr) {
        for (int index = 0; index < detailsTree_->topLevelItemCount(); ++index) {
            auto* item = detailsTree_->topLevelItem(index);
            if (item->data(0, Qt::UserRole + 12).toInt() == kind
                && item->data(0, Qt::UserRole).toLongLong() == role) {
                item->setSelected(true);
                return;
            }
        }
        return;
    }
    if (iconList_ == nullptr) {
        return;
    }
    for (int index = 0; index < iconList_->count(); ++index) {
        auto* item = iconList_->item(index);
        if (item->data(Qt::UserRole + 12).toInt() == kind
            && item->data(Qt::UserRole).toLongLong() == role) {
            item->setSelected(true);
            return;
        }
    }
}

void MainWindow::applyPendingSelection() {
    if (galleryStack_ == nullptr) {
        pendingSelection_.clear();
        return;
    }
    if (pendingSelection_.empty()) {
        return;
    }
    const auto rows = pendingSelection_;
    pendingSelection_.clear();
    QAbstractItemView* view = galleryStack_->currentWidget() == detailsTree_
        ? static_cast<QAbstractItemView*>(detailsTree_)
        : static_cast<QAbstractItemView*>(iconList_);
    if (view == nullptr) {
        return;
    }
    view->clearSelection();
    for (const auto& row : rows) {
        selectGalleryRow(row.first, row.second);
    }
    const auto selected = view->selectionModel()->selectedIndexes();
    if (!selected.isEmpty()) {
        view->setCurrentIndex(selected.first());
        view->scrollTo(selected.first());
    }
}

void MainWindow::cutSelection() {
    const auto rows = selectedRows();
    if (rows.empty()) {
        showStatus(QStringLiteral("Select files or folders, then press Ctrl+X."), false);
        return;
    }
    cutItems_ = rows;
    cutFolderId_ = currentFolderId_;
    applyCutDimming();
    showStatus(QStringLiteral("Cut %1 item%2. Ctrl+V moves here, Escape cancels.")
        .arg(rows.size())
        .arg(rows.size() == 1 ? QString() : QStringLiteral("s")), false);
}

void MainWindow::pasteCutSelection() {
    if (cutItems_.empty()) {
        showStatus(QStringLiteral("Nothing is cut. Select items and press Ctrl+X first."), false);
        return;
    }
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    const auto rows = cutItems_;
    if (moveRowsToFolder(rows, currentFolderId_, cutFolderId_)) {
        clearCut();
    }
}

void MainWindow::clearCut() {
    cutItems_.clear();
    cutFolderId_ = 0;
    applyCutDimming();
}

void MainWindow::applyCutDimming() {
    const auto isCut = [this](const int kind, const qlonglong role) {
        for (const auto& row : cutItems_) {
            if (row.first == kind && row.second == role) {
                return true;
            }
        }
        return false;
    };
    const QBrush dim(QColor(142, 142, 147));
    if (detailsTree_ != nullptr) {
        for (int index = 0; index < detailsTree_->topLevelItemCount(); ++index) {
            auto* item = detailsTree_->topLevelItem(index);
            const bool cut = isCut(item->data(0, Qt::UserRole + 12).toInt(),
                item->data(0, Qt::UserRole).toLongLong());
            for (int column = 0; column < detailsTree_->columnCount(); ++column) {
                item->setForeground(column, cut ? dim : QBrush());
            }
        }
    }
    if (iconList_ != nullptr) {
        for (int index = 0; index < iconList_->count(); ++index) {
            auto* item = iconList_->item(index);
            const bool cut = isCut(item->data(Qt::UserRole + 12).toInt(),
                item->data(Qt::UserRole).toLongLong());
            item->setData(kCutDimRole, cut);
        }
    }
}

void MainWindow::goUpFolder() {
    if (currentFolderId_ == 0) {
        return;
    }
    std::int64_t parent = 0;
    for (const auto& folder : folders_) {
        if (folder.id == currentFolderId_) {
            parent = folder.parent_id;
            break;
        }
    }
    openFolder(parent);
}

void MainWindow::rebuildFolderTree() {
    if (folderTree_ == nullptr || folderPane_ == nullptr) {
        return;
    }
    const bool unlocked = vault_ && vault_->is_unlocked();
    folderTree_->blockSignals(true);
    folderTree_->clear();
    folderPane_->setVisible(unlocked);
    if (!unlocked) {
        folderTree_->blockSignals(false);
        layoutLibraryChrome();
        return;
    }
    // The path down to the open folder always stays expanded, as in Explorer.
    expandedFolders_.insert(0);
    for (std::int64_t id = currentFolderId_; id != 0; ) {
        expandedFolders_.insert(id);
        std::int64_t parent = 0;
        bool found = false;
        for (const auto& folder : folders_) {
            if (folder.id == id) {
                parent = folder.parent_id;
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        id = parent;
    }
    auto* root = new QTreeWidgetItem(folderTree_);
    root->setText(0, QStringLiteral("Vault"));
    root->setData(0, Qt::UserRole, static_cast<qlonglong>(0));
    root->setToolTip(0, QStringLiteral("Vault"));
    std::function<void(QTreeWidgetItem*, std::int64_t)> fill =
        [this, &fill](QTreeWidgetItem* parentItem, const std::int64_t parentId) {
        std::vector<const core::FolderInfo*> children;
        for (const auto& folder : folders_) {
            if (folder.parent_id == parentId) {
                children.push_back(&folder);
            }
        }
        std::sort(children.begin(), children.end(),
            [](const core::FolderInfo* left, const core::FolderInfo* right) {
                return QString::fromStdString(left->name)
                    .compare(QString::fromStdString(right->name), Qt::CaseInsensitive) < 0;
            });
        for (const auto* folder : children) {
            auto* item = new QTreeWidgetItem(parentItem);
            const QString name = QString::fromStdString(folder->name);
            item->setText(0, name);
            item->setToolTip(0, name);
            item->setData(0, Qt::UserRole, static_cast<qlonglong>(folder->id));
            fill(item, folder->id);
            item->setExpanded(expandedFolders_.count(folder->id) != 0);
        }
    };
    fill(root, 0);
    root->setExpanded(expandedFolders_.count(0) != 0);
    QTreeWidgetItem* open = root;
    if (currentFolderId_ != 0) {
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find =
            [this, &find](QTreeWidgetItem* node) -> QTreeWidgetItem* {
            for (int index = 0; index < node->childCount(); ++index) {
                auto* child = node->child(index);
                if (child->data(0, Qt::UserRole).toLongLong() == currentFolderId_) {
                    return child;
                }
                if (auto* hit = find(child)) {
                    return hit;
                }
            }
            return nullptr;
        };
        if (auto* hit = find(root)) {
            open = hit;
        }
    }
    const QBrush mark(QColor(38, 114, 200, 70));
    for (auto* item : {root, open}) {
        if (item != nullptr && item->data(0, Qt::UserRole).toLongLong() == currentFolderId_) {
            item->setBackground(0, mark);
        }
    }
    if (open != nullptr) {
        QFont font = open->font(0);
        font.setBold(true);
        open->setFont(0, font);
        folderTree_->scrollToItem(open, QAbstractItemView::PositionAtCenter);
    }
    folderTree_->blockSignals(false);
    layoutLibraryChrome();
}

void MainWindow::setFolderPaneCollapsed(const bool collapsed) {
    folderPaneCollapsed_ = collapsed;
    if (folderTree_ != nullptr) {
        folderTree_->setVisible(!collapsed);
    }
    if (folderPaneLabel_ != nullptr) {
        folderPaneLabel_->setVisible(!collapsed);
    }
    if (folderPaneToggle_ != nullptr) {
        icons::apply(folderPaneToggle_,
            collapsed ? QStringLiteral("chevron-right") : QStringLiteral("chevron-left"));
        folderPaneToggle_->setToolTip(collapsed
            ? QStringLiteral("Show the folder pane")
            : QStringLiteral("Collapse the folder pane"));
    }
    layoutLibraryChrome();
}

bool MainWindow::handleExplorerDrag(QObject* watched, QEvent* event) {
    const bool on_details = detailsTree_ != nullptr
        && (watched == detailsTree_ || watched == detailsTree_->viewport());
    const bool on_icons = iconList_ != nullptr
        && (watched == iconList_ || watched == iconList_->viewport());
    auto* crumb = qobject_cast<QPushButton*>(watched);
    const bool on_crumb = crumb != nullptr && crumb->property("mvpDropFolder").isValid();
    const bool on_tree = folderTree_ != nullptr
        && (watched == folderTree_ || watched == folderTree_->viewport());
    if (!on_details && !on_icons && !on_crumb && !on_tree) {
        return false;
    }
    if (event->type() == QEvent::DragLeave) {
        if (dragExpandTimer_ != nullptr) {
            dragExpandTimer_->stop();
            dragExpandFolder_ = 0;
        }
        highlightDropFolder(0, false);
        return false;
    }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove
        && event->type() != QEvent::Drop) {
        return false;
    }
    auto* drop = static_cast<QDropEvent*>(event);
    const bool internal = drop->source() == detailsTree_ || drop->source() == iconList_;
    const bool external = drop->mimeData()->hasUrls();
    if (!internal && !external) {
        return false;
    }

    const auto rejectPhoneDrop = [this, event, drop] {
        highlightDropFolder(0, false);
        drop->ignore();
        if (event->type() == QEvent::Drop) {
            setError(galleryStatus_, QStringLiteral(
                "The phone's library is read-only. Open it to copy from it."));
            galleryStatus_->setVisible(true);
        }
        return true;
    };

    std::int64_t destination = currentFolderId_;
    bool on_folder = false;
    QTreeWidgetItem* treeTarget = nullptr;
    // Drop events normally reach the viewport, where drop->pos() is already
    // viewport-relative. Mapping from the view again shifted the hit test by the
    // header/frame height, so a tree with a header only hit rows near the top:
    // drops worked in the icon views and stopped working after a view switch.
    // The cursor's own position is correct no matter which widget got the event.
    const auto cursorIn = [](const QWidget* viewport) {
        return viewport->mapFromGlobal(QCursor::pos());
    };
    if (on_tree) {
        treeTarget = folderTree_->itemAt(cursorIn(folderTree_->viewport()));
        if (treeTarget == nullptr) {
            // Empty pane space is not a destination; only rows are.
            highlightDropFolder(0, false);
            drop->ignore();
            return true;
        }
        destination = treeTarget->data(0, Qt::UserRole).toLongLong();
        on_folder = true;
    } else if (on_crumb) {
        destination = crumb->property("mvpDropFolder").toLongLong();
        on_folder = true;
    } else if (on_details) {
        auto* item = detailsTree_->itemAt(cursorIn(detailsTree_->viewport()));
        if (item != nullptr) {
            const int kind = item->data(0, Qt::UserRole + 12).toInt();
            const auto role = item->data(0, Qt::UserRole).toLongLong();
            if (kind == kKindPhone) {
                return rejectPhoneDrop();
            }
            if (kind == 1) {
                destination = -role;
                on_folder = true;
            } else if (kind == 2) {
                destination = role;
                on_folder = true;
            }
        }
    } else {
        auto* item = iconList_->itemAt(cursorIn(iconList_->viewport()));
        if (item != nullptr) {
            const int kind = item->data(Qt::UserRole + 12).toInt();
            const auto role = item->data(Qt::UserRole).toLongLong();
            if (kind == kKindPhone) {
                return rejectPhoneDrop();
            }
            if (kind == 1) {
                destination = -role;
                on_folder = true;
            } else if (kind == 2) {
                destination = role;
                on_folder = true;
            }
        }
    }

    // Empty gallery space is a destination too: it means the current folder,
    // which is a no-op for rows that already live here.
    // TargetMoveAction keeps the gallery rows in place. A plain MoveAction makes
    // Qt delete the dragged rows after the drop, which fights the refresh.
    drop->setDropAction(internal ? Qt::TargetMoveAction : Qt::CopyAction);
    if (event->type() != QEvent::Drop) {
        highlightDropFolder(on_folder ? destination : -1, on_folder);
        // Hovering a closed folder in the pane opens it after a moment.
        if (dragExpandTimer_ != nullptr) {
            const bool closedTarget = on_tree && treeTarget != nullptr
                && !treeTarget->isExpanded() && treeTarget->childCount() > 0;
            if (closedTarget) {
                const auto expand_id = treeTarget->data(0, Qt::UserRole).toLongLong();
                if (dragExpandFolder_ != expand_id || !dragExpandTimer_->isActive()) {
                    dragExpandFolder_ = expand_id;
                    dragExpandTimer_->start(700);
                }
            } else {
                dragExpandTimer_->stop();
                dragExpandFolder_ = 0;
            }
        }
        drop->accept();
        return true;
    }

    if (dragExpandTimer_ != nullptr) {
        dragExpandTimer_->stop();
        dragExpandFolder_ = 0;
    }
    highlightDropFolder(0, false);
    if (internal) {
        moveItemsToFolder(destination);
    } else {
        std::vector<std::filesystem::path> sources;
        const auto urls = drop->mimeData()->urls();
        for (const auto& url : urls) {
            if (url.isLocalFile()) {
                sources.push_back(pathFromText(url.toLocalFile()));
            }
        }
        if (!sources.empty()) {
            beginImportTree(std::move(sources), destination);
        }
    }
    drop->accept();
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (vault_ && vault_->is_unlocked() && event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        for (const auto& url : urls) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
    std::vector<std::filesystem::path> sources;
    const auto urls = event->mimeData()->urls();
    for (const auto& url : urls) {
        if (url.isLocalFile()) {
            sources.push_back(pathFromText(url.toLocalFile()));
        }
    }
    if (!sources.empty()) {
        event->acceptProposedAction();
        beginImportTree(std::move(sources), currentFolderId_);
        return;
    }
    QMainWindow::dropEvent(event);
}

void MainWindow::openFolder(const std::int64_t folder_id) {
    currentFolderId_ = folder_id < 0 ? 0 : folder_id;
    refreshGallery();
}

namespace {
// The path bar's crumbs share one stylesheet; the states are properties so that a drag
// highlight cannot rewrite it. min-height is explicit because a widget stylesheet that
// replaced this one used to drop the application's height rule, collapsing the row.
const QString kCrumbStyle = QStringLiteral(
    "QPushButton { color: #8ec8ff; background: transparent; border: none;"
    " padding: 2px 4px; min-height: 36px; }"
    "QPushButton[crumbCurrent=\"true\"] { color: white; }"
    "QPushButton[dropTarget=\"true\"] { color: white;"
    " background: rgba(38, 114, 200, 150); border-radius: 8px; }");
} // namespace

void MainWindow::rebuildBreadcrumb() {
    if (crumbLayout_ == nullptr) {
        return;
    }
    while (QLayoutItem* item = crumbLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto addButton = [this](const QString& text, const std::int64_t id, const bool current) {
        auto* button = new QPushButton(text, crumbHost_);
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setAcceptDrops(true);
        button->setProperty("mvpDropFolder", static_cast<qlonglong>(id));
        button->setProperty("crumbCurrent", current);
        button->setProperty("dropTarget", false);
        button->installEventFilter(this);
        // One stylesheet for every crumb, switched by properties rather than replaced by
        // setStyleSheet(). Replacing the stylesheet of a live button changes its size hint:
        // the header then shrinks and clips the path bar. The explicit min-height keeps the
        // row's height independent of whether an application-level rule survives a restyle.
        button->setStyleSheet(kCrumbStyle);
        connect(button, &QPushButton::clicked, this, [this, id] { openFolder(id); });
        crumbLayout_->addWidget(button);
    };
    std::vector<std::pair<std::int64_t, QString>> chain;
    std::int64_t id = currentFolderId_;
    for (int guard = 0; guard < 64 && id != 0; ++guard) {
        const core::FolderInfo* found = nullptr;
        for (const auto& folder : folders_) {
            if (folder.id == id) {
                found = &folder;
                break;
            }
        }
        if (found == nullptr) {
            break;
        }
        chain.push_back({id, QString::fromStdString(found->name)});
        id = found->parent_id;
    }
    addButton(QStringLiteral("Library"), 0, chain.empty());
    for (int index = static_cast<int>(chain.size()) - 1; index >= 0; --index) {
        auto* slash = new QLabel(QStringLiteral("/"), crumbHost_);
        slash->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
        crumbLayout_->addWidget(slash);
        addButton(chain[static_cast<std::size_t>(index)].second,
            chain[static_cast<std::size_t>(index)].first, index == 0);
    }
    crumbLayout_->addStretch(1);
    // The bar's geometry was computed from a hint taken before these crumbs existed.
    layoutLibraryChrome();
}

void MainWindow::beginNewFolder() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    InWindowSheet prompt(this);
    prompt.setTitle(QStringLiteral("New folder"));
    prompt.setMessage(QStringLiteral("Name"));
    prompt.addTextEntry(QLineEdit::Normal, QString(), QStringLiteral("Folder name"));
    prompt.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    prompt.addButton(QStringLiteral("Create"), 1, true, QStringLiteral("plus"));
    if (prompt.run() != 1) {
        return;
    }
    const QString name = prompt.text();
    auto created = vault_->create_folder(currentFolderId_, name.toStdString());
    if (!created) {
        setError(galleryStatus_, vaultErrorText(created.error()));
        galleryStatus_->setVisible(true);
        return;
    }
    setError(galleryStatus_, {});
    refreshGallery();
}

void MainWindow::beginRenameFolder(const std::int64_t folder_id) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    QString current;
    for (const auto& folder : folders_) {
        if (folder.id == folder_id) {
            current = QString::fromStdString(folder.name);
            break;
        }
    }
    InWindowSheet prompt(this);
    prompt.setTitle(QStringLiteral("Rename folder"));
    prompt.setMessage(QStringLiteral("Name"));
    prompt.addTextEntry(QLineEdit::Normal, current, QStringLiteral("Folder name"));
    prompt.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    prompt.addButton(QStringLiteral("Rename"), 1, true, QStringLiteral("pencil"));
    if (prompt.run() != 1) {
        return;
    }
    const QString name = prompt.text();
    auto renamed = vault_->rename_folder(folder_id, name.toStdString());
    if (!renamed) {
        setError(galleryStatus_, vaultErrorText(renamed.error()));
        galleryStatus_->setVisible(true);
        return;
    }
    setError(galleryStatus_, {});
    refreshGallery();
}

void MainWindow::beginRemoveFolder(const std::int64_t folder_id) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    QString name = QStringLiteral("this folder");
    std::int64_t parent = 0;
    for (const auto& folder : folders_) {
        if (folder.id == folder_id) {
            name = QString::fromStdString(folder.name);
            parent = folder.parent_id;
            break;
        }
    }
    int inside = 0;
    auto listed = vault_->list_videos();
    if (listed) {
        std::vector<std::int64_t> tree;
        tree.push_back(folder_id);
        for (std::size_t index = 0; index < tree.size(); ++index) {
            for (const auto& folder : folders_) {
                if (folder.parent_id == tree[index]) {
                    tree.push_back(folder.id);
                }
            }
        }
        for (const auto& video : listed.value()) {
            if (std::find(tree.begin(), tree.end(), video.folder_id) != tree.end()) {
                ++inside;
            }
        }
    }
    InWindowSheet box(this);
    box.setTitle(QStringLiteral("Remove folder"));
    box.setMessage(QStringLiteral("Remove \"%1\"?\n\n%2").arg(name, inside > 0
        ? QStringLiteral("Delete %1 file(s) inside this folder? This cannot be undone.").arg(inside)
        : QStringLiteral("This folder is empty.")));
    box.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    box.addButton(inside > 0 ? QStringLiteral("Delete") : QStringLiteral("Remove folder"), 1, true);
    if (box.run() != 1) {
        return;
    }
    auto removed = vault_->remove_folder(folder_id);
    if (!removed) {
        setError(galleryStatus_, vaultErrorText(removed.error()));
        galleryStatus_->setVisible(true);
        return;
    }
    std::int64_t cursor = currentFolderId_;
    bool leaving = false;
    for (int guard = 0; guard < 64 && cursor != 0; ++guard) {
        if (cursor == folder_id) {
            leaving = true;
            break;
        }
        std::int64_t next = 0;
        for (const auto& folder : folders_) {
            if (folder.id == cursor) {
                next = folder.parent_id;
                break;
            }
        }
        cursor = next;
    }
    if (leaving) {
        currentFolderId_ = parent;
    }
    setError(galleryStatus_, {});
    refreshGallery();
}

void MainWindow::beginRenameVideo(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    QString current;
    if (galleryStack_->currentWidget() == detailsTree_) {
        for (int index = 0; index < detailsTree_->topLevelItemCount(); ++index) {
            auto* item = detailsTree_->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toLongLong() == video_id
                && item->data(0, Qt::UserRole + 12).toInt() == 0) {
                current = item->text(0);
                break;
            }
        }
    } else if (auto* item = galleryIconItem(video_id)) {
        current = item->data(Qt::UserRole + 1).toString();
    }
    InWindowSheet prompt(this);
    prompt.setTitle(QStringLiteral("Rename"));
    prompt.setMessage(QStringLiteral("Name"));
    prompt.addTextEntry(QLineEdit::Normal, current, QStringLiteral("Name"));
    prompt.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    prompt.addButton(QStringLiteral("Rename"), 1, true, QStringLiteral("pencil"));
    if (prompt.run() != 1) {
        return;
    }
    const QString name = prompt.text();
    auto renamed = vault_->rename_video(video_id, name.toStdString());
    if (!renamed) {
        setError(galleryStatus_, vaultErrorText(renamed.error()));
        galleryStatus_->setVisible(true);
        return;
    }
    setError(galleryStatus_, {});
    refreshGallery();
}

void MainWindow::beginNewRecord() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    InWindowSheet prompt(this);
    prompt.setTitle(QStringLiteral("New record"));
    prompt.setMessage(QStringLiteral("A record file holds a list of typed records."));
    QLineEdit* titleEntry = prompt.addTextEntry(QLineEdit::Normal, {}, QStringLiteral("Title"));
    prompt.addButton(QStringLiteral("Cancel"), 0, false);
    prompt.addButton(QStringLiteral("Create"), 1, true, QStringLiteral("plus"));
    if (prompt.run() != 1) {
        return;
    }
    const QString title = titleEntry->text().trimmed();
    if (title.isEmpty()) {
        showStatus(QStringLiteral("A record needs a title."), true);
        return;
    }

    // The item is created before the editor opens, so it appears in this folder
    // immediately — the same shape as creating a folder. The content is an empty
    // file of records, which keeps the item recognisable as a record before its
    // first record is added; a zero-byte item could not be routed to the editor.
    // The title lives on the library item, not inside the content.
    RecordDocument document;
    createRecordItem(title, serializeRecord(document));
}

// Shared by New record and New from template: import the content as an item in the open
// folder, refresh, then open the editor on it.
void MainWindow::createRecordItem(const QString& title, const QByteArray& bytes) {
    const auto vault = vault_;
    const auto folder_id = currentFolderId_;
    auto* watcher = new QFutureWatcher<std::shared_ptr<core::Result<std::int64_t>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, title] {
        const auto outcome = *watcher->result();
        watcher->deleteLater();
        if (!outcome) {
            setError(galleryStatus_, vaultErrorText(outcome.error()));
            galleryStatus_->setVisible(true);
            return;
        }
        setError(galleryStatus_, {});
        refreshGallery();
        beginRecord(outcome.value(), title);
    });
    watcher->setFuture(QtConcurrent::run([vault, bytes, title, folder_id] {
        const std::span<const unsigned char> content(
            reinterpret_cast<const unsigned char*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size()));
        return std::make_shared<core::Result<std::int64_t>>(
            vault->import_bytes(content, title.toStdString(), folder_id));
    }));
}

void MainWindow::beginRecord(const std::int64_t video_id, const QString& title) {
    if (!vault_ || !vault_->is_unlocked() || video_id <= 0) {
        return;
    }
    closePdf();
    if (playerWindow_ != nullptr) {
        auto* dying = playerWindow_;
        playerWindow_ = nullptr;
        dying->close();
    }
    if (recordWindow_ == nullptr) {
        auto* window = new RecordWindow(contentStack_);
        window->setAttribute(Qt::WA_DeleteOnClose);
        connect(window, &QObject::destroyed, this, [this, window] {
            if (recordWindow_ == window) {
                recordWindow_ = nullptr;
                if (contentStack_ != nullptr && pages_ != nullptr
                    && playerWindow_ == nullptr) {
                    fadeStack(contentStack_, pages_);
                }
            }
            QTimer::singleShot(0, this, [this] { restoreGalleryFocus(); });
        });
        connect(window, &RecordWindow::closeRequested, this, [this] { closeRecord(); });
        connect(window, &RecordWindow::saved, this, [this] { refreshGallery(); });
        // A saved template is a new library item; refresh so it is there when the user goes back.
        connect(window, &RecordWindow::templateSaved, this, [this](const QString&) { refreshGallery(); });
        recordWindow_ = window;
    }
    recordWindow_->openRecord(vault_, video_id, title, currentFolderId_);
    if (contentStack_ != nullptr && contentStack_->indexOf(recordWindow_) < 0) {
        contentStack_->addWidget(recordWindow_);
    }
    if (contentStack_ != nullptr) {
        fadeStack(contentStack_, recordWindow_);
    }
    recordWindow_->show();
    recordWindow_->setFocus();
}

void MainWindow::closeRecord() {
    if (recordWindow_ == nullptr) {
        return;
    }
    auto* dying = recordWindow_;
    recordWindow_ = nullptr;
    if (contentStack_ != nullptr) {
        const bool showing = contentStack_->currentWidget() == dying;
        contentStack_->removeWidget(dying);
        if (showing && pages_ != nullptr) {
            fadeStack(contentStack_, pages_);
        }
    }
    dying->close();
}

void MainWindow::beginMoveSelection(const std::int64_t moving_folder_id) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    std::vector<std::int64_t> blocked;
    if (moving_folder_id > 0) {
        blocked.push_back(moving_folder_id);
        for (std::size_t index = 0; index < blocked.size(); ++index) {
            for (const auto& folder : folders_) {
                if (folder.parent_id == blocked[index]) {
                    blocked.push_back(folder.id);
                }
            }
        }
    }
    std::int64_t destination = 0;
    if (!chooseFolder(QStringLiteral("Move to"), blocked, &destination)) {
        return;
    }
    if (moving_folder_id > 0) {
        auto moved = vault_->move_folder(moving_folder_id, destination);
        if (!moved) {
            setError(galleryStatus_, vaultErrorText(moved.error()));
            galleryStatus_->setVisible(true);
            return;
        }
    } else {
        const auto ids = selectedVideoIds();
        for (const auto id : ids) {
            auto moved = vault_->move_video(id, destination);
            if (!moved) {
                setError(galleryStatus_, vaultErrorText(moved.error()));
                galleryStatus_->setVisible(true);
                refreshGallery();
                return;
            }
        }
    }
    setError(galleryStatus_, {});
    refreshGallery();
}

bool MainWindow::chooseFolder(const QString& title, const std::vector<std::int64_t>& blocked,
    std::int64_t* chosen) {
    auto blockedHas = [&blocked](const std::int64_t id) {
        for (const auto id_value : blocked) {
            if (id_value == id) {
                return true;
            }
        }
        return false;
    };
    auto pathFor = [this](const std::int64_t folder_id) {
        QStringList parts;
        std::int64_t cursor = folder_id;
        for (int guard = 0; guard < 64 && cursor != 0; ++guard) {
            const core::FolderInfo* found = nullptr;
            for (const auto& folder : folders_) {
                if (folder.id == cursor) {
                    found = &folder;
                    break;
                }
            }
            if (found == nullptr) {
                break;
            }
            parts.prepend(QString::fromStdString(found->name));
            cursor = found->parent_id;
        }
        return parts.isEmpty() ? QStringLiteral("Library") : parts.join(QStringLiteral(" / "));
    };

    InWindowSheet dialog(this);
    dialog.setTitle(title);
    auto* layout = dialog.body();
    auto* list = new QListWidget();
    if (!blockedHas(0)) {
        auto* root = new QListWidgetItem(QStringLiteral("Library"), list);
        root->setData(Qt::UserRole, static_cast<qlonglong>(0));
    }
    std::vector<core::FolderInfo> ordered = folders_;
    std::sort(ordered.begin(), ordered.end(), [](const core::FolderInfo& left, const core::FolderInfo& right) {
        return left.name < right.name;
    });
    for (const auto& folder : ordered) {
        if (blockedHas(folder.id)) {
            continue;
        }
        auto* item = new QListWidgetItem(pathFor(folder.id), list);
        item->setData(Qt::UserRole, static_cast<qlonglong>(folder.id));
    }
    if (list->count() == 0) {
        setError(galleryStatus_, QStringLiteral("There is nowhere else to move this."));
        galleryStatus_->setVisible(true);
        return false;
    }
    list->setCurrentRow(0);
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toLongLong() == *chosen) {
            list->setCurrentRow(row);
            break;
        }
    }
    list->setMinimumHeight(240);
    layout->addWidget(list);
    dialog.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    dialog.addButton(QStringLiteral("Move"), 1, true, QStringLiteral("arrow-right"));
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog] { dialog.finish(1); });
    if (dialog.run() != 1 || list->currentItem() == nullptr) {
        return false;
    }
    *chosen = list->currentItem()->data(Qt::UserRole).toLongLong();
    return true;
}

int MainWindow::selectedGalleryKind() const {
    // The gallery keeps a current row that is not always the one the user
    // clicked (selection can change without the current index following), so
    // the selection wins whenever the current row is not part of it.
    if (galleryStack_->currentWidget() == detailsTree_) {
        auto* item = detailsTree_->currentItem();
        if (item == nullptr || !item->isSelected()) {
            const auto selected = detailsTree_->selectedItems();
            item = selected.isEmpty() ? nullptr : selected.first();
        }
        return item == nullptr ? -1 : item->data(0, Qt::UserRole + 12).toInt();
    }
    auto* item = iconList_->currentItem();
    if (item == nullptr || !item->isSelected()) {
        const auto selected = iconList_->selectedItems();
        item = selected.isEmpty() ? nullptr : selected.first();
    }
    return item == nullptr ? -1 : item->data(Qt::UserRole + 12).toInt();
}

void MainWindow::activateCurrentItem() {
    if (galleryStack_->currentWidget() == detailsTree_) {
        auto* item = detailsTree_->currentItem();
        if (item == nullptr || !item->isSelected()) {
            const auto selected = detailsTree_->selectedItems();
            item = selected.isEmpty() ? nullptr : selected.first();
        }
        if (item == nullptr) {
            return;
        }
        const int kind = item->data(0, Qt::UserRole + 12).toInt();
        const auto id = item->data(0, Qt::UserRole).toLongLong();
        if (kind == 1) {
            openFolder(-id);
        } else if (kind == 2) {
            openFolder(id);
        } else if (kind == kKindPhone) {
            openPeerBrowser();
        } else if (id > 0) {
            beginPlayback(id);
        }
        return;
    }
    auto* item = iconList_->currentItem();
    if (item == nullptr || !item->isSelected()) {
        const auto selected = iconList_->selectedItems();
        item = selected.isEmpty() ? nullptr : selected.first();
    }
    if (item == nullptr) {
        return;
    }
    const int kind = item->data(Qt::UserRole + 12).toInt();
    const auto id = item->data(Qt::UserRole).toLongLong();
    if (kind == 1) {
        openFolder(-id);
    } else if (kind == 2) {
        openFolder(id);
    } else if (kind == kKindPhone) {
        openPeerBrowser();
    } else if (id > 0) {
        beginPlayback(id);
    }
}

namespace {

int rowRank(const int kind) {
    if (kind == 2 || kind == 3) {
        return 0;
    }
    return kind == 1 ? 1 : 2;
}

int compareArrival(const int key, const bool ascending, const qlonglong id, const QString& name,
    const qlonglong size, const qlonglong importedAt, const QListWidgetItem* other) {
    auto textCompare = [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive);
    };
    auto numberCompare = [](const qlonglong a, const qlonglong b) {
        return a < b ? -1 : (a > b ? 1 : 0);
    };
    int compared = 0;
    switch (key) {
    case 1:
        compared = numberCompare(size, other->data(Qt::UserRole + 6).toLongLong());
        break;
    case 2:
        compared = numberCompare(0, other->data(Qt::UserRole + 7).toLongLong());
        break;
    case 3:
        compared = numberCompare(0, other->data(Qt::UserRole + 8).toLongLong());
        break;
    case 4:
        compared = textCompare(QString(), other->data(Qt::UserRole + 11).toString());
        break;
    case 5:
        compared = textCompare(QString(), other->data(Qt::UserRole + 2).toString());
        break;
    case 6:
        compared = numberCompare(importedAt, other->data(Qt::UserRole + 9).toLongLong());
        break;
    default:
        compared = textCompare(name, other->data(Qt::UserRole + 1).toString());
        break;
    }
    if (compared == 0) {
        compared = numberCompare(id, other->data(Qt::UserRole).toLongLong());
    }
    if (!ascending) {
        compared = -compared;
    }
    return compared;
}

int insertIndex(QListWidget* list, const int wantedRank, const int key, const bool ascending,
    const qlonglong id, const QString& name, const qlonglong size, const qlonglong importedAt) {
    for (int i = 0; i < list->count(); ++i) {
        const int rank = rowRank(list->item(i)->data(Qt::UserRole + 12).toInt());
        if (rank < wantedRank) {
            continue;
        }
        if (rank > wantedRank
            || compareArrival(key, ascending, id, name, size, importedAt, list->item(i)) < 0) {
            return i;
        }
    }
    return list->count();
}

} // namespace

void MainWindow::revealVideo(const qlonglong id, const QString& name, const qlonglong size,
    const qlonglong folderId, const qlonglong importedAt) {
    if (id <= 0 || findListItemById(iconList_, id) != nullptr) {
        return;
    }
    if (galleryListing_) {
        for (const auto& pending : pendingVideos_) {
            if (pending.id == id) {
                return;
            }
        }
        pendingVideos_.push_back(ArrivedVideo{id, name, size, folderId, importedAt});
        return;
    }
    if (!vault_ || !vault_->is_unlocked() || folderId != currentFolderId_) {
        return;
    }
    if (!tagFilterNames_.isEmpty()) {
        return;
    }
    if (!searchText_.isEmpty() && !name.contains(searchText_, Qt::CaseInsensitive)) {
        return;
    }
    placeVideoRow(id, name, size, importedAt);
}

void MainWindow::revealFolder(const qlonglong id, const qlonglong parentId, const QString& name) {
    if (id <= 0 || findListItemById(iconList_, -id) != nullptr) {
        return;
    }
    if (galleryListing_) {
        for (const auto& pending : pendingFolders_) {
            if (pending.id == id) {
                return;
            }
        }
        pendingFolders_.push_back(ArrivedFolder{id, parentId, name});
        return;
    }
    if (!vault_ || !vault_->is_unlocked() || parentId != currentFolderId_) {
        return;
    }
    if (!searchText_.isEmpty() && !name.contains(searchText_, Qt::CaseInsensitive)) {
        return;
    }
    placeFolderRow(id, name);
}

void MainWindow::placeVideoRow(const qlonglong id, const QString& name, const qlonglong size,
    const qlonglong importedAt) {
    const int index = insertIndex(iconList_, 2, sortKey_, sortAscending_, id, name, size, importedAt);
    // Units step down to KB for small files; a 4 KB file used to read "0.00 MB".
    const QString sizeText = compactSize(static_cast<std::uint64_t>(std::max<qlonglong>(0, size)));
    auto* iconItem = new QListWidgetItem();
    iconItem->setData(Qt::UserRole, id);
    iconItem->setData(Qt::UserRole + 1, name);
    iconItem->setData(Qt::UserRole + 5, sizeText);
    iconItem->setData(Qt::UserRole + 6, size);
    iconItem->setData(Qt::UserRole + 9, importedAt);
    iconItem->setData(Qt::UserRole + 12, 0);
    iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    if (isAudioFileName(name)) {
        const auto label = songLabel(name);
        iconItem->setData(Qt::UserRole + 13, label.first);
        iconItem->setData(Qt::UserRole + 14, label.second);
    }
    iconItem->setToolTip(QStringLiteral("Name:%1\nSize: %2\nCreated:%3")
        .arg(name, sizeText, QDateTime::fromSecsSinceEpoch(importedAt).toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
    updateIconItemText(iconItem);
    iconList_->insertItem(index, iconItem);

    auto* treeItem = new QTreeWidgetItem();
    treeItem->setText(0, name);
    treeItem->setText(1, sizeText);
    treeItem->setText(2, QStringLiteral("…"));
    treeItem->setText(3, QString());
    treeItem->setData(3, Qt::UserRole, static_cast<qlonglong>(0));
    treeItem->setText(6, QDateTime::fromSecsSinceEpoch(importedAt).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
    treeItem->setData(0, Qt::UserRole, id);
    treeItem->setData(0, Qt::UserRole + 12, 0);
    treeItem->setToolTip(0, iconItem->toolTip());
    detailsTree_->insertTopLevelItem(std::min(index, detailsTree_->topLevelItemCount()), treeItem);
    galleryCache_->iconRows[id] = iconItem;
    galleryCache_->treeRows[id] = treeItem;
    decorateVisible();
    recountLibrary();
}

void MainWindow::placeFolderRow(const qlonglong id, const QString& name) {
    const int index = insertIndex(iconList_, 1, sortKey_, sortAscending_, -id, name, 0, 0);
    addLibraryEntry(iconList_, detailsTree_, -id, 1, name, QStringLiteral("Folder"));
    if (iconList_->count() > 0 && index < iconList_->count() - 1) {
        iconList_->insertItem(index, iconList_->takeItem(iconList_->count() - 1));
    }
    if (detailsTree_->topLevelItemCount() > 0 && index < detailsTree_->topLevelItemCount() - 1) {
        detailsTree_->insertTopLevelItem(
            index, detailsTree_->takeTopLevelItem(detailsTree_->topLevelItemCount() - 1));
    }
    recountLibrary();
}

void MainWindow::recountLibrary() {
    if (statusCountLabel_ == nullptr || iconList_ == nullptr) {
        return;
    }
    int items = 0;
    qlonglong bytes = 0;
    for (int i = 0; i < iconList_->count(); ++i) {
        const int kind = iconList_->item(i)->data(Qt::UserRole + 12).toInt();
        if (kind == 2) {
            continue;
        }
        ++items;
        if (kind == 0) {
            bytes += iconList_->item(i)->data(Qt::UserRole + 6).toLongLong();
        }
    }
    setFooterSummary(QStringLiteral("%1 items · %2")
        .arg(items)
        .arg(compactSize(static_cast<std::uint64_t>(std::max<qlonglong>(0, bytes)))));
    if (emptyState_ != nullptr) {
        if (items == 0) {
            emptyState_->setText(currentFolderId_ == 0
                ? QStringLiteral("Nothing here yet.\nImport files, or drop them on the library.")
                : QStringLiteral("This folder is empty."));
            emptyState_->show();
            layoutLibraryChrome();
        } else {
            emptyState_->hide();
        }
    }
}

// The footer reports what the user is acting on. Selecting rows must never
// trigger a scan: sizes come from the row data the listing already carries, so
// selecting a folder shows its count, not a recursive total nobody asked for.
QString MainWindow::selectionFooterText() const {
    if (galleryStack_ == nullptr) {
        return {};
    }
    auto* view = qobject_cast<QAbstractItemView*>(galleryStack_->currentWidget());
    if (view == nullptr || view->selectionModel() == nullptr) {
        return {};
    }
    int files = 0;
    int folders = 0;
    qlonglong bytes = 0;
    QSet<int> visitedRows;
    const auto indexes = view->selectionModel()->selectedIndexes();
    for (const auto& index : indexes) {
        // The details tree reports one index per column of the same row.
        if (visitedRows.contains(index.row())) {
            continue;
        }
        visitedRows.insert(index.row());
        const int kind = index.data(Qt::UserRole + 12).toInt();
        if (kind == 0) {
            ++files;
            bytes += index.data(Qt::UserRole + 6).toLongLong();
        } else if (kind == 1) {
            ++folders;
        }
    }
    if (files == 0 && folders == 0) {
        return {};
    }
    const QString size = compactSize(static_cast<std::uint64_t>(std::max<qlonglong>(0, bytes)));
    if (folders == 0) {
        return QStringLiteral("%1 selected · %2").arg(files).arg(size);
    }
    if (files == 0) {
        return QStringLiteral("%1 folder%2 selected · contents not counted")
            .arg(folders)
            .arg(folders == 1 ? QString() : QStringLiteral("s"));
    }
    return QStringLiteral("%1 selected · %2 in files · folder contents not counted")
        .arg(files + folders)
        .arg(size);
}

void MainWindow::setFooterSummary(const QString& summary) {
    footerSummary_ = summary;
    updateSelectionFooter();
}

void MainWindow::updateSelectionFooter() {
    if (statusCountLabel_ == nullptr) {
        return;
    }
    const QString selection = selectionFooterText();
    statusCountLabel_->setText(selection.isEmpty() ? footerSummary_ : selection);
    statusCountLabel_->setToolTip(selection.isEmpty()
        ? QString()
        : QStringLiteral("Selected files only. Subfolder contents are never "
                         "counted, so large libraries stay responsive."));
}

QListWidgetItem* MainWindow::galleryIconItem(const qlonglong id) const {
    const auto found = galleryCache_->iconRows.find(id);
    return found == galleryCache_->iconRows.end() ? nullptr : found->second;
}

QTreeWidgetItem* MainWindow::galleryTreeItem(const qlonglong id) const {
    const auto found = galleryCache_->treeRows.find(id);
    return found == galleryCache_->treeRows.end() ? nullptr : found->second;
}

void MainWindow::rememberGalleryThumbnail(const qlonglong id, const QPixmap& pixmap) {
    // Row QVariant/QIcon copies also own the decoded pixels. Evict ALL owners,
    // not just the map, or scrolling through a vault still retains every tile.
    auto& cache = *galleryCache_;
    const auto old = std::find(cache.thumbOrder.begin(), cache.thumbOrder.end(), id);
    if (old != cache.thumbOrder.end()) cache.thumbOrder.erase(old);
    cache.thumbs[id] = pixmap;
    cache.thumbOrder.push_back(id);
    while (cache.thumbOrder.size() > galleryResidentLimit_) {
        const auto evicted = cache.thumbOrder.front();
        cache.thumbOrder.pop_front();
        cache.thumbs.erase(evicted);
        cache.pdfThumbs.erase(evicted);
        decoratedIds_.erase(evicted);
        if (auto* item = galleryIconItem(evicted)) {
            item->setData(Qt::UserRole + 4, QVariant());
            item->setIcon(QIcon());
        }
        if (auto* item = galleryTreeItem(evicted)) item->setIcon(0, QIcon());
    }
}

void MainWindow::galleryTaskFinished() {
    --galleryTasksInFlight_;
    QTimer::singleShot(0, this, [this] { decorateVisible(); });
}

void MainWindow::decorateVideo(const qlonglong video_id, const QString& name, const quint64 originalSize,
    const bool loadThumbnail) {
    auto applyThumb = [](QListWidgetItem* iconItem, QTreeWidgetItem* treeItem, const QPixmap& source) {
        if (source.isNull()) {
            return;
        }
        const QPixmap tile = source.width() <= 320 && source.height() <= 320
            ? source
            : source.scaled(320, 320, Qt::KeepAspectRatio, Qt::FastTransformation);
        const QPixmap tiny = tile.scaled(32, 32, Qt::KeepAspectRatio, Qt::FastTransformation);
        if (iconItem != nullptr) {
            iconItem->setData(Qt::UserRole + 4, tile);
            iconItem->setIcon(QIcon(tiny));
        }
        if (treeItem != nullptr) {
            treeItem->setIcon(0, QIcon(tiny));
        }
    };
    const auto vault = vault_;
    const auto generation = galleryGeneration_;
    auto presentIdentity = [this, vault, video_id, name, originalSize, applyThumb, generation, loadThumbnail](const FileIdentity& identity) {
        if (generation != galleryGeneration_) {
            return;
        }
        galleryCache_->kinds[video_id] = identity;
        auto* iconItem = galleryIconItem(video_id);
        auto* treeItem = galleryTreeItem(video_id);
        if (iconItem == nullptr && treeItem == nullptr) {
            return;
        }
        if (iconItem != nullptr) {
            iconItem->setData(Qt::UserRole + 15, identity.label);
            iconItem->setData(Qt::UserRole + 16, identity.route);
        }
        // Metadata sorts do not need off-screen decoded previews.
        if (!loadThumbnail && identity.route != 1 && identity.route != 2) {
            if (treeItem) {
                treeItem->setText(2, QString());
                treeItem->setText(4, identity.label);
            }
            return;
        }
        if (identity.route == 0) {
            const QPixmap badge = kindBadge(identity.label, compactSize(originalSize), 320);
            rememberGalleryThumbnail(video_id, badge);
            applyThumb(iconItem, treeItem, badge);
            if (treeItem != nullptr) {
                treeItem->setText(2, QString());
                treeItem->setText(3, QString());
                treeItem->setText(4, identity.label);
            }
            return;
        }
        const bool pdfFile = identity.route == 3;
        if (pdfFile && treeItem != nullptr) {
            treeItem->setText(2, QString());
            treeItem->setText(4, QStringLiteral("PDF"));
        }
        if (pdfFile) {
            const auto cachedThumb = galleryCache_->thumbs.find(video_id);
            if (cachedThumb != galleryCache_->thumbs.end()) {
                applyThumb(iconItem, treeItem, cachedThumb->second);
                return;
            }
            applyThumb(iconItem, treeItem, pdfDocumentPixmap(320));
            if (galleryCache_->pdfThumbs.count(video_id) != 0U || !vault) {
                return;
            }
            galleryCache_->pdfThumbs.insert(video_id);
            auto* thumbWatcher = new QFutureWatcher<QByteArray>(this);
            ++galleryTasksInFlight_;
            connect(thumbWatcher, &QFutureWatcherBase::finished, this,
                [this, video_id, thumbWatcher, applyThumb, generation] {
                    const QByteArray jpeg = thumbWatcher->result();
                    thumbWatcher->deleteLater();
                    galleryTaskFinished();
                    if (generation != galleryGeneration_ || jpeg.isEmpty()) {
                        return;
                    }
                    QPixmap pixmap;
                    if (!pixmap.loadFromData(jpeg, "JPEG")) {
                        return;
                    }
                    const QPixmap tile = pixmap.width() <= 320 && pixmap.height() <= 320
                        ? pixmap
                        : pixmap.scaled(320, 320, Qt::KeepAspectRatio, Qt::FastTransformation);
                    rememberGalleryThumbnail(video_id, tile);
                    auto* iconItem = galleryIconItem(video_id);
                    auto* treeItem = galleryTreeItem(video_id);
                    applyThumb(iconItem, treeItem, tile);
                });
            thumbWatcher->setFuture(QtConcurrent::run(galleryPool_, [vault, video_id] {
                auto stored = vault->thumbnail(video_id);
                if (stored && stored.value().mime == "image/jpeg"
                    && stored.value().width >= 16U && !stored.value().bytes.empty()) {
                    return QByteArray(reinterpret_cast<const char*>(stored.value().bytes.data()),
                        static_cast<int>(stored.value().bytes.size()));
                }
                std::vector<unsigned char> plain;
                std::uint64_t offset = 0;
                constexpr std::size_t kChunk = 1024U * 1024U;
                constexpr std::size_t kMaxPdf = 80U * 1024U * 1024U;
                for (;;) {
                    auto chunk = vault->read_video_range(video_id, offset, kChunk);
                    if (!chunk || chunk.value().empty()) {
                        break;
                    }
                    if (plain.size() + chunk.value().size() > kMaxPdf) {
                        return QByteArray();
                    }
                    plain.insert(plain.end(), chunk.value().begin(), chunk.value().end());
                    offset += static_cast<std::uint64_t>(chunk.value().size());
                    if (chunk.value().size() < kChunk) {
                        break;
                    }
                }
                const QImage page = renderPdfFirstPage(plain, 480);
                if (page.isNull()) {
                    return QByteArray();
                }
                QByteArray encoded;
                QBuffer buffer(&encoded);
                buffer.open(QIODevice::WriteOnly);
                if (!page.save(&buffer, "JPEG", 82) || encoded.size() < 32) {
                    return QByteArray();
                }
                std::vector<unsigned char> jpeg(
                    reinterpret_cast<const unsigned char*>(encoded.constData()),
                    reinterpret_cast<const unsigned char*>(encoded.constData()) + encoded.size());
                (void)vault->store_thumbnail(
                    video_id, jpeg,
                    static_cast<std::uint32_t>(page.width()),
                    static_cast<std::uint32_t>(page.height()));
                return encoded;
            }));
            return;
        }
        if (identity.route == 4) {
            if (treeItem != nullptr) {
                treeItem->setText(2, QString());
                treeItem->setText(4, identity.label);
            }
            const QPixmap badge = textDocumentPixmap(identity.label, 320);
            rememberGalleryThumbnail(video_id, badge);
            applyThumb(iconItem, treeItem, badge);
            return;
        }
        if (identity.route == 5) {
            // A record is text: there is nothing to decode and no thumbnail to
            // generate, so the media columns stay empty and the kind is shown
            // instead. Probing it would only waste a decoder pass on JSON. The
            // plate carries the record's name because the icon grid has no
            // captions, and records would otherwise all look identical.
            if (treeItem != nullptr) {
                treeItem->setText(2, QString());
                treeItem->setText(3, QString());
                treeItem->setText(4, identity.label);
            }
            const QPixmap badge = recordBadge(
                identity.label, name, compactSize(originalSize), 320);
            rememberGalleryThumbnail(video_id, badge);
            applyThumb(iconItem, treeItem, badge);
            return;
        }
        if (loadThumbnail) {
        const auto cachedThumb = galleryCache_->thumbs.find(video_id);
        if (cachedThumb != galleryCache_->thumbs.end()) {
            applyThumb(iconItem, treeItem, cachedThumb->second);
        } else if (vault) {
            auto* thumbWatcher =
                new QFutureWatcher<std::shared_ptr<core::Result<core::ThumbnailInfo>>>(this);
            ++galleryTasksInFlight_;
            connect(thumbWatcher, &QFutureWatcherBase::finished, this,
                [this, video_id, name, originalSize, thumbWatcher, applyThumb, generation] {
                    const auto outcome = *thumbWatcher->result();
                    thumbWatcher->deleteLater();
                    galleryTaskFinished();
                    if (generation != galleryGeneration_) {
                        return;
                    }
                    // Continue the same request chain, never fan one sniff
                    // into concurrent thumbnail AND metadata jobs.
                    decorateVideo(video_id, name, originalSize, false);
                    if (!outcome || outcome.value().bytes.empty()) {
                        return;
                    }
                    auto* iconItem = galleryIconItem(video_id);
                    auto* treeItem = galleryTreeItem(video_id);
                    if (iconItem == nullptr && treeItem == nullptr) {
                        return;
                    }
                    if (outcome.value().mime == "image/x-mvp-note"
                        || outcome.value().mime == "image/x-mvp-empty") {
                        applyThumb(iconItem, treeItem, musicPlaceholder(96));
                        return;
                    }
                    QPixmap pixmap;
                    if (!pixmap.loadFromData(outcome.value().bytes.data(),
                            static_cast<int>(outcome.value().bytes.size()))) {
                        return;
                    }
                    const QPixmap tile = pixmap.width() <= 320 && pixmap.height() <= 320
                        ? pixmap
                        : pixmap.scaled(320, 320, Qt::KeepAspectRatio, Qt::FastTransformation);
                    rememberGalleryThumbnail(video_id, tile);
                    applyThumb(iconItem, treeItem, tile);
                });
            thumbWatcher->setFuture(QtConcurrent::run(galleryPool_, [vault, video_id] {
                auto stored = vault->display_thumbnail(video_id, 320U);
                return std::make_shared<core::Result<core::ThumbnailInfo>>(std::move(stored));
            }));
            return;
        }
        }
        if (!vault) {
            return;
        }
        auto applyMedia = [](QListWidgetItem* iconItem, QTreeWidgetItem* treeItem, const core::MediaInfo& info) {
            const std::int64_t duration = static_cast<std::int64_t>(info.duration_ms);
            const QString durationText = QStringLiteral("%1:%2")
                .arg(duration / 60000)
                .arg((duration / 1000) % 60, 2, 10, QLatin1Char('0'));
            if (iconItem != nullptr) {
                if (duration > 500) {
                    iconItem->setData(Qt::UserRole + 3, durationText);
                }
                iconItem->setData(Qt::UserRole + 7, static_cast<qlonglong>(duration));
                iconItem->setData(Qt::UserRole + 8, pixelCount(info.width, info.height));
                iconItem->setData(Qt::UserRole + 11, QString::fromStdString(info.codec_name));
                if (!info.artist.empty()) {
                    iconItem->setData(Qt::UserRole + 13, QString::fromStdString(info.artist));
                }
                if (!info.title.empty()) {
                    iconItem->setData(Qt::UserRole + 14, QString::fromStdString(info.title));
                }
            }
            if (treeItem != nullptr) {
                treeItem->setText(2, durationText);
                treeItem->setText(3, QStringLiteral("%1 × %2").arg(info.width).arg(info.height));
                treeItem->setData(3, Qt::UserRole, pixelCount(info.width, info.height));
                treeItem->setText(4, QString::fromStdString(info.codec_name));
            }
        };
        const auto cachedMedia = galleryCache_->media.find(video_id);
        if (cachedMedia != galleryCache_->media.end()) {
            const qlonglong durationBefore = iconItem == nullptr
                ? 0 : iconItem->data(Qt::UserRole + 7).toLongLong();
            const qlonglong pixelsBefore = iconItem == nullptr
                ? 0 : iconItem->data(Qt::UserRole + 8).toLongLong();
            const QString codecBefore = iconItem == nullptr
                ? QString() : iconItem->data(Qt::UserRole + 11).toString();
            applyMedia(iconItem, treeItem, cachedMedia->second);
            const bool changed = iconItem != nullptr && (
                iconItem->data(Qt::UserRole + 7).toLongLong() != durationBefore
                || iconItem->data(Qt::UserRole + 8).toLongLong() != pixelsBefore
                || iconItem->data(Qt::UserRole + 11).toString() != codecBefore);
            if (changed && (sortKey_ == 2 || sortKey_ == 3 || sortKey_ == 4)) {
                scheduleGallerySort();
            }
            return;
        }
        auto* mediaWatcher = new QFutureWatcher<std::shared_ptr<core::Result<core::MediaInfo>>>(this);
        ++galleryTasksInFlight_;
        connect(mediaWatcher, &QFutureWatcherBase::finished, this,
            [this, video_id, mediaWatcher, applyMedia, generation] {
                const auto outcome = *mediaWatcher->result();
                mediaWatcher->deleteLater();
                galleryTaskFinished();
                if (generation != galleryGeneration_ || !outcome) {
                    return;
                }
                galleryCache_->media[video_id] = outcome.value();
                auto* iconItem = galleryIconItem(video_id);
                auto* treeItem = galleryTreeItem(video_id);
                if (iconItem == nullptr && treeItem == nullptr) {
                    return;
                }
                const qlonglong durationBefore = iconItem == nullptr
                    ? 0 : iconItem->data(Qt::UserRole + 7).toLongLong();
                const qlonglong pixelsBefore = iconItem == nullptr
                    ? 0 : iconItem->data(Qt::UserRole + 8).toLongLong();
                const QString codecBefore = iconItem == nullptr
                    ? QString() : iconItem->data(Qt::UserRole + 11).toString();
                applyMedia(iconItem, treeItem, outcome.value());
                const bool changed = iconItem != nullptr && (
                    iconItem->data(Qt::UserRole + 7).toLongLong() != durationBefore
                    || iconItem->data(Qt::UserRole + 8).toLongLong() != pixelsBefore
                    || iconItem->data(Qt::UserRole + 11).toString() != codecBefore);
                if (changed && (sortKey_ == 2 || sortKey_ == 3 || sortKey_ == 4)) {
                    scheduleGallerySort();
                }
            });
        mediaWatcher->setFuture(QtConcurrent::run(galleryPool_, [vault, video_id] {
            return std::make_shared<core::Result<core::MediaInfo>>(vault->media_info(video_id));
        }));
    };
    const auto knownKind = galleryCache_->kinds.find(video_id);
    if (knownKind != galleryCache_->kinds.end()) {
        presentIdentity(knownKind->second);
        return;
    }
    auto* sniffWatcher = new QFutureWatcher<FileIdentity>(this);
    ++galleryTasksInFlight_;
    connect(sniffWatcher, &QFutureWatcherBase::finished, this, [this, sniffWatcher, presentIdentity] {
        const FileIdentity identity = sniffWatcher->result();
        sniffWatcher->deleteLater();
        galleryTaskFinished();
        presentIdentity(identity);
    });
    sniffWatcher->setFuture(QtConcurrent::run(galleryPool_, [vault, video_id, name] {
        return identityForVideo(vault, video_id, name);
    }));
}

void MainWindow::paintGalleryBatch() {
    if (iconList_ == nullptr || detailsTree_ == nullptr
        || galleryPaintGeneration_ != galleryGeneration_) {
        return;
    }
    iconList_->setUpdatesEnabled(false);
    detailsTree_->setUpdatesEnabled(false);
    // Paint the first screen promptly, then avoid hundreds of full relayouts.
    const int kBatch = galleryQueueIndex_ == 0 ? 48 : 512;
    int made = 0;
    while (made < kBatch && galleryQueueIndex_ < galleryQueue_.size()) {
        const PendingGalleryVideo& video = galleryQueue_[galleryQueueIndex_++];
        const QString sizeText = compactSize(
            static_cast<std::uint64_t>(std::max<qlonglong>(0, video.size)));
        const QString created = QDateTime::fromSecsSinceEpoch(video.importedAt)
            .toLocalTime()
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const QString tip = QStringLiteral("Name:%1\nSize: %2\nTags:%3\nCreated:%4")
            .arg(video.name, sizeText, video.tags, created);

        auto* iconItem = new QListWidgetItem(iconList_);
        iconItem->setData(Qt::UserRole, static_cast<qlonglong>(video.id));
        iconItem->setData(Qt::UserRole + 1, video.name);
        iconItem->setData(Qt::UserRole + 2, video.tags);
        iconItem->setData(Qt::UserRole + 5, sizeText);
        iconItem->setData(Qt::UserRole + 6, static_cast<qlonglong>(video.size));
        iconItem->setData(Qt::UserRole + 9, static_cast<qlonglong>(video.importedAt));
        iconItem->setData(Qt::UserRole + 12, 0);
        iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (isAudioFileName(video.name)) {
            const auto label = songLabel(video.name);
            iconItem->setData(Qt::UserRole + 13, label.first);
            iconItem->setData(Qt::UserRole + 14, label.second);
        }
        iconItem->setToolTip(tip);
        updateIconItemText(iconItem);

        auto* treeItem = new QTreeWidgetItem(detailsTree_);
        treeItem->setText(0, video.name);
        treeItem->setText(1, sizeText);
        treeItem->setText(2, QStringLiteral("…"));
        treeItem->setText(3, QString());
        treeItem->setData(3, Qt::UserRole, static_cast<qlonglong>(0));
        treeItem->setText(5, video.tags);
        treeItem->setText(6, created);
        treeItem->setData(0, Qt::UserRole, static_cast<qlonglong>(video.id));
        treeItem->setData(0, Qt::UserRole + 12, 0);
        treeItem->setToolTip(0, tip);
        galleryCache_->iconRows[video.id] = iconItem;
        galleryCache_->treeRows[video.id] = treeItem;
        ++made;
    }
    const bool more = galleryQueueIndex_ < galleryQueue_.size();
    if (!more) {
        galleryQueue_.clear();
        galleryQueueIndex_ = 0;
        sortGalleryItems();
        showGallerySortIndicator();
        galleryListing_ = false;
        // Rows exist now, so a post-move selection and any pending cut can be
        // drawn onto them.
        applyPendingSelection();
        applyCutDimming();
    }
    iconList_->setUpdatesEnabled(true);
    detailsTree_->setUpdatesEnabled(true);
    if (!more) {
        const auto videos = pendingVideos_;
        const auto folders = pendingFolders_;
        pendingVideos_.clear();
        pendingFolders_.clear();
        for (const auto& folder : folders) {
            revealFolder(folder.id, folder.parentId, folder.name);
        }
        for (const auto& video : videos) {
            revealVideo(video.id, video.name, video.size, video.folderId, video.importedAt);
        }
    }
    const auto generation = galleryPaintGeneration_;
    QTimer::singleShot(0, this, [this, generation, more] {
        if (generation != galleryPaintGeneration_) {
            return;
        }
        decorateVisible();
        if (more) {
            paintGalleryBatch();
        }
    });
}

void MainWindow::decorateVisible() {
    if (iconList_ == nullptr || detailsTree_ == nullptr || galleryStack_ == nullptr) {
        return;
    }
    // Bound queued work too; completions pump the next visible rows.
    if (galleryTasksInFlight_ >= 4 || !vault_ || !vault_->is_unlocked()) return;
    if (contentStack_ && pages_ && contentStack_->currentWidget() != pages_) return;
    const bool details = galleryStack_->currentWidget() == detailsTree_;
    QAbstractItemView* view = details
        ? static_cast<QAbstractItemView*>(detailsTree_)
        : static_cast<QAbstractItemView*>(iconList_);
    if (view->viewport() == nullptr) {
        return;
    }
    const int count = details ? detailsTree_->topLevelItemCount() : iconList_->count();
    if (count <= 0) {
        return;
    }
    int columns = 1;
    if (!details && iconList_->viewMode() == QListView::IconMode) {
        const int gridWidth = std::max(1, iconList_->gridSize().width());
        columns = std::max(1, view->viewport()->width() / gridWidth);
    }
    const QModelIndex top = view->indexAt(QPoint(4, 4));
    const QModelIndex bottom = view->indexAt(
        QPoint(4, std::max(0, view->viewport()->height() - 4)));
    int first = top.isValid() ? top.row() : 0;
    int last = bottom.isValid() ? bottom.row() : first;
    if (!top.isValid()) {
        first = 0;
        const int rowHeight = details
            ? std::max(1, detailsTree_->sizeHintForRow(0))
            : std::max(1, iconList_->gridSize().height());
        const int rows = std::max(1, view->viewport()->height() / rowHeight);
        last = std::min(count - 1, rows * columns + columns);
    } else {
        if (last < first) {
            last = first;
        }
        if (!bottom.isValid()) {
            const int rowHeight = details
                ? std::max(1, detailsTree_->sizeHintForRow(0))
                : std::max(1, iconList_->gridSize().height());
            const int rows = std::max(1, view->viewport()->height() / rowHeight);
            last = std::min(count - 1, first + rows * columns);
        }
        first = std::max(0, first - columns);
        last = std::min(count - 1, last + columns * 2);
    }

    // Keep at least one viewport plus the prefetch rows on large/HiDPI
    // monitors, otherwise visible rows continuously evict one another.
    galleryResidentLimit_ = std::max<std::size_t>(128, static_cast<std::size_t>(last - first + 1));
    int started = 0;
    constexpr int kCap = 16;
    auto consider = [&](const qlonglong id, const QString& name, const quint64 size, const int kind,
        const bool thumbnail = true) {
        if (started >= kCap || galleryTasksInFlight_ >= 4 || kind != 0 || id <= 0) {
            return;
        }
        auto& requested = thumbnail ? decoratedIds_ : metadataRequested_;
        if (!requested.insert(static_cast<std::int64_t>(id)).second) {
            return;
        }
        metadataRequested_.insert(id);
        decorateVideo(id, name, size, thumbnail);
        ++started;
    };
    for (int index = first; index <= last && started < kCap; ++index) {
        if (details) {
            auto* item = detailsTree_->topLevelItem(index);
            if (item == nullptr) {
                continue;
            }
            const qlonglong id = item->data(0, Qt::UserRole).toLongLong();
            quint64 size = 0;
            if (auto* icon = findListItemById(iconList_, id)) {
                size = static_cast<quint64>(icon->data(Qt::UserRole + 6).toULongLong());
            }
            consider(id, item->text(0), size, item->data(0, Qt::UserRole + 12).toInt());
        } else {
            auto* item = iconList_->item(index);
            if (item == nullptr) {
                continue;
            }
            consider(
                item->data(Qt::UserRole).toLongLong(),
                item->data(Qt::UserRole + 1).toString(),
                static_cast<quint64>(item->data(Qt::UserRole + 6).toULongLong()),
                item->data(Qt::UserRole + 12).toInt());
        }
    }
    const bool probeLibrary = sortKey_ == 2 || sortKey_ == 3 || sortKey_ == 4;
    const bool moreVisible = last > first && started >= kCap;
    if (probeLibrary && started < kCap && galleryTasksInFlight_ < 4) {
        const int libraryCount = iconList_->count();
        int scanned = 0;
        while (started < kCap && galleryTasksInFlight_ < 4 && libraryCount > 0 && scanned < libraryCount) {
            const int index = sortProbeCursor_ % libraryCount;
            ++sortProbeCursor_;
            ++scanned;
            auto* item = iconList_->item(index);
            if (item == nullptr) {
                continue;
            }
            consider(
                item->data(Qt::UserRole).toLongLong(),
                item->data(Qt::UserRole + 1).toString(),
                static_cast<quint64>(item->data(Qt::UserRole + 6).toULongLong()),
                item->data(Qt::UserRole + 12).toInt(), false);
        }
    }
    if (moreVisible || (probeLibrary && started > 0)) {
        const auto generation = galleryPaintGeneration_;
        QTimer::singleShot(probeLibrary ? 30 : 0, this, [this, generation] {
            if (generation != galleryPaintGeneration_) {
                return;
            }
            decorateVisible();
        });
    }
}

void MainWindow::refreshGallery() {
    if (pluginFolder_) {
        return;  // the folder owns the view until Back
    }
    // Invalidate callbacks before deleting any rows, including error paths.
    const auto generation = ++galleryGeneration_;
    galleryPaintGeneration_ = generation;
    galleryCache_->iconRows.clear();
    galleryCache_->treeRows.clear();
    galleryCache_->thumbs.clear();
    galleryCache_->thumbOrder.clear();
    galleryCache_->pdfThumbs.clear();
    decoratedIds_.clear();
    metadataRequested_.clear();
    galleryListing_ = true;
    galleryQueue_.clear();
    galleryQueueIndex_ = 0;
    detailsTree_->clear();
    iconList_->clear();
    if (emptyState_ != nullptr) {
        emptyState_->hide();
    }
    if (galleryStatus_ != nullptr && galleryStatus_->property("problem").toBool()) {
        showStatus({}, false);
    }
    if (!vault_ || !vault_->is_unlocked()) {
        folders_.clear();
        rebuildBreadcrumb();
        rebuildFolderTree();
        galleryListing_ = false;
        return;
    }
    auto listed = vault_->list_folders();
    if (!listed) {
        setError(galleryStatus_, vaultErrorText(listed.error()));
        galleryStatus_->setVisible(true);
        pendingSelection_.clear();
        galleryListing_ = false;
        return;
    }
    folders_ = std::move(listed.value());
    // The templates folder is a store, not a place in the library. Dropping it here keeps it
    // out of every view built from folders_ -- the grid, the tree, the breadcrumb and search
    // -- so a template never appears as a file.
    templateFolderId_ = 0;
    for (auto entry = folders_.begin(); entry != folders_.end();) {
        if (isTemplateFolderName(QString::fromStdString(entry->name))) {
            templateFolderId_ = entry->id;
            entry = folders_.erase(entry);
        } else {
            ++entry;
        }
    }
    bool open = currentFolderId_ == 0;
    std::int64_t parentOfOpen = 0;
    for (const auto& folder : folders_) {
        if (folder.id == currentFolderId_) {
            open = true;
            parentOfOpen = folder.parent_id;
            break;
        }
    }
    if (!open) {
        currentFolderId_ = 0;
        parentOfOpen = 0;
    }
    rebuildBreadcrumb();
    rebuildFolderTree();
    QVector<QPair<qlonglong, QString>> childFolders;
    for (const auto& folder : folders_) {
        if (folder.parent_id == currentFolderId_) {
            childFolders.append(qMakePair(
                static_cast<qlonglong>(folder.id), QString::fromStdString(folder.name)));
        }
    }
    const auto openId = currentFolderId_;
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<std::vector<core::VideoInfo>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
        [this, generation, vault, watcher, openId, parentOfOpen, childFolders]() mutable {
        if (generation != galleryGeneration_ || vault_ != vault
            || !vault_->is_unlocked()) {
            // A newer refresh superseded this snapshot (e.g. a delete landed
            // mid-list); publishing it would resurrect removed videos.
            if (generation == galleryGeneration_) {
                galleryListing_ = false;
                galleryQueue_.clear();
                galleryQueueIndex_ = 0;
            }
            watcher->deleteLater();
            return;
        }
        std::vector<core::VideoInfo> videos = watcher->result();
        for (const auto& extra : pendingVideos_) {
            const bool known = std::any_of(videos.begin(), videos.end(),
                [&extra](const core::VideoInfo& video) { return video.id == extra.id; });
            if (known) {
                continue;
            }
            core::VideoInfo info;
            info.id = extra.id;
            info.display_name = extra.name.toUtf8().toStdString();
            info.original_size = static_cast<std::uint64_t>(std::max<qlonglong>(0, extra.size));
            info.folder_id = extra.folderId;
            info.imported_at = static_cast<std::uint64_t>(std::max<qlonglong>(0, extra.importedAt));
            videos.push_back(std::move(info));
        }
        pendingVideos_.clear();
        for (const auto& extra : pendingFolders_) {
            const bool known = std::any_of(childFolders.begin(), childFolders.end(),
                [&extra](const QPair<qlonglong, QString>& child) { return child.first == extra.id; });
            if (!known && extra.parentId == openId) {
                childFolders.append(qMakePair(extra.id, extra.name));
            }
        }
        pendingFolders_.clear();
        int folderCount = 0;
        if (openId != 0) {
            addLibraryEntry(iconList_, detailsTree_, parentOfOpen, 2,
                QStringLiteral(".."), QStringLiteral("Up"));
            ++folderCount;
        } else if (peerLinked()) {
            const QString phone = shareServer_->peer()->name();
            if (searchText_.isEmpty() || phone.contains(searchText_, Qt::CaseInsensitive)) {
                addLibraryEntry(iconList_, detailsTree_, 0, kKindPhone, phone,
                    QStringLiteral("Phone · shared with you"), phoneFolderPixmap());
                ++folderCount;
            }
        }
        for (const auto& child : childFolders) {
            if (!searchText_.isEmpty()
                && !child.second.contains(searchText_, Qt::CaseInsensitive)) {
                continue;
            }
            addLibraryEntry(iconList_, detailsTree_, -child.first, 1,
                child.second, QStringLiteral("Folder"));
            ++folderCount;
        }
        const int realFolders = folderCount - (openId != 0 ? 1 : 0);
        std::vector<core::VideoInfo> visible;
        visible.reserve(videos.size());
        for (const auto& video : videos) {
            if (video.folder_id != openId) {
                continue;
            }
            bool tag_matches = tagFilterNames_.isEmpty();
            if (!tag_matches) {
                for (const auto& tag : video.tags) {
                    if (tagFilterNames_.contains(QString::fromStdString(tag), Qt::CaseInsensitive)) {
                        tag_matches = true;
                        break;
                    }
                }
            }
            if (!tag_matches) {
                continue;
            }
            bool search_matches = searchText_.isEmpty();
            if (!search_matches) {
                for (const auto& tag : video.tags) {
                    if (QString::fromStdString(tag).contains(searchText_, Qt::CaseInsensitive)) {
                        search_matches = true;
                        break;
                    }
                }
                if (!search_matches && QString::fromStdString(video.display_name)
                        .contains(searchText_, Qt::CaseInsensitive)) {
                    search_matches = true;
                }
            }
            if (!search_matches) {
                continue;
            }
            visible.push_back(video);
        }
        std::uint64_t total_bytes = 0U;
        for (const auto& video : visible) {
            total_bytes += video.original_size;
        }
        const int sortKey = sortKey_;
        const bool ascending = sortAscending_;
        std::stable_sort(visible.begin(), visible.end(), [sortKey, ascending](const core::VideoInfo& left, const core::VideoInfo& right) {
            int compared = 0;
            switch (sortKey) {
            case 1:
                compared = left.original_size < right.original_size
                    ? -1
                    : (left.original_size > right.original_size ? 1 : 0);
                break;
            case 5: {
                const QString a = QString::fromStdString(left.tags.empty() ? std::string() : left.tags.front());
                const QString b = QString::fromStdString(right.tags.empty() ? std::string() : right.tags.front());
                compared = a.compare(b, Qt::CaseInsensitive);
                break;
            }
            case 6:
                compared = left.imported_at < right.imported_at
                    ? -1
                    : (left.imported_at > right.imported_at ? 1 : 0);
                break;
            default:
                compared = QString::fromStdString(left.display_name).compare(
                    QString::fromStdString(right.display_name), Qt::CaseInsensitive);
                break;
            }
            if (compared == 0) {
                compared = left.id < right.id ? -1 : (left.id > right.id ? 1 : 0);
            }
            return ascending ? compared < 0 : compared > 0;
        });
        galleryQueue_.clear();
        galleryQueueIndex_ = 0;
        galleryQueue_.reserve(visible.size());
        for (const auto& video : visible) {
            PendingGalleryVideo row;
            row.id = video.id;
            row.name = QString::fromStdString(video.display_name);
            row.size = video.original_size;
            row.importedAt = static_cast<std::int64_t>(video.imported_at);
            for (std::size_t i = 0U; i < video.tags.size(); ++i) {
                if (i > 0U) {
                    row.tags += QStringLiteral(", ");
                }
                row.tags += QString::fromStdString(video.tags[i]);
            }
            galleryQueue_.push_back(std::move(row));
        }
        decoratedIds_.clear();
        sortProbeCursor_ = 0;
        galleryPaintGeneration_ = generation;
        detailsTree_->setSortingEnabled(false);
        showGallerySortIndicator();
        const bool libraryEmpty = visible.empty() && realFolders == 0;
        if (emptyState_ != nullptr) {
            if (libraryEmpty) {
                if (!tagFilterNames_.isEmpty() || !searchText_.isEmpty()) {
                    emptyState_->setText(QStringLiteral("Nothing matches this search."));
                } else if (openId == 0) {
                    emptyState_->setText(QStringLiteral(
                        "Nothing here yet.\nImport files, or drop them on the library."));
                } else {
                    emptyState_->setText(QStringLiteral("This folder is empty."));
                }
                emptyState_->show();
                layoutLibraryChrome();
            } else {
                emptyState_->hide();
            }
        }
        setFooterSummary(QStringLiteral("%1 items · %2")
            .arg(visible.size() + realFolders)
            .arg(compactSize(static_cast<std::uint64_t>(std::max<qlonglong>(0, total_bytes)))));
        watcher->deleteLater();
        paintGalleryBatch();
        offerPluginSearchRow();
        // The first layout after unlock could paint rows before their final
        // geometry was known, so the opening screen stayed wrong until the first
        // selection forced a repaint. Lay out and repaint once the batch lands.
        if (iconList_ != nullptr) {
            iconList_->doItemsLayout();
            iconList_->viewport()->update();
        }
        if (detailsTree_ != nullptr) {
            detailsTree_->doItemsLayout();
            detailsTree_->viewport()->update();
        }
    });
    watcher->setFuture(QtConcurrent::run([vault] {
        auto videos = vault->list_videos();
        if (!videos) {
            return std::vector<core::VideoInfo>{};
        }
        return videos.value();
    }));
}

std::int64_t MainWindow::selectedVideoId() const {
    // Same rule as selectedGalleryKind: the selection is the truth, the current
    // row only when it is part of it. Enter/F2 used to act on a stale row.
    if (galleryStack_->currentWidget() == detailsTree_) {
        auto* item = detailsTree_->currentItem();
        if (item == nullptr || !item->isSelected()) {
            const auto selected = detailsTree_->selectedItems();
            item = selected.isEmpty() ? nullptr : selected.first();
        }
        if (item == nullptr || item->data(0, Qt::UserRole + 12).toInt() != 0) {
            return -1;
        }
        return item->data(0, Qt::UserRole).toLongLong();
    }
    auto* item = iconList_->currentItem();
    if (item == nullptr || !item->isSelected()) {
        const auto selected = iconList_->selectedItems();
        item = selected.isEmpty() ? nullptr : selected.first();
    }
    if (item == nullptr || item->data(Qt::UserRole + 12).toInt() != 0) {
        return -1;
    }
    return item->data(Qt::UserRole).toLongLong();
}

void MainWindow::embedPlayer() {
    if (playerWindow_ == nullptr || contentStack_ == nullptr) {
        return;
    }
    playerWindow_->prepareDock();
    if (contentStack_->indexOf(playerWindow_) < 0) {
        contentStack_->addWidget(playerWindow_);
    }
    fadeStack(contentStack_, playerWindow_);
    playerWindow_->show();
    playerWindow_->setFocus();
}

void MainWindow::popPlayer() {
    if (playerWindow_ == nullptr || contentStack_ == nullptr) {
        return;
    }
    contentStack_->removeWidget(playerWindow_);
    fadeStack(contentStack_, pages_);
    playerWindow_->detach();
}

void MainWindow::embedPdf() {
    if (pdfWindow_ == nullptr || contentStack_ == nullptr) {
        return;
    }
    pdfWindow_->prepareDock();
    if (contentStack_->indexOf(pdfWindow_) < 0) {
        contentStack_->addWidget(pdfWindow_);
    }
    fadeStack(contentStack_, pdfWindow_);
    pdfWindow_->show();
    pdfWindow_->setFocus();
}

void MainWindow::popPdf() {
    if (pdfWindow_ == nullptr || contentStack_ == nullptr) {
        return;
    }
    contentStack_->removeWidget(pdfWindow_);
    if (pages_ != nullptr) {
        fadeStack(contentStack_, pages_);
    }
    pdfWindow_->detach();
}

void MainWindow::restoreGalleryFocus() {
    if (galleryStack_ == nullptr || contentStack_ == nullptr || pages_ == nullptr) {
        return;
    }
    if (contentStack_->currentWidget() != pages_) {
        return;
    }
    const bool details = galleryStack_->currentWidget() == detailsTree_;
    QAbstractItemView* view = details
        ? static_cast<QAbstractItemView*>(detailsTree_)
        : static_cast<QAbstractItemView*>(iconList_);
    if (view == nullptr) {
        return;
    }
    if (!view->currentIndex().isValid() && view->model() != nullptr
        && view->model()->rowCount() > 0) {
        view->setCurrentIndex(view->model()->index(0, 0));
    }
    view->setFocus(Qt::OtherFocusReason);
}

void MainWindow::beginPlayback(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked() || video_id < 0) {
        return;
    }
    QString title = QStringLiteral("Video");
    int index = 0;
    int count = 0;
    bool hasPrevious = false;
    bool hasNext = false;
    const bool details = galleryStack_ != nullptr && galleryStack_->currentWidget() == detailsTree_;
    const int items = details
        ? (detailsTree_ != nullptr ? detailsTree_->topLevelItemCount() : 0)
        : (iconList_ != nullptr ? iconList_->count() : 0);
    int found = -1;
    for (int i = 0; i < items; ++i) {
        const qlonglong id = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole).toLongLong()
            : iconList_->item(i)->data(Qt::UserRole).toLongLong();
        if (id == video_id) {
            found = i;
            title = details
                ? detailsTree_->topLevelItem(i)->text(0)
                : iconList_->item(i)->data(Qt::UserRole + 1).toString();
            break;
        }
    }
    count = items;
    if (found >= 0) {
        index = found + 1;
        hasPrevious = found > 0;
        hasNext = found + 1 < items;
    }
    if (title.isEmpty()) {
        title = QStringLiteral("Video");
    }
    FileIdentity identity;
    if (galleryCache_ != nullptr) {
        const auto known = galleryCache_->kinds.find(video_id);
        if (known != galleryCache_->kinds.end()) {
            identity = known->second;
        }
    }
    if (identity.label.isEmpty()) {
        identity = identityForVideo(vault_, video_id, title);
        if (galleryCache_ != nullptr) {
            galleryCache_->kinds[video_id] = identity;
        }
    }
    if (identity.route == 4) {
        beginText(video_id, title);
        return;
    }
    if (identity.route == 0) {
        showNotice(QStringLiteral("this format currently not supported"));
        return;
    }
    if (!closeText()) {
        return;
    }
    const bool stillImage = identity.route == 1 && (
        identity.label.compare(QStringLiteral("Jpeg"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Jpg"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Png"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Gif"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Bmp"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Webp"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Tif"), Qt::CaseInsensitive) == 0
        || identity.label.compare(QStringLiteral("Tiff"), Qt::CaseInsensitive) == 0);
    if (stillImage) {
        closePdf();
        auto applyClip = [&](PlayerWindow* window) {
            window->presentClip(title, index, count, hasPrevious, hasNext);
        };
        if (playerWindow_ != nullptr) {
            playerWindow_->playStill(vault_, video_id);
            applyClip(playerWindow_);
            if (playerWindow_->isDetached()) {
                playerWindow_->raise();
                playerWindow_->activateWindow();
            } else {
                embedPlayer();
            }
            return;
        }
        auto* window = new PlayerWindow(vault_, video_id, title, contentStack_, true);
        window->setAttribute(Qt::WA_DeleteOnClose);
        connect(window, &QDialog::destroyed, this,
            [this, window] {
                if (playerWindow_ == window) {
                    playerWindow_ = nullptr;
                }
                if (contentStack_ != nullptr
                    && ((pdfWindow_ != nullptr && contentStack_->currentWidget() == pdfWindow_)
                        || (textPage_ != nullptr && contentStack_->currentWidget() == textPage_)
                        || (peerBrowser_ != nullptr && contentStack_->currentWidget() == peerBrowser_))) {
                    return;
                }
                if (contentStack_ != nullptr && pages_ != nullptr) {
                    fadeStack(contentStack_, pages_);
                }
                QTimer::singleShot(0, this, [this] { restoreGalleryFocus(); });
            });
        connect(window, &PlayerWindow::detachRequested, this, [this] { popPlayer(); });
        connect(window, &PlayerWindow::dockRequested, this, [this] { embedPlayer(); });
        connect(window, &PlayerWindow::previousRequested, this, [this] { stepPlayback(-1); });
        connect(window, &PlayerWindow::nextRequested, this, [this] { stepPlayback(1); });
        playerWindow_ = window;
        applyClip(window);
        embedPlayer();
        return;
    }
    if (identity.route == 5) {
        // A record opens in the editor rather than a player, so previous/next
        // neighbours do not apply: records have no playback order.
        beginRecord(video_id, title);
        return;
    }
    if (identity.route == 2) {
        closePdf();
        beginMusic(video_id);
        return;
    }
    if (identity.route == 3) {
        beginPdf(video_id, title, hasPrevious, hasNext);
        return;
    }
    closePdf();
    if (musicWindow_ != nullptr) {
        musicWindow_->pauseForVideo();
    }

    auto applyClip = [&](PlayerWindow* window) {
        window->presentClip(title, index, count, hasPrevious, hasNext);
    };
    if (playerWindow_ != nullptr) {
        playerWindow_->playVideo(vault_, video_id);
        applyClip(playerWindow_);
        if (playerWindow_->isDetached()) {
            playerWindow_->raise();
            playerWindow_->activateWindow();
        } else {
            embedPlayer();
        }
        return;
    }
    auto* window = new PlayerWindow(vault_, video_id, title, contentStack_);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &QDialog::destroyed, this,
        [this, window] {
            if (playerWindow_ == window) {
                playerWindow_ = nullptr;
            }
            if (contentStack_ != nullptr
                && ((pdfWindow_ != nullptr && contentStack_->currentWidget() == pdfWindow_)
                    || (textPage_ != nullptr && contentStack_->currentWidget() == textPage_)
                    || (peerBrowser_ != nullptr && contentStack_->currentWidget() == peerBrowser_))) {
                return;
            }
            if (contentStack_ != nullptr && pages_ != nullptr) {
                fadeStack(contentStack_, pages_);
            }
            // The gallery was hidden while the player had the stack, so Qt
            // cannot hand focus back to it. Put it back on the selected item.
            QTimer::singleShot(0, this, [this] { restoreGalleryFocus(); });
        });
    connect(window, &PlayerWindow::detachRequested, this, [this] { popPlayer(); });
    connect(window, &PlayerWindow::dockRequested, this, [this] { embedPlayer(); });
    connect(window, &PlayerWindow::previousRequested, this, [this] { stepPlayback(-1); });
    connect(window, &PlayerWindow::nextRequested, this, [this] { stepPlayback(1); });
    connect(window, &PlayerWindow::playbackFinished, this, [this] { stepPlayback(1); });
    playerWindow_ = window;
    applyClip(window);
    embedPlayer();
}

void MainWindow::closePdf() {
    if (pdfWindow_ == nullptr) {
        return;
    }
    auto* dying = pdfWindow_;
    pdfWindow_ = nullptr;
    if (contentStack_ != nullptr) {
        const bool showing = contentStack_->currentWidget() == dying;
        contentStack_->removeWidget(dying);
        if (showing && pages_ != nullptr) {
            fadeStack(contentStack_, pages_);
        }
    }
    dying->close();
}

void MainWindow::beginPdf(
    const std::int64_t video_id,
    const QString& title,
    const bool previous,
    const bool next) {
    if (!vault_ || !vault_->is_unlocked() || video_id <= 0) {
        return;
    }
    if (playerWindow_ != nullptr) {
        auto* dying = playerWindow_;
        playerWindow_ = nullptr;
        if (contentStack_ != nullptr) {
            contentStack_->removeWidget(dying);
        }
        dying->close();
    }
    if (pdfWindow_ == nullptr) {
        auto* window = new PdfWindow(contentStack_);
        window->setAttribute(Qt::WA_DeleteOnClose);
        connect(window, &QObject::destroyed, this, [this, window] {
                if (pdfWindow_ == window) {
                pdfWindow_ = nullptr;
                if (contentStack_ != nullptr && pages_ != nullptr && playerWindow_ == nullptr) {
                    fadeStack(contentStack_, pages_);
                }
            }
            QTimer::singleShot(0, this, [this] { restoreGalleryFocus(); });
        });
        connect(window, &PdfWindow::closeRequested, this, [this] { closePdf(); });
        connect(window, &PdfWindow::previousRequested, this, [this] { stepPlayback(-1); });
        connect(window, &PdfWindow::nextRequested, this, [this] { stepPlayback(1); });
        connect(window, &PdfWindow::detachRequested, this, [this] { popPdf(); });
        connect(window, &PdfWindow::dockRequested, this, [this] { embedPdf(); });
        pdfWindow_ = window;
    }
    pdfWindow_->openDocument(vault_, video_id, title);
    pdfWindow_->setNeighbors(previous, next);
    if (pdfWindow_->isDetached()) {
        pdfWindow_->raise();
        pdfWindow_->activateWindow();
        return;
    }
    if (contentStack_ != nullptr && contentStack_->indexOf(pdfWindow_) < 0) {
        contentStack_->addWidget(pdfWindow_);
    }
    if (contentStack_ != nullptr) {
        fadeStack(contentStack_, pdfWindow_);
    }
    pdfWindow_->show();
    pdfWindow_->setFocus();
}

void MainWindow::beginMusic(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked() || video_id <= 0) {
        return;
    }
    closePdf();
    if (playerWindow_ != nullptr) {
        auto* dying = playerWindow_;
        playerWindow_ = nullptr;
        dying->close();
    }
    const bool details = galleryStack_ != nullptr && galleryStack_->currentWidget() == detailsTree_;
    const int items = details
        ? (detailsTree_ != nullptr ? detailsTree_->topLevelItemCount() : 0)
        : (iconList_ != nullptr ? iconList_->count() : 0);
    std::vector<MusicTrack> queue;
    int start = 0;
    for (int i = 0; i < items; ++i) {
        const int kind = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole + 12).toInt()
            : iconList_->item(i)->data(Qt::UserRole + 12).toInt();
        const qlonglong id = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole).toLongLong()
            : iconList_->item(i)->data(Qt::UserRole).toLongLong();
        const QString name = details
            ? detailsTree_->topLevelItem(i)->text(0)
            : iconList_->item(i)->data(Qt::UserRole + 1).toString();
        bool audio = isAudioFileName(name);
        if (!audio && galleryCache_ != nullptr) {
            const auto knownKind = galleryCache_->kinds.find(static_cast<std::int64_t>(id));
            audio = knownKind != galleryCache_->kinds.end() && knownKind->second.route == 2;
        }
        if (kind != 0 || id <= 0 || !audio) {
            continue;
        }
        if (id == video_id) {
            start = static_cast<int>(queue.size());
        }
        queue.push_back(MusicTrack{id, name});
    }
    if (queue.empty()) {
        queue.push_back(MusicTrack{video_id, QStringLiteral("Audio")});
    }
    if (musicWindow_ != nullptr) {
        const bool arriving = !musicWindow_->isVisible();
        musicWindow_->playQueue(vault_, queue, start);
        musicWindow_->show();
        if (arriving) {
            slideInBar(musicWindow_);
        }
        return;
    }
    auto* window = new MusicWindow(centralWidget());
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &QObject::destroyed, this, [this, window] {
        if (musicWindow_ == window) {
            musicWindow_ = nullptr;
        }
        QTimer::singleShot(0, this, [this] { restoreGalleryFocus(); });
    });
    if (auto* layout = qobject_cast<QVBoxLayout*>(centralWidget()->layout())) {
        layout->addWidget(window);
    }
    musicWindow_ = window;
    window->playQueue(vault_, queue, start);
    window->show();
    slideInBar(window);
}

void MainWindow::stepPlayback(const int delta) {
    const std::int64_t current = playerWindow_ != nullptr
        ? playerWindow_->videoId()
        : (pdfWindow_ != nullptr ? pdfWindow_->documentId() : -1);
    if (current < 0 || delta == 0) {
        return;
    }
    const bool details = galleryStack_ != nullptr && galleryStack_->currentWidget() == detailsTree_;
    const int items = details
        ? (detailsTree_ != nullptr ? detailsTree_->topLevelItemCount() : 0)
        : (iconList_ != nullptr ? iconList_->count() : 0);
    QVector<qlonglong> files;
    int found = -1;
    for (int i = 0; i < items; ++i) {
        const int kind = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole + 12).toInt()
            : iconList_->item(i)->data(Qt::UserRole + 12).toInt();
        const qlonglong id = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole).toLongLong()
            : iconList_->item(i)->data(Qt::UserRole).toLongLong();
        if (kind != 0 || id <= 0) {
            continue;
        }
        if (id == current) {
            found = files.size();
        }
        files.append(id);
    }
    if (files.isEmpty() || found < 0) {
        return;
    }
    const bool shuffle = playerWindow_ != nullptr && playerWindow_->shuffleOn();
    const bool repeatAll = playerWindow_ != nullptr && playerWindow_->repeatMode() == 1;
    int target = found;
    if (shuffle && files.size() > 1) {
        int pick = QRandomGenerator::global()->bounded(files.size() - 1);
        if (pick >= found) {
            ++pick;
        }
        target = pick;
    } else {
        target = found + (delta > 0 ? 1 : -1);
        if (target < 0 || target >= files.size()) {
            if (!repeatAll) {
                return;
            }
            target = delta > 0 ? 0 : files.size() - 1;
        }
    }
    const qlonglong nextId = files.at(target);
    if (nextId == current && playerWindow_ != nullptr) {
        playerWindow_->restartFromStart();
        return;
    }
    beginPlayback(nextId);
}

void MainWindow::beginRemoveSelected(const bool ask) {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    setError(galleryStatus_, {});
    const auto ids = selectedVideoIds();
    if (ids.empty()) {
        setError(galleryStatus_, QStringLiteral("Select one or more items to remove."));
        return;
    }
    if (ask) {
        InWindowSheet confirm(this);
        confirm.setTitle(QStringLiteral("Remove items"));
        confirm.setMessage(QStringLiteral("Remove %1 item(s) from the vault? This cannot be undone.")
            .arg(ids.size()));
        confirm.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
        confirm.addButton(QStringLiteral("Remove"), 1, true, QStringLiteral("trash"));
        if (confirm.run() != 1) {
            return;
        }
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Removing");
    const auto vault = vault_;
    const auto shared_ids = std::make_shared<std::vector<std::int64_t>>(ids);
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Removing…"), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    // Run on the batch worker thread; progress/completion arrive as queued
    // signals so the UI stays fully responsive during the removal.
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, ids = std::move(shared_ids)] {
            worker->removeVideos(ids, vault);
        },
        Qt::QueuedConnection);
}

void MainWindow::beginChangePassword() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});

    InWindowSheet dialog(this);
    dialog.setTitle(QStringLiteral("Change vault password"));
    auto* current = new QLineEdit();
    current->setEchoMode(QLineEdit::Password);
    current->setPlaceholderText(QStringLiteral("Current password"));
    auto* next = new QLineEdit();
    next->setEchoMode(QLineEdit::Password);
    next->setPlaceholderText(QStringLiteral("New password"));
    auto* confirmation = new QLineEdit();
    confirmation->setEchoMode(QLineEdit::Password);
    confirmation->setPlaceholderText(QStringLiteral("Confirm new password"));
    auto* profile = new QComboBox();
    profile->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    profile->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    dialog.body()->addWidget(current);
    dialog.body()->addWidget(next);
    dialog.body()->addWidget(confirmation);
    dialog.body()->addWidget(profile);
    dialog.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    dialog.addButton(QStringLiteral("Change"), 1, true);
    if (dialog.run() != 1) {
        return;
    }
    if (next->text() != confirmation->text()) {
        setError(galleryStatus_, QStringLiteral("The new password confirmation does not match."));
        return;
    }

    const auto vault = vault_;
    // Both passwords move into locked buffers and both fields are emptied
    // before the (slow) re-key starts.
    auto current_text = std::make_shared<core::Secret>();
    auto next_text = std::make_shared<core::Secret>();
    auto capturedCurrent = takeSecretFromLineEdit(current);
    auto capturedNext = takeSecretFromLineEdit(next);
    confirmation->clear();
    if (!capturedCurrent || !capturedNext) {
        setError(galleryStatus_, QStringLiteral(
            "A password could not be held in protected memory. Try a shorter one."));
        return;
    }
    *current_text = std::move(*capturedCurrent);
    *next_text = std::move(*capturedNext);
    const auto parameters = selectedParameters(profile->currentIndex());

    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    showStatus(QStringLiteral("Changing password…"), false);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] { finishChangePassword(); });
    adminWatcher_->setFuture(QtConcurrent::run([vault, current_text, next_text, parameters] {
        auto result = vault->change_password(*current_text, *next_text, parameters);
        if (result.has_value()) {
            // Re-seal the sync state under the new password but keep the package
            // key, so packages already on the server stay readable.
            const auto root = vault->root_path();
            // Same conversion as the settings page: u8string() is char8_t
            // under C++20 on GCC, so go through data()/size().
            const auto utf8 = root.u8string();
            const QString folder = QString::fromUtf8(
                reinterpret_cast<const char*>(utf8.data()), int(utf8.size()));
            (void)sync::resealForPasswordChange(sync::stateDirectoryFor(folder),
                                                *current_text, *next_text);
        }
        // The core wipes its own copies; these holders are wiped here on every
        // path (success, failure and the reseal exception path).
        current_text->wipe();
        next_text->wipe();
        return std::make_shared<core::Result<bool>>(std::move(result));
    }));
}

void MainWindow::finishChangePassword() {
    auto* completed = adminWatcher_;
    adminWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    showStatus(QStringLiteral("Password changed."), false);
}

void MainWindow::setBusy(const bool busy) {
    createButton_->setEnabled(!busy);
    unlockButton_->setEnabled(!busy);
    setupPage_->setEnabled(!busy);
    loginPage_->setEnabled(!busy);
    if (busy) {
        setCursor(Qt::WaitCursor);
    } else {
        unsetCursor();
    }
}

void MainWindow::showSetup() {
    setError(setupError_, {});
    fadeStack(pages_, setupPage_);
    setupPassword_->clear();
    setupConfirmation_->clear();
    setupLocation_->setFocus();
}

void MainWindow::showLogin(const std::filesystem::path& path) {
    setError(loginError_, {});
    if (!path.empty()) {
        loginLocation_->setText(textFromPath(path));
    }
    if (privacyBanner_ != nullptr) {
        // A privacy screen is not a lock. Say so, name who still has access,
        // and keep the way out one click away.
        privacyBanner_->setVisible(privacyScreenActive_);
        if (privacyScreenActive_) {
            const QString who = peerLinked() ? shareServer_->peer()->name()
                                             : QStringLiteral("the connected device");
            privacyBannerText_->setText(QStringLiteral(
                "Privacy screen: the vault is NOT locked. Its keys are still in "
                "memory and %1 can still read and copy this library. Lock fully "
                "to wipe them and stop sharing.").arg(who));
        }
    }
    fadeStack(pages_, loginPage_);
    loginPassword_->clear();
    loginPassword_->setFocus();
}

void MainWindow::showUnlocked() {
    if (pluginMusic_) pluginMusic_->setVault(vault_);
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    privacyScreenActive_ = false;
    if (privacyBanner_ != nullptr) {
        privacyBanner_->hide();
    }
    // Show only the vault's folder name. The full path used to sit here and
    // leaked the storage location in every screenshot and on a shared screen;
    // it stays available as a tooltip for the person at the machine.
    const auto rootText = textFromPath(vault_->root_path());
    const auto leaf = QDir(rootText).dirName();
    unlockedLocation_->setText(
        QStringLiteral("Vault: %1").arg(leaf.isEmpty() ? rootText : leaf));
    unlockedLocation_->setToolTip(rootText);
    fadeStack(pages_, unlockedPage_);
    resetAutoLock();
    tagFilterNames_.clear();
    {
        const QSignalBlocker blocker(tagFilterButton_);
        tagFilterButton_->setTags({});
    }
    searchText_.clear();
    searchEdit_->clear();
    refreshTagFilter();
    refreshGallery();
    flushExternalImports();
    // The first layout pass runs against the pre-show geometry and can inherit
    // the icon-mode item sizes, which left the opening screen wrong until the
    // user selected something. Re-run layout once the event loop has settled.
    QTimer::singleShot(0, this, [this] {
        layoutLibraryChrome();
        if (iconList_ != nullptr) {
            iconList_->doItemsLayout();
            iconList_->viewport()->update();
        }
        if (detailsTree_ != nullptr) {
            detailsTree_->doItemsLayout();
            detailsTree_->viewport()->update();
        }
        update();
    });
    configureUpdateChecks();
}

void MainWindow::ensureShareServer() {
    if (shareServer_) {
        return;
    }
    shareServer_ = std::make_unique<ShareServer>();
    shareServer_->setOnRunning([this](bool sharing) {
        if (shareButton_ != nullptr) {
            shareButton_->setToolTip(sharing
                ? QStringLiteral("Sharing…")
                : QStringLiteral("Share this vault on Wi-Fi over an encrypted connection. "
                                 "The phone must enter the vault password. Locking this PC does not stop sharing."));
            shareButton_->setProperty("sharing", sharing);
            shareButton_->style()->unpolish(shareButton_);
            shareButton_->style()->polish(shareButton_);
        }
    });
    shareServer_->setLinkApprover(this, [this](const QString& phone, std::function<void(bool)> answer) {
        approvePeer(phone, std::move(answer));
    });
    shareServer_->setOnPeerChanged([this] { peerChanged(); });
}

void MainWindow::approvePeer(const QString& phone, std::function<void(bool)> answer) {
    if (!vault_ || !vault_->is_unlocked()) {
        answer(false);
        return;
    }
    if (pendingPeerAnswer_) {
        answer(false);
        return;
    }
    pendingPeerAnswer_ = std::move(answer);
    auto* veil = new QWidget(this);
    veil->setObjectName(QStringLiteral("approvalVeil"));
    veil->setAttribute(Qt::WA_StyledBackground, true);
    veil->setStyleSheet(QStringLiteral(
        "QWidget#approvalVeil { background: rgba(0, 0, 0, 150); }"
        "QFrame#approvalCard { background: #161618; border-radius: 16px; }"
        "QLabel { color: white; background: transparent; }"
        "QLabel#approvalBody { color: #8e8e93; }"
        "QPushButton#approvalAllow { background: #2672c8; color: white; border: none;"
        "  border-radius: 12px; min-height: 36px; padding: 0 18px; font-weight: 600; }"
        "QPushButton#approvalDecline { background: transparent; color: #ff6b6b; border: none;"
        "  min-height: 36px; padding: 0 12px; }"));
    veil->setGeometry(rect());
    auto* outer = new QVBoxLayout(veil);
    outer->addStretch(1);
    auto* card = new QFrame(veil);
    card->setObjectName(QStringLiteral("approvalCard"));
    card->setFixedWidth(440);
    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(22, 18, 22, 18);
    column->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Accept connection"), card);
    title->setStyleSheet(QStringLiteral("font: 600 16px \"Segoe UI\";"));
    auto* body = new QLabel(QStringLiteral(
        "\"%1\" wants to swap libraries with this PC.\n\n"
        "The phone will see your library and can copy from it. Its library opens here, "
        "in this window. Nobody can change or remove the other's files.").arg(phone), card);
    body->setObjectName(QStringLiteral("approvalBody"));
    body->setWordWrap(true);
    auto* row = new QHBoxLayout();
    auto* decline = new QPushButton(QStringLiteral("Decline"), card);
    icons::withText(decline, QStringLiteral("x"));
    decline->setObjectName(QStringLiteral("approvalDecline"));
    decline->setCursor(Qt::PointingHandCursor);
    auto* allow = new QPushButton(QStringLiteral("Allow"), card);
    icons::withText(allow, QStringLiteral("check"));
    allow->setObjectName(QStringLiteral("approvalAllow"));
    allow->setCursor(Qt::PointingHandCursor);
    row->addStretch(1);
    row->addWidget(decline);
    row->addWidget(allow);
    column->addWidget(title);
    column->addWidget(body);
    column->addLayout(row);
    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch(1);
    connect(allow, &QPushButton::clicked, this, [this] { finishPeerApproval(true); });
    connect(decline, &QPushButton::clicked, this, [this] { finishPeerApproval(false); });
    QTimer::singleShot(55000, veil, [this] { finishPeerApproval(false); });
    approvalVeil_ = veil;
    veil->show();
    veil->raise();
    QApplication::alert(this);
}

void MainWindow::finishPeerApproval(const bool allow) {
    if (pendingPeerAnswer_) {
        auto answer = std::move(pendingPeerAnswer_);
        pendingPeerAnswer_ = nullptr;
        answer(allow);
    }
    if (approvalVeil_ != nullptr) {
        auto* veil = approvalVeil_;
        approvalVeil_ = nullptr;
        veil->deleteLater();
    }
}

bool MainWindow::peerLinked() const {
    return shareServer_ && shareServer_->peer()->linked();
}

void MainWindow::peerChanged() {
    const bool linked = peerLinked();
    if (!linked) {
        dismissPeerBrowser();
    }
    // The privacy screen was only hiding the library. With the phone gone there
    // is nothing left to share, so finish the lock instead of leaving the keys
    // in memory behind a lock screen nobody is using.
    if (!linked && privacyScreenActive_ && vault_ && vault_->is_unlocked()) {
        privacyScreenActive_ = false;
        if (privacyBanner_ != nullptr) {
            privacyBanner_->hide();
        }
        if (shareServer_) {
            shareServer_->setLocalVault(nullptr);
        }
        vault_->lock();
        vault_.reset();
        syncKeys_.wipe();
        return;
    }
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    if (currentFolderId_ == 0) {
        refreshGallery();
    }
    if (!batchBusy_) {
        setError(galleryStatus_, linked
            ? QStringLiteral("Swapped libraries with %1. Its library is the phone folder in Library.")
                  .arg(shareServer_->peer()->name())
            : QStringLiteral("The phone's library is no longer shared."));
        galleryStatus_->setVisible(true);
    }
}

void MainWindow::openPeerBrowser() {
    if (!peerLinked() || contentStack_ == nullptr) {
        setError(galleryStatus_, QStringLiteral("The phone is no longer connected."));
        galleryStatus_->setVisible(true);
        refreshGallery();
        return;
    }
    if (peerBrowser_ == nullptr) {
        peerBrowser_ = new PeerBrowser(shareServer_->peer(), contentStack_);
        peerBrowser_->setOnCopy([this](std::shared_ptr<PeerCopyPlan> plan) {
            beginCopyFromPeer(std::move(plan));
        });
        peerBrowser_->setOnUnlink([this] {
            if (shareServer_) {
                shareServer_->unlinkPeer();
            }
        });
        peerBrowser_->setOnBack([this] { closePeerBrowser(); });
        auto* opened = peerBrowser_;
        connect(opened, &QObject::destroyed, this, [this, opened] {
            if (peerBrowser_ == opened) {
                peerBrowser_ = nullptr;
            }
        });
    }
    if (contentStack_->indexOf(peerBrowser_) < 0) {
        contentStack_->addWidget(peerBrowser_);
    }
    fadeStack(contentStack_, peerBrowser_);
}

void MainWindow::closePeerBrowser() {
    if (peerBrowser_ == nullptr || contentStack_ == nullptr || pages_ == nullptr) {
        return;
    }
    if (contentStack_->currentWidget() == peerBrowser_) {
        fadeStack(contentStack_, pages_);
    }
}

void MainWindow::dismissPeerBrowser() {
    if (peerBrowser_ == nullptr) {
        return;
    }
    auto* closing = peerBrowser_;
    peerBrowser_ = nullptr;
    if (contentStack_ != nullptr) {
        const bool showing = contentStack_->currentWidget() == closing;
        contentStack_->removeWidget(closing);
        if (showing && pages_ != nullptr) {
            fadeStack(contentStack_, pages_);
        }
    }
    closing->close();
}

void MainWindow::beginCopyFromPeer(std::shared_ptr<PeerCopyPlan> plan) {
    if (!vault_ || !vault_->is_unlocked() || !peerLinked() || !plan) {
        return;
    }
    if (auto listed = vault_->list_folders(); listed) {
        folders_ = std::move(listed.value());
    }
    std::int64_t destination = currentFolderId_;
    if (!chooseFolder(QStringLiteral("Copy into"), {}, &destination)) {
        return;
    }
    plan->destination = destination;
    plan->stop = std::make_shared<std::atomic_bool>(false);
    PeerTransfer job;
    job.id = transferSerial_++;
    job.plan = std::move(plan);
    job.stop = job.plan->stop;
    const int files = static_cast<int>(job.plan->files.size());
    job.title = files == 1 && !job.plan->files.empty()
        ? job.plan->files.front().name
        : QStringLiteral("%1 from %2").arg(files).arg(job.plan->phone);
    job.state = PeerTransfer::Waiting;
    transfers_.push_back(std::move(job));
    if (transfersButton_ != nullptr) {
        transfersButton_->show();
    }
    showTransfers();
    pumpPeerCopies();
}

void MainWindow::pumpPeerCopies() {
    if (batchBusy_ || peerCopyActive_ || !vault_ || !peerLinked()) {
        refreshTransfers();
        return;
    }
    auto next = std::find_if(transfers_.begin(), transfers_.end(), [](const PeerTransfer& job) {
        return job.state == PeerTransfer::Waiting && job.plan;
    });
    if (next == transfers_.end()) {
        if (std::none_of(transfers_.begin(), transfers_.end(), [](const PeerTransfer& job) {
                return job.state == PeerTransfer::Running;
            })) {
            progressBar_->hide();
        }
        refreshTransfers();
        return;
    }
    if (next->stop && next->stop->load()) {
        next->state = PeerTransfer::Canceled;
        next->plan.reset();
        refreshTransfers();
        pumpPeerCopies();
        return;
    }
    next->state = PeerTransfer::Running;
    next->percent = 0;
    batchBusy_ = true;
    peerCopyActive_ = true;
    batchLabel_ = QStringLiteral("Copying");
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    showStatus(QStringLiteral("Copying %1…").arg(next->title), false);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    const auto vault = vault_;
    const auto peer = shareServer_->peer();
    const auto plan = next->plan;
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, plan, peer, vault] {
            worker->copyFromPeer(plan, peer, vault);
        },
        Qt::QueuedConnection);
    refreshTransfers();
}

void MainWindow::notePeerCopyFinished(const bool ok, const QString& message) {
    for (auto& job : transfers_) {
        if (job.state != PeerTransfer::Running) continue;
        const bool canceled = message == QStringLiteral("Canceled.");
        job.state = canceled ? PeerTransfer::Canceled : (ok ? PeerTransfer::Done : PeerTransfer::Failed);
        job.percent = ok && !canceled ? 100 : job.percent;
        job.plan.reset();
        break;
    }
    refreshTransfers();
}

void MainWindow::showTransfers() {
    if (transferPanel_ == nullptr) {
        auto* panel = new QFrame(this);
        panel->setObjectName(QStringLiteral("transferPanel"));
        panel->setStyleSheet(QStringLiteral(
            "QFrame#transferPanel { background: #101014; border: 1px solid #2c2c2e; border-radius: 16px; }"
            "QFrame#transferPanel QLabel { color: white; background: transparent; }"
            "QFrame#transferPanel QPushButton { color: #ff6b6b; background: transparent; border: none; }"
            "QFrame#transferPanel QPushButton#clearDone, QFrame#transferPanel QPushButton#transferClose { color: #2672c8; }"));
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(16, 14, 16, 14);
        auto* titleRow = new QHBoxLayout();
        auto* heading = new QLabel(QStringLiteral("Transfers"), panel);
        heading->setStyleSheet(QStringLiteral("font: 600 16px \"Segoe UI\";"));
        auto* close = new QPushButton(QStringLiteral("Close"), panel);
        icons::withText(close, QStringLiteral("x"));
        close->setObjectName(QStringLiteral("transferClose"));
        close->setCursor(Qt::PointingHandCursor);
        titleRow->addWidget(heading);
        titleRow->addStretch(1);
        titleRow->addWidget(close);
        layout->addLayout(titleRow);
        auto* hint = new QLabel(QStringLiteral(
            "Copies keep going while you use the library. Another one waits its turn."), panel);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color: #8e8e93;"));
        layout->addWidget(hint);
        transferHost_ = new QWidget(panel);
        auto* list = new QVBoxLayout(transferHost_);
        list->setContentsMargins(0, 0, 0, 0);
        list->setSpacing(8);
        layout->addWidget(transferHost_, 1);
        auto* clear = new QPushButton(QStringLiteral("Clear finished"), panel);
        icons::withText(clear, QStringLiteral("trash"));
        clear->setObjectName(QStringLiteral("clearDone"));
        clear->setCursor(Qt::PointingHandCursor);
        connect(clear, &QPushButton::clicked, this, [this] {
            transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(), [](const PeerTransfer& job) {
                return job.state != PeerTransfer::Waiting && job.state != PeerTransfer::Running;
            }), transfers_.end());
            if (transfers_.empty() && transfersButton_ != nullptr) {
                transfersButton_->hide();
            }
            refreshTransfers();
        });
        connect(close, &QPushButton::clicked, panel, &QWidget::hide);
        layout->addWidget(clear, 0, Qt::AlignRight);
        transferPanel_ = panel;
    }
    refreshTransfers();
    transferPanel_->show();
    placeTransferPanel();
}

void MainWindow::placeTransferPanel() {
    if (transferPanel_ == nullptr || !transferPanel_->isVisible()) {
        return;
    }
    const int panelWidth = std::min(400, std::max(280, width() - 40));
    const int panelHeight = std::min(560, std::max(220, height() - 96));
    transferPanel_->setGeometry(width() - panelWidth - 16, 56, panelWidth, panelHeight);
    transferPanel_->raise();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    placeTransferPanel();
    if (approvalVeil_ != nullptr) {
        approvalVeil_->setGeometry(rect());
        approvalVeil_->raise();
    }
    syncPluginRowButtons();
}

void MainWindow::beginNewRecordFromTemplate() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    // The built-ins are resources and cost nothing. The user's templates are ordinary
    // items whose head carries the template flag, so the scan reads 64 bytes per item and
    // decrypts in full only the few that are templates.
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<std::shared_ptr<QVector<QPair<QString, RecordEntry>>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const auto found = watcher->result();
        watcher->deleteLater();
        showTemplateChooser(*found);
    });
    watcher->setFuture(QtConcurrent::run([vault] {
        auto choices = std::make_shared<QVector<QPair<QString, RecordEntry>>>();
        for (const auto& document : builtinTemplates()) {
            for (const auto& entry : document.entries) {
                choices->push_back({document.templateName.isEmpty() ? entry.title
                                                                    : document.templateName,
                    entry});
            }
        }
        const auto listed = vault->list_videos();
        if (!listed) {
            return choices;
        }
        for (const auto& item : listed.value()) {
            const auto head = vault->read_video_range(item.id, 0, 64);
            if (!head || head.value().empty()) {
                continue;
            }
            const QByteArray peek(reinterpret_cast<const char*>(head.value().data()),
                static_cast<int>(head.value().size()));
            if (!looksLikeTemplate(peek)) {
                continue;
            }
            const auto full = vault->read_video_bytes(item.id);
            if (!full) {
                continue;
            }
            RecordDocument document;
            const QByteArray bytes(reinterpret_cast<const char*>(full.value().data()),
                static_cast<int>(full.value().size()));
            if (!parseRecord(bytes, &document) || !document.isTemplate) {
                continue;
            }
            for (const auto& entry : document.entries) {
                choices->push_back({document.templateName.isEmpty() ? entry.title
                                                                    : document.templateName,
                    entry});
            }
        }
        return choices;
    }));
}

void MainWindow::showTemplateChooser(const QVector<QPair<QString, RecordEntry>>& choices) {
    if (choices.isEmpty()) {
        showStatus(QStringLiteral("No templates yet. Save a record as a template to get one."), true);
        return;
    }
    CommandPopup menu(this);
    for (int index = 0; index < choices.size(); ++index) {
        menu.addCommand(choices.at(index).first, index + 1);
    }
    const QPoint origin = importButton_->mapToGlobal(QPoint(0, importButton_->height()));
    placePopup(&menu, origin);
    const int picked = menu.exec();
    if (picked < 1 || picked > choices.size()) {
        return;
    }

    // The name the user gives here is the record's title, not the template's.
    const RecordEntry& templateEntry = choices.at(picked - 1).second;
    InWindowSheet sheet(this);
    sheet.setTitle(QStringLiteral("New from template"));
    sheet.setMessage(QStringLiteral("Name this record. The fields and their values are "
                                    "ready to edit."));
    auto* entry = sheet.addTextEntry(QLineEdit::Normal, templateEntry.title,
        QStringLiteral("Title"));
    sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    sheet.addButton(QStringLiteral("Create"), 1, true, QStringLiteral("plus"));
    if (sheet.run() != 1) {
        return;
    }
    const QString title = entry->text().trimmed();
    if (title.isEmpty()) {
        showStatus(QStringLiteral("A record needs a title."), true);
        return;
    }
    createRecordItem(title, serializeRecord(cloneTemplate(templateEntry, title)));
}

void MainWindow::beginNewNote() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_ || contentStack_ == nullptr) {
        return;
    }
    if (textPage_ != nullptr && contentStack_->currentWidget() == textPage_) {
        if (!closeText()) {
            return;
        }
    }
    InWindowSheet prompt(this);
    prompt.setTitle(QStringLiteral("New note"));
    prompt.setMessage(QStringLiteral("Name"));
    prompt.addTextEntry(QLineEdit::Normal, QStringLiteral("Note.txt"), QStringLiteral("Name"));
    prompt.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
    prompt.addButton(QStringLiteral("Create"), 1, true, QStringLiteral("plus"));
    if (prompt.run() != 1) {
        return;
    }
    const QString entered = prompt.text();
    const QString name = noteFileName(entered);
    closePdf();
    if (playerWindow_ != nullptr) {
        auto* dying = playerWindow_;
        playerWindow_ = nullptr;
        contentStack_->removeWidget(dying);
        dying->close();
    }
    auto* page = static_cast<TextPage*>(textPage_);
    if (page == nullptr) {
        page = new TextPage(contentStack_);
        connect(page->backButton(), &QPushButton::clicked, this, [this] { closeText(); });
        connect(page->saveButton(), &QPushButton::clicked, this, [this] { saveOpenText(false); });
        textPage_ = page;
    }
    openTextId_ = 0;
    openTextFolderId_ = currentFolderId_;
    openTextTitle_ = name;
    openTextTruncated_ = false;
    openTextBusy_ = false;
    closeTextAfterSave_ = false;
    page->showDocument(name, QString(), true);
    if (contentStack_->indexOf(page) < 0) {
        contentStack_->addWidget(page);
    }
    fadeStack(contentStack_, page);
    page->setFocus();
}

void MainWindow::beginText(const std::int64_t video_id, const QString& title) {
    if (!vault_ || !vault_->is_unlocked() || video_id <= 0 || contentStack_ == nullptr) {
        return;
    }
    auto* page = static_cast<TextPage*>(textPage_);
    if (page != nullptr && contentStack_->currentWidget() == page && openTextId_ == video_id) {
        return;
    }
    if (page != nullptr && contentStack_->currentWidget() == page) {
        if (!closeText()) {
            return;
        }
    }
    closePdf();
    if (playerWindow_ != nullptr) {
        auto* dying = playerWindow_;
        playerWindow_ = nullptr;
        contentStack_->removeWidget(dying);
        dying->close();
    }
    if (page == nullptr) {
        page = new TextPage(contentStack_);
        connect(page->backButton(), &QPushButton::clicked, this, [this] { closeText(); });
        connect(page->saveButton(), &QPushButton::clicked, this, [this] { saveOpenText(false); });
        textPage_ = page;
    }
    const auto folderId = currentFolderId_;
    openTextId_ = video_id;
    openTextFolderId_ = folderId;
    openTextTitle_ = title;
    openTextTruncated_ = false;
    openTextBusy_ = true;
    closeTextAfterSave_ = false;
    page->showDocument(title, QString(), false);
    if (contentStack_->indexOf(page) < 0) {
        contentStack_->addWidget(page);
    }
    fadeStack(contentStack_, page);
    const auto vault = vault_;
    const int generation = page->bump();
    auto* watcher = new QFutureWatcher<LoadedText>(page);
    connect(watcher, &QFutureWatcherBase::finished, page, [this, page, watcher, title, generation, video_id, folderId] {
        const LoadedText loaded = watcher->result();
        watcher->deleteLater();
        if (page->generation() != generation) {
            return;
        }
        openTextId_ = video_id;
        openTextFolderId_ = folderId;
        openTextTitle_ = title;
        openTextTruncated_ = loaded.truncated;
        openTextBusy_ = false;
        QString body = loaded.body;
        if (loaded.truncated) {
            body += QStringLiteral("\n\n— This file is longer than the editor can change. —");
        }
        page->showDocument(title, body, !loaded.truncated);
    });
    watcher->setFuture(QtConcurrent::run([vault, video_id] {
        constexpr std::uint64_t cap = 2U * 1024U * 1024U;
        LoadedText loaded;
        QByteArray data;
        std::uint64_t offset = 0U;
        bool read_failed = false;
        while (static_cast<std::uint64_t>(data.size()) < cap) {
            const auto room = cap - static_cast<std::uint64_t>(data.size());
            const auto want = std::min<std::uint64_t>(256U * 1024U, room);
            auto chunk = vault->read_video_range(video_id, offset, static_cast<std::size_t>(want));
            if (!chunk) {
                read_failed = true;
                break;
            }
            if (chunk.value().empty()) {
                break;
            }
            const auto& bytes = chunk.value();
            const auto take = std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(room));
            data.append(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(take));
            offset += take;
            if (take < bytes.size()) {
                break;
            }
        }
        // Neither stopping at the cap nor a short read proves the end of the
        // file: a file of exactly the cap leaves nothing more to read, while a
        // longer one can only be recognised by looking. One byte past what was
        // read decides it, because treating a partial read as complete would let
        // Save write back a shortened copy. A read that failed leaves the length
        // unknown, which is equally unsafe to edit.
        if (read_failed) {
            loaded.truncated = true;
        } else {
            auto probe = vault->read_video_range(video_id, offset, 1U);
            if (!probe || !probe.value().empty()) {
                loaded.truncated = true;
            }
        }
        loaded.body = QString::fromUtf8(data);
        if (loaded.body.startsWith(QChar(0xFEFF))) {
            loaded.body.remove(0, 1);
        }
        return loaded;
    }));
}

bool MainWindow::closeText(const bool force) {
    if (textPage_ == nullptr || contentStack_ == nullptr) {
        return true;
    }
    if (contentStack_->currentWidget() != textPage_) {
        return true;
    }
    auto* page = static_cast<TextPage*>(textPage_);
    if (!force && openTextBusy_ && page != nullptr && page->dirty()) {
        return false;
    }
    if (!force && page != nullptr && !openTextTruncated_ && page->dirty()) {
        InWindowSheet prompt(this);
        prompt.setTitle(QStringLiteral("Save note"));
        prompt.setMessage(QStringLiteral("Save changes to %1?").arg(openTextTitle_));
        prompt.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
        prompt.addButton(QStringLiteral("Discard"), 2, false, QStringLiteral("undo-2"));
        prompt.addButton(QStringLiteral("Save"), 1, true, QStringLiteral("check"));
        const int answer = prompt.run();
        if (answer == 0) {
            return false;
        }
        if (answer == 1) {
            saveOpenText(true);
            return false;
        }
    }
    if (page != nullptr) {
        page->bump();
    }
    openTextId_ = 0;
    openTextTitle_.clear();
    openTextTruncated_ = false;
    openTextBusy_ = false;
    closeTextAfterSave_ = false;
    if (pages_ != nullptr) {
        fadeStack(contentStack_, pages_);
    }
    return true;
}

void MainWindow::saveOpenText(const bool closeAfter) {
    auto* page = static_cast<TextPage*>(textPage_);
    if (page == nullptr || !vault_ || !vault_->is_unlocked() || openTextTruncated_ || openTextBusy_) {
        return;
    }
    const QString body = page->documentText();
    const QByteArray utf8 = body.toUtf8();
    const QString title = openTextTitle_.isEmpty() ? QStringLiteral("Note.txt") : openTextTitle_;
    const std::int64_t previous = openTextId_;
    const std::int64_t folderId = openTextFolderId_;
    // A note that is already in the vault keeps the name it has; only a new one
    // is sanitized into a safe name.
    const QString safeTitle = previous > 0 ? title : noteFileName(title);
    const int generation = page->generation();
    closeTextAfterSave_ = closeAfter;
    openTextBusy_ = true;
    page->setBusy(true);
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<TextSaveOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, page, generation, previous, folderId, title] {
        const TextSaveOutcome outcome = watcher->result();
        watcher->deleteLater();
        const bool current = page->generation() == generation;
        if (current) {
            openTextBusy_ = false;
            page->setBusy(false);
        }
        if (!outcome.ok) {
            closeTextAfterSave_ = false;
            lockAfterTextSave_ = false;
            if (current) {
                setError(galleryStatus_, outcome.error.isEmpty()
                    ? QStringLiteral("The note could not be saved.")
                    : outcome.error);
                galleryStatus_->setVisible(true);
            }
            resetAutoLock();
            return;
        }
        const bool collapsed = outcome.already && outcome.id != previous;
        if (current) {
            page->markSaved();
            if (collapsed) {
                openTextId_ = 0;
            } else {
                openTextId_ = outcome.id;
                openTextFolderId_ = folderId;
                openTextTitle_ = title;
            }
        }
        if (previous > 0 && outcome.id == previous) {
            // Edited in place: the row is already listed, but the size it shows
            // and the library total are now stale.
            refreshGallery();
        } else if (!collapsed && findListItemById(iconList_, outcome.id) == nullptr) {
            revealVideo(
                outcome.id,
                title,
                outcome.size,
                folderId,
                QDateTime::currentSecsSinceEpoch());
        } else {
            recountLibrary();
        }
        showStatus(collapsed
            ? QStringLiteral("That text is already in the library.")
            : QStringLiteral("Saved"), false);
        if (closeTextAfterSave_) {
            closeTextAfterSave_ = false;
            closeText(true);
        }
        if (lockAfterTextSave_) {
            lockAfterTextSave_ = false;
            lockVault();
        }
    });
    watcher->setFuture(QtConcurrent::run([vault, safeTitle, utf8, previous, folderId] {
        TextSaveOutcome outcome;
        try {
            // The content goes to the core in memory. A note is never written to
            // disk in the clear, not even for a moment in the system temp folder.
            const std::span<const unsigned char> content(
                reinterpret_cast<const unsigned char*>(utf8.constData()),
                static_cast<std::size_t>(utf8.size()));
            const std::string name = safeTitle.toStdString();
            if (previous > 0) {
                // Editing the note that is open: rewrite it in place, so the
                // item keeps its id, folder, tags and place in the library
                // instead of being replaced by a copy of itself.
                const auto replaced = vault->replace_video_content(previous, content, name);
                if (!replaced) {
                    outcome.error = vaultErrorText(replaced.error());
                    return outcome;
                }
                outcome.ok = true;
                outcome.id = previous;
                outcome.size = utf8.size();
                return outcome;
            }
            bool already = false;
            auto imported = vault->import_bytes(content, name, folderId, &already);
            if (!imported) {
                outcome.error = vaultErrorText(imported.error());
                return outcome;
            }
            outcome.ok = true;
            outcome.already = already;
            outcome.id = imported.value();
            outcome.size = utf8.size();
        } catch (const std::exception&) {
            outcome = {};
            outcome.error = QStringLiteral("The note could not be saved.");
        }
        return outcome;
    }));
}

void MainWindow::refreshTransfers() {
    if (transferHost_ == nullptr) return;
    auto* layout = qobject_cast<QVBoxLayout*>(transferHost_->layout());
    if (layout == nullptr) return;
    while (QLayoutItem* child = layout->takeAt(0)) {
        if (child->widget() != nullptr) child->widget()->deleteLater();
        delete child;
    }
    if (transfers_.empty()) {
        auto* empty = new QLabel(QStringLiteral("Nothing moving."), transferHost_);
        empty->setStyleSheet(QStringLiteral("color: #8e8e93;"));
        layout->addWidget(empty);
        layout->addStretch(1);
        return;
    }
    for (int index = static_cast<int>(transfers_.size()) - 1; index >= 0; --index) {
        const auto& job = transfers_[static_cast<std::size_t>(index)];
        auto* row = new QWidget(transferHost_);
        row->setStyleSheet(QStringLiteral("background: #161618; border-radius: 10px;"));
        auto* box = new QVBoxLayout(row);
        auto* line = new QHBoxLayout();
        auto* title = new QLabel(job.title, row);
        title->setStyleSheet(QStringLiteral("font-weight: 600;"));
        line->addWidget(title, 1);
        const bool alive = job.state == PeerTransfer::Waiting || job.state == PeerTransfer::Running;
        if (alive) {
            auto* cancel = new QPushButton(QStringLiteral("Cancel"), row);
            icons::withText(cancel, QStringLiteral("x"));
            const auto id = job.id;
            connect(cancel, &QPushButton::clicked, this, [this, id] {
                for (auto& item : transfers_) {
                    if (item.id != id) continue;
                    if (item.stop) item.stop->store(true);
                    if (item.state == PeerTransfer::Waiting) {
                        item.state = PeerTransfer::Canceled;
                        item.plan.reset();
                    }
                    break;
                }
                refreshTransfers();
                pumpPeerCopies();
            });
            line->addWidget(cancel);
        } else {
            const QString word = job.state == PeerTransfer::Done ? QStringLiteral("Done")
                : job.state == PeerTransfer::Canceled ? QStringLiteral("Canceled")
                : QStringLiteral("Stopped");
            auto* mark = new QLabel(word, row);
            mark->setStyleSheet(QStringLiteral("color: #8e8e93;"));
            line->addWidget(mark);
        }
        box->addLayout(line);
        if (job.state == PeerTransfer::Running) {
            auto* bar = new QProgressBar(row);
            bar->setRange(0, 100);
            bar->setValue(job.percent);
            bar->setTextVisible(false);
            bar->setFixedHeight(6);
            box->addWidget(bar);
            auto* pct = new QLabel(QStringLiteral("%1%").arg(job.percent), row);
            pct->setStyleSheet(QStringLiteral("color: #8e8e93;"));
            box->addWidget(pct);
        } else if (job.state == PeerTransfer::Waiting) {
            auto* wait = new QLabel(QStringLiteral("Waiting"), row);
            wait->setStyleSheet(QStringLiteral("color: #8e8e93;"));
            box->addWidget(wait);
        }
        layout->addWidget(row);
    }
    layout->addStretch(1);
}

void MainWindow::openShare() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    ensureShareServer();
    shareServer_->setLocalVault(vault_);
    if (!shareServer_->running()) {
        QString error;
        if (!shareServer_->start(vault_->root_path(), &error)) {
            InWindowSheet notice(this);
            notice.setTitle(QStringLiteral("Share with phone"));
            notice.setMessage(error, true);
            notice.addButton(QStringLiteral("OK"), 1, true, QStringLiteral("check"));
            notice.run();
            return;
        }
    }
    shareServer_->showDialog(this);
}

void MainWindow::lockVault() {
    autoLockTimer_->stop();
    if (!closeText()) {
        if (closeTextAfterSave_) {
            lockAfterTextSave_ = true;
        } else {
            resetAutoLock();
        }
        return;
    }

    // The ordinary Lock action is a HARD lock: sharing stops, the SQLCipher
    // connection closes, the master key is wiped and every reader that outlived
    // the vault mutex is revoked. Only when this PC is actually sharing does
    // the user get a choice, and then the alternative is labelled as what it
    // is — a screen-only lock that keeps the keys alive.
    const bool interactive = interactiveLock_;
    interactiveLock_ = true;
    bool keepSharing = privacyScreenActive_ && interactive;
    if (peerLinked() && interactive) {
        InWindowSheet sheet(this);
        sheet.setTitle(QStringLiteral("Lock this PC"));
        sheet.setMessage(QStringLiteral(
            "A phone is still connected.\n\n"
            "Locking wipes the vault keys from memory and stops sharing. "
            "Hiding the screen keeps the vault UNLOCKED in this program so the "
            "phone can keep reading and copying it."));
        sheet.addButton(QStringLiteral("Lock and stop sharing"), 1, true, QStringLiteral("lock"));
        sheet.addButton(QStringLiteral("Hide screen, keep sharing"), 2, false, QStringLiteral("wifi"));
        sheet.addButton(QStringLiteral("Cancel"), 0, false, QStringLiteral("x"));
        const int choice = sheet.run();
        if (choice == 0) {
            resetAutoLock(); // Nothing happened: the vault stays as it was.
            return;
        }
        keepSharing = choice == 2;
    }

    if (settingsPage_) {
        auto* page = settingsPage_;
        settingsPage_ = nullptr;
        contentStack_->setCurrentWidget(pages_);
        contentStack_->removeWidget(page);
        delete page;
    }
    closeSync(); // A hidden-but-shared vault must not keep desktop sync running.
    if (pluginMusic_) { pluginMusic_->setVault({}); contentStack_->setCurrentWidget(pages_); }
    if (extensionTools_) { extensionTools_->stopAndClear(); contentStack_->setCurrentWidget(pages_); }
    syncKeys_.wipe(); // The derived sync keys die with the unlock.
    finishPeerApproval(false);
    dismissPeerBrowser();
    std::filesystem::path root;
    if (transferPanel_ != nullptr) {
        transferPanel_->hide();
    }
    if (playerWindow_ != nullptr) {
        auto* dying = playerWindow_;
        playerWindow_ = nullptr;
        dying->close();
    }
    if (musicWindow_ != nullptr) {
        auto* dying = musicWindow_;
        musicWindow_ = nullptr;
        dying->close();
    }
    // A record window shows decrypted values, so it must not outlive the vault.
    if (recordWindow_ != nullptr) {
        auto* dying = recordWindow_;
        recordWindow_ = nullptr;
        dying->close();
    }

    // File ids start over in every vault. Thumbnails, durations, and file
    // types kept from the one being locked would be drawn on the next one.
    if (galleryCache_ != nullptr) {
        galleryCache_->thumbs.clear();
        galleryCache_->thumbOrder.clear();
        galleryCache_->iconRows.clear();
        galleryCache_->treeRows.clear();
        galleryCache_->media.clear();
        galleryCache_->kinds.clear();
        galleryCache_->pdfThumbs.clear();
    }
    pendingVideos_.clear();
    galleryQueue_.clear();
    galleryQueueIndex_ = 0;
    decoratedIds_.clear();
    galleryListing_ = false;
    metadataRequested_.clear();
    cutItems_.clear();
    dragPayload_.clear();
    dragPayloadValid_ = false;
    pendingSelection_.clear();
    ++galleryGeneration_;
    if (folderPane_ != nullptr) {
        folderPane_->hide();
    }
    if (vault_) {
        root = vault_->root_path();
    }
    importButton_->setEnabled(true);
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
    detailsTree_->clear();
    iconList_->clear();
    tagFilterNames_.clear();
    {
        const QSignalBlocker blocker(tagFilterButton_);
        tagFilterButton_->setTags({});
    }
    searchText_.clear();
    currentFolderId_ = 0;
    folders_.clear();

    if (keepSharing && vault_ && vault_->is_unlocked()) {
        enterPrivacyScreen(root);
        return;
    }

    // Hard lock. Stopping the share server first is what actually revokes the
    // phone's access: its per-connection vault handle is dropped and the
    // session token and link key are wiped before the keys go away.
    privacyScreenActive_ = false;
    if (shareServer_) {
        shareServer_->setLocalVault(nullptr);
        shareServer_->stop();
    }
    if (vault_) {
        vault_->lock();
        vault_.reset();
    }
    showLogin(root);
}

void MainWindow::enterPrivacyScreen(const std::filesystem::path& root) {
    // Deliberately does NOT touch the vault: sharing keeps working because the
    // vault stays unlocked. The banner on the login page says so.
    privacyScreenActive_ = true;
    showLogin(root);
}

void MainWindow::resetAutoLock() {
    if (autoLockTimer_ == nullptr) {
        return;
    }
    const int interval = autoLockMilliseconds();
    if (!vault_ || !vault_->is_unlocked() || interval <= 0) {
        autoLockTimer_->stop();
        return;
    }
    autoLockTimer_->setInterval(interval);
    autoLockTimer_->start();
}

void MainWindow::setError(QLabel* label, const QString& message) {
    label->setText(message);
    label->setVisible(!message.isEmpty());
    if (label == galleryStatus_) {
        label->setProperty("problem", !message.isEmpty());
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
}

void MainWindow::showNotice(const QString& message) {
    if (notice_ == nullptr) {
        notice_ = new QLabel(this);
        notice_->setAlignment(Qt::AlignCenter);
        notice_->setStyleSheet(QStringLiteral(
            "QLabel { background-color: #c62828; color: white; border-radius: 8px;"
            " padding: 8px 14px; font: 600 12px \"Segoe UI\"; }"));
        notice_->setAttribute(Qt::WA_TransparentForMouseEvents);
        notice_->hide();
    }
    if (noticeTimer_ == nullptr) {
        noticeTimer_ = new QTimer(this);
        noticeTimer_->setSingleShot(true);
        connect(noticeTimer_, &QTimer::timeout, this, [this] {
            auto* effect = qobject_cast<QGraphicsOpacityEffect*>(notice_->graphicsEffect());
            if (effect == nullptr) {
                notice_->hide();
                return;
            }
            const int generation = notice_->property("noticeGen").toInt();
            auto* fade = new QPropertyAnimation(effect, "opacity", notice_);
            fade->setDuration(180);
            fade->setStartValue(effect->opacity());
            fade->setEndValue(0.0);
            fade->setEasingCurve(QEasingCurve::InCubic);
            connect(fade, &QPropertyAnimation::finished, notice_, [this, generation] {
                if (notice_->property("noticeGen").toInt() == generation) {
                    notice_->hide();
                }
            });
            fade->start(QAbstractAnimation::DeleteWhenStopped);
        });
    }
    notice_->setText(message);
    notice_->adjustSize();
    const int boxWidth = std::min(notice_->sizeHint().width(), std::max(160, this->width() - 48));
    const int boxHeight = std::max(notice_->sizeHint().height(), 32);
    const QPoint end((this->width() - boxWidth) / 2, this->height() - boxHeight - 84);
    notice_->setGeometry(QRect(end + QPoint(0, 16), QSize(boxWidth, boxHeight)));
    notice_->raise();
    notice_->show();
    auto* effect = new QGraphicsOpacityEffect(notice_);
    effect->setOpacity(0.0);
    notice_->setGraphicsEffect(effect);
    const int generation = notice_->property("noticeGen").toInt() + 1;
    notice_->setProperty("noticeGen", generation);
    auto* fade = new QPropertyAnimation(effect, "opacity", notice_);
    fade->setDuration(180);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
    auto* slide = new QPropertyAnimation(notice_, "pos", notice_);
    slide->setDuration(220);
    slide->setStartValue(end + QPoint(0, 16));
    slide->setEndValue(end);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
    noticeTimer_->start(2600);
}

void MainWindow::showStatus(const QString& message, const bool problem) {
    if (galleryStatus_ == nullptr) {
        return;
    }
    galleryStatus_->setText(message);
    galleryStatus_->setVisible(!message.isEmpty());
    galleryStatus_->setProperty("problem", problem);
    galleryStatus_->style()->unpolish(galleryStatus_);
    galleryStatus_->style()->polish(galleryStatus_);
}

} // namespace videovault::app
