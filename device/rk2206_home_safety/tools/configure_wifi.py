#!/usr/bin/env python3
"""Generate ignored fallback Wi-Fi credentials locally."""
from pathlib import Path
import getpass
import json
import os
ssid = input("Backup hotspot SSID: ")
password = getpass.getpass("Backup hotspot password: ")
if not ssid or len(ssid.encode()) > 32 or not 8 <= len(password) <= 63:
    raise SystemExit("Invalid hotspot configuration")
p = Path(__file__).resolve().parents[1] / "include/wifi_private.h"
fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, "w") as f:
    f.write("#define WIFI_BACKUP_SSID " + json.dumps(ssid) + "\n")
    f.write("#define WIFI_BACKUP_PASSWORD " + json.dumps(password) + "\n")
os.chmod(p, 0o600)
print("Backup hotspot credentials generated; password hidden.")
