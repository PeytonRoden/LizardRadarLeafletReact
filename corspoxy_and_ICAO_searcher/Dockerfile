FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

RUN pip install --no-cache-dir \
    fastapi==0.116.1 \
    requests==2.32.4 \
    s3fs==2025.7.0 \
    uvicorn==0.35.0 \
    && addgroup --system app \
    && adduser --system --ingroup app app

COPY --chown=app:app main.py ./main.py

USER app

EXPOSE 8002

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8002"]
