import { DeviceLink } from '../services/DeviceLink';
import { http } from '@kit.NetworkKit';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { bundleManager } from '@kit.AbilityKit';
import { abilityAccessCtrl } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import { promptAction } from '@kit.ArkUI';
import { CareTheme } from '../theme/CareTheme';
import { roleAllows, roleLabel, roleNote, collaborationError } from '../models/Permissions';
import { AlarmNotifications, NotificationRoute } from '../services/AlarmNotifications';
import { Backend } from '../services/Backend';
import { watchVital, heartText, DistributedNode, DistributedFamily, HouseholdMember, readingFresh, sensorText, motionText, roomDoorText, EnvironmentProperties } from '../models/Distributed';
import { DeviceSession } from '../services/DeviceSession';
import { Accounts, SavedAccount } from '../services/Accounts';
import { Voice } from '../services/Voice';
import { VoiceCommand, VoiceIntent } from '../services/VoiceCommand';
import { VoiceListen } from '../services/VoiceListen';
import { medicineLabel, nightLabel, medicineTime, validMedicineTime, medicineSlots, slotLabel, pendingMedicine, demoLivingUpdate, livingCommandMatches } from '../models/Living';
import { AlarmSnapshot, alarmStateLabel, alarmIsActive, noteText, resolutionLabel } from '../models/AlarmState';
import { doorLabel } from '../models/DoorState';
import { AlarmInbox, mergeAlarms } from '../models/AlarmInbox';
import { assessCare, CareAssessment } from '../models/CareAssessment';
import { ActivityReport, PeriodReport, AppSOSResult, Shadow, Health, Properties, AlarmEvent, AlarmList, Statistics, DayStats, Command, CommandRequest, LivingStatus, MedicineSlot, MedicineDraft, MedicineHistory, MedicineRecord, CommandParameters, NoteList, NoteItem, Push, alarmTitle, alarmKey, alarmTime } from '../models/Models';

const INK: string = CareTheme.ink;
const SUB: string = CareTheme.secondary;
const PRIMARY: string = CareTheme.primary;
const BG: string = CareTheme.background;
const RED: string = CareTheme.alarm;
const AMBER: string = CareTheme.warmInk;
const DEVICE: string = '6acdec5b18855b39c5552ec3_TXSMART001';

interface DoorRecord { state:string; previous:string; observed_at:number; }
interface DoorSummary { hint:string; level:string; door?:string; }
interface DoorRecords { summary:DoorSummary; items:DoorRecord[]; }
function pad2(value:number):string { return value < 10 ? '0' + value : '' + value; }
interface EntryView { icon: Resource; title: string; value: string; action: string; }
interface ReadingView { label: string; value: string; unit: string; icon: Resource; }
interface MetricView { icon: Resource; label: string; value: string; caption: string; tint: string; }
interface PillView { text: string; color: string; background: string; }
interface SectionView { title: string; subtitle: string; }
interface SummaryView { label: string; value: string; }
interface InfoView { label: string; value: string; color: string; }
interface UnreadTotal { unread: number; }
interface AlarmRemind { event_key?: string; username?: string; created?: number; }

@Entry
@Component
struct Index {
  @State familyNodes: DistributedNode[] = [];
  @State familyRole: string = '';
  @State members: HouseholdMember[] = [];
  @State memberName: string = '';
  @State memberPassword: string = '';
  @State memberRole: string = 'viewer';
  @State memberBusy: boolean = false;
  @State memberMessage: string = '';
  @State memberCreated: string = '';
  private readonly memberRoles: string[] = ['viewer','caregiver','community','property'];
  @State claimBusy: boolean = false;
  @State remindBusy: boolean = false;
  @State remindMessage: string = '';
  @State unreadTotal: number = 0;
  private lastUnread: number = -1;
  private unreadPolling: boolean = false;
  private noticeOpen: boolean = false;
  private noteRequest: string = '';
  private noteRequestBody: string = '';
  private allowed(action:string): boolean { return this.demo || roleAllows(this.familyRole,action); }
  private expiredSession(generation:number): void {
    if(generation!==this.generation||this.api.token||!this.loggedIn){return;}
    this.generation++;this.loggedIn=false;this.demo=false;this.simulated=false;this.online=false;this.pushOnline=false;
    this.polling=false;this.detailsPolling=false;this.healthPolling=false;this.familyPolling=false;this.loading=false;
    this.familyNodes=[];this.members=[];this.familyRole='';this.alarms=[];this.alertQueue=[];this.notes=[];this.selected=null;
    this.living=null;this.livingOpen=false;this.medicineNotice=false;this.hours=[];this.days=[];this.activityReport=null;
    this.temp=-999;this.humi=-999;this.gas=-999;this.light=-999;this.body=-1;this.inactive=-1;this.status=-1;
    this.received=0;this.deviceTime=0;this.healthKnown=false;this.sessionPassword='';this.api.stopPush();Voice.stop();
    this.error='登录已失效或权限已调整，请重新登录';this.passwordDraft='';this.settingsOpen=true;
  }
  @State accounts: SavedAccount[] = [];
  @State appVersion: string = '';
  @State switchBusy: boolean = false;
  @State switchMessage: string = '';
  @State switchUser: string = '';
  @State switchPassword: string = '';
  @State switchServer: string = '';
  @State switchOpen: boolean = false;
  private async addMember(): Promise<void> {
    if (this.memberBusy || !this.allowed('members.manage')) { return; }
    this.memberName=this.memberName.trim();
    if(!/^[A-Za-z0-9_-]{3,32}$/.test(this.memberName)){this.memberMessage='账号需为3～32位字母、数字、下划线或短横线';return;}
    if(this.memberPassword.length<8||this.memberPassword.length>128){this.memberMessage='家庭成员密码需为8～128位，你当前输入了'+this.memberPassword.length+'位';return;}
    this.memberBusy=true;this.memberMessage='';
    const created=this.memberName;
    try {
      await this.api.request('/api/v1/household/members',http.RequestMethod.POST,JSON.stringify({username:created,password:this.memberPassword,role:this.memberRole}));
      this.memberCreated=created;
      this.memberName='';this.memberPassword='';
      this.memberMessage='已创建「'+created+'」';
      await this.refreshFamily();
    } catch (error) { this.memberMessage=(error as Error).message; }
    finally { this.memberBusy=false; }
  }

  private memberRoleLabel(role:string): string {
    return roleLabel(role);
  }
  private memberRoleNote(role:string): string {
    return roleNote(role);
  }
  private async manageMember(member:HouseholdMember, remove:boolean): Promise<void> {
    if(this.memberBusy || this.familyRole!=='owner' || member.role==='owner'){return;}
    const generation=this.generation;
    let role=member.role;
    try {
      if(remove){
        const answer=await promptAction.showDialog({title:'删除成员账号',message:'确认删除「'+member.username+'」？该账号将无法登录，所有登录状态失效；以前的处置记录会保留。此操作不可撤销。',buttons:[{text:'取消',color:SUB},{text:'删除账号',color:RED}]});
        if(answer.index!==1){return;}
      }else{
        const answer=await promptAction.showActionMenu({title:'设置 '+member.username+' 的权限',buttons:[{text:'只读成员',color:PRIMARY},{text:'家属／监护人',color:PRIMARY},{text:'社区网格员',color:PRIMARY},{text:'物业值班人员',color:PRIMARY}]});
        role=this.memberRoles[answer.index];if(!role||role===member.role){return;}
      }
      if(generation!==this.generation){return;}
      this.memberBusy=true;
      await this.api.request('/api/v1/household/members/'+encodeURIComponent(member.username),remove?http.RequestMethod.DELETE:http.RequestMethod.PATCH,remove?'':JSON.stringify({role:role}));
      if(generation!==this.generation){return;}
      this.accounts=await Accounts.remove(getContext(this) as common.UIAbilityContext,this.api.base,member.username);
      this.memberMessage=remove?'已删除账号「'+member.username+'」，历史记录保留':'权限已更新，该成员需要重新登录';
      await this.refreshFamily();
    }catch(error){if(generation===this.generation){this.memberMessage='操作未完成：'+(error as Error).message;}}
    finally{this.memberBusy=false;}
  }
  // A member row doubles as the switch entry: the same person never appears twice.
  private accountOf(username:string): SavedAccount | null {
    const base=this.api.base;
    for(const item of this.accounts){
      if(item.username===username&&item.server===base){return item;}
    }
    return null;
  }
  // 切换到其他成员始终要求该账号密码，不复用其他账号会话。
  private async switchMember(username:string): Promise<void> {
    this.switchMessage='';
    this.switchUser=username;
    this.switchServer=this.api.base;
    this.switchPassword='';
  }
  private localExtras(): SavedAccount[] {
    const base=this.api.base;
    return this.accounts.filter((item:SavedAccount):boolean=>
      item.server===base&&item.username!==this.accountName&&!this.members.some((member:HouseholdMember):boolean=>member.username===item.username));
  }
  // The active account is not always present in the member list right after login.
  private knownRole(username:string): string {
    if(username===this.accountName&&this.familyRole){return this.familyRole;}
    const member=this.members.find((item:HouseholdMember):boolean=>item.username===username);
    if(member){return member.role;}
    const saved=this.accounts.find((item:SavedAccount):boolean=>item.username===username&&item.server===this.api.base);
    return saved?saved.role:'';
  }
  private accountLine(): string {
    if (!this.loggedIn) { return '体验用户 · 演示模式'; }
    const role = this.familyRole ? ' · ' + this.memberRoleLabel(this.familyRole) : '';
    return this.accountName + role;
  }
  private accountStatusText(): string {
    const parts:string[]=[];
    parts.push(this.pushOnline?'云端已连接':'前台轮询');
    parts.push(this.familyNodes.length+' 台设备');
    if(this.received>0){
      const age=Math.max(0,Math.floor((Date.now()-this.received)/1000));
      parts.push(age<60?age+' 秒前同步':Math.floor(age/60)+' 分钟前同步');
    }
    return parts.join(' · ');
  }

  @State familyMessage: string = '登录后查看家庭设备';
  private async refreshFamily(): Promise<void> {
    if (this.familyPolling) { return; }
    const generation = this.generation;
    this.familyPolling = true;
    try {
      const family = await this.api.refreshHousehold();
      if (generation !== this.generation || !this.loggedIn) { return; }
      this.familyNodes = family.devices || [];
      this.familyRole = family.role || '';
      this.members = family.members || [];
      this.familyMessage = this.familyNodes.length ? '同一家庭，分布守护' : '暂未绑定设备';
    } catch (error) {
      if (generation === this.generation) { this.familyMessage = '房间与随身设备可从下方入口查看';this.expiredSession(generation); }
    } finally { if (generation === this.generation) { this.familyPolling = false; } }
  }

  @State living: LivingStatus | null = null;
  @State livingOpen: boolean = false;
  @State livingDataFresh: boolean = false;
  @State nightDraft: boolean = true;
  @State medicineDrafts: MedicineDraft[] = [];
  @State medicineHistory: MedicineRecord[] = [];
  @State medicineHistoryMessage: string = '';
  @State medicineNoticeSlot: number = -1;
  @State livingRevision: number = 0;
  @State livingBusy: boolean = false;
  @State livingMessage: string = '';
  @State medicineNotice: boolean = false;
  private medicineSeen: string[] = [];
  @State detail: string = '';
  @State batchActionsOpen: boolean = false;
  @State tab: number = 0;
  @State displayName: string = '守护者';
  @State careName: string = '家人';
  @State contactPhone: string = '';
  @State accountName: string = '';
  @State profileOpen: boolean = false;
  @State nameDraft: string = '';
  @State careDraft: string = '';
  @State contactDraft: string = '';
  @State profileError: string = '';
  @State profileSaving: boolean = false;
  @State elderMode: boolean = false;
  @State elderModeSaving: boolean = false;
  @State elderModeMessage: string = '';
  @State alarmReminders: boolean = true;
  @State voiceOn: boolean = true;
  @State voiceMessage: string = '';
  @State voiceStatus: string = '点一下麦克风，说“小颐开灯”';
  @State voiceHeard: string = '';
  @State voiceReply: string = '';
  @State voiceTyped: string = '';
  @State voiceBusy: boolean = false;
  @State voiceTick: string = '';
  @State alertQueue: AlarmEvent[] = [];
  @State demo: boolean = true;
  @State loggedIn: boolean = false;
  @State simulated: boolean = true;
  @State online: boolean = false;
  @State pushOnline: boolean = false;
  @State loading: boolean = false;
  @State error: string = '';
  @State now: number = Date.now();
  @State received: number = 0;
  @State deviceTime: number = 0;
  @State temp: number = -999;
  @State humi: number = -999;
  @State gas: number = -999;
  @State light: number = -999;
  @State body: number = -1;
  @State inactive: number = -1;
  @State status: number = 0;
  @State envValid: boolean = false;
  @State imuValid: boolean = false;
  @State healthKnown: boolean = false;
  @State healthReceived: number = 0;
  @State muted: boolean = false;
  @State firmware: string = '';
  @State doorRecords:DoorRecords|null=null;
  @State doorRecordMessage:string='';
  @State doorEnabled: number = -1;
  @State doorValid: number = -1;
  @State doorValue: number = -1;
  @State fwTls: boolean = false;
  @State alarms: AlarmEvent[] = [];
  @State alarmFilter: number = 0;
  @State hours: number[] = [];
  @State days: DayStats[] = [];
  @State notificationsEnabled:boolean=false;
  @State activityReport: ActivityReport | null = null;
  @State reportMessage: string = '';
  @State activityTotal: number = 0;
  @State alarmTotal: number = 0;
  @State chartDay: number = 0;
  @State settingsOpen: boolean = false;
  @State serverDraft: string = 'https://47.116.171.114:8443';
  @State userDraft: string = 'guardian';
  @State passwordDraft: string = '';
  @State loginBusy: boolean = false;
  @State selected: AlarmEvent | null = null;
  @State acknowledgedAlarms: string[] = [];
  @State healthBoot: number = -1;
  @State healthUp: number = -1;
  @State noteSaving: boolean = false;
  @State noteMessage: string = '';
  @State noteDraft: string = '';
  @State notes: NoteItem[] = [];
  @State resolution: string = 'handled';
  @State commandBusy: boolean = false;
  @State careActionBusy: boolean = false;
  @State careActionMessage: string = '';
  private sosRequest: string = '';
  @State commandMessage: string = '';
  @State refreshCount: number = 0;
  private api: Backend = DeviceSession.backend;
  private inbox: AlarmInbox = new AlarmInbox();
  private pageScroller: Scroller = new Scroller();
  private sessionPassword: string = ''; // Current login only; never persisted.
  private timer: number = -1;
  private generation: number = 0;
  private visible: boolean = true;
  private polling: boolean = false;
  private detailsPolling: boolean = false;
  private healthPolling: boolean = false;
  private familyPolling: boolean = false;
  private ticks: number = 0;
  private healthSignature: string = '';
  private noteOperation: number = 0;
  private demoNotes: Map<string, NoteItem[]> = new Map<string, NoteItem[]>();

  aboutToAppear(): void { this.readVersion(); this.initialize(); }
  // Read the installed version so the about row can never drift from app.json5 again.
  private readVersion(): void {
    try {
      this.appVersion = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT).versionName;
    } catch (error) { this.appVersion = ''; }
  }
  onPageShow(): void { this.openDeviceLink(); if(this.loggedIn&&!this.demo&&!this.api.token){this.expiredSession(this.generation);} this.visible = true; this.startTimer(); if (this.loggedIn && !this.demo) { this.refresh(true); this.startPush(); } }
  onPageHide(): void { this.visible = false; this.stopTimer(); Voice.stop(); this.api.stopPush(); this.generation++; this.polling = false; this.detailsPolling = false; this.healthPolling = false; this.familyPolling = false; this.loading = false; }
  aboutToDisappear(): void { this.onPageHide(); }
  private async initialize(): Promise<void> {
    this.loadDemo();
    const context = getContext(this) as common.UIAbilityContext;
    await this.api.prepare(context);
    try {
      const store = await preferences.getPreferences(context, 'zhian_settings');
      this.serverDraft = await store.get('server', this.serverDraft) as string;
      this.userDraft = await store.get('username', 'guardian') as string;
      this.elderMode = await store.get('elder_mode', false) as boolean;
      this.voiceOn = await store.get('voice_announce', true) as boolean;
      Voice.setEnabled(this.voiceOn);
    } catch (_) { /* Settings are optional; never persist passwords. */ }
    this.accounts = await Accounts.load(context);
    await this.loadProfile();
    this.notificationsEnabled=await AlarmNotifications.inspect();
    this.startTimer();
    // Resume the last account silently so switching between accounts survives an app restart.
    await this.restoreLastAccount();
    this.sessionReady=true;this.openDeviceLink();
  }
  private async restoreLastAccount(): Promise<void> {
    const usable = this.accounts.filter((item:SavedAccount):boolean=>item.username===this.userDraft&&item.server===this.serverDraft&&Accounts.usable(item,Date.now()))
      .sort((a:SavedAccount,b:SavedAccount):number=>b.lastUsed-a.lastUsed);
    if(!usable.length){return;}
    const entry=usable[0];
    try{
      await this.api.adopt(entry.server,entry.token,entry.expiresAt);
      if(this.loggedIn){return;}
      this.applySessionState(entry.username,'');
      this.serverDraft=entry.server;this.userDraft=entry.username;
      await this.persistSettings();
      await this.refreshFamily();
      await this.refresh(true);this.startPush();
    }catch(error){
      // Expired or revoked while the app was closed; stay in demo and let the user pick again.
      const dropped:SavedAccount={server:entry.server,username:entry.username,role:entry.role,token:'',expiresAt:0,devices:entry.devices,lastUsed:entry.lastUsed};
      this.accounts=await Accounts.save(getContext(this) as common.UIAbilityContext,dropped);
    }
  }
  private startTimer(): void {
    if (this.timer !== -1 || !this.visible) { return; }
    this.timer = setInterval(() => {
      this.now = Date.now(); this.ticks++;this.openDeviceLink();
      if(this.loggedIn&&!this.demo&&NotificationRoute.key) {
        if(NotificationRoute.device!==this.api.device){NotificationRoute.key='';this.error='请用对应家庭的账号查看该报警';}
        else {const event=this.alarms.find((record:AlarmEvent)=>alarmKey(record)===NotificationRoute.key);
          if(event){NotificationRoute.key='';this.navigate(1);this.showAlarm(event);}
          else if(this.ticks%4===0){this.refresh(true);}
        }
      }
      if (this.demo) {
        if (this.ticks % 5 === 0) { this.temp = 26.4 + Math.sin(this.ticks / 12) * 0.4; this.humi = 53 + Math.sin(this.ticks / 15) * 2; this.received = this.now; this.deviceTime = this.now; }
      } else if (this.loggedIn && this.ticks % 2 === 0) { this.refresh(this.ticks % 16 === 0); if (this.ticks % 4 === 0 && this.detail !== 'members') { this.refreshFamily(); } if (this.ticks % 10 === 0) { this.pollUnread(); } }
    }, 1000);
  }
  private stopTimer(): void { if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; } }
  private loadDemo(): void {
    this.activityReport=null;this.reportMessage='';
    this.livingOpen = false; this.medicineNotice = false; this.medicineSeen = []; this.livingDataFresh = true; this.livingMessage='';
    this.living = {schema:2,config_revision:1,night_auto:true,night_on:false,light_valid:true,light_owner:'off',medicine_enabled:false,medicine_minute:480,medicine_state:0,dose_day:Math.floor((Date.now()/1000+28800)/86400),confirmed_epoch:0,time_synced:true,medicine_revision:1,slots:[{slot:0,enabled:false,minute:480,state:0,confirmed_epoch:0},{slot:1,enabled:false,minute:720,state:0,confirmed_epoch:0},{slot:2,enabled:false,minute:1200,state:0,confirmed_epoch:0}]};
    this.demoNotes=new Map<string, NoteItem[]>();
    this.acknowledgedAlarms=[]; this.healthBoot=-1; this.healthUp=-1; this.noteSaving=false; this.noteMessage=''; this.noteOperation++;
    this.sessionPassword = ''; this.passwordDraft = '';
    this.doorRecords=null;this.doorRecordMessage='';
    this.careActionMessage='';this.sosRequest='';
    this.accountName = ''; this.alertQueue = []; this.inbox = new AlarmInbox();
    this.displayName = '守护者'; this.careName = '家人'; this.contactPhone = ''; this.alarmReminders = true;
    this.generation++; this.polling = false; this.detailsPolling = false; this.healthPolling = false; this.familyPolling = false; this.loading = false; this.api.stopPush();
    this.demo = true; this.loggedIn = false; this.simulated = true; this.online = true;
    this.temp = 26.4; this.humi = 53; this.gas = 136; this.light = 248; this.body = 1;
    this.inactive = 12; this.status = 0; this.envValid = true; this.imuValid = true;
    this.healthKnown = true; this.healthReceived = Date.now(); this.muted = false; this.firmware = '演示固件';
    this.fwTls = false; this.doorEnabled = 1; this.doorValid = 1; this.doorValue = 0; this.received = Date.now(); this.deviceTime = this.received;
    this.error = ''; this.commandMessage = ''; this.refreshCount = 0;
    const epoch = Math.floor(Date.now() / 1000);
    this.alarms = [
      { boot_id: 0, event_id: 3, code: 4, phase: 'end', epoch: epoch - 1800, up: 120, duration: 42, value: 60 },
      { boot_id: 0, event_id: 2, code: 1, phase: 'end', epoch: epoch - 7200, up: 100, duration: 18, value: 0 },
      { boot_id: 0, event_id: 1, code: 5, phase: 'end', epoch: epoch - 86400, up: 60, duration: 35, value: 36 }
    ];
    this.hours = [0,0,0,0,0,1,3,7,12,8,6,10,14,9,7,11,15,12,8,6,4,2,1,0];
    this.activityTotal = 146; this.alarmTotal = 3;
    this.days = [{ date: new Date().toLocaleDateString(), activity_total: 146, alarm_total: 3, activity_by_hour: this.hours }];
    this.chartDay = 0;
  }
  private applyProperties(p: Properties): void {
    this.temp = typeof p.temp === 'number' ? p.temp : -999;
    this.humi = typeof p.humi === 'number' ? p.humi : -999;
    this.gas = typeof p.gas === 'number' ? p.gas : -999;
    this.light = typeof p.light === 'number' ? p.light : -999;
    this.body = typeof p.body === 'number' ? p.body : -1;
    this.inactive = typeof p.inactive === 'number' ? p.inactive : -1;
    this.doorValue = p.door === 0 || p.door === 1 ? p.door : -1;
    this.status = typeof p.status === 'number' ? p.status : -1;
  }
  private applyHealth(h: Health): void {
    const signature = JSON.stringify(h);
    if (signature !== this.healthSignature) {
      this.healthReceived = h.epoch && h.epoch > 0 ? h.epoch * 1000 : Date.now();
      this.healthSignature = signature;
    }
    this.livingDataFresh = h.data_stale !== true;
    this.living = h.living && (h.living.schema === 1 || h.living.schema === 2) ? h.living : null;
    this.healthBoot = typeof h.boot_id === 'number' ? h.boot_id : -1;
    this.healthUp = typeof h.up === 'number' ? h.up : -1;
    this.healthKnown = true;
    this.envValid = h.env_available !== undefined ? h.env_available : h.env_valid === 1;
    this.imuValid = h.fall_detection_available === true;
    this.muted = h.mute === 1; this.firmware = h.fw || '未提供'; this.doorEnabled = h.door_enabled === 0 || h.door_enabled === 1 ? h.door_enabled : -1;
    this.doorValid = h.door_valid === 0 || h.door_valid === 1 ? h.door_valid : -1; this.fwTls = h.tls === true;
    if (h.data_stale === true) { this.envValid = false; this.imuValid = false; }
    this.updateMedicineNotice();
    this.pruneAlerts();
  }
  private async refresh(full: boolean = false): Promise<void> {
    if (this.demo) { this.received = Date.now(); return; }
    if (!this.loggedIn || !this.visible) { return; }
    const generation = this.generation;
    if(this.api.device===''){this.familyMessage='尚未加入家庭，请在家庭与账号中接受邀请';return;}
    if(!this.allowed('view')){
      if(!this.familyRole){await this.refreshFamily();}
      if(generation===this.generation){await this.refreshEvents(generation);}
      return;
    }
    // Auxiliary requests must never hold the live-reading lane.
    this.refreshDeviceHealth(generation);
    if (full) { this.refreshDetails(generation); }
    if (this.polling) { return; }
    this.polling = true; this.loading = true;
    try {
      // /household is a cached catalog, not an authoritative live shadow.
      const response = await this.api.request(this.api.path('shadow'), http.RequestMethod.GET, '', 8000);
      if (generation !== this.generation || !this.visible) { return; }
      const shadow = response.data as Shadow;
      this.simulated = response.simulated === true;
      this.applyProperties(shadow.properties || {});
      this.online = shadow.online === true || readingFresh(shadow.updated_at, Date.now());
      this.deviceTime = shadow.updated_at ? Date.parse(shadow.updated_at) : 0;
      this.received = Date.now(); this.refreshCount++; this.error = '';
      this.pruneAlerts();
    } catch (error) {
      if (generation === this.generation) { this.error = '读数同步失败，请检查网络与服务：' + (error as Error).message;this.expiredSession(generation); }
    } finally {
      if (generation === this.generation) { this.polling = false; this.loading = false; }
    }
  }
  private async refreshDetails(generation: number): Promise<void> {
    if (this.detailsPolling) { return; }
    this.detailsPolling = true;
    try {
      await Promise.all([this.refreshRoomRecords(generation),
        this.refreshEvents(generation), this.refreshStats(generation), this.refreshReports(generation)]);
    } finally { if (generation === this.generation) { this.detailsPolling = false; } }
  }
  private async refreshRoomRecords(generation: number): Promise<void> {
    try {
      await this.refreshFamily();
      if (generation !== this.generation || !this.visible) { return; }
      const room = this.familyNodes.find((node: DistributedNode) => node.kind === 'environment' || node.kind === 'nearlink');
      if (!room) { this.doorRecords = null; return; }
      const reply = await this.api.request('/api/v1/devices/' + encodeURIComponent(room.device_id) + '/door-records');
      if (generation === this.generation && this.visible) { this.doorRecords = reply.data as DoorRecords; this.doorRecordMessage = ''; }
    } catch (_) {
      if (generation === this.generation) { this.doorRecordMessage = '门窗记录同步失败，暂保留最近记录'; }
    }
  }
  private async refreshDeviceHealth(generation: number): Promise<void> {
    if (this.healthPolling) { return; }
    this.healthPolling = true;
    try {
      const reply = await this.api.request(this.api.path('health'), http.RequestMethod.GET, '', 8000);
      if (generation !== this.generation || !this.visible) { return; }
      if (reply.data) { this.applyHealth(reply.data as Health); }
      else { this.healthKnown = false; this.living = null; this.medicineNotice = false; }
    } catch (_) {
      // A temporary request failure does not invalidate a still-fresh received report.
    } finally { if (generation === this.generation) { this.healthPolling = false; } }
  }
  private async refreshEvents(generation: number): Promise<void> {
    try {
      const reply = await this.api.request(this.api.path('alarms'));
      if (generation === this.generation && this.visible) { this.receiveAlarms((reply.data as AlarmList).items || []); }
    } catch (error) { if(generation===this.generation){this.error='报警同步失败：'+(error as Error).message;this.expiredSession(generation);} }
  }
  private async refreshStats(generation: number): Promise<void> {
    try {
      const reply = await this.api.request(this.api.path('stats'));
      if (generation !== this.generation || !this.visible) { return; }
      const stats = reply.data as Statistics;
      const date = this.days.length > this.chartDay ? this.days[this.chartDay].date : '';
      this.days = stats.days || [];
      const selected = this.days.findIndex((day: DayStats) => day.date === date);
      this.chartDay = selected >= 0 ? selected : Math.max(0, this.days.length - 1);
      this.hours = this.days.length ? this.days[this.chartDay].activity_by_hour : [];
      this.activityTotal = this.days.length ? this.days[this.chartDay].activity_total : 0;
      this.alarmTotal = this.days.length ? this.days[this.chartDay].alarm_total : 0;
    } catch (_) { /* Do not turn a network failure into a zero-activity record. */ }
  }
  private async refreshReports(generation: number): Promise<void> {
    try {
      const reply = await this.api.request(this.api.path('reports'));
      if (generation === this.generation && this.visible) {
        const report=reply.data as ActivityReport;
        if(report&&report.comparison&&report.week&&report.month){this.activityReport=report;this.reportMessage='';}
        else{this.activityReport=null;this.reportMessage='报告响应不完整，请确认服务器已更新';}
      }
    } catch (_) {
      if (generation === this.generation) { this.reportMessage = this.activityReport ? '报告暂未更新，当前为最近一次记录' : '报告暂不可用，请确认服务已部署'; }
    }
  }
  private startPush(): void {
    this.api.startPush((message: Push) => {
      if (!this.visible || this.demo) { return; }
      if (message.type === 'alarm' && message.data) { this.receiveAlarms([message.data as AlarmEvent]); }
      if (message.type === 'health' && message.data) { this.applyHealth(message.data as Health); }
      if (message.type === 'alarm_remind' && message.data) {
        const remind = message.data as AlarmRemind;
        if (remind.username !== this.accountName && this.allowed('alarms.handle')) { this.showRemindNotice(remind.username||''); }
        this.refresh(true);
      }
      if (message.type === 'alarm_note' && this.selected) { this.refreshSelectedNotes(); }
      if (message.type === 'alarm_note' || message.type === 'alarm_claim' || message.type === 'alarm' || message.type === 'stats' || message.type === 'resync_required' || message.type === 'command_ack') { this.refresh(true); }
    }, (online: boolean) => { this.pushOnline = online; });
  }
  private navigate(index: number): void {
    this.detail = '';
    this.tab = index;
    // Scroll after the new tab is laid out, so the preceding tab offset is not retained.
    setTimeout(()=>{this.pageScroller.scrollTo({ xOffset: 0, yOffset: 0 });},60);
    if (!this.demo && this.loggedIn) { this.refresh(true); }
  }
  private receiveAlarms(items: AlarmEvent[]): void {
    const fresh = this.inbox.accept(items);
    this.alarms = mergeAlarms(this.alarms, items);
    if (this.selected) {
      const key = alarmKey(this.selected);
      const updated = this.alarms.find((item: AlarmEvent) => alarmKey(item) === key);
      if (updated) { this.selected = updated; }
    }
    if (this.alarmReminders && this.visible) {
      this.alertQueue = this.alertQueue.concat(fresh);
      if(!this.demo){fresh.forEach((item:AlarmEvent)=>{AlarmNotifications.publish(this.api.device,item);});}
    }
    if (fresh.length && this.visible) { this.announceAlarms(fresh); }
    this.pruneAlerts();
  }
  // 语音播报只念最新一条：老人听到一句明确的提示就够了，连播反而听不清。
  private announceAlarms(items: AlarmEvent[]): void {
    if (!this.voiceOn || !items.length) { return; }
    const first = items[0];
    const help = items.some((item: AlarmEvent): boolean => item.code === 1);
    const fall = items.some((item: AlarmEvent): boolean => item.code === 2);
    if (help) { Voice.speak('紧急提醒，收到求助信号，请立即查看', true); return; }
    if (fall) { Voice.speak('紧急提醒，疑似跌倒，请立即查看', true); return; }
    Voice.speak('收到' + alarmTitle(first.code) + '提醒，请在手机上查看');
  }
  private alarmSnapshot(): AlarmSnapshot {
    return { fresh: this.demo || this.online && !this.stale(), reportTime: this.deviceTime,
      status: this.status, bootId: this.healthFresh() ? this.healthBoot : -1,
      healthUp: this.healthFresh() ? this.healthUp : -1 };
  }
  private eventActive(item: AlarmEvent): boolean {
    return alarmIsActive(item,this.alarms,this.alarmSnapshot(),this.acknowledgedAlarms);
  }
  private eventLabel(item: AlarmEvent): string {
    const label=alarmStateLabel(item,this.alarms,this.alarmSnapshot(),this.acknowledgedAlarms);
    return item.claim?label+' · '+item.claim.username+' 正在处理':label;
  }
  private pruneAlerts(): void {
    this.alertQueue=this.alertQueue.filter((item: AlarmEvent)=>{
      const current=this.alarms.find((record: AlarmEvent)=>alarmKey(record)===alarmKey(item));
      return this.eventActive(current || item);
    });
  }
  private rememberAcknowledgement(keys: string[]): void {
    this.acknowledgedAlarms=Array.from(new Set<string>(this.acknowledgedAlarms.concat(keys)));
    this.pruneAlerts();
  }
  private closeAlarm(): void {
    if(!this.noteSaving){this.selected=null;this.noteOperation++;}
  }

  private async openAlert(): Promise<void> {
    if (!this.alertQueue.length) { return; }
    const item = this.alertQueue[0];
    this.alertQueue = this.alertQueue.slice(1);
    this.navigate(1);
    await this.showAlarm(item);
  }
  private assessment(): CareAssessment {
    return assessCare({ demo: this.demo || this.simulated, online: this.online, fresh: !this.stale(),
      healthFresh: this.healthFresh(), environment: this.envValid, motion: this.imuValid,
      alarmCode: this.status, activeAlarms: this.pendingCount() });
  }
  private assessmentColor(): string {
    const level = this.assessment().level;
    return level === 'alert' ? RED : level === 'normal' ? PRIMARY : AMBER;
  }
  private profilePrefix(): string { return this.loggedIn ? this.accountName : 'demo'; }
  private async loadProfile(): Promise<void> {
    const prefix = this.profilePrefix();
    try {
      const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
      const name = await store.get(prefix + ':name', '守护者') as string;
      const care = await store.get(prefix + ':care', '家人') as string;
      const phone = await store.get(prefix + ':contact', '') as string;
      const reminders = await store.get(prefix + ':reminders', true) as boolean;
      if (prefix !== this.profilePrefix()) { return; }
      this.displayName = name; this.careName = care; this.contactPhone = phone; this.alarmReminders = reminders;
    } catch (_) { /* Local profile is optional. */ }
  }
  private editProfile(): void {
    this.nameDraft = this.displayName; this.careDraft = this.careName; this.contactDraft = this.contactPhone;
    this.profileError = ''; this.profileOpen = true;
  }
  private async saveProfile(): Promise<void> {
    if (this.profileSaving) { return; }
    if (!this.nameDraft.trim() || !this.careDraft.trim()) { this.profileError = '请填写昵称与关怀对象称呼'; return; }
    const name = this.nameDraft.trim(); const care = this.careDraft.trim();
    const phone = this.contactDraft.trim();
    if (phone && !/^[+0-9 ()-]{5,24}$/.test(phone)) { this.profileError = '请检查联系电话格式，或留空'; return; }
    this.profileSaving = true;
    const prefix = this.profilePrefix();
    try {
      const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
      await store.put(prefix + ':name', name); await store.put(prefix + ':care', care);
      await store.put(prefix + ':contact', phone); await store.flush();
      if (prefix !== this.profilePrefix()) { return; }
      this.displayName = name; this.careName = care; this.contactPhone = phone; this.profileOpen = false;
    } catch (_) { this.profileError = '保存失败，请稍后重试'; }
    finally { this.profileSaving = false; }
  }
  private readableSize(size: number): number {
    return this.elderMode ? Math.max(16, Math.round(size * 1.2)) : size;
  }
  private readableLine(size: number): number {
    return this.elderMode ? Math.max(26, Math.round(size * 1.3)) : size;
  }
  private async setElderMode(enabled: boolean): Promise<void> {
    if (this.elderModeSaving || enabled === this.elderMode) { return; }
    const previous = this.elderMode;
    this.elderMode = enabled;
    this.elderModeSaving = true;
    this.elderModeMessage = '';
    try {
      const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
      await store.put('elder_mode', enabled);
      await store.flush();
      this.elderModeMessage = enabled ? '大字模式已保存到本机' : '已恢复标准显示';
    } catch (_) {
      this.elderMode = previous;
      this.elderModeMessage = '保存失败，已恢复原显示，请重试';
    } finally { this.elderModeSaving = false; }
  }
  private async setReminders(enabled: boolean): Promise<void> {
    this.alarmReminders = enabled;
    if (!enabled) { this.alertQueue = []; }
    try {
      const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
      await store.put(this.profilePrefix() + ':reminders', enabled); await store.flush();
    } catch (_) { this.error = '提醒偏好未保存，下次启动请重新确认'; }
  }
  private openVoice(): void {
    this.detail = 'voice';
    this.voiceStatus = '点一下“开始说话”，然后说“小颐开灯”';
    this.voiceHeard = ''; this.voiceReply = ''; this.voiceTyped = '';
    setTimeout(()=>{this.pageScroller.scrollTo({ xOffset: 0, yOffset: 0 });},60);
  }
  private async ensureMic(): Promise<boolean> {
    try {
      const manager = abilityAccessCtrl.createAtManager();
      const result = await manager.requestPermissionsFromUser(getContext(this), ['ohos.permission.MICROPHONE']);
      return result.authResults.length > 0 && result.authResults[0] === 0;
    } catch (error) { return false; }
  }
  private async listenVoice(): Promise<void> {
    if (this.voiceBusy) { VoiceListen.stop(); this.voiceStatus = '已停止收音，正在识别…'; return; }
    const mic = await this.ensureMic();
    if (!mic) {
      this.voiceStatus = '没有拿到麦克风权限，可以在下方输入框输入指令';
      return;
    }
    this.voiceBusy = true; this.voiceHeard = ''; this.voiceReply = '';
    this.voiceStatus = '正在打开麦克风…';
    const started = await VoiceListen.start(
      (text: string, final: boolean): void => {
        this.voiceHeard = text;
        if (final) { this.voiceBusy = false; this.runVoiceText(text, true); }
      },
      (message: string): void => { this.voiceStatus = message; },
      (message: string): void => {
        this.voiceBusy = false;
        if (!this.voiceHeard) { this.voiceStatus = message || '没听清，请再说一次'; }
      });
    if (!started) {
      this.voiceBusy = false;
      this.voiceStatus = '这台设备暂时用不了语音识别，可在下方输入框输入同样的话';
    }
  }
  // 手动输入与语音识别走同一条解析与执行路径，没麦克风也能验证全部指令。
  private async runVoiceText(text: string, fromVoice: boolean): Promise<void> {
    const content = (text || '').trim();
    if (!content) { this.voiceStatus = '请先说“小颐”，再说要做什么'; return; }
    const intent: VoiceIntent = VoiceCommand.parse(content);
    this.voiceHeard = content;
    if (fromVoice && !intent.wake) {
      this.voiceStatus = '先说唤醒词“小颐”，再说要做什么';
      this.voiceReply = '例如：小颐开灯';
      if (this.voiceOn) { Voice.speak('请先说小颐'); }
      return;
    }
    await this.executeVoice(intent);
  }
  private async executeVoice(intent: VoiceIntent): Promise<void> {
    const type = intent.type;
    const controlled=['light_on','light_off','light_auto','sos','medicine','ack'].includes(type);
    const privateData=['temperature','trend','device','camera'].includes(type);
    if(controlled&&!this.allowed('control')||privateData&&!this.allowed('view')){this.voiceReply='当前账号没有这项操作权限';return;}
    let reply = intent.reply;
    if (type === 'light_on' || type === 'light_off' || type === 'light_auto') {
      if (!this.demo && !this.loggedIn) { reply = '还没登录家庭账号，先登录才能控制夜灯'; }
      else if (!this.living) { reply = '还没收到设备状态，稍等几秒再说一次'; }
      else if (!this.livingFresh()) { reply = '设备数据已过期，先刷新再控制'; }
      else {
        const mode: number = type === 'light_on' ? 1 : type === 'light_off' ? 0 : -1;
        this.sendLiving('set_light', { mode: mode });
        reply = type === 'light_on' ? '已发送开灯' : type === 'light_off' ? '已发送关灯' : '已切换为自动夜灯';
      }
    } else if (type === 'sos') {
      if (!this.demo && !this.loggedIn) { reply = '还没登录家庭账号，无法发送求助'; }
      else { this.careAction(true); reply = '正在发起求助，请在弹窗里确认'; }
    } else if (type === 'medicine') {
      const pending = pendingMedicine(this.living);
      if (!this.living || !pending.length) { reply = '现在没有待确认的服药提醒'; }
      else { this.medicineNoticeSlot = pending[0].slot; this.confirmMedicine(); reply = '已记下这次服药'; }
    } else if (type === 'ack') {
      if (!this.demo && !this.loggedIn) { reply = '还没登录家庭账号，无法确认报警'; }
      else { this.command('ack'); reply = '正在确认报警，请在弹窗里核对'; }
    } else if (type === 'temperature') {
      const hasTemp = this.temp > -900;
      const hasHumi = this.humi > -900;
      if (hasTemp && hasHumi) { reply = '现在 ' + this.temp.toFixed(1) + ' 度，湿度 ' + this.humi.toFixed(0) + '%'; }
      else if (hasTemp) { reply = '现在 ' + this.temp.toFixed(1) + ' 度，湿度还没收到'; }
      else { reply = '还没收到温湿度数据，稍后再问一次'; }
    } else if (type === 'alarm') { this.navigate(1); reply = '已打开报警记录'; }
    else if (type === 'trend') { this.navigate(2); reply = '已打开活动趋势'; }
    else if (type === 'device') { this.navigate(3); reply = '已打开设备状态'; }
    else if (type === 'camera') { this.detail = ''; router.pushUrl({ url: 'pages/Camera' }); reply = '正在打开家中画面'; }
    else if (type === 'help') { reply = '我可以说：开灯、关灯、自动夜灯、救命、我吃过药了、现在温度、看报警、看趋势、看设备、打开摄像头'; }
    else if (type === 'awake') { reply = '我在，请说要做什么'; }
    this.voiceStatus = type === 'unknown' ? '这句没听懂，换个说法试试' : '已执行';
    this.voiceReply = reply;
    this.voiceTick = reply;
    if (this.voiceOn) { Voice.speak(reply, type === 'sos'); }
  }
  private async setVoice(on: boolean): Promise<void> {
    this.voiceOn = on;
    Voice.setEnabled(on);
    if (on) {
      const ready = await Voice.prepare();
      this.voiceMessage = ready ? '语音已打开：报警和服药会念出来' : '本机没有可用的离线语音引擎，暂时只能看弹窗提醒';
      if (ready) { Voice.speak('语音提醒已打开'); }
    } else {
      this.voiceMessage = '已关闭语音播报，弹窗与记录仍然保留';
    }
    try {
      const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
      await store.put('voice_announce', on); await store.flush();
    } catch (_) { this.error = '语音偏好未保存，下次启动请重新确认'; }
  }
  private previewAlert(): void {
    if (!this.demo) { return; }
    const item: AlarmEvent = { boot_id: 0, event_id: Date.now(), code: 1, phase: 'start',
      epoch: Math.floor(Date.now()/1000), up: 0, duration: 0, value: 0 };
    this.status=item.code;
    this.muted=false;
    this.receiveAlarms([item]);
  }
  private openConnection(): void {
    this.passwordDraft = this.loggedIn ? this.sessionPassword : '';
    this.settingsOpen = true;
  }
  private async persistSettings(): Promise<void> {
    try {
      const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
      await store.put('server', this.serverDraft); await store.put('username', this.userDraft); await store.flush();
    } catch (_) { /* Login can work without persisted settings. */ }
  }
  // Everything that must be rebuilt when the active account changes.
  private applySessionState(name: string, password: string): void {
    this.api.username = name;
    this.sessionPassword = password;
    this.familyNodes = []; this.familyRole = ''; this.members = [];
    this.doorRecords=null;this.doorRecordMessage='';this.careActionMessage='';this.sosRequest='';
    this.accountName = name; this.alertQueue = []; this.inbox = new AlarmInbox();
    this.generation++; this.polling = false; this.detailsPolling = false; this.healthPolling = false; this.familyPolling = false; this.demo = false; this.loggedIn = true; this.simulated = false;
    this.temp = -999; this.humi = -999; this.gas = -999; this.light = -999; this.received = 0; this.deviceTime = 0;
    this.doorValue = -1; this.doorEnabled = -1; this.doorValid = -1;
    this.activityReport=null;this.reportMessage='';this.living=null;this.medicineNotice=false;this.medicineSeen=[];this.livingOpen=false;this.acknowledgedAlarms=[];this.healthBoot=-1;this.healthUp=-1;this.healthKnown = false; this.healthReceived = 0; this.healthSignature = ''; this.firmware = ''; this.muted = false; this.alarms = []; this.hours = []; this.days = []; this.activityTotal = 0; this.alarmTotal = 0;
  }
  private linkOpening:boolean=false;
  private sessionReady:boolean=false;
  private async openDeviceLink():Promise<void>{
    if(!this.sessionReady||!DeviceLink.pending||this.linkOpening){return;}
    if(!this.loggedIn||this.demo){this.error='请先登录，登录后验证 NFC 设备的家庭权限';this.settingsOpen=true;return;}
    this.linkOpening=true;const id=DeviceLink.pending,token=this.api.token;
    try{
      const family=await this.api.refreshHousehold();if(token!==this.api.token||!this.visible){return;}
      const node=family.devices.find((item:DistributedNode)=>item.device_id===id);
      if(DeviceLink.pending===id){DeviceLink.pending='';}
      if(!node||!(family.permissions||[]).includes('view')){this.error='当前账号没有这台 NFC 设备的查看权限';this.navigate(3);return;}
      if(node.kind==='gateway'){router.pushUrl({url:DeviceLink.localIp?'pages/LocalControl':'pages/DeviceContact'});}
      else{router.pushUrl({url:'pages/Nodes',params:{kind:node.kind==='wearable'?'wearable':'environment'}});}
    }catch(error){this.error='设备链接验证失败：'+(error as Error).message;}finally{this.linkOpening=false;}
  }
  private async signIn(server: string, username: string, password: string): Promise<void> {
    await this.api.login(server, username, password);
    this.applySessionState(username, password);
    this.serverDraft = server; this.userDraft = username;
    this.passwordDraft = ''; this.settingsOpen = false;
    await this.persistSettings();
    await this.loadProfile();
    await this.refreshFamily();
    await this.refresh(true); this.startPush();
    await this.rememberAccount(username);
    this.openDeviceLink();
  }
  private async rememberAccount(username: string): Promise<void> {
    if (!this.api.token || !username) { return; }
    const entry: SavedAccount = {
      server: this.api.base, username: username, role: this.familyRole,
      token: this.api.token, expiresAt: this.api.tokenExpiresAt,
      devices: this.familyNodes.length, lastUsed: Date.now()
    };
    this.accounts = await Accounts.save(getContext(this) as common.UIAbilityContext, entry);
  }
  private async login(): Promise<void> {
    if (this.loginBusy) { return; }
    this.loginBusy = true; this.error = '';
    const loginPassword = this.passwordDraft;
    try {
      await this.signIn(this.serverDraft, this.userDraft.trim(), loginPassword);
    } catch (error) { this.error = (error as Error).message || '登录失败，请核对地址、密码和开发证书'; }
    finally { this.loginBusy = false; }
  }
  // Switching identity always requires its own password.
  private async switchAccount(entry: SavedAccount): Promise<void> {
    if (this.switchBusy || this.loginBusy) { return; }
    if (!this.demo && this.loggedIn && entry.username === this.accountName && entry.server === this.api.base) {
      this.switchMessage = '当前已经在使用 ' + entry.username; return;
    }
    this.switchMessage = '';
    this.switchUser = entry.username;
    this.switchServer = entry.server;
    this.switchPassword = '';
    this.switchOpen = true;
  }
  private async signInSwitch(): Promise<void> {
    if (this.switchBusy || this.loginBusy || !this.switchUser) { return; }
    if (this.switchPassword.length < 1) { this.switchMessage = '请输入该账号的密码'; return; }
    this.switchBusy = true;
    try {
      await this.signIn(this.switchServer, this.switchUser, this.switchPassword);
      this.switchMessage = '已切换到 ' + this.switchUser;
      this.switchOpen = false;
      this.switchUser = ''; this.switchPassword = ''; this.switchServer = '';
    } catch (error) { this.switchMessage = (error as Error).message; }
    finally { this.switchBusy = false; }
  }
  private async disconnect(): Promise<void> {
    const name = this.accountName, server = this.api.base, known = this.accounts;
    this.generation++; this.loggedIn = false; this.polling = false; this.online = false;
    await this.api.logout().catch(() => {}); this.selected = null; this.settingsOpen = false; this.profileOpen = false; this.loadDemo(); await this.loadProfile();
    // The server dropped this token, so keep the account for switching but mark it as needing a password.
    const previous = known.find((item: SavedAccount): boolean => item.username === name && item.server === server);
    if (previous) {
      this.accounts = await Accounts.save(getContext(this) as common.UIAbilityContext, {
        server: previous.server, username: previous.username, role: previous.role,
        token: '', expiresAt: 0, devices: previous.devices, lastUsed: previous.lastUsed });
    }
  }
  private livingFresh(): boolean { return this.demo || this.livingDataFresh && this.healthFresh() && this.online && this.loggedIn; }
  private openLiving(): void {
    if (!this.living) { this.livingMessage='等待设备生活提醒状态；收到上报后可进行设置'; this.livingOpen=true; return; }
    this.nightDraft=this.living.night_auto;
    this.medicineDrafts=medicineSlots(this.living).map((slot:MedicineSlot):MedicineDraft=>({slot:slot.slot,enabled:slot.enabled,hour:Math.floor(slot.minute/60).toString().padStart(2,'0'),minute:(slot.minute%60).toString().padStart(2,'0')}));
    this.livingRevision=this.living.config_revision; this.livingMessage=this.livingFresh()?'':'设备数据已过期，待同步后可操作'; this.livingOpen=true;
    this.loadMedicineHistory();
  }
  private updateMedicineNotice(): void {
    const pending=pendingMedicine(this.living);
    const day=this.living?this.living.dose_day:-1;
    const keys=pending.map((slot:MedicineSlot)=>day+':'+slot.slot);
    this.medicineSeen=this.medicineSeen.filter((key:string)=>keys.includes(key));
    if(!this.living || !this.living.time_synced || !this.livingFresh() || pending.length===0) { this.medicineNotice=false;return; }
    if(this.medicineNotice && pending.some((slot:MedicineSlot)=>slot.slot===this.medicineNoticeSlot)){return;}
    this.medicineNotice=false;
    for(const slot of pending) {
      const key=day+':'+slot.slot;
      if(!this.medicineSeen.includes(key)) {
        this.medicineSeen=this.medicineSeen.concat([key]);this.medicineNoticeSlot=slot.slot;this.medicineNotice=true;
        if(this.voiceOn){Voice.speak('到吃药时间了，请按时服药，服完在手机上确认',true);}
        break;
      }
    }
  }
  private updateMedicineDraft(index: number, enabled?: boolean, hour?: string, minute?: string): void {
    this.livingMessage='服药时段设置待保存，请点击保存提醒设置';
    this.medicineDrafts=this.medicineDrafts.map((draft:MedicineDraft):MedicineDraft=>draft.slot===index?{slot:index,enabled:enabled!==undefined?enabled:draft.enabled,hour:hour!==undefined?hour:draft.hour,minute:minute!==undefined?minute:draft.minute}:draft);
  }
  private async loadMedicineHistory(): Promise<void> {
    this.medicineHistory=[];this.medicineHistoryMessage='';
    if(this.demo){this.medicineHistoryMessage='演示状态仅保留在本次会话';return;}
    const generation=this.generation;
    try {
      const reply=await this.api.request(this.api.path('medicine/history'));
      if(generation===this.generation){this.medicineHistory=(reply.data as MedicineHistory).items || [];}
    } catch(error) {if(generation===this.generation){this.medicineHistoryMessage='服药历史暂不可用，请确认后端已更新';}}
  }
  private async sendLiving(name: string, paras: CommandParameters): Promise<void> {
    if(!this.allowed('control')){this.livingMessage='此账号没有设备控制权限';return;}
    if (this.careActionBusy || this.livingBusy || this.commandBusy || !this.living || !this.livingFresh()) { return; }
    const generation=this.generation;
    this.livingBusy=true; this.livingMessage='正在等待设备执行回执…';
    try {
      if (this.demo) {
        if(name==='set_light') {
          this.living.light_mode=paras.mode;
          if(this.living.light_owner!=='alarm' && this.living.light_owner!=='medicine'){
            this.living.night_on=paras.mode===1 || paras.mode===-1 && this.living.night_auto && this.light<30;
            this.living.light_owner=this.living.night_on?'night':'off';
          }
        } else {this.living=demoLivingUpdate(this.living,name,paras,Math.floor(Date.now()/1000),this.light);}
        this.livingMessage='演示已更新，未操作真实设备';
        this.medicineNotice=false;this.updateMedicineNotice();
        return;
      }
      const request: CommandRequest={device_id:this.api.device,service_id:'HomeSafety',command_name:name,paras:paras,client_request_id:'living-'+Date.now()+'-'+Math.floor(Math.random()*1000000)};
      let result=(await this.api.request('/api/v1/commands',http.RequestMethod.POST,JSON.stringify(request))).data as Command;
      for(let i=0;i<15&&result.state==='pending';i++) {
        if(generation!==this.generation){return;}
        await new Promise<void>((resolve)=>{setTimeout(()=>{resolve();},1000);});
        if(generation!==this.generation){return;}
        result=(await this.api.request('/api/v1/commands/'+encodeURIComponent(result.command_id))).data as Command;
      }
      if(generation!==this.generation){return;}
      if(result.state==='executed' && result.result_code===0) {
        this.livingMessage='设备已执行，正在核对最新状态…';
        let matched=false;
        for(let attempt=0;attempt<10;attempt++) {
          if(generation!==this.generation){return;}
          const reply=await this.api.request(this.api.path('health'),http.RequestMethod.GET,'',8000);
          if(generation!==this.generation){return;}
          if(reply.data){this.applyHealth(reply.data as Health);}
          if(this.livingFresh()&&livingCommandMatches(this.living,name,paras)){matched=true;break;}
          await new Promise<void>((resolve)=>{setTimeout(()=>{resolve();},1000);});
        }
        if(generation!==this.generation){return;}
        this.livingMessage=matched?'设备设置已生效，状态已同步':'设备已确认执行，但新状态尚未同步，请稍后核对；当前显示最近上报';
        if(matched&&this.living){this.livingRevision=this.living.config_revision;this.nightDraft=this.living.night_auto;}
        if(name==='confirm_medicine'||name==='set_medicine_state'){this.medicineNotice=false;}
      }
      else {this.livingMessage=result.result_code===4?'设置已在板端或其他手机更改，请关闭后重新打开设置':result.state==='pending'||result.state==='unknown'?'执行结果未确认，请核对设备状态':'设备未执行，请检查设备时间及后端物模型';}
      await this.refresh(true);
    } catch(error) {if(generation===this.generation){this.livingMessage='操作未确认：'+(error as Error).message;}}
    finally {this.livingBusy=false;}
  }
  private saveLiving(): void {
    if(!this.living || this.medicineDrafts.length===0){return;}
    if(this.medicineDrafts.some((draft:MedicineDraft)=>!validMedicineTime(draft.hour,draft.minute))) {this.livingMessage='请输入有效时间：小时 00–23，分钟 00–59';return;}
    const first=this.medicineDrafts[0];
    const paras: CommandParameters={night_auto:this.nightDraft,medicine_enabled:first.enabled,medicine_minute:Number(first.hour)*60+Number(first.minute),expected_revision:this.livingRevision};
    if(this.living.schema===2 && this.medicineDrafts.length===3) {
      paras.medicine_enabled_2=this.medicineDrafts[1].enabled;paras.medicine_minute_2=Number(this.medicineDrafts[1].hour)*60+Number(this.medicineDrafts[1].minute);
      paras.medicine_enabled_3=this.medicineDrafts[2].enabled;paras.medicine_minute_3=Number(this.medicineDrafts[2].hour)*60+Number(this.medicineDrafts[2].minute);
      this.sendLiving('set_medicine_plan',paras);
    } else {this.sendLiving('set_living',paras);}
  }
  private changeMedicine(slot: number, taken: boolean): void {
    if(!this.living || !this.livingFresh() || this.livingBusy){return;}
    if(this.living.schema===2 && this.living.medicine_revision) {
      this.sendLiving('set_medicine_state',{dose_day:this.living.dose_day,slot:slot,taken:taken,expected_medicine_revision:this.living.medicine_revision});
    } else if(slot===0 && taken) {this.sendLiving('confirm_medicine',{dose_day:this.living.dose_day});}
  }
  private askMedicine(slot: MedicineSlot): void {
    if(slot.state!==4){this.changeMedicine(slot.slot,true);return;}
    promptAction.showDialog({title:'改为未服药？',message:'仅更改当天第 '+(slot.slot+1)+' 时段的记录。到时未服药提醒会恢复。',buttons:[{text:'取消',color:SUB},{text:'确认修改',color:PRIMARY}]}).then((result)=>{if(result.index===1){this.changeMedicine(slot.slot,false);}}).catch(()=>{});
  }
  private confirmMedicine(): void {
    const slot=pendingMedicine(this.living).find((item:MedicineSlot)=>item.slot===this.medicineNoticeSlot);
    if(slot){this.changeMedicine(slot.slot,true);}
  }
  private async command(name: string): Promise<void> {
    if(!this.allowed('control')){this.commandMessage='此账号没有设备控制权限';return;}
    if (this.careActionBusy || this.commandBusy || (name !== 'ack' && name !== 'mute')) { return; }
    if (!this.demo && !this.loggedIn) {
      this.commandMessage = '请先登录后端，再操作真实设备';
      this.openConnection();
      return;
    }
    // Lock before opening the dialog, not only before the network request.
    const generation = this.generation;
    const targetDevice = this.api.device;
    const demoOperation = this.demo;
    this.commandBusy = true;
    this.commandMessage = '';
    try {
      const uncertain = !demoOperation && (!this.online || this.stale());
      if (name === 'ack' || uncertain) {
        const scope = demoOperation ? '当前为演示模式，不操作真实设备。' :
          `操作终端：${targetDevice}。${uncertain ? '设备离线或数据过期，命令可能无法送达，请联系现场人员。' : ''}`;
        const detail = name === 'ack' ?
          '请先核实现场。此操作会解除当前终端的求助及兼容旧固件的疑似跌倒锁定，并消音；持续环境异常仍保留。不是只处理正在查看的历史事件，也不会自动保存处理记录。' :
          '消音只停止设备提示声音，不表示异常已经解除。';
        const answer = await promptAction.showDialog({
          title: name === 'ack' ? '确认设备报警' : '设备状态待确认',
          message: scope + '\n' + detail,
          buttons: [{ text: '取消', color: SUB }, { text: name === 'ack' ? '确认设备报警' : '尝试消音', color: PRIMARY }]
        });
        if (answer.index !== 1) { return; }
      }
      // A dialog may outlive a login change, page hide or device switch.
      if (generation !== this.generation || demoOperation !== this.demo || targetDevice !== this.api.device) { return; }
      const acknowledgedKeys = this.alarms.filter((item: AlarmEvent) =>
        item.source !== 'wearable' && this.eventActive(item) && (item.code === 1 || item.code === 2)).map((item: AlarmEvent) => alarmKey(item));
      if (demoOperation) {
        this.muted = true;
        if (name === 'ack') {
          if (this.status === 1 || this.status === 2) { this.status = 0; }
          this.rememberAcknowledgement(acknowledgedKeys);
        }
        this.commandMessage = '演示：' + (name === 'ack' ? '已确认设备报警，处理记录请另行保存' : '已消音') + '；未操作真实设备';
        return;
      }
      this.commandMessage = '正在提交命令，等待设备回执…';
      const request: CommandRequest = { device_id: targetDevice, service_id: 'HomeSafety', command_name: name,
        paras: {}, client_request_id: 'zhian-' + Date.now() + '-' + Math.floor(Math.random() * 1000000) };
      const accepted = await this.api.request('/api/v1/commands', http.RequestMethod.POST, JSON.stringify(request));
      let result = accepted.data as Command;
      for (let i = 0; i < 12 && result.state === 'pending'; i++) {
        await new Promise<void>((resolve) => { setTimeout(() => { resolve(); }, 1000); });
        if (generation !== this.generation || targetDevice !== this.api.device) { return; }
        result = (await this.api.request('/api/v1/commands/' + encodeURIComponent(result.command_id))).data as Command;
      }
      if (generation !== this.generation || targetDevice !== this.api.device) { return; }
      if (name === 'ack' && result.state === 'executed' && result.result_code === 0) { this.rememberAcknowledgement(acknowledgedKeys); }
      this.commandMessage = result.state === 'executed' ?
        (result.result_code === 0 ? (name === 'ack' ? '设备已确认报警；处理记录请另行保存' : '设备已消音；异常状态仍需现场核实') : '设备未成功执行，请核对状态后重试') :
        result.state === 'pending' ? '仍在等待设备回执，请核对现场状态，避免重复操作' :
        result.state === 'unknown' ? '结果未确认，请核对设备，勿视为成功' : '命令执行失败';
      await this.refresh(true);
    } catch (error) {
      if (generation === this.generation && targetDevice === this.api.device) {
        this.commandMessage = '操作未完成或结果未确认：' + (error as Error).message;
      }
    } finally { this.commandBusy = false; }
  }
  private unhandledAlarms(): AlarmEvent[] {
    return this.alarms.filter((item:AlarmEvent)=>!item.handling || item.handling.resolution==='follow_up');
  }
  private async careAction(sos: boolean, oldOnly: boolean = false): Promise<void> {
    if(!this.allowed(sos?'control':'alarms.handle')){this.careActionMessage='此账号没有该操作权限';return;}
    if(this.careActionBusy || this.noteSaving || this.commandBusy){return;}
    if(!this.demo && !this.loggedIn){this.openConnection();return;}
    const generation=this.generation;
    const device=this.api.device;
    const simulated=this.demo;
    const items=this.unhandledAlarms().filter((item:AlarmEvent)=>!oldOnly || !this.eventActive(item));
    if(!sos && !items.length){this.careActionMessage='当前已加载的事件均已处理';return;}
    this.careActionBusy=true;this.careActionMessage='';
    try {
      const answer=await promptAction.showDialog({title:sos?'发起主动呼救':oldOnly?'整理旧报警':'批量处置记录',
        message:sos?'向家庭发送求助事件，并请求居家主机启动本地声光报警。设备离线、旧固件或未回执时不能保证现场已报警；不会自动拨打急救电话。':
          '请确认已核实现场，将'+(oldOnly?'已结束的旧记录':'当前已加载的记录')+'共 '+items.length+' 条保存为“已处理”。此操作不删除日志、不消音，也不解除设备报警。',
        buttons:[{text:'取消',color:SUB},{text:sos?'确认呼救':'确认全部已处理',color:sos?RED:PRIMARY}]});
      if(answer.index!==1 || generation!==this.generation || device!==this.api.device){return;}
      if(sos) {
        if(!this.sosRequest){this.sosRequest='app-sos-'+Date.now()+'-'+Math.floor(Math.random()*1000000);}
        const event:AppSOSResult=simulated?{boot_id:2147483647,event_id:Date.now(),code:1,phase:'start',epoch:Math.floor(Date.now()/1000),up:0,duration:0,value:0,source:'app'}:
          (await this.api.request(this.api.path('sos'),http.RequestMethod.POST,JSON.stringify({client_request_id:this.sosRequest,activate_device:true}))).data as AppSOSResult;
        if(generation!==this.generation){return;}
        this.sosRequest='';this.receiveAlarms([event]);
        this.careActionMessage=simulated?'演示呼救已生成，未通知真实家庭':'求助记录已保存，正在等待主机报警回执…';
        if(simulated){this.status=1;this.muted=false;}
        else if(event.device_command){
          let receipt=event.device_command;
          for(let attempt=0;attempt<12&&receipt.state==='pending';attempt++){
            await new Promise<void>((resolve)=>{setTimeout(()=>{resolve();},1000);});
            if(generation!==this.generation||device!==this.api.device){return;}
            receipt=(await this.api.request('/api/v1/commands/'+encodeURIComponent(receipt.command_id))).data as Command;
          }
          if(generation!==this.generation||device!==this.api.device){return;}
          this.careActionMessage=receipt.state==='executed'&&receipt.result_code===0?'求助已发送，主机已确认启动本地报警':receipt.state==='pending'?'求助已保存，主机仍未回执，请联系现场核实':'求助已保存，但主机报警未确认，请联系现场核实';
        } else {this.careActionMessage='求助已保存，但服务器未提供主机回执，请确认后端已升级';}
      } else {
        if(!simulated){await this.api.request(this.api.path('alarms/batch-notes'),http.RequestMethod.POST,
          JSON.stringify({event_keys:items.map((item:AlarmEvent)=>alarmKey(item)),note:'已核实现场，通过 App 批量处理。',resolution:'handled'}));}
        if(generation!==this.generation){return;}
        const keys=items.map((item:AlarmEvent)=>alarmKey(item));
        this.alarms=this.alarms.map((item:AlarmEvent):AlarmEvent=>{
          if(keys.indexOf(alarmKey(item))>=0){item.handling={resolution:'handled',username:this.accountName,created:Date.now()/1000};
            if(simulated){this.demoNotes.set(alarmKey(item),[{note:'已核实现场，通过 App 批量处理。',resolution:'handled',username:'演示用户'}]);}}
          return item;
        });
        this.pruneAlerts();this.careActionMessage='处理记录已保存；持续设备异常仍需排除';
      }
    } catch(error){this.careActionMessage='操作未完成：'+(error as Error).message+'。请确认后端已升级并重试。';}
    finally{this.careActionBusy=false;}
  }
  private chooseBatchScope(): void {
    promptAction.showActionMenu({title:'选择处理范围',buttons:[{text:'当前加载的待处理记录',color:PRIMARY},{text:'仅已结束的旧报警',color:PRIMARY}]}).then((answer)=>{this.careAction(false,answer.index===1);}).catch(()=>{});
  }
  @Builder
  careActions() {
    Column({space:10}) {
      Row({space:12}) {
        Button(this.careActionBusy?'提交中…':'主动呼救').fontSize(this.readableSize(15)).fontWeight(FontWeight.Bold).fontColor(Color.White).backgroundColor(RED).height(this.elderMode?60:48).borderRadius(12).layoutWeight(1).enabled(!this.careActionBusy&&!this.noteSaving&&!this.commandBusy&&this.allowed('control')).onClick(()=>{this.careAction(true);})
        Button('一键处理全部').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).height(this.elderMode?60:48).borderRadius(12).layoutWeight(1).enabled(!this.careActionBusy&&!this.noteSaving&&!this.commandBusy&&this.allowed('alarms.handle')).onClick(()=>{this.chooseBatchScope();})
      }.width('100%')
      if(this.careActionMessage){Text(this.careActionMessage).fontSize(this.readableSize(13)).fontColor(SUB).width('100%')}
    }.padding(16).backgroundColor(Color.White).borderRadius(16).width('100%').margin({top:16,bottom:8})
  }
  private async showAlarm(item: AlarmEvent): Promise<void> {
    if(this.noteSaving){return;}
    const operation=++this.noteOperation;
    this.selected=item; this.noteDraft=''; this.notes=[]; this.resolution='handled'; this.noteMessage='';this.noteRequest='';this.noteRequestBody='';
    if(this.demo){
      this.notes=this.demoNotes.get(alarmKey(item)) || [];
      return;
    }
    try {
      const reply=await this.api.request(this.api.path('alarms/'+alarmKey(item)+'/notes'));
      if(operation===this.noteOperation && this.selected && alarmKey(this.selected)===alarmKey(item)) {
        this.notes=(reply.data as NoteList).items || [];
      }
    } catch(error) {
      if(operation===this.noteOperation){this.noteMessage='备注读取失败：'+(error as Error).message;}
    }
  }
  // Accounts without handling rights can still ask someone who has them.
  private async remindAlarm(): Promise<void> {
    if(this.remindBusy||!this.selected||this.demo){if(this.demo){this.remindMessage='演示环境不会真的发送提醒';}return;}
    const key=alarmKey(this.selected), generation=this.generation;
    this.remindBusy=true;this.remindMessage='';
    try{
      await this.api.request(this.api.path('alarms/'+key+'/remind'),http.RequestMethod.POST,'{}');
      if(generation!==this.generation){return;}
      this.remindMessage='已提醒有处置权限的成员，对方打开 App 即可看到';
    }catch(error){
      if(generation===this.generation){this.remindMessage='提醒未发送：'+(error as Error).message;}
    }finally{this.remindBusy=false;}
  }
  private async pollUnread(): Promise<void> {
    if(this.unreadPolling||!this.loggedIn||this.demo){return;}
    this.unreadPolling=true;
    try{
      const response=await this.api.request('/api/v1/messages/unread');
      const data=response.data as UnreadTotal;
      const total=Number((data&&data.unread)||0);
      this.unreadTotal=total;
      if(this.lastUnread>=0&&total>this.lastUnread&&this.tab!==4){this.showMessageNotice(total);}
      this.lastUnread=total;
    }catch(error){/* Keep the previous count while the network is down. */}
    finally{this.unreadPolling=false;}
  }
  private async showMessageNotice(total:number): Promise<void> {
    if(this.noticeOpen){return;}
    this.noticeOpen=true;
    try{
      const answer=await promptAction.showDialog({title:'有新的家人消息',message:'未读消息 '+total+' 条，是否现在查看？',
        buttons:[{text:'稍后',color:CareTheme.secondary},{text:'查看',color:CareTheme.primary}]});
      if(answer.index===1){this.lastUnread=this.unreadTotal;router.pushUrl({url:'pages/Messages'});}
    }catch(error){/* A dismissed dialog must never break polling. */}
    finally{this.noticeOpen=false;}
  }
  private async showRemindNotice(by:string): Promise<void> {
    if(this.noticeOpen){return;}
    this.noticeOpen=true;
    try{
      const answer=await promptAction.showDialog({title:'有人请你查看报警',message:(by||'一位成员')+' 提醒你查看并处理这条报警记录',
        buttons:[{text:'稍后',color:CareTheme.secondary},{text:'查看',color:CareTheme.primary}]});
      if(answer.index===1){this.navigate(1);}
    }catch(error){}
    finally{this.noticeOpen=false;}
  }
  private async claimAlarm(release:boolean=false): Promise<void> {
    if(!this.selected||this.claimBusy||this.noteSaving||!this.allowed('alarms.handle')){return;}
    const key=alarmKey(this.selected), generation=this.generation;
    this.claimBusy=true;this.noteMessage='';
    try{
      if(!this.demo){await this.api.request(this.api.path('alarms/'+key+'/claim'),release?http.RequestMethod.DELETE:http.RequestMethod.POST);}
      if(generation!==this.generation){return;}
      if(this.selected&&alarmKey(this.selected)===key){
        const item=this.selected;
        this.selected={boot_id:item.boot_id,event_id:item.event_id,code:item.code,phase:item.phase,epoch:item.epoch,up:item.up,duration:item.duration,value:item.value,source:item.source,source_device_id:item.source_device_id,handling:item.handling,claim:release?undefined:{username:this.accountName||'演示用户',created:Date.now()/1000}};
      }
      this.noteMessage=release?'已释放认领，其他成员可以接手':'已认领，请核实现场后保存处理记录';
      if(!this.demo){await this.refreshEvents(generation);}
    }catch(error){if(generation===this.generation){this.noteMessage='认领操作未完成：'+collaborationError((error as Error).message);await this.refreshEvents(generation);}}
    finally{this.claimBusy=false;}
  }
  private async refreshSelectedNotes(): Promise<void> {
    if(!this.selected||this.demo){return;}
    const key=alarmKey(this.selected), generation=this.generation;
    try{
      const reply=await this.api.request(this.api.path('alarms/'+key+'/notes'));
      if(generation===this.generation&&this.selected&&alarmKey(this.selected)===key){this.notes=(reply.data as NoteList).items||[];}
    }catch(error){this.expiredSession(generation);}
  }
  private async saveNote(): Promise<void> {
    if(!this.selected || this.noteSaving || this.careActionBusy || !this.allowed('alarms.handle')){return;}
    const item=this.selected;
    const text=noteText(this.noteDraft,this.resolution);
    const resolution=this.resolution;
    const payload=JSON.stringify({note:text,resolution:resolution});
    if(payload!==this.noteRequestBody){this.noteRequestBody=payload;this.noteRequest='note-'+Date.now()+'-'+Math.random().toString(36).slice(2);}
    const operation=++this.noteOperation;
    this.noteSaving=true; this.noteMessage='正在保存处理记录…';
    try {
      if(!this.demo) {
        await this.api.request(this.api.path('alarms/'+alarmKey(item)+'/notes'),http.RequestMethod.POST,
          JSON.stringify({note:text,resolution:resolution,client_request_id:this.noteRequest}));
      }
      if(operation!==this.noteOperation){return;}
      this.notes=this.notes.concat([{note:text,resolution:resolution,username:this.demo?'演示用户':this.accountName}]);
      if(this.demo){this.demoNotes.set(alarmKey(item),this.notes);}
      this.alarms=this.alarms.map((record:AlarmEvent):AlarmEvent=>{
        if(alarmKey(record)===alarmKey(item)){record.handling={resolution:resolution,username:this.accountName,created:Date.now()/1000};}
        return record;
      });
      this.pruneAlerts();
      this.noteDraft='';
      this.noteMessage='';
      // Close only after persistence succeeds; queued taps then have no selected event.
      this.selected=null;
    } catch(error) {
      if(operation===this.noteOperation){this.noteMessage=(error as Error).message.includes('Alarm not found')?'该事件已不在服务器中，请关闭后刷新报警列表，再选择现有事件处理':'保存失败：'+collaborationError((error as Error).message);}
    } finally {
      if(operation===this.noteOperation){this.noteSaving=false;}
    }
  }
  private age(): string {
    // Query success must not make an old device report look fresh.
    if (!Number.isFinite(this.deviceTime) || this.deviceTime <= 0) { return '等待设备上报'; }
    const seconds = Math.max(0, Math.floor((this.now - this.deviceTime) / 1000));
    if (this.demo) { return `演示数据 · ${seconds} 秒前`; }
    return seconds < 2 ? '设备刚刚上报' : `${seconds} 秒前上报`;
  }
  private stale(): boolean { return !this.demo && (!this.deviceTime || this.now - this.deviceTime > 45000); }
  private roomDoor(): string {
    return doorLabel({ demo: this.demo, fresh: this.online && !this.stale(),
      healthFresh: this.healthFresh(), enabled: this.doorEnabled, valid: this.doorValid, value: this.doorValue });
  }
  private healthFresh(): boolean { return this.healthKnown && (this.demo || this.now - this.healthReceived <= 120000); }
  private number(value: number, suffix: string, digits: number = 0): string { return value === -999 || !Number.isFinite(value) ? '--' : value.toFixed(digits) + suffix; }
  private lastActivity():string {
    if(!this.demo && (!this.online || this.stale()) || this.inactive<0 || !this.deviceTime){return '最后活动时间待确认';}
    return '最后活动约 '+new Date(this.deviceTime-this.inactive*1000).toLocaleString('zh-CN',{hour12:false});
  }
  private roomNode(): DistributedNode | null {
    const found = this.familyNodes.find((node: DistributedNode) => node.kind === 'environment' || node.kind === 'nearlink');
    return found ? found : null;
  }
  private roomProps(): EnvironmentProperties | null {
    const node = this.roomNode();
    return node && node.properties ? node.properties : null;
  }
  private roomStamp(): string {
    const node = this.roomNode();
    return node && node.updated_at ? node.updated_at : '';
  }
  private roomAge(): string {
    const node = this.roomNode();
    if (this.demo) { return '演示数据'; }
    if (!node) { return '尚未绑定房间节点'; }
    return '房间节点 '+this.roomAgeShort()+'上报';
  }
  private roomAgeShort(): string {
    if (this.demo) { return '演示数据'; }
    const node = this.roomNode();
    if (!node) { return '未绑定节点'; }
    const at = Date.parse(node.updated_at || '');
    if (!Number.isFinite(at)) { return '时间待确认'; }
    const seconds = Math.max(0, Math.floor((this.now - at) / 1000));
    if (seconds < 60) { return seconds+' 秒前'; }
    if (seconds < 3600) { return Math.floor(seconds / 60)+' 分钟前'; }
    if (seconds < 86400) { return Math.floor(seconds / 3600)+' 小时前'; }
    return Math.floor(seconds / 86400)+' 天前';
  }
  private roomStampCaption(): string {
    if (this.demo) { return '演示读数'; }
    const node = this.roomNode();
    if (!node) { return '未绑定节点'; }
    return '上次上报 '+this.roomAgeShort();
  }
  private roomOnline(): string {
    if (this.demo) { return '演示'; }
    const node = this.roomNode();
    if (!node) { return '待确认'; }
    return node.online === true || readingFresh(node.updated_at, this.now) ? '近期在线' : '离线或待更新';
  }
  private doorStateTitle(): string {
    const label = this.roomDoor();
    return label.indexOf('演示') === 0 ? '房门关闭' : label;
  }
  private doorEntryValue(): string {
    if (this.demo) { return '演示 · 已关闭'; }
    if (!this.online || this.stale()) { return '数据待更新'; }
    if (this.doorEnabled === 0) { return '未开启'; }
    if (this.doorValid === 0) { return '读数无效'; }
    if (this.doorValue === 1) { return '已打开'; }
    if (this.doorValue === 0) { return '已关闭'; }
    return '待上报';
  }
  private activityEntryValue(): string {
    if (this.stale()) { return '数据待更新'; }
    if (this.inactive < 0) { return this.activityTotal > 0 ? '今日 '+this.activityTotal+' 次' : '待上报'; }
    return '距上次 '+this.inactive+' 秒';
  }
  private openDetail(action: string): void {
    if (this.loggedIn && !this.demo) { this.refresh(true); }
    this.detail = action;
    this.pageScroller.scrollTo({ xOffset: 0, yOffset: 0 });
  }
  private doorItems(): DoorRecord[] { return this.doorRecords ? this.doorRecords.items.slice(0,10) : []; }
  private doorRecordTime(epoch:number):string {
    if (!Number.isFinite(epoch) || epoch <= 0) { return '时间待确认'; }
    const date = new Date(epoch * 1000);
    return (date.getMonth()+1)+'/'+date.getDate()+' '+pad2(date.getHours())+':'+pad2(date.getMinutes());
  }
  private doorHint(): string {
    if (this.doorRecords) { return this.doorRecords.summary.hint; }
    if (this.demo) { return '演示模式不读取真实门窗记录；连接设备后这里显示开合历史与异常提示。'; }
    return this.doorRecordMessage || '正在同步门窗记录，初始状态不计为开关事件。';
  }
  private doorHintTone(): string { return this.doorRecords && this.doorRecords.summary.level==='attention' ? AMBER : SUB; }
  private doorRecordCaption(): string {
    if (this.demo) { return '演示模式'; }
    if (!this.doorRecords) { return '同步中'; }
    return this.doorRecords.items.length ? '最近 '+this.doorRecords.items.length+' 条' : '暂无开关变化';
  }
  private roomLight(): string { const p = this.roomProps(); return p ? sensorText(p.light,'') : '--'; }
  private roomSmoke(): string { const p = this.roomProps(); return p ? sensorText(p.smoke,'') : '--'; }
  private roomMotion(): string {
    const p = this.roomProps();
    if (!p) { return '待上报'; }
    return p.body === 1 ? '有活动' : p.body === 0 ? '无活动' : '待上报';
  }
  private roomDoorShort(): string {
    const p = this.roomProps();
    if (!p) { return '待上报'; }
    return p.door === 'OPEN' || p.door === 1 ? '已开' : p.door === 'CLOSED' || p.door === 0 ? '已关' : '待上报';
  }
  private chartMax(): number { let max = 1; this.hours.forEach((value: number) => { max = Math.max(max, value); }); return max; }
  private daysDesc(): DayStats[] { return this.days.slice().reverse(); }
  private isSelectedDay(day: DayStats): boolean {
    return this.days.length > 0 && this.chartDay < this.days.length && this.days[this.chartDay].date === day.date;
  }
  private selectDay(day: DayStats): void {
    const index = this.days.findIndex((item: DayStats) => item.date === day.date);
    if (index < 0) { return; }
    this.chartDay = index; this.hours = day.activity_by_hour; this.activityTotal = day.activity_total; this.alarmTotal = day.alarm_total;
  }
  private pendingCount(): number { return this.alarms.filter((item: AlarmEvent) => this.eventActive(item)).length; }
  private statusTitle(): string { return this.status > 0 ? alarmTitle(this.status) : this.stale() ? '数据等待更新' : !this.online ? '设备状态待确认' : '当前未见报警'; }

  @Builder
  icon(source: Resource, size: number = 22) { Image(source).width(size).height(size).objectFit(ImageFit.Contain) }
  @Builder
  pill($$: PillView) {
    Text($$.text).fontSize(this.readableSize(13)).fontWeight(FontWeight.Medium).fontColor($$.color).padding({ left: 9, right: 9, top: 5, bottom: 5 }).backgroundColor($$.background).borderRadius(12)
  }
  @Builder
  section($$: SectionView) {
    Row() { Text($$.title).fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); if ($$.subtitle) { Text($$.subtitle).fontSize(this.readableSize(13)).fontColor(SUB) } }.width('100%').margin({ top: 22, bottom: 12 })
  }
  @Builder
  metric($$: MetricView) {
    Column({ space: 12 }) {
      Row() { Stack() { Rect().fill(CareTheme.softBlue).width(36).height(36).radius(12); this.icon($$.icon, 21) }; Blank(); Text($$.label).fontSize(this.readableSize(13)).fontColor(SUB) }.width('100%')
      Text($$.value).fontSize(this.readableSize(29)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      Text($$.caption).fontSize(this.readableSize(13)).fontColor($$.tint).width('100%')
    }.padding(16).backgroundColor(Color.White).borderRadius(16).layoutWeight(1).alignItems(HorizontalAlign.Start)
  }
  @Builder
  homeEntry(title: string, subtitle: string, symbol: Resource, camera: boolean) {
    Column({space:8}) {
      this.icon(symbol,26)
      Text(title).fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(INK)
      Text(subtitle).fontSize(this.readableSize(13)).fontColor(SUB)
    }.padding({top:15,bottom:15,left:4,right:4}).layoutWeight(1).border({width:{right:camera?0:0.5},color:CareTheme.line})
      .onClick(()=>{if (camera) {router.pushUrl({url:'pages/Camera'});} else if (title === '随身健康') {router.pushUrl({url:'pages/Nodes',params:{kind:'wearable'}});} else {this.navigate(3);}})
  }
  @Builder
  brandHeader() {
    Row({space:8}) {
      this.icon($r('app.media.brand_mark'),30)
      Text('颐鸿安').fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK)
      Blank()
      Button({type:ButtonType.Normal}) {this.icon($r('app.media.user'),23)}.width(44).height(44).backgroundColor(Color.Transparent).borderRadius(12).onClick(()=>{this.navigate(4);})
    }.width('100%').margin({top:4,bottom:8})
  }
  @Builder
  entryRow($$: EntryView) {
    Row({space:12}) {
      this.icon($$.icon,23)
      Text($$.title).fontSize(this.readableSize(16)).fontWeight(FontWeight.Medium).fontColor(INK).layoutWeight(1)
      Text($$.value).fontSize(this.readableSize(13)).fontColor(SUB).constraintSize({maxWidth:'48%'}).textAlign(TextAlign.End)
      this.icon($r('app.media.chevron'),16)
    }.width('100%').constraintSize({minHeight:56}).padding({top:10,bottom:10}).onClick(()=>{
      if(['living','trend','environment','wearable','camera','activity','door','reports','diagnostics','remotecare'].includes($$.action)&&!this.allowed('view')){this.familyMessage='当前账号仅开放报警协作';return;}
      if($$.action==='living'){this.openLiving();}
      else if($$.action==='trend'){this.navigate(2);}
      else if($$.action==='environment'||$$.action==='wearable'){router.pushUrl({url:'pages/Nodes',params:{kind:$$.action}});}
      else if($$.action==='camera'){router.pushUrl({url:'pages/Camera'});}
      else if($$.action==='messages'){if(this.loggedIn&&!this.demo){router.pushUrl({url:'pages/Messages'});}else{this.openConnection();}}
      else if($$.action==='accounts'){router.pushUrl({url:'pages/AccountCenter',params:{server:this.serverDraft}});}
      else if($$.action==='remotecare'){if(this.loggedIn&&!this.demo){router.pushUrl({url:'pages/CareSettings'});}else{this.openConnection();}}
      else if($$.action==='localcontrol'){if(this.loggedIn&&!this.demo){router.pushUrl({url:'pages/LocalControl'});}else{this.openConnection();}}
      else if($$.action==='devicecontact'){if(this.loggedIn&&!this.demo){router.pushUrl({url:'pages/DeviceContact'});}else{this.openConnection();}}
      else if($$.action==='connection'){this.openConnection();}
      else if($$.action==='profile'){this.editProfile();}
      else {this.openDetail($$.action);if($$.action==='members'){this.refreshFamily();}}
    })
  }
  @Builder
  readingCell($$: ReadingView) {
    Column({space:7}) {
      Row({space:7}) {this.icon($$.icon,19);Text($$.label).fontSize(this.readableSize(13)).fontColor(SUB)}.width('100%')
      Row({space:4}) {Text($$.value).fontSize(this.readableSize(28)).minFontSize(18).maxFontSize(this.readableSize(28)).maxLines(1).layoutWeight(1).fontWeight(FontWeight.Bold).fontColor(INK);Text($$.unit).fontSize(this.readableSize(13)).fontColor(SUB).margin({bottom:4})}.width('100%').alignItems(VerticalAlign.Bottom)
    }.padding({left:14,right:10,top:14,bottom:14}).layoutWeight(1).alignItems(HorizontalAlign.Start)
  }
  @Builder
  overview() {
    Column() {
      this.brandHeader()
      Text(this.careName+'的守护').fontSize(this.readableSize(27)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      Text('把关心，放在日常里').fontSize(this.readableSize(14)).fontColor(SUB).width('100%').margin({top:5,bottom:14})
      Row({space:12}){
        Column({space:5}){
          Text('对小颐说').fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
          Text(this.voiceTick||'“小颐开灯” · “小颐救命” · “小颐我吃过药了”').fontSize(this.readableSize(12)).fontColor(SUB).width('100%').maxLines(1).textOverflow({overflow:TextOverflow.Ellipsis})
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        Button('语音',{type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(Color.White).backgroundColor(PRIMARY).borderRadius(12).constraintSize({minHeight:44,minWidth:72}).onClick(()=>{this.openVoice();})
      }.width('100%').padding(14).backgroundColor(Color.White).borderRadius(16).margin({bottom:14})
      Column({space:12}) {
        Row({space:12}) {
          this.icon(this.assessment().level==='normal'?$r('app.media.shield_white'):this.assessment().level==='alert'?$r('app.media.warning_white'):$r('app.media.clock_white'),30)
          Column({space:5}) {
            Text(this.demo?'演示中的守护状态':this.assessment().title).fontSize(this.readableSize(20)).fontWeight(FontWeight.Bold).fontColor(Color.White).width('100%')
            Text(this.age()).fontSize(this.readableSize(13)).fontColor('#E3ECF3').width('100%')
          }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        }.width('100%')
        Divider().color('#7293AF').strokeWidth(0.5)
        Row({space:7}) {
          Circle().fill(this.demo?CareTheme.apricot:this.pushOnline?'#FFFFFF':'#B9CADD').width(6).height(6)
          Text(this.demo?'本地演示':!this.loggedIn?'尚未连接服务':this.pushOnline?'云端已连接':'云端待连接 · 轮询备用').fontSize(this.readableSize(13)).fontColor(Color.White).layoutWeight(1)
          Button(this.loading?'更新中':'刷新', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(Color.White).backgroundColor(Color.Transparent).constraintSize({minWidth:62,minHeight:44}).borderRadius(12).border({width:0.7,color:'#C9D9E7'}).enabled(!this.loading).onClick(()=>{this.refresh(true);})
        }.width('100%')
      }.padding(16).backgroundColor(PRIMARY).borderRadius(16).width('100%')
      Row({space:10}) {
        this.icon($r('app.media.bell'),21)
        Text(this.pendingCount()>0?this.pendingCount()+' 条报警需要查看':'查看报警与处理记录').fontSize(this.readableSize(15)).fontWeight(FontWeight.Medium).fontColor(this.pendingCount()>0?RED:INK).layoutWeight(1)
        this.icon($r('app.media.chevron'),17)
      }.padding({left:14,right:14,top:14,bottom:14}).width('100%').constraintSize({minHeight:52}).backgroundColor(this.pendingCount()>0?CareTheme.alarmSurface:Color.White).borderRadius(12).margin({top:12}).onClick(()=>{this.navigate(1);})
      if(this.error){Text(this.error).fontSize(this.readableSize(13)).fontColor(AMBER).width('100%').margin({top:10})}
      this.section({title:'生活提醒',subtitle:''})
      Column() {
        this.entryRow({icon:$r('app.media.lamp'),title:'自动夜灯',value:nightLabel(this.living,this.livingFresh()),action:'living'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.pill'),title:'服药提醒',value:this.livingFresh()?'查看三个时段':'待同步',action:'living'})
      }.padding({left:16,right:16}).width('100%').backgroundColor(Color.White).borderRadius(16)
      this.section({title:'房间环境',subtitle:this.stale()?'最近一次记录':'设备最近上报'})
      Column() {
        Row() {
          this.readingCell({label:'温度',value:this.number(this.temp,'',1),unit:'℃',icon:$r('app.media.thermometer')})
          Divider().vertical(true).height(65).color(CareTheme.line).strokeWidth(0.5)
          this.readingCell({label:'湿度',value:this.number(this.humi,'',1),unit:'%',icon:$r('app.media.droplet')})
        }.width('100%')
        Divider().color(CareTheme.line).strokeWidth(0.5).width('90%')
        Row() {
          this.readingCell({label:'烟气估算',value:this.number(this.gas,''),unit:'参考 ppm',icon:$r('app.media.flame')})
          Divider().vertical(true).height(65).color(CareTheme.line).strokeWidth(0.5)
          this.readingCell({label:'光照',value:this.number(this.light,''),unit:'lx',icon:$r('app.media.sun')})
        }.width('100%')
      }.width('100%').backgroundColor(Color.White).borderRadius(16)
      Text(this.stale()?'记录已过期，不能用于确认当前安全状态':this.demo?'演示读数，不代表真实设备状态':this.age()).fontSize(this.readableSize(13)).fontColor(this.stale()?AMBER:SUB).width('100%').margin({top:8,bottom:16})
      Column() {
        this.entryRow({icon:$r('app.media.door'),title:'房门',value:this.doorEntryValue(),action:'door'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.activity'),title:'活动记录',value:this.activityEntryValue(),action:'activity'})
      }.padding({left:16,right:16}).width('100%').backgroundColor(Color.White).borderRadius(16)
      Row(){Text('需要帮助？').fontSize(this.readableSize(14)).fontColor(SUB).layoutWeight(1);Button('主动呼救', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(RED).backgroundColor(Color.White).border({width:0.7,color:CareTheme.line}).borderRadius(12).constraintSize({minHeight:48,minWidth:104}).enabled(!this.careActionBusy).onClick(()=>{this.careAction(true);})}.width('100%').margin({top:20})
      if(this.careActionMessage){Text(this.careActionMessage).fontSize(13).fontColor(SUB).width('100%').margin({top:8})}
    }.width('100%')
  }
  @Builder
  livingCard() {
    Column({space:14}) {
      Row() { Rect().width(4).height(18).fill(CareTheme.apricot).radius(2).margin({right:8}); Text('生活提醒').fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK);Blank();Text('设置').fontSize(this.readableSize(13)).fontColor(PRIMARY).onClick(()=>{this.openLiving();}) }.width('100%')
      Row({space:12}) {
        this.icon($r('app.media.sun'),25)
        Column({space:4}) { Text('夜灯控制').fontSize(this.readableSize(14)).fontColor(INK);Text('低照度自动点亮 · 报警优先').fontSize(this.readableSize(13)).fontColor(SUB) }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Text(nightLabel(this.living,this.livingFresh())).fontSize(this.readableSize(13)).fontColor(PRIMARY).constraintSize({maxWidth:130}).textAlign(TextAlign.End)
      }.width('100%')
      Row({space:8}) {
        Button('开灯').fontSize(13).fontColor(Color.White).backgroundColor(PRIMARY).layoutWeight(1).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.sendLiving('set_light',{mode:1});})
        Button('关灯').fontSize(13).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).layoutWeight(1).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.sendLiving('set_light',{mode:0});})
        Button('恢复自动').fontSize(13).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).layoutWeight(1).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.sendLiving('set_light',{mode:-1});})
      }.width('100%')
      Text('报警、服药提示优先；重启后恢复自动策略').fontSize(13).fontColor(SUB).width('100%')
      Divider().color('#EDF2F3')
      Row() {Text('每日服药提醒').fontSize(this.readableSize(14)).fontColor(INK);Blank();Text(this.living&&this.living.schema===2?'最多三个时段':'单次提醒 · 旧版设备').fontSize(this.readableSize(13)).fontColor(SUB)}.width('100%')
      ForEach(medicineSlots(this.living),(slot:MedicineSlot)=>{
        Row({space:10}) {
          this.icon($r('app.media.bell'),22)
          Column({space:4}) {
            Text('第 '+(slot.slot+1)+' 时段 · '+medicineTime(slot.minute)).fontSize(this.readableSize(13)).fontColor(INK)
            Text(slotLabel(slot,this.living?this.living.time_synced:false,this.livingFresh())).fontSize(this.readableSize(13)).fontColor(slot.state===3?AMBER:PRIMARY)
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          if(slot.enabled && (slot.state===3||slot.state===4) && this.livingFresh()) {
            Button(slot.state===4?'改为未服药':'确认已服药').fontSize(this.readableSize(13)).height(34).backgroundColor(slot.state===4?CareTheme.softBlue:PRIMARY).fontColor(slot.state===4?PRIMARY:'#FFFFFF').enabled(!this.livingBusy&&!this.commandBusy&&(slot.state!==4||(this.living!==null&&this.living.schema===2))).onClick(()=>{this.askMedicine(slot);})
          }
        }.width('100%')
      },(slot:MedicineSlot)=>slot.slot+':'+slot.minute+':'+slot.enabled+':'+slot.state+':'+this.livingFresh())
      if(!this.living){Text('等待设备生活提醒状态').fontSize(this.readableSize(13)).fontColor(SUB)}
      if(this.livingMessage) {Text(this.livingMessage).fontSize(this.readableSize(13)).fontColor(AMBER).width('100%')}
    }.padding(18).width('100%').backgroundColor(CareTheme.warmSurface).borderRadius(16).margin({top:14})
  }
  @Builder
  doseActions(slot:MedicineSlot) {
    Text(slotLabel(slot,this.living?this.living.time_synced:false,this.livingFresh())).fontSize(this.readableSize(14)).fontColor(!this.livingFresh()||slot.state===3?AMBER:SUB)
    if(slot.enabled&&(slot.state===3||slot.state===4)) {
      Button(slot.state===4?'撤销已服药':'确认已服药',{type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(slot.state===4?PRIMARY:Color.White).backgroundColor(slot.state===4?CareTheme.softBlue:PRIMARY).borderRadius(12).constraintSize({minHeight:48}).enabled(this.livingFresh()&&!this.livingBusy&&!this.commandBusy&&(slot.state!==4||(this.living!==null&&this.living.schema===2))).onClick(()=>{this.askMedicine(slot);})
    }
  }
  @Builder
  livingSheet() {
    Column() {
      Row({space:10}){Button({type:ButtonType.Normal}){this.icon($r('app.media.back'),24)}.width(44).height(44).backgroundColor(Color.Transparent).enabled(!this.livingBusy).onClick(()=>{this.livingOpen=false;});Text('生活提醒').fontSize(this.readableSize(24)).fontWeight(FontWeight.Bold).fontColor(INK)}.width('100%').padding({left:12,right:20,top:8,bottom:8})
      Scroll() {
        Column({space:20}) {
          Text(this.demo?'演示设置，不操作真实设备':this.livingFresh()?'设置以设备回执为准':'设备状态待同步，当前仅可查看').fontSize(this.readableSize(14)).fontColor(this.livingFresh()?SUB:AMBER).width('100%')
          Column({space:14}) {
            Row({space:10}){this.icon($r('app.media.lamp'),24);Column({space:5}){Text('自动夜灯').fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK);Text(nightLabel(this.living,this.livingFresh())).fontSize(this.readableSize(14)).fontColor(SUB)}.layoutWeight(1).alignItems(HorizontalAlign.Start);Toggle({type:ToggleType.Switch,isOn:this.nightDraft}).selectedColor(PRIMARY).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onChange((v:boolean)=>{this.nightDraft=v;this.livingMessage='自动夜灯设置待保存，请点击保存提醒设置';})}.width('100%')
            Text('开关用于启用自动感光；修改后需保存。低于 30 lx 开灯，高于 60 lx 关灯').fontSize(this.readableSize(14)).fontColor(SUB).width('100%')
            Row({space:8}) {
              Button('开灯',{type:ButtonType.Normal}).fontSize(14).fontColor(this.living?.light_mode===1?Color.White:PRIMARY).backgroundColor(this.living?.light_mode===1?PRIMARY:CareTheme.softBlue).layoutWeight(1).constraintSize({minHeight:48}).borderRadius(12).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.sendLiving('set_light',{mode:1});})
              Button('关灯',{type:ButtonType.Normal}).fontSize(14).fontColor(this.living?.light_mode===0?Color.White:PRIMARY).backgroundColor(this.living?.light_mode===0?PRIMARY:CareTheme.softBlue).layoutWeight(1).constraintSize({minHeight:48}).borderRadius(12).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.sendLiving('set_light',{mode:0});})
              Button('自动',{type:ButtonType.Normal}).fontSize(14).fontColor(this.living?.light_mode===-1?Color.White:PRIMARY).backgroundColor(this.living?.light_mode===-1?PRIMARY:CareTheme.softBlue).layoutWeight(1).constraintSize({minHeight:48}).borderRadius(12).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.sendLiving('set_light',{mode:-1});})
            }.width('100%')
            Text('报警与服药提示优先，重启后恢复自动策略').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
          }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
          Column({space:8}){
            Row({space:8}){this.icon($r('app.media.pill'),24);Text('每日服药').fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK)}.width('100%')
            Text('北京时间 · 按已有医嘱设置').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
            ForEach(this.medicineDrafts,(draft:MedicineDraft)=>{
              Column({space:10}) {
                Row(){Text('第 '+(draft.slot+1)+' 时段').fontSize(this.readableSize(16)).fontColor(INK).layoutWeight(1);Toggle({type:ToggleType.Switch,isOn:draft.enabled}).selectedColor(PRIMARY).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onChange((v:boolean)=>{this.updateMedicineDraft(draft.slot,v);})}.width('100%')
                Row({space:8}){
                  TextInput({text:draft.hour,placeholder:'08'}).type(InputType.Number).maxLength(2).fontSize(this.readableSize(27)).fontColor(INK).textAlign(TextAlign.Center).height(56).layoutWeight(1).backgroundColor(BG).borderRadius(12).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onChange((v:string)=>{this.updateMedicineDraft(draft.slot,undefined,v);})
                  Text(':').fontSize(27).fontColor(INK)
                  TextInput({text:draft.minute,placeholder:'00'}).type(InputType.Number).maxLength(2).fontSize(this.readableSize(27)).fontColor(INK).textAlign(TextAlign.Center).height(56).layoutWeight(1).backgroundColor(BG).borderRadius(12).enabled(!this.livingBusy&&this.livingFresh()&&this.allowed('control')).onChange((v:string)=>{this.updateMedicineDraft(draft.slot,undefined,undefined,v);})
                }.width('100%')
                ForEach(medicineSlots(this.living).filter((slot:MedicineSlot)=>slot.slot===draft.slot),(slot:MedicineSlot)=>{this.doseActions(slot)},(slot:MedicineSlot)=>slot.slot+':'+slot.state+':'+this.livingFresh())
              }.width('100%').padding({top:12,bottom:14}).border({width:{bottom:0.5},color:CareTheme.line})
            },(draft:MedicineDraft)=>draft.slot+':'+draft.enabled)
            if(!this.living){Text('等待设备生活提醒状态').fontSize(14).fontColor(AMBER)}
            if(this.living&&this.living.schema===1){Text('旧版设备仅支持一个时段，已保留兼容。').fontSize(13).fontColor(AMBER)}
            Text('确认仅记录用户操作，不能证明已实际服药。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
          }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
          if(this.livingMessage){Text(this.livingMessage).fontSize(this.readableSize(14)).fontColor(AMBER).width('100%')}
          Button(this.livingBusy?'等待设备确认…':'保存提醒设置',{type:ButtonType.Normal}).fontSize(this.readableSize(16)).width('100%').constraintSize({minHeight:48}).borderRadius(12).backgroundColor(PRIMARY).enabled(this.livingFresh()&&!this.livingBusy&&!this.commandBusy&&this.allowed('control')).onClick(()=>{this.saveLiving();})
          Column({space:12}) {
            Text('服药历史').fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
            if(this.medicineHistoryMessage){Text(this.medicineHistoryMessage).fontSize(13).fontColor(SUB).width('100%')}
            if(!this.medicineHistory.length&&!this.medicineHistoryMessage){Text('暂无已上报的服药记录').fontSize(14).fontColor(SUB).width('100%')}
            ForEach(this.medicineHistory.slice(0,12),(record:MedicineRecord)=>{Text(new Date(record.dose_day*86400000).toISOString().slice(0,10)+' · 第 '+(record.slot+1)+' 时段 '+medicineTime(record.minute)+' · '+(record.state===4?'已服药':'未服药')).fontSize(14).fontColor(record.state===4?PRIMARY:AMBER).width('100%')},(record:MedicineRecord)=>record.dose_day+':'+record.slot+':'+record.medicine_revision+':'+record.state)
          }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
        }.padding({left:20,right:20,bottom:24}).width('100%')
      }.layoutWeight(1).align(Alignment.Top).width('100%').scrollBar(BarState.Off)
    }.width('100%').height('100%').backgroundColor(BG)
  }
  @Builder
  medicineSheet() {
    Column({space:18}) {
      this.icon($r('app.media.bell'),40)
      Text('第 '+(this.medicineNoticeSlot+1)+' 时段服药提醒').fontSize(this.readableSize(24)).fontWeight(FontWeight.Bold).fontColor(INK)
      Text(this.demo?'演示提醒，不操作真实设备':'已到每日提醒时间，请根据实际情况确认。').fontSize(this.readableSize(14)).fontColor(SUB)
      Text('确认仅记录一次用户操作，无法证明已经实际服药。').fontSize(this.readableSize(13)).fontColor(SUB)
      if(this.livingMessage){Text(this.livingMessage).fontSize(this.readableSize(13)).fontColor(AMBER)}
      Button(this.livingBusy?'等待设备回执…':'确认已服药').width('100%').height(this.elderMode ? 60 : 48).backgroundColor(PRIMARY).enabled(!this.livingBusy&&!this.commandBusy&&this.livingFresh()&&this.allowed('control')).onClick(()=>{this.confirmMedicine();})
      Button('稍后处理').width('100%').height(this.elderMode ? 60 : 42).backgroundColor(CareTheme.softBlue).fontColor(PRIMARY).enabled(!this.livingBusy).onClick(()=>{this.medicineNotice=false;})
    }.padding(24).width('90%').backgroundColor(Color.White).borderRadius(16)
  }
  @Builder
  controls() {
    Row({ space: 12 }) {
      Button(this.commandBusy ? '操作进行中…' : '确认设备报警').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(Color.White).backgroundColor(PRIMARY).height(this.elderMode ? 60 : 48).layoutWeight(1).borderRadius(14).enabled(!this.commandBusy&&this.allowed('control')).onClick(() => { this.command('ack'); })
      Button(this.healthFresh() && this.muted ? '设备已消音' : '设备消音').fontSize(this.readableSize(14)).fontColor(INK).backgroundColor('#E7EDF0').height(this.elderMode ? 60 : 48).layoutWeight(1).borderRadius(14).enabled(!this.commandBusy&&this.allowed('control')).onClick(() => { this.command('mute'); })
    }.width('100%')
    if (!this.demo && (!this.online || this.stale())) { Text('设备离线或上报过期，远程操作可能无法送达，请联系现场人员。').fontSize(this.readableSize(13)).fontColor(AMBER).lineHeight(this.readableLine(20)).width('100%').margin({top:9}) }
    Text('消音只停止提示声音；确认设备报警不等于完成现场处置，也不会自动保存处理记录。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(19)).width('100%').margin({top:9})
    if (this.commandMessage) { Text(this.commandMessage).fontSize(this.readableSize(13)).fontColor(SUB).margin({ top: 9 }).width('100%') }
  }
  @Builder
  chart(large: boolean) {
    Column({ space: 14 }) {
      Row() { Column({ space: 4 }) { Text(this.activityTotal.toString()).fontSize(this.readableSize(32)).fontWeight(FontWeight.Bold).fontColor(INK); Text('已观察活动次数').fontSize(this.readableSize(13)).fontColor(SUB) }.alignItems(HorizontalAlign.Start); Blank(); this.pill({ text: this.demo ? '演示曲线' : '仅观察时段', color: PRIMARY, background: CareTheme.softBlue }) }.width('100%')
      if (this.hours.length) {
        Row({ space: 3 }) { ForEach(this.hours, (value: number, index: number) => { Column() { Blank(); Rect().fill(index === Math.floor((this.now / 3600000 + 8) % 24) ? PRIMARY : '#8EADD0').width('100%').height(Math.max(0, value / this.chartMax() * (large ? 150 : 85))).radius(3) }.height(large ? 160 : 96).layoutWeight(1) }, (value: number, index: number) => `${index}:${value}`) }.width('100%').alignItems(VerticalAlign.Bottom)
        Row() { Text('00:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('06:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('12:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('18:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('23:00').fontSize(this.readableSize(9)).fontColor(SUB) }.width('100%')
      } else { Text('暂无可用统计，请等待设备上报').fontSize(this.readableSize(13)).fontColor(SUB).padding({ top: 30, bottom: 30 }) }
    }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
  }
  @Builder
  alarmGroup(active:boolean) {
    Column() {
      ForEach(this.alarms.filter((item:AlarmEvent)=>this.eventActive(item)===active),(item:AlarmEvent)=>{
        Column({space:10}) {
          Row({space:10}) {this.icon(active?$r('app.media.warning'):$r('app.media.bell'),22);Text(alarmTitle(item.code)).fontSize(this.readableSize(active?18:16)).fontWeight(FontWeight.Bold).fontColor(INK).layoutWeight(1);if(!active){Text(this.eventLabel(item)).fontSize(this.readableSize(13)).fontColor(SUB).constraintSize({maxWidth:'42%'})}}
          Text(alarmTime(item)).fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
          if(active){
            Text(this.eventLabel(item)).fontSize(this.readableSize(14)).fontColor(RED).width('100%')
            Row({space:10}) {
              Button('查看详情', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(Color.White).backgroundColor(PRIMARY).constraintSize({minHeight:48}).borderRadius(12).layoutWeight(1).onClick(()=>{this.showAlarm(item);})
              Button('记录处理', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(Color.White).border({width:0.7,color:PRIMARY}).constraintSize({minHeight:48}).borderRadius(12).layoutWeight(1).onClick(()=>{this.showAlarm(item);})
            }.width('100%')
          }else {Text('查看详情与处置记录 ›').fontSize(this.readableSize(13)).fontColor(PRIMARY).width('100%')}
        }.padding(16).width('100%').backgroundColor(Color.White).borderRadius(16).border({width:{left:active?3:0},color:RED}).margin({bottom:12}).onClick(()=>{if(!active){this.showAlarm(item);}})
      },(item:AlarmEvent)=>alarmKey(item)+':'+item.phase+':'+this.eventLabel(item))
    }.width('100%')
  }
  @Builder
  alarmsPage() {
    Column() {
      this.brandHeader()
      this.pageTitle('报警','先核实，再安心')
      Row({space:20}) {
        Button('正在报警 '+this.pendingCount(), {type:ButtonType.Normal}).fontSize(this.readableSize(15)).fontWeight(FontWeight.Medium).fontColor(this.alarmFilter===1?PRIMARY:SUB).backgroundColor(Color.Transparent).constraintSize({minHeight:48}).border({width:{bottom:this.alarmFilter===1?2:0},color:PRIMARY}).borderRadius(0).onClick(()=>{this.alarmFilter=1;})
        Button('全部记录', {type:ButtonType.Normal}).fontSize(this.readableSize(15)).fontColor(this.alarmFilter===0?PRIMARY:SUB).backgroundColor(Color.Transparent).constraintSize({minHeight:48}).border({width:{bottom:this.alarmFilter===0?2:0},color:PRIMARY}).borderRadius(0).onClick(()=>{this.alarmFilter=0;})
        Blank()
        Button({type:ButtonType.Normal}){this.icon($r('app.media.refresh'),20)}.width(44).height(44).backgroundColor(Color.Transparent).onClick(()=>{this.refresh(true);})
      }.width('100%').margin({bottom:16})
      if(this.pendingCount()>0){Text(this.pendingCount()+' 条异常需要核实').fontSize(this.readableSize(14)).fontColor(RED).padding(12).backgroundColor(CareTheme.alarmSurface).borderRadius(10).width('100%').margin({bottom:12})}
      if(!this.alarms.length){this.empty('暂无报警记录','这里将保留收到的异常与处置记录。没有记录不等于设备已确认安全。')}
      else if(this.alarmFilter===1 && this.pendingCount()===0){this.empty('当前没有待核实报警','可切换全部记录，查看历史事件和处置说明。')}
      this.alarmGroup(true)
      if(this.alarmFilter===0 && this.alarms.length>this.pendingCount()){
        this.section({title:'最近记录',subtitle:'保留每一次响应'})
        this.alarmGroup(false)
      }
      this.section({title:'设备操作',subtitle:'以执行回执为准'})
      Column({space:12}){this.controls()}.padding(16).backgroundColor(Color.White).borderRadius(16).width('100%')
      Text('更多操作').fontSize(this.readableSize(14)).fontColor(SUB).width('100%').padding({top:22,bottom:10}).onClick(()=>{this.batchActionsOpen=!this.batchActionsOpen;})
      if(this.batchActionsOpen){this.careActions()}
    }.width('100%')
  }
  @Builder
  trendsPage() {
    Column() {
      this.brandHeader()
      this.pageTitle('活动趋势', '用数据，了解日常的节奏')
      Row({ space: 12 }) { this.summary({ label: '活动次数', value: this.activityTotal.toString() }); this.summary({ label: '报警次数', value: this.alarmTotal.toString() }); this.summary({ label: '记录天数', value: this.days.length.toString() }) }.width('100%')
      Column(){this.entryRow({icon:$r('app.media.chart'),title:'活动报告',value:'昨日对比 · 周报 · 月报',action:'reports'})}.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%').margin({top:16})
      this.section({ title: '每日活动分布', subtitle: this.days.length ? this.days[this.chartDay].date : '暂无日期' })
      if (this.days.length > 1) {
        Scroll() { Row({space:8}) { ForEach(this.daysDesc(),(day:DayStats)=>{ Button(day.date.substring(5)).fontSize(this.readableSize(13)).backgroundColor(this.isSelectedDay(day)?INK:'#E6EDF0').fontColor(this.isSelectedDay(day)?'#FFFFFF':SUB).onClick(()=>{this.selectDay(day);}) },(day:DayStats)=>`${day.date}:${day.activity_total}:${day.alarm_total}`) } }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).margin({bottom:12})
      }
      this.chart(true)
      Column({space:9}) { Text('如何理解这张图').fontSize(this.readableSize(14)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%'); Text('活动次数是人体感应从无活动变为有活动的次数，并非步数。设备关机或未覆盖的时段，不代表老人没有活动。').fontSize(this.readableSize(13)).lineHeight(this.readableLine(21)).fontColor(SUB); Text(this.demo?'当前为演示曲线，仅用于展示界面。':'仅展示设备实际保存的统计；未对时或未收到消息时，不生成虚假历史。').fontSize(this.readableSize(13)).lineHeight(this.readableLine(19)).fontColor(AMBER) }.padding(18).backgroundColor('#EDF2F3').borderRadius(16).margin({top:16})
    }.width('100%')
  }
  @Builder
  activityReports() {
    Column(){
      this.section({title:'活动观察报告',subtitle:'北京时间 · 已结束日期'})
      if(this.activityReport) {
        Column({space:10}) {
          Text('昨日：'+this.activityReport.comparison.date).fontSize(this.readableSize(15)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
          Text(this.activityReport.comparison.change_percent===null?'缺少基线或基线为零，暂不能计算变化':('较前日已观察次数变化 '+this.activityReport.comparison.change_percent+'%')).fontSize(this.readableSize(14)).fontColor(PRIMARY).width('100%')
          Text(this.activityReport.comparison.note).fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
        this.periodReport('近 7 日周报',this.activityReport.week)
        this.periodReport('近 30 日月报',this.activityReport.month)
        Text(this.activityReport.advice).fontSize(this.readableSize(13)).fontColor(SUB).padding(12)
      } else {
        Text(this.demo?'演示模式不生成虚构报告':this.reportMessage||'等待统计报告；需服务器提供 reports 接口和实际活动记录').fontSize(this.readableSize(14)).fontColor(AMBER).width('100%').margin({bottom:12})
        ForEach(['昨日活动对比','近 7 日周报','近 30 日月报'],(title:string)=>{
          Column({space:10}){Text(title).fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%');Text('暂无可用统计，收到真实数据后展示；不把缺失记录计为零活动。').fontSize(this.readableSize(14)).fontColor(SUB).width('100%').lineHeight(23)}.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%').margin({bottom:12})
        },(title:string)=>title)
      }
      Button('刷新报告',{type:ButtonType.Normal}).fontSize(16).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:48}).width('100%').borderRadius(12).enabled(!this.loading).onClick(()=>{this.refresh(true);})
    }.width('100%')
  }
  @Builder
  periodReport(title:string,period:PeriodReport) {
    Column({space:8}) {
      Text(title).fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      Text(period.from_date+' 至 '+period.to_date).fontSize(13).fontColor(SUB).width('100%')
      Text('有记录 '+period.observed_days+'/'+period.calendar_days+' 天 · 活动 '+period.activity_total+' 次 · 报警 '+period.alarm_total+' 次').fontSize(this.readableSize(13)).fontColor(INK).width('100%')
      Text(period.peak_hour===null?'暂无活动时段数据':'观察次数最多时段：'+period.peak_hour+':00–'+(period.peak_hour+1)+':00').fontSize(13).fontColor(SUB).width('100%')
    }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%').margin({top:12})
  }
  @Builder
  summary($$: SummaryView) { Column({space:7}) { Text($$.value).fontSize(this.readableSize(25)).fontWeight(FontWeight.Bold).fontColor(INK); Text($$.label).fontSize(this.readableSize(13)).fontColor(SUB) }.padding({top:18,bottom:18}).layoutWeight(1).backgroundColor(Color.White).borderRadius(16) }
  @Builder
  pageTitle(title: string, subtitle: string) { Column({space:7}) { Text(title).fontSize(this.readableSize(28)).fontWeight(FontWeight.Bold).fontColor(INK); Text(subtitle).fontSize(this.readableSize(13)).fontColor(SUB) }.alignItems(HorizontalAlign.Start).width('100%').margin({top:16,bottom:24}) }
  @Builder
  empty(title: string, text: string) { Column({space:12}) { this.icon($r('app.media.shield'),40); Text(title).fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK); Text(text).fontSize(this.readableSize(13)).fontColor(SUB).textAlign(TextAlign.Center).lineHeight(this.readableLine(20)) }.padding(28).width('100%').backgroundColor(Color.White).borderRadius(16) }
  @Builder
  info($$: InfoView) { Row() { Text($$.label).fontSize(this.readableSize(13)).fontColor(SUB); Blank(); Text($$.value).fontSize(this.readableSize(13)).fontColor($$.color).textAlign(TextAlign.End).layoutWeight(1).margin({left:12}) }.width('100%').padding({top:13,bottom:13}).border({width:{bottom:0.5},color:'#EEF2F3'}) }
  @Builder
  doorStateCard() {
    Column({space:14}) {
      Row({space:14}) {
        Stack(){ Rect().fill(CareTheme.softBlue).width(52).height(52).radius(16); this.icon($r('app.media.door'),29) }
        Column({space:6}) {
          Text(this.doorStateTitle()).fontSize(this.readableSize(21)).fontWeight(FontWeight.Bold).fontColor(INK).maxLines(1).textOverflow({overflow:TextOverflow.Ellipsis}).width('100%')
          Text(this.roomAge()).fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        this.pill({text:this.roomOnline(),color:this.roomOnline()==='近期在线'||this.roomOnline()==='演示'?PRIMARY:AMBER,background:this.roomOnline()==='近期在线'||this.roomOnline()==='演示'?CareTheme.softBlue:'#FBF1E6'})
      }.width('100%')
      Divider().color(CareTheme.line).strokeWidth(0.5)
      Text(this.doorHint()).fontSize(this.readableSize(13)).fontColor(this.doorHintTone()).lineHeight(this.readableLine(21)).width('100%')
    }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
  }
  @Builder
  roomCell(icon: Resource, label: string, value: string, unit: string) {
    Column({space:7}) {
      Row({space:7}) {this.icon(icon,19);Text(label).fontSize(this.readableSize(13)).fontColor(SUB)}
      Row({space:4}) {
        Text(value).fontSize(this.readableSize(22)).fontWeight(FontWeight.Bold).fontColor(INK).maxLines(1).minFontSize(14).maxFontSize(this.readableSize(22)).layoutWeight(1)
        Text(unit).fontSize(this.readableSize(12)).fontColor(SUB).margin({bottom:3})
      }.width('100%').alignItems(VerticalAlign.Bottom)
    }.padding({left:14,right:10,top:14,bottom:14}).layoutWeight(1).alignItems(HorizontalAlign.Start)
  }
  @Builder
  roomReadings() {
    Column() {
      Row() {
        this.roomCell($r('app.media.sun'),'光照采样',this.roomLight(),'mV')
        Divider().vertical(true).height(62).color(CareTheme.line).strokeWidth(0.5)
        this.roomCell($r('app.media.flame'),'烟雾采样',this.roomSmoke(),'mV')
      }.width('100%')
      Divider().color(CareTheme.line).strokeWidth(0.5).width('90%')
      Row() {
        this.roomCell($r('app.media.activity'),'人体活动',this.roomMotion(),'PIR')
        Divider().vertical(true).height(62).color(CareTheme.line).strokeWidth(0.5)
        this.roomCell($r('app.media.home'),'房门传感器',this.roomDoorShort(),'磁簧')
      }.width('100%')
    }.width('100%').backgroundColor(Color.White).borderRadius(16)
  }
  @Builder
  doorRecordsCard() {
    Column() {
      if(this.doorItems().length){
        ForEach(this.doorItems(),(item:DoorRecord)=>{
          Row({space:10}) {
            Text(item.state==='OPEN'?'开启':'关闭').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(item.state==='OPEN'?AMBER:PRIMARY).width(42)
            Text('由'+(item.previous==='OPEN'?'开启':'关闭')+'变为'+(item.state==='OPEN'?'开启':'关闭')).fontSize(this.readableSize(14)).fontColor(INK).layoutWeight(1)
            Text(this.doorRecordTime(item.observed_at)).fontSize(this.readableSize(13)).fontColor(SUB)
          }.width('100%').padding({top:14,bottom:14}).border({width:{bottom:0.5},color:CareTheme.line})
        },(item:DoorRecord)=>''+item.observed_at)
      } else {
        Text(this.doorRecords?'等待首次实际开关变化，初始状态不计为开关事件':(this.demo?'演示模式没有真实记录，连接设备后自动同步':(this.doorRecordMessage||'正在同步门窗记录…'))).fontSize(this.readableSize(14)).fontColor(SUB).lineHeight(this.readableLine(22)).width('100%').padding({top:16,bottom:16})
      }
    }.padding({left:18,right:18}).width('100%').backgroundColor(Color.White).borderRadius(16)
  }
  @Builder
  doorDetail() {
    Column({space:16}) {
      this.doorStateCard()
      this.section({title:'房间读数',subtitle:this.roomStampCaption()})
      this.roomReadings()
      Text('光照与烟雾为传感器原始电压（mV）采样，不等同于照度或烟雾浓度。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(19)).width('100%')
      this.section({title:'出入记录',subtitle:this.doorRecordCaption()})
      this.doorRecordsCard()
      Text('磁簧记录只反映门体开合，不能代替现场核实。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(21)).width('100%')
    }.width('100%')
  }
  @Builder
  activityDetail() {
    Column({space:16}) {
      Row({space:12}) {
        this.summary({label:'活动次数',value:this.activityTotal.toString()})
        this.summary({label:'报警次数',value:this.alarmTotal.toString()})
        this.summary({label:'记录天数',value:this.days.length.toString()})
      }.width('100%')
      Text(this.lastActivity()).fontSize(this.readableSize(14)).fontColor(INK).width('100%')
      this.section({title:'每日活动分布',subtitle:this.days.length?this.days[this.chartDay].date:'暂无日期'})
      if(this.days.length>1){
        Scroll(){ Row({space:8}){ ForEach(this.daysDesc(),(day:DayStats)=>{ Button(day.date.substring(5)).fontSize(this.readableSize(13)).backgroundColor(this.isSelectedDay(day)?INK:'#E6EDF0').fontColor(this.isSelectedDay(day)?'#FFFFFF':SUB).onClick(()=>{this.selectDay(day);}) },(day:DayStats)=>`${day.date}:${day.activity_total}:${day.alarm_total}`) } }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).width('100%')
      }
      this.chart(true)
      this.activityReports()
      Column({space:9}) { Text('如何理解这些数据').fontSize(this.readableSize(14)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%'); Text('活动次数是人体感应从无活动变为有活动的次数，并非步数。设备关机或未覆盖的时段，不代表老人没有活动。').fontSize(this.readableSize(13)).lineHeight(this.readableLine(21)).fontColor(SUB); Text(this.demo?'当前为演示曲线，仅用于展示界面。':'仅展示设备实际保存的统计；未对时或未收到消息时，不生成虚假历史。').fontSize(this.readableSize(13)).lineHeight(this.readableLine(19)).fontColor(AMBER) }.padding(18).backgroundColor('#EDF2F3').borderRadius(16).width('100%')
    }.width('100%')
  }
  @Builder
  devicePage() {
    Column() {
      this.brandHeader()
      this.pageTitle('家庭设备','各自守护，清楚看见')
      Column({space:12}) {
        Row({space:12}) {this.icon($r('app.media.device'),27);Column({space:4}){Text('居家安全主机').fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK);Text('通晓 TX-SMART-R').fontSize(this.readableSize(13)).fontColor(SUB)}.layoutWeight(1).alignItems(HorizontalAlign.Start);Text(this.demo?'演示':this.online&&!this.stale()?'近期在线':'状态待确认').fontSize(this.readableSize(13)).fontColor(this.demo?SUB:this.online&&!this.stale()?PRIMARY:AMBER)}.width('100%')
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.info({label:'最近上报',value:this.age(),color:SUB})
        Text('温湿度 · 人体活动 · 烟气估算 · 夜灯与服药提醒').fontSize(this.readableSize(14)).fontColor(INK).width('100%')
        Text('外接供电').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
      }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
      this.section({title:'房间与随身设备',subtitle:this.familyRole==='owner'?'家庭管理员':this.familyRole==='viewer'?'只读成员':this.familyRole?'照护成员':''})
      Column() {
        ForEach(this.familyNodes.filter((node:DistributedNode)=>node.kind!=='gateway'),(node:DistributedNode)=>{
          Column({space:8}) {
            Row({space:10}) {this.icon(node.kind==='wearable'?$r('app.media.activity'):$r('app.media.home'),24);Text(node.name).fontSize(this.readableSize(17)).fontWeight(FontWeight.Medium).fontColor(INK).layoutWeight(1);Text(node.room).fontSize(this.readableSize(13)).fontColor(SUB)}.width('100%')
            Text(node.online&&readingFresh(node.updated_at||node.watch?.received_at,this.now)?'近期在线':'离线或待更新').fontSize(this.readableSize(13)).fontColor(node.online&&readingFresh(node.updated_at||node.watch?.received_at,this.now)?PRIMARY:AMBER).width('100%')
            if(node.kind==='wearable'){
              Text(node.watch?'心率 '+heartText(node.watch)+'  ·  血氧 '+watchVital(node.watch.spo2,'%',100):'等待手表上报').fontSize(this.readableSize(14)).fontColor(INK).width('100%')
              Text(node.watch&&node.watch.battery_percent!==null&&node.watch.battery_percent!==undefined?'电量 '+node.watch.battery_percent+'%':'电量暂无数据').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
            }else {
              Text(node.properties?'人体 '+motionText(node.properties.body)+'  ·  '+roomDoorText(node.properties.door):'等待环境节点上报').fontSize(this.readableSize(14)).fontColor(INK).width('100%')
              Text('外接供电').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
            }
            Text(node.updated_at||node.watch?.received_at||'尚未收到有效上报').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
          }.padding({top:16,bottom:16}).width('100%').border({width:{bottom:0.5},color:CareTheme.line})
        },(node:DistributedNode)=>node.device_id+JSON.stringify(node))
        this.entryRow({icon:$r('app.media.home'),title:'房间环境',value:'查看详情',action:'environment'})
        this.entryRow({icon:$r('app.media.activity'),title:'随身健康',value:'查看详情',action:'wearable'})
      }.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%')
      this.section({title:'其他设备',subtitle:''})
      Column(){this.entryRow({icon:$r('app.media.camera'),title:'家中画面',value:'连接后观看',action:'camera'})}.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%')
      this.section({title:'连接与诊断',subtitle:''})
      Column(){
        this.entryRow({icon:$r('app.media.device'),title:'连接设置',value:this.loggedIn?'管理':'登录',action:'connection'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.chart'),title:'技术诊断',value:'设备与网络',action:'diagnostics'})
      }.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%')
      if(this.error){Text(this.error).fontSize(this.readableSize(13)).fontColor(AMBER).width('100%').margin({top:12})}
    }.width('100%')
  }
  @Builder
  diagnostics() {
    Column({space:8}) {
      this.info({label:'数据环境',value:this.demo?'本地演示':this.simulated?'模拟后端':'真实设备',color:SUB})
      this.info({label:'服务连接',value:this.demo?'演示':this.pushOnline?'WebSocket 已连接':'前台轮询备用',color:SUB})
      this.info({label:'设备固件',value:this.firmware||'待上报',color:INK})
      this.info({label:'主机传输',value:!this.healthFresh()?'待更新':this.fwTls?'MQTT + TLS':'普通 MQTT',color:INK})
      this.info({label:'温湿度采样',value:!this.healthFresh()?'待确认':this.envValid?'有效':'不可用',color:!this.healthFresh()||!this.envValid?AMBER:PRIMARY})
      Text('STA：主板连接热点上云。AP：主板开启本地热点，通过浏览器查看。状态以设备实际报告为准。').fontSize(this.readableSize(14)).fontColor(SUB).width('100%').lineHeight(23)
      Text('固定主机不判断人体跌倒，跌倒监测来自独立手表。烟气为传感器估算，不能替代经认证的燃气报警器。').fontSize(this.readableSize(14)).fontColor(SUB).width('100%').lineHeight(23)
      Text('目前仅前台接警与本地通知；App 被关闭后的系统推送尚未接通。').fontSize(this.readableSize(14)).fontColor(AMBER).width('100%').lineHeight(23)
      Button(this.notificationsEnabled?'通知权限已开启':'开启系统通知权限', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).constraintSize({minHeight:48}).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).width('100%').borderRadius(12).onClick(async()=>{this.notificationsEnabled=await AlarmNotifications.enable(getContext(this) as common.UIAbilityContext);})
    }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
  }
  @Builder
  assessmentCard() {
    Column({space:12}) {
      Row() { Text('守护建议').fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); this.pill({text:this.assessment().level==='normal'?'监测正常':this.assessment().level==='alert'?'优先查看':'状态说明',color:this.assessmentColor(),background:CareTheme.softBlue}) }.width('100%')
      Text(this.assessment().title).fontSize(this.readableSize(18)).fontWeight(FontWeight.Medium).fontColor(this.assessmentColor()).width('100%')
      Text(this.assessment().advice).fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(21)).width('100%')
      Row() { Text('依据：报警 · 数据时效 · 传感器可用性').fontSize(this.readableSize(13)).fontColor(SUB); Blank(); Text('查看 ›').fontSize(this.readableSize(13)).fontColor(PRIMARY).onClick(()=>{this.navigate(this.assessment().level==='alert'?1:3);}) }.width('100%')
    }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(16).margin({top:16})
  }
  @Builder
  userPage() {
    Column() {
      this.brandHeader()
      this.pageTitle('我的','让每一份牵挂，都有回应')
      Row({space:14}) {
        Stack(){Circle().fill(CareTheme.softBlue).width(56).height(56);this.icon($r('app.media.user'),29)}
        Column({space:5}){Text(this.displayName).fontSize(this.readableSize(22)).fontWeight(FontWeight.Bold).fontColor(INK).maxLines(1);Text(this.accountLine()).fontSize(this.readableSize(13)).fontColor(SUB).maxLines(1)}.alignItems(HorizontalAlign.Start).layoutWeight(1)
        if(this.loggedIn&&!this.demo){
          Button('切换', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).borderRadius(10).constraintSize({minHeight:44,minWidth:58}).onClick(()=>{this.switchUser='';this.switchPassword='';this.switchMessage='';this.switchOpen=true;})
        }
      }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
      this.section({title:'关怀资料',subtitle:''})
      Column(){
        this.info({label:'关怀对象',value:this.careName,color:INK})
        this.info({label:'联系电话',value:this.contactPhone||'未填写',color:SUB})
        this.entryRow({icon:$r('app.media.user'),title:'守护资料',value:'编辑',action:'profile'})
      }.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%')
      this.section({title:'显示与提醒',subtitle:''})
      Column(){
        Row({space:12}){Column({space:4}){Text('老年人模式').fontSize(this.readableSize(16)).fontColor(INK);Text('放大文字与常用操作').fontSize(this.readableSize(13)).fontColor(SUB)}.layoutWeight(1).alignItems(HorizontalAlign.Start);Toggle({type:ToggleType.Switch,isOn:this.elderMode}).selectedColor(PRIMARY).enabled(!this.elderModeSaving).onChange((value:boolean)=>{this.setElderMode(value);})}.width('100%').padding({top:16,bottom:16})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        Row({space:12}){Column({space:4}){Text('前台报警弹窗').fontSize(this.readableSize(16)).fontColor(INK);Text(this.alarmReminders?'新报警及时提醒':'关闭弹窗，记录仍保留').fontSize(this.readableSize(13)).fontColor(SUB)}.layoutWeight(1).alignItems(HorizontalAlign.Start);Toggle({type:ToggleType.Switch,isOn:this.alarmReminders}).selectedColor(PRIMARY).onChange((value:boolean)=>{this.setReminders(value);})}.width('100%').padding({top:16,bottom:16})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        Row({space:12}){
          Column({space:4}){Text('语音播报').fontSize(this.readableSize(16)).fontColor(INK);Text(this.voiceOn?'报警和服药会用语音念出来':'只弹窗与振动，不出声').fontSize(this.readableSize(13)).fontColor(SUB)}.layoutWeight(1).alignItems(HorizontalAlign.Start)
          Toggle({type:ToggleType.Switch,isOn:this.voiceOn}).selectedColor(PRIMARY).onChange((value:boolean)=>{this.setVoice(value);})
        }.width('100%').padding({top:16,bottom:16})
        if(this.voiceMessage){Text(this.voiceMessage).fontSize(this.readableSize(13)).fontColor(SUB).width('100%').padding({bottom:12}).lineHeight(this.readableLine(19))}
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.pill'),title:'生活提醒',value:'夜灯与服药',action:'living'})
        if(this.elderModeMessage){Text(this.elderModeMessage).fontSize(this.readableSize(13)).fontColor(SUB).width('100%').padding({bottom:12})}
      }.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%')
      this.section({title:'账户与帮助',subtitle:''})
      Column(){
        this.entryRow({icon:$r('app.media.home'),title:'家人消息',value:'好友与聊天',action:'messages'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.home'),title:'家庭与账号',value:'邀请 · 登录设备 · 改密码',action:'accounts'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.shield'),title:'无活动远程设置',value:'阈值与睡眠时段',action:'remotecare'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.device'),title:'主板本地控制',value:'无活动与配对',action:'localcontrol'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.device'),title:'NFC与家人聊天',value:'文字与回复',action:'devicecontact'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.home'),title:'家庭成员',value:this.familyRole==='owner'?'权限与邀请':'成员与权限',action:'members'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.device'),title:'账户与连接',value:this.loggedIn?'管理':'登录',action:'connection'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.entryRow({icon:$r('app.media.shield'),title:'守护说明',value:'了解监测边界',action:'help'})
        Divider().color(CareTheme.line).strokeWidth(0.5)
        this.info({label:'应用版本',value:'颐鸿安 '+(this.appVersion||'1.13.8'),color:SUB})
      }.padding({left:16,right:16}).backgroundColor(Color.White).borderRadius(16).width('100%')
      if(this.demo){Button('体验报警提醒', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:48}).borderRadius(12).width('100%').margin({top:20}).onClick(()=>{this.previewAlert();})}
      if(this.loggedIn){Button('退出登录', {type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(SUB).backgroundColor(Color.Transparent).constraintSize({minHeight:48}).width('100%').margin({top:20}).onClick(()=>{this.disconnect();})}
      Text('颐鸿安 · 把关心，放在日常里').fontSize(this.readableSize(13)).fontColor(SUB).margin({top:20,bottom:12})
    }.width('100%')
  }
  @Builder
  voicePage() {
    Column({space:14}) {
      Column({space:8}){
        Row({space:8}){
          Text('唤醒词').fontSize(this.readableSize(13)).fontColor(SUB)
          Text('小颐').fontSize(this.readableSize(20)).fontWeight(FontWeight.Bold).fontColor(PRIMARY)
        }.width('100%')
        Text('先喊“小颐”，再说要做什么。比如“小颐，开灯”“小颐，救命”。开灯、关灯、自动夜灯、求助、确认服药、报温湿度、看报警、看趋势、看设备、打开摄像头都能说。').fontSize(this.readableSize(14)).fontColor(INK).width('100%').lineHeight(this.readableLine(24))
      }.width('100%').padding(16).backgroundColor(Color.White).borderRadius(16).alignItems(HorizontalAlign.Start)
      Button(this.voiceBusy?'正在听，点一下结束':'开始说话',{type:ButtonType.Normal}).height(this.elderMode?64:54).width('100%').borderRadius(14).fontSize(this.readableSize(17)).backgroundColor(this.voiceBusy?AMBER:PRIMARY).fontColor(Color.White).onClick(()=>{this.listenVoice();})
      Text(this.voiceStatus).fontSize(this.readableSize(14)).fontColor(SUB).width('100%').lineHeight(this.readableLine(21))
      if(this.voiceHeard){Text('听到：'+this.voiceHeard).fontSize(this.readableSize(15)).fontColor(INK).width('100%').lineHeight(this.readableLine(22))}
      if(this.voiceReply){
        Row({space:10}){
          Text('小颐').fontSize(this.readableSize(13)).fontColor(PRIMARY).padding({left:8,right:8,top:3,bottom:3}).backgroundColor(CareTheme.softBlue).borderRadius(8)
          Text(this.voiceReply).fontSize(this.readableSize(15)).fontColor(INK).layoutWeight(1).lineHeight(this.readableLine(22))
        }.width('100%').padding(14).backgroundColor(Color.White).borderRadius(14).alignItems(VerticalAlign.Top)
      }
      Column({space:10}){
        Text('不方便说话时，打字一样有效').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%')
        TextInput({text:this.voiceTyped,placeholder:'例如：小颐开灯'}).height(this.elderMode?60:48).fontSize(this.readableSize(15)).backgroundColor(Color.White).borderRadius(12).onChange((value:string)=>{this.voiceTyped=value;})
        Button('执行这句',{type:ButtonType.Normal}).height(this.elderMode?58:46).width('100%').borderRadius(12).fontSize(this.readableSize(15)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).onClick(()=>{const line=this.voiceTyped;this.voiceTyped='';this.runVoiceText(line,false);})
      }.width('100%').alignItems(HorizontalAlign.Start)
      Text('可以这样说').fontSize(this.readableSize(15)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%').margin({top:4})
      Flex({wrap:FlexWrap.Wrap}){
        ForEach(VoiceCommand.exampleList(),(line:string)=>{
          Text(line).fontSize(this.readableSize(13)).fontColor(PRIMARY).padding({left:12,right:12,top:8,bottom:8}).backgroundColor(Color.White).borderRadius(10).margin({right:8,bottom:8}).onClick(()=>{this.runVoiceText(line,false);})
        },(line:string)=>line)
      }.width('100%')
      Text('语音识别在离线引擎可用时工作；识别不准时可以重说或直接打字。控制类指令仍受账号权限与设备在线状态限制。').fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(19))
    }.width('100%')
  }
  @Builder
  incidentAccessPage() {
    Column({space:16}){
      this.brandHeader()
      Text('协同守护').fontSize(this.readableSize(27)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      Text(this.memberRoleLabel(this.familyRole)).fontSize(this.readableSize(16)).fontColor(PRIMARY).width('100%')
      Text(this.memberRoleNote(this.familyRole)).fontSize(this.readableSize(14)).fontColor(SUB).width('100%').lineHeight(this.readableLine(22))
      Text(this.pushOnline?'前台实时连接已建立':'正在通过轮询同步报警').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
      Button('查看报警与处置记录').width('100%').constraintSize({minHeight:48}).backgroundColor(PRIMARY).borderRadius(12).onClick(()=>{this.navigate(1);})
      Button('查看账号与成员').width('100%').constraintSize({minHeight:48}).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).borderRadius(12).onClick(()=>{this.detail='members';this.refreshFamily();})
      if(this.error){Text(this.error).fontSize(this.readableSize(13)).fontColor(AMBER).width('100%')}
    }.width('100%')
  }
  @Builder
  familyMembers() {
    Column({space:14}) {
      if(this.loggedIn&&this.familyRole==='owner'){
        Text('邀请家人一起守护').fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
        TextInput({text:this.memberName,placeholder:'账号：3～32位字母、数字或下划线'}).fontSize(14).height(52).backgroundColor(Color.White).borderRadius(12).onChange((v:string)=>{this.memberName=v;})
        TextInput({text:this.memberPassword,placeholder:'初始密码：8～128位'}).type(InputType.Password).fontSize(14).height(52).backgroundColor(Color.White).borderRadius(12).onChange((v:string)=>{this.memberPassword=v;})
        Text('这位成员可以做什么').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        Row({space:10}){
          Button('只读成员',{type:ButtonType.Normal}).layoutWeight(1).height(46).borderRadius(12).fontSize(this.readableSize(15))
            .backgroundColor(this.memberRole==='viewer'?PRIMARY:Color.White).fontColor(this.memberRole==='viewer'?Color.White:SUB)
            .onClick(()=>{this.memberRole='viewer';})
          Button('照护成员',{type:ButtonType.Normal}).layoutWeight(1).height(46).borderRadius(12).fontSize(this.readableSize(15))
            .backgroundColor(this.memberRole==='caregiver'?PRIMARY:Color.White).fontColor(this.memberRole==='caregiver'?Color.White:SUB)
            .onClick(()=>{this.memberRole='caregiver';})
        }.width('100%')
        Row({space:10}){
          Button('社区网格员',{type:ButtonType.Normal}).layoutWeight(1).height(46).borderRadius(12).fontSize(this.readableSize(14)).backgroundColor(this.memberRole==='community'?PRIMARY:Color.White).fontColor(this.memberRole==='community'?Color.White:SUB).onClick(()=>{this.memberRole='community';})
          Button('物业值班人员',{type:ButtonType.Normal}).layoutWeight(1).height(46).borderRadius(12).fontSize(this.readableSize(14)).backgroundColor(this.memberRole==='property'?PRIMARY:Color.White).fontColor(this.memberRole==='property'?Color.White:SUB).onClick(()=>{this.memberRole='property';})
        }.width('100%')
        Text(this.memberRoleNote(this.memberRole)).fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(19))
        Button(this.memberBusy?'正在创建…':'创建家庭成员', {type:ButtonType.Normal}).enabled(!this.memberBusy).backgroundColor(PRIMARY).borderRadius(12).constraintSize({minHeight:48}).width('100%').onClick(()=>{this.addMember();})
        if(this.memberMessage){Text(this.memberMessage).fontSize(this.readableSize(13)).fontColor(this.memberMessage.startsWith('已创建')?PRIMARY:SUB).width('100%').lineHeight(this.readableLine(20))}
        Text('家庭管理员由家庭创建者持有，暂不能在这里再指派一位。').fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(19))
      }else{Text(this.loggedIn?'由家庭管理员邀请新成员；你的设备权限由家庭账号决定。':'登录家庭账号后，查看已授权设备与成员功能。').fontSize(this.readableSize(16)).fontColor(SUB).width('100%').lineHeight(26)}
      if(this.loggedIn){
        Row(){Text('家庭成员').fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK).layoutWeight(1);Text(this.members.length?this.members.length+' 个账号':'读取中…').fontSize(this.readableSize(13)).fontColor(SUB)}.width('100%').margin({top:6})
        if(!this.members.length){
          Text('暂时没有读取到成员列表。下拉或重新进入本页可刷新。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        }
        ForEach(this.members,(member:HouseholdMember)=>{
          Column({space:8}){
            Row({space:8}){
              Text(member.username).fontSize(this.readableSize(16)).fontWeight(FontWeight.Medium).fontColor(INK).layoutWeight(1)
              Text(this.memberRoleLabel(member.role)).fontSize(this.readableSize(12)).fontColor(PRIMARY).padding({left:8,right:8,top:3,bottom:3}).backgroundColor(CareTheme.softBlue).borderRadius(8)
            }.width('100%')
            Row({space:6}){
              if(member.username===this.accountName){Text('当前登录').fontSize(this.readableSize(12)).fontColor(AMBER)}
              if(member.username===this.memberCreated){Text('刚刚创建').fontSize(this.readableSize(12)).fontColor(PRIMARY)}
              Text(this.memberRoleNote(member.role)).fontSize(this.readableSize(12)).fontColor(SUB).layoutWeight(1)
            }.width('100%')
            if(member.username===this.accountName){
              Text(this.accountStatusText()+' · '+this.api.base).fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(18))
            }else if(this.switchUser===member.username){
              Column({space:8}){
                Text('输入 '+member.username+' 的密码后切换，密码不会被保存').fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(18))
                TextInput({text:this.switchPassword,placeholder:'该账号的密码'}).type(InputType.Password).height(this.elderMode?58:46).fontSize(this.readableSize(15)).backgroundColor(BG).borderRadius(12).onChange((v:string)=>{this.switchPassword=v;})
                Row({space:8}){
                  Button(this.switchBusy?'登录中…':'登录并切换',{type:ButtonType.Normal}).enabled(!this.switchBusy).layoutWeight(1).height(44).borderRadius(12).fontSize(this.readableSize(14)).backgroundColor(PRIMARY).onClick(()=>{this.signInSwitch();})
                  Button('取消',{type:ButtonType.Normal}).enabled(!this.switchBusy).height(44).borderRadius(12).fontSize(this.readableSize(14)).fontColor(SUB).backgroundColor(BG).onClick(()=>{this.switchUser='';this.switchPassword='';this.switchMessage='';})
                }.width('100%')
              }.width('100%')
            }else{
              Row({space:10}){
                Text('成员使用各自账号登录，密码不向其他成员展示').fontSize(this.readableSize(12)).fontColor(SUB).layoutWeight(1).lineHeight(this.readableLine(18))
              }.width('100%')
            }
            if(this.familyRole==='owner'&&member.role!=='owner'){
              Row({space:10}){
                Button('修改权限',{type:ButtonType.Normal}).enabled(!this.memberBusy).constraintSize({minHeight:44}).borderRadius(10).fontSize(this.readableSize(13)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).onClick(()=>{this.manageMember(member,false);})
                Button('删除账号',{type:ButtonType.Normal}).enabled(!this.memberBusy).constraintSize({minHeight:44}).borderRadius(10).fontSize(this.readableSize(13)).fontColor(RED).backgroundColor(CareTheme.alarmSurface).onClick(()=>{this.manageMember(member,true);})
              }.width('100%')
            }
          }.width('100%').padding(12).backgroundColor(Color.White).borderRadius(12).alignItems(HorizontalAlign.Start)
        },(member:HouseholdMember)=>member.username)
        if(this.localExtras().length){
          Text('本机保存的其他账号').fontSize(this.readableSize(15)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%').margin({top:4})
          ForEach(this.localExtras(),(entry:SavedAccount)=>{
            Row({space:10}){
              Column({space:4}){
                Text(entry.username).fontSize(this.readableSize(15)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%')
                Text(this.memberRoleLabel(entry.role)+' · 服务器成员列表里没有这个账号').fontSize(this.readableSize(12)).fontColor(AMBER).width('100%')
              }.layoutWeight(1).alignItems(HorizontalAlign.Start)
              Button(this.switchBusy?'切换中…':'切换',{type:ButtonType.Normal}).enabled(!this.switchBusy&&!this.loginBusy).height(38).borderRadius(10).fontSize(this.readableSize(13)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).onClick(()=>{this.switchAccount(entry);})
            }.width('100%').padding(12).backgroundColor(Color.White).borderRadius(12)
          },(entry:SavedAccount)=>entry.server+'|'+entry.username)
        }
        if(this.switchMessage){Text(this.switchMessage).fontSize(this.readableSize(13)).fontColor(this.switchMessage.startsWith('已切换')?PRIMARY:AMBER).width('100%').lineHeight(this.readableLine(20))}
        Column({space:6}){
          Text('在另一台手机登录').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%')
          Text('1. 安装同一版本颐鸿安 App。\n2. 服务器地址填 '+this.api.base+'。\n3. 用成员账号和初始密码登录，即可看到同一家庭的设备。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(21)).width('100%')
        }.width('100%').padding(14).backgroundColor(BG).borderRadius(12).alignItems(HorizontalAlign.Start)
        Button('添加其他账号',{type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:44}).width('100%').borderRadius(12).onClick(()=>{this.userDraft='';this.passwordDraft='';this.settingsOpen=true;})
      }
      Button('查看家庭设备', {type:ButtonType.Normal}).backgroundColor(CareTheme.softBlue).fontColor(PRIMARY).constraintSize({minHeight:48}).width('100%').borderRadius(12).onClick(()=>{this.detail='';this.navigate(3);})
    }.width('100%')
  }
  @Builder
  switchRowOf(name:string, role:string, server:string, localOnly:boolean) {
    Column({space:10}) {
      Row({space:10}) {
        Column({space:4}) {
          Text(name).fontSize(this.readableSize(16)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%')
          Text(this.memberRoleLabel(role)+(localOnly?' · 本机保存':'')).fontSize(this.readableSize(12)).fontColor(localOnly?AMBER:SUB).width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        if(name===this.accountName&&!localOnly){
          Text('当前使用').fontSize(this.readableSize(12)).fontColor(PRIMARY)
        }else if(this.switchUser!==name){
          Button('切换',{type:ButtonType.Normal}).enabled(!this.switchBusy&&!this.loginBusy).height(40).borderRadius(10).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).onClick(()=>{this.switchUser=name;this.switchServer=server;this.switchPassword='';this.switchMessage='';})
        }
      }.width('100%')
      if(this.switchUser===name&&!(name===this.accountName&&!localOnly)){
        TextInput({text:this.switchPassword,placeholder:'输入 '+name+' 的密码'}).type(InputType.Password).height(this.elderMode?56:46).fontSize(this.readableSize(15)).backgroundColor(BG).borderRadius(10).onChange((v:string)=>{this.switchPassword=v;})
        Row({space:8}){
          Button(this.switchBusy?'登录中…':'登录并切换',{type:ButtonType.Normal}).enabled(!this.switchBusy).layoutWeight(1).height(44).borderRadius(10).fontSize(this.readableSize(14)).backgroundColor(PRIMARY).onClick(()=>{this.signInSwitch();})
          Button('取消',{type:ButtonType.Normal}).enabled(!this.switchBusy).height(44).borderRadius(10).fontSize(this.readableSize(14)).fontColor(SUB).backgroundColor(BG).onClick(()=>{this.switchUser='';this.switchPassword='';})
        }.width('100%')
      }
    }.width('100%').padding(14).backgroundColor(Color.White).borderRadius(12).border({width:0.5,color:CareTheme.line})
  }
  @Builder
  accountSwitchSheet() {
    Scroll() {
      Column({space:12}) {
        Row() {
          Text('切换账号').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK)
          Blank()
          Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).constraintSize({minWidth:44,minHeight:44}).textAlign(TextAlign.End).onClick(()=>{if(!this.switchBusy){this.switchOpen=false;this.switchUser='';this.switchPassword='';}})
        }.width('100%')
        Text('每次切换都要输入该账号自己的密码，密码只用于本次登录，不保存在手机上。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%').lineHeight(this.readableLine(20))
        if(this.loggedIn){this.switchRowOf(this.accountName,this.knownRole(this.accountName),this.api.base,false)}
        if(this.members.filter((m:HouseholdMember)=>m.username!==this.accountName).length){
          Text('同一家庭的其他成员').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%').margin({top:4})
          ForEach(this.members.filter((m:HouseholdMember)=>m.username!==this.accountName),(m:HouseholdMember)=>{this.switchRowOf(m.username,m.role,this.api.base,false)},(m:HouseholdMember)=>m.username)
        }
        if(this.localExtras().length){
          Text('本机保存过的其他账号').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(INK).width('100%').margin({top:4})
          ForEach(this.localExtras(),(entry:SavedAccount)=>{this.switchRowOf(entry.username,entry.role,entry.server,true)},(entry:SavedAccount)=>entry.server+'|'+entry.username)
        }
        if(!this.loggedIn){Text('当前未登录。请先在“连接设置”里登录一个账号。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%').lineHeight(this.readableLine(20))}
        if(this.switchMessage){Text(this.switchMessage).fontSize(this.readableSize(13)).fontColor(this.switchMessage.startsWith('已切换')?PRIMARY:AMBER).width('100%').lineHeight(this.readableLine(20))}
        Button('添加其他账号',{type:ButtonType.Normal}).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:44}).width('100%').borderRadius(10).onClick(()=>{this.switchOpen=false;this.switchUser='';this.switchPassword='';this.userDraft='';this.passwordDraft='';this.settingsOpen=true;})
      }.padding(20).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(CareTheme.background).borderRadius(16).scrollBar(BarState.Off)
  }
  @Builder
  profileSheet() {
    Scroll() {
      Column({space:14}) {
        Row() { Text('守护资料').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).constraintSize({minWidth:44,minHeight:44}).textAlign(TextAlign.End).onClick(()=>{if(!this.profileSaving){this.profileOpen=false;}}) }.width('100%')
        Text('这些称呼与联系电话仅保存在本机，可随时修改。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(20)).width('100%')
        Text('你的昵称').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        TextInput({text:this.nameDraft,placeholder:'例如：小安'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).maxLength(20).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.nameDraft=value;})
        Text('关怀对象称呼').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        TextInput({text:this.careDraft,placeholder:'例如：家人'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).maxLength(20).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.careDraft=value;})
        Text('联系电话（可选）').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        TextInput({text:this.contactDraft,placeholder:'方便核实时联系'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).maxLength(24).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.contactDraft=value;})
        if(this.profileError){Text(this.profileError).fontSize(this.readableSize(13)).fontColor(RED).width('100%')}
        Button(this.profileSaving?'正在保存…':'保存资料').height(this.elderMode ? 60 : 48).width('100%').backgroundColor(PRIMARY).borderRadius(14).enabled(!this.profileSaving).onClick(()=>{this.saveProfile();})
      }.padding(24).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(Color.White).borderRadius(16).scrollBar(BarState.Off)
  }
  @Builder
  incomingSheet() {
    Column({space:18}) {
      Column({space:12}) {
        Stack() { Circle().fill(CareTheme.alarmSurface).width(60).height(60); this.icon($r('app.media.bell'),32) }
        Text(this.demo?'演示报警提醒':this.alertQueue.length&&this.alertQueue[0].epoch>0&&this.now-this.alertQueue[0].epoch*1000>120000?'报警记录待核实':'收到设备报警').fontSize(this.readableSize(13)).fontColor(RED)
        Text(this.alertQueue.length?alarmTitle(this.alertQueue[0].code):'设备报警').fontSize(this.readableSize(26)).fontWeight(FontWeight.Bold).fontColor(INK).textAlign(TextAlign.Center)
        Text(this.careName+'的居家终端，请及时查看并联系现场核实。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(22)).textAlign(TextAlign.Center)
        Text(this.alertQueue.length?alarmTime(this.alertQueue[0]):'').fontSize(this.readableSize(13)).fontColor(SUB)
        if(this.alertQueue.length>1){Text('还有 '+(this.alertQueue.length-1)+' 条提醒待查看').fontSize(this.readableSize(13)).fontColor(AMBER)}
      }.width('100%').onClick(()=>{this.openAlert();})
      Button('查看并处理').width('100%').height(this.elderMode ? 60 : 48).backgroundColor(RED).borderRadius(14).fontSize(this.readableSize(15)).onClick(()=>{this.openAlert();})
      Text('稍后查看').fontSize(this.readableSize(13)).fontColor(SUB).padding(8).onClick(()=>{this.alertQueue=this.alertQueue.slice(1);})
      Text(this.demo?'此提醒仅为本地演示，不操作真实设备':'查看提醒不会自动确认报警或消音').fontSize(this.readableSize(13)).fontColor(SUB).textAlign(TextAlign.Center)
    }.padding(26).width('90%').backgroundColor(Color.White).borderRadius(16)
  }
  private navIcon(index:number):Resource {
    if(index===0){return this.tab===index?$r('app.media.home_selected'):$r('app.media.home_idle');}
    if(index===1){return this.tab===index?$r('app.media.bell_selected'):$r('app.media.bell_idle');}
    if(index===2){return this.tab===index?$r('app.media.chart_selected'):$r('app.media.chart_idle');}
    if(index===3){return this.tab===index?$r('app.media.device_selected'):$r('app.media.device_idle');}
    return this.tab===index?$r('app.media.user_selected'):$r('app.media.user_idle');
  }
  @Builder
  navItem(index:number,icon:Resource,title:string) {
    Column({space:5}) {Image(this.navIcon(index)).width(24).height(24);Text(title).fontSize(12).fontWeight(this.tab===index?FontWeight.Bold:FontWeight.Normal).fontColor(this.tab===index?PRIMARY:SUB)}.constraintSize({minHeight:60}).justifyContent(FlexAlign.Center).layoutWeight(1).onClick(()=>{this.navigate(index);})
  }
  @Builder
  settingsSheet() {
    Scroll() {
    Column({space:14}) {
      Row() { Text('连接后端').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).constraintSize({minWidth:44,minHeight:44}).textAlign(TextAlign.End).onClick(()=>{this.settingsOpen=false;this.passwordDraft='';}) }.width('100%')
      Text('登录后查询真实设备；不会把云账号密钥放进 App。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%').lineHeight(this.readableLine(20))
      TextInput({text:this.serverDraft,placeholder:'https://47.116.171.114:8443'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.serverDraft=value;})
      Text('使用公网服务器地址').fontSize(this.readableSize(13)).fontColor(PRIMARY).width('100%').onClick(()=>{this.serverDraft='https://47.116.171.114:8443';})
      TextInput({text:this.userDraft,placeholder:'用户名'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.userDraft=value;})
      if (this.loggedIn) { Text('本次登录密码可点右侧眼睛查看；退出或重启后清除。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%') }
      TextInput({text:this.passwordDraft,placeholder:'后端登录密码'}).type(InputType.Password).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.passwordDraft=value;})
      if (this.error) { Text(this.error).fontSize(this.readableSize(13)).fontColor(RED).width('100%') }
      Button(this.loginBusy?'正在登录…':'登录并同步').width('100%').height(this.elderMode ? 60 : 48).backgroundColor(PRIMARY).borderRadius(14).enabled(!this.loginBusy&&this.passwordDraft.length>0).onClick(()=>{this.login();})
      Text('公网服务器使用上方 IP 地址；登录后同步真实设备。USB 演示可改填 https://127.0.0.1:18443，并运行电脑端演示连接脚本。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(19))
    }.padding(24).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(Color.White).borderRadius(16).scrollBar(BarState.Off)
  }
  @Builder
  alarmSheet() {
    Scroll() {
    Column({space:14}) {
      Row() { Text(this.selected?alarmTitle(this.selected.code):'事件详情').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).constraintSize({minWidth:44,minHeight:44}).textAlign(TextAlign.End).onClick(()=>{this.closeAlarm();}) }.width('100%')
      Text(this.selected?alarmTime(this.selected):'').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
      this.pill({text:this.selected?this.eventLabel(this.selected):'事件详情',color:this.selected&&this.eventActive(this.selected)?RED:PRIMARY,background:'#F4F8F7'})
      if (this.selected && this.selected.code === 4) { Text('设备上报长时间未检测到活动。可能与静坐、睡眠、离开感应范围或突发意外有关，不能据此确认跌倒。请先联系老人，必要时安排现场查看；远程消音不能代替核实。').fontSize(this.readableSize(13)).fontColor(AMBER).lineHeight(this.readableLine(22)).width('100%') }
      Text('当前终端操作：确认或消音不会删除这条事件记录。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%').lineHeight(this.readableLine(19))
      this.controls()
      Text('处理备注').fontSize(this.readableSize(15)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      if(this.selected?.claim){Text('正在处理：'+this.selected?.claim?.username).fontSize(this.readableSize(14)).fontColor(PRIMARY).width('100%')}
      if(this.allowed('alarms.handle')){
        Row({space:8}){
          Button(this.claimBusy?'正在同步…':this.selected?.claim?(this.selected?.claim?.username===this.accountName?'释放认领':'释放他人认领'):'我来处理').fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:44}).enabled(!this.claimBusy&&!this.noteSaving&&(!this.selected?.claim||this.selected?.claim?.username===this.accountName||this.familyRole==='owner')).onClick(()=>{this.claimAlarm(!!this.selected?.claim);})
          Text('认领与处理记录不会解除设备报警').fontSize(this.readableSize(12)).fontColor(SUB).layoutWeight(1)
        }.width('100%')
      }else{Column({space:8}){
        Text('当前账号可查看记录，没有处置权限').fontSize(this.readableSize(13)).fontColor(AMBER).width('100%')
        Button(this.remindBusy?'正在提醒…':'提醒家人查看处理',{type:ButtonType.Normal}).enabled(!this.remindBusy).constraintSize({minHeight:44}).width('100%').borderRadius(10).fontSize(this.readableSize(14)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).onClick(()=>{this.remindAlarm();})
        Text('提醒只会通知有处置权限的成员，不会改变这条报警的状态').fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(19))
        if(this.remindMessage){Text(this.remindMessage).fontSize(this.readableSize(12)).fontColor(this.remindMessage.indexOf('未发送')>=0?AMBER:PRIMARY).width('100%').lineHeight(this.readableLine(19))}
      }.width('100%').alignItems(HorizontalAlign.Start)}
      ForEach(this.notes,(note:NoteItem,index:number)=>{ Text(`${resolutionLabel(note.resolution)} · ${note.username||'护理人员'}${note.account_deleted?'（账号已删除）':''}：${note.note}`).fontSize(this.readableSize(13)).fontColor(SUB).padding(10).width('100%').backgroundColor(BG).borderRadius(10) },(_note:NoteItem,index:number)=>index.toString())
      Row({space:8}) { Button('已处理').fontSize(this.readableSize(13)).backgroundColor(this.resolution==='handled'?PRIMARY:'#E7EDF0').fontColor(this.resolution==='handled'?'#FFFFFF':SUB).enabled(!this.noteSaving).onClick(()=>{this.resolution='handled';}); Button('误报').fontSize(this.readableSize(13)).backgroundColor(this.resolution==='false_alarm'?PRIMARY:'#E7EDF0').fontColor(this.resolution==='false_alarm'?'#FFFFFF':SUB).enabled(!this.noteSaving).onClick(()=>{this.resolution='false_alarm';}); Button('需跟进').fontSize(this.readableSize(13)).backgroundColor(this.resolution==='follow_up'?PRIMARY:'#E7EDF0').fontColor(this.resolution==='follow_up'?'#FFFFFF':SUB).enabled(!this.noteSaving).onClick(()=>{this.resolution='follow_up';}) }.width('100%')
      Text('先选择处置结果，再点击“保存处理记录”。成功后关闭详情；只保存这条事件的处理说明，不解除设备报警。').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
      if(this.allowed('alarms.handle')){
        Row({space:8}){
          Button('已联系社区上门',{type:ButtonType.Normal}).fontSize(this.readableSize(12)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:44}).layoutWeight(1).enabled(!this.noteSaving).onClick(()=>{this.noteDraft='已联系社区网格员上门查看';this.resolution='follow_up';})
          Button('已核实，老人正常',{type:ButtonType.Normal}).fontSize(this.readableSize(12)).fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:44}).layoutWeight(1).enabled(!this.noteSaving).onClick(()=>{this.noteDraft='已联系老人并核实一切正常，确认为误报';this.resolution='false_alarm';})
        }.width('100%')
      }
      TextArea({text:this.noteDraft,placeholder:'补充联系家属、现场查看等处理情况…'}).maxLength(2000).enabled(!this.noteSaving&&this.allowed('alarms.handle')).height(90).fontSize(this.readableSize(13)).backgroundColor(BG).onChange((value:string)=>{this.noteDraft=value;})
      Button(this.noteSaving?'正在保存…':'保存处理记录').height(this.elderMode ? 60 : 48).width('100%').backgroundColor(PRIMARY).enabled(!this.noteSaving&&this.selected!==null&&this.allowed('alarms.handle')&&(!this.selected?.claim||this.selected?.claim?.username===this.accountName)).onClick(()=>{this.saveNote();})
      if(this.noteMessage){Text(this.noteMessage).fontSize(this.readableSize(13)).fontColor(this.noteMessage.indexOf('失败')>=0?RED:PRIMARY).width('100%')}
      if (this.demo) { Text('演示操作不会发送到真实设备').fontSize(this.readableSize(13)).fontColor(AMBER) }
    }.padding(22).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(Color.White).borderRadius(16).scrollBar(BarState.Off)
  }
  private detailTitle(): string {
    return this.detail==='door'?'房门':this.detail==='reports'?'活动报告':this.detail==='activity'?'活动记录':this.detail==='members'?'家庭成员':this.detail==='voice'?'语音助手':this.detail==='diagnostics'?'技术诊断':'守护说明';
  }
  @Builder
  detailHeader() {
    Row({space:8}) {
      Button({type:ButtonType.Normal}){this.icon($r('app.media.back'),24)}.width(44).height(44).backgroundColor(Color.Transparent).onClick(()=>{this.detail='';})
      Text(this.detailTitle()).fontSize(this.readableSize(24)).fontWeight(FontWeight.Bold).fontColor(INK).layoutWeight(1)
    }.width('100%').padding({left:20,right:20,top:4,bottom:8})
  }
  @Builder
  detailPage() {
    Column({space:16}) {
      if(this.detail==='door'){this.doorDetail();}
      else if(this.detail==='activity'){this.activityDetail();}
      else if(this.detail==='reports'){this.activityReports();}
      else if(this.detail==='members'){this.familyMembers();}
      else if(this.detail==='voice'){this.voicePage();}
      else if(this.detail==='diagnostics'){this.diagnostics();}
      else {
        this.assessmentCard()
        Text(this.lastActivity()).fontSize(this.readableSize(16)).fontColor(INK).width('100%')
        Text('人体传感器记录感应范围内的活动，不能据此确认人在家、外出或跌倒。异常状态需要联系家人核实。').fontSize(this.readableSize(14)).fontColor(SUB).lineHeight(24).width('100%')
        Button('查看活动趋势与报告').fontColor(PRIMARY).backgroundColor(CareTheme.softBlue).constraintSize({minHeight:48}).width('100%').onClick(()=>{this.navigate(2);})
      }
    }.width('100%')
  }
  onBackPress():boolean {
    if(this.livingOpen&&!this.livingBusy){this.livingOpen=false;return true;}
    if(this.selected&&!this.noteSaving){this.closeAlarm();return true;}
    if(this.profileOpen&&!this.profileSaving){this.profileOpen=false;return true;}
    if(this.settingsOpen&&!this.loginBusy){this.settingsOpen=false;return true;}
    if(this.detail){this.detail='';return true;}
    return false;
  }
  build() {
    Stack({alignContent:Alignment.Center}) {
      Column() {
        if (this.demo || this.simulated) { Text(this.demo?'演示环境 · 所有数据仅用于展示':'模拟后端 · 非真实设备数据').fontSize(this.readableSize(13)).fontColor(CareTheme.warmInk).width('100%').textAlign(TextAlign.Center).padding({top:5,bottom:5}).backgroundColor(CareTheme.warmSurface) }
        if(this.detail){this.detailHeader()}
        Scroll(this.pageScroller) {
          Column() { if(this.detail){this.detailPage()} else if(this.loggedIn&&!this.allowed('view')&&this.tab!==1&&this.tab!==4){this.incidentAccessPage()} else if(this.tab===0){this.overview()} else if(this.tab===1){this.alarmsPage()} else if(this.tab===2){this.trendsPage()} else if(this.tab===3){this.devicePage()} else {this.userPage()} }.padding({left:20,right:20,bottom:24}).width('100%')
        }.layoutWeight(1).align(Alignment.Top).scrollBar(BarState.Off).width('100%')
        if(!this.detail){Row() { this.navItem(0,$r('app.media.home'),'守护'); this.navItem(1,$r('app.media.bell'),'报警'); this.navItem(2,$r('app.media.chart'),'趋势'); this.navItem(3,$r('app.media.device'),'设备'); this.navItem(4,$r('app.media.user'),'我的') }.width('100%').padding({left:12,right:12,top:4,bottom:12}).backgroundColor(Color.White).border({width:{top:0.5},color:CareTheme.line})}
      }.width('100%').height('100%').backgroundColor(BG).padding({top:0,bottom:0})
      if (this.settingsOpen || this.profileOpen || this.selected!==null) {
        Rect().fill('#660B2028').width('100%').height('100%').onClick(()=>{if(!this.loginBusy&&!this.profileSaving&&!this.livingBusy&&!this.noteSaving){this.noteOperation++;this.livingOpen=false;this.settingsOpen=false;this.profileOpen=false;this.selected=null;this.passwordDraft='';}})
        if(this.settingsOpen){this.settingsSheet()}else if(this.profileOpen){this.profileSheet()}else{this.alarmSheet()}
      }
      if(this.livingOpen){this.livingSheet()}
      if(this.switchOpen){
        Rect().fill('#660B2028').width('100%').height('100%').onClick(()=>{if(!this.switchBusy){this.switchOpen=false;this.switchUser='';this.switchPassword='';}})
        this.accountSwitchSheet()
      }
      if (this.alertQueue.length > 0 && !this.settingsOpen && !this.profileOpen && !this.livingOpen && !this.switchOpen && this.selected === null) {
        Rect().fill('#880B2028').width('100%').height('100%')
        this.incomingSheet()
      }
      if(this.medicineNotice && this.alertQueue.length===0 && !this.settingsOpen && !this.profileOpen && !this.livingOpen && !this.switchOpen && this.selected===null && this.livingFresh()) {
        Rect().fill('#880B2028').width('100%').height('100%')
        this.medicineSheet()
      }
    }.width('100%').height('100%')
  }
}
