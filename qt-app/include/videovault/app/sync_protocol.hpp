#pragma once
#include <QUrl>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
namespace videovault::app::sync {
bool isHex(const QString& value, int length);
bool validServer(const QUrl& url);
bool validRecord(const QJsonObject& record);
bool acceptRevision(const QJsonObject& record,double observed,double acknowledged);
int retryDelay(int attempts);
QJsonObject conflictCopy(const QJsonObject& pending, const QString& newId, const QString& newOperation);
}
