FROM python:3.14-slim
# No bytecode writes: the root filesystem is read-only at runtime and the
# process does not own /app.
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
# Package and static UI must stay siblings: `serve` resolves the web directory
# as <package>/../sync_web, so both copies are required for / to answer.
COPY sync_server /app/sync_server
COPY sync_web /app/sync_web
# Only the data directory has to be writable; compose mounts a volume there and
# Docker copies this ownership into a fresh volume.
RUN mkdir -p /data && chown 10001:10001 /data
USER 10001:10001
EXPOSE 8766
ENTRYPOINT ["python", "-m", "sync_server", "--data", "/data"]
# Loopback plaintext is the safe default for `docker run`; compose supplies the
# TLS bind, because a non-loopback host is refused without one.
CMD ["serve", "--host", "127.0.0.1", "--port", "8766"]
