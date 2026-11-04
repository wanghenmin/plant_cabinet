# Plant Care System Backend

The backend uses FastAPI, SQLite, and a native HTML/JavaScript dashboard. Python dependencies and runtime are managed in the `code/backend/.venv` virtual environment, and configuration is provided through `.env` environment variables.

Dashboard files live in `app/static/`: `index.html` contains the page structure, `styles.css` contains the styles and responsive layout, and `app.js` contains charts, API requests, event bindings, and initialization. FastAPI serves these assets under `/static`.

## Run on Windows / PowerShell

First-time setup:

```powershell
cd code\backend
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
Copy-Item .env.example .env
```

Edit `.env` and replace `DEVICE_TOKEN` with a long random string. Start the service:

```powershell
cd code\backend
.\.venv\Scripts\Activate.ps1
python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

Open `http://127.0.0.1:8000` on the computer. API documentation is available at `/docs`. By default, the SQLite database is stored in `code/backend/data/` and is excluded from version control. `DATABASE_PATH`, `DEVICE_ID`, `DEVICE_TOKEN`, and `DEVICE_ONLINE_TIMEOUT_SECONDS` can all be configured in `.env`.

## ESP32 Network Setup

1. Connect the computer and ESP32 to the same local network. Run `ipconfig` on Windows and find the IPv4 address of the active Wi-Fi or Ethernet adapter.
2. In the Sketch `plant_cabinet.ino`, set the Wi-Fi name and password, `BACKEND_URL` (for example, `http://192.168.1.23:8000`), `DEVICE_ID`, and `DEVICE_TOKEN`.
3. `DEVICE_ID` must match the value in `.env`, and `DEVICE_TOKEN` must match it exactly. `127.0.0.1` refers to the ESP32 itself, not the computer, so it must not be used as the backend address on the device.
4. Start the backend first, then compile and upload the Sketch. Send control commands only after the dashboard shows the device as online.

If `DEVICE_TOKEN` is set in `.env`, device API requests must include the same token in the `X-Device-Token` header. The dashboard is intended for use on a trusted lab network; do not expose the service directly to the public internet.

## Features and API

- The dashboard shows device connectivity, live sensor readings, historical charts, sampling analysis, and operation events, and supports CSV export.
- Lighting supports manual on/off and restoring the schedule. The pump supports short tests. Every manual dashboard control requires confirmation, and new commands are rejected while the device is offline.
- The dashboard can adjust lighting hours and PWM, automatic-light lux threshold and hysteresis, soil-moisture threshold, watering pulse and interval, and the daily pump runtime limit. Automatic watering and automatic lighting are disabled by default.
- Automatic lighting is controlled locally by the ESP32: it turns on below `light_lux_threshold` and turns off above the threshold plus `light_lux_hysteresis`. Manual lighting mode takes priority.
- The time-series analysis page reads the configured device's database records by start and end date (Beijing time, UTC+8, inclusive, up to 31 days). On page load, it defaults to the most recent seven calendar days, including today. The analysis query returns every record in the selected range, without the overview's sample limit.

- `POST /api/sensors`: submit sensor readings from a device.
- `GET /api/status`, `GET /api/readings`, `GET /api/readings.csv`, `GET/PUT /api/config`: retrieve status and history, export readings, and manage control configuration.
- `GET /api/analysis/readings?start_date=2026-10-21&end_date=2026-10-27`: retrieve database readings for inclusive Beijing dates, sorted by timestamp.
- `POST /api/commands`: create a control command; `GET /api/device/poll`: device heartbeat and retrieval of configuration and commands; `POST /api/device/commands/{id}/result`: submit a device command result.
- `GET /api/events`: retrieve control and configuration events.

## Hardware Testing Notes

Dashboard pump tests are limited to 3 seconds. The firmware also enforces a 3-second per-run limit and a 60-second cumulative limit per 24 hours. Before testing, make sure the pump intake is submerged, the outlet tube is secured, and the power supply is safe. Power the light strip and pump with an external 5 V supply, and connect their ground to ESP32 ground according to the verified wiring. Keep automatic watering disabled during the first test. Software limits do not replace on-site supervision or hardware power-cut protection.

