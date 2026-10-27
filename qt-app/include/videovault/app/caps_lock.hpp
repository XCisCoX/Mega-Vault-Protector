#pragma once

#include <QLabel>
#include <QLineEdit>

class QLayout;

namespace videovault::app {

// Adds a "Caps Lock is on" hint for a password field and keeps it in sync.
//
// Qt has no portable way to ask whether Caps Lock is on, so the state is derived from what the
// field actually receives: the Caps Lock key itself toggles it, and a letter typed in the "wrong"
// case corrects it. Caps Lock inverts Shift, so an upper-case letter with no Shift -- or a
// lower-case one with Shift held -- means it is on. The hint therefore appears with the first
// letter rather than on focus, which is the moment it can cost somebody their password.
//
// `layout` is where the label goes, directly after the field. Passing nullptr uses the field's own
// parent layout. Returns the label (nullptr when `field` is not a password field), so a caller or a
// test can find it by its object name, "capsLockHint".
QLabel* addCapsLockHint(QLineEdit* field, QLayout* layout = nullptr);

} // namespace videovault::app
