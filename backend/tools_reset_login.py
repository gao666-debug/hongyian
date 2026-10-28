"""Reset an existing login without deleting business records or printing secrets."""
import argparse
import getpass
import hashlib
import os
from pathlib import Path
import secrets
import sqlite3
from dotenv import dotenv_values

ROOT = Path(__file__).resolve().parent


def reset_password(db_path, username, password):
    if not username or len(password) < 6:
        raise ValueError('Username required; password must contain at least 6 characters')
    path = Path(db_path).resolve(strict=True)
    salt = secrets.token_hex(16)
    hashed = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), 210000).hex()
    conn = sqlite3.connect(path.as_uri() + '?mode=rw', uri=True, timeout=10)
    try:
        conn.execute('BEGIN IMMEDIATE')
        row = conn.execute('SELECT 1 FROM users WHERE name=?', (username,)).fetchone()
        if not row:
            raise ValueError('Account does not exist in the selected database; nothing changed')
        conn.execute('UPDATE users SET salt=?,password_hash=? WHERE name=?', (salt, hashed, username))
        conn.execute('DELETE FROM tickets WHERE session_hash IN (SELECT token_hash FROM sessions WHERE username=?)', (username,))
        conn.execute('DELETE FROM sessions WHERE username=?', (username,))
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--from-env', action='store_true', help='Use APP_PASSWORD from this project .env without printing it')
    args = parser.parse_args()
    values = dotenv_values(ROOT / '.env')
    username = os.getenv('APP_USERNAME', values.get('APP_USERNAME') or 'guardian')
    db = os.getenv('DATABASE_PATH', values.get('DATABASE_PATH') or str(ROOT / '.local/backend.sqlite3'))
    path = Path(db)
    if not path.is_absolute():
        path = ROOT / path
    if args.from_env:
        password = os.getenv('APP_PASSWORD', values.get('APP_PASSWORD') or '')
    else:
        password = getpass.getpass('New password: ')
        if password != getpass.getpass('Confirm password: '):
            raise SystemExit('Passwords differ; nothing changed')
    try:
        reset_password(path, username, password)
    except (ValueError, OSError, sqlite3.Error) as exc:
        raise SystemExit('Reset failed: ' + str(exc))
    print('Login password reset; old sessions/tickets revoked. Business records preserved.')


if __name__ == '__main__':
    main()
