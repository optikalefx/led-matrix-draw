FROM python:3.12-alpine

RUN pip install --no-cache-dir aiohttp==3.12.15

WORKDIR /app
COPY server.py sandbox_runner.py ./

CMD ["python", "-u", "server.py"]
