#include "videovault/app/tag_filter_button.hpp"
#include "videovault/app/theme.hpp"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <iostream>
#include <stdexcept>

using videovault::app::TagFilterButton;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setPalette(videovault::app::darkPalette());
    app.setStyleSheet(videovault::app::appStyleSheet());
    try {
        TagFilterButton button;
        int changes = 0;
        QObject::connect(&button, &TagFilterButton::selectionChanged,
                         [&changes](const QStringList&) { ++changes; });
        require(button.text() == QStringLiteral("All items"), "initial label");
        button.setTags({{QStringLiteral("Work"), 4}, {QStringLiteral("Travel"), 8}});
        button.setSelectedTags({QStringLiteral("work"), QStringLiteral("WORK"), QStringLiteral("unknown")});
        require(button.selectedTags() == QStringList{QStringLiteral("Work")}, "case-insensitive identity");
        require(button.text() == QStringLiteral("Work"), "single label");
        require(changes == 1, "one signal for logical change");
        button.setSelectedTags({QStringLiteral("WORK")});
        require(changes == 1, "equivalent setter is silent");
        std::cout << "PASS: selection identity and labels\n";
        button.setSelectedTags({QStringLiteral("Travel"), QStringLiteral("Work")});
        require(button.text() == QStringLiteral("2 tags"), "multi label");
        require(button.toolTip().contains(QStringLiteral("Work")) && button.toolTip().contains(QStringLiteral("Travel")), "tooltip names");
        const int beforeRefresh = changes;
        button.setTags({{QStringLiteral("TRAVEL"), 9}, {QStringLiteral("Work"), 5}, {QStringLiteral("work"), 12}});
        require(button.selectedTags().size() == 2, "refresh deduplicates identities");
        require(changes == beforeRefresh, "count, order and casing changes do not change selection");
        button.setTags({{QStringLiteral("Work"), 7}});
        require(button.selectedTags() == QStringList{QStringLiteral("Work")}, "removed selection pruned");
        require(changes == beforeRefresh + 1, "pruning emits once");
        std::cout << "PASS: refresh and prune\n";
        QWidget host;
        auto* control = new TagFilterButton(&host);
        control->setGeometry(350, 15, 180, 38);
        host.resize(550, 400);
        host.show();
        control->setTags({{QStringLiteral("Work"), 4}, {QStringLiteral("Travel"), 8}, {QStringLiteral("Family"), 1}});
        control->click();
        app.processEvents();
        auto* popup = control->findChild<QWidget*>(QStringLiteral("tagFilterPopup"));
        require(popup && popup->isVisible(), "button opens popup");
        auto* search = popup->findChild<QLineEdit*>(QStringLiteral("tagSearch"));
        auto* list = popup->findChild<QListWidget*>(QStringLiteral("tagList"));
        auto* clear = popup->findChild<QPushButton*>(QStringLiteral("clearTagFilter"));
        require(search && list && clear, "accessible named controls exist");
        require(!control->accessibleName().isEmpty() && !search->accessibleName().isEmpty()
                && !list->accessibleName().isEmpty() && !clear->accessibleName().isEmpty(), "accessibility names");
        require((popup->windowFlags() & Qt::Popup) == Qt::Popup, "real popup outside-dismiss semantics");
        require(popup->windowFlags().testFlag(Qt::FramelessWindowHint), "frameless popup");
        require(list->count() == 3, "all rows available");
        if (!qEnvironmentVariableIsEmpty("TAG_FILTER_SCREENSHOT"))
            require(popup->grab().save(qEnvironmentVariable("TAG_FILTER_SCREENSHOT") + QStringLiteral("-all.png")), "save unfiltered render");
        const QPoint center = list->visualItemRect(list->item(0)).center();
        QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &press);
        QApplication::sendEvent(list->viewport(), &release);
        require(control->selectedTags() == QStringList{QStringLiteral("Work")}, "actual row click toggles once");
        list->item(0)->setCheckState(Qt::Unchecked);
        require(control->selectedTags().isEmpty(), "accessible check state change updates selection");
        list->item(0)->setCheckState(Qt::Checked);
        require(control->selectedTags() == QStringList{QStringLiteral("Work")}, "accessible check action reselects");
        require(popup->isVisible(), "row click keeps popup open");
        search->setText(QStringLiteral("trav"));
        require(list->item(0)->isHidden() && !list->item(1)->isHidden(), "search filters rows");
        require(control->selectedTags().contains(QStringLiteral("Work")), "hidden selection preserved");
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(search, &down);
        require(list->currentItem() == list->item(1), "search down reaches visible row");
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
        QApplication::sendEvent(list, &space);
        require(control->selectedTags().size() == 2 && popup->isVisible(), "space toggles without dismissing");
        require(list->item(0)->checkState() == Qt::Checked && list->item(1)->checkState() == Qt::Checked,
                "accessible check states match selection");
        if (!qEnvironmentVariableIsEmpty("TAG_FILTER_SCREENSHOT"))
            require(popup->grab().save(qEnvironmentVariable("TAG_FILTER_SCREENSHOT")), "save popup render");
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
        QApplication::sendEvent(list, &tab);
        require(clear->hasFocus(), "Tab from list reaches clear action");
        list->setFocus();
        QKeyEvent type(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QApplication::sendEvent(list, &type);
        if (search->text() != QStringLiteral("travx")) std::cerr << "search after typing: " << search->text().toStdString() << '\n';
        require(search->text() == QStringLiteral("travx"), "typing from list reaches search");
        search->setText(QStringLiteral("trav"));
        control->setTags({{QStringLiteral("Work"), 99}, {QStringLiteral("Travel"), 1234}, {QStringLiteral("Family"), 1}});
        require(control->selectedTags().size() == 2 && search->text() == QStringLiteral("trav")
                && list->item(0)->isHidden(), "live refresh preserves search and hidden selection");
        clear->click();
        require(control->selectedTags().isEmpty() && popup->isVisible(), "clear selection stays open");
        search->setText(QStringLiteral("zzzz"));
        auto* empty = popup->findChild<QLabel*>(QStringLiteral("tagEmptyState"));
        require(empty && empty->isVisible() && empty->text() == QStringLiteral("No matching tags"), "no results state");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(search, &escape);
        require(!popup->isVisible(), "escape dismisses popup");
        control->click();
        app.processEvents();
        require(search->text().isEmpty(), "fresh search on reopening");
        QMouseEvent outside(QEvent::MouseButtonPress, QPointF(-10, -10), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(popup, &outside);
        require(!popup->isVisible(), "outside press dismisses popup");
        control->click();
        app.processEvents();
        require(popup->isVisible(), "popup reopens after outside dismissal");
        host.hide();
        app.processEvents();
        require(!popup->isVisible(), "parent page hide dismisses popup");
        host.show();
        control->click();
        app.processEvents();
        control->hide();
        require(!popup->isVisible(), "button hide dismisses popup");
        control->show();
        app.processEvents();
        control->setTags({});
        control->click();
        app.processEvents();
        if (!empty->isVisible() || empty->text() != QStringLiteral("No tags yet"))
            std::cerr << "empty state: popup=" << popup->isVisible() << " empty=" << empty->isVisible()
                      << " text=" << empty->text().toStdString() << '\n';
        require(empty->isVisible() && empty->text() == QStringLiteral("No tags yet"), "no tags state");
        popup->hide();
        app.processEvents();
        QVector<videovault::app::TagFilterOption> manyTags;
        for (int i = 0; i < 50; ++i)
            manyTags.append({QStringLiteral("Tag %1").arg(i), static_cast<quint64>(i)});
        control->setTags(manyTags);
        const QRect screenRect = app.primaryScreen()->availableGeometry();
        host.move(screenRect.right() - host.width(), screenRect.bottom() - host.height());
        control->move(host.width() - control->width(), host.height() - control->height());
        app.processEvents();
        control->click();
        app.processEvents();
        require(screenRect.contains(popup->geometry()), "popup clamped to screen at bottom right");
        require(list->count() == 50 && list->visualItemRect(list->item(0)).height() >= 36,
                "long lists retain comfortable row height");
        search->clear();
        QApplication::sendEvent(search, &down);
        require(list->currentRow() == 0, "down enters first available row");
        QApplication::sendEvent(list, &down);
        require(list->currentRow() == 1, "list arrows move current row");
        QApplication::sendEvent(list, &space);
        require(control->selectedTags() == QStringList{QStringLiteral("Tag 1")}, "keyboard toggles navigated row");
        delete control;
        app.processEvents();
        std::cout << "PASS: mouse, search, keyboard, clear, lifecycle and empty states\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
