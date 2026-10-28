import { AlarmEvent, alarmKey } from './Models';

export interface AlarmSnapshot {
  fresh: boolean;
  reportTime: number;
  status: number;
  bootId: number;
  healthUp: number;
}

function alarmState(item: AlarmEvent, records: AlarmEvent[], snapshot: AlarmSnapshot,
  acknowledged: string[]): string {
  if (item.phase === 'end') { return 'ended'; }
  if (acknowledged.indexOf(alarmKey(item)) >= 0) { return 'acknowledged'; }
  if (snapshot.bootId >= 0 && item.boot_id < snapshot.bootId) { return 'historical'; }
  const newer = records.some((record: AlarmEvent) => record.boot_id === item.boot_id &&
    record.code === item.code && record.event_id > item.event_id);
  if (newer) { return 'historical'; }
  const reportAfterEvent = item.epoch > 0 ? snapshot.reportTime > item.epoch * 1000 :
    snapshot.bootId === item.boot_id && snapshot.healthUp > item.up;
  if (snapshot.fresh && snapshot.status === 0 && reportAfterEvent) { return 'recovered'; }
  return 'active';
}

export function alarmStateLabel(item: AlarmEvent, records: AlarmEvent[], snapshot: AlarmSnapshot,
  acknowledged: string[]): string {
  const state = alarmState(item, records, snapshot, acknowledged);
  if (state === 'ended') { return '已结束'; }
  if (state === 'acknowledged') { return '已确认，待结束同步'; }
  if (state === 'historical') { return '历史记录待核实'; }
  if (state === 'recovered') { return '设备已恢复，结束记录待同步'; }
  return snapshot.fresh ? '报警中' : '报警待核实';
}

export function alarmIsActive(item: AlarmEvent, records: AlarmEvent[], snapshot: AlarmSnapshot,
  acknowledged: string[]): boolean {
  return alarmState(item, records, snapshot, acknowledged) === 'active';
}

export function resolutionLabel(resolution: string): string {
  if (resolution === 'false_alarm') { return '误报'; }
  if (resolution === 'follow_up') { return '需跟进'; }
  return '已处理';
}

export function noteText(draft: string, resolution: string): string {
  const text = draft.trim();
  if (text.length > 0) { return text; }
  return '处置结果：' + resolutionLabel(resolution) + '（未补充具体说明）';
}
