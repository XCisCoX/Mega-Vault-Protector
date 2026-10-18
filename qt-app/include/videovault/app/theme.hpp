#pragma once

// Shared application theme. Telegram night glass: black field, frosted
// panels, blue actions. Used by main() and by tooling that renders the UI.

#include <QColor>
#include <QPalette>
#include <QString>

class QApplication;

namespace videovault::app {

// Missing/invalid preferences intentionally retain the original dark theme.
enum class ThemeMode { System, Light, Dark };
ThemeMode savedThemeMode();
void setThemeMode(ThemeMode mode);
void initializeTheme(QApplication& application);
bool isDarkTheme();
QPalette lightPalette();
// Accepts a DARK source stylesheet; dark mode returns it byte-for-byte.
QString themedStyleSheet(const QString& darkStyle);

inline QPalette darkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0, 0, 0));
    palette.setColor(QPalette::WindowText, QColor(255, 255, 255));
    palette.setColor(QPalette::Base, QColor(20, 20, 22));
    palette.setColor(QPalette::AlternateBase, QColor(16, 16, 18));
    palette.setColor(QPalette::Text, QColor(242, 242, 247));
    palette.setColor(QPalette::PlaceholderText, QColor(109, 109, 114));
    palette.setColor(QPalette::Button, QColor(28, 28, 30));
    palette.setColor(QPalette::ButtonText, QColor(255, 255, 255));
    palette.setColor(QPalette::Highlight, QColor(51, 144, 236));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    // Explicit mid tones: Qt otherwise derives them from a light system
    // palette, which painted near-white hairlines around dark cards.
    palette.setColor(QPalette::Midlight, QColor(70, 70, 74));
    palette.setColor(QPalette::Mid, QColor(90, 90, 95));
    return palette;
}

inline QString appStyleSheet() {
    return QStringLiteral(R"(
        QMainWindow, QStackedWidget, QDialog { background: #000000; }
        QWidget#libraryBar {
            background: transparent;
            border: none;
        }
        QFrame#card {
            background: #161618;
            border: 1px solid rgba(255, 255, 255, 22);
            border-radius: 22px;
        }
        QFrame#settingsGroup {
            background: #1c1c1e;
            border: none;
            border-radius: 14px;
        }
        QLabel#pageTitle {
            color: #ffffff;
            font-size: 25px;
            font-weight: 600;
        }
        QLabel#description, QLabel#section { color: #8e8e93; font-size: 13px; }
        QLabel#errorLabel {
            color: #ff6b6b;
            background: #2c1518;
            border: 1px solid #5c2a30;
            border-radius: 10px;
            padding: 9px;
        }
        QLineEdit, QComboBox {
            min-height: 36px;
            color: #ffffff;
            background: #1c1c1e;
            border: 1px solid rgba(255, 255, 255, 28);
            border-radius: 14px;
            padding: 0 12px;
            selection-background-color: #3390ec;
        }
        QLineEdit:focus, QComboBox:focus { border: 1px solid #3390ec; }
        /* The native drop-down is a square block with its own frame, which
           reads as a foreign control inside the rounded fields. Draw only a
           chevron and keep the whole field on one surface. */
        QComboBox { padding-right: 30px; }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: center right;
            width: 26px;
            border: none;
            background: transparent;
        }
        QComboBox::down-arrow {
            image: url(:/icons/chevron_down.png);
            width: 12px;
            height: 12px;
        }
        QComboBox QAbstractItemView::item { min-height: 28px; padding: 2px 8px; }
        QComboBox QAbstractItemView {
            background: #1c1c1e;
            color: #ffffff;
            border: 1px solid rgba(255, 255, 255, 36);
            selection-background-color: #3390ec;
        }
        QMenu {
            background-color: #1c1c1e;
            color: #ffffff;
            border: 1px solid rgba(255, 255, 255, 36);
            border-radius: 14px;
            padding: 6px;
        }
        QMenu::item {
            background: transparent;
            color: #ffffff;
            padding: 8px 16px;
            margin: 2px 4px;
            border-radius: 8px;
        }
        QMenu::item:selected { background: rgba(51, 144, 236, 90); }
        QMenu::separator {
            height: 1px;
            background: rgba(255, 255, 255, 28);
            margin: 6px 12px;
        }
        QPushButton {
            min-height: 36px;
            color: #ffffff;
            background: #1c1c1e;
            border: 1px solid rgba(255, 255, 255, 24);
            border-radius: 18px;
            padding: 0 16px;
        }
        QPushButton:hover { background: #2c2c2e; }
        QPushButton:disabled { color: #636366; background: #1c1c1e; }
        QPushButton[primary="true"] {
            color: white;
            background: #3390ec;
            border-color: #3390ec;
            font-weight: 600;
        }
        QPushButton[primary="true"]:hover { background: #4ba0f5; }
        /* A disabled primary action must not still look clickable. The record
           editor's Save button is the first primary button that gets disabled
           (while nothing is unsaved), and without this rule the plain
           QPushButton:disabled rule above loses to the attribute selector. */
        QPushButton[primary="true"]:disabled {
            color: #9a9aa0;
            background: #1d5b96;
            border-color: #1d5b96;
        }
        QPushButton[quiet="true"] {
            color: #d1d1d6;
            background: transparent;
            border: none;
            min-height: 32px;
        }
        QPushButton[quiet="true"]:hover { background: rgba(255, 255, 255, 18); }
        QPushButton[quiet="true"]:disabled { color: #636366; background: transparent; }
        QLabel#statusNote {
            color: #aeaeb2;
            background: transparent;
            border: none;
            padding: 0;
        }
        QLabel#statusNote[problem="true"] { color: #ff6b6b; }
        QProgressBar {
            background: #1c1c1e;
            border: none;
            border-radius: 4px;
            max-height: 8px;
            min-height: 8px;
        }
        QProgressBar::chunk { background: #3390ec; border-radius: 4px; }
        QToolTip {
            color: #ffffff;
            background: #2c2c2e;
            border: 1px solid rgba(255, 255, 255, 36);
            padding: 6px 8px;
        }
        QMessageBox { background: #161618; }
        QMessageBox QLabel { color: #f2f2f7; background: transparent; }
        QPushButton#tagRemove {
            min-height: 28px;
            max-width: 28px;
            padding: 0;
            color: #ff453a;
            background: transparent;
            border: none;
            font-size: 18px;
        }
        QPushButton#tagRemove:hover { background: rgba(255, 69, 58, 40); }
        QListWidget#tagList {
            background: transparent;
            border: none;
            outline: none;
            color: #ffffff;
        }
        QListWidget#tagList::item { border: none; }
        QListWidget#tagList::item:selected { background: rgba(51, 144, 236, 70); }
        QTreeWidget#gallery, QListWidget#gallery {
            background: #000000;
            alternate-background-color: #101012;
            color: #f2f2f7;
            border: none;
            outline: none;
        }
        QTreeWidget#gallery::item, QListWidget#gallery::item {
            color: #f2f2f7;
            padding: 4px;
        }
        QTreeWidget#gallery::item:selected, QListWidget#gallery::item:selected {
            background: rgba(51, 144, 236, 90);
            color: #ffffff;
        }
        QTreeWidget#gallery::item:hover, QListWidget#gallery::item:hover {
            background: rgba(255, 255, 255, 16);
        }
        QHeaderView { background: #101012; color: #8e8e93; }
        QHeaderView::section {
            background: #101012;
            color: #8e8e93;
            border: none;
            border-right: 1px solid rgba(255, 255, 255, 18);
            border-bottom: 1px solid rgba(255, 255, 255, 18);
            padding: 6px 8px;
        }
        QSlider::groove:horizontal {
            height: 4px;
            background: rgba(255, 255, 255, 40);
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #3390ec;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px;
            height: 14px;
            margin: -5px 0;
            border-radius: 7px;
            background: #ffffff;
            border: 1px solid #3390ec;
        }
        QSlider::handle:horizontal:hover { background: #e8f3ff; }
        QScrollBar:vertical { background: transparent; width: 12px; }
        QScrollBar::handle:vertical {
            background: #3a3a3c;
            border-radius: 5px;
            min-height: 30px;
            margin: 2px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: transparent; height: 12px; }
        QScrollBar::handle:horizontal {
            background: #3a3a3c;
            border-radius: 5px;
            min-width: 30px;
            margin: 2px;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )");
}

} // namespace videovault::app
