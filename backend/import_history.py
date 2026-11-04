import csv
import json
import sqlite3
from contextlib import closing
from datetime import datetime, timedelta, timezone

from app.main import BACKEND_DIR, DATABASE_PATH, DEVICE_ID, connect_db, init_db, timestamp


def import_history() -> dict:
    init_db()
    migration = "history_2026_10_21_27_v1"
    with closing(connect_db()) as db, db:
        db.execute("CREATE TABLE IF NOT EXISTS data_migrations (name TEXT PRIMARY KEY, applied_at TEXT NOT NULL, details TEXT NOT NULL)")
        previous = db.execute("SELECT details FROM data_migrations WHERE name=?", (migration,)).fetchone()
        if previous:
            return {"already_imported": True, **json.loads(previous["details"])}
    backup_path = DATABASE_PATH.with_name(f"{DATABASE_PATH.stem}.before-history-{datetime.now():%Y%m%d-%H%M%S-%f}.sqlite3")
    with closing(connect_db()) as source, closing(sqlite3.connect(backup_path)) as backup:
        source.backup(backup)
    local_zone = timezone(timedelta(hours=8))
    start = datetime(2026, 10, 21, tzinfo=local_zone).astimezone(timezone.utc)
    end = datetime(2026, 10, 28, tzinfo=local_zone).astimezone(timezone.utc)
    with (BACKEND_DIR / "data" / "demo_week_scenario.csv").open(newline="", encoding="utf-8") as source:
        records = list(csv.DictReader(source))
    if len(records) != 56:
        raise ValueError("Expected exactly 56 readings for seven days")
    rows = []
    for index, record in enumerate(records):
        rows.append((DEVICE_ID, timestamp(start + timedelta(hours=index * 3)),
                     *[None if record[key] == "NA" else float(record[key])
                       for key in ("air_temperature", "air_humidity", "light_lux")],
                     *[int(record[key]) for key in ("soil_moisture_raw", "soil_moisture_percent", "pump_running", "light_duty")]))
    with closing(connect_db()) as db, db:
        db.execute("BEGIN IMMEDIATE")
        if db.execute("SELECT 1 FROM data_migrations WHERE name=?", (migration,)).fetchone():
            raise RuntimeError("History import has already completed in another process")
        conflicts = db.execute(
            "SELECT id, recorded_at FROM readings WHERE device_id=? AND recorded_at>=? AND recorded_at<? ORDER BY recorded_at, id",
            (DEVICE_ID, timestamp(start), timestamp(end)),
        ).fetchall()
        shifted = []
        if conflicts:
            earliest = datetime.fromisoformat(conflicts[0]["recorded_at"])
            offset = timedelta(days=(end.astimezone(local_zone).date() - earliest.astimezone(local_zone).date()).days)
            for record in conflicts:
                moved_time = timestamp(datetime.fromisoformat(record["recorded_at"]) + offset)
                db.execute("UPDATE readings SET recorded_at=? WHERE id=?", (moved_time, record["id"]))
                shifted.append({"id": record["id"], "original": record["recorded_at"], "updated": moved_time})
        removed = db.execute("DELETE FROM readings WHERE device_id='plant-box-demo'").rowcount
        db.executemany(
            "INSERT INTO readings (device_id, recorded_at, air_temperature, air_humidity, light_lux, soil_moisture_raw, soil_moisture_percent, pump_running, light_duty) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            rows,
        )
        details = {"samples": len(rows), "shifted_records": len(shifted), "removed_old_demo_records": removed,
                   "backup": str(backup_path), "source": "demo_week_scenario.csv", "shifted_timestamps": shifted}
        db.execute("INSERT INTO data_migrations VALUES (?, ?, ?)", (migration, timestamp(), json.dumps(details)))
    return details


if __name__ == "__main__":
    result = import_history()
    print(json.dumps({key: value for key, value in result.items() if key != "shifted_timestamps"}, ensure_ascii=True, indent=2))
