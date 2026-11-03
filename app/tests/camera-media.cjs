const fs=require('fs'),vm=require('vm'),assert=require('assert');
const ts=require(process.env.TYPESCRIPT_PATH);
const source=fs.readFileSync(__dirname+'/../entry/src/main/ets/models/CameraMedia.ets','utf8');
const m={exports:{}};vm.runInNewContext('(function(module,exports){'+ts.transpileModule(source,{compilerOptions:{module:1,target:7}}).outputText+'})',{Uint8Array,DataView,ArrayBuffer,Math,Error})(m,m.exports);
const {JpegStream,makeAvi}=m.exports,p=new JpegStream();
const wire=Buffer.from('--frame\r\nContent-Type: image/jpeg\r\n\r\n','ascii');const jpg=Buffer.from([255,216,1,2,255,217]);const stream=Buffer.concat([wire,jpg,wire,jpg]);let received=[];
for(const b of stream){received.push(...p.feed(Uint8Array.from([b]).buffer));}
assert.equal(received.length,2);assert.deepEqual(Buffer.from(received[0]),jpg);
assert.throws(()=>new JpegStream().feed(new ArrayBuffer(2097153)),/过大/);
const video=Buffer.from(makeAvi([{jpeg:new Uint8Array(jpg),at:1000},{jpeg:new Uint8Array(jpg),at:1600}],640,480,2000));
assert.equal(video.toString('ascii',0,4),'RIFF');assert.equal(video.readUInt32LE(4),video.length-8);assert.equal(video.toString('ascii',8,12),'AVI ');
assert.equal(video.readUInt32LE(48),5);assert.equal(video.readUInt32LE(64),640);assert.equal(video.readUInt32LE(68),480);
let pos=224,count=0;while(video.toString('ascii',pos,pos+4)==='00dc'){const n=video.readUInt32LE(pos+4);assert.deepEqual(video.subarray(pos+8,pos+8+n),jpg);pos+=8+n+(n%2);count++;}assert.equal(count,5);assert.equal(video.toString('ascii',pos,pos+4),'idx1');assert.equal(video.readUInt32LE(pos+4),80);
assert.throws(()=>makeAvi([],640,480,0),/尚无/);
fs.writeFileSync('/tmp/yihongan-camera-test.avi',video);
console.log('PASS multipart arbitrary splits, bounded input, AVI RIFF sizes, timed gap holding, JPEG chunks and frame index');
