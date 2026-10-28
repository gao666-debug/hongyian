"""Start/reuse this computer's HTTPS backend and AMQP consumer for USB demos."""
import os
from pathlib import Path
import subprocess
import sys
import time

import requests
from proton import SSL
from backend.settings import Settings, ROOT


def existing_process(script):
    for directory in Path('/proc').iterdir():
        if not directory.name.isdecimal():
            continue
        try:
            if directory.joinpath('cwd').resolve() != ROOT:
                continue
            args = directory.joinpath('cmdline').read_bytes().split(b'\0')
            if any(Path(value.decode()).name == script for value in args if value):
                return int(directory.name)
        except (OSError, ValueError):
            continue
    return None


def launch(script, arguments):
    log_path = ROOT / '.local' / (Path(script).stem + '-demo.log')
    with log_path.open('ab') as log:
        os.chmod(log_path, 0o600)
        child = subprocess.Popen([sys.executable, '-u', str(ROOT / script), *arguments],
                                 cwd=ROOT, stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=log, start_new_session=True)
    (ROOT / '.local' / (Path(script).stem + '-demo.pid')).write_text(str(child.pid))
    return child


def healthy():
    try:
        response = requests.get('https://127.0.0.1:8443/healthz',
                                verify=str(ROOT / '.local/tls/development-ca.crt'), timeout=2)
        response.raise_for_status()
        return response.json().get('mode') == 'live'
    except (requests.RequestException, ValueError):
        return False


def main():
    settings = Settings.load()
    settings.validate()
    if settings.mode != 'live':
        raise SystemExit('Real-device demo requires BACKEND_MODE=live')
    if not SSL.present():
        raise SystemExit('AMQP dependency requires TLS support; see README')
    if any(not os.getenv(key) for key in ['AMQP_HOST', 'AMQP_ACCESS_KEY', 'AMQP_ACCESS_CODE', 'AMQP_QUEUE']):
        raise SystemExit('Complete AMQP configuration in the local backend .env first')
    for name in ['server.crt', 'server.key', 'development-ca.crt']:
        if not (ROOT / '.local/tls' / name).exists():
            raise SystemExit('Missing local TLS material; run tools_tls.py first')
    if not healthy():
        server = launch('run.py', ['--host', '0.0.0.0', '--port', '8443',
                                  '--cert', '.local/tls/server.crt', '--key', '.local/tls/server.key'])
        for attempt in range(30):
            if healthy():
                break
            if server.poll() is not None:
                raise SystemExit('Backend exited; inspect .local/run-demo.log')
            time.sleep(0.5)
        else:
            raise SystemExit('Backend startup timed out; inspect .local/run-demo.log')
    pid = existing_process('consume_cloud.py')
    if not pid:
        consumer = launch('consume_cloud.py', [])
        time.sleep(1)
        if consumer.poll() is not None:
            raise SystemExit('Consumer exited; inspect .local/consume_cloud-demo.log')
        pid = consumer.pid
    print('Live HTTPS backend ready on WSL port 8443; AMQP consumer PID', pid)


if __name__ == '__main__':
    main()
