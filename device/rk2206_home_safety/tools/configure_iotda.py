#!/usr/bin/env python3
"""Generate an ignored, local build credential header; never print credentials."""
import argparse
import datetime
import hashlib
import hmac
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("credential_file", type=Path)
parser.add_argument("--host", required=True)
args = parser.parse_args()
creds = json.loads(args.credential_file.read_text(encoding="utf-8-sig"))
device, secret = creds["device_id"], creds["secret"]
if not isinstance(device, str) or not isinstance(secret, str) or not device or not secret:
    raise SystemExit("Invalid credential file")
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d%H")
# IoTDA signature type 0 does not check timestamp freshness. Still uses HMAC.
password = hmac.new(stamp.encode(), secret.encode(), hashlib.sha256).hexdigest()
values = {"IOTDA_HOST": args.host, "IOTDA_DEVICE_ID": device,
          "IOTDA_CLIENT_ID": device + "_0_0_" + stamp, "IOTDA_PASSWORD": password}
out = Path(__file__).resolve().parents[1] / "include/iotda_private.h"
fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, "w") as f:
    f.write("/* PRIVATE: generated build credentials; do not distribute. */\n")
    for key, value in values.items():
        f.write("#define " + key + " " + json.dumps(value) + "\n")
os.chmod(out, 0o600)
print("Local IoTDA build credentials generated; values hidden.")
