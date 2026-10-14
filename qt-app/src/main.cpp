#include "videovault/app/main_window.hpp"
#include "videovault/app/theme.hpp"
#include "videovault/core/version.hpp"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QLocalSocket>
#include <QSettings>
#include <QString>

#include <string>

namespace {

const QString kImportInbox = QStringLiteral("MegaVaultProtect.inbox");

bool forwardToRunning(const QStringList& paths) {
    QLocalSocket socket;
    socket.connectToServer(kImportInbox);
    if (!socket.waitForConnected(400)) {
        return false;
    }
    QByteArray body;
    if (paths.isEmpty()) {
        body = "ACTIVATE\n";
    } else {
        for (const QString& path : paths) {
            body += path.toUtf8();
            body += '\n';
        }
    }
    socket.write(body);
    socket.flush();
    socket.waitForBytesWritten(3000);
    socket.disconnectFromServer();
    socket.waitForDisconnected(1000);
    return true;
}

void installSendToShortcut() {
#ifdef Q_OS_WIN
    wchar_t send_to[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_SENDTO, nullptr, SHGFP_TYPE_CURRENT, send_to))) {
        return;
    }
    const std::wstring link = std::wstring(send_to) + L"\\Mega Vault Protect.lnk";
    const QString exe = QCoreApplication::applicationFilePath();
    const std::wstring target = exe.toStdWString();
    const std::wstring work = QFileInfo(exe).absolutePath().toStdWString();
    // Qt has already initialized COM. A second successful init must not be
    // torn down, or the rest of the process loses it.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&shell))) || shell == nullptr) {
        return;
    }
    shell->SetPath(target.c_str());
    shell->SetDescription(L"Import into Mega Vault Protect");
    shell->SetWorkingDirectory(work.c_str());
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && file != nullptr) {
        file->Save(link.c_str(), TRUE);
        file->Release();
    }
    shell->Release();
#else
    (void)0;
#endif
}

#ifdef Q_OS_WIN
// Gives .mvpk packages the package icon in Explorer and lets a double-click
// open this app's package import. Everything sits under HKEY_CURRENT_USER, so
// it needs no elevation and never touches another user's settings. The
// resource id 2 is mvpk.ico, compiled in by resources/app.rc.
void installPackageFileType() {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString progId = QStringLiteral("MegaVaultProtect.mvpk");
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
        QSettings::NativeFormat);
    bool changed = false;
    const auto assign = [&](const QString& key, const QString& value) {
        if (classes.value(key).toString() == value) {
            return;
        }
        classes.setValue(key, value);
        changed = true;
    };
    assign(QStringLiteral(".mvpk/"), progId);
    assign(QStringLiteral(".mvpk/Content Type"),
        QStringLiteral("application/x-megavault-package"));
    assign(progId + QStringLiteral("/"), QStringLiteral("Mega Vault package"));
    assign(progId + QStringLiteral("/DefaultIcon/"),
        QStringLiteral("\"%1\",2").arg(exe));
    assign(progId + QStringLiteral("/shell/open/command/"),
        QStringLiteral("\"%1\" \"%2\"").arg(exe, QStringLiteral("%1")));
    if (!changed) {
        return;
    }
    classes.sync();
    // Explorer caches the icon it showed for a file type, so tell it to look
    // again after the first run (or after the app moved).
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Mega Vault Protect"));
    application.setOrganizationName(QStringLiteral("Mega Vault Protect"));
    // The previous name stored the vault path and gallery preferences.
    // Copy them once so an existing install still opens the same vault.
    {
        QSettings current;
        QSettings previous(QStringLiteral("Mega Video Protect"), QStringLiteral("Mega Video Protect"));
        const auto keys = previous.allKeys();
        for (const QString& key : keys) {
            if (!current.contains(key)) {
                current.setValue(key, previous.value(key));
            }
        }
    }
    const auto version = videovault::core::version();
    application.setApplicationVersion(QString::fromLatin1(
        version.data(), static_cast<int>(version.size())));
    // Window/taskbar icon on every platform (the .ico resource handles the
    // Explorer icon for the Windows exe itself).
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    QFont appFont(QStringLiteral("Segoe UI"));
    appFont.setPointSize(10);
    application.setFont(appFont);
    application.setPalette(videovault::app::darkPalette());
    application.setStyleSheet(videovault::app::appStyleSheet());

    QStringList incoming;
    const QStringList arguments = QCoreApplication::arguments();
    for (int index = 1; index < arguments.size(); ++index) {
        const QFileInfo info(arguments.at(index));
        if (info.exists()) {
            incoming.push_back(info.absoluteFilePath());
        }
    }
    installSendToShortcut();
    installPackageFileType();
    if (forwardToRunning(incoming)) {
        return 0;
    }

    videovault::app::MainWindow window;
    if (!incoming.isEmpty()) {
        window.acceptExternalPaths(incoming);
    }
    window.show();
    return application.exec();
}
