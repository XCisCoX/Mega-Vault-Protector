// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Telegram Desktop media/player/media_player.style and
// media_player_button.cpp. Copyright Telegram Desktop contributors.
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
// Qt 5 adapter: no Telegram session/mixer or missing lib_ui dependency.
#pragma once
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace videovault::app::telegram_music {
// media_player.style: height 35 + lineWidth 1, controls 30, seek fullWidth 6.
inline constexpr int barHeight = 36;
inline constexpr int controlHeight = 30;
inline constexpr int seekHeight = 6;
inline constexpr int wideWidth = 460;
inline constexpr int playlistWidth = 344;
inline constexpr int playlistMaxHeight = 280;

struct BarGeometry {
    QRect previous, play, next, name, time, volume, order, repeat, speed, close, seek;
};
inline BarGeometry geometry(int width, bool skip, bool controls, int timeWidth,
                            int textHeight, int ascent) {
    BarGeometry g;
    int x = 9;
    if (skip) { g.previous = {x, 0, 24, 30}; x += 25; }
    g.play = {x, 0, 24, 30}; x += 24;
    if (skip) { ++x; g.next = {x, 0, 24, 30}; x += 24; }
    x += 8;
    int right = width;
    g.close = {right - 39, 0, 39, 30}; right -= 39;
    if (controls) {
        g.speed = {right - 30, 0, 30, 30}; right -= 30;
        g.repeat = {right - 30, 0, 30, 30}; right -= 30;
        g.order = {right - 30, 0, 30, 30}; right -= 30;
        g.volume = {right - 34, 0, 34, 30}; right -= 34;
    }
    right -= 8;
    // Widget::updateLabelsGeometry uses baseline 22, not vertical centering.
    g.time = {right - timeWidth, 22 - ascent, timeWidth, textHeight};
    g.name = {x, 22 - ascent, std::max(0, right - (controls ? timeWidth + 8 : 0) - x), textHeight};
    g.seek = {0, barHeight - seekHeight, width, seekHeight};
    return g;
}

using Quad = std::array<QPointF, 4>;
struct Shape { Quad left, right; qreal radius = 0.; };
inline QPointF normalized(QPointF v) {
    const auto length = std::hypot(v.x(), v.y());
    return length ? v / length : v;
}
inline QPointF insetCorner(QPointF previous, QPointF corner, QPointF next, qreal radius) {
    const auto in = normalized(corner - previous), out = normalized(next - corner);
    const auto cosine = in.x() * out.x() + in.y() * out.y();
    const auto sine = std::sqrt(std::max((1. + cosine) / 2., 0.));
    return corner + normalized(out - in) * (radius / sine);
}
inline Shape playShape() {
    // mediaPlayerPlayIcon: position (7,4), size (12,16), radius 1.5.
    const QPointF first(7, 4), second(19, 12), third(7, 20);
    const auto one = insetCorner(third, first, second, 1.5);
    const auto two = insetCorner(first, second, third, 1.5);
    const auto three = insetCorner(second, third, first, 1.5);
    const auto upper = (one + two) / 2., lower = (three + two) / 2.;
    return {{one, upper, lower, three}, {upper, two, two, lower}, 1.5};
}
inline Shape pauseShape() {
    // mediaPlayerPlayIcon: position (7,6), size (10,12), bar 4, radius 1.
    const Quad left = {QPointF(8,7), QPointF(10,7), QPointF(10,17), QPointF(8,17)};
    const Quad right = {QPointF(14,7), QPointF(16,7), QPointF(16,17), QPointF(14,17)};
    return {left, right, 1.};
}
inline Shape interpolate(const Shape& from, const Shape& to, qreal ratio) {
    Shape result;
    for (int i = 0; i != 4; ++i) {
        result.left[i] = from.left[i] + (to.left[i] - from.left[i]) * ratio;
        result.right[i] = from.right[i] + (to.right[i] - from.right[i]) * ratio;
    }
    result.radius = from.radius + (to.radius - from.radius) * ratio;
    return result;
}
inline void addQuad(QPainterPath& path, const Quad& quad, qreal radius) {
    const auto same = [](QPointF a, QPointF b) {
        return std::abs(a.x()-b.x()) < .001 && std::abs(a.y()-b.y()) < .001;
    };
    Quad points;
    int count = 0;
    for (const auto& point : quad)
        if (!count || !same(points[count-1], point)) points[count++] = point;
    if (count > 2 && same(points[0], points[count-1])) --count;
    QPainterPath skeleton;
    skeleton.moveTo(points[0]);
    for (int i = 1; i != count; ++i) skeleton.lineTo(points[i]);
    if (count > 2) skeleton.closeSubpath();
    QPainterPathStroker stroker;
    stroker.setWidth(radius * 2.);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);
    path.addPath(stroker.createStroke(skeleton));
    path.addPath(skeleton);
}
inline void paintShape(QPainter& p, const Shape& shape, QColor color) {
    QPainterPath path;
    addQuad(path, shape.left, shape.radius);
    addQuad(path, shape.right, shape.radius);
    path.setFillRule(Qt::WindingFill);
    p.fillPath(path, color);
}
} // namespace videovault::app::telegram_music

namespace videovault::app {
// FilledSlider adapter: preview on press/move, commit once on left release.
class ProgressLine final : public QWidget {
public:
    explicit ProgressLine(QWidget* parent = nullptr) : QWidget(parent), hover_(this) {
        setObjectName(QStringLiteral("musicSeekLine"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAccessibleName(tr("Playback position"));
        hover_.setDuration(150); // mediaPlayerPlayback.duration
        connect(&hover_, &QVariantAnimation::valueChanged, this, [this] { update(); });
    }
    int value() const { return value_; }
    bool dragging() const { return dragging_; }
    void setValue(int value) { value_ = std::clamp(value, 0, 1000); update(); }
    std::function<void(int)> moved, released;
    std::function<QString(int)> hoverText;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const qreal thick = dragging_ || hasFocus() ? 6. : 2. + hover_.currentValue().toReal() * 4.;
        const qreal played = isEnabled() ? width() * value_ / 1000. : 0.;
        const auto palette = QApplication::palette();
        p.fillRect(QRectF(0, height()-thick, width(), thick), palette.color(QPalette::Midlight));
        p.fillRect(QRectF(0, height()-thick, played, thick), palette.color(QPalette::Highlight));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (!isEnabled() || e->button() != Qt::LeftButton) { e->ignore(); return; }
        dragging_ = true;
        apply(e->pos().x());
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging_) apply(e->pos().x());
        if (hoverText && isEnabled()) {
            const auto text = hoverText(at(e->pos().x()));
            if (!text.isEmpty()) QToolTip::showText(mapToGlobal(QPoint(e->pos().x(), -20)), text, this);
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (!dragging_ || e->button() != Qt::LeftButton) { e->ignore(); return; }
        apply(e->pos().x());
        dragging_ = false;
        if (released) released(value_);
        update();
        e->accept();
    }
    void enterEvent(QEvent* e) override { animateHover(1.); QWidget::enterEvent(e); }
    void leaveEvent(QEvent* e) override { animateHover(0.); QToolTip::hideText(); QWidget::leaveEvent(e); }
    void keyPressEvent(QKeyEvent* e) override {
        int next = value_;
        if (e->key() == Qt::Key_Left) next -= 10;
        else if (e->key() == Qt::Key_Right) next += 10;
        else if (e->key() == Qt::Key_Home) next = 0;
        else if (e->key() == Qt::Key_End) next = 1000;
        else { QWidget::keyPressEvent(e); return; }
        setValue(next);
        if (moved) moved(value_);
        if (released) released(value_);
        e->accept();
    }
private:
    int at(int x) const { return width() <= 0 ? 0 : int(qint64(std::clamp(x, 0, width())) * 1000 / width()); }
    void apply(int x) { setValue(at(x)); if (moved) moved(value_); }
    void animateHover(qreal target) {
        const auto from = hover_.currentValue().toReal();
        hover_.stop(); hover_.setStartValue(from); hover_.setEndValue(target); hover_.start();
    }
    int value_ = 0;
    bool dragging_ = false;
    QVariantAnimation hover_;
};
} // namespace videovault::app
