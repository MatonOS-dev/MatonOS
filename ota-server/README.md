# MatonOS OTA API server

Small Node 24 HTTP server using only `node:http` and built-in modules. It
serves configured update discovery endpoints and immutable release files; it
does not upload releases. See [the OTA contract](../install/OTA-CONTRACT.md)
for complete endpoint schemas and behavior.

## Run

```sh
export OTA_STORAGE_DIR=/srv/matonos-ota/storage
export OTA_CONFIG=/etc/matonos-ota/channels.json
export OTA_PORT=8080
node src/server.ts
```

Promote an immutable update by editing the channels configuration through the
guarded CLI. It rejects a lower Android base than the channel currently
serves:

```sh
OTA_STORAGE_DIR=/srv/matonos-ota/storage \
OTA_CONFIG=/etc/matonos-ota/channels.json \
node src/promote.ts testing 1795430400
```

Configuration paths are supplied by environment variables. Example:

```json
{
  "channels": {
    "testing": { "update_id": 1795430400 },
    "main": { "update_id": 1795430400 }
  }
}
```

## systemd user unit

```ini
[Unit]
Description=MatonOS OTA API server
After=network-online.target

[Service]
Type=simple
WorkingDirectory=%h/matonos/ota-server
Environment=OTA_PORT=8080
Environment=OTA_STORAGE_DIR=%h/matonos-ota/storage
Environment=OTA_CONFIG=%h/.config/matonos-ota/channels.json
ExecStart=/usr/bin/node src/server.ts
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
```

Place this at `~/.config/systemd/user/matonos-ota.service`, then run:

```sh
systemctl --user daemon-reload
systemctl --user enable --now matonos-ota.service
```

Put TLS termination and the `/api` and `/storage` routes behind the deployment
proxy. API responses are `no-store`; immutable files may be cached for one
year. Forward `Range` and `If-Range` requests without rewriting them.

## Tests

```sh
npm test
python3 ../install/ota/test_ota.py
```

Node tests cover all four API endpoints, downgrade filtering and promotion,
the required add-on gate, fixed EOL response, malformed requests, and byte ranges. The Python test
checks publisher output and compatibility with the native slot-writer
manifest verifier.
