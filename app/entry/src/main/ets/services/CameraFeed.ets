import { http } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { JpegStream } from '../models/CameraMedia';
import { Backend } from './Backend';
interface StreamTicket { ticket:string; }
export class CameraFeed {
  private client:http.HttpRequest|null=null;
  private ticketClient:http.HttpRequest|null=null;
  private generation:number=0;
  stop():void{this.generation++;if(this.client){this.client.destroy();this.client=null;}if(this.ticketClient){this.ticketClient.destroy();this.ticketClient=null;}}
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
