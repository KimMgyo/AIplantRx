# Dokploy builds this from the repo. Single stage: the dependency set is small
# and the image is rebuilt rarely, so a multi-stage build would buy less than it
# costs in confusion.
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1

WORKDIR /srv

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

# SQLite lives here; mount a Dokploy volume on it or every redeploy starts the
# greenhouse's history over.
ENV PLANTRX_DB=/srv/data/plantrx.db
RUN mkdir -p /srv/data
VOLUME ["/srv/data"]

EXPOSE 8000

# One worker on purpose. The state that matters (the "is a diagnosis already
# running" lock) is in-process, and a second worker would let two model calls
# race on the same window. This serves one device polling every ~60s; there is
# nothing to scale out.
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--workers", "1"]
