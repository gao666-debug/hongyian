from dataclasses import dataclass
from pathlib import Path
import os
from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parents[1]

@dataclass
class Settings:
    mode: str = 'mock'
    db: str = str(ROOT / '.local/backend.sqlite3')
    username: str = 'guardian'
    password: str = ''
    webhook_token: str = ''
    device_id: str = 'demo-device'
    # 手表（WS63 穿戴节点）在 IoTDA 里是另一台直连设备，可能和主板不同 ID。
    # 留空表示手表与主板共用 device_id。
    watch_device_id: str = ''
    # 产品物模型里的服务 ID。主板是 HomeSafety，手表是产品自定义的那个（本产品是 "11"）。
    service_id: str = 'HomeSafety'
    watch_service_id: str = '11'
    cloud_endpoint: str = 'https://YOUR_INSTANCE.iotda-app.cn-east-3.myhuaweicloud.com'
    region: str = 'cn-east-3'
    project_id: str = ''
    ak: str = ''
    sk: str = ''
    cors: str = ''
    token_seconds: int = 28800

    @classmethod
    def load(cls):
        load_dotenv(ROOT / '.env', override=False)
        return cls(mode=os.getenv('BACKEND_MODE', 'mock'),
                   db=os.getenv('DATABASE_PATH', cls.db),
                   username=os.getenv('APP_USERNAME', 'guardian'),
                   password=os.getenv('APP_PASSWORD', ''),
                   webhook_token=os.getenv('WEBHOOK_TOKEN', ''),
                   device_id=os.getenv('DEVICE_ID', cls.device_id),
                   watch_device_id=os.getenv('WATCH_DEVICE_ID', ''),
                   service_id=os.getenv('IOTDA_SERVICE_ID', cls.service_id),
                   watch_service_id=os.getenv('WATCH_SERVICE_ID', cls.watch_service_id),
                   cloud_endpoint=os.getenv('IOTDA_ENDPOINT', cls.cloud_endpoint),
                   region=os.getenv('IOTDA_REGION', 'cn-east-3'),
                   project_id=os.getenv('IOTDA_PROJECT_ID', ''),
                   ak=os.getenv('HUAWEICLOUD_SDK_AK', ''),
                   sk=os.getenv('HUAWEICLOUD_SDK_SK', ''),
                   cors=os.getenv('CORS_ORIGINS', ''))

    def validate(self):
        if self.mode not in ('mock', 'live'):
            raise ValueError('BACKEND_MODE must be mock or live')
        if len(self.password) < 6 or len(self.webhook_token) < 24:
            raise ValueError('Initialize local configuration: python tools_init.py')
        if self.mode == 'live' and (not self.ak or not self.sk or not self.project_id):
            raise ValueError('Live mode requires application AK/SK and IOTDA_PROJECT_ID')
        if self.mode == 'live' and not self.cloud_endpoint.startswith('https://'):
            raise ValueError('IOTDA_ENDPOINT must use HTTPS')
