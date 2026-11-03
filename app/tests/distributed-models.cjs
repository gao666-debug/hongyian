const fs = require('fs'), path = require('path'), vm = require('vm'), assert = require('assert');
const ts = require(process.env.TYPESCRIPT_PATH);
function load(relative, requireMock) {
  const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets', relative), 'utf8');
  const js = ts.transpileModule(source, {compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
  const module = {exports:{}};
  vm.runInNewContext('(function(require,module,exports){'+js+'})',{console,setTimeout,clearTimeout})(requireMock || require,module,module.exports);
  return module.exports;
}
const m = load('models/Distributed.ets');
const now = Date.parse('2026-11-02T08:00:00Z');
assert.equal(m.sensorText(0,' mV'),'0 mV');
for(const value of [null,undefined,NaN,Infinity,-1]) assert.equal(m.sensorText(value,' mV'),'未上报');
assert.equal(m.motionText(0),'未检测到活动');
assert.equal(m.motionText(1),'检测到活动');
assert.equal(m.motionText(null),'未上报');
for(const value of ['OPEN',1]) assert.equal(m.roomDoorText(value),'房门已开');
for(const value of ['CLOSED',0]) assert.equal(m.roomDoorText(value),'房门已关');
assert.equal(m.roomDoorText(null),'未上报');
assert.equal(m.watchVital(0,'%',100),'待测量');
assert.equal(m.watchVital(null,'%',100),'待测量');
assert.equal(m.watchVital(98,'%',100),'98%');
assert.equal(m.watchVital(130,'%',100),'待测量');
assert.equal(m.readingFresh('2026-11-02T07:59:30Z',now),true);
for(const stamp of ['',null,'invalid','2026-11-02T07:00:00Z','2026-11-03T08:00:00Z']) assert.equal(m.readingFresh(stamp,now),false);
assert.match(m.reportAge('2026-11-02T07:00:00Z',now),/过期/);
assert.equal(m.reportAge(null,now),'采集时间待确认');
assert.match(m.hourLabel('2026-11-02T08'),/^\d{2}时$/);

// Login must honor the server's authorized device list rather than a hardcoded identity.
let payload = {code:0,data:{access_token:'test-token',device_ids:['test-main','test-room']}};
const network = {http:{RequestMethod:{GET:0,POST:1},HttpDataType:{STRING:0},createHttp:()=>({
  request:async()=>({responseCode:200,result:JSON.stringify(payload)}),destroy:()=>{}})}};
const {Backend} = load('services/Backend.ets',name=>name==='@kit.NetworkKit'?network:{});
(async()=>{
  const api = new Backend();
  await api.login('https://example.test/','test-user','test-password');
  assert.equal(api.device,'test-main');
  assert.equal(Array.from(api.deviceIds).join(','),'test-main,test-room');
  assert.equal(api.path('watch'),'/api/v1/devices/test-main/watch');
  await api.logout();assert.equal(api.deviceIds.length,0);assert.equal(api.token,'');
  payload={code:0,data:{access_token:'test-token',device_ids:['other-main']}};
  await api.login('https://example.test','another-user','test-password');
  assert.equal(api.device,'other-main');
  console.log('PASS distributed: missing vs zero, sensor units, door types, vitals, stale/future data, UTC buckets, authorized device selection and logout');
})().catch(error=>{console.error(error);process.exitCode=1;});
