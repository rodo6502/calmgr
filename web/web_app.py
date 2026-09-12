"""Authenticated local web presentation layer for CALMGR.

The COBOL batch program remains the system of record.  This module only adds a
richer local GUI (list, week/month calendar, trash, exports, backup and audit)
on top of the existing batch interface.
"""
from __future__ import annotations

import calendar
import csv
from datetime import date, datetime, timedelta
import io
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import subprocess
import tempfile
import threading
from typing import Any

from flask import (
    Flask, Response, abort, flash, redirect, render_template, request,
    send_file, session, url_for,
)

PROJECT_DIR = Path(__file__).resolve().parents[1]
CONFIG_FILE = Path(os.environ.get("CALMGR_CONFIG", os.environ.get("APPOINTMENT_CONFIG", PROJECT_DIR / "calmgr.conf")))
BATCH = Path(os.environ.get("CALMGR_BATCH", os.environ.get("APPOINTMENT_BATCH", PROJECT_DIR / "calmgr-batch"))).resolve()
TIMEOUT = float(os.environ.get("CALMGR_BATCH_TIMEOUT", os.environ.get("APPOINTMENT_BATCH_TIMEOUT", "20")))
LOCK = threading.Lock()
COMMANDS = {
    "list", "show", "add", "edit", "cancel", "reactivate", "delete",
    "backup", "restore", "reset-database", "print-export", "print-list-export",
    "csv-export", "csv-import", "ical-export", "init", "user-bootstrap",
    "user-add", "user-list", "user-password", "user-disable", "user-enable",
    "auth-login", "auth-check",
}
ID_RE = re.compile(r"^\d{9}$")
AUDIT_RE = re.compile(
    r"^(?P<time>[^|]+?)\s*\|\s*(?P<action>[^|]+?)\s*\|\s*ID=(?P<id>\d{9})\s*\|\s*SOURCE=(?P<source>[^|]+?)\s*\|\s*(?P<detail>.*)$"
)


class ConfigurationError(RuntimeError):
    pass


class BatchError(RuntimeError):
    pass


def read_web_config() -> tuple[str, int]:
    values = {"WEB_HOST": "127.0.0.1", "WEB_PORT": "5080"}
    try:
        lines = CONFIG_FILE.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        lines = []
    except OSError as exc:
        raise ConfigurationError(f"Configuration file could not be read: {exc}") from exc
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ConfigurationError(f"Invalid configuration line {number}")
        key, value = (part.strip() for part in line.split("=", 1))
        if key in values:
            values[key] = value
    host = os.environ.get("WEB_HOST", values["WEB_HOST"])
    port_text = os.environ.get("WEB_PORT", values["WEB_PORT"])
    if not host or any(ord(ch) < 33 or ord(ch) > 126 for ch in host):
        raise ConfigurationError("WEB_HOST must be a non-empty printable hostname or address")
    try:
        port = int(port_text, 10)
    except ValueError as exc:
        raise ConfigurationError("WEB_PORT must be an integer between 1024 and 65535") from exc
    if not 1024 <= port <= 65535 or str(port) != port_text.strip():
        raise ConfigurationError("WEB_PORT must be an integer between 1024 and 65535")
    return host, port


def config_values() -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = CONFIG_FILE.read_text(encoding="utf-8").splitlines()
    except OSError:
        return values
    for raw in lines:
        line = raw.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            values[key.strip().upper()] = value.strip()
    return values


def config_path(key: str, default: str) -> Path:
    raw = config_values().get(key, default)
    path = Path(raw)
    return path if path.is_absolute() else PROJECT_DIR / path


WEB_HOST, WEB_PORT = read_web_config()
app = Flask(
    __name__,
    template_folder=str(PROJECT_DIR / "web" / "templates"),
    static_folder=str(PROJECT_DIR / "web" / "static"),
)
app.secret_key = os.environ.get("CALMGR_WEB_SECRET") or os.environ.get("APPOINTMENT_WEB_SECRET") or secrets.token_hex(32)
app.config.update(
    MAX_CONTENT_LENGTH=2 * 1024 * 1024,
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Strict",
    PERMANENT_SESSION_LIFETIME=timedelta(hours=8),
)


def call_batch(payload: dict[str, str]) -> dict[str, Any]:
    if payload.get("command", "") not in COMMANDS:
        raise BatchError("Unsupported operation")
    if not BATCH.is_file() or not os.access(BATCH, os.X_OK):
        raise BatchError("The COBOL batch program is unavailable; run make all")
    clean_payload = {str(k): str(v) for k, v in payload.items()}
    clean_payload.setdefault("source", "WEB")
    request_json = json.dumps(clean_payload, ensure_ascii=False, indent=2) + "\n"
    with LOCK, tempfile.TemporaryDirectory(prefix="appointment-web-") as temp_name:
        response_path = Path(temp_name) / "response.json"
        descriptor = os.open(response_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(descriptor)
        try:
            completed = subprocess.run(
                [str(BATCH), "-", str(response_path)], cwd=PROJECT_DIR,
                input=request_json, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", errors="replace", timeout=TIMEOUT,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise BatchError("The appointment operation timed out") from exc
        except OSError as exc:
            raise BatchError("The appointment operation could not be started") from exc
        try:
            result = json.loads(response_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise BatchError("The appointment service returned an invalid response") from exc
    if not isinstance(result, dict) or not isinstance(result.get("ok"), bool):
        raise BatchError("The appointment service returned an invalid response schema")
    if completed.returncode != 0 or not result["ok"]:
        raise BatchError(str(result.get("message") or "Appointment operation failed")[:200])
    if not isinstance(result.get("appointments"), list):
        raise BatchError("The appointment service returned invalid appointment data")
    return result


def csrf() -> str:
    value = session.get("csrf")
    if not isinstance(value, str):
        value = secrets.token_urlsafe(32)
        session["csrf"] = value
    return value


@app.before_request
def security_checks() -> Any:
    if request.remote_addr not in {"127.0.0.1", "::1", None}:
        abort(403)
    if request.method == "POST" and not secrets.compare_digest(request.form.get("csrf", ""), csrf()):
        abort(400, "Invalid request token")
    if request.endpoint in {"login", "static"}:
        return None
    user_id = session.get("user_id")
    if not isinstance(user_id, str):
        return redirect(url_for("login"))
    try:
        result = call_batch({"command": "auth-check", "user_id": user_id})
    except BatchError:
        session.clear()
        return redirect(url_for("login"))
    user = result.get("user")
    if not isinstance(user, dict) or user.get("user_id") != user_id:
        session.clear()
        return redirect(url_for("login"))
    session["username"] = str(user.get("username", ""))
    session["role"] = str(user.get("role", ""))
    return None


def display_date(value: Any) -> str:
    text = str(value or "")
    if len(text) == 8 and text.isdigit() and text != "00000000":
        return f"{text[6:8]}.{text[4:6]}.{text[0:4]}"
    return ""


def iso_date(value: date) -> str:
    return value.strftime("%Y-%m-%d")


def parse_anchor(value: str | None, default: date | None = None) -> date:
    if value:
        try:
            return date.fromisoformat(value)
        except ValueError:
            abort(400, "Invalid calendar date")
    return default or date.today()


def checked_id(value: str) -> str:
    if not ID_RE.fullmatch(value):
        abort(404)
    return value


def list_appointments(**filters: str) -> list[dict[str, Any]]:
    payload = {"command": "list"}
    for key in ("mode", "query", "from_date", "to_date", "status", "series_id", "sort"):
        value = filters.get(key, "")
        if value:
            payload[key] = value
    result = call_batch(payload)
    return [item for item in result["appointments"] if isinstance(item, dict)]


def parse_audit() -> list[dict[str, str]]:
    path = config_path("AUDIT_FILE", "data/appointment-audit.log")
    if not path.is_file():
        return []
    entries: list[dict[str, str]] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    for number, line in enumerate(lines, 1):
        match = AUDIT_RE.match(line.strip())
        if match:
            item = {key: value.strip() for key, value in match.groupdict().items()}
            item["line"] = str(number)
            item["valid"] = "yes"
        else:
            item = {"time": "", "action": "", "id": "", "source": "", "detail": line,
                    "line": str(number), "valid": "no"}
        entries.append(item)
    entries.reverse()
    return entries


@app.context_processor
def helpers() -> dict[str, Any]:
    today = date.today()
    monday = today - timedelta(days=today.weekday())
    return {
        "csrf_token": csrf,
        "signed_in_user": session.get("username"),
        "display_date": display_date,
        "today_iso": today.isoformat(),
        "today_display": today.strftime("%d.%m.%Y"),
        "this_week_from": monday.strftime("%d.%m.%Y"),
        "this_week_to": (monday + timedelta(days=6)).strftime("%d.%m.%Y"),
        "next_week_iso": (monday + timedelta(days=7)).isoformat(),
    }


@app.route("/login", methods=["GET", "POST"])
def login() -> Any:
    if request.method == "POST":
        try:
            result = call_batch({
                "command": "auth-login",
                "username": request.form.get("username", ""),
                "password": request.form.get("password", ""),
            })
            user = result.get("user")
            if not isinstance(user, dict):
                raise BatchError("Invalid username or password")
        except BatchError:
            flash("Invalid username or password", "error")
            return render_template("login.html"), 401
        session.clear()
        session.permanent = True
        session["user_id"] = str(user["user_id"])
        session["username"] = str(user["username"])
        session["role"] = str(user["role"])
        csrf()
        return redirect(url_for("index"))
    return render_template("login.html")


@app.post("/logout")
def logout() -> Any:
    session.clear()
    return redirect(url_for("login"))


@app.get("/")
@app.get("/appointments")
def index() -> Any:
    mode = request.args.get("mode", "current")
    if mode not in {"current", "all"}:
        mode = "current"
    status_ui = request.args.get("status", "all")
    status = {"active": "A", "cancelled": "C"}.get(status_ui, "")
    sort = request.args.get("sort", "date")
    if sort not in {"date", "client", "subject", "id", "status"}:
        sort = "date"
    filters = {
        "mode": mode,
        "client": request.args.get("client", "")[:60],
        "subject": request.args.get("subject", "")[:100],
        "from_date": request.args.get("from_date", "")[:10],
        "to_date": request.args.get("to_date", "")[:10],
        "iso_year": request.args.get("iso_year", "")[:4],
        "iso_week": request.args.get("iso_week", "")[:2],
        "status": status_ui,
        "series_id": request.args.get("series_id", "")[:9],
        "sort": sort,
    }
    items = list_appointments(
        mode=mode,
        from_date=filters["from_date"],
        to_date=filters["to_date"],
        status=status,
        series_id=filters["series_id"],
        sort="date" if sort == "status" else sort,
    )
    client_q = filters["client"].casefold().strip()
    subject_q = filters["subject"].casefold().strip()
    if client_q:
        items = [x for x in items if client_q in str(x.get("client", "")).casefold()]
    if subject_q:
        items = [x for x in items if subject_q in str(x.get("subject", "")).casefold()]
    if filters["iso_year"]:
        items = [x for x in items if str(x.get("iso_year", "")) == filters["iso_year"]]
    if filters["iso_week"]:
        wanted = filters["iso_week"].lstrip("0") or "0"
        items = [x for x in items if str(x.get("iso_week", "")).lstrip("0") == wanted]
    if sort == "status":
        items.sort(key=lambda x: (str(x.get("status", "")), str(x.get("start_date", ""))))
    return render_template("appointments.html", items=items, count=len(items), mode=mode, filters=filters)


@app.route("/new", methods=["GET", "POST"])
@app.route("/appointments/new", methods=["GET", "POST"])
def new() -> Any:
    item: dict[str, str] = {"start_date": "", "end_date": "", "client": "", "subject": "",
                            "recurrence": "N", "series_until": ""}
    if request.method == "GET":
        copy_id = request.args.get("copy", "")
        if copy_id and ID_RE.fullmatch(copy_id):
            copied = call_batch({"command": "show", "id": copy_id})["appointments"][0]
            item.update(
                start_date=display_date(copied.get("start_date")),
                end_date=display_date(copied.get("end_date")),
                client=str(copied.get("client", "")), subject=str(copied.get("subject", "")),
                recurrence=str(copied.get("recurrence", "N")),
                series_until=display_date(copied.get("series_until")),
            )
    if request.method == "POST":
        payload = {"command": "add"}
        for name in ("start_date", "end_date", "client", "subject", "recurrence", "series_until"):
            value = request.form.get(name, "").strip()
            if value:
                payload[name] = value
            item[name] = value
        result = call_batch(payload)
        flash(result["message"], "success")
        created = result.get("appointments", [])
        if created and isinstance(created[0], dict) and created[0].get("id"):
            return redirect(url_for("show", appointment_id=created[0]["id"]))
        return redirect(url_for("index", mode="all"))
    return render_template("form.html", item=item, action="Create", add=True)


@app.get("/appointment/<appointment_id>")
@app.get("/appointments/<appointment_id>")
def show(appointment_id: str) -> Any:
    appointment_id = checked_id(appointment_id)
    result = call_batch({"command": "show", "id": appointment_id})
    item = result["appointments"][0]
    series_count = 1
    series_id = str(item.get("series_id", ""))
    if series_id and series_id != "000000000":
        series_count = len(list_appointments(mode="all", series_id=series_id, sort="date"))
    return render_template("show.html", item=item, series_count=series_count)


@app.route("/appointment/<appointment_id>/edit", methods=["GET", "POST"])
@app.route("/appointments/<appointment_id>/edit", methods=["GET", "POST"])
def edit(appointment_id: str) -> Any:
    appointment_id = checked_id(appointment_id)
    item = call_batch({"command": "show", "id": appointment_id})["appointments"][0]
    if request.method == "POST":
        payload = {
            "command": "edit", "id": appointment_id,
            "expected_revision": str(item["revision"]),
            "scope": request.form.get("scope", "single"),
        }
        for name in ("start_date", "end_date", "client", "subject", "series_until"):
            value = request.form.get(name, "").strip()
            if value:
                payload[name] = value
        result = call_batch(payload)
        flash(result["message"], "success")
        return redirect(url_for("show", appointment_id=appointment_id))
    view_item = dict(item)
    view_item["start_date"] = display_date(item.get("start_date"))
    view_item["end_date"] = display_date(item.get("end_date"))
    view_item["series_until"] = display_date(item.get("series_until"))
    return render_template("form.html", item=view_item, action="Edit", add=False)


@app.post("/appointment/<appointment_id>/<action>")
def mutate(appointment_id: str, action: str) -> Any:
    appointment_id = checked_id(appointment_id)
    if action not in {"cancel", "reactivate", "delete"}:
        abort(404)
    payload = {
        "command": action, "id": appointment_id,
        "scope": request.form.get("scope", "single"),
        "cancel_note": request.form.get("cancel_note", ""),
    }
    if action == "delete":
        payload["confirm"] = request.form.get("confirm", "")
    result = call_batch(payload)
    flash(result["message"], "success")
    return redirect(url_for("index", mode="all"))


@app.get("/calendar/week")
def calendar_week() -> Any:
    anchor = parse_anchor(request.args.get("date"))
    monday = anchor - timedelta(days=anchor.weekday())
    sunday = monday + timedelta(days=6)
    items = list_appointments(mode="all", from_date=iso_date(monday), to_date=iso_date(sunday), sort="date")
    days: list[tuple[date, list[dict[str, Any]]]] = []
    for offset in range(7):
        day = monday + timedelta(days=offset)
        ymd = day.strftime("%Y%m%d")
        days.append((day, [x for x in items if str(x.get("start_date", "")) <= ymd <= str(x.get("end_date", ""))]))
    return render_template(
        "calendar_week.html", monday=monday, sunday=sunday, days=days, today=date.today(),
        previous=monday - timedelta(days=7), next=monday + timedelta(days=7),
    )


@app.get("/calendar/month")
def calendar_month() -> Any:
    anchor = parse_anchor(request.args.get("date"))
    first = anchor.replace(day=1)
    next_month = (first.replace(day=28) + timedelta(days=4)).replace(day=1)
    previous = first - timedelta(days=1)
    previous = previous.replace(day=1)
    cal = calendar.Calendar(firstweekday=0)
    weeks = cal.monthdatescalendar(first.year, first.month)
    range_start, range_end = weeks[0][0], weeks[-1][-1]
    items = list_appointments(mode="all", from_date=iso_date(range_start), to_date=iso_date(range_end), sort="date")
    week_rows: list[dict[str, Any]] = []
    for week in weeks:
        iso = week[0].isocalendar()
        cells = []
        for day in week:
            ymd = day.strftime("%Y%m%d")
            cells.append({
                "day": day,
                "current_month": day.month == first.month,
                "appts": [x for x in items if str(x.get("start_date", "")) <= ymd <= str(x.get("end_date", ""))],
            })
        week_rows.append({"iso_week": iso.week, "cells": cells})
    return render_template(
        "calendar_month.html", anchor=first, weeks=week_rows, today=date.today(),
        previous=previous, next=next_month,
    )


@app.get("/trash")
def trash_view() -> Any:
    items = list_appointments(mode="all", status="C", sort="date")
    query = request.args.get("q", "").casefold().strip()
    if query:
        items = [x for x in items if query in (str(x.get("client", "")) + " " + str(x.get("subject", ""))).casefold()]
    cancellations: dict[str, str] = {}
    for entry in parse_audit():
        if entry["action"].upper() == "CANCEL" and entry["id"] not in cancellations:
            cancellations[entry["id"]] = entry["time"]
    retention = int(config_values().get("CANCEL_RETENTION_DAYS", "90") or "90")
    return render_template("trash.html", items=items, cancellations=cancellations, retention=retention, query=request.args.get("q", ""))


@app.route("/export", methods=["GET", "POST"])
def export_view() -> Any:
    current_year = date.today().year
    result = None
    first_year = request.form.get("first_year", str(current_year))
    last_year = request.form.get("last_year", str(current_year))
    report_type = request.form.get("report_type", "appointments")
    if report_type not in {"appointments", "calendar"}:
        report_type = "appointments"
    if request.method == "POST":
        if not (first_year.isdigit() and last_year.isdigit() and len(first_year) == 4 and len(last_year) == 4):
            flash("Years must contain four digits.", "error")
        else:
            command = "print-list-export" if report_type == "appointments" else "print-export"
            result = call_batch({
                "command": command,
                "from_date": f"{first_year}-01-01", "to_date": f"{last_year}-12-31",
            })
            flash(result["message"], "success")
    return render_template(
        "export.html", first_year=first_year, last_year=last_year,
        report_type=report_type, result=result,
    )


def send_batch_output(payload: dict[str, str], mimetype: str, download_name: str) -> Any:
    result = call_batch(payload)
    output = str(result.get("output_path", "")).strip()
    if not output:
        raise BatchError("The export did not return an output file")
    path = Path(output)
    if not path.is_absolute():
        path = PROJECT_DIR / path
    path = path.resolve()
    if not path.is_file():
        raise BatchError("The export file was not created")
    return send_file(path, mimetype=mimetype, as_attachment=True, download_name=download_name, max_age=0)


@app.get("/export/print.txt")
def print_export_download() -> Any:
    year = date.today().year
    first = request.args.get("first_year", str(year))
    last = request.args.get("last_year", first)
    report_type = request.args.get("report_type", "appointments")
    if report_type == "calendar":
        command = "print-export"
        filename = "appointment-calendar.txt"
    else:
        command = "print-list-export"
        filename = "streamlined-appointments.txt"
    return send_batch_output(
        {"command": command, "from_date": f"{first}-01-01", "to_date": f"{last}-12-31"},
        "text/plain; charset=utf-8", filename,
    )


@app.get("/csv/export")
def csv_export() -> Any:
    payload = {"command": "csv-export"}
    if request.args.get("status") in {"A", "C"}:
        payload["status"] = request.args["status"]
    for name in ("from_date", "to_date"):
        if request.args.get(name):
            payload[name] = request.args[name]
    return send_batch_output(payload, "text/csv; charset=utf-8", "appointments.csv")


@app.get("/calendar/export")
def ical_export() -> Any:
    payload = {"command": "ical-export"}
    if request.args.get("status") in {"A", "C"}:
        payload["status"] = request.args["status"]
    for name in ("from_date", "to_date"):
        if request.args.get(name):
            payload[name] = request.args[name]
    return send_batch_output(payload, "text/calendar; charset=utf-8", "appointments.ics")


@app.post("/csv-import")
def csv_import() -> Any:
    upload = request.files.get("file")
    if upload is None or not upload.filename:
        abort(400, "CSV file is required")
    import_root = PROJECT_DIR / "imports"
    import_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="web-", dir=import_root) as temp_name:
        target = Path(temp_name) / "input.csv"
        upload.save(target)
        os.chmod(target, 0o600)
        result = call_batch({"command": "csv-import", "path": target.relative_to(PROJECT_DIR).as_posix()})
    flash(result["message"], "success")
    return redirect(url_for("export_view"))


@app.route("/backup", methods=["GET", "POST"])
def backup_restore() -> Any:
    if request.method == "POST":
        action = request.form.get("action", "")
        if action == "backup":
            result = call_batch({"command": "backup"})
        elif action == "restore":
            result = call_batch({"command": "restore", "confirm": request.form.get("confirm", "")})
        else:
            abort(400, "Invalid backup operation")
        flash(result["message"], "success")
        return redirect(url_for("backup_restore"))
    path = config_path("BACKUP_FILE", "data/appointments.backup.db")
    info: dict[str, Any] = {"exists": path.is_file(), "size": 0, "created": None, "count": None}
    if path.is_file():
        stat = path.stat()
        info["size"] = stat.st_size
        info["created"] = datetime.fromtimestamp(stat.st_mtime)
        try:
            con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
            info["count"] = con.execute("SELECT count(*) FROM appointments").fetchone()[0]
            con.close()
        except sqlite3.Error:
            info["count"] = "?"
    return render_template("backup.html", info=info)


@app.get("/audit")
def audit_view() -> Any:
    filters = {name: request.args.get(name, "").strip() for name in ("id", "action", "source")}
    entries = parse_audit()
    if filters["id"]:
        entries = [x for x in entries if filters["id"] in x["id"]]
    if filters["action"]:
        needle = filters["action"].casefold()
        entries = [x for x in entries if needle in x["action"].casefold()]
    if filters["source"]:
        entries = [x for x in entries if x["source"].upper() == filters["source"].upper()]
    return render_template("audit.html", entries=entries, filters=filters)


@app.get("/audit/download.<format>")
def audit_download(format: str) -> Any:
    entries = parse_audit()
    if format == "txt":
        text = "\n".join(f"{x['time']} | {x['action']} | ID={x['id']} | SOURCE={x['source']} | {x['detail']}" for x in entries)
        return Response(text + ("\n" if text else ""), mimetype="text/plain", headers={"Content-Disposition": "attachment; filename=audit.txt"})
    if format == "csv":
        stream = io.StringIO()
        writer = csv.writer(stream, delimiter=";")
        writer.writerow(["time", "action", "id", "source", "detail"])
        for x in entries:
            writer.writerow([x["time"], x["action"], x["id"], x["source"], x["detail"]])
        return Response(stream.getvalue(), mimetype="text/csv", headers={"Content-Disposition": "attachment; filename=audit.csv"})
    abort(404)


@app.route("/maintenance", methods=["GET", "POST"])
def maintenance() -> Any:
    if request.method == "POST":
        command = request.form.get("command", "")
        payload = {"command": command}
        for name in ("from_date", "to_date"):
            value = request.form.get(name, "").strip()
            if value:
                payload[name] = value
        if command in {"restore", "reset-database"}:
            payload["confirm"] = request.form.get("confirm", "")
        result = call_batch(payload)
        flash(f"{result['message']} {result.get('output_path', '')}".strip(), "success")
        return redirect(url_for("maintenance"))
    return render_template("maintenance.html")


@app.errorhandler(BatchError)
def batch_error(error: BatchError) -> tuple[str, int]:
    return render_template("error.html", message=str(error)), 502


if __name__ == "__main__":
    app.run(host=WEB_HOST, port=WEB_PORT, debug=False)
