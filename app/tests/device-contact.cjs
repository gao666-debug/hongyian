const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH);
function load(file){const src=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets',file),'utf8');const js=ts.transpileModule(src,{compilerOptions:{module:1,target:7}}).outputText;const m={exports:{}};vm.runInNewContext('(function(module,exports){'+js+'})',{decodeURIComponent})(m,m.exports);return m.exports;}
const {mergeDeviceContacts,contactText}=load('models/DeviceContact.ets');
const msg=(boot,id,revision,cursor)=>({boot_id:boot,message_id:id,revision,cursor,direction:'board',code:2,seen:revision>1});
let merged=mergeDeviceContacts([msg(1,1,1,1)],[msg(1,1,2,2),msg(1,1,1,1),msg(2,1,1,3)]);
assert.equal(merged.length,2);assert(merged.find(x=>x.boot_id===1).seen);assert.equal(merged.find(x=>x.boot_id===1).revision,2);assert.equal(contactText(merged[0]),'请给我打电话');
assert.equal(mergeDeviceContacts([],Array.from({length:250},(_,i)=>msg(1,i,1,i))).length,200);
const {DeviceLink,localIpv4}=load('services/DeviceLink.ets');
assert(DeviceLink.capture('yihongan://device?device_id=main_001&local_ip=192.168.2.1'));assert.equal(DeviceLink.pending,'main_001');assert.equal(DeviceLink.localIp,'192.168.2.1');
for(const uri of ['https://evil.test','yihongan://device?device_id=main&token=secret','yihongan://device?device_id=%2Fadmin','yihongan://device?device_id=main&local_ip=8.8.8.8','yihongan://device?device_id=main&local_ip=127.0.0.1'])assert(!DeviceLink.capture(uri));
for(const ip of ['127.0.0.1','8.8.8.8','192.168.2.255','172.016.0.1','10.1.2.999','10.1.2.0','10.1.2.3:8080','10.1.2.3/path'])assert(!localIpv4(ip),ip);
for(const ip of ['192.168.2.1','10.46.76.214','172.16.1.1','172.31.1.1'])assert(localIpv4(ip),ip);
console.log('PASS device contacts: revision deduplication, reboot identity, bounded list; NFC link allowlist, private IPv4 and no credentials');
