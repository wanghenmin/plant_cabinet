import csv
import hmac
import io
import json
import os
import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Annotated

from dotenv import load_dotenv
from fastapi import Depends, FastAPI, Header, HTTPException, Query
from fastapi.responses import FileResponse, Response, StreamingResponse
from pydantic import BaseModel, Field


APP_DIR = Path(__file__).resolve().parent
BACKEND_DIR = APP_DIR.parent
load_dotenv(BACKEND_DIR / ".env")
DATABASE_PATH = Path(os.getenv("DATABASE_PATH", "./data/plant_cabinet.sqlite3"))
if not DATABASE_PATH.is_absolute():
    DATABASE_PATH = (BACKEND_DIR / DATABASE_PATH).resolve()
DEVICE_ID = os.getenv("DEVICE_ID", "plant-box-01")
DEVICE_TOKEN = os.getenv("DEVICE_TOKEN", "")
ONLINE_TIMEOUT = max(10, int(os.getenv("DEVICE_ONLINE_TIMEOUT_SECONDS", "45")))

app = FastAPI(title="Plant Cabinet", version="1.0.0")


class SensorReading(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    air_temperature: float | None = Field(default=None, ge=-40, le=100)
    air_humidity: float | None = Field(default=None, ge=0, le=100)
    light_lux: float | None = Field(default=None, ge=0, le=200000)
    soil_moisture_raw: int = Field(ge=0, le=4095)
    soil_moisture_percent: int = Field(ge=0, le=100)
    pump_running: bool = False
    light_duty: int = Field(default=0, ge=0, le=4095)


class DeviceConfig(BaseModel):
    schedule_enabled: bool = True
    light_on_minute: int = Field(default=480, ge=0, le=1439)
    light_off_minute: int = Field(default=1200, ge=0, le=1439)
    light_duty: int = Field(default=4095, ge=0, le=4095)
    auto_water_enabled: bool = False
    soil_start_threshold: int = Field(default=30, ge=0, le=100)
    water_pulse_ms: int = Field(default=1500, ge=250, le=3000)
    water_interval_ms: int = Field(default=600000, ge=10000, le=86400000)
    max_daily_pump_ms: int = Field(default=60000, ge=1000, le=60000)


class CommandRequest(BaseModel):
    device_id: str = Field(default=DEVICE_ID, min_length=1, max_length=64)
    action: str
    duration_ms: int = Field(default=2000, ge=250, le=3000)


class CommandResult(BaseModel):
    result: str = Field(min_length=1, max_length=32)
    message: str = Field(default="", max_length=300)


def now_utc() -> datetime:
    return datetime.now(timezone.utc)


def timestamp(value: datetime | None = None) -> str:
    return (value or now_utc()).isoformat(timespec="seconds")


def connect_db() -> sqlite3.Connection:
    connection = sqlite3.connect(DATABASE_PATH, timeout=10)
    connection.row_factory = sqlite3.Row
    return connection


def init_db() -> None:
    DATABASE_PATH.parent.mkdir(parents=True, exist_ok=True)
    with connect_db() as db:
        db.executescript(
            """
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                recorded_at TEXT NOT NULL,
                air_temperature REAL,
                air_humidity REAL,
                light_lux REAL,
                soil_moisture_raw INTEGER NOT NULL,
                soil_moisture_percent INTEGER NOT NULL,
                pump_running INTEGER NOT NULL,
                light_duty INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_readings_device_time
                ON readings(device_id, recorded_at);
            CREATE TABLE IF NOT EXISTS device_config (
                device_id TEXT PRIMARY KEY,
                config_json TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS device_state (
                device_id TEXT PRIMARY KEY,
                last_seen TEXT NOT NULL,
                pump_running INTEGER NOT NULL DEFAULT 0,
                light_duty INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS commands (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                action TEXT NOT NULL,
                duration_ms INTEGER NOT NULL DEFAULT 0,
                status TEXT NOT NULL DEFAULT 'pending',
                created_at TEXT NOT NULL,
                completed_at TEXT,
                result_message TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_commands_pending
                ON commands(device_id, status, id);
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                event_type TEXT NOT NULL,
                message TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            """
        )
        db.execute(
            "INSERT OR IGNORE INTO device_config VALUES (?, ?, ?)",
            (DEVICE_ID, DeviceConfig().model_dump_json(), timestamp()),
        )


@app.on_event("startup")
def startup() -> None:
    init_db()


def verify_device(x_device_token: Annotated[str | None, Header()] = None) -> None:
    if DEVICE_TOKEN and (x_device_token is None or not hmac.compare_digest(x_device_token, DEVICE_TOKEN)):
        raise HTTPException(status_code=401, detail="Invalid device token")


DeviceAuth = Annotated[None, Depends(verify_device)]


def ensure_config(device_id: str) -> dict:
    with connect_db() as db:
        row = db.execute("SELECT config_json FROM device_config WHERE device_id=?", (device_id,)).fetchone()
        if row is None:
            config = DeviceConfig().model_dump()
            db.execute(
                "INSERT INTO device_config VALUES (?, ?, ?)",
                (device_id, json.dumps(config), timestamp()),
            )
            return config
    return json.loads(row["config_json"])


def device_is_online(row: sqlite3.Row | None) -> bool:
    if row is None:
        return False
    try:
        last_seen = datetime.fromisoformat(row["last_seen"])
        return now_utc() - last_seen <= timedelta(seconds=ONLINE_TIMEOUT)
    except ValueError:
        return False


@app.get("/")
def dashboard() -> FileResponse:
    return FileResponse(APP_DIR / "static" / "index.html")


@app.get("/api/status")
def status(device_id: str = Query(DEVICE_ID)) -> dict:
    with connect_db() as db:
        state = db.execute("SELECT * FROM device_state WHERE device_id=?", (device_id,)).fetchone()
        latest = db.execute(
            "SELECT * FROM readings WHERE device_id=? ORDER BY id DESC LIMIT 1", (device_id,)
        ).fetchone()
        count = db.execute("SELECT COUNT(*) AS n FROM readings WHERE device_id=?", (device_id,)).fetchone()["n"]
    return {
        "device_id": device_id,
        "online": device_is_online(state),
        "last_seen": state["last_seen"] if state else None,
        "latest": dict(latest) if latest else None,
        "reading_count": count,
    }


@app.post("/api/sensors")
def receive_reading(reading: SensorReading, _: DeviceAuth) -> dict:
    received_at = timestamp()
    with connect_db() as db:
        db.execute(
            """INSERT INTO readings
            (device_id, recorded_at, air_temperature, air_humidity, light_lux,
             soil_moisture_raw, soil_moisture_percent, pump_running, light_duty)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (reading.device_id, received_at, reading.air_temperature, reading.air_humidity,
             reading.light_lux, reading.soil_moisture_raw, reading.soil_moisture_percent,
             int(reading.pump_running), reading.light_duty),
        )
        db.execute(
            """INSERT INTO device_state(device_id,last_seen,pump_running,light_duty)
            VALUES(?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET
            last_seen=excluded.last_seen,pump_running=excluded.pump_running,light_duty=excluded.light_duty""",
            (reading.device_id, received_at, int(reading.pump_running), reading.light_duty),
        )
    return {"accepted": True, "recorded_at": received_at}


@app.get("/api/readings")
def readings(
    device_id: str = Query(DEVICE_ID),
    hours: int = Query(24, ge=1, le=24 * 31),
    limit: int = Query(3000, ge=1, le=10000),
) -> list[dict]:
    cutoff = timestamp(now_utc() - timedelta(hours=hours))
    with connect_db() as db:
        rows = db.execute(
            "SELECT * FROM readings WHERE device_id=? AND recorded_at>=? ORDER BY id DESC LIMIT ?",
            (device_id, cutoff, limit),
        ).fetchall()
    return [dict(row) for row in reversed(rows)]


@app.get("/api/readings.csv")
def readings_csv(device_id: str = Query(DEVICE_ID), hours: int = Query(168, ge=1, le=744)) -> StreamingResponse:
    data = readings(device_id=device_id, hours=hours, limit=10000)
    output = io.StringIO()
    columns = ["recorded_at", "air_temperature", "air_humidity", "light_lux",
               "soil_moisture_raw", "soil_moisture_percent", "pump_running", "light_duty"]
    writer = csv.DictWriter(output, fieldnames=columns, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(data)
    return StreamingResponse(iter([output.getvalue()]), media_type="text/csv; charset=utf-8",
                             headers={"Content-Disposition": "attachment; filename=plant-readings.csv"})


@app.get("/api/config")
def get_config(device_id: str = Query(DEVICE_ID)) -> dict:
    return {"device_id": device_id, **ensure_config(device_id)}


@app.put("/api/config")
def update_config(config: DeviceConfig, device_id: str = Query(DEVICE_ID)) -> dict:
    with connect_db() as db:
        db.execute(
            "INSERT INTO device_config VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET config_json=excluded.config_json,updated_at=excluded.updated_at",
            (device_id, config.model_dump_json(), timestamp()),
        )
        db.execute("INSERT INTO events(device_id,event_type,message,created_at) VALUES(?,?,?,?)",
                   (device_id, "config", "Configuration updated from dashboard", timestamp()))
    return {"device_id": device_id, **config.model_dump()}


@app.post("/api/commands")
def create_command(request: CommandRequest) -> dict:
    if request.action not in {"pump_test", "light_on", "light_off", "light_auto"}:
        raise HTTPException(status_code=422, detail="Unsupported command")
    with connect_db() as db:
        state = db.execute("SELECT * FROM device_state WHERE device_id=?", (request.device_id,)).fetchone()
        if not device_is_online(state):
            raise HTTPException(status_code=409, detail="Device is offline; command not queued")
        pending = db.execute("SELECT id FROM commands WHERE device_id=? AND status IN ('pending','sent')",
                             (request.device_id,)).fetchone()
        if pending:
            raise HTTPException(status_code=409, detail="A device command is already pending")
        cursor = db.execute(
            "INSERT INTO commands(device_id,action,duration_ms,status,created_at) VALUES(?,?,?,'pending',?)",
            (request.device_id, request.action, request.duration_ms, timestamp()),
        )
        db.execute("INSERT INTO events(device_id,event_type,message,created_at) VALUES(?,?,?,?)",
                   (request.device_id, "command", f"Queued command: {request.action}", timestamp()))
    return {"id": cursor.lastrowid, "status": "pending", "action": request.action}


@app.get("/api/device/poll")
def device_poll(device_id: str = Query(DEVICE_ID), _: DeviceAuth = None) -> dict:
    current = timestamp()
    config = ensure_config(device_id)
    with connect_db() as db:
        db.execute(
            "INSERT INTO device_state(device_id,last_seen) VALUES(?,?) ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen",
            (device_id, current),
        )
        row = db.execute(
            "SELECT * FROM commands WHERE device_id=? AND status='pending' ORDER BY id LIMIT 1",
            (device_id,),
        ).fetchone()
        command = None
        if row:
            db.execute("UPDATE commands SET status='sent' WHERE id=?", (row["id"],))
            command = {"id": row["id"], "action": row["action"], "duration_ms": row["duration_ms"]}
    return {"config": config, "command": command, "server_time": current}


@app.post("/api/device/commands/{command_id}/result")
def command_result(command_id: int, result: CommandResult, _: DeviceAuth) -> dict:
    completed = timestamp()
    with connect_db() as db:
        row = db.execute("SELECT * FROM commands WHERE id=?", (command_id,)).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="Command not found")
        db.execute("UPDATE commands SET status=?,completed_at=?,result_message=? WHERE id=?",
                   ("completed" if result.result == "ok" else "failed", completed, result.message, command_id))
        db.execute("INSERT INTO events(device_id,event_type,message,created_at) VALUES(?,?,?,?)",
                   (row["device_id"], row["action"], f"{result.result}: {result.message}".strip(), completed))
    return {"accepted": True}


@app.get("/api/events")
def events(device_id: str = Query(DEVICE_ID), limit: int = Query(50, ge=1, le=500)) -> list[dict]:
    with connect_db() as db:
        rows = db.execute("SELECT * FROM events WHERE device_id=? ORDER BY id DESC LIMIT ?",
                          (device_id, limit)).fetchall()
        commands = db.execute("SELECT id,action,status,created_at,completed_at,result_message FROM commands WHERE device_id=? ORDER BY id DESC LIMIT ?",
                              (device_id, limit)).fetchall()
    combined = [dict(row) for row in rows]
    combined.extend({"id": row["id"], "event_type": "command_" + row["status"],
                     "message": f"{row['action']}: {row['result_message'] or row['status']}",
                     "created_at": row["completed_at"] or row["created_at"]} for row in commands)
    return sorted(combined, key=lambda item: item["created_at"], reverse=True)[:limit]
