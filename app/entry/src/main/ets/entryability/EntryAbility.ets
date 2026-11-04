import { UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { NotificationRoute } from '../services/AlarmNotifications';
export default class EntryAbility extends UIAbility {
  private route(want:Want):void {
    const device=want.parameters?.alarm_device,key=want.parameters?.alarm_key;
    if(typeof device==='string' && typeof key==='string') {NotificationRoute.device=device;NotificationRoute.key=key;}
  }
  onCreate(want:Want):void {this.route(want);}
  onNewWant(want:Want):void {this.route(want);}
  onWindowStageCreate(stage:window.WindowStage):void {stage.loadContent('pages/Index');}
}
