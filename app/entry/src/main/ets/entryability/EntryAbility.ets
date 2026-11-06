import { DeviceLink } from '../services/DeviceLink';
import { UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { NotificationRoute } from '../services/AlarmNotifications';
export default class EntryAbility extends UIAbility {
  private stage:window.WindowStage|null=null;
  private route(want:Want):boolean {
    const link=typeof want.uri==='string'&&DeviceLink.capture(want.uri);
    const device=want.parameters?.alarm_device,key=want.parameters?.alarm_key;
    if(typeof device==='string'&&typeof key==='string'){NotificationRoute.device=device;NotificationRoute.key=key;}
    return link;
  }
  onCreate(want:Want):void{this.route(want);}
  onNewWant(want:Want):void{
    if(this.route(want)&&this.stage){this.stage.loadContent('pages/Index').catch(()=>{});}
  }
  onWindowStageCreate(stage:window.WindowStage):void{this.stage=stage;stage.loadContent('pages/Index');}
  onWindowStageDestroy():void{this.stage=null;}
}
