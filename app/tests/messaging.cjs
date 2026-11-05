const fs=require('fs'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH);
function load(file,requireStub){
  const source=fs.readFileSync(__dirname+'/../entry/src/main/ets/'+file,'utf8');
  const code=ts.transpileModule(source,{compilerOptions:{module:1,target:7}}).outputText;
  const mod={exports:{}};
  vm.runInNewContext('(function(require,module,exports){'+code+'})',{
    ArrayBuffer,Uint8Array,Date,Math,Error,Promise,Object,JSON
  })(requireStub||(()=>({})),mod,mod.exports);
  return mod.exports;
}
(async()=>{
  const {mergeMessages,messagePreview}=load('models/Messaging.ets');
  const m=(id,date,body='x')=>({id,created_at:date,body,conversation_id:'one',sender:'alice',kind:'text'});
  const original=[m('2','2026-11-05T02:00:00Z')];
  const result=mergeMessages(original,[m('1','2026-11-05T01:00:00Z'),m('2','2026-11-05T02:00:00Z','confirmed')]);
  assert.equal(result.length,2);assert.equal(result[0].id,'1');assert.equal(result[1].body,'confirmed');
  assert.equal(original[0].body,'x');assert.equal(messagePreview({...m('3',''),kind:'voice'}),'[语音]');
  let calls=[],response={responseCode:201,result:JSON.stringify({code:0,data:{id:'asset',kind:'image',size:4}})};
  let pending=null,client=null,permission=[-1],recorders=0;
  const network={http:{RequestMethod:{POST:'POST'},HttpDataType:{STRING:1,ARRAY_BUFFER:2},createHttp:()=>{
    client={destroyed:false,destroy(){this.destroyed=true},async request(url,options){calls.push({url,options});if(pending)return await pending;return response}};return client;
  }}};
  const {ChatMedia}=load('services/ChatMedia.ets',name=>{
    if(name==='@kit.NetworkKit')return network;
    if(name==='@kit.AbilityKit')return {abilityAccessCtrl:{createAtManager:()=>({requestPermissionsFromUser:async()=>({authResults:permission})})}};
    if(name==='@kit.MediaKit')return {media:{createAVRecorder:async()=>{recorders++;throw Error('not reached')}}};
    return {};
  });
  const media=new ChatMedia(),api={base:'https://server.test',token:'private-token',certificatePath:()=>'/private/ca.crt'};
  const uploaded=await media.upload(api,'chat/one',{bytes:new ArrayBuffer(4),type:'image/jpeg'});
  assert.equal(uploaded.id,'asset');assert.equal(calls[0].options.header.Authorization,'Bearer private-token');
  assert.equal(calls[0].options.caPath,'/private/ca.crt');assert(!calls[0].url.includes('private-token'));
  assert(calls[0].url.includes('chat%2Fone'));assert(client.destroyed);
  await assert.rejects(()=>media.upload(api,'one',{bytes:new ArrayBuffer(8*1024*1024+1),type:'image/jpeg'}),/无效/);
  await assert.rejects(()=>media.startRecord({}),/麦克风权限/);assert.equal(recorders,0);
  let resolve;pending=new Promise(r=>resolve=r);
  const task=media.upload(api,'one',{bytes:new ArrayBuffer(4),type:'image/jpeg'});
  await Promise.resolve();api.token='another-token';resolve(response);
  await assert.rejects(()=>task,/账号已切换/);assert(client.destroyed);
  pending=null;response={responseCode:403,result:new ArrayBuffer(1)};
  await assert.rejects(()=>media.download(api,'asset'),/权限/);assert(client.destroyed);
  response={responseCode:200,result:new ArrayBuffer(3)};
  assert.equal((await media.download(api,'asset')).byteLength,3);assert(client.destroyed);
  media.close();
  // A slow release from the old recording must never close a newer file.
  let releaseOld;
  const closed=[],unlinked=[];
  const {ChatMedia: RecordingMedia}=load('services/ChatMedia.ets',name=>name==='@kit.CoreFileKit'?{fileIo:{closeSync:f=>closed.push(f.fd),unlinkSync:p=>unlinked.push(p)}}:{});
  const recording=new RecordingMedia();
  recording.recorder={release:()=>new Promise(resolve=>{releaseOld=resolve;})};
  recording.recordFile={fd:1};recording.recordPath='old.m4a';
  const cancelling=recording.cancelRecord();
  recording.recordFile={fd:2};recording.recordPath='new.m4a';
  releaseOld();await cancelling;
  assert.deepEqual(closed,[1]);assert.deepEqual(unlinked,['old.m4a']);
  assert.equal(recording.recordFile.fd,2);assert.equal(recording.recordPath,'new.m4a');
  // Execute the real refresh method against >100 messages received offline.
  // It must bridge every middle page before marking the conversation read.
  const page=fs.readFileSync(__dirname+'/../entry/src/main/ets/pages/Messages.ets','utf8');
  const refresh=page.slice(page.indexOf('  private async refresh()'),page.indexOf('  private open(item:'));
  const rows=Array.from({length:402},(_,i)=>m(String(i),new Date(Date.UTC(2026,10,5,0,0,i)).toISOString()));
  let reads=0;
  const chatApi={token:'current',async request(path){
    if(path==='/api/v1/conversations')return {data:{items:[{conversation_id:'one'}]}};
    if(path==='/api/v1/contacts')return {data:{items:[]}};
    if(path.endsWith('/read')){reads++;return {data:{}}};
    const url=new URL('https://test'+path),before=Number(url.searchParams.get('before')||Infinity);
    const candidates=rows.filter(row=>Number(row.id)<before),items=candidates.slice(-100);
    return {data:{items,has_more:candidates.length>100,next_before:items.length?Number(items[0].id):null}};
  }};
  const body='class Harness { '+refresh+' }; module.exports=Harness;';
  const compiled=ts.transpileModule(body,{compilerOptions:{module:1,target:7}}).outputText;
  const fixture={exports:{}};
  vm.runInNewContext(compiled,{module:fixture,DeviceSession:{backend:chatApi},http:{RequestMethod:{GET:'GET',POST:'POST'}},mergeMessages,Promise});
  const harness=new fixture.exports();
  Object.assign(harness,{active:true,authorized:true,loading:false,generation:1,selected:{conversation_id:'one'},messages:[rows[0]],gapBefore:0,gapStop:'',
    current:op=>op===harness.generation,path:id=>'/api/v1/conversations/'+id,scrollBottom:()=>{},scroller:{isAtEnd:()=>true}});
  await harness.refresh();assert(harness.gapBefore>0);assert.equal(reads,0);
  await harness.refresh();assert.equal(harness.gapBefore,0);assert.equal(reads,1);assert.equal(harness.messages.length,402);
  assert.equal(new Set(harness.messages.map(row=>row.id)).size,402);
  harness.scroller.isAtEnd=()=>false;await harness.refresh();assert.equal(reads,1);
  harness.scroller.isAtEnd=()=>true;await harness.refresh();assert.equal(reads,2);
  console.log('PASS chat identity dedup/order, immutable merge, upload bounds, authenticated media, no token URL, denied microphone, stale-account rejection and request cleanup');
  console.log('PASS 401 offline arrivals: bounded cursor catch-up, no missing middle pages, no duplicates, mark-read only after catch-up');
})().catch(error=>{console.error(error);process.exit(1)});
