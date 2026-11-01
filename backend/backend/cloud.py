from datetime import datetime, timezone
from decimal import Decimal

from . import watch as watch_model


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
        self.settings = settings
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
        services = raw.get('shadow',[])
        # 主板和手表挂在同一个设备下时会有两条 service：先找主板，再退回手表。
        service = next((s for s in services if s.get('service_id')==self.settings.service_id),None)
        if service is None:
            service = watch_model.pick_service(services,self.settings.watch_service_id)
        if service is None:
            service = {}
        report = service.get('reported') or {}
        stamp = report.get('event_time')
        if stamp:
            try:
                stamp = datetime.strptime(stamp,'%Y%m%dT%H%M%SZ').replace(tzinfo=timezone.utc).isoformat().replace('+00:00','Z')
            except ValueError:
                pass
        properties = json_numbers(report.get('properties') or {})
        data = {'device_id':device,'service_id':service.get('service_id',self.settings.service_id),
                'online':True if status=='ONLINE' else False if status=='OFFLINE' else None,
                'updated_at':stamp,'properties':properties}
        if service.get('service_id') != self.settings.service_id:
            # 手表：额外给一份归一化结果，App 不用自己换算 mg -> g
            data['kind'] = 'watch'
            try:
                data['watch'] = watch_model.normalize(properties,
                                                      service_id=data['service_id'],
                                                      received_at=stamp)
            except watch_model.WatchReportError:
                data['watch'] = None
        return data

    def command(self, device, name, paras=None):
        from huaweicloudsdkiotda.v5 import CreateCommandRequest, DeviceCommandRequest
        try:
            raw = self.client.create_command(CreateCommandRequest(device_id=device,
                  body=DeviceCommandRequest(service_id=self.settings.service_id,command_name=name,paras=paras or {}))).to_dict()
        except Exception as exc:
            raise CloudError('IoTDA command outcome unknown') from exc
        response = raw.get('response')
        result = response.get('result_code') if isinstance(response,dict) else None
        if type(result) is not int:
            return 'unknown',None,raw.get('command_id')
        state = 'executed' if result==0 else 'unknown' if result==3 else 'failed'
        return state,result,raw.get('command_id')
