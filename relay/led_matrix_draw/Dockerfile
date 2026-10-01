FROM python:3.12-alpine

RUN apk add --no-cache lua5.4 && pip install --no-cache-dir aiohttp==3.12.15

WORKDIR /app
COPY server.py lua_runner.lua ./

CMD ["python", "-u", "server.py"]
