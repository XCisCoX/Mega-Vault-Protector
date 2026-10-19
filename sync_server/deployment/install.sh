#!/bin/sh
# Install the MegaVault sync server on a fresh Debian/Ubuntu or RHEL-family host.
#
#   ./install.sh --domain sync.example.com [--alias old.example.com] [--email you@example.com]
#
# What it does, in order: checks the Python version, creates a `megavault`
# system user, copies this folder to /opt/megavault-sync, keeps its data in
# /var/lib/megavault-sync, registers a hardened systemd unit on 127.0.0.1:8766,
# puts Caddy in front of it for TLS, and opens 80/443 in ufw when ufw is active.
#
# Without --domain it installs Caddy with its internal CA: HTTPS works on the
# host, but every device will reject the certificate, so it is a dry run of the
# deployment, not a usable one.
set -eu

DOMAIN=""
ALIASES=""   # extra names served by the same certificate and site block
EMAIL=""
SOURCE="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SERVICE_USER=megavault
PREFIX=/opt/megavault-sync
STATE=/var/lib/megavault-sync
PORT=8766

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --domain) DOMAIN=${2:?--domain needs a value}; shift 2 ;;
        --alias)  ALIASES="$ALIASES ${2:?--alias needs a value}"; shift 2 ;;
        --email)  EMAIL=${2:?--email needs a value}; shift 2 ;;
        --source) SOURCE=${2:?--source needs a value}; shift 2 ;;
        --port)   PORT=${2:?--port needs a value}; shift 2 ;;
        -h|--help) usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "run this as root" >&2; exit 1; }
[ -f "$SOURCE/__main__.py" ] || {
    echo "$SOURCE does not look like the sync_server folder" >&2; exit 1; }

# ── python ────────────────────────────────────────────────────────────────
PYTHON=$(command -v python3.13 || command -v python3.12 || command -v python3 || true)
have_python() {
    [ -n "$PYTHON" ] || return 1
    "$PYTHON" - <<'EOF' >/dev/null 2>&1
import sys
raise SystemExit(0 if sys.version_info >= (3, 12) else 1)
EOF
}

if ! have_python; then
    echo "Python 3.12+ is required (found ${PYTHON:-none}); trying to install it"
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update -qq
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3.12 python3.12-venv \
            || { echo "could not install python3.12; add the deadsnakes PPA or use a 24.04 image" >&2; exit 1; }
        PYTHON=$(command -v python3.12)
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y -q python3.12 || { echo "install python3.12 first" >&2; exit 1; }
        PYTHON=$(command -v python3.12)
    else
        echo "install Python 3.12+ and re-run" >&2; exit 1
    fi
    have_python || { echo "python still reports < 3.12" >&2; exit 1; }
fi
echo "python: $($PYTHON -V 2>&1)"

# ── user and files ────────────────────────────────────────────────────────
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --shell /usr/sbin/nologin "$SERVICE_USER"
fi
install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_USER" "$STATE"
install -d -m 0755 "$PREFIX"
rm -rf "$PREFIX/sync_server"
cp -a "$SOURCE" "$PREFIX/sync_server"
rm -rf "$PREFIX/sync_server/tests" "$PREFIX/sync_server/deployment"
find "$PREFIX/sync_server" -name '__pycache__' -type d -prune -exec rm -rf {} +
chown -R root:root "$PREFIX"
chmod -R a+rX "$PREFIX"

# ── systemd ───────────────────────────────────────────────────────────────
cat > /etc/systemd/system/megavault-sync.service <<EOF
[Unit]
Description=MegaVault encrypted sync relay
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
WorkingDirectory=$PREFIX
Environment=PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
ExecStart=$PYTHON -m sync_server --data $STATE/data serve --host 127.0.0.1 --port $PORT --registration closed
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$STATE
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictAddressFamilies=AF_INET AF_INET6
SystemCallFilter=@system-service
SystemCallErrorNumber=EPERM

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
# One word for the CLI. `python -m sync_server` needs the package's parent on
# the import path, which the shell's working directory does not provide, so an
# operator typing the raw command from /root gets "No module named sync_server".
cat > /usr/local/bin/megavault-sync <<WRAPPER
#!/bin/sh
exec sudo -u $SERVICE_USER env PYTHONPATH=$PREFIX $PYTHON -m sync_server --data $STATE/data "\$@"
WRAPPER
chmod 0755 /usr/local/bin/megavault-sync
systemctl enable megavault-sync.service
# restart, not enable --now: re-running the installer must pick up new code.
systemctl restart megavault-sync.service

# ── caddy (TLS at the edge) ───────────────────────────────────────────────
if ! command -v caddy >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
        # Ubuntu ships caddy in universe; prefer it, because the upstream repo
        # wants debian-keyring packages that Ubuntu does not always provide.
        apt-get update -qq
        if ! DEBIAN_FRONTEND=noninteractive apt-get install -y -qq caddy; then
            apt-get install -y -qq ca-certificates curl gnupg
            curl -fsSL https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
                | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
            curl -fsSL https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt \
                | tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
            apt-get update -qq
            DEBIAN_FRONTEND=noninteractive apt-get install -y -qq caddy
        fi
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y 'dnf-command(copr)'
        dnf copr enable -y @caddy/caddy
        dnf install -y caddy
    else
        echo "install caddy, then re-run with --domain" >&2; exit 1
    fi
fi

GLOBAL_OPTIONS=""
if [ -n "$DOMAIN" ]; then
    SITE="$DOMAIN"
    # Caddy takes a comma-separated address list per site block, so an old name
    # keeps working after the real domain is in place.
    for extra in $ALIASES; do SITE="$SITE, $extra"; done
else
    SITE=":443"
    # No domain given: Caddy signs with its own CA, which no device trusts.
    GLOBAL_OPTIONS="	local_certs"
fi
# Caddy allows exactly one global options block, so both lines share it.
[ -n "$EMAIL" ] && GLOBAL_OPTIONS="$GLOBAL_OPTIONS
	email $EMAIL"
GLOBAL=""
if [ -n "$GLOBAL_OPTIONS" ]; then
    GLOBAL="{
$GLOBAL_OPTIONS
}"
fi

mkdir -p /etc/caddy
if [ -f /etc/caddy/Caddyfile ] && ! grep -q "$SITE" /etc/caddy/Caddyfile; then
    cp /etc/caddy/Caddyfile "/etc/caddy/Caddyfile.before-megavault"
fi
{
    [ -n "$GLOBAL" ] && printf '%s\n\n' "$GLOBAL"
    cat <<EOF
$SITE {
	encode zstd gzip
	reverse_proxy 127.0.0.1:$PORT {
		# Packages are already compressed ciphertext: no buffering games.
		flush_interval -1
	}
}
EOF
} > /etc/caddy/Caddyfile
caddy validate --config /etc/caddy/Caddyfile >/dev/null
systemctl enable caddy >/dev/null 2>&1 || true
systemctl restart caddy

# ── firewall ──────────────────────────────────────────────────────────────
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
    ufw allow 80/tcp >/dev/null
    ufw allow 443/tcp >/dev/null
    echo "ufw: 80/443 allowed"
fi

# ── report ────────────────────────────────────────────────────────────────
sleep 1
systemctl --no-pager --lines=0 status megavault-sync.service | head -3 || true
if [ -n "$DOMAIN" ]; then
    URL="https://$DOMAIN"
else
    URL="https://<this-host>"
fi
cat <<EOF

Installed.
  code     $PREFIX/sync_server
  data     $STATE/data
  service  systemctl status megavault-sync
  console  $URL/

This host does not accept self-service signup; accounts are created here.
Create the owner account (it prompts for the password twice):
  megavault-sync account create you@example.com

Then open the console, create a vault, and issue one device token per device.
The relay port $PORT stays bound to 127.0.0.1; only Caddy answers on 443.
EOF
