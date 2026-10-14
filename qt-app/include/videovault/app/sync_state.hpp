#pragma once
#include <QString>
#include <QJsonObject>
namespace videovault::app::sync {
// Authenticated encryption; recovery key is supplied only while local vault is unlocked.
QJsonObject loadState(const QString& path,const QString& recovery);
void saveState(const QString& path,const QString& recovery,const QJsonObject& state);
QString randomHex(int bytes=16);
QString fileHash(const QString& path);
}
