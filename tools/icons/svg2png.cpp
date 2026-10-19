// Rasterises the Lucide SVG masters into the PNG ladder the Qt app embeds.
//
//   svg2png <masters-dir> <out-dir> <size> [size...]
//
// The PNGs are committed, so CI never has to rasterise: this tool only needs to
// run when the masters change. Any Qt 5 install can build it (QtSvg is the only
// extra module, and it is needed only here, not by the app).
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 4) {
        std::fprintf(stderr, "usage: svg2png <masters-dir> <out-dir> <size> [size...]\n");
        return 2;
    }
    const QDir masters(args.at(1));
    const QString outDir = args.at(2);
    if (!QDir().mkpath(outDir)) {
        std::fprintf(stderr, "cannot create %s\n", qPrintable(outDir));
        return 1;
    }
    QList<int> sizes;
    for (int i = 3; i < args.size(); ++i) {
        sizes.append(args.at(i).toInt());
    }

    int written = 0;
    const QFileInfoList files = masters.entryInfoList({QStringLiteral("*.svg")}, QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        QSvgRenderer renderer(info.absoluteFilePath());
        if (!renderer.isValid()) {
            std::fprintf(stderr, "invalid svg: %s\n", qPrintable(info.fileName()));
            return 1;
        }
        const QString name = info.completeBaseName();
        for (const int size : sizes) {
            QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            renderer.render(&painter, QRectF(0, 0, size, size));
            painter.end();

            // One ink for every icon: the app tints at runtime, so only alpha matters.
            QPainter tint(&image);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(image.rect(), QColor(0xFF, 0xFF, 0xFF));
            tint.end();

            const QString path = outDir + QLatin1Char('/') + name + QLatin1Char('-')
                + QString::number(size) + QStringLiteral(".png");
            if (!image.save(path)) {
                std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
                return 1;
            }
            ++written;
        }
    }
    std::printf("wrote %d png from %d masters\n", written, int(files.size()));
    return 0;
}
