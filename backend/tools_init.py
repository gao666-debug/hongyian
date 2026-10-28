"""Initialize private local settings. Never print passwords or tokens."""
from pathlib import Path
import secrets
import os

root=Path(__file__).resolve().parent
path=root/'.env'
if path.exists():
    raise SystemExit('.env already exists; preserved')
password=secrets.token_urlsafe(18)
webhook=secrets.token_urlsafe(32)
text=f'''BACKEND_MODE=mock
APP_USERNAME=guardian
APP_PASSWORD={password}
WEBHOOK_TOKEN={webhook}
DEVICE_ID=demo-device
IOTDA_ENDPOINT=https://YOUR_INSTANCE.iotda-app.cn-east-3.myhuaweicloud.com
IOTDA_REGION=cn-east-3
IOTDA_PROJECT_ID=
HUAWEICLOUD_SDK_AK=
HUAWEICLOUD_SDK_SK=
CORS_ORIGINS=
'''
fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
with os.fdopen(fd,'w') as f:f.write(text)
print('Private .env initialized. Read APP_USERNAME/APP_PASSWORD locally for App login.')
