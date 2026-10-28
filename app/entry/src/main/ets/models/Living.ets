import { LivingStatus, MedicineSlot, CommandParameters } from './Models';
export function medicineLabel(s: LivingStatus | null, fresh: boolean): string {
  if (!s) { return '等待生活提醒数据'; }
  if (!fresh) { return '状态待更新'; }
  if (!s.medicine_enabled) { return '未开启'; }
  if (!s.time_synced) { return '时间待同步'; }
  const labels: string[] = ['未开启', '时间待同步', '待提醒', '待确认服药', '已确认服药'];
  return s.medicine_state >= 0 && s.medicine_state < labels.length ? labels[s.medicine_state] : '状态待确认';
}
export function nightLabel(s: LivingStatus | null, fresh: boolean): string {
  if (!s || !fresh) { return '状态待更新'; }
  if (s.light_owner === 'alarm') { return '报警灯优先'; }
  if (s.light_owner === 'medicine') { return '服药提醒中'; }
  if (!s.night_auto) { return '自动夜灯未开启'; }
  if (!s.light_valid) { return '光照检测不可用'; }
  return s.night_on ? '自动 · 已亮灯' : '自动 · 待机';
}
export function medicineTime(minute: number): string {
  return Math.floor(minute / 60).toString().padStart(2, '0') + ':' + (minute % 60).toString().padStart(2, '0');
}
export function validMedicineTime(hour: string, minute: string): boolean {
  return /^\d{1,2}$/.test(hour) && /^\d{1,2}$/.test(minute) && Number(hour) < 24 && Number(minute) < 60;
}

export function medicineSlots(s: LivingStatus | null): MedicineSlot[] {
  if (!s) { return []; }
  if (s.schema === 2 && s.slots && s.slots.length === 3) { return s.slots; }
  return [{slot:0,enabled:s.medicine_enabled,minute:s.medicine_minute,state:s.medicine_state,confirmed_epoch:s.confirmed_epoch}];
}
export function slotLabel(slot: MedicineSlot, synced: boolean, fresh: boolean): string {
  if (!fresh) { return '状态待更新'; }
  if (!slot.enabled) { return '未开启'; }
  if (!synced) { return '时间待同步'; }
  const labels: string[] = ['未开启','时间待同步','待提醒','未服药','已服药'];
  return labels[slot.state] || '状态待确认';
}
export function pendingMedicine(s: LivingStatus | null): MedicineSlot[] {
  return medicineSlots(s).filter((slot:MedicineSlot)=>slot.enabled && slot.state===3);
}
export function demoLivingUpdate(current: LivingStatus, name: string, paras: CommandParameters, epoch: number, light: number): LivingStatus {
  const day=Math.floor((epoch+28800)/86400);const minute=Math.floor(((epoch+28800)%86400)/60);
  const slots: MedicineSlot[]=medicineSlots(current).map((slot:MedicineSlot):MedicineSlot=>({slot:slot.slot,enabled:slot.enabled,minute:slot.minute,state:slot.state,confirmed_epoch:slot.confirmed_epoch}));
  let revision=current.medicine_revision || 1;
  if(name==='set_living'||name==='set_medicine_plan') {
    slots[0].enabled=paras.medicine_enabled===true;slots[0].minute=paras.medicine_minute || 0;
    if(name==='set_medicine_plan' && slots.length===3) {
      slots[1].enabled=paras.medicine_enabled_2===true;slots[1].minute=paras.medicine_minute_2 || 0;
      slots[2].enabled=paras.medicine_enabled_3===true;slots[2].minute=paras.medicine_minute_3 || 0;
    }
  } else {
    const index=paras.slot || 0;
    slots[index].confirmed_epoch=paras.taken===false?0:epoch;revision++;
  }
  slots.forEach((slot:MedicineSlot)=>{
    if(Math.floor((slot.confirmed_epoch+28800)/86400)!==day){slot.confirmed_epoch=0;}
    slot.state=!slot.enabled?0:slot.confirmed_epoch>0?4:minute>=slot.minute?3:2;
  });
  const pending=slots.some((slot:MedicineSlot)=>slot.state===3);
  const night=paras.night_auto!==undefined?paras.night_auto:current.night_auto;
  return {schema:current.schema,config_revision:current.config_revision+((name==='set_living'||name==='set_medicine_plan')?1:0),night_auto:night,night_on:night&&light<30&&!pending,light_valid:true,light_owner:pending?'medicine':night&&light<30?'night':'off',medicine_enabled:slots[0].enabled,medicine_minute:slots[0].minute,medicine_state:slots[0].state,dose_day:day,confirmed_epoch:slots[0].confirmed_epoch,time_synced:true,medicine_revision:revision,slots:slots};
}
