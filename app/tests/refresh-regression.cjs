// Exercise the actual Index refresh methods against a controlled network, without ArkUI.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH || 'F:/DevEco Studio/tools/hvigor/hvigor-ohos-plugin/node_modules/typescript');
const src=fs.readFileSync(__dirname+'/../entry/src/main/ets/pages/Index.ets','utf8');
const names=['refresh','refreshDetails','refreshRoomRecords','refreshDeviceHealth','refreshEvents','refreshStats','refreshReports'];
const methods=names.map(name=>{
 const at=src.indexOf('  private async '+name+'(');assert(at>=0,name);
 const tail=src.slice(at);const end=tail.indexOf('\n  private ',4);
 return end<0?tail:tail.slice(0,end);
}).join('\n');
const compiled=ts.transpileModule('class Harness {\n'+methods+'\n}; module.exports=Harness;', {compilerOptions:{module:1,target:7}}).outputText;
const m={exports:{}};vm.runInNewContext(compiled,{module:m,Promise,Date,Math,encodeURIComponent,http:{RequestMethod:{GET:0}},readingFresh:()=>true});
const deferred=()=>{let resolve;const promise=new Promise(r=>resolve=r);return{promise,resolve};};
const stamp=new Date().toISOString();
function fixture(){
 const h=new m.exports();Object.assign(h,{demo:false,loggedIn:true,visible:true,polling:false,detailsPolling:false,generation:1,days:[],chartDay:0,familyNodes:[{kind:'gateway',device_id:'main',updated_at:stamp,properties:{temp:11}}]});
 h.allowed=()=>true;h.expiredSession=()=>{};h.applyProperties=p=>{h.last=p;};h.pruneAlerts=()=>{};h.refreshFamily=async()=>{};h.refreshDeviceHealth=async()=>{};h.applyHealth=()=>{};h.receiveAlarms=items=>{h.events=items;};return h;
}
(async()=>{
 const h=fixture(),calls=[],slow=deferred();let temperature=22;
 h.api={path:s=>'/main/'+s,request:async(path)=>{calls.push(path);if(path.endsWith('shadow'))return{data:{properties:{temp:temperature},updated_at:stamp,online:true}};if(path.endsWith('stats'))return slow.promise;if(path.endsWith('alarms'))return{data:{items:[]}};if(path.endsWith('reports'))throw Error('unsupported');return{data:{}};}};
 await h.refresh(true);assert.equal(h.last.temp,22,'fresh catalog cache must not bypass authoritative shadow');assert.equal(h.polling,false,'slow statistics must release live lane');assert.equal(h.detailsPolling,true);
 temperature=23;await h.refresh(false);assert.equal(h.last.temp,23,'live readings continue during hung statistics');assert.equal(calls.filter(x=>x.endsWith('shadow')).length,2);
 slow.resolve({data:{days:[]}});await new Promise(r=>setTimeout(r,0));assert.equal(h.detailsPolling,false);
 const late=fixture(),pending=deferred();late.api={path:s=>s,request:()=>pending.promise};const task=late.refresh();late.generation++;late.visible=false;pending.resolve({data:{properties:{temp:99},updated_at:stamp}});await task;assert.equal(late.last,undefined,'old page/session responses cannot replace current readings');
 const busy=fixture(),gate=deferred();let count=0;busy.api={path:s=>s,request:()=>{count++;return gate.promise;}};const first=busy.refresh();await busy.refresh();assert.equal(count,1,'avoid overlapping shadow requests');gate.resolve({data:{properties:{temp:25},updated_at:stamp}});await first;
 const fail=fixture();fail.hours=[2];fail.activityTotal=2;fail.api={path:s=>s,request:async()=>{throw Error('offline');}};await fail.refreshStats(1);assert.equal(fail.activityTotal,2,'failed statistics must not fabricate zero activity');await fail.refresh();assert.equal(fail.polling,false);assert.match(fail.error,/同步失败/);
 const limited=fixture(),limitedCalls=[];limited.familyRole='community';limited.allowed=action=>action.startsWith('alarms.');limited.api={path:s=>s,request:async path=>{limitedCalls.push(path);return{data:{items:[]}};}};await limited.refresh(true);assert.deepEqual(limitedCalls,['alarms'],'incident-only roles must never request private sensor or health data');
 console.log('PASS refresh: authoritative shadow, slow ancillary isolation, parallel live refresh, stale-session exclusion, request overlap and failed statistics');
})().catch(e=>{console.error(e);process.exitCode=1;});
