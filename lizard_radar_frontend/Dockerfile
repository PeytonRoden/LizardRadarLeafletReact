FROM node:22-alpine AS build

WORKDIR /app

COPY package.json package-lock.json ./
RUN npm ci --legacy-peer-deps

COPY . .
RUN npm run build

FROM nginx:1.28-alpine

ENV FASTAPI_HOST=fastapi \
    FASTAPI_PORT=8002 \
    NGINX_ENVSUBST_FILTER="FASTAPI_HOST|FASTAPI_PORT"

RUN rm /etc/nginx/conf.d/default.conf \
    && mkdir -p /etc/nginx/templates \
    && printf '%s\n' \
        'server {' \
        '    listen 80;' \
        '    server_name _;' \
        '    root /usr/share/nginx/html;' \
        '    index index.html;' \
        '' \
        '    add_header Cross-Origin-Opener-Policy "same-origin" always;' \
        '    add_header Cross-Origin-Embedder-Policy "require-corp" always;' \
        '' \
        '    location ~ ^/(latest|available_|nexrad) {' \
        '        proxy_pass http://${FASTAPI_HOST}:${FASTAPI_PORT};' \
        '        proxy_http_version 1.1;' \
        '        proxy_buffering off;' \
        '        proxy_read_timeout 75s;' \
        '    }' \
        '' \
        '    location / {' \
        '        try_files $uri $uri/ /index.html;' \
        '    }' \
        '}' \
        > /etc/nginx/templates/default.conf.template

COPY --from=build /app/dist /usr/share/nginx/html

EXPOSE 80
