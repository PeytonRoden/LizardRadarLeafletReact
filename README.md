# Lizard Radar

Lizard Radar consists of a FastAPI backend and a React/Vite frontend.

## Prerequisites

For local development:

- Python 3.11 or newer
- Node.js 22.12 or newer
- npm

For containerized deployment:

- Docker with a running Docker daemon

Run all commands below from the repository root unless a command changes directories explicitly.

## Start the apps locally

### 1. Start FastAPI

Open a terminal and run:

```sh
cd corspoxy_and_ICAO_searcher
python3 -m venv .venv
source .venv/bin/activate
python -m pip install fastapi==0.116.1 requests==2.32.4 s3fs==2025.7.0 uvicorn==0.35.0
uvicorn main:app --host 0.0.0.0 --port 8002
```

The API is available at:

- API: http://localhost:8002
- Interactive documentation: http://localhost:8002/docs

On Windows PowerShell, activate the virtual environment with:

```powershell
.venv\Scripts\Activate.ps1
```

### 2. Start React

Keep FastAPI running, open another terminal, and run:

```sh
cd lizard_radar_frontend
npm ci
npm run dev
```

Open the URL printed by Vite, normally http://localhost:5173. The Vite development server proxies API requests to FastAPI on port `8002`.

## Build the Docker images

Verify that Docker is running:

```sh
docker info
```

Build the FastAPI image:

```sh
docker build -t lizard-radar-api ./corspoxy_and_ICAO_searcher
```

Build the React image:

```sh
docker build -t lizard-radar-web ./lizard_radar_frontend
```

## Run the Docker containers

Both containers must use the same Docker network so the frontend can proxy API requests to the backend.

Create the network once:

```sh
docker network create lizard-radar
```

Start FastAPI:

```sh
docker run --rm --detach \
  --name lizard-radar-api \
  --network lizard-radar \
  --publish 8002:8002 \
  lizard-radar-api
```

Start React and Nginx:

```sh
docker run --rm --detach \
  --name lizard-radar-web \
  --network lizard-radar \
  --env FASTAPI_HOST=lizard-radar-api \
  --env FASTAPI_PORT=8002 \
  --publish 8080:80 \
  lizard-radar-web
```

Open the applications at:

- React application: http://localhost:8080
- FastAPI documentation: http://localhost:8002/docs

### View container status and logs

```sh
docker ps
docker logs lizard-radar-api
docker logs lizard-radar-web
```

Follow logs continuously with `--follow`:

```sh
docker logs --follow lizard-radar-api
docker logs --follow lizard-radar-web
```

### Stop the containers

```sh
docker stop lizard-radar-web lizard-radar-api
```

Because the containers were started with `--rm`, Docker removes them after they stop. The images remain available and can be run again with the same `docker run` commands.


### GCLOUD:

gcloud compute firewall-rules create allow-lizard-radar-web \
  --direction=INGRESS \
  --priority=1000 \
  --network=default \
  --action=ALLOW \
  --rules=tcp:80,tcp:443 \
  --source-ranges=0.0.0.0/0 \
  --target-tags=http-server,https-server



# Caddyfile

lizardradar.com {
    reverse_proxy lizard-radar-web:80
}



```sh
docker network create lizard-radar
```

```sh
docker run --detach \
  --restart unless-stopped \
  --name lizard-radar-caddy \
  --network lizard-radar \
  --publish 80:80 \
  --publish 443:443 \
  --volume "$PWD/Caddyfile:/etc/caddy/Caddyfile:ro" \
  --volume caddy_data:/data \
  --volume caddy_config:/config \
  caddy:2.10-alpine
```


```sh
docker run --detach \
  --restart unless-stopped \
  --name lizard-radar-web \
  --network lizard-radar \
  --env FASTAPI_HOST=lizard-radar-api \
  --env FASTAPI_PORT=8002 \
  lizard-radar-web
```

```sh
docker run --detach \
  --restart unless-stopped \
  --name lizard-radar-api \
  --network lizard-radar \
  --publish 8002:8002 \
  lizard-radar-api
```