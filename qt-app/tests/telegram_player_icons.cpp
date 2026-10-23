#include "videovault/app/telegram_player_icons.hpp"

#include <QApplication>
#include <QDirIterator>
#include <QImage>
#include <QPainter>
#include <QSet>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void checkPixels(const QImage& image, const QColor& color) {
    bool transparent = false;
    bool opaque = false;
    bool antialiased = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            transparent = transparent || pixel.alpha() == 0;
            opaque = opaque || pixel.alpha() == color.alpha();
            antialiased = antialiased || (pixel.alpha() > 0 && pixel.alpha() < color.alpha());
            if (pixel.alpha() == color.alpha()) {
                require(pixel.red() == color.red() && pixel.green() == color.green()
                    && pixel.blue() == color.blue(), "opaque mask pixel lost the requested tint");
            }
        }
    }
    require(transparent && opaque && antialiased,
        "mask must retain transparent background, solid glyph and antialiased edge");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        QDirIterator it(QStringLiteral(":/telegram/player"), QStringList{QStringLiteral("*.png")},
            QDir::Files, QDirIterator::Subdirectories);
        QSet<QString> names;
        int count = 0;
        while (it.hasNext()) {
            const QString path = it.next();
            const QImage image(path);
            require(!image.isNull(), "bundled Telegram mask could not be loaded");
            require(image.isGrayscale(), "Telegram coverage mask contains non-grayscale pixels");
            QString name = path.mid(QStringLiteral(":/telegram/player/").size());
            name.chop(4);
            if (!name.contains(QLatin1Char('@'))) names.insert(name);
            ++count;
        }
        require(count == 117, "Telegram mask resource inventory is incomplete");
        for (const auto& name : names) {
            require(!videovault::app::telegram::icon(name, Qt::white).isNull(),
                "a bundled mask has no loadable icon");
        }
        for (int scale = 1; scale <= 3; ++scale) {
            const auto pixmap = videovault::app::telegram::mask(
                QStringLiteral("player_play"), QColor(30, 120, 210), scale);
            require(pixmap.size() == QSize(24 * scale, 24 * scale), "incorrect high-DPI mask size");
            require(pixmap.devicePixelRatio() == scale, "incorrect mask device pixel ratio");
            checkPixels(pixmap.toImage(), QColor(30, 120, 210));
        }
        checkPixels(videovault::app::telegram::mask(
            QStringLiteral("player_play"), QColor(255, 0, 0, 128), 2).toImage(), QColor(255, 0, 0, 128));
        require(videovault::app::telegram::mask(QStringLiteral("../player_play"), Qt::white, 1).isNull(),
            "mask lookup accepted traversal");
        require(videovault::app::telegram::icon(QStringLiteral("does_not_exist"), Qt::white).isNull(),
            "missing asset silently reported success");
        QImage target(48, 48, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::transparent);
        target.setDevicePixelRatio(2);
        QPainter painter(&target);
        require(videovault::app::telegram::paint(painter, QRectF(0, 0, 24, 24),
            QStringLiteral("player_play"), QColor(30, 120, 210)), "high-DPI paint failed");
        painter.end();
        checkPixels(target, QColor(30, 120, 210));
        std::cout << "Verified " << count << " original masks; tint, alpha, high-DPI and paint checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
