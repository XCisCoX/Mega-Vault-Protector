// The Caps Lock hint: when it shows, when it hides, and where it sits.
//
// Caps Lock has no portable query in Qt, so the state is inferred from key events. The rules are
// easy to get subtly wrong -- Caps Lock *inverts* Shift, so an upper-case letter with no Shift and a
// lower-case letter with Shift both mean it is on -- which is what most of this file pins down.
#include "videovault/app/caps_lock.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

namespace app = videovault::app;

namespace {

int failures = 0;

void check(const bool ok, const char* what, const char* detail = nullptr) {
    if (detail != nullptr && *detail != '\0') {
        std::printf("%s %s  [%s]\n", ok ? "PASS" : "FAIL", what, detail);
    } else {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    }
    if (!ok) {
        ++failures;
    }
}

// A letter typed into the field, through the event filter, the way Qt delivers it.
void type(QLineEdit* field, const QString& text, const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, text.isEmpty() ? Qt::Key_unknown : text.at(0).toUpper().unicode(),
        modifiers, text);
    QApplication::sendEvent(field, &event);
}

void pressCapsLock(QLineEdit* field) {
    QKeyEvent press(QEvent::KeyPress, Qt::Key_CapsLock, Qt::NoModifier);
    QApplication::sendEvent(field, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_CapsLock, Qt::NoModifier);
    QApplication::sendEvent(field, &release);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);

    QWidget page;
    auto* layout = new QVBoxLayout(&page);
    auto* field = new QLineEdit(&page);
    field->setEchoMode(QLineEdit::Password);
    layout->addWidget(field);

    QLabel* hint = app::addCapsLockHint(field, layout);
    check(hint != nullptr, "a password field gets a hint");
    if (hint == nullptr) {
        std::printf("\n1 failure(s)\n");
        return 1;
    }
    check(!hint->isVisibleTo(&page), "the hint starts hidden");
    check(hint->sizePolicy().retainSizeWhenHidden(),
        "the hint keeps its space, so the sheet does not jump when it appears");

    // Directly under the field it describes, not at the end of the form.
    const int fieldIndex = layout->indexOf(field);
    check(layout->indexOf(hint) == fieldIndex + 1, "the hint sits directly after its field",
        qPrintable(QStringLiteral("%1 vs field %2").arg(layout->indexOf(hint)).arg(fieldIndex)));

    // --- the inference rules --------------------------------------------
    type(field, QStringLiteral("A"));
    check(hint->isVisibleTo(&page), "an upper-case letter with no Shift means Caps Lock is on");

    type(field, QStringLiteral("a"));
    check(!hint->isVisibleTo(&page), "a lower-case letter with no Shift means Caps Lock is off");

    type(field, QStringLiteral("a"), Qt::ShiftModifier);
    check(hint->isVisibleTo(&page), "a lower-case letter WITH Shift also means Caps Lock is on");

    type(field, QStringLiteral("A"), Qt::ShiftModifier);
    check(!hint->isVisibleTo(&page), "an upper-case letter with Shift means Caps Lock is off");

    // A digit says nothing about Caps Lock, so it must not clear the warning.
    type(field, QStringLiteral("A"));
    const bool wasOn = hint->isVisibleTo(&page);
    type(field, QStringLiteral("5"));
    check(wasOn && hint->isVisibleTo(&page), "a digit neither sets nor clears the state");

    // --- the Caps Lock key itself ---------------------------------------
    // Start from a known state: the 'A' above left the warning showing.
    type(field, QStringLiteral("a"));
    pressCapsLock(field);
    check(hint->isVisibleTo(&page), "the Caps Lock key turns the warning on");
    pressCapsLock(field);
    check(!hint->isVisibleTo(&page), "pressing it again turns the warning off");

    // --- leaving the field -----------------------------------------------
    type(field, QStringLiteral("A"));
    QEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(field, &focusOut);
    check(!hint->isVisibleTo(&page), "walking away from the field clears the warning");

    // --- only password fields --------------------------------------------
    QWidget other;
    auto* otherLayout = new QVBoxLayout(&other);
    auto* plain = new QLineEdit(&other);
    otherLayout->addWidget(plain);
    check(app::addCapsLockHint(plain, otherLayout) == nullptr,
        "a field that is not masked gets no hint");

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
