import { router, promptAction } from '@kit.ArkUI';
import { http } from '@kit.NetworkKit';
import { webview } from '@kit.ArkWeb';
import { DeviceSession } from '../services/DeviceSession';
import { DeviceLink, localIpv4 } from '../services/DeviceLink';
import { CareTheme } from '../theme/CareTheme';
interface LocalIdentity { device_id?:string; }
@Entry
@Component
struct LocalControl {
  @State address:string=DeviceLink.localIp||'192.168.2.1';
  @State source:string='';
  @State message:string='与主板连接同一 Wi-Fi，输入主板显示的 IP。配对后可管理无活动、夜灯、服药和本地消息。';
  @State busy:boolean=false;
  private controller:webview.WebviewController=new webview.WebviewController();
  private active:boolean=false;
  private token:string='';
  private timer:number=-1;
  aboutToAppear():void{this.active=true;this.token=DeviceSession.backend.token;this.timer=setInterval(()=>{if(this.source){this.verify();}},15000);}
  aboutToDisappear():void{this.active=false;this.source='';if(this.timer!==-1){clearInterval(this.timer);this.timer=-1;}}
  private async verify():Promise<boolean>{
    const api=DeviceSession.backend;
    if(!this.token||api.token!==this.token||api.tokenExpiresAt<=Date.now()){this.source='';this.message='请返回登录后再打开；本地热点离线使用可通过手机浏览器碰 NFC 进入。';return false;}
    try{const family=await api.refreshHousehold();if(!this.active||api.token!==this.token){this.source='';return false;}if(!(family.permissions||[]).includes('control')||!family.devices.some((d)=>d.device_id===api.device)){this.source='';this.message='当前账号没有主板控制权限';return false;}return true;}
    catch(_){this.source='';this.message='暂时无法核对账号权限。离线本地热点可使用浏览器 NFC 页面，并由板端确认配对。';return false;}
  }
  private async connect():Promise<void>{
    if(this.busy){return;}const ip=this.address.trim();if(!localIpv4(ip)){this.message='请输入主板的局域网 IPv4 地址，例如192.168.2.1';return;}
    this.busy=true;
    try{if(await this.verify()){
      const client=http.createHttp();let identity:LocalIdentity={};
      try{const result=await client.request('http://'+ip+':8080/status',{method:http.RequestMethod.GET,expectDataType:http.HttpDataType.STRING,connectTimeout:5000,readTimeout:5000,usingCache:false});if(result.responseCode!==200){throw new Error('主板状态读取失败');}identity=JSON.parse(result.result as string) as LocalIdentity;}finally{client.destroy();}
      if(!this.active||DeviceSession.backend.token!==this.token){return;}
      if(identity.device_id!==DeviceSession.backend.device){throw new Error('该地址与家庭主板不匹配，或固件未上报设备身份');}
      DeviceLink.localIp=ip;this.source='http://'+ip+':8080/';this.message='本地连接 · 按网页提示到主板确认配对。';
    }}catch(error){this.source='';this.message=(error as Error).message+'。请核对同一 Wi-Fi 和主板 IP。';}finally{this.busy=false;}
  }
  build(){Column(){
    Row(){Button('返回').height(48).backgroundColor(Color.Transparent).fontColor(CareTheme.primary).onClick(()=>{router.back();});Text('主板本地控制').fontSize(22).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).layoutWeight(1)}.width('100%').padding(12)
    if(!this.source){Column({space:16}){Text(this.message).fontSize(16).fontColor(CareTheme.secondary).width('100%');TextInput({text:this.address,placeholder:'主板 IP 地址'}).height(52).onChange((v:string)=>{this.address=v;});Button(this.busy?'核对权限中':'连接本地主板').enabled(!this.busy).height(48).width('100%').backgroundColor(CareTheme.primary).borderRadius(12).onClick(()=>{this.connect();})}.padding(20).width('100%')}
    else{
      Text(this.message).fontSize(13).fontColor(CareTheme.secondary).width('100%').padding(12)
      Web({src:this.source,controller:this.controller}).javaScriptAccess(true).domStorageAccess(true).onConfirm((event)=>{
        if(!event){return false;}
        if(!this.active||!event.url.startsWith(this.source)){event.result.handleCancel();return true;}
        try{promptAction.showDialog({title:'确认主板操作',message:event.message,buttons:[{text:'取消',color:CareTheme.secondary},{text:'确认',color:CareTheme.primary}]}).then((result)=>{
          if(result.index===1&&this.active&&DeviceSession.backend.token===this.token){event.result.handleConfirm();}else{event.result.handleCancel();}
        }).catch(()=>{event.result.handleCancel();});}catch(_){event.result.handleCancel();}
        return true;
      }).layoutWeight(1).width('100%').onLoadIntercept((event)=>{return !!event&&event.data.getRequestUrl()!=='about:blank'&&!event.data.getRequestUrl().startsWith(this.source);})
    }
  }.width('100%').height('100%').backgroundColor(CareTheme.background)}
}
