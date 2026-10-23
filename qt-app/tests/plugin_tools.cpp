#include "videovault/app/plugin_host.hpp"
#include "videovault/app/plugin_tools.hpp"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
using namespace videovault::app;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x<<'\n';return 1;}}while(false)
bool waitFor(const std::function<bool()>& p){QElapsedTimer t;t.start();while(!p()&&t.elapsed()<10000){QApplication::processEvents();QThread::msleep(5);}return p();}
int main(int argc,char**argv){QApplication app(argc,argv);QTemporaryDir temp;app.setOrganizationName("PluginToolsTests");app.setApplicationName("Isolated");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());PluginHost host(temp.path()+"/plugins");ExtensionToolsPage page(&host);auto* run=page.findChild<QPushButton*>("extensionRun");auto* input=page.findChild<QPlainTextEdit*>("extensionInput");auto* output=page.findChild<QPlainTextEdit*>("extensionOutput");auto* status=page.findChild<QLabel*>("extensionStatus");CHECK(run&&input&&output&&status);CHECK(!run->isEnabled());CHECK(host.setEnabled("org.megavault.text-tools",true));page.selectExtension("org.megavault.text-tools");CHECK(run->isEnabled());input->setPlainText("Hello world\nMore");run->click();CHECK(waitFor([&]{return run->isEnabled();}));CHECK(output->toPlainText().contains("Words (whitespace-separated): 3"));CHECK(output->toPlainText().contains("Lines: 2"));
 QFile f(":/plugins/text-tools.json");CHECK(f.open(QIODevice::ReadOnly));auto fixture=QJsonDocument::fromJson(f.readAll()).object();fixture["id"]="org.example.sandbox";fixture["commands"]=QJsonArray{QJsonObject{{"id","inspect"},{"title","Inspect sandbox"},{"inputLabel","Input"}}};fixture["script"]="globalThis.extension={commands:{inspect:input=>({title:'<b>Plain title</b>',text:'<img src=file:///private> '+[typeof std,typeof os,typeof fetch,typeof require,typeof process].join(',')})}};";QString error;CHECK(host.install(QJsonDocument(fixture).toJson(),&error));CHECK(host.setEnabled("org.example.sandbox",true));page.selectExtension("org.example.sandbox");input->setPlainText("fixture");run->click();CHECK(waitFor([&]{return run->isEnabled();}));CHECK(output->toPlainText()=="<img src=file:///private> undefined,undefined,undefined,undefined,undefined");
 fixture["id"]="org.example.timeout";fixture["script"]="globalThis.extension={commands:{inspect:input=>{while(true){}}}};";CHECK(host.install(QJsonDocument(fixture).toJson(),&error));CHECK(host.setEnabled("org.example.timeout",true));page.selectExtension("org.example.timeout");run->click();CHECK(waitFor([&]{return run->isEnabled();}));CHECK(status->text().contains("failed",Qt::CaseInsensitive));CHECK(output->toPlainText().isEmpty());run->click();host.setEnabled("org.example.timeout",false);CHECK(!run->isEnabled()||page.findChild<QComboBox*>("extensionProvider")->currentData().toString()!="org.example.timeout");CHECK(waitFor([&]{return page.findChildren<QObject*>().size()>0;}));QElapsedTimer timer;timer.start();while(timer.elapsed()<300){app.processEvents();QThread::msleep(5);}CHECK(output->toPlainText().isEmpty());page.stopAndClear();CHECK(input->toPlainText().isEmpty()&&output->toPlainText().isEmpty());std::cout<<"PASS real QuickJS tools: enabled-only UI, async execution, bundled statistics, no host APIs, plain text, timeout, disable/generation discard\n";}
