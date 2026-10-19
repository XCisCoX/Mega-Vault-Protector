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
// Writes one value under HKEY_CURRENT_USER and reports whether it changed.
// name is nullptr for the key's default value, which is where a ProgID and the
// shell's open command live. QSettings was tried first and dropped its "/"
// suffix, putting the values on the wrong key.
bool writeRegistryString(const std::wstring& path, const wchar_t* name,
    const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool differs = true;
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS
        && type == REG_SZ && size >= sizeof(wchar_t)) {
        std::wstring current(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type,
                reinterpret_cast<BYTE*>(current.data()), &size) == ERROR_SUCCESS) {
            while (!current.empty() && current.back() == L'\0') {
                current.pop_back();
            }
            differs = current != value;
        }
    }
    if (differs) {
        RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    return differs;
}

// Gives .mvpk packages the package icon in Explorer and lets a double-click
// open this app's package import. Everything sits under HKEY_CURRENT_USER, so
// it needs no elevation and never touches another user's settings. The
// resource id 2 is mvpk.ico, compiled in by resources/app.rc.
void installPackageFileType() {
    const std::wstring exe =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    const std::wstring classes = L"Software\\Classes";
    const std::wstring prog_id = L"MegaVaultProtect.mvpk";
    const std::wstring icon = L"\"" + exe + L"\",2";
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";
    // Bumped when the registration itself changes (the icon resource was
    // rebuilt from PNG frames to BMP frames, which is what the shell reads).
    // Without this, an install whose values already match never re-announces
    // the type, and Explorer keeps serving its cached generic icon.
    constexpr int kRegistrationVersion = 2;
    QSettings settings;
    const bool stale = settings.value(QStringLiteral("shell/registrationVersion"), 0).toInt()
        != kRegistrationVersion;
    bool changed = false;
    changed |= writeRegistryString(classes + L"\\.mvpk", nullptr, prog_id);
    changed |= writeRegistryString(classes + L"\\.mvpk", L"Content Type",
        L"application/x-megavault-package");
    changed |= writeRegistryString(classes + L"\\" + prog_id, nullptr,
        L"Mega Vault package");
    changed |= writeRegistryString(classes + L"\\" + prog_id + L"\\DefaultIcon",
        nullptr, icon);
    changed |= writeRegistryString(
        classes + L"\\" + prog_id + L"\\shell\\open\\command", nullptr, command);
    if (!changed && !stale) {
        return;
    }
    settings.setValue(QStringLiteral("shell/registrationVersion"), kRegistrationVersion);
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
    videovault::app::initializeTheme(application);

    QStringList incoming;
    const QStringList arguments = QCoreApplication::arguments();
    for (int index = 1; index < arguments.size(); ++index) {
        const QFileInfo info(arguments.at(index));
        if (info.exists()) {
            incoming.push_back(info.absoluteFilePath());
        }
    }
    installSendToShortcut();
#ifdef Q_OS_WIN
    installPackageFileType();
#endif
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
