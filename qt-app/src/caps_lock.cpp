#include "videovault/app/caps_lock.hpp"

#include <QBoxLayout>
#include <QEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QSizePolicy>

namespace videovault::app {
namespace {

// Watches one field. Owned by the field, so it lives exactly as long as the field does.
class CapsLockWatcher final : public QObject {
public:
    CapsLockWatcher(QLineEdit* field, QLabel* label)
        : QObject(field), label_(label) {
        field->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const auto type = event->type();
        if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_CapsLock) {
                // The key itself is the only reliable signal for the state. On press only,
                // so a press/release pair does not toggle twice.
                if (type == QEvent::KeyPress) {
                    update(!on_);
                }
            } else if (type == QEvent::KeyPress && !key->text().isEmpty()) {
                const QChar typed = key->text().at(0);
                if (typed.isLetter()) {
                    const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
                    // Caps Lock inverts Shift.
                    update(typed.isUpper() != shifted);
                }
            }
        } else if (type == QEvent::FocusOut) {
            // Don't leave the warning behind on a field the user has walked away from.
            update(false);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void update(const bool on) {
        on_ = on;
        if (label_ != nullptr) {
            label_->setVisible(on);
        }
    }

    QLabel* label_{nullptr};
    bool on_{false};
};

} // namespace

QLabel* addCapsLockHint(QLineEdit* field, QLayout* layout) {
    if (field == nullptr || field->echoMode() != QLineEdit::Password) {
        return nullptr;
    }
    QWidget* host = field->parentWidget();
    auto* label = new QLabel(QStringLiteral("Caps Lock is on"), host);
    label->setObjectName(QStringLiteral("capsLockHint"));
    label->setAccessibleName(QStringLiteral("Caps Lock is on"));
    label->setStyleSheet(QStringLiteral(
        "QLabel#capsLockHint { color: #ff9f0a; background: transparent; font-size: 12px; }"));
    // Keep the space while hidden: the warning appears mid-typing, and a sheet that grows or
    // shrinks under the cursor is worse than the warning is useful.
    QSizePolicy policy = label->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    label->setSizePolicy(policy);
    label->hide();

    QLayout* target = layout;
    if (target == nullptr && host != nullptr) {
        target = host->layout();
    }
    if (target != nullptr) {
        int after = -1;
        for (int index = 0; index < target->count(); ++index) {
            auto* item = target->itemAt(index);
            if (item != nullptr && item->widget() == field) {
                after = index;
                break;
            }
        }
        // Only a box layout can place a widget at an index; anything else gets it appended.
        auto* box = qobject_cast<QBoxLayout*>(target);
        if (box != nullptr && after >= 0) {
            box->insertWidget(after + 1, label);
        } else {
            target->addWidget(label);
        }
    }
    new CapsLockWatcher(field, label);
    return label;
}

} // namespace videovault::app
