"""Start one detached HTTPS instance, with private logs and no access URL logging."""
from pathlib import Path
import os
import subprocess
import sys

root=Path(__file__).resolve().parent
local=root/'.local';local.mkdir(exist_ok=True)
pidfile=local/'server.pid'
if pidfile.exists():
    try:
        pid=int(pidfile.read_text())
        os.kill(pid,0)
        raise SystemExit('Server already running; preserve it or stop it first')
    except ProcessLookupError:pass
cert=local/'tls/server.crt';key=local/'tls/server.key'
if not cert.exists() or not key.exists():raise SystemExit('Run tools_tls.py first')
with (local/'server.log').open('ab') as log:
    proc=subprocess.Popen([sys.executable,str(root/'run.py'),'--host','0.0.0.0','--port','8443',
                           '--cert',str(cert),'--key',str(key)],cwd=root,stdout=log,stderr=log,
                          stdin=subprocess.DEVNULL,start_new_session=True)
pidfile.write_text(str(proc.pid))
print('Local HTTPS backend started; PID',proc.pid)
