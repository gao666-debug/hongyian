// 语音指令解析的回归用例（纯字符串逻辑，不需要 SDK、不连设备，2 秒跑完）。
//
//   node app/tools/check_voice_command.js
//   node app/tools/check_voice_command.js --app F:/hongyian-app-build   # 测另一份工作副本
//
// 原理：把 VoiceCommand.ets 的类型标注去掉后直接跑，避免在测试里手抄一份逻辑，
// 保证测的就是 App 实际用的那份代码。改 VoiceCommand.ets 后跑一次即可。
const fs = require('fs');
const path = require('path');

// 仓库里的 app/ 是提交快照，日常改的是构建目录，所以要能指到别处。
const argAt = process.argv.indexOf('--app');
const APP = argAt > 0 && process.argv[argAt + 1] ? process.argv[argAt + 1] : path.join(__dirname, '..');
const ETS = path.join(APP, 'entry', 'src', 'main', 'ets', 'services', 'VoiceCommand.ets');
let source = fs.readFileSync(ETS, 'utf8');
// 源码是 CRLF，先统一成 LF，否则下面按行匹配的规则会失手。
source = source.replace(/\r\n/g, '\n');
source = source.replace(/export interface[\s\S]*?\n}\n/g, '');
source = source.replace(/interface[\s\S]*?\n}\n/g, '');
source = source.replace(/\bprivate static /g, 'static ');
source = source.replace(/\bstatic readonly /g, 'static ');
source = source.replace(/\bexport class /, 'class ');
source = source.replace(/:\s*(string\[\]|number\[\]|VoiceRule\[\]|VoiceIntent\[\]|VoiceRule|VoiceIntent|string|boolean|number|void)(?![\w\[\]])/g, '');
source += '\nmodule.exports = VoiceCommand;\n';
const tmp = path.join(__dirname, '.voice_command.check.js');
fs.writeFileSync(tmp, source);
const VC = require(tmp);
fs.unlinkSync(tmp);

const cases=[
 ['小颐开灯','light_on'],['小颐关灯','light_off'],['小颐自动夜灯','light_auto'],
 ['小颐啊，帮我把灯打开吧','light_on'],['把灯给我关了','light_off'],['灯关一下','light_off'],
 ['小颐救命','sos'],['小颐我摔倒了','sos'],['小依我有点不舒服','sos'],['小颐快来','sos'],
 ['小颐我吃过药了','medicine'],['小颐我吃了药','medicine'],['小颐药吃了','medicine'],
 ['小颐现在温度多少','temperature'],['小颐湿度多少','temperature'],['小颐冷不冷','temperature'],
 ['小颐看报警','alarm'],['小颐确认报警','ack'],['小颐报警我收到了','ack'],
 // 与板子词条对齐后新增的四类：取消求救 / 静音 / 燃气 / 光照
 ['小颐取消求救','help_off'],['小颐没事了','help_off'],['小颐别叫了','help_off'],
 ['小颐报警取消','ack'],
 ['小颐静音','mute'],['小颐别响了','mute'],['小颐太吵了','mute'],
 ['小颐取消静音','unmute'],['小颐恢复播报','unmute'],
 ['小颐燃气多少','gas'],['小颐有没有燃气泄漏','gas'],['小颐煤气浓度多少','gas'],
 ['小颐光照多少','light_level'],['小颐房间亮不亮','light_level'],['小颐光线够不够','light_level'],
 ['小颐看趋势','trend'],['小颐看活动记录','trend'],['小颐看设备','device'],
 ['小颐打开摄像头','camera'],['小颐你会什么','help'],['小颐开登','light_on'],
 ['小颐关登','light_off'],['小颐抱警','alarm'],['小颐温渡多少','temperature'],
 ['小颐湿渡','temperature'],['小颐要水','unknown'],['今天天气怎么样','unknown'],['小颐','awake']
];
let bad=0;
for(const [q,exp] of cases){const r=VC.parse(q);if(r.type!==exp){bad++;console.log('FAIL',q,'=>',r.type,'期望',exp,'|',r.reply);}}
console.log('失败',bad,'/',cases.length);
console.log('--- 模糊与建议 ---');
for(const q of ['小颐开个灯','小颐我药吃过了','小颐温度几多','小颐帮我看下报警记录','小颐我摔了一跤']){
  const r=VC.parse(q);console.log(q,'=>',r.type,'|',r.reply);
}

console.log('--- 不该误触发的日常话 ---');
for(const q of ['小颐我想看电视','小颐我要吃饭','小颐今天几号','小颐我有点累','小颐家里有人吗',
  '小颐开开门','小颐明天天气','小颐帮我数一下','小颐我睡不着','电视关掉']){
  const r=VC.parse(q);console.log(q,'=>',r.type,'|',r.reply);
}
