#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QAbstractButton;

namespace videovault::app::icons {

// The app's icon family is Lucide (ISC). Masters live in assets/icons/lucide and
// the generated PNG ladder is embedded from qt-app/resources/icons.qrc by
// tools/icons/generate.py - see docs/icon-strategy.md.
//
// Two tones: chrome sits at full strength, while row actions stay quiet until the
// pointer is over them (Qt asks the icon for its Active pixmap on hover).
enum class Tone {
    Normal,
    Quiet,
};

QIcon lucide(const QString& name, Tone tone = Tone::Normal);

// One colour for every state, with a faded copy for Disabled. Use this when the
// button supplies its own background (e.g. the white glyph on the primary button).
QIcon lucideSolid(const QString& name, const QColor& color);

// Puts an icon on a button and re-tints it whenever the palette changes, so it
// follows a theme switch without the call site doing anything.
void apply(QAbstractButton* button, const QString& name, Tone tone = Tone::Normal);

// Adds an icon *beside* the label. Primary and destructive actions keep their
// words: a glyph alone is not enough for "Delete permanently" or "Create a vault".
void withText(QAbstractButton* button, const QString& name, int size = 15);

} // namespace videovault::app::icons
