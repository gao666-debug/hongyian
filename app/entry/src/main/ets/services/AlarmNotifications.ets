import notificationManager from '@ohos.notificationManager';
import { common, wantAgent } from '@kit.AbilityKit';
import { AlarmEvent, alarmKey } from '../models/Models';

// Notification taps retain only a routing key, never credentials or health records.
export class NotificationRoute {
  static device:string='';
  static key:string='';
}
export class AlarmNotifications {
  static enabled:boolean=false;
  static async inspect():Promise<boolean> {
    try {AlarmNotifications.enabled=await notificationManager.isNotificationEnabled();}catch(error){AlarmNotifications.enabled=false;}
    return AlarmNotifications.enabled;
  }
  static async enable(context:common.UIAbilityContext):Promise<boolean> {
    try {await notificationManager.requestEnableNotification(context);}catch(error){}
    return AlarmNotifications.inspect();
  }
  static async publish(device:string,event:AlarmEvent):Promise<void> {
    if(!AlarmNotifications.enabled || event.phase!=='start' || event.handling && event.handling.resolution!=='follow_up'){return;}
    try {
      const key=alarmKey(event);
      let id=0;const identity=device+':'+key;
      for(let i=0;i<identity.length;i++){id=(id*31+identity.charCodeAt(i))|0;}
      const agent=await wantAgent.getWantAgent({
        wants:[{bundleName:'com.zhian.eldercare',abilityName:'EntryAbility',parameters:{alarm_device:device,alarm_key:key}}],
        actionType:wantAgent.OperationType.START_ABILITY,requestCode:id&0x7fffffff,
        wantAgentFlags:[wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      });
      await notificationManager.publish({id:id&0x7fffffff,notificationSlotType:notificationManager.SlotType.SOCIAL_COMMUNICATION,
        content:{notificationContentType:notificationManager.ContentType.NOTIFICATION_CONTENT_BASIC_TEXT,
          normal:{title:'颐鸿安 · 安全提醒',text:'家中有新的异常记录，请及时查看并联系核实。'}},
        wantAgent:agent,tapDismissed:true});
    } catch(error) { /* System permission failure must not prevent the in-app alarm. */ }
  }
}
