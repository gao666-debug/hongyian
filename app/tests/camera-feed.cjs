const fs=require('fs'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH);
function load(file,imports={}){
  const source=fs.readFileSync(__dirname+'/../entry/src/main/ets/'+file,'utf8');
  const mod={exports:{}};
  const js=ts.transpileModule(source,{compilerOptions:{module:1,target:7}}).outputText;
  vm.runInNewContext('(function(require,module,exports){'+js+'})',{Uint8Array,ArrayBuffer,DataView,Math,Error})(name=>imports[name]||{},mod,mod.exports);
  return mod.exports;
}
const media=load('models/CameraMedia.ets');
let client,request;
const network={http:{HttpProtocol:{HTTP1_1:1},createHttp:()=>{
  client={events:{},on(name,callback){this.events[name]=callback;},destroy(){this.destroyed=true;},
    async requestInStream(url,options){request={url,options};return 200;}};
  return client;
}}};
const {CameraFeed}=load('services/CameraFeed.ets',{'@kit.NetworkKit':network,'../models/CameraMedia':media});
const api={base:'https://example.test',token:'test-session-token',path:p=>'/api/v1/devices/gateway/'+p,certificatePath:()=>'/cache/trusted.crt'};
const feed=new CameraFeed();let frames=0,ends=0;
feed.startRemote(api,()=>frames++,()=>ends++);
assert.equal(request.url,'https://example.test/api/v1/devices/gateway/camera/stream');
assert(!request.url.includes(api.token));
assert.equal(request.options.header.Authorization,'Bearer '+api.token);
assert.equal(request.options.caPath,'/cache/trusted.crt');
const data=Uint8Array.from([255,216,1,2,255,217]).buffer;
client.events.dataReceive(data);assert.equal(frames,1);
api.token='other-session';client.events.dataReceive(data);assert.equal(frames,1);
feed.stop();client.events.dataReceive(data);client.events.dataEnd();
assert.equal(frames,1);assert.equal(ends,0);assert(client.destroyed);
console.log('PASS remote camera: bearer header, certificate verification, no URL credentials, session switch and stop cleanup');
