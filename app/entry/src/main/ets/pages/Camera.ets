import { CareTheme } from '../theme/CareTheme';
import { webview } from '@kit.ArkWeb';
import { router } from '@kit.ArkUI';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { CameraConnection, CameraFound } from '../services/CameraConnection';

@Entry
@Component
struct HomeCameraPage {
  @State address: string = '';
  @State source: string = '';
  @State username: string = 'user';
  @State password: string = '';
  @State message: string = '同一网络，一键看见家中';
  @State busy: boolean = false;
  @State settingsOpen: boolean = false;
  @State failed: boolean = false;
  private connector: CameraConnection = new CameraConnection();
  private controller: webview.WebviewController = new webview.WebviewController();
  private generation: number = 0;
  aboutToAppear(): void { this.restore().then(()=>{this.connect();}); }
  aboutToDisappear(): void { this.generation++; this.connector.cancel(); this.source=''; }
  private async restore(): Promise<void> {
    try {
      const context=getContext(this) as common.UIAbilityContext;
      const pref=await preferences.getPreferences(context,{name:'yihongan-camera'});
      this.address=await pref.get('address','') as string;
    } catch (error) { this.address=''; }
  }
  private stop(): void {
    this.generation++;this.connector.cancel();this.source='';this.busy=false;this.failed=false;
    this.message='已停止观看';
  }
  private async connect(manual:boolean=false): Promise<void> {
    if (this.busy) { return; }
    const generation=++this.generation;
    this.busy=true;this.failed=false;this.source='';
    this.message='正在查找摄像头…';
    try {
      let found:CameraFound;
      if (manual) { found=await this.connector.probe(this.address,this.username,this.password); }
      else { found=await this.connector.find(this.address,this.username,this.password,(value:string)=>{if(generation===this.generation){this.message=value;}}); }
      if (generation!==this.generation) { return; }
      this.address=found.address;this.source=found.address;this.settingsOpen=false;
      this.message='摄像头已就绪 · 正在加载实时画面';
      try {
        const context=getContext(this) as common.UIAbilityContext;
        const pref=await preferences.getPreferences(context,{name:'yihongan-camera'});
        await pref.put('address',found.address);await pref.flush();
      } catch (error) { /* Watching does not depend on persistence. */ }
    } catch (error) {
      if (generation===this.generation) { this.failed=true;this.message=(error as Error).message; }
    } finally { if(generation===this.generation){this.busy=false;} }
  }
  build() {
    Column({space:16}) {
      Row() {
        Button('‹ 返回').fontSize(14).fontColor(CareTheme.primary).backgroundColor(Color.Transparent).onClick(()=>{this.stop();router.back();})
        Blank()
        Text('颐鸿安').fontSize(15).fontWeight(FontWeight.Medium).fontColor(CareTheme.primary)
      }.width('100%')
      Column({space:6}) {
        Text('家中画面').fontSize(28).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).width('100%')
        Text('看见日常，安心相伴').fontSize(14).fontColor(CareTheme.secondary).width('100%')
      }.width('100%')
      Row({space:8}) {
        Circle().fill(this.failed?CareTheme.alarm:this.source?CareTheme.primary:CareTheme.secondary).width(7).height(7)
        Text(this.message).fontSize(13).fontColor(this.failed?CareTheme.alarm:CareTheme.secondary).layoutWeight(1)
      }.width('100%').padding(14).backgroundColor(Color.White).borderRadius(12)
      if (this.source) {
        Web({src:this.source,controller:this.controller})
          .onHttpAuthRequest((event)=>{
            if (event && event.host===this.source.split('/')[2].split(':')[0]) {event.handler.confirm(this.username,this.password);return true;}
            return false;
          })
          .onPageEnd(()=>{
            this.controller.runJavaScript("document.title='颐鸿安 · 家中画面';const h=document.querySelector('h1');if(h)h.style.display='none';const style=document.createElement('style');style.textContent='body{background:#F5F7FA;color:#20364B;margin:0;padding:12px;font-family:system-ui}body>p:first-of-type{display:none}button{background:#285B8A;border-radius:12px;font-size:14px;padding:10px 14px}#capture{display:none}img{border-radius:12px;margin-top:12px}pre{font-size:12px;color:#607286}';document.head.appendChild(style);").catch(()=>{});
          })
          .onErrorReceive(()=>{this.failed=true;this.message='画面加载失败，请检查网络后重新连接';})
          .layoutWeight(1).width('100%').borderRadius(16)
      } else {
        Column({space:18}) {
          Blank()
          Image($r('app.media.camera')).width(60).height(60)
          Text(this.busy?'正在寻找家中摄像头':'画面，由你开启').fontSize(20).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink)
          Text('摄像头与手机连接同一热点后，点击下方按钮即可连接。').fontSize(14).fontColor(CareTheme.secondary).textAlign(TextAlign.Center).lineHeight(23).padding({left:20,right:20})
          Blank()
        }.layoutWeight(1).width('100%').backgroundColor(CareTheme.softBlue).borderRadius(16)
      }
      Row({space:10}) {
        Button(this.busy?'连接中…':this.source?'重新连接':'一键连接摄像头').enabled(!this.busy).height(48).layoutWeight(1).backgroundColor(CareTheme.primary).fontSize(15).onClick(()=>{this.connect();})
        if(this.busy||this.source){Button(this.busy?'取消':'停止').height(48).fontSize(14).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(()=>{this.stop();})}
      }.width('100%')
      Text('连接设置 '+(this.settingsOpen?'⌃':'⌄')).fontSize(13).fontColor(CareTheme.primary).onClick(()=>{this.settingsOpen=!this.settingsOpen;})
      if(this.settingsOpen){
        Column({space:8}){
          TextInput({text:this.address,placeholder:'自动查找失败时填写摄像头地址'}).onChange((v:string)=>{this.address=v;})
          Row({space:8}){
            TextInput({text:this.username,placeholder:'摄像头账号'}).layoutWeight(1).onChange((v:string)=>{this.username=v;})
            TextInput({text:this.password,placeholder:'摄像头密码'}).type(InputType.Password).layoutWeight(1).onChange((v:string)=>{this.password=v;})
          }
          Button('使用此地址连接').enabled(!this.busy).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).width('100%').onClick(()=>{this.connect(true);})
        }.width('100%')
      }
      Text('仅在同一局域网观看 · 退出页面停止加载').fontSize(11).fontColor(CareTheme.secondary).margin({bottom:8})
    }.padding({left:20,right:20,top:12,bottom:18}).width('100%').height('100%').backgroundColor(CareTheme.background)
  }
}
