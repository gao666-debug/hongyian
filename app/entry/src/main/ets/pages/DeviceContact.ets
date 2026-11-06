import { router, promptAction } from '@kit.ArkUI';
import { http } from '@kit.NetworkKit';
import { DeviceSession } from '../services/DeviceSession';
import { CareTheme } from '../theme/CareTheme';
import { Command } from '../models/Models';
import { DeviceContactItem, DeviceContactFeed, mergeDeviceContacts, contactText } from '../models/DeviceContact';

@Entry
@Component
struct DeviceContact {
  @State items: DeviceContactItem[] = [];
  @State status: string = '';
  @State busy: boolean = false;
  @State sending: boolean = false;
  @State permitted: boolean = false;
  private active: boolean = false;
  private token: string = '';
  private device: string = '';
  private cursor: number = 0;
  private timer: number = -1;
  private retries: Map<string,string> = new Map<string,string>();
  aboutToAppear(): void {
    this.active=true;this.token=DeviceSession.backend.token;this.device=DeviceSession.backend.device;
    this.permitted=(DeviceSession.backend.family.permissions||[]).includes('control');
    if(!this.token||!this.device){this.status='请先登录并加入有守护主机的家庭';return;}
    this.refresh();this.timer=setInterval(()=>{this.refresh();},4000);
  }
  aboutToDisappear(): void {this.active=false;if(this.timer!==-1){clearInterval(this.timer);this.timer=-1;}}
  private current(): boolean {
    if(this.token!==DeviceSession.backend.token||this.device!==DeviceSession.backend.device){this.items=[];this.status='账号或家庭已变化，请返回重新进入';return false;}
    return this.active;
  }
  private async refresh(): Promise<void> {
    if(!this.current()||this.busy||!this.token||!this.device){return;}
    this.busy=true;
    try{
      const r=await DeviceSession.backend.request('/api/v1/devices/'+encodeURIComponent(this.device)+'/messages?after='+this.cursor+'&limit=100',http.RequestMethod.GET,'',8000);
      if(!this.current()){return;}
      const feed=r.data as DeviceContactFeed;
      this.items=mergeDeviceContacts(this.items,feed.items||[]);this.cursor=feed.next_cursor;
      this.status=feed.has_more?'正在补拉离线期间的消息，稍后继续同步':this.items.length?'消息已同步，确认状态以主板回报为准':'暂无主板联系消息';
    }catch(error){if(this.current()){this.status='消息暂未同步：'+(error as Error).message+'。请确认服务器已部署设备消息接口。';}}
    finally{this.busy=false;}
  }
  private async send(item: DeviceContactItem,code: number): Promise<void> {
    if(this.sending||!this.permitted||!this.current()){return;}
    let accepted=false;try{const decision=await promptAction.showDialog({title:code?'回复主板消息':'标记已读',message:code?'确认发送这条回复？主板收到后还需本人按键确认看到。':'发送已读状态到主板；不会确认或解除设备报警。',buttons:[{text:'取消',color:CareTheme.secondary},{text:'确认',color:CareTheme.primary}]});
    accepted=decision.index===1;}catch(_){return;}
    if(!accepted||!this.current()){return;}
    this.sending=true;
    const key=item.boot_id+':'+item.message_id+':'+code;
    if(!this.retries.has(key)){this.retries.set(key,'contact-'+Date.now()+'-'+Math.random().toString(36).slice(2));}
    const body=code?JSON.stringify({code:code,client_request_id:this.retries.get(key)}):JSON.stringify({client_request_id:this.retries.get(key)});
    try{
      const response=await DeviceSession.backend.request('/api/v1/devices/'+encodeURIComponent(this.device)+'/messages/'+item.boot_id+'/'+item.message_id+(code?'/reply':'/seen'),http.RequestMethod.POST,body,10000);
      if(!this.current()){return;}
      let command=response.data as Command;
      this.status='已提交，等待主板执行回执…';
      for(let i=0;i<8&&command.state==='pending';i++){
        await new Promise<void>((resolve)=>{setTimeout(resolve,750);});
        if(!this.current()){return;}
        const result=await DeviceSession.backend.request('/api/v1/commands/'+encodeURIComponent(command.command_id),http.RequestMethod.GET,'',8000);command=result.data as Command;
      }
      if(!this.current()){return;}
      await this.refresh();
      this.status=command.state==='executed'?'主板已执行；家人回复是否被看到，请查看消息上的“板端已确认”。':command.state==='failed'?'主板执行失败，请确认固件、启动编号与原消息仍有效':'尚未取得明确执行结果，请核对后重试；重试沿用原请求编号。';
    }catch(error){if(this.current()){this.status='发送未确认：'+(error as Error).message;}}
    finally{this.sending=false;}
  }
  private timestamp(item: DeviceContactItem): string {return item.epoch?new Date(item.epoch*1000).toLocaleString():'设备时间未同步 · 云端收到于 '+new Date(item.received_at*1000).toLocaleString();}
  build(){
    Column(){
      Row({space:12}){Button('返回').fontColor(CareTheme.primary).backgroundColor(Color.Transparent).height(48).onClick(()=>{router.back();});Text('主板联系消息').fontSize(24).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).layoutWeight(1)}.width('100%').padding(16)
      Scroll(){Column({space:18}){
        Text('这里是主板与家人的联系消息。紧急报警请到报警页处理；成员聊天请到“家人消息”。').fontSize(15).fontColor(CareTheme.secondary).width('100%')
        Text(this.status).fontSize(15).fontColor(CareTheme.warmInk).width('100%')
        Button(this.busy?'正在同步':'刷新消息').enabled(!this.busy).height(48).width('100%').borderRadius(12).backgroundColor(CareTheme.primary).onClick(()=>{this.refresh();})
        ForEach(this.items.slice().reverse(),(item:DeviceContactItem)=>{
          Column({space:12}){
            Text(item.direction==='board'?'主板发来':'家人回复').fontSize(14).fontColor(CareTheme.secondary).width('100%')
            Text(contactText(item)).fontSize(21).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink).width('100%')
            Text(this.timestamp(item)).fontSize(13).fontColor(CareTheme.secondary).width('100%')
            Text(item.seen?(item.direction==='board'?'家人已读':'板端已确认看到'):'等待确认').fontSize(14).fontColor(item.seen?CareTheme.primary:CareTheme.warmInk).width('100%')
            if(item.direction==='board'&&this.permitted){
              Button('标记已读').enabled(!this.sending&&!item.seen).width('100%').height(48).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).borderRadius(12).onClick(()=>{this.send(item,0);})
              ForEach([1,2,3],(code:number)=>{Button(code===1?'已收到消息':code===2?'正在赶来':'已联系人员协助').enabled(!this.sending).width('100%').height(48).borderRadius(12).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).onClick(()=>{this.send(item,code);})})
            }
          }.width('100%').padding(18).backgroundColor(CareTheme.surface).borderRadius(16)
        },(item:DeviceContactItem)=>item.boot_id+':'+item.message_id+':'+item.revision)
        Text('显示最近 200 条已同步消息；同一消息的已读状态会更新，离线期间记录从服务器补拉。').fontSize(13).fontColor(CareTheme.secondary).width('100%')
      }.padding(20).width('100%')}.layoutWeight(1).width('100%')
    }.width('100%').height('100%').backgroundColor(CareTheme.background).expandSafeArea([SafeAreaType.SYSTEM],[SafeAreaEdge.BOTTOM])
  }
}
