import { CareTheme } from '../theme/CareTheme';
import { http } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { CameraFeed } from '../services/CameraFeed';
import { DeviceSession } from '../services/DeviceSession';
import { RecordedFrame, makeAvi } from '../models/CameraMedia';
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
  @State frame: image.PixelMap | undefined = undefined;
  private frameTimer: number = -1;
  private generation: number = 0;
  private feed:CameraFeed=new CameraFeed();
  @State smooth:boolean=true;
  @State recording:boolean=false;
  @State clipReady:boolean=false;
  @State mediaMessage:string='';
  @State saving:boolean=false;
  private latest:Uint8Array|null=null;
  private decoding:boolean=false;
  private queued:Uint8Array|null=null;
  private recorded:RecordedFrame[]=[];
  private recordBytes:number=0;
  private recordTimer:number=-1;
  private recordWidth:number=0;
  private recordHeight:number=0;
  private recordingData:ArrayBuffer|null=null;
  private frameWidth:number=0;
  private frameHeight:number=0;
  private noticeAt:number=0;
  private lastFrameAt:number=0;
  private async saveMedia(video:boolean):Promise<void>{
    if(this.saving){return;}
    const bytes=video?this.recordingData:this.latest?.buffer.slice(this.latest.byteOffset,this.latest.byteOffset+this.latest.byteLength);
    if(!bytes||(!video&&Date.now()-this.lastFrameAt>5000)){this.mediaMessage='请先等待有效画面';return;}
    this.saving=true;
    try{
      const context=getContext(this) as common.UIAbilityContext;
      const chooser=new picker.DocumentViewPicker(context);
      const extension=video?'avi':'jpg';
      const result=await chooser.save({newFileNames:['颐鸿安-'+Date.now()+'.'+extension],fileSuffixChoices:[video?'无声录像|.avi':'摄像头截图|.jpg']});
      if(!result.length){this.mediaMessage='已取消保存';return;}
      const file=fs.openSync(result[0],fs.OpenMode.WRITE_ONLY|fs.OpenMode.TRUNC);
      try{let offset=0;while(offset<bytes.byteLength){const n=fs.writeSync(file.fd,bytes.slice(offset));if(n<=0){throw new Error('文件写入失败');}offset+=n;}}finally{fs.closeSync(file);}
      this.mediaMessage=video?'录像已保存（AVI 无声视频）':'截图已保存至所选位置';
    }catch(error){this.mediaMessage='保存失败，请重新选择保存位置';}finally{this.saving=false;}
  }
  private toggleRecording():void{
    if(this.recording){this.finishRecording();return;}
    if(!this.latest||!this.frameWidth||Date.now()-this.lastFrameAt>5000){this.mediaMessage='画面就绪后才能录像';return;}
    this.recorded=[];this.recordBytes=0;this.recordWidth=this.frameWidth;this.recordHeight=this.frameHeight;
    this.recordingData=null;this.clipReady=false;this.recording=true;this.mediaMessage='正在录像 · 最长 60 秒 · 无声';
    this.recordTimer=setTimeout(()=>{this.finishRecording();},60000);
  }
  private finishRecording():void{
    if(!this.recording){return;}this.recording=false;
    if(this.recordTimer>=0){clearTimeout(this.recordTimer);this.recordTimer=-1;}
    try{this.recordingData=makeAvi(this.recorded,this.recordWidth,this.recordHeight,Date.now());this.clipReady=true;this.mediaMessage='录像已生成，点击保存录像选择位置';}
    catch(error){this.mediaMessage=(error as Error).message;}
    this.recorded=[];
  }
  private offerFrame(jpeg:Uint8Array,generation:number):void{
    if(generation!==this.generation){return;}
    this.latest=jpeg;this.lastFrameAt=Date.now();
    const now=Date.now();
    if(this.recording&&(!this.recorded.length||now-this.recorded[this.recorded.length-1].at>=200)){
      if(this.recordBytes+jpeg.length>16777216){this.finishRecording();}
      else{this.recorded.push({jpeg:jpeg,at:now});this.recordBytes+=jpeg.length;}
    }
    this.queued=jpeg;if(!this.decoding){this.decodeFrames(generation);}
  }
  private async decodeFrames(generation:number):Promise<void>{
    this.decoding=true;
    try{while(this.queued&&generation===this.generation){
      const jpeg=this.queued;this.queued=null;
      const decoder=image.createImageSource(jpeg.buffer.slice(jpeg.byteOffset,jpeg.byteOffset+jpeg.byteLength));
      let next:image.PixelMap;
      try{next=await decoder.createPixelMap();}finally{await decoder.release();}
      if(generation!==this.generation){await next.release();break;}
      const info=await next.getImageInfo();this.frameWidth=info.size.width;this.frameHeight=info.size.height;
      const old=this.frame;this.frame=next;if(old){await old.release();}
      this.failed=false;
      if(Date.now()-this.noticeAt>1000){this.noticeAt=Date.now();this.message=(this.smooth?'流畅观看':'省流观看')+' · '+new Date().toLocaleTimeString();}
    }}catch(error){if(generation===this.generation){this.failed=true;this.message='画面解码失败，正在等待下一帧';}}
    finally{this.decoding=false;if(this.queued){this.decodeFrames(this.generation);}}
  }
  private async startFeed(generation:number):Promise<void>{
    try{await this.feed.start(this.source,this.username,this.password,(jpeg:Uint8Array)=>{this.offerFrame(jpeg,generation);},()=>{
      if(generation===this.generation){this.failed=true;this.message='视频中断，正在重新连接';this.frameTimer=setTimeout(()=>{this.startFeed(generation);},2000);}
    });}catch(error){if(generation===this.generation){this.failed=true;this.message='流畅连接失败，可切换省流模式';this.frameTimer=setTimeout(()=>{this.startFeed(generation);},3000);}}
  }
  private changeMode():void{if(this.recording){return;}this.smooth=!this.smooth;if(this.source){this.connect(true);}}

  aboutToAppear(): void { this.restore().then(()=>{this.connect();}); }
  aboutToDisappear(): void { this.stop(); }
  private async restore(): Promise<void> {
    try {
      const context=getContext(this) as common.UIAbilityContext;
      const pref=await preferences.getPreferences(context,{name:'yihongan-camera'});
      this.address=await pref.get('address','') as string;
    } catch (error) { this.address=''; }
  }
  private async releaseStream(): Promise<void> {
    this.feed.stop();this.queued=null;this.latest=null;this.finishRecording();
    if(this.frameTimer>=0){clearTimeout(this.frameTimer);this.frameTimer=-1;}
    const old=this.frame;this.frame=undefined;
    if(old){await old.release();}
  }
  private async refreshFrame(generation:number): Promise<void> {
    if(generation!==this.generation||!this.source){return;}
    const client=http.createHttp();
    try {
      const basic=new util.Base64Helper().encodeToStringSync(new util.TextEncoder().encodeInto(this.username+':'+this.password));
      const response=await client.request(this.source+'/capture?t='+Date.now(),{
        method:http.RequestMethod.GET,header:{'Authorization':' Basic '+basic,'Cache-Control':'no-store'},
        usingProtocol:http.HttpProtocol.HTTP1_1,usingCache:false,usingProxy:false,connectTimeout:2500,readTimeout:6000,expectDataType:http.HttpDataType.ARRAY_BUFFER
      });
      if(response.responseCode!==200){throw new Error('暂未取得画面');}
      if(generation===this.generation){this.offerFrame(new Uint8Array(response.result as ArrayBuffer),generation);}

    } catch(error){if(generation===this.generation){this.failed=true;this.message='画面暂时中断，正在自动重试';}}
    finally{
      client.destroy();
      if(generation===this.generation&&this.source){this.frameTimer=setTimeout(()=>{this.refreshFrame(generation);},1000);}
    }
  }
  private stop(): void {
    const generation=++this.generation;
    this.connector.cancel();this.busy=false;this.failed=false;
    this.message='已停止观看';
    this.releaseStream().finally(()=>{if(generation===this.generation){this.source='';}});
  }
  private async connect(manual:boolean=false): Promise<void> {
    if(!DeviceSession.backend.token){this.failed=true;this.message='请先登录家庭账号，再访问摄像头';return;}
    if (this.busy) { return; }
    const generation=++this.generation;
    this.busy=true;this.failed=false;
    await this.releaseStream();
    if(generation!==this.generation){return;}
    this.source='';
    this.message='正在查找摄像头…';
    try {
      let found:CameraFound;
      if (manual) { found=await this.connector.probe(this.address,this.username,this.password); }
      else { found=await this.connector.find(this.address,this.username,this.password,(value:string)=>{if(generation===this.generation){this.message=value;}}); }
      if (generation!==this.generation) { return; }
      this.address=found.address;this.source=found.address;this.settingsOpen=false;
      if(this.smooth){this.startFeed(generation);}else{this.refreshFrame(generation);}
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
        Column({space:10}) {
          if(this.frame){Image(this.frame).objectFit(ImageFit.Contain).width('100%').layoutWeight(1)}
          else{Blank();Text('正在加载实时画面…').fontColor(CareTheme.secondary);Blank();}
          Text(this.recording?'● 录像中 · 最长 60 秒':this.smooth?'连续视频 · 中断自动重连':'省流取图 · 中断自动重试').fontSize(12).fontColor(CareTheme.secondary).margin(12)
        }.layoutWeight(1).width('100%').backgroundColor(Color.White).borderRadius(16)
      } else {
        Column({space:18}) {
          Blank()
          Image($r('app.media.camera')).width(60).height(60)
          Text(this.busy?'正在寻找家中摄像头':'画面，由你开启').fontSize(20).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink)
          Text('摄像头与手机连接同一热点后，点击下方按钮即可连接。').fontSize(14).fontColor(CareTheme.secondary).textAlign(TextAlign.Center).lineHeight(23).padding({left:20,right:20})
          Blank()
        }.layoutWeight(1).width('100%').backgroundColor(CareTheme.softBlue).borderRadius(16)
      }
      if(this.source){
        Row({space:8}){
          Button('截图').enabled(!!this.frame&&!this.saving).layoutWeight(1).fontSize(13).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).onClick(()=>{this.saveMedia(false);})
          Button(this.recording?'结束录像':'录像').enabled(!!this.frame).layoutWeight(1).fontSize(13).backgroundColor(this.recording?CareTheme.alarm:CareTheme.softBlue).fontColor(this.recording?Color.White:CareTheme.primary).onClick(()=>{this.toggleRecording();})
          Button(this.smooth?'流畅模式':'省流模式').enabled(!this.recording).layoutWeight(1).fontSize(13).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).onClick(()=>{this.changeMode();})
        }.width('100%')
      }
      if(this.clipReady){Button('保存录像（AVI）').enabled(!this.saving).fontSize(13).onClick(()=>{this.saveMedia(true);})}
      if(this.mediaMessage){Text(this.mediaMessage).fontSize(12).fontColor(CareTheme.secondary)}
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
