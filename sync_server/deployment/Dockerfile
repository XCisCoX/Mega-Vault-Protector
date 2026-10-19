FROM python:3.14-slim
# No bytecode writes: the root filesystem is read-only at runtime and the
# process does not own /app.
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
# One folder, one copy: the console lives in sync_server/web, so `serve` finds
# it with no extra flag and no sibling directory.
COPY sync_server /app/sync_server
# Only the data directory has to be writable; compose mounts a volume there and
# Docker copies this ownership into a fresh volume.
RUN mkdir -p /data && chown 10001:10001 /data
USER 10001:10001
EXPOSE 8766
ENTRYPOINT ["python", "-m", "sync_server", "--data", "/data"]
# Loopback plaintext is the safe default for `docker run`; compose supplies the
# TLS bind, because a non-loopback host is refused without one.
CMD ["serve", "--host", "127.0.0.1", "--port", "8766"]
