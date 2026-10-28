export interface Envelope { code: number; message: string; data?: object; mode?: string; simulated?: boolean; }
export interface LoginData { access_token: string; }
export interface Properties {
  temp?: number; humi?: number; gas?: number; light?: number; body?: number; inactive?: number;
  alarm?: number; status?: number; help?: number; fall?: number; gas_alarm?: number; smoke?: number; door?: number;
}
export interface Shadow { online?: boolean; updated_at?: string; properties?: Properties; }
export interface MedicineSlot { slot: number; enabled: boolean; minute: number; state: number; confirmed_epoch: number; }
export interface MedicineDraft { slot: number; enabled: boolean; hour: string; minute: string; }
export interface MedicineRecord { dose_day: number; slot: number; state: number; minute: number; confirmed_epoch: number; epoch: number; medicine_revision: number; }
export interface MedicineHistory { items: MedicineRecord[]; }
export interface LivingStatus {
  schema: number; config_revision: number; night_auto: boolean; night_on: boolean; light_valid: boolean;
  light_owner: string; medicine_enabled: boolean; medicine_minute: number; medicine_state: number;
  dose_day: number; confirmed_epoch: number; time_synced: boolean;
  medicine_revision?: number; slots?: MedicineSlot[];
}
export interface CommandParameters { night_auto?: boolean; medicine_enabled?: boolean; medicine_minute?: number; expected_revision?: number; dose_day?: number;
  medicine_enabled_2?: boolean; medicine_minute_2?: number; medicine_enabled_3?: boolean; medicine_minute_3?: number;
  slot?: number; taken?: boolean; expected_medicine_revision?: number; }
export interface Health {
  living?: LivingStatus;
  epoch?: number; up?: number; boot_id?: number; fw?: string; env_valid?: number; imu_valid?: number; mute?: number; tls?: boolean;
  door_enabled?: number; door_valid?: number; wireless_btn_enabled?: number; data_state?: string; data_stale?: boolean;
  env_available?: boolean; fall_detection_available?: boolean; time_synced?: boolean;
}
export interface AlarmEvent { boot_id: number; event_id: number; code: number; phase: string; epoch: number; up: number; duration: number; value: number; }
export interface AlarmList { items: AlarmEvent[]; }
export interface Note { note: string; resolution: string; }
export interface NoteItem { note: string; resolution: string; username?: string; created?: string; }
export interface NoteList { items: NoteItem[]; }
export interface Command { command_id: string; state: string; result_code?: number; }
export interface Ticket { ticket: string; }
export interface Push { type: string; data?: object; }
export interface DayStats { date: string; activity_total: number; alarm_total: number; activity_by_hour: number[]; }
export interface Statistics { days: DayStats[]; activity_total: number; alarm_total: number; }
export interface LocalStatus extends Properties {
  env_valid?: number; imu_valid?: number; mute?: number; fw?: string; mqtt?: number; data_state?: string;
  env_available?: boolean; fall_detection_available?: boolean; door_enabled?: boolean;
}
export function alarmTitle(code: number): string {
  const titles: string[] = ['设备正常', '紧急求助', '疑似跌倒', '燃气异常', '活动异常待核实', '温湿度异常'];
  return code >= 0 && code < titles.length ? titles[code] : '设备事件';
}
export function alarmKey(item: AlarmEvent): string { return `${item.boot_id}:${item.event_id}`; }
export function alarmTime(item: AlarmEvent): string {
  return item.epoch > 0 ? new Date(item.epoch * 1000).toLocaleString('zh-CN', { hour12: false }) : `开机 ${item.up} 秒 · 时间未同步`;
}

export interface CommandRequest { device_id: string; service_id: string; command_name: string; paras: CommandParameters; client_request_id: string; }
