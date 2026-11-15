import { http } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { JpegStream } from '../models/CameraMedia';
import { Backend } from './Backend';
interface StreamTicket { ticket:string; }
export class CameraFeed {
  private client:http.HttpRequest|null=null;
  private ticketClient:http.HttpRequest|null=null;
  private pollTimer:number=-1;
  private generation:number=0;
  stop():void{this.generation++;if(this.pollTimer>=0){clearTimeout(this.pollTimer);this.pollTimer=-1;}if(this.client){this.client.destroy();this.client=null;}if(this.ticketClient){this.ticketClient.destroy();this.ticketClient=null;}}
  // ESP32-CAM 的 WebServer 是单线程阻塞式，80/81 共用同一份处理能力；lwIP 的 TCP PCB 池
  // 只有十几个名额，每个关掉的连接都要占 TIME_WAIT。实测真机 200ms 一帧（约 5 个连接/秒），
  // 连续 40 秒后 PCB 池耗尽，accept 拿不到资源，整机从网络消失且不自行恢复（需断电）。
  // 1 秒/帧（省流）在同样硬件上连测 30 次全通、可长时间稳定。故局域网固定 1 秒/帧。
  startPolling(base:string,user:string,password:string,onFrame:(jpeg:Uint8Array)=>void,onEnd:()=>void,intervalMs:number=1000):void{
    this.stop();
    const generation=this.generation;
    // 值前的空格不能省：OpenHarmony 拼 "key:value" 不加空格，ESP32 解析时固定跳过一个字符。
    const auth=' Basic '+new util.Base64Helper().encodeToStringSync(new util.TextEncoder().encodeInto(user+':'+password));
    let misses=0;
    const tick=async():Promise<void>=>{
      if(generation!==this.generation){return;}
      const client=http.createHttp();
      try{
        const response=await client.request(base+'/capture?t='+Date.now(),{
          method:http.RequestMethod.GET,
          header:{'Authorization':auth,'Cache-Control':'no-store'},
          usingProtocol:http.HttpProtocol.HTTP1_1,usingCache:false,usingProxy:false,
          connectTimeout:2500,readTimeout:5000,expectDataType:http.HttpDataType.ARRAY_BUFFER
        });
        if(generation!==this.generation){return;}
        const body=response.result as ArrayBuffer;
        const ok=response.responseCode===200&&body instanceof ArrayBuffer&&body.byteLength>512;
        if(ok){misses=0;onFrame(new Uint8Array(body));}
        else{misses++;}
      }catch(error){misses++;}
      finally{client.destroy();}
      if(generation!==this.generation){return;}
      if(misses>=5){onEnd();return;}
      this.pollTimer=setTimeout(()=>{tick();},misses>0?500:intervalMs) as number;
    };
    this.pollTimer=setTimeout(()=>{tick();},0) as number;
  }
  startRemote(api:Backend,onFrame:(jpeg:Uint8Array)=>void,onEnd:()=>void):void {
    this.stop(); const generation=this.generation; const token=api.token;
    const client=http.createHttp(); this.client=client;const parser=new JpegStream();let ended=false;
    const finish=()=>{if(generation===this.generation&&!ended){ended=true;onEnd();}};
    client.on('dataReceive',(data:ArrayBuffer)=>{
      if(generation!==this.generation||token!==api.token){return;}
      try{for(const jpeg of parser.feed(data)){onFrame(jpeg);}}catch(error){client.destroy();finish();}
    });
    client.on('dataEnd',finish);
    const options:http.HttpRequestOptions={header:{Authorization:'Bearer '+token},
      usingCache:false,usingProtocol:http.HttpProtocol.HTTP1_1,connectTimeout:8000,readTimeout:600000};
    const cert=api.certificatePath();if(cert){options.caPath=cert;}
    client.requestInStream(api.base+api.path('camera/stream'),options)
      .then((code:number)=>{if(code!==200){finish();}}).catch(finish);
  }
  async start(base:string,user:string,password:string,onFrame:(jpeg:Uint8Array)=>void,onEnd:()=>void):Promise<void>{
    this.stop();const generation=this.generation;
    // 值前的空格不能省：OpenHarmony 拼 "key:value" 不加空格，ESP32 解析时固定跳过一个字符。
    const auth=' Basic '+new util.Base64Helper().encodeToStringSync(new util.TextEncoder().encodeInto(user+':'+password));
    const control=http.createHttp();this.ticketClient=control;
    try{
      const result=await control.request(base+'/stream-ticket',{header:{Authorization:auth},usingProtocol:http.HttpProtocol.HTTP1_1,usingProxy:false,usingCache:false,connectTimeout:3000,readTimeout:6000});
      if(generation!==this.generation){return;}
      if(result.responseCode!==200){throw new Error('无法获取视频访问许可');}
      const ticket=(JSON.parse(result.result as string) as StreamTicket).ticket;
      if(!ticket){throw new Error('视频许可无效');}
      const client=http.createHttp();this.client=client;const parser=new JpegStream();let ended=false;
      const finish=()=>{if(generation===this.generation&&!ended){ended=true;onEnd();}};
      client.on('dataReceive',(data:ArrayBuffer)=>{if(generation!==this.generation){return;}try{for(const jpeg of parser.feed(data)){onFrame(jpeg);}}catch(error){client.destroy();finish();}});
      client.on('dataEnd',finish);
      // Native incremental receive avoids a fresh HTTP connection for every JPEG.
      const host=base.split('/')[2].split(':')[0];
      client.requestInStream(base.split('://')[0]+'://'+host+':81/stream?ticket='+encodeURIComponent(ticket),{usingProxy:false,usingCache:false,usingProtocol:http.HttpProtocol.HTTP1_1,connectTimeout:3000,readTimeout:600000}).then((code:number)=>{if(code!==200){finish();}}).catch(finish);
    }finally{control.destroy();if(this.ticketClient===control){this.ticketClient=null;}}
  }
}
