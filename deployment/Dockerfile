FROM python:3.14-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY sync_server /app/sync_server
RUN mkdir -p /data /backups && chown -R 10001:10001 /data /backups /app
USER 10001:10001
EXPOSE 8766
ENTRYPOINT ["python", "-m", "sync_server", "--data", "/data"]
CMD ["serve", "--host", "127.0.0.1", "--port", "8766"]
