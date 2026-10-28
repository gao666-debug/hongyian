const fs = require('fs');
const path = require('path');
const assert = require('assert');
const vm = require('vm');
const ts = require(process.env.TYPESCRIPT_PATH || 'F:/DevEco Studio/tools/hvigor/hvigor-ohos-plugin/node_modules/typescript');
const root = path.resolve(__dirname, '../entry/src/main/ets/models');
const cache = {};
function load(name) {
  name = path.basename(name).replace(/\.ets$/, '');
  if (cache[name]) return cache[name];
  const source = fs.readFileSync(path.join(root,name+'.ets'),'utf8');
  const code = ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
  const module = {exports:{}};
  vm.runInNewContext('(function(require,module,exports){'+code+'\n})',{})(load,module,module.exports);
  cache[name] = module.exports; return module.exports;
}
const {AlarmInbox,mergeAlarms,validAlarm} = load('AlarmInbox');
const event = (boot,id,phase='start',code=1) => ({boot_id:boot,event_id:id,phase,code,epoch:1000+id,up:id,duration:0,value:0});
const box = new AlarmInbox();
assert.equal(box.accept([event(1,1)]).length,1);
assert.equal(box.accept([event(1,1)]).length,0); // WS/poll/reconnect duplicate
assert.equal(box.accept([event(1,1,'end')]).length,0);
assert.equal(box.accept([event(1,1)]).length,0); // late start
assert.equal(box.accept([event(2,1)]).length,1); // reboot identity
assert.equal(box.accept([event(2,2),event(2,3)]).length,2); // concurrent alarms
assert.equal(new AlarmInbox().accept([event(3,1),event(3,1,'end')]).length,0); // history
assert.equal(new AlarmInbox().accept([event(3,1,'end'),event(3,1)]).length,0);
assert.equal(new AlarmInbox().accept([event(3,2),event(3,2)]).length,1);
assert(!validAlarm(null)); assert(!validAlarm(event(1,1,'bad'))); assert(!validAlarm(event(1,1,'start',99)));
let merged = mergeAlarms([event(1,1,'end')],[event(1,1)]);
assert.equal(merged.length,1);assert.equal(merged[0].phase,'end');
merged = mergeAlarms([event(1,1)],[event(1,1,'end'),event(1,2)]);
assert.equal(merged.length,2);assert.equal(merged.find(e=>e.event_id===1).phase,'end');
assert.equal(mergeAlarms([],Array.from({length:120},(_,i)=>event(1,i))).length,100);
const {assessCare} = load('CareAssessment');
const normal = {demo:false,online:true,fresh:true,healthFresh:true,environment:true,motion:true,alarmCode:0,activeAlarms:0};
assert.equal(assessCare(normal).level,'normal');
for(const field of ['online','fresh','healthFresh']) assert.equal(assessCare({...normal,[field]:false}).level,'waiting');
assert.equal(assessCare({...normal,environment:false}).level,'attention');
assert.equal(assessCare({...normal,motion:false}).level,'normal');
assert(!assessCare(normal).reason.includes('跌倒检测可用'));
assert.equal(assessCare({...normal,alarmCode:1}).level,'alert');
assert.equal(assessCare({...normal,activeAlarms:1,fresh:false}).level,'alert');
assert.equal(assessCare({...normal,demo:true}).level,'demo');
assert(!('score' in assessCare(normal)));
console.log('PASS alarm start/end deduplication, reconnect/history ordering, independent alarms, record bounds');
console.log('PASS assessment: unknown/stale/offline never normal; missing sensors, alarms and demo distinguished');

const {medicineLabel,nightLabel,medicineTime,validMedicineTime}=load('Living');
const life={schema:1,config_revision:1,night_auto:true,night_on:true,light_valid:true,light_owner:'night',medicine_enabled:false,medicine_minute:480,medicine_state:0,dose_day:21000,confirmed_epoch:0,time_synced:true};
assert.equal(medicineLabel(null,true),'等待生活提醒数据');
assert.equal(medicineLabel(life,false),'状态待更新');
assert.equal(medicineLabel(life,true),'未开启');
assert.equal(medicineLabel({...life,medicine_enabled:true,time_synced:false},true),'时间待同步');
assert.equal(medicineLabel({...life,medicine_enabled:true,medicine_state:3},true),'待确认服药');
assert.equal(medicineLabel({...life,medicine_enabled:true,medicine_state:4},true),'已确认服药');
assert.equal(nightLabel(life,true),'自动 · 已亮灯');
assert.equal(nightLabel({...life,light_owner:'alarm'},true),'报警灯优先');
assert.equal(nightLabel({...life,night_on:false,light_valid:false},true),'光照检测不可用');
assert.equal(medicineTime(0),'00:00');assert.equal(medicineTime(1439),'23:59');
assert(validMedicineTime('08','00'));assert(!validMedicineTime('24','00'));assert(!validMedicineTime('23','60'));assert(!validMedicineTime('','0'));assert(!validMedicineTime('1.5','0'));assert(!validMedicineTime('-1','0'));
console.log('PASS living display: missing/stale/no UTC, disabled, pending, confirmation, alarm priority, daily time validation');

const {alarmStateLabel,alarmIsActive,noteText}=load('AlarmState');
const alarm=event(50,1,'start',1);
const snap={fresh:true,reportTime:(alarm.epoch+5)*1000,status:0,bootId:50,healthUp:10};
assert(!alarmIsActive(alarm,[alarm],snap,[]));
assert.equal(alarmStateLabel(alarm,[alarm],snap,[]),'设备已恢复，结束记录待同步');
assert(alarmIsActive(alarm,[alarm],{...snap,status:1},[]));
assert(alarmIsActive(alarm,[alarm],{...snap,fresh:false},[]));
assert(alarmIsActive(alarm,[alarm],{...snap,reportTime:alarm.epoch*1000-1},[]));
assert(!alarmIsActive(alarm,[alarm],{...snap,status:1},['50:1']));
const next=event(50,2,'start',1);
assert(alarmIsActive(next,[alarm,next],{...snap,status:1},['50:1']));
assert(!alarmIsActive(alarm,[alarm,next],{...snap,status:1},[]));
assert(!alarmIsActive(alarm,[alarm],{...snap,bootId:51,status:1},[]));
assert(alarmIsActive(event(51,1),[event(51,1)],{...snap,reportTime:0},[]));
assert(!alarmIsActive({...alarm,phase:'end'},[alarm],{...snap,status:1},[]));
assert(alarmIsActive({...alarm,epoch:0,up:20},[alarm],snap,[]));
assert(alarmIsActive({...alarm,epoch:0,up:10},[alarm],snap,[]));
assert(!alarmIsActive({...alarm,epoch:0,up:9},[alarm],snap,[]));
assert(alarmIsActive(alarm,[alarm],{...snap,reportTime:alarm.epoch*1000},[]));
assert.equal(noteText('   ','handled'),'处置结果：已处理（未补充具体说明）');
assert.equal(noteText('','false_alarm'),'处置结果：误报（未补充具体说明）');
assert.equal(noteText('','follow_up'),'处置结果：需跟进（未补充具体说明）');
assert.equal(noteText('  已联系现场人员  ','handled'),'已联系现场人员');
console.log('PASS alarm state: recovery, stale reports, ack, new/reboot alarms; note defaults and free text');

const {medicineSlots,slotLabel,pendingMedicine,demoLivingUpdate}=load('Living');
const plan={...life,schema:2,medicine_revision:1,slots:[{slot:0,enabled:true,minute:0,state:3,confirmed_epoch:0},{slot:1,enabled:true,minute:0,state:3,confirmed_epoch:0},{slot:2,enabled:true,minute:0,state:3,confirmed_epoch:0}]};
assert.equal(medicineSlots(life).length,1);
assert.equal(medicineSlots(plan).length,3);
assert.equal(slotLabel(plan.slots[0],true,true),'未服药');
assert.equal(slotLabel(plan.slots[0],true,false),'状态待更新');
const second=demoLivingUpdate(plan,'set_medicine_state',{slot:1,taken:true},21000*86400,100);
assert.equal(JSON.stringify(second.slots.map(s=>s.state)),JSON.stringify([3,4,3]));
assert.equal(pendingMedicine(second).length,2);
const undone=demoLivingUpdate(second,'set_medicine_state',{slot:1,taken:false},21000*86400,100);
assert.equal(JSON.stringify(undone.slots.map(s=>s.state)),JSON.stringify([3,3,3]));
assert.equal(undone.medicine_revision,3);
const tomorrow=demoLivingUpdate(second,'set_medicine_plan',{night_auto:true,medicine_enabled:true,medicine_minute:0,medicine_enabled_2:true,medicine_minute_2:0,medicine_enabled_3:false,medicine_minute_3:1200},21001*86400,100);
assert.equal(JSON.stringify(tomorrow.slots.map(s=>s.state)),JSON.stringify([3,3,0]));
assert.equal(second.slots[1].state,4); // Helpers must not mutate received device state.
console.log('PASS three-dose slots, legacy display, independence, undo, midnight and immutable updates');
