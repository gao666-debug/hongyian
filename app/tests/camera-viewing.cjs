const fs=require('fs'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH);
const requests=[],timers=new Map();let timerId=0,sequence=0,resolvePending=null,hold=false;
class Backend {
  constructor(){this.base='';this.token='';this.device='';this.caPath='';}
  path(suffix){return '/api/v1/devices/'+this.device+'/'+suffix;}
  async request(path,method){
    requests.push({path,method,token:this.token,ca:this.caPath});
    if(method===1){
      if(hold){return new Promise(resolve=>{resolvePending=resolve;});}
      return {data:{view_id:'view-'+(++sequence),expires_in:20}};
    }
    return {data:{stopped:true}};
  }
}
const source=fs.readFileSync(__dirname+'/../entry/src/main/ets/services/CameraViewing.ets','utf8');
const js=ts.transpileModule(source,{compilerOptions:{module:1,target:7}}).outputText,mod={exports:{}};
vm.runInNewContext('(function(require,module,exports){'+js+'})',{
  Error,encodeURIComponent,setInterval:fn=>{timers.set(++timerId,fn);return timerId;},clearInterval:id=>timers.delete(id)
})(name=>name==='./Backend'?{Backend}:{http:{RequestMethod:{POST:1,PUT:2,DELETE:3}}},mod,mod.exports);
(async()=>{
  const api={base:'https://example.test',token:'phone-one',device:'gateway',certificatePath:()=>'/trusted.pem'};
  const lease=new mod.exports.CameraViewing();await lease.start(api,()=>{});
  assert.equal(timers.size,1);assert.equal(requests[0].ca,'/trusted.pem');
  api.token='different-login';lease.stop();await Promise.resolve();
  assert.equal(timers.size,0);assert.equal(requests.at(-1).token,'phone-one');
  assert.equal(requests.at(-1).method,3);
  // A late server response after exiting the page must be released immediately.
  hold=true;const pending=lease.start(api,()=>{});lease.stop();
  resolvePending({data:{view_id:'late-grant',expires_in:20}});await pending;
  assert.equal(timers.size,0);assert(requests.at(-1).path.endsWith('/late-grant'));
  console.log('PASS on-demand viewing: heartbeat lifecycle, session-bound cleanup, late acquisition cancellation');
})().catch(error=>{console.error(error);process.exitCode=1;});
