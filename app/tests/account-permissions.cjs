const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH||'F:/DevEco Studio/tools/hvigor/hvigor-ohos-plugin/node_modules/typescript');
let saved='';
const storage={get:async()=>saved,put:async(_key,value)=>{saved=value;},flush:async()=>{}};
function load(file){
  const code=ts.transpileModule(fs.readFileSync(path.join(__dirname,'../entry/src/main/ets',file),'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
  const module={exports:{}};
  const requireStub=name=>name==='@kit.ArkData'?{preferences:{getPreferences:async()=>storage}}:require(name);
  vm.runInNewContext('(function(require,module,exports){'+code+'})',{})(requireStub,module,module.exports);
  return module.exports;
}
(async()=>{
  const {roleAllows,collaborationError}=load('models/Permissions.ets');
  assert(collaborationError('Alarm already handled').includes('已有处理记录'));
  assert(collaborationError('Alarm is being handled by another member').includes('其他成员正在处理'));
  assert.equal(collaborationError('连接超时'),'连接超时');
  const {backendErrorMessage}=load('models/Models.ets');
  assert.equal(collaborationError(backendErrorMessage(409,{detail:'Alarm already handled'})),'此报警已有处理记录，请刷新查看处置结果');
  assert(backendErrorMessage(429,{}).includes('同一网络'));
  for(const role of ['community','property']){
    assert(roleAllows(role,'alarms.read'));assert(roleAllows(role,'alarms.handle'));
    for(const action of ['view','control','members.manage'])assert(!roleAllows(role,action));
  }
  assert(!roleAllows('viewer','alarms.handle'));assert(!roleAllows('caregiver','members.manage'));
  assert(!roleAllows('unknown','control'));assert(!roleAllows('','view'));
  const {Accounts}=load('services/Accounts.ets');
  const entry=name=>({server:'https://example.test',username:name,role:'viewer',token:'token-'+name,expiresAt:Date.now()+10000,devices:1,lastUsed:Date.now()});
  await Accounts.save({},entry('alice'));
  const list=await Accounts.save({},entry('bob'));
  assert.equal(list.find(x=>x.username==='alice').token,'');
  assert.equal(list.find(x=>x.username==='bob').token,'token-bob');
  const removed=await Accounts.remove({},'https://example.test','bob');
  assert(!removed.some(x=>x.username==='bob'));
  console.log('PASS role restrictions, unknown-role denial, cross-account token cleanup, local account removal');
})().catch(error=>{console.error(error);process.exit(1);});
