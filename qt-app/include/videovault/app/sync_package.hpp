#pragma once
#include <QString>
namespace videovault::app::sync {
struct PackageIdentity { QString relativePath,plaintextHash; quint64 size{}; };
// Authenticates and hashes exactly one MVPK file in bounded memory. No plaintext cache.
PackageIdentity inspectPackage(const QString& encryptedPath,const QString& recovery);
}
