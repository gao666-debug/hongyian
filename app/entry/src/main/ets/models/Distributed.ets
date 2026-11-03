export interface EnvironmentProperties {
  light?: number | null; smoke?: number | null; body?: number | null;
  door?: string | number | null; temp?: number | null; humi?: number | null;
}
export interface EnvironmentShadow { properties?: EnvironmentProperties; updated_at?: string | null; online?: boolean | null; }
export interface EnvironmentHealth extends EnvironmentProperties { epoch?: number; up?: number; fw?: string; }
export interface WearReading {
  heart_rate?: number | null; spo2?: number | null; steps?: number | null;
  received_at?: string; fall_state?: number; fall_count?: number; posture?: string;
  fall_active?: boolean; hr_measuring?: boolean; motion_mg?: number; dynamic_mg?: number;
}
export interface WearAggregate { avg?: number | null; n?: number; }
export interface WearHour { hour: string; hr?: WearAggregate; spo2?: WearAggregate; steps_delta?: number; }
export interface WearFall { key?: string; fall_count: number; detected_at: string; heart_rate?: number; spo2?: number; }
export interface WearHours { items: WearHour[]; }
export interface WearFalls { items: WearFall[]; }
export interface DistributedNode { device_id: string; kind: string; name: string; room: string; }
export interface DistributedFamily { devices: DistributedNode[]; }

export function validNumber(value: number | null | undefined, min: number = 0, max: number = 10000000): boolean {
  return typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max;
}
export function sensorText(value: number | null | undefined, unit: string, decimals: number = 0): string {
  return validNumber(value) ? (value as number).toFixed(decimals) + unit : '未上报';
}
export function motionText(value: number | null | undefined): string {
  return value === 1 ? '检测到活动' : value === 0 ? '未检测到活动' : '未上报';
}
export function roomDoorText(value: string | number | null | undefined): string {
  return value === 'OPEN' || value === 1 ? '房门已开' : value === 'CLOSED' || value === 0 ? '房门已关' : '未上报';
}
export function watchVital(value: number | null | undefined, unit: string, max: number): string {
  return validNumber(value, 1, max) ? (value as number).toFixed(0) + unit : '待测量';
}
export function reportAge(stamp: string | null | undefined, now: number): string {
  const at = stamp ? Date.parse(stamp) : NaN;
  if (!Number.isFinite(at) || at > now + 60000) { return '采集时间待确认'; }
  const age = Math.max(0, Math.floor((now - at) / 1000));
  if (age > 120) { return '数据已过期 · ' + (age < 3600 ? Math.floor(age / 60) + ' 分钟前' : Math.floor(age / 3600) + ' 小时前'); }
  return age + ' 秒前收到上报';
}
export function readingFresh(stamp: string | null | undefined, now: number): boolean {
  const at = stamp ? Date.parse(stamp) : NaN;
  return Number.isFinite(at) && at <= now + 60000 && now - at <= 120000;
}
export function hourLabel(hour: string): string {
  const at = Date.parse(hour.length === 13 ? hour + ':00:00Z' : hour);
  return Number.isFinite(at) ? new Date(at).getHours().toString().padStart(2, '0') + '时' : hour.slice(-5);
}
