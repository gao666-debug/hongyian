"""手表（WS63 穿戴节点）属性上报的归一化与合法性校验。

手表固件每 10 秒向 IoTDA 上报一次属性，主题是
`$oc/devices/{device_id}/sys/properties/report`，报文形如：

    {"services":[{"service_id":"11","properties":{
        "heart_rate":60,"spo2":100,
        "accel_x":-188,"accel_y":-168,"accel_z":951,
        "steps":2,"fall_state":0,"fall_count":0}}]}

这里做三件事：

1. 从一条上报里挑出属于手表的那条 service（先按配置的服务 ID，找不到再按属性特征识别）；
2. 范围校验。越界、非数字直接拒绝，不让脏数据进库；
3. 换算成后端统一单位（加速度 mg -> g），补齐 App 能直接用的派生字段。

约定：`fall_state` 是"此刻的状态"（0 正常 / 1 冲击后观察中 / 2 疑似摔倒），
`fall_count` 是上电以来的累计次数（重启归零），两者不能互相替代。
"""
from datetime import datetime, timezone
from math import isqrt

# 手表物模型的全部属性。缺字段是允许的（老固件可能只上报心率），
# 但至少要有一个，否则无法判断这是不是手表的数据。
WATCH_KEYS = ('heart_rate', 'spo2', 'accel_x', 'accel_y', 'accel_z',
              'steps', 'fall_state', 'fall_count')

ACCEL_KEYS = ('accel_x', 'accel_y', 'accel_z')

# 单值合法区间。加速度按 +=16g 给余量（固件是 ±8g 量程，摔击瞬间会顶到边界）。
LIMITS = {'heart_rate': (0, 250), 'spo2': (0, 100),
          'accel_x': (-16000, 16000), 'accel_y': (-16000, 16000), 'accel_z': (-16000, 16000),
          'steps': (0, 5000000), 'fall_state': (0, 2), 'fall_count': (0, 1000000)}

# 手持时 z 轴朝上约 +1000mg；平放桌面时按安装方向落在另一个轴上。
# 这里只做"大致姿态"判断，不用于跌倒结论。
POSTURE_TILT_MG = 700


class WatchReportError(ValueError):
    """上报内容无法作为手表数据处理。"""


def _number(name, value):
    """IoTDA 的属性值可能是 int / float / Decimal / 字符串，统一成 int。"""
    if isinstance(value, bool) or isinstance(value, (dict, list, tuple)):
        raise WatchReportError('invalid %s' % name)
    try:
        number = int(float(value))
    except (TypeError, ValueError, OverflowError):
        raise WatchReportError('invalid %s' % name) from None
    low, high = LIMITS[name]
    if not low <= number <= high:
        raise WatchReportError('%s out of range' % name)
    return number


def pick_service(services, configured=''):
    """挑出手表那条 service。给了服务 ID 就只认它，否则按属性特征识别。"""
    services = [s for s in services or [] if isinstance(s, dict)]
    if configured:
        for service in services:
            if service.get('service_id') == configured:
                return service
    for service in services:
        properties = service.get('properties')
        if isinstance(properties, dict) and any(key in properties for key in WATCH_KEYS):
            return service
    return None


def _magnitude(x, y, z):
    return isqrt(x * x + y * y + z * z)


def posture(x, y, z):
    """按最大值落在哪个轴给一个粗略姿态，仅用于展示。"""
    biggest = max((abs(x), 'x'), (abs(y), 'y'), (abs(z), 'z'))
    if biggest[0] < POSTURE_TILT_MG:
        return 'moving'
    return 'z_up' if biggest[1] == 'z' else ('x_up' if biggest[1] == 'x' else 'y_up')


def normalize(properties, service_id='', received_at=None):
    """把一条上报的属性字典归一化；校验失败抛 WatchReportError。"""
    if not isinstance(properties, dict):
        raise WatchReportError('properties must be an object')
    if not any(key in properties for key in WATCH_KEYS):
        raise WatchReportError('no watch property in report')

    values, extra = {}, []
    for key in WATCH_KEYS:
        if key in properties:
            values[key] = _number(key, properties[key])
    for key in properties:
        if key not in LIMITS:
            extra.append(key)

    accel = {axis: values.get('accel_' + axis, 0) for axis in ('x', 'y', 'z')}
    motion = _magnitude(accel['x'], accel['y'], accel['z'])
    report = {
        'kind': 'watch',
        'service_id': service_id,
        'received_at': received_at or datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z'),
        'heart_rate': values.get('heart_rate'),
        'spo2': values.get('spo2'),
        'accel_mg': accel,
        'accel_g': {axis: round(accel[axis] / 1000, 3) for axis in accel},
        'motion_mg': motion,
        # 扣掉重力后的动态分量，走路/抖动看这个比看单轴直观
        'dynamic_mg': abs(motion - 1000),
        'posture': posture(accel['x'], accel['y'], accel['z']),
        'steps': values.get('steps', 0),
        'fall_state': values.get('fall_state', 0),
        'fall_count': values.get('fall_count', 0),
    }
    # 心率/血氧为 0 表示"这次没测到"（手指没贴上），不是真实读数，
    # 单独标出来，免得 App 把 0 画进曲线。
    report['hr_measuring'] = (report['heart_rate'] or 0) > 0
    report['fall_active'] = report['fall_state'] == 2
    report['fall_watch'] = report['fall_state'] == 1
    if extra:
        report['ignored_properties'] = sorted(extra)
    return report
