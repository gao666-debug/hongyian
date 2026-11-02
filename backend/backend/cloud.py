from datetime import datetime, timezone
from decimal import Decimal


def json_numbers(value):
    """Convert SDK Decimal property values before SQLite/JSON serialization."""
    if isinstance(value, Decimal):
        return float(value)
    if isinstance(value, dict):
        return {key: json_numbers(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_numbers(item) for item in value]
    return value

class CloudError(Exception):
    pass

class HuaweiCloud:
    """Application-side SDK only; never connects with the board's MQTT identity."""
    def __init__(self, settings):
        from huaweicloudsdkcore.auth.credentials import BasicCredentials, DerivedCredentials
        from huaweicloudsdkcore.region.region import Region
        from huaweicloudsdkcore.http.http_config import HttpConfig
        from huaweicloudsdkiotda.v5 import IoTDAClient
        credentials = BasicCredentials(settings.ak,settings.sk,settings.project_id)
        credentials.with_derived_predicate(DerivedCredentials.get_default_derived_predicate())
        config = HttpConfig()
        config.timeout = (5,15)
        self.client = (IoTDAClient.new_builder().with_credentials(credentials)
                       .with_region(Region(settings.region,settings.cloud_endpoint))
                       .with_http_config(config).build())

    def shadow(self, device):
        from huaweicloudsdkiotda.v5 import ShowDeviceShadowRequest, ShowDeviceRequest
        try:
            raw = self.client.show_device_shadow(ShowDeviceShadowRequest(device_id=device)).to_dict()
            try:
                status = self.client.show_device(ShowDeviceRequest(device_id=device)).status
            except Exception:
                status = None
        except Exception as exc:
            raise CloudError('IoTDA shadow request failed') from exc
        # 多设备（2026-11-02）：不同产品的物模型服务 ID 不同 ——
        # 主监护板是 HomeSafety，WS63 星闪环境节点是 11（华为云产品 33）。
        # 旧逻辑只挑 HomeSafety，导致新设备影子明明有属性却返回空对象，
        # 前端读 shadow.properties 会整屏空白。
        # 现在：优先 HomeSafety（主监护板行为逐字节不变），没有时才合并其余服务的属性。
        services = raw.get('shadow') or []

        def reported_props(service):
            return (service.get('reported') or {}).get('properties') or {}

        preferred = next((s for s in services
                          if s.get('service_id') == 'HomeSafety' and reported_props(s)), None)
        if preferred is not None:
            chosen = preferred
            properties = dict(reported_props(preferred))
            # 属性取自哪个服务，时间就取哪个服务的，避免「旧属性配新时间」自相矛盾
            stamp = (preferred.get('reported') or {}).get('event_time')
        else:
            chosen = next((s for s in services if reported_props(s)), {})
            properties = {}
            for service in services:
                properties.update(reported_props(service))
            # 合并模式下时间取所有有属性服务里最新的。
            # 时间格式固定为 YYYYMMDDTHHMMSSZ，字典序即时间序。
            stamps = [(s.get('reported') or {}).get('event_time') for s in services
                      if reported_props(s) and (s.get('reported') or {}).get('event_time')]
            stamp = max(stamps) if stamps else (chosen.get('reported') or {}).get('event_time')
        if stamp:
            try:
                stamp = datetime.strptime(stamp,'%Y%m%dT%H%M%SZ').replace(tzinfo=timezone.utc).isoformat().replace('+00:00','Z')
            except ValueError:
                pass
        return {'device_id':device,'service_id':chosen.get('service_id') or 'HomeSafety',
                'online':True if status=='ONLINE' else False if status=='OFFLINE' else None,
                'updated_at':stamp,'properties':json_numbers(properties)}

    def command(self, device, name, paras=None):
        from huaweicloudsdkiotda.v5 import CreateCommandRequest, DeviceCommandRequest
        try:
            raw = self.client.create_command(CreateCommandRequest(device_id=device,
                  body=DeviceCommandRequest(service_id='HomeSafety',command_name=name,paras=paras or {}))).to_dict()
        except Exception as exc:
            raise CloudError('IoTDA command outcome unknown') from exc
        response = raw.get('response')
        result = response.get('result_code') if isinstance(response,dict) else None
        if type(result) is not int:
            return 'unknown',None,raw.get('command_id')
        state = 'executed' if result==0 else 'unknown' if result==3 else 'failed'
        return state,result,raw.get('command_id')
