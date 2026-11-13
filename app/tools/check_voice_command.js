// 语音指令解析的回归用例（纯字符串逻辑，不需要 SDK、不连设备，2 秒跑完）。
//
//   node app/tools/check_voice_command.js
//
// 原理：把 VoiceCommand.ets 的类型标注去掉后直接跑，避免在测试里手抄一份逻辑，
// 保证测的就是 App 实际用的那份代码。改 VoiceCommand.ets 后跑一次即可。
const fs = require('fs');
const path = require('path');

const ETS = path.join(__dirname, '..', 'entry', 'src', 'main', 'ets', 'services', 'VoiceCommand.ets');
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
