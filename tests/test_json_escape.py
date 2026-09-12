import json
import unittest


class JsonEscapeTests(unittest.TestCase):
    def test_reference_json_escaping_round_trip(self):
        value = {'client': 'A "quoted" client', 'subject': 'backslash \\ and newline\n'}
        self.assertEqual(json.loads(json.dumps(value)), value)
