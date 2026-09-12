import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BATCH = ROOT / "calmgr-batch"

class SQLiteBatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="appointment-tests-", dir=ROOT / "build")
        self.work = Path(self.temp.name); self.db = self.work / "appointments.db"
        (self.work / "calmgr.conf").write_text(
            f"DATABASE_FILE={self.db}\nBACKUP_FILE={self.work / 'backup.db'}\nSAFETY_FILE={self.work / 'safety.db'}\n"
            f"AUDIT_FILE={self.work / 'audit.log'}\nPRINT_FILE={self.work / 'APPT01.txt'}\n"
            f"LIST_PRINT_FILE={self.work / 'APPT02.txt'}\nCSV_FILE={self.work / 'out.csv'}\n"
            f"ICAL_FILE={self.work / 'out.ics'}\nLINES_PER_PAGE=20\nCANCEL_RETENTION_DAYS=0\n", encoding="utf-8")

    def tearDown(self): self.temp.cleanup()

    def run(self, payload=None, ok=True):
        if not isinstance(payload, dict):
            return super().run(payload)
        response = self.work / "response.json"
        completed = subprocess.run([str(BATCH), "-", str(response)], cwd=self.work,
            input=json.dumps(payload, indent=2) + "\n", text=True, capture_output=True, timeout=15)
        value = json.loads(response.read_text(encoding="utf-8"))
        self.assertEqual(ok, completed.returncode == 0, (completed.stderr, value)); self.assertEqual(ok, value["ok"], value)
        return value

    def add(self, subject="Subject"):
        return self.run({"command": "add", "start_date": "2026-09-01", "end_date": "2026-09-01",
                         "client": "Client", "subject": subject})

    def test_initialization_crud_duplicate_and_schema(self):
        self.run({"command": "init"}); con = sqlite3.connect(self.db)
        self.assertEqual(3, con.execute("PRAGMA user_version").fetchone()[0])
        self.assertEqual("wal", con.execute("PRAGMA journal_mode").fetchone()[0])
        columns = {row[1] for row in con.execute("PRAGMA table_info(appointments)")}
        self.assertIn("appointment_id", columns); self.assertIn("schema_version", columns); con.close()
        created = self.add()["appointments"][0]
        self.assertEqual(created["id"], self.run({"command": "show", "id": created["id"]})["appointments"][0]["id"])
        self.run({"command": "add", "start_date": "2026-09-01", "end_date": "2026-09-01",
                  "client": "Client", "subject": "Subject"}, ok=False)

    def test_series_transaction_rolls_back(self):
        self.run({"command": "init"}); con = sqlite3.connect(self.db)
        con.execute("CREATE TRIGGER reject_second BEFORE INSERT ON appointments WHEN NEW.appointment_id='000000002' BEGIN SELECT RAISE(ABORT,'injected'); END")
        con.commit(); con.close()
        self.run({"command": "add", "start_date": "2026-09-01", "end_date": "2026-09-01",
                  "client": "Client", "subject": "Series", "recurrence": "W", "series_until": "2026-09-15"}, ok=False)
        con = sqlite3.connect(self.db); self.assertEqual(0, con.execute("SELECT count(*) FROM appointments").fetchone()[0]); con.close()

    def test_busy_timeout(self):
        self.run({"command": "init"}); lock = sqlite3.connect(self.db); lock.execute("BEGIN IMMEDIATE")
        started = time.monotonic()
        self.run({"command": "add", "start_date": "2026-09-01", "end_date": "2026-09-01",
                  "client": "Client", "subject": "Busy"}, ok=False)
        elapsed = time.monotonic() - started; lock.rollback(); lock.close()
        self.assertGreaterEqual(elapsed, 2.0); self.assertLess(elapsed, 8.0)

    def test_backup_restore_and_reset_safety(self):
        self.add("Before"); self.run({"command": "backup"}); self.add("After")
        self.run({"command": "restore", "confirm": "RESTORE"})
        self.assertEqual(1, len(self.run({"command": "list"})["appointments"]))
        self.run({"command": "reset-database"}, ok=False)
        self.run({"command": "reset-database", "confirm": "ERASE"})
        self.assertTrue((self.work / "safety.db").is_file())
        self.assertEqual(0, len(self.run({"command": "list"})["appointments"]))

    def test_damaged_database_is_rejected(self):
        self.db.write_bytes(b"not a sqlite database"); self.run({"command": "init"}, ok=False)

    def test_wrong_restore_database_is_rejected_without_replacing_live(self):
        self.add("Preserved")
        backup = self.work / "backup.db"
        if backup.exists(): backup.unlink()
        wrong = sqlite3.connect(backup); wrong.execute("CREATE TABLE unrelated(value TEXT)"); wrong.commit(); wrong.close()
        self.run({"command": "restore", "confirm": "RESTORE"}, ok=False)
        self.assertEqual("Preserved", self.run({"command": "list"})["appointments"][0]["subject"])

    def test_users_authentication_and_no_secret_output(self):
        password = "a-strong-admin-password"
        self.run({"command": "user-bootstrap", "username": "admin", "password": password, "confirm": "CREATE-ADMIN"})
        self.run({"command": "user-bootstrap", "username": "other", "password": password, "confirm": "CREATE-ADMIN"}, ok=False)
        self.run({"command": "user-add", "actor_username": "admin", "actor_password": password,
                  "username": "normal.user", "password": "a-strong-user-password", "role": "user"})
        auth = self.run({"command": "auth-login", "username": "normal.user", "password": "a-strong-user-password"})
        self.assertEqual({"user_id", "username", "role"}, set(auth["user"]))
        bad = self.run({"command": "auth-login", "username": "normal.user", "password": "wrong-password-value"}, ok=False)
        self.assertNotIn("normal.user", bad["message"])
        users = self.run({"command": "user-list", "actor_username": "admin", "actor_password": password})
        self.assertNotIn("password", json.dumps(users).lower())
        con = sqlite3.connect(self.db); stored = con.execute("SELECT password_hash FROM users WHERE username='admin'").fetchone()[0]
        self.assertTrue(stored.startswith("$argon2id$")); self.assertNotEqual(password, stored)
        self.assertGreaterEqual(con.execute("SELECT count(*) FROM audit_events").fetchone()[0], 2); con.close()

    def test_user_validation_disable_enable_last_admin_and_password(self):
        admin = "another-strong-admin-password"
        self.run({"command": "user-bootstrap", "username": "admin", "password": admin, "confirm": "CREATE-ADMIN"})
        self.run({"command": "user-add", "actor_username": "admin", "actor_password": admin,
                  "username": "bad name", "password": "a-strong-user-password", "role": "user"}, ok=False)
        self.run({"command": "user-disable", "actor_username": "admin", "actor_password": admin, "username": "admin"}, ok=False)
        self.run({"command": "user-add", "actor_username": "admin", "actor_password": admin,
                  "username": "second-admin", "password": "second-strong-password", "role": "admin"})
        self.run({"command": "user-disable", "actor_username": "admin", "actor_password": admin, "username": "second-admin"})
        self.run({"command": "auth-login", "username": "second-admin", "password": "second-strong-password"}, ok=False)
        self.run({"command": "user-enable", "actor_username": "admin", "actor_password": admin, "username": "second-admin"})
        self.run({"command": "user-password", "username": "second-admin", "password": "second-strong-password",
                  "new_password": "replacement-password"})
        self.run({"command": "auth-login", "username": "second-admin", "password": "replacement-password"})

    def test_login_attempts_are_limited(self):
        password = "rate-limit-admin-password"
        self.run({"command": "user-bootstrap", "username": "admin", "password": password,
                  "confirm": "CREATE-ADMIN"})
        for _ in range(5):
            self.run({"command": "auth-login", "username": "admin",
                      "password": "incorrect-password"}, ok=False)
        self.run({"command": "auth-login", "username": "admin", "password": password}, ok=False)

if __name__ == "__main__": unittest.main()
