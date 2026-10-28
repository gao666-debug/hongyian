export interface DoorReading {
  demo: boolean;
  fresh: boolean;
  healthFresh: boolean;
  enabled: number; // -1 unknown, 0 disabled, 1 enabled
  valid: number; // -1 not supplied by older backend, 0 invalid, 1 valid
  value: number; // -1 missing, 0 closed, 1 open
}
export function doorLabel(input: DoorReading): string {
  if (input.demo) { return '演示 · 房门关闭'; }
  if (!input.fresh) { return '等待新上报'; }
  if (!input.healthFresh || input.enabled < 0) { return '接入状态待确认'; }
  if (input.enabled === 0) { return '未开启'; }
  if (input.valid === 0) { return '读数无效'; }
  if (input.value !== 0 && input.value !== 1) { return '等待房门数据'; }
  const state = input.value === 1 ? '打开' : '关闭';
  return input.valid === 1 ? state : '最近上报：' + state;
}
