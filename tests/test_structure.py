from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ArchitectureTests(unittest.TestCase):
    def test_frontends_do_not_manage_persistence(self):
        for name in ("calmgr.cob", "calmgr-batch.cob"):
            text = (ROOT / "src" / "programs" / name).read_text(encoding="utf-8").upper()
            self.assertNotIn("FILE-CONTROL", text)
            self.assertNotRegex(text, r"\bOPEN\s+(INPUT|OUTPUT|I-O|EXTEND)\b")
            self.assertIn('CALL "APPOINTMENT-SERVICE"', text)

    def test_business_modules_are_shared(self):
        expected = {
            "appointment-service.cob", "appointment-repository.cob", "appointment-dates.cob",
            "appointment-recurrence.cob", "appointment-backup.cob", "appointment-audit.cob",
            "appointment-export.cob",
        }
        self.assertEqual(expected, {p.name for p in (ROOT / "src" / "services").glob("*.cob")})

    def test_sql_is_confined_to_native_adapter(self):
        for path in list((ROOT / "src").rglob("*.cob")) + list((ROOT / "web").rglob("*.py")):
            self.assertNotRegex(path.read_text(encoding="utf-8").upper(),
                                r"\b(SELECT|INSERT|UPDATE|DELETE)\s+(FROM|INTO|SET|APPOINTMENTS|USERS)\b", path)
        native = (ROOT / "src" / "native" / "appointment_sqlite.c").read_text(encoding="utf-8")
        self.assertIn("sqlite3_prepare_v2", native)
        self.assertIn("crypto_pwhash_str_verify", native)
        self.assertIn("BEGIN IMMEDIATE", native)

    def test_sources_are_free_format_english_modules(self):
        for path in (ROOT / "src").rglob("*.cob"):
            text = path.read_text(encoding="utf-8")
            self.assertTrue(text.startswith(">>SOURCE FORMAT FREE"), path)
            self.assertRegex(text, r"PROGRAM-ID\. [A-Z0-9-]+\.")

    def test_web_has_transport_controls(self):
        text = (ROOT / "web" / "web_app.py").read_text(encoding="utf-8")
        for fragment in ("TemporaryDirectory", "timeout=TIMEOUT", "check=False", "completed.returncode"):
            self.assertIn(fragment, text)
        self.assertIn('SESSION_COOKIE_SAMESITE="Strict"', text)
        self.assertIn('request.endpoint in {"login", "static"}', text)

    def test_makefile_links_main_program_sources(self):
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("calmgr: src/programs/calmgr.cob", text)
        self.assertIn("calmgr-batch: src/programs/calmgr-batch.cob", text)
        self.assertNotIn("calmgr: $(BUILD)/src/programs/calmgr.o", text)
        self.assertIn("-lsqlite3", text)
        self.assertIn("-lsodium", text)

    def test_tui_uses_full_screen_crt_layout(self):
        tui = (ROOT / "src" / "programs" / "calmgr.cob").read_text(
            encoding="utf-8"
        ).upper()
        screens = (ROOT / "src" / "copybooks" / "appointment-screens.cpy").read_text(
            encoding="utf-8"
        ).upper()
        self.assertIn("SCREEN SECTION", tui)
        self.assertIn('COPY "APPOINTMENT-SCREENS.CPY"', tui)
        self.assertIn("ACCEPT MAIN-MENU-SCREEN", tui)
        self.assertIn("CONSOLE IS CRT", tui)
        self.assertIn('VALUE "CALMGR"', screens)
        self.assertIn("FOREGROUND-COLOR 7 BACKGROUND-COLOR 1", screens)
        self.assertIn('VALUE "C   CURRENT APPOINTMENTS"', screens)
        self.assertIn('VALUE "M   PERMANENT-DELETE MAINTENANCE"', screens)

    def test_tui_warning_sensitive_moves_are_bounded(self):
        tui = (ROOT / "src" / "programs" / "calmgr.cob").read_text(
            encoding="utf-8"
        ).upper()
        self.assertNotIn("INITIALIZE TUI-LIST-ROWS\n", tui)
        self.assertIn("INITIALIZE TUI-LIST-ROWS(TUI-LIST-INDEX)", tui)
        self.assertIn('MOVE "0101" TO ASR-FROM-DATE(5:4)', tui)
        self.assertIn('MOVE "1231" TO ASR-TO-DATE(5:4)', tui)
        self.assertIn("MOVE ASO-MESSAGE(1:72) TO TUI-RESULT-LINE-1", tui)

    def test_both_fanfold_report_formats_are_wired(self):
        export = (ROOT / "src" / "services" / "appointment-export.cob").read_text(
            encoding="utf-8"
        ).upper()
        batch = (ROOT / "src" / "programs" / "calmgr-batch.cob").read_text(
            encoding="utf-8"
        ).upper()
        self.assertIn("ORGANIZATION IS BINARY SEQUENTIAL", export)
        self.assertIn('"APPOINTMENT CALENDAR "', export)
        self.assertIn('"CALENDAR MANAGEMENT SYSTEM"', export)
        self.assertIn('"RUN DATE"', export)
        self.assertIn('"*** END OF REPORT CALR01 ***"', export)
        self.assertIn('"APPOINTMENT LIST "', export)
        self.assertIn('"*** END OF REPORT CALR02 ***"', export)
        self.assertIn('WHEN "PRINT-LIST-EXPORT"', batch)
        self.assertIn('MOVE "WK  RANGE          CLIENT            APPOINTMENT / SUBJECT" TO PRINT-TEXT', export)
        self.assertIn('MOVE AR-ISO-WEEK TO LS-WEEK-DISPLAY', export)
        self.assertIn('MOVE LS-DATE-DISPLAY TO PRINT-TEXT(5:10)', export)
        self.assertIn('MOVE LS-DATE-RANGE TO PRINT-TEXT(17:13)', export)
        self.assertIn('PERFORM PREPARE-APPOINTMENT-LIST-BLOCK', export)
        self.assertIn('IF LS-TEXT-LENGTH <= 31', export)
        self.assertIn('MOVE LS-WRAP-LINE(LS-WRAP-INDEX)(1:31) TO PRINT-TEXT(50:31)', export)
        self.assertIn('OR LS-WRITTEN-LINES NOT = LS-WRAP-COUNT', export)
        # Calendar report must load and emit matching appointments, not only week numbers.
        self.assertIn('PERFORM LOAD-CALENDAR-APPOINTMENTS', export)
        self.assertIn('IF AR-ISO-YEAR = LS-EXPORT-YEAR AND AR-ISO-WEEK = LS-WEEK', export)
        self.assertIn('PERFORM WRITE-CALENDAR-BLOCK', export)
