import { http } from '@kit.NetworkKit';
import { Backend } from './Backend';
interface ViewingGrant { view_id:string; expires_in:number; }

// Independent viewer leases: closing one phone never stops a second phone.
export class CameraViewing {
  private generation:number=0;
  private timer:number=-1;
  private client:Backend|null=null;
  private viewId:string='';
  private busy:boolean=false;
  private failed:((message:string)=>void)|null=null;

  private async release(api:Backend,id:string):Promise<void>{
    if(!id){return;}
    try{await api.request(api.path('camera/viewing/')+encodeURIComponent(id),http.RequestMethod.DELETE);}catch(error){}
  }
  stop():void{
    this.generation++;
    if(this.timer>=0){clearInterval(this.timer);this.timer=-1;}
    const api=this.client,id=this.viewId;
    this.client=null;this.viewId='';this.busy=false;
    if(api&&id){this.release(api,id);}
  }
  private async acquire(api:Backend,generation:number):Promise<void>{
    const response=await api.request(api.path('camera/viewing'),http.RequestMethod.POST,'{}');
    const grant=response.data as ViewingGrant;
    if(!grant.view_id){throw new Error('无法开启摄像头观看');}
    if(generation!==this.generation){await this.release(api,grant.view_id);return;}
    this.viewId=grant.view_id;
  }
  async start(source:Backend,failed:(message:string)=>void):Promise<void>{
    this.stop();const generation=this.generation;this.failed=failed;
    // Capture the current session, device and CA; late cleanup must not use another login.
    const api=new Backend();api.base=source.base;api.token=source.token;
    api.device=source.device;api.caPath=source.certificatePath();this.client=api;
    await this.acquire(api,generation);
    if(generation!==this.generation){return;}
    this.timer=setInterval(()=>{this.renew(generation);},6000);
  }
  private async renew(generation:number):Promise<void>{
    const api=this.client;
    if(!api||generation!==this.generation||this.busy){return;}
    this.busy=true;
    try{
      if(this.viewId){
        try{await api.request(api.path('camera/viewing/')+encodeURIComponent(this.viewId),http.RequestMethod.PUT,'{}');}
        catch(error){if(generation===this.generation){this.viewId='';await this.acquire(api,generation);}}
      }else{await this.acquire(api,generation);}
    }catch(error){if(generation===this.generation&&this.failed){this.failed('观看请求中断，恢复网络后自动重试');}}
    finally{if(generation===this.generation){this.busy=false;}}
  }
}
