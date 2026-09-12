import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from datetime import timedelta
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("appointment_web", ROOT / "web" / "web_app.py")
web = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(web)

class WebTests(unittest.TestCase):
    def setUp(self):
        web.app.config.update(TESTING=True, SECRET_KEY="test-only-secret")
        self.client = web.app.test_client()

    def test_examples_are_controlled_json_objects(self):
        for path in (ROOT / "examples").glob("*-request.json"):
            value = json.loads(path.read_text(encoding="utf-8"))
            self.assertIsInstance(value, dict)
            self.assertIsInstance(value.get("command"), str)

    def test_default_web_configuration(self):
        self.assertEqual("127.0.0.1", web.WEB_HOST)
        self.assertEqual(5080, web.WEB_PORT)
        self.assertEqual(timedelta(hours=8), web.app.config["PERMANENT_SESSION_LIFETIME"])

    def test_invalid_port_aborts_import(self):
        env = os.environ.copy(); env["WEB_PORT"] = "5000x"
        result = subprocess.run([sys.executable, str(ROOT / "web" / "web_app.py")],
                                cwd=ROOT, env=env, capture_output=True, text=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("WEB_PORT must be an integer between 1024 and 65535", result.stderr)

    def test_every_application_route_requires_login(self):
        for path in ("/", "/new", "/maintenance", "/appointment/000000001",
                     "/appointment/000000001/edit", "/missing"):
            response = self.client.get(path)
            self.assertEqual(302, response.status_code, path)
            self.assertTrue(response.headers["Location"].endswith("/login"), path)
        self.assertEqual(200, self.client.get("/login").status_code)

    def test_login_logout_and_cookie_controls(self):
        auth = {"ok": True, "appointments": [],
                "user": {"user_id": "1", "username": "admin", "role": "admin"}}
        listing = {"ok": True, "appointments": []}
        with self.client.session_transaction() as state: state["csrf"] = "token"
        with patch.object(web, "call_batch", side_effect=[auth, auth, listing, auth]):
            response = self.client.post("/login", data={"csrf": "token", "username": "admin",
                                                        "password": "long-enough-password"})
            self.assertEqual(302, response.status_code)
            cookie = response.headers.get("Set-Cookie", "")
            self.assertIn("HttpOnly", cookie); self.assertIn("SameSite=Strict", cookie); self.assertIn("Expires=", cookie)
            self.assertEqual(200, self.client.get("/").status_code)
            with self.client.session_transaction() as state: token = state["csrf"]
            response = self.client.post("/logout", data={"csrf": token})
            self.assertTrue(response.headers["Location"].endswith("/login"))
        self.assertEqual(302, self.client.get("/").status_code)

    def test_login_failure_is_generic(self):
        with self.client.session_transaction() as state: state["csrf"] = "token"
        with patch.object(web, "call_batch", side_effect=web.BatchError("backend detail")):
            response = self.client.post("/login", data={"csrf": "token", "username": "nobody",
                                                        "password": "incorrect-password"})
        self.assertEqual(401, response.status_code)
        self.assertIn(b"Invalid username or password", response.data)
        self.assertNotIn(b"backend detail", response.data)

    def test_no_password_logging_or_hashing_in_python(self):
        source = (ROOT / "web" / "web_app.py").read_text(encoding="utf-8")
        self.assertNotIn("password_hash", source); self.assertNotIn("argon2", source.lower())
        self.assertNotIn("shell=True", source); self.assertIn('[str(BATCH), "-", str(response_path)]', source)

    def test_config_override(self):
        with tempfile.TemporaryDirectory() as name:
            config = Path(name) / "manager.conf"
            config.write_text("WEB_HOST=127.0.0.1\nWEB_PORT=6200\n", encoding="utf-8")
            with patch.object(web, "CONFIG_FILE", config), patch.dict(os.environ, {}, clear=True):
                self.assertEqual(("127.0.0.1", 6200), web.read_web_config())

if __name__ == "__main__": unittest.main()
