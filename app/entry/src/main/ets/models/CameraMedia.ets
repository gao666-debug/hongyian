// Bounded multipart parser shared by the native live viewer and recording tests.
export class JpegStream {
  private pending: Uint8Array = new Uint8Array(0);
  feed(chunk: ArrayBuffer): Uint8Array[] {
    const incoming=new Uint8Array(chunk);
    if(this.pending.length+incoming.length>2097152){this.pending=new Uint8Array(0);throw new Error('视频帧过大');}
    const bytes=new Uint8Array(this.pending.length+incoming.length);bytes.set(this.pending);bytes.set(incoming,this.pending.length);
    const frames:Uint8Array[]=[];let cursor=0;
    while(cursor<bytes.length){
      let start=-1,end=-1;
      for(let i=cursor;i+1<bytes.length;i++){if(bytes[i]===255&&bytes[i+1]===216){start=i;break;}}
      if(start<0){cursor=Math.max(cursor,bytes.length-1);break;}
      for(let i=start+2;i+1<bytes.length;i++){if(bytes[i]===255&&bytes[i+1]===217){end=i+2;break;}}
      if(end<0){cursor=start;break;}
      frames.push(bytes.slice(start,end));cursor=end;
    }
    this.pending=bytes.slice(cursor);return frames;
  }
}
export interface RecordedFrame { jpeg: Uint8Array; at: number; }
// Silent MJPEG AVI, 5 fps timeline. Gaps hold the last received frame.
export function makeAvi(frames:RecordedFrame[], width:number,height:number,ended:number):ArrayBuffer {
  if(!frames.length||width<1||height<1){throw new Error('尚无可保存画面');}
  const duration=Math.min(60000,Math.max(200,ended-frames[0].at));
  const timeline:Uint8Array[]=[];let current=0;
  for(let tick=0;tick<Math.ceil(duration/200);tick++){
    while(current+1<frames.length&&frames[current+1].at<=frames[0].at+tick*200){current++;}
    timeline.push(frames[current].jpeg);
  }
  let movie=0;for(const jpeg of timeline){movie+=8+jpeg.length+(jpeg.length%2);}
  const total=224+movie+8+timeline.length*16;
  if(total>33554432){throw new Error('录像超过 32 MB，请缩短录制');}
  const bytes=new Uint8Array(total);const view=new DataView(bytes.buffer);let pos=0;
  const tag=(value:string):void=>{for(let i=0;i<4;i++){bytes[pos++]=value.charCodeAt(i);}}
  const u32=(value:number):void=>{view.setUint32(pos,value,true);pos+=4;}
  const u16=(value:number):void=>{view.setUint16(pos,value,true);pos+=2;}
  tag('RIFF');u32(total-8);tag('AVI ');tag('LIST');u32(192);tag('hdrl');
  tag('avih');u32(56);u32(200000);u32(0);u32(0);u32(16);u32(timeline.length);u32(0);u32(1);u32(0);u32(width);u32(height);for(let i=0;i<4;i++){u32(0);}
  tag('LIST');u32(116);tag('strl');tag('strh');u32(56);tag('vids');tag('MJPG');u32(0);u16(0);u16(0);u32(0);u32(1);u32(5);u32(0);u32(timeline.length);u32(0);u32(0xffffffff);u32(0);u16(0);u16(0);u16(width);u16(height);
  tag('strf');u32(40);u32(40);u32(width);u32(height);u16(1);u16(24);tag('MJPG');u32(width*height*3);for(let i=0;i<4;i++){u32(0);}
  tag('LIST');u32(movie+4);tag('movi');const offsets:number[]=[];let offset=4;
  for(const jpeg of timeline){offsets.push(offset);tag('00dc');u32(jpeg.length);bytes.set(jpeg,pos);pos+=jpeg.length;if(jpeg.length%2){bytes[pos++]=0;}offset+=8+jpeg.length+(jpeg.length%2);}
  tag('idx1');u32(timeline.length*16);
  for(let i=0;i<timeline.length;i++){tag('00dc');u32(16);u32(offsets[i]);u32(timeline[i].length);}
  return bytes.buffer;
}
