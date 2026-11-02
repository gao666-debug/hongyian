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
    # 留空表示不启用独立手表；兼容旧版主设备查询入口。
    nearlink_device_id: str = ''
    watch_device_id: str = ''
    # 手表产品物模型的服务 ID（华为云产品「22」是 11）。
    # 注意：WS63 环境节点（产品「33」）的服务 ID 也是 11，
    # 所以属性上报进来时还要靠属性特征二次确认，见 backend/watch.py。
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
                   service_id=os.getenv('IOTDA_SERVICE_ID', cls.service_id),
                   nearlink_device_id=os.getenv('NEARLINK_A_DEVICE_ID', ''),
                   watch_device_id=os.getenv('WATCH_DEVICE_ID', ''),
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

    @property
    def device_ids(self) -> list[str]:
        """App 可见的设备列表（DEVICE_ID 逗号分隔）。

        注意：手表**不在**这里。手表是「人」身上的穿戴设备，它的数据挂靠在
        主设备下（见 primary_device 与 /devices/{主板}/watch），
        App 不需要把一块表当成第三台独立设备来展示。
        """
        raw = (self.device_id or '').strip()
        ids = [d.strip() for d in raw.split(',') if d.strip()]
        if self.nearlink_device_id and self.nearlink_device_id not in ids:
            ids.append(self.nearlink_device_id)
        return list(dict.fromkeys(ids))

    @property
    def allowed_device_ids(self) -> list[str]:
        """允许入库的设备 = App 可见设备 + 手表。

        consume_cloud 的白名单与 app.py 的 check_device 都用这个，
        否则手表的消息在消费者那一层就被静默丢弃了。
        """
        ids = list(self.device_ids)
        watch = (self.watch_device_id or '').strip()
        if watch and watch not in ids:
            ids.append(watch)
        return ids

    @property
    def primary_device(self) -> str:
        """手表数据挂靠的主设备（取列表第一台）。"""
        ids = self.device_ids
        return ids[0] if ids else ''

    def has_device(self, device: str) -> bool:
        return device in self.allowed_device_ids

    def is_watch(self, device: str) -> bool:
        """该设备号是否就是配置的手表。"""
        return bool(self.watch_device_id) and device == self.watch_device_id
