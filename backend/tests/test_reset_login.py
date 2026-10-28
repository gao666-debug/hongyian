import tempfile
from pathlib import Path
import unittest
from backend.store import Store
from tools_reset_login import reset_password


class ResetLoginTests(unittest.TestCase):
    def test_reset_preserves_data_and_other_users(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.sqlite3'
            store = Store(str(path))
            store.bootstrap_user('guardian', 'old-password')
            store.bootstrap_user('other', 'other-password')
            token = store.login('guardian', 'old-password', 600)
            other_token = store.login('other', 'other-password', 600)
            from backend.store import digest
            store.ticket(digest(token), 'device')
            store.ticket(digest(other_token), 'device')
            store.put('live', 'device', 'alarms', {'event': 7})
            reset_password(path, 'guardian', 'new-password')
            self.assertIsNone(store.login('guardian', 'old-password', 600))
            self.assertTrue(store.login('guardian', 'new-password', 600))
            self.assertIsNone(store.session(digest(token)))
            self.assertTrue(store.session(digest(other_token)))
            self.assertEqual(store.doc('live', 'device', 'alarms'), {'event': 7})
            with store.connection() as db:
                self.assertEqual(db.execute('SELECT COUNT(*) FROM tickets').fetchone()[0], 1)
            with self.assertRaises(ValueError):
                reset_password(path, 'missing', 'new-password')
            self.assertTrue(store.login('guardian', 'new-password', 600))

    def test_missing_database_is_not_created(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'missing.sqlite3'
            with self.assertRaises(FileNotFoundError):
                reset_password(path, 'guardian', 'new-password')
            self.assertFalse(path.exists())
