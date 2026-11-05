// The server remains authoritative. Unknown roles fail closed in the UI.
export function collaborationError(message: string): string {
  if (message.includes('Alarm already handled')) { return '此报警已有处理记录，请刷新查看处置结果'; }
  if (message.includes('Alarm is being handled by another member')) { return '其他成员正在处理此报警，请等待或联系该成员'; }
  if (message.includes('Only claimant or administrator can release')) { return '只有认领人或家庭管理员可以释放认领'; }
  return message;
}
export function roleLabel(role: string): string {
  if (role === 'owner') { return '家庭管理员'; }
  if (role === 'caregiver') { return '家属／监护人'; }
  if (role === 'community') { return '社区网格员'; }
  if (role === 'property') { return '物业值班人员'; }
  if (role === 'viewer') { return '只读成员'; }
  return '权限待同步';
}
export function roleNote(role: string): string {
  if (role === 'owner') { return '管理账号与设备，可处理报警和调整设置'; }
  if (role === 'caregiver') { return '查看家庭数据、处理报警和操作设备'; }
  if (role === 'community' || role === 'property') { return '接收并处理本家庭报警，不开放摄像头、健康数据和设备控制'; }
  if (role === 'viewer') { return '查看家庭数据，不能处理报警或控制设备'; }
  return '正在读取账号权限';
}
export function roleAllows(role: string, action: string): boolean {
  if (role === 'owner') { return ['view','control','alarms.read','alarms.handle','members.manage'].includes(action); }
  if (role === 'caregiver') { return ['view','control','alarms.read','alarms.handle'].includes(action); }
  if (role === 'viewer') { return ['view','alarms.read'].includes(action); }
  if (role === 'community' || role === 'property') { return ['alarms.read','alarms.handle'].includes(action); }
  return false;
}
