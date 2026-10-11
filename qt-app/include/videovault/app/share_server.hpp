#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>

class QObject;
class QWidget;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

class PeerLink;

// Serves a vault to a phone on the same network. The QR code carries the
// address and the SHA-256 of this session's TLS certificate. The phone sends
// the vault password only after that certificate matches. Locking the PC
// window does not stop sharing. Stop sharing does.
//
// The same code also carries a one-time swap key. A phone that scans it from
// "Swap with PC" asks to trade libraries: once the PC accepts, the phone can
// browse and copy this PC's open library, and the PC can browse and copy the
// phone's through peer(). Neither side can change the other's library.
class ShareServer {
public:
    using Approver = std::function<void(const QString& phone, std::function<void(bool)> answer)>;

    ShareServer();
    ~ShareServer();

    ShareServer(const ShareServer&) = delete;
    ShareServer& operator=(const ShareServer&) = delete;

    // Listens on every IPv4 interface. `error` is a sentence for a dialog.
    // The vault itself is opened later, when the phone presents the password.
    bool start(const std::filesystem::path& root, QString* error);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QStringList hosts() const;
    [[nodiscard]] QString linkFor(const QString& host) const;

    // Called on the UI thread when sharing starts or stops.
    void setOnRunning(std::function<void(bool)> callback);

    // The PC's own open vault, offered to a swapped phone. Null while locked.
    void setLocalVault(std::shared_ptr<core::Vault> vault);

    // Runs on `context`'s thread when a phone asks to swap. `answer` decides.
    // Without an approver every swap is declined.
    void setLinkApprover(QObject* context, Approver approver);

    // Runs on the approver's context thread whenever a phone links or leaves.
    void setOnPeerChanged(std::function<void()> callback);

    // Ends the swap. The phone's next request is refused.
    void unlinkPeer();

    [[nodiscard]] std::shared_ptr<PeerLink> peer() const;

    // Non-modal. Closing it leaves the listener running.
    void showDialog(QWidget* parent);

    struct State;
    struct Hooks;

private:
    class ListenThread;

    std::shared_ptr<State> state_;
    std::shared_ptr<Hooks> hooks_;
    std::shared_ptr<PeerLink> peer_;
    ListenThread* thread_{nullptr};
    quint16 port_{0};
    QString fingerprint_;
    QString linkKey_;
    std::function<void(bool)> onRunning_;
    QWidget* dialog_{nullptr};
};

} // namespace videovault::app
