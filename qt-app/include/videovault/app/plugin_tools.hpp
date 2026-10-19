#pragma once
#include "videovault/app/plugin_host.hpp"
class QPlainTextEdit;
namespace videovault::app {
// Generic command surface; it does not know about music, vault keys or files.
class ExtensionToolsPage final : public QWidget {
    Q_OBJECT
public:
    explicit ExtensionToolsPage(PluginHost*, QWidget* parent=nullptr);
    void selectExtension(const QString& id);
    void stopAndClear();
signals:
    void closed();
private:
    void refresh();
    void selectCommand();
    void run();
    PluginManifest selected() const;
    PluginHost* host_;
    QComboBox* extensions_; QComboBox* commands_;
    QPlainTextEdit* input_; QPlainTextEdit* output_;
    QLabel* notice_; QLabel* inputLabel_; QLabel* resultTitle_; QLabel* status_;
    QPushButton* run_; bool busy_=false; quint64 generation_=0;
};
}
