import { router, promptAction } from '@kit.ArkUI';
import { http } from '@kit.NetworkKit';
import { DeviceSession } from '../services/DeviceSession';
import { CareTheme } from '../theme/CareTheme';
import { Command } from '../models/Models';
import { CareSnapshot, CareActual, CarePolicyRequest, CarePauseRequest, careFresh, carePolicyMatches, carePauseMatches, careMinute, careTime } from '../models/CarePolicy';
@Entry
@Component
struct CareSettings {
  @State snapshot:CareSnapshot|null=null;
  @State status:string='正在读取主板实际设置…';
  @State careEnabled:boolean=true;
  @State sleeping:boolean=false;
  @State start:string='22:00';
  @State end:string='07:00';
  @State recordOnly:boolean=false;
  @State seconds:string='1800';
  @State loading:boolean=false;
  @State sending:boolean=false;
  @State pending:boolean=false;
  @State dirty:boolean=false;
  @State permitted:boolean=false;
  private token:string='';private device:string='';private active:boolean=false;private timer:number=-1;
  private draftRevision:number=0;private draftBoot:number=0;
  private policy:CarePolicyRequest|null=null;private pause:CarePauseRequest|null=null;
  private pendingPath:string='';private pendingBody:string='';
  aboutToAppear():void{this.active=true;this.token=DeviceSession.backend.token;this.device=DeviceSession.backend.device;this.permitted=(DeviceSession.backend.family.permissions||[]).includes('control');this.load();this.timer=setInterval(()=>{if(!this.sending){this.load();}},4000);}
  aboutToDisappear():void{this.active=false;if(this.timer!==-1){clearInterval(this.timer);this.timer=-1;}}
  private current():boolean{if(this.token!==DeviceSession.backend.token||this.device!==DeviceSession.backend.device){this.snapshot=null;this.status='账号或家庭已改变，请返回重新进入';return false;}return this.active;}
  private path(suffix:string):string{return '/api/v1/devices/'+encodeURIComponent(this.device)+'/'+suffix;}
  private populate(value:CareSnapshot):void{if(!value.actual||value.boot_id===null){return;}const a=value.actual;this.careEnabled=a.enabled;this.sleeping=a.sleep_enabled;this.start=careTime(a.start);this.end=careTime(a.end);this.recordOnly=a.quiet===1;this.seconds=String(a.noact_ms/1000);this.draftRevision=a.revision;this.draftBoot=value.boot_id;this.dirty=false;}
  private async read():Promise<CareSnapshot>{const r=await DeviceSession.backend.request(this.path('care-policy'),http.RequestMethod.GET,'',8000);if(!this.current()){throw new Error('账号已改变');}return r.data as CareSnapshot;}
  private async load(reset:boolean=false):Promise<void>{
    if(!this.current()||!this.token||!this.device||this.loading||this.sending){return;}this.loading=true;
    try{const value=await this.read();this.snapshot=value;if(this.pending&&this.matched(value)){this.clearPending();this.populate(value);this.status='主板实际设置已与本次请求一致。';return;}if(reset||(!this.dirty&&!this.pending)){this.populate(value);}if(!this.pending){this.status=!value.supported?'当前固件不支持远程无活动设置，请升级主板':!careFresh(value)?'设置待同步或报告已过期，等待主板重新上报':this.dirty?'有未保存修改，自动刷新不会覆盖输入':'已读取主板实际设置';}}
    catch(error){if(this.current()){this.snapshot=null;this.status='设置读取失败：'+(error as Error).message+'。请确认服务器已部署新接口。';}}finally{this.loading=false;}
  }
  private writable():boolean{return this.permitted&&careFresh(this.snapshot)&&!this.loading&&!this.sending&&!this.pending;}
  private async confirm(message:string):Promise<boolean>{try{const r=await promptAction.showDialog({title:'无活动守护设置',message,buttons:[{text:'取消',color:CareTheme.secondary},{text:'确认',color:CareTheme.primary}]});return r.index===1&&this.current();}catch(_){return false;}}
  private async reload():Promise<void>{if(this.pending){this.status='请先重试或放弃上次未确认请求';return;}if(this.dirty&&!await this.confirm('重新读取会放弃当前未保存的输入，确定继续？')){return;}await this.load(true);}
  private async save():Promise<void>{
    if(!this.writable()||!this.snapshot||this.snapshot.boot_id!==this.draftBoot){this.status='主板设置或启动状态已变化，请重新读取';return;}
    const start=careMinute(this.start),end=careMinute(this.end),seconds=Number(this.seconds);
    if(start<0||end<0||(this.sleeping&&start===end)||!Number.isInteger(seconds)||seconds<10||seconds>86400){this.status='请填写有效时间和10～86400秒的阈值；睡眠起止不能相同';return;}
    if(!await this.confirm('保存将改变无活动提醒策略，不关闭求助、燃气、跌倒或温湿度异常报警。确定继续？')){return;}
    this.policy={boot_id:this.draftBoot,enabled:this.careEnabled,sleep_enabled:this.sleeping,start,end,quiet:this.recordOnly?1:0,noact_ms:seconds*1000,expected_revision:this.draftRevision,client_request_id:'care-'+Date.now()+'-'+Math.random().toString(36).slice(2)};this.pause=null;this.pendingPath=this.path('care-policy');this.pendingBody=JSON.stringify(this.policy);this.pending=true;await this.run();
  }
  private async pauseFor(minutes:number):Promise<void>{
    if(!this.writable()||!this.snapshot||!this.snapshot.actual||this.snapshot.boot_id===null){return;}
    if(!await this.confirm(minutes?'临时暂停无活动提醒 '+minutes+' 分钟，到期后自动恢复当前策略。其他报警继续运行。':'结束临时暂停，恢复当前无活动策略？')){return;}
    this.pause={boot_id:this.snapshot.boot_id,minutes,expected_pause_revision:this.snapshot.actual.pause_revision,client_request_id:'pause-'+Date.now()+'-'+Math.random().toString(36).slice(2)};this.policy=null;this.pendingPath=this.path('care-pause');this.pendingBody=JSON.stringify(this.pause);this.pending=true;await this.run();
  }
  private matched(value:CareSnapshot):boolean{return this.policy?carePolicyMatches(value,this.policy):this.pause?carePauseMatches(value,this.pause):false;}
  private clearPending():void{this.pending=false;this.pendingPath='';this.pendingBody='';this.policy=null;this.pause=null;}
  private async run():Promise<void>{
    if(!this.pending||this.sending||this.loading||!this.current()){return;}this.sending=true;this.status='已提交，正在等待主板执行与实际配置上报…';
    try{const response=await DeviceSession.backend.request(this.pendingPath,http.RequestMethod.POST,this.pendingBody,10000);if(!this.current()){return;}let command=response.data as Command;
      for(let i=0;i<12&&command.state==='pending';i++){await new Promise<void>((resolve)=>{setTimeout(resolve,750);});if(!this.current()){return;}const result=await DeviceSession.backend.request('/api/v1/commands/'+encodeURIComponent(command.command_id),http.RequestMethod.GET,'',8000);command=result.data as Command;}
      if(!this.current()){return;}if(command.state==='failed'){this.clearPending();throw new Error(command.result_code===4?'配置版本或启动编号已改变，请重新读取':command.result_code===2?'主板保存失败，原设置未确认改变':'主板拒绝了这次操作');}
      if(command.state!=='executed'){throw new Error('暂未取得明确执行结果，保留原请求，可重试核对');}
      for(let i=0;i<12;i++){const value=await this.read();this.snapshot=value;if(this.matched(value)){this.clearPending();this.populate(value);this.status='主板已执行，实际设置已核对。';return;}await new Promise<void>((resolve)=>{setTimeout(resolve,750);});if(!this.current()){return;}}
      throw new Error('已收到执行回执，实际设置仍待同步；重试不会重复执行原请求');
    }catch(error){if(this.current()){this.status='操作未确认：'+(error as Error).message;}}finally{this.sending=false;}
  }
  private async abandon():Promise<void>{if(this.sending||!await this.confirm('放弃重试不会撤销可能已经生效的设置，随后重新读取实际状态。确定继续？')){return;}this.clearPending();this.dirty=false;await this.load(true);}
  private pauseLabel():string{const value=this.snapshot;if(!careFresh(value)||!value||!value.actual){return value&&value.actual?'最近设置记录，等待更新':'状态待同步';}const remaining=Math.max(0,value.actual.pause_seconds-Math.floor(value.age_seconds||0));return remaining>0?'临时暂停 · 约'+Math.ceil(remaining/60)+'分钟后恢复':value.actual.mode===0?'按当前策略暂停':value.actual.mode===2?'睡眠时段仅记录':'按当前策略守护';}
  build(){Column(){
    Row(){Button('返回').height(48).backgroundColor(Color.Transparent).fontColor(CareTheme.primary).onClick(()=>{router.back();});Text('无活动远程设置').fontSize(23).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).layoutWeight(1)}.width('100%').padding(12)
    Scroll(){Column({space:18}){
      Text(this.status).fontSize(15).fontColor(CareTheme.warmInk).width('100%');Text(this.pauseLabel()).fontSize(18).fontColor(CareTheme.ink).width('100%')
      if(this.snapshot&&this.snapshot.actual&&this.snapshot.actual.sleep_enabled&&!this.snapshot.actual.time_synced){Text('主板时间待同步，睡眠时段尚未生效。').fontSize(14).fontColor(CareTheme.warmInk).width('100%')}
      Button('重新读取主板设置').enabled(!this.loading&&!this.sending&&!this.pending).height(48).width('100%').borderRadius(12).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).onClick(()=>{this.reload();})
      Column({space:16}){
        Row(){Text('启用无活动提醒').fontSize(17).layoutWeight(1);Toggle({type:ToggleType.Switch,isOn:this.careEnabled}).enabled(this.writable()).selectedColor(CareTheme.primary).onChange((v:boolean)=>{this.careEnabled=v;this.dirty=true;})}.width('100%')
        Text('无活动阈值（秒，10～86400）').fontSize(15).width('100%');TextInput({text:this.seconds,placeholder:'例如1800代表30分钟'}).type(InputType.Number).enabled(this.writable()).height(52).onChange((v:string)=>{this.seconds=v;this.dirty=true;})
        Row(){Text('启用睡眠时段').fontSize(17).layoutWeight(1);Toggle({type:ToggleType.Switch,isOn:this.sleeping}).enabled(this.writable()).selectedColor(CareTheme.primary).onChange((v:boolean)=>{this.sleeping=v;this.dirty=true;})}.width('100%')
        Text('北京时间，可跨午夜').fontSize(13).fontColor(CareTheme.secondary).width('100%')
        TextInput({text:this.start,placeholder:'开始 HH:mm，例如22:00'}).enabled(this.writable()).height(52).onChange((v:string)=>{this.start=v;this.dirty=true;})
        TextInput({text:this.end,placeholder:'结束 HH:mm，例如07:00'}).enabled(this.writable()).height(52).onChange((v:string)=>{this.end=v;this.dirty=true;})
        Row(){Text('睡眠期间只记录，无声光提醒').fontSize(15).layoutWeight(1);Toggle({type:ToggleType.Switch,isOn:this.recordOnly}).enabled(this.writable()).selectedColor(CareTheme.primary).onChange((v:boolean)=>{this.recordOnly=v;this.dirty=true;})}.width('100%')
        Text('关闭上项则睡眠期间暂停无活动检测。恢复正常守护时重新计时；时间未同步时睡眠时段不会自动生效。').fontSize(13).fontColor(CareTheme.secondary).width('100%')
        Button('保存并核对主板回执').enabled(this.writable()).height(48).width('100%').borderRadius(12).backgroundColor(CareTheme.primary).onClick(()=>{this.save();})
      }.width('100%').padding(18).backgroundColor(CareTheme.surface).borderRadius(16)
      Column({space:12}){Text('临时暂停').fontSize(19).fontWeight(FontWeight.Bold).width('100%');ForEach([30,60,0],(minutes:number)=>{Button(minutes?'暂停'+minutes+'分钟':'结束临时暂停').enabled(this.writable()).height(48).width('100%').borderRadius(12).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(()=>{this.pauseFor(minutes);})})}.width('100%').padding(18).backgroundColor(CareTheme.surface).borderRadius(16)
      if(this.pending){Button('重试上次请求').enabled(!this.sending).height(48).width('100%').borderRadius(12).backgroundColor(CareTheme.primary).onClick(()=>{this.run();});Button('放弃重试，核对实际状态').enabled(!this.sending).height(48).width('100%').fontColor(CareTheme.warmInk).backgroundColor(CareTheme.warmSurface).borderRadius(12).onClick(()=>{this.abandon();})}
      Text('这些设置仅影响无活动提醒。求助、跌倒、燃气及温湿度异常仍保持各自报警逻辑。只读账号可查看，修改需要设备控制权限。').fontSize(13).fontColor(CareTheme.secondary).width('100%')
    }.padding(20).width('100%')}.layoutWeight(1).width('100%')
  }.width('100%').height('100%').backgroundColor(CareTheme.background)}
}
