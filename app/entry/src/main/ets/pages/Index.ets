import { http } from '@kit.NetworkKit';
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { Backend } from '../services/Backend';
import { medicineLabel, nightLabel, medicineTime, validMedicineTime, medicineSlots, slotLabel, pendingMedicine, demoLivingUpdate } from '../models/Living';
import { AlarmSnapshot, alarmStateLabel, alarmIsActive, noteText, resolutionLabel } from '../models/AlarmState';
import { doorLabel } from '../models/DoorState';
import { AlarmInbox, mergeAlarms } from '../models/AlarmInbox';
import { assessCare, CareAssessment } from '../models/CareAssessment';
import { Shadow, Health, Properties, AlarmEvent, AlarmList, Statistics, DayStats, Command, CommandRequest, LivingStatus, MedicineSlot, MedicineDraft, MedicineHistory, MedicineRecord, CommandParameters, NoteList, NoteItem, Push, alarmTitle, alarmKey, alarmTime } from '../models/Models';

const INK: string = '#18343F';
const SUB: string = '#7B8D94';
const TEAL: string = '#159A91';
const BG: string = '#F4F7F8';
const RED: string = '#D55A55';
const AMBER: string = '#C78E35';
const DEVICE: string = 'demo-device';

interface MetricView { icon: Resource; label: string; value: string; caption: string; tint: string; }
interface PillView { text: string; color: string; background: string; }
interface SectionView { title: string; subtitle: string; }
interface SummaryView { label: string; value: string; }
interface InfoView { label: string; value: string; color: string; }

@Entry
@Component
struct Index {
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
  @State doorEnabled: number = -1;
  @State doorValid: number = -1;
  @State doorValue: number = -1;
  @State fwTls: boolean = false;
  @State alarms: AlarmEvent[] = [];
  @State alarmFilter: number = 0;
  @State hours: number[] = [];
  @State days: DayStats[] = [];
  @State activityTotal: number = 0;
  @State alarmTotal: number = 0;
  @State chartDay: number = 0;
  @State settingsOpen: boolean = false;
  @State serverDraft: string = 'https://backend.example.com';
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
  @State commandMessage: string = '';
  @State refreshCount: number = 0;
  private api: Backend = new Backend();
  private inbox: AlarmInbox = new AlarmInbox();
  private pageScroller: Scroller = new Scroller();
  private sessionPassword: string = ''; // Current login only; never persisted.
  private timer: number = -1;
  private generation: number = 0;
  private visible: boolean = true;
  private polling: boolean = false;
  private ticks: number = 0;
  private healthSignature: string = '';
  private noteOperation: number = 0;
  private demoNotes: Map<string, NoteItem[]> = new Map<string, NoteItem[]>();

  aboutToAppear(): void { this.initialize(); }
  onPageShow(): void { this.visible = true; this.startTimer(); if (this.loggedIn && !this.demo) { this.refresh(true); this.startPush(); } }
  onPageHide(): void { this.visible = false; this.stopTimer(); this.api.stopPush(); this.generation++; this.polling = false; this.loading = false; }
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
    } catch (_) { /* Settings are optional; never persist passwords. */ }
    await this.loadProfile();
    this.startTimer();
  }
  private startTimer(): void {
    if (this.timer !== -1 || !this.visible) { return; }
    this.timer = setInterval(() => {
      this.now = Date.now(); this.ticks++;
      if (this.demo) {
        if (this.ticks % 5 === 0) { this.temp = 26.4 + Math.sin(this.ticks / 12) * 0.4; this.humi = 53 + Math.sin(this.ticks / 15) * 2; this.received = this.now; this.deviceTime = this.now; }
      } else if (this.loggedIn && this.ticks % 2 === 0) { this.refresh(this.ticks % 16 === 0); }
    }, 1000);
  }
  private stopTimer(): void { if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; } }
  private loadDemo(): void {
    this.livingOpen = false; this.medicineNotice = false; this.medicineSeen = []; this.livingDataFresh = true; this.livingMessage='';
    this.living = {schema:2,config_revision:1,night_auto:true,night_on:false,light_valid:true,light_owner:'off',medicine_enabled:false,medicine_minute:480,medicine_state:0,dose_day:Math.floor((Date.now()/1000+28800)/86400),confirmed_epoch:0,time_synced:true,medicine_revision:1,slots:[{slot:0,enabled:false,minute:480,state:0,confirmed_epoch:0},{slot:1,enabled:false,minute:720,state:0,confirmed_epoch:0},{slot:2,enabled:false,minute:1200,state:0,confirmed_epoch:0}]};
    this.demoNotes=new Map<string, NoteItem[]>();
    this.acknowledgedAlarms=[]; this.healthBoot=-1; this.healthUp=-1; this.noteSaving=false; this.noteMessage=''; this.noteOperation++;
    this.sessionPassword = ''; this.passwordDraft = '';
    this.accountName = ''; this.alertQueue = []; this.inbox = new AlarmInbox();
    this.displayName = '守护者'; this.careName = '家人'; this.contactPhone = ''; this.alarmReminders = true;
    this.generation++; this.polling = false; this.loading = false; this.api.stopPush();
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
    if (!this.loggedIn || !this.visible || this.polling) { return; }
    const generation = this.generation; this.polling = true; this.loading = true;
    try {
      const response = await this.api.request(this.api.path('shadow'));
      if (generation !== this.generation || !this.visible) { return; }
      const shadow = response.data as Shadow;
      this.applyProperties(shadow.properties || {});
      this.online = shadow.online === true; this.simulated = response.simulated === true;
      this.deviceTime = shadow.updated_at ? Date.parse(shadow.updated_at) : 0;
      this.received = Date.now(); this.refreshCount++; this.error = '';
      this.pruneAlerts();
      if (full) {
        const health = await this.api.request(this.api.path('health'));
        if (generation !== this.generation) { return; }
        if (health.data) { this.applyHealth(health.data as Health); } else { this.healthKnown = false; this.living=null;this.medicineNotice=false; }
        const events = await this.api.request(this.api.path('alarms'));
        if (generation !== this.generation) { return; }
        this.receiveAlarms((events.data as AlarmList).items || []);
        const statistics = await this.api.request(this.api.path('stats'));
        if (generation !== this.generation) { return; }
        const stats = statistics.data as Statistics;
        const selectedDate = this.days.length > this.chartDay ? this.days[this.chartDay].date : '';
        this.days = stats.days || [];
        const selectedIndex = this.days.findIndex((day: DayStats) => day.date === selectedDate);
        this.chartDay = selectedIndex >= 0 ? selectedIndex : Math.max(0, this.days.length - 1);
        this.hours = this.days.length ? this.days[this.chartDay].activity_by_hour : [];
        this.activityTotal = this.days.length ? this.days[this.chartDay].activity_total : 0;
        this.alarmTotal = this.days.length ? this.days[this.chartDay].alarm_total : 0;
      }
    } catch (error) {
      if (generation === this.generation) { this.error = '同步失败，请检查后端、网络和证书：' + (error as Error).message; }
    } finally { if (generation === this.generation) { this.polling = false; this.loading = false; } }
  }
  private startPush(): void {
    this.api.startPush((message: Push) => {
      if (!this.visible || this.demo) { return; }
      if (message.type === 'alarm' && message.data) { this.receiveAlarms([message.data as AlarmEvent]); }
      if (message.type === 'health' && message.data) { this.applyHealth(message.data as Health); }
      if (message.type === 'alarm' || message.type === 'stats' || message.type === 'resync_required' || message.type === 'command_ack') { this.refresh(true); }
    }, (online: boolean) => { this.pushOnline = online; });
  }
  private navigate(index: number): void {
    this.tab = index;
    this.pageScroller.scrollTo({ xOffset: 0, yOffset: 0 });
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
    }
    this.pruneAlerts();
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
    return alarmStateLabel(item,this.alarms,this.alarmSnapshot(),this.acknowledgedAlarms);
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
    return level === 'alert' ? RED : level === 'normal' ? TEAL : AMBER;
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
  private async login(): Promise<void> {
    if (this.loginBusy) { return; }
    this.loginBusy = true; this.error = '';
    const loginPassword = this.passwordDraft;
    try {
      await this.api.login(this.serverDraft, this.userDraft, loginPassword);
      this.sessionPassword = loginPassword;
      this.accountName = this.userDraft.trim(); this.alertQueue = []; this.inbox = new AlarmInbox();
      this.generation++; this.polling = false; this.demo = false; this.loggedIn = true; this.simulated = false;
      this.temp = -999; this.humi = -999; this.gas = -999; this.light = -999; this.received = 0; this.deviceTime = 0;
      this.doorValue = -1; this.doorEnabled = -1; this.doorValid = -1;
      this.living=null;this.medicineNotice=false;this.medicineSeen=[];this.livingOpen=false;this.acknowledgedAlarms=[];this.healthBoot=-1;this.healthUp=-1;this.healthKnown = false; this.healthReceived = 0; this.healthSignature = ''; this.firmware = ''; this.muted = false; this.alarms = []; this.hours = []; this.days = []; this.activityTotal = 0; this.alarmTotal = 0;
      this.passwordDraft = ''; this.settingsOpen = false;
      try {
        const store = await preferences.getPreferences(getContext(this), 'zhian_settings');
        await store.put('server', this.serverDraft); await store.put('username', this.userDraft); await store.flush();
      } catch (_) { /* Login can work without persisted settings. */ }
      await this.loadProfile();
      await this.refresh(true); this.startPush();
    } catch (error) { this.error = (error as Error).message || '登录失败，请核对地址、密码和开发证书'; }
    finally { this.loginBusy = false; }
  }
  private async disconnect(): Promise<void> {
    this.generation++; this.loggedIn = false; this.polling = false; this.online = false;
    await this.api.logout().catch(() => {}); this.selected = null; this.settingsOpen = false; this.profileOpen = false; this.loadDemo(); await this.loadProfile();
  }
  private livingFresh(): boolean { return this.demo || this.livingDataFresh && this.healthFresh() && this.online && this.loggedIn; }
  private openLiving(): void {
    if (!this.living || !this.livingFresh()) { this.livingMessage='请等待设备新版状态上报，确认固件和后端均已更新'; return; }
    this.nightDraft=this.living.night_auto;
    this.medicineDrafts=medicineSlots(this.living).map((slot:MedicineSlot):MedicineDraft=>({slot:slot.slot,enabled:slot.enabled,hour:Math.floor(slot.minute/60).toString().padStart(2,'0'),minute:(slot.minute%60).toString().padStart(2,'0')}));
    this.livingRevision=this.living.config_revision; this.livingMessage=''; this.livingOpen=true;
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
        this.medicineSeen=this.medicineSeen.concat([key]);this.medicineNoticeSlot=slot.slot;this.medicineNotice=true;break;
      }
    }
  }
  private updateMedicineDraft(index: number, enabled?: boolean, hour?: string, minute?: string): void {
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
    if (this.livingBusy || this.commandBusy || !this.living || !this.livingFresh()) { return; }
    const generation=this.generation;
    this.livingBusy=true; this.livingMessage='正在等待设备执行回执…';
    try {
      if (this.demo) {
        this.living=demoLivingUpdate(this.living,name,paras,Math.floor(Date.now()/1000),this.light);
        this.livingMessage='演示已更新，未操作真实设备';this.livingOpen=false;
        this.medicineNotice=false;this.updateMedicineNotice();
        return;
      }
      const request: CommandRequest={device_id:this.api.device,service_id:'HomeSafety',command_name:name,paras:paras,client_request_id:'living-'+Date.now()+'-'+Math.floor(Math.random()*1000000)};
      let result=(await this.api.request('/api/v1/commands',http.RequestMethod.POST,JSON.stringify(request))).data as Command;
      for(let i=0;i<15&&result.state==='pending';i++) {
        await new Promise<void>((resolve)=>{setTimeout(()=>{resolve();},1000);});
        result=(await this.api.request('/api/v1/commands/'+encodeURIComponent(result.command_id))).data as Command;
      }
      if(generation!==this.generation){return;}
      if(result.state==='executed' && result.result_code===0) { this.livingMessage='设备已确认执行，等待状态上报';this.livingOpen=false;if(name==='confirm_medicine'||name==='set_medicine_state'){this.medicineNotice=false;} }
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
    promptAction.showDialog({title:'改为未服药？',message:'仅更改当天第 '+(slot.slot+1)+' 时段的记录。到时未服药提醒会恢复。',buttons:[{text:'取消',color:SUB},{text:'确认修改',color:TEAL}]}).then((result)=>{if(result.index===1){this.changeMedicine(slot.slot,false);}}).catch(()=>{});
  }
  private confirmMedicine(): void {
    const slot=pendingMedicine(this.living).find((item:MedicineSlot)=>item.slot===this.medicineNoticeSlot);
    if(slot){this.changeMedicine(slot.slot,true);}
  }
  private async command(name: string): Promise<void> {
    if (this.commandBusy || (name !== 'ack' && name !== 'mute')) { return; }
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
          buttons: [{ text: '取消', color: SUB }, { text: name === 'ack' ? '确认设备报警' : '尝试消音', color: TEAL }]
        });
        if (answer.index !== 1) { return; }
      }
      // A dialog may outlive a login change, page hide or device switch.
      if (generation !== this.generation || demoOperation !== this.demo || targetDevice !== this.api.device) { return; }
      const acknowledgedKeys = this.alarms.filter((item: AlarmEvent) =>
        this.eventActive(item) && (item.code === 1 || item.code === 2)).map((item: AlarmEvent) => alarmKey(item));
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
  private async showAlarm(item: AlarmEvent): Promise<void> {
    if(this.noteSaving){return;}
    const operation=++this.noteOperation;
    this.selected=item; this.noteDraft=''; this.notes=[]; this.resolution='handled'; this.noteMessage='';
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
  private async saveNote(): Promise<void> {
    if(!this.selected || this.noteSaving){return;}
    const item=this.selected;
    const text=noteText(this.noteDraft,this.resolution);
    const resolution=this.resolution;
    const operation=++this.noteOperation;
    this.noteSaving=true; this.noteMessage='正在保存处理记录…';
    try {
      if(!this.demo) {
        await this.api.request(this.api.path('alarms/'+alarmKey(item)+'/notes'),http.RequestMethod.POST,
          JSON.stringify({note:text,resolution:resolution}));
      }
      if(operation!==this.noteOperation){return;}
      this.notes=this.notes.concat([{note:text,resolution:resolution,username:this.demo?'演示用户':this.userDraft}]);
      if(this.demo){this.demoNotes.set(alarmKey(item),this.notes);}
      this.noteDraft='';
      this.noteMessage='';
      // Close only after persistence succeeds; queued taps then have no selected event.
      this.selected=null;
    } catch(error) {
      if(operation===this.noteOperation){this.noteMessage='保存失败：'+(error as Error).message;}
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
  private chartMax(): number { let max = 1; this.hours.forEach((value: number) => { max = Math.max(max, value); }); return max; }
  private pendingCount(): number { return this.alarms.filter((item: AlarmEvent) => this.eventActive(item)).length; }
  private statusTitle(): string { return this.status > 0 ? alarmTitle(this.status) : this.stale() ? '数据等待更新' : !this.online ? '设备状态待确认' : '当前未见报警'; }

  @Builder
  icon(source: Resource, size: number = 22) { Image(source).width(size).height(size).objectFit(ImageFit.Contain) }
  @Builder
  pill($$: PillView) {
    Text($$.text).fontSize(this.readableSize(11)).fontWeight(FontWeight.Medium).fontColor($$.color).padding({ left: 9, right: 9, top: 5, bottom: 5 }).backgroundColor($$.background).borderRadius(12)
  }
  @Builder
  section($$: SectionView) {
    Row() { Text($$.title).fontSize(this.readableSize(18)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); if ($$.subtitle) { Text($$.subtitle).fontSize(this.readableSize(11)).fontColor(SUB) } }.width('100%').margin({ top: 22, bottom: 12 })
  }
  @Builder
  metric($$: MetricView) {
    Column({ space: 12 }) {
      Row() { Stack() { Rect().fill('#EFF7F5').width(36).height(36).radius(12); this.icon($$.icon, 21) }; Blank(); Text($$.label).fontSize(this.readableSize(13)).fontColor(SUB) }.width('100%')
      Text($$.value).fontSize(this.readableSize(29)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      Text($$.caption).fontSize(this.readableSize(11)).fontColor($$.tint).width('100%')
    }.padding(16).backgroundColor(Color.White).borderRadius(20).layoutWeight(1).alignItems(HorizontalAlign.Start)
  }
  @Builder
  overview() {
    Column() {
      Row() { Column({ space: 5 }) { Text('鸿颐安 · 居家守护').fontSize(this.readableSize(13)).fontColor(SUB); Text(this.careName + '的居家守护').fontSize(this.readableSize(27)).fontWeight(FontWeight.Bold).fontColor(INK) }.alignItems(HorizontalAlign.Start); Blank(); Stack() { Circle().fill('#E6F2EF').width(46).height(this.elderMode ? 60 : 46); this.icon($r('app.media.user'),26) }.onClick(() => { this.navigate(4); }) }.width('100%').margin({ top: 12, bottom: 20 })
      Row({ space: 6 }) { this.pill({ text: this.demo ? '本地演示' : this.simulated ? '模拟后端' : '真实设备', color: this.demo || this.simulated ? AMBER : TEAL, background: this.demo || this.simulated ? '#FFF3DE' : '#E8F5F1' }); Text(this.demo ? '演示数据，不代表真实监测' : 'TX-SMART-R · HomeSafety').fontSize(this.readableSize(11)).fontColor(SUB); Blank() }.width('100%').margin({ bottom: 14 })
      Column({ space: 16 }) {
        Row() { this.icon($r('app.media.shield_white'),32); Blank(); Text(this.demo ? '演示中' : this.online ? '云端在线' : '状态待确认').fontSize(this.readableSize(12)).fontColor('#BBDCD7') }.width('100%')
        Text(this.assessment().title).fontSize(this.readableSize(25)).fontWeight(FontWeight.Bold).fontColor(Color.White).width('100%')
        Text(this.assessment().reason).fontSize(this.readableSize(12)).fontColor('#BBD0D5').width('100%')
        Row() { Circle().fill(this.online ? '#76D7AD' : '#D6B770').width(6).height(6); Text(this.age()).fontSize(this.readableSize(11)).fontColor('#D5E4E7').margin({ left: 6 }); Blank(); Text(this.loading ? '查询中…' : '查询 ↻').fontSize(this.readableSize(12)).fontColor(Color.White).onClick(() => { this.refresh(true); }) }.width('100%')
      }.padding(22).backgroundColor(this.assessment().level === 'alert' ? '#873F40' : '#183C47').borderRadius(24).width('100%').onClick(()=>{this.navigate(this.assessment().level==='alert'?1:3);})
      if (this.error) { Text(this.error).fontSize(this.readableSize(12)).fontColor(RED).padding(12).width('100%').backgroundColor('#FFF0EB').borderRadius(12).margin({ top: 12 }) }
      Row({space:14}) {
        Stack() { Rect().fill('#EAF5F1').width(44).height(this.elderMode ? 60 : 44).radius(14); this.icon($r('app.media.home'),26) }
        Column({space:5}) { Text('房门').fontSize(this.readableSize(16)).fontWeight(FontWeight.Medium).fontColor(INK); Text(this.demo?'演示状态':this.stale()?'等待设备重新上报':'随设备上报更新').fontSize(this.readableSize(10)).fontColor(SUB).maxLines(1) }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Blank()
        Text(this.roomDoor()).fontSize(this.readableSize(15)).fontWeight(FontWeight.Bold).fontColor(!this.demo&&(this.stale()||!this.healthFresh()||this.doorValid===0)?SUB:this.doorValue===1?AMBER:TEAL).constraintSize({maxWidth:160}).textAlign(TextAlign.End)
      }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(20).margin({top:14})
      this.livingCard()
      this.section({ title: '实时环境', subtitle: this.stale() ? '上报已过期' : '每 2 秒查询云端' })
      Row({ space: 12 }) {
        this.metric({ icon: $r('app.media.thermometer'), label: '室内温度', value: this.number(this.temp, '°', 1), caption: this.healthFresh() && !this.envValid ? '温度传感器不可用' : this.temp === -999 ? '暂无有效读数' : '摄氏度 · ℃', tint: this.healthFresh() && !this.envValid ? AMBER : TEAL })
        this.metric({ icon: $r('app.media.droplet'), label: '空气湿度', value: this.number(this.humi, '%', 1), caption: this.humi === -999 ? '暂无有效读数' : '相对湿度 · RH', tint: TEAL })
      }.width('100%')
      Row({ space: 12 }) { this.metric({ icon: $r('app.media.flame'), label: '燃气读数', value: this.number(this.gas, ''), caption: '传感器估算 · ppm', tint: TEAL }); this.metric({ icon: $r('app.media.sun'), label: '环境光照', value: this.number(this.light, ''), caption: '照度 · lx', tint: TEAL }) }.width('100%').margin({ top: 12 })
      this.assessmentCard()
      this.section({ title: '居家动态', subtitle: '设备观察状态' })
      Row({ space: 14 }) { this.icon($r('app.media.activity'),28); Column({ space: 5 }) { Text(this.stale() ? '活动数据已过期' : this.body < 0 ? '等待活动数据' : this.body === 1 ? '检测到活动' : '暂未检测到活动').fontSize(this.readableSize(15)).fontWeight(FontWeight.Medium).fontColor(INK); Text(this.inactive < 0 ? '等待设备上报 · PIR 感应' : `距上次活动 ${this.inactive} 秒 · PIR 感应`).fontSize(this.readableSize(11)).fontColor(SUB) }.alignItems(HorizontalAlign.Start); Blank(); this.pill({ text: '固定式居家监测', color: TEAL, background: '#E9F6F3' }) }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(18)
      this.section({ title: '快速处理', subtitle: '执行结果以设备回执为准' })
      this.controls()
      this.section({ title: '今日活动', subtitle: 'PIR 活动次数' })
      this.chart(false)
      Text('鸿颐安 · 让关心及时抵达').fontSize(this.readableSize(11)).fontColor('#A1AFB5').margin({ top: 24, bottom: 20 })
    }.width('100%')
  }
  @Builder
  livingCard() {
    Column({space:14}) {
      Row() { Text('生活提醒').fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK);Blank();Text('设置').fontSize(this.readableSize(13)).fontColor(TEAL).onClick(()=>{this.openLiving();}) }.width('100%')
      Row({space:12}) {
        this.icon($r('app.media.sun'),25)
        Column({space:4}) { Text('自动夜灯').fontSize(this.readableSize(14)).fontColor(INK);Text('低照度自动点亮 · 报警优先').fontSize(this.readableSize(10)).fontColor(SUB) }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Text(nightLabel(this.living,this.livingFresh())).fontSize(this.readableSize(12)).fontColor(TEAL).constraintSize({maxWidth:130}).textAlign(TextAlign.End)
      }.width('100%')
      Divider().color('#EDF2F3')
      Row() {Text('每日服药提醒').fontSize(this.readableSize(14)).fontColor(INK);Blank();Text(this.living&&this.living.schema===2?'最多三个时段':'单次提醒 · 旧版设备').fontSize(this.readableSize(10)).fontColor(SUB)}.width('100%')
      ForEach(medicineSlots(this.living),(slot:MedicineSlot)=>{
        Row({space:10}) {
          this.icon($r('app.media.bell'),22)
          Column({space:4}) {
            Text('第 '+(slot.slot+1)+' 时段 · '+medicineTime(slot.minute)).fontSize(this.readableSize(13)).fontColor(INK)
            Text(slotLabel(slot,this.living?this.living.time_synced:false,this.livingFresh())).fontSize(this.readableSize(12)).fontColor(slot.state===3?AMBER:TEAL)
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          if(slot.enabled && (slot.state===3||slot.state===4) && this.livingFresh()) {
            Button(slot.state===4?'改为未服药':'确认已服药').fontSize(this.readableSize(11)).height(34).backgroundColor(slot.state===4?'#EDF4F3':TEAL).fontColor(slot.state===4?TEAL:'#FFFFFF').enabled(!this.livingBusy&&!this.commandBusy&&(slot.state!==4||(this.living!==null&&this.living.schema===2))).onClick(()=>{this.askMedicine(slot);})
          }
        }.width('100%')
      },(slot:MedicineSlot)=>slot.slot+':'+slot.minute+':'+slot.enabled+':'+slot.state+':'+this.livingFresh())
      if(!this.living){Text('等待设备生活提醒状态').fontSize(this.readableSize(12)).fontColor(SUB)}
      if(this.livingMessage) {Text(this.livingMessage).fontSize(this.readableSize(11)).fontColor(AMBER).width('100%')}
    }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(20).margin({top:14})
  }
  @Builder
  livingSheet() {
    Scroll() {
      Column({space:18}) {
        Row() { Text('生活提醒设置').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK);Blank();Text('关闭').fontColor(SUB).onClick(()=>{if(!this.livingBusy){this.livingOpen=false;}}) }.width('100%')
        Text(this.demo?'本地演示设置，不操作真实设备':'设置保存在板子，重启保留；以设备回执为准').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')
        Row() {Column({space:5}) {Text('自动夜灯').fontColor(INK);Text('低于 30 lx 开灯，高于 60 lx 关灯').fontSize(this.readableSize(11)).fontColor(SUB)}.alignItems(HorizontalAlign.Start);Blank();Toggle({type:ToggleType.Switch,isOn:this.nightDraft}).selectedColor(TEAL).enabled(!this.livingBusy).onChange((value:boolean)=>{this.nightDraft=value;})}.width('100%')
        Text('每日最多三个时段 · 北京时间 UTC+8').fontSize(this.readableSize(13)).fontColor(SUB).width('100%')
        ForEach(this.medicineDrafts,(draft:MedicineDraft)=>{
          Column({space:10}) {
            Row() {Text('第 '+(draft.slot+1)+' 时段').fontColor(INK);Blank();Toggle({type:ToggleType.Switch,isOn:draft.enabled}).selectedColor(TEAL).enabled(!this.livingBusy).onChange((value:boolean)=>{this.updateMedicineDraft(draft.slot,value);})}.width('100%')
            Row({space:12}) {
              TextInput({text:draft.hour,placeholder:'08'}).type(InputType.Number).maxLength(2).layoutWeight(1).height(this.elderMode ? 60 : 46).enabled(!this.livingBusy).onChange((value:string)=>{this.updateMedicineDraft(draft.slot,undefined,value);})
              Text(':').fontSize(this.readableSize(23)).fontColor(INK)
              TextInput({text:draft.minute,placeholder:'00'}).type(InputType.Number).maxLength(2).layoutWeight(1).height(this.elderMode ? 60 : 46).enabled(!this.livingBusy).onChange((value:string)=>{this.updateMedicineDraft(draft.slot,undefined,undefined,value);})
            }.width('100%')
          }.padding(12).backgroundColor(BG).borderRadius(14).width('100%')
        },(draft:MedicineDraft)=>draft.slot.toString())
        Text('按已有医嘱设置。到时板端蜂鸣提醒，前台 App 弹出提醒；可分别确认或撤销当天状态。确认不代表自动检测到实际服药。').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')
        if(this.living&&this.living.schema===1){Text('当前设备仅支持一个时段；升级固件后可设置三个。').fontSize(this.readableSize(12)).fontColor(AMBER)}
        if(this.livingMessage) {Text(this.livingMessage).fontSize(this.readableSize(12)).fontColor(AMBER).width('100%')}
        Button(this.livingBusy?'等待设备确认…':'保存到设备').width('100%').height(this.elderMode ? 60 : 48).backgroundColor(TEAL).enabled(!this.livingBusy&&!this.commandBusy).onClick(()=>{this.saveLiving();})
        Text('服药历史').fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
        if(this.medicineHistoryMessage){Text(this.medicineHistoryMessage).fontSize(this.readableSize(11)).fontColor(SUB).width('100%')}
        if(this.medicineHistory.length===0&&!this.medicineHistoryMessage){Text('暂无已上报的服药记录').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')}
        ForEach(this.medicineHistory.slice(0,12),(record:MedicineRecord)=>{
          Text(new Date(record.dose_day*86400000).toISOString().slice(0,10)+' · 第 '+(record.slot+1)+' 时段 '+medicineTime(record.minute)+' · '+(record.state===4?'已服药':'未服药')).fontSize(this.readableSize(12)).fontColor(record.state===4?TEAL:AMBER).width('100%')
        },(record:MedicineRecord)=>record.dose_day+':'+record.slot+':'+record.medicine_revision+':'+record.state)

      }.padding(22).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(Color.White).borderRadius(24).scrollBar(BarState.Off)
  }
  @Builder
  medicineSheet() {
    Column({space:18}) {
      this.icon($r('app.media.bell'),40)
      Text('第 '+(this.medicineNoticeSlot+1)+' 时段服药提醒').fontSize(this.readableSize(24)).fontWeight(FontWeight.Bold).fontColor(INK)
      Text(this.demo?'演示提醒，不操作真实设备':'已到每日提醒时间，请根据实际情况确认。').fontSize(this.readableSize(14)).fontColor(SUB)
      Text('确认仅记录一次用户操作，无法证明已经实际服药。').fontSize(this.readableSize(12)).fontColor(SUB)
      if(this.livingMessage){Text(this.livingMessage).fontSize(this.readableSize(12)).fontColor(AMBER)}
      Button(this.livingBusy?'等待设备回执…':'确认已服药').width('100%').height(this.elderMode ? 60 : 46).backgroundColor(TEAL).enabled(!this.livingBusy&&!this.commandBusy&&this.livingFresh()).onClick(()=>{this.confirmMedicine();})
      Button('稍后处理').width('100%').height(this.elderMode ? 60 : 42).backgroundColor('#EDF4F3').fontColor(TEAL).enabled(!this.livingBusy).onClick(()=>{this.medicineNotice=false;})
    }.padding(24).width('90%').backgroundColor(Color.White).borderRadius(24)
  }
  @Builder
  controls() {
    Row({ space: 12 }) {
      Button(this.commandBusy ? '操作进行中…' : '确认设备报警').fontSize(this.readableSize(14)).fontWeight(FontWeight.Medium).fontColor(Color.White).backgroundColor(TEAL).height(this.elderMode ? 60 : 46).layoutWeight(1).borderRadius(14).enabled(!this.commandBusy).onClick(() => { this.command('ack'); })
      Button(this.healthFresh() && this.muted ? '设备已消音' : '设备消音').fontSize(this.readableSize(14)).fontColor(INK).backgroundColor('#E7EDF0').height(this.elderMode ? 60 : 46).layoutWeight(1).borderRadius(14).enabled(!this.commandBusy).onClick(() => { this.command('mute'); })
    }.width('100%')
    if (!this.demo && (!this.online || this.stale())) { Text('设备离线或上报过期，远程操作可能无法送达，请联系现场人员。').fontSize(this.readableSize(12)).fontColor(AMBER).lineHeight(this.readableLine(20)).width('100%').margin({top:9}) }
    Text('消音只停止提示声音；确认设备报警不等于完成现场处置，也不会自动保存处理记录。').fontSize(this.readableSize(11)).fontColor(SUB).lineHeight(this.readableLine(19)).width('100%').margin({top:9})
    if (this.commandMessage) { Text(this.commandMessage).fontSize(this.readableSize(12)).fontColor(SUB).margin({ top: 9 }).width('100%') }
  }
  @Builder
  chart(large: boolean) {
    Column({ space: 14 }) {
      Row() { Column({ space: 4 }) { Text(this.activityTotal.toString()).fontSize(this.readableSize(32)).fontWeight(FontWeight.Bold).fontColor(INK); Text('已观察活动次数').fontSize(this.readableSize(11)).fontColor(SUB) }.alignItems(HorizontalAlign.Start); Blank(); this.pill({ text: this.demo ? '演示曲线' : '仅观察时段', color: TEAL, background: '#E9F6F3' }) }.width('100%')
      if (this.hours.length) {
        Row({ space: 3 }) { ForEach(this.hours, (value: number, index: number) => { Column() { Blank(); Rect().fill(index === Math.floor((this.now / 3600000 + 8) % 24) ? '#183C47' : '#63BFB1').width('100%').height(Math.max(3, value / this.chartMax() * (large ? 150 : 85))).radius(3) }.height(large ? 160 : 96).layoutWeight(1) }, (value: number, index: number) => `${index}:${value}`) }.width('100%').alignItems(VerticalAlign.Bottom)
        Row() { Text('00:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('06:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('12:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('18:00').fontSize(this.readableSize(9)).fontColor(SUB); Blank(); Text('23:00').fontSize(this.readableSize(9)).fontColor(SUB) }.width('100%')
      } else { Text('暂无可用统计，请等待设备上报').fontSize(this.readableSize(13)).fontColor(SUB).padding({ top: 30, bottom: 30 }) }
    }.padding(18).backgroundColor(Color.White).borderRadius(20).width('100%')
  }
  @Builder
  alarmsPage() {
    Column() {
      this.pageTitle('报警中心', '每一次异常，都值得及时回应')
      Row({ space: 10 }) { this.summary({ label: '事件记录', value: this.alarms.length.toString() }); this.summary({ label: '仍在报警', value: this.pendingCount().toString() }); this.summary({ label: '统计报警', value: this.alarmTotal.toString() }) }.width('100%')
      Row({ space: 10 }) { Button('全部事件').fontSize(this.readableSize(12)).backgroundColor(this.alarmFilter===0?INK:'#E6EDF0').fontColor(this.alarmFilter===0?'#FFFFFF':SUB).onClick(()=>{this.alarmFilter=0;}); Button('正在报警').fontSize(this.readableSize(12)).backgroundColor(this.alarmFilter===1?INK:'#E6EDF0').fontColor(this.alarmFilter===1?'#FFFFFF':SUB).onClick(()=>{this.alarmFilter=1;}); Blank(); Text('刷新 ↻').fontSize(this.readableSize(13)).fontColor(TEAL).onClick(()=>{this.refresh(true);}) }.width('100%').margin({ top: 20, bottom: 14 })
      if (!this.alarms.length) { this.empty('暂无报警记录', this.demo ? '当前演示中没有新增事件' : '尚未收到报警事件；历史只包含已接收并保存的记录') }
      ForEach(this.alarms.filter((item: AlarmEvent) => this.alarmFilter===0 || this.eventActive(item)), (item: AlarmEvent) => {
        Column({ space: 12 }) {
          Row({ space: 10 }) { this.icon($r('app.media.bell'),22); Text(alarmTitle(item.code)).fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); this.pill({ text: this.eventLabel(item), color: this.eventActive(item)?RED:TEAL, background: this.eventActive(item)?'#FFF0EC':'#EAF6F1' }) }.width('100%')
          Text(alarmTime(item)).fontSize(this.readableSize(11)).fontColor(SUB).width('100%')
          Row() { Text(item.duration ? `持续 ${item.duration} 秒` : '查看事件详情与处理备注').fontSize(this.readableSize(12)).fontColor(SUB); Blank(); Text('查看详情  ›').fontSize(this.readableSize(12)).fontColor(TEAL) }.width('100%')
        }.padding(18).backgroundColor(Color.White).borderRadius(18).width('100%').margin({ bottom: 12 }).onClick(()=>{this.navigate(1);this.showAlarm(item);})
      }, (item: AlarmEvent)=>`${alarmKey(item)}:${item.phase}:${item.duration}:${item.epoch}:${this.eventLabel(item)}`)
      this.section({ title: '远程操作', subtitle: '' })
      this.controls()
    }.width('100%')
  }
  @Builder
  trendsPage() {
    Column() {
      this.pageTitle('活动趋势', '用数据，了解日常的节奏')
      Row({ space: 12 }) { this.summary({ label: '活动次数', value: this.activityTotal.toString() }); this.summary({ label: '报警次数', value: this.alarmTotal.toString() }); this.summary({ label: '记录天数', value: this.days.length.toString() }) }.width('100%')
      this.section({ title: '每日活动分布', subtitle: this.days.length ? this.days[this.chartDay].date : '暂无日期' })
      if (this.days.length > 1) {
        Scroll() { Row({space:8}) { ForEach(this.days,(day:DayStats,index:number)=>{ Button(day.date.substring(5)).fontSize(this.readableSize(12)).backgroundColor(index===this.chartDay?INK:'#E6EDF0').fontColor(index===this.chartDay?'#FFFFFF':SUB).onClick(()=>{this.chartDay=index;this.hours=day.activity_by_hour;this.activityTotal=day.activity_total;this.alarmTotal=day.alarm_total;}) },(day:DayStats)=>`${day.date}:${day.activity_total}:${day.alarm_total}`) } }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).margin({bottom:12})
      }
      this.chart(true)
      Column({space:9}) { Text('如何理解这张图').fontSize(this.readableSize(14)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%'); Text('活动次数是人体感应从无活动变为有活动的次数，并非步数。设备关机或未覆盖的时段，不代表老人没有活动。').fontSize(this.readableSize(12)).lineHeight(this.readableLine(21)).fontColor(SUB); Text(this.demo?'当前为演示曲线，仅用于展示界面。':'仅展示设备实际保存的统计；未对时或未收到消息时，不生成虚假历史。').fontSize(this.readableSize(11)).lineHeight(this.readableLine(19)).fontColor(AMBER) }.padding(18).backgroundColor('#EDF2F3').borderRadius(18).margin({top:16})
    }.width('100%')
  }
  @Builder
  summary($$: SummaryView) { Column({space:7}) { Text($$.value).fontSize(this.readableSize(25)).fontWeight(FontWeight.Bold).fontColor(INK); Text($$.label).fontSize(this.readableSize(11)).fontColor(SUB) }.padding({top:18,bottom:18}).layoutWeight(1).backgroundColor(Color.White).borderRadius(17) }
  @Builder
  pageTitle(title: string, subtitle: string) { Column({space:7}) { Text(title).fontSize(this.readableSize(28)).fontWeight(FontWeight.Bold).fontColor(INK); Text(subtitle).fontSize(this.readableSize(12)).fontColor(SUB) }.alignItems(HorizontalAlign.Start).width('100%').margin({top:16,bottom:24}) }
  @Builder
  empty(title: string, text: string) { Column({space:12}) { this.icon($r('app.media.shield'),40); Text(title).fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK); Text(text).fontSize(this.readableSize(12)).fontColor(SUB).textAlign(TextAlign.Center).lineHeight(this.readableLine(20)) }.padding(28).width('100%').backgroundColor(Color.White).borderRadius(20) }
  @Builder
  info($$: InfoView) { Row() { Text($$.label).fontSize(this.readableSize(13)).fontColor(SUB); Blank(); Text($$.value).fontSize(this.readableSize(13)).fontColor($$.color).textAlign(TextAlign.End).layoutWeight(1).margin({left:12}) }.width('100%').padding({top:13,bottom:13}).border({width:{bottom:0.5},color:'#EEF2F3'}) }
  @Builder
  devicePage() {
    Column() {
      this.pageTitle('设备与连接', '状态透明，守护才更安心')
      Column({space:14}) {
        Row({space:14}) { Stack() { Rect().fill('#ECF5F2').width(54).height(this.elderMode ? 60 : 54).radius(18); this.icon($r('app.media.device'),30) }; Column({space:5}) { Text('通晓 TX-SMART-R').fontSize(this.readableSize(17)).fontWeight(FontWeight.Bold).fontColor(INK); Text('OpenHarmony · 居家安全终端').fontSize(this.readableSize(11)).fontColor(SUB) }.alignItems(HorizontalAlign.Start); Blank() }.width('100%')
        this.info({ label: '数据环境', value: this.demo?'本地演示':this.simulated?'模拟后端':'真实设备', color: this.demo||this.simulated?AMBER:TEAL })
        this.info({ label: '云端设备', value: this.online?'在线':'离线或待确认', color: this.online?TEAL:AMBER })
        this.info({ label: '推送连接', value: this.demo?'演示模式':!this.loggedIn?'未登录':this.pushOnline?'已连接':'未连接 · 前台轮询备用', color: this.pushOnline?TEAL:SUB })
        this.info({ label: '设备固件', value: this.firmware||'等待健康消息', color: INK })
        this.info({ label: '温湿度', value: !this.healthFresh()?'健康状态待确认':this.envValid?'读数有效':'读数无效 · 请检查', color: this.healthFresh()&&this.envValid?TEAL:AMBER })
        this.info({ label: '人体跌倒检测', value: '未开启 · 需独立穿戴设备', color: SUB })
        this.info({ label: '板端传输', value: this.demo?'演示状态':!this.healthFresh()?'状态待更新':this.fwTls?'MQTT + TLS':'普通 MQTT', color: INK })
        this.info({ label: '设备上报', value: this.age(), color: INK })
      }.padding(18).backgroundColor(Color.White).borderRadius(22).width('100%')
      this.section({ title: '连接管理', subtitle: '' })
      Button(this.loggedIn?'修改后端连接':'连接后端服务').width('100%').height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(TEAL).borderRadius(14).onClick(()=>{this.openConnection();})
      if (this.loggedIn) { Button('退出登录并返回演示').width('100%').height(45).fontSize(this.readableSize(13)).fontColor(SUB).backgroundColor('#E8EEF0').borderRadius(14).margin({top:10}).onClick(()=>{this.disconnect();}) }
      Column({space:8}) { Text('AP 与 STA').fontSize(this.readableSize(14)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%'); Text('STA：开发板连接热点后上云，本 App 通过后端访问。AP：开发板开启 HomeSafety 热点，可直接通过浏览器访问 192.168.2.1:8080/status。两种模式由板上长按 K5 切换。').fontSize(this.readableSize(12)).fontColor(SUB).lineHeight(this.readableLine(21)); Text('App 不使用开发板设备密钥登录 MQTT，不会抢占开发板连接。').fontSize(this.readableSize(11)).fontColor(TEAL).lineHeight(this.readableLine(20)) }.padding(18).backgroundColor('#EAF2F1').borderRadius(18).margin({top:18})
      if (this.error) { Text(this.error).fontSize(this.readableSize(12)).fontColor(RED).padding(12).width('100%') }
      Text('鸿颐安 1.3.0 · 比赛演示版').fontSize(this.readableSize(11)).fontColor(SUB).margin({top:24,bottom:18})
    }.width('100%')
  }
  @Builder
  assessmentCard() {
    Column({space:12}) {
      Row() { Text('守护建议').fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); this.pill({text:this.assessment().level==='normal'?'监测正常':this.assessment().level==='alert'?'优先查看':'状态说明',color:this.assessmentColor(),background:'#F0F6F4'}) }.width('100%')
      Text(this.assessment().title).fontSize(this.readableSize(18)).fontWeight(FontWeight.Medium).fontColor(this.assessmentColor()).width('100%')
      Text(this.assessment().advice).fontSize(this.readableSize(12)).fontColor(SUB).lineHeight(this.readableLine(21)).width('100%')
      Row() { Text('依据：报警 · 数据时效 · 传感器可用性').fontSize(this.readableSize(10)).fontColor(SUB); Blank(); Text('查看 ›').fontSize(this.readableSize(12)).fontColor(TEAL).onClick(()=>{this.navigate(this.assessment().level==='alert'?1:3);}) }.width('100%')
    }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(20).margin({top:16})
  }
  @Builder
  userPage() {
    Column() {
      this.pageTitle('我的守护', '把关心，变成日常的陪伴')
      Column({space:18}) {
        Row({space:14}) {
          Stack() { Circle().fill('#E2F3ED').width(62).height(62); this.icon($r('app.media.user'),33) }
          Column({space:6}) { Text(this.displayName).fontSize(this.readableSize(22)).fontWeight(FontWeight.Bold).fontColor(INK); Text(this.loggedIn?'已登录 · '+this.accountName:'体验用户 · 本地演示').fontSize(this.readableSize(12)).fontColor(SUB) }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          Text('编辑 ›').fontSize(this.readableSize(13)).fontColor(TEAL).onClick(()=>{this.editProfile();})
        }.width('100%')
        Row() { Column({space:5}) { Text('关怀对象').fontSize(this.readableSize(11)).fontColor(SUB); Text(this.careName).fontSize(this.readableSize(17)).fontWeight(FontWeight.Medium).fontColor(INK) }.alignItems(HorizontalAlign.Start); Blank(); this.pill({text:this.demo?'演示模式':this.online?'设备在线':'连接待确认',color:this.demo?AMBER:this.online?TEAL:SUB,background:'#EDF6F3'}) }.width('100%')
      }.padding(22).width('100%').backgroundColor(Color.White).borderRadius(24)
      this.assessmentCard()
      this.section({title:'显示与易用性',subtitle:'按使用者习惯选择'})
      Column({space:12}) {
        Row({space:12}) {
          Column({space:6}) {
            Text('老年人模式').fontSize(this.readableSize(16)).fontWeight(FontWeight.Bold).fontColor(INK)
            Text('放大文字与主要操作按钮').fontSize(this.readableSize(12)).fontColor(SUB)
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          Toggle({type:ToggleType.Switch,isOn:this.elderMode}).selectedColor(TEAL)
            .enabled(!this.elderModeSaving).onChange((value:boolean)=>{this.setElderMode(value);})
        }.width('100%')
        Text('只调整本机显示，不改变报警规则、服药计划或其他家属的设置。').fontSize(this.readableSize(12)).fontColor(SUB).lineHeight(this.readableLine(20)).width('100%')
        if(this.elderModeMessage){Text(this.elderModeMessage).fontSize(this.readableSize(12)).fontColor(INK).width('100%')}
      }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(20)
      this.section({title:'提醒与联系',subtitle:'按你的习惯守护'})
      Column({space:4}) {
        Row() { Column({space:6}) { Text('前台报警弹窗').fontSize(this.readableSize(14)).fontColor(INK); Text(this.alarmReminders?'收到报警后提醒，点击进入详情':'弹窗已关闭，报警仍会记录').fontSize(this.readableSize(11)).fontColor(SUB) }.alignItems(HorizontalAlign.Start).layoutWeight(1); Toggle({type:ToggleType.Switch,isOn:this.alarmReminders}).selectedColor(TEAL).onChange((value:boolean)=>{this.setReminders(value);}) }.width('100%').padding({top:10,bottom:12})
        this.info({label:'联系电话',value:this.contactPhone||'尚未填写',color:this.contactPhone?INK:SUB})
        Text('资料仅保存在此设备。App 关闭或进入后台后，当前版本不保证系统通知送达；没有弹窗不代表没有报警。').fontSize(this.readableSize(11)).fontColor(SUB).lineHeight(this.readableLine(19)).width('100%').padding({top:8,bottom:4})
        if (this.demo) { Button('体验报警提醒').width('100%').height(this.elderMode ? 60 : 42).fontSize(this.readableSize(13)).backgroundColor('#EAF5F1').fontColor(TEAL).borderRadius(12).margin({top:8}).onClick(()=>{this.previewAlert();}) }
      }.padding(18).width('100%').backgroundColor(Color.White).borderRadius(20)
      this.section({title:'账户与帮助',subtitle:''})
      Column() {
        Row() { Text('账户与连接').fontSize(this.readableSize(14)).fontColor(INK); Blank(); Text(this.loggedIn?'管理 ›':'登录 ›').fontSize(this.readableSize(13)).fontColor(TEAL) }.width('100%').padding({top:16,bottom:16}).onClick(()=>{this.openConnection();})
        Row() { Text('查看设备状态').fontSize(this.readableSize(14)).fontColor(INK); Blank(); Text('›').fontSize(this.readableSize(18)).fontColor(SUB) }.width('100%').padding({top:16,bottom:16}).border({width:{top:0.5},color:'#EEF2F3'}).onClick(()=>{this.navigate(3);})
        this.info({label:'应用版本',value:'鸿颐安 1.2.1',color:INK})
        Text('守护建议用于解释监测状态，不提供个人健康评分或诊断。报警需结合现场核实，确认处理与设备消音含义不同。').fontSize(this.readableSize(11)).fontColor(SUB).lineHeight(this.readableLine(20)).width('100%').padding({top:12,bottom:8})
      }.padding({left:18,right:18}).width('100%').backgroundColor(Color.White).borderRadius(20)
      if (this.loggedIn) { Button('退出登录').width('100%').height(this.elderMode ? 60 : 46).fontSize(this.readableSize(14)).fontColor(RED).backgroundColor('#FBEDEA').borderRadius(14).margin({top:20}).onClick(()=>{this.disconnect();}) }
      Text('鸿颐安 · 让关心及时抵达').fontSize(this.readableSize(11)).fontColor(SUB).margin({top:24,bottom:20})
    }.width('100%')
  }
  @Builder
  profileSheet() {
    Scroll() {
      Column({space:14}) {
        Row() { Text('守护资料').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).onClick(()=>{if(!this.profileSaving){this.profileOpen=false;}}) }.width('100%')
        Text('这些称呼与联系电话仅保存在本机，可随时修改。').fontSize(this.readableSize(12)).fontColor(SUB).lineHeight(this.readableLine(20)).width('100%')
        Text('你的昵称').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')
        TextInput({text:this.nameDraft,placeholder:'例如：小安'}).height(this.elderMode ? 60 : 46).fontSize(this.readableSize(14)).maxLength(20).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.nameDraft=value;})
        Text('关怀对象称呼').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')
        TextInput({text:this.careDraft,placeholder:'例如：家人'}).height(this.elderMode ? 60 : 46).fontSize(this.readableSize(14)).maxLength(20).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.careDraft=value;})
        Text('联系电话（可选）').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')
        TextInput({text:this.contactDraft,placeholder:'方便核实时联系'}).height(this.elderMode ? 60 : 46).fontSize(this.readableSize(14)).maxLength(24).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.contactDraft=value;})
        if(this.profileError){Text(this.profileError).fontSize(this.readableSize(12)).fontColor(RED).width('100%')}
        Button(this.profileSaving?'正在保存…':'保存资料').height(this.elderMode ? 60 : 46).width('100%').backgroundColor(TEAL).borderRadius(14).enabled(!this.profileSaving).onClick(()=>{this.saveProfile();})
      }.padding(24).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(Color.White).borderRadius(24).scrollBar(BarState.Off)
  }
  @Builder
  incomingSheet() {
    Column({space:18}) {
      Column({space:12}) {
        Stack() { Circle().fill('#FBEAE6').width(60).height(60); this.icon($r('app.media.bell'),32) }
        Text(this.demo?'演示报警提醒':this.alertQueue.length&&this.alertQueue[0].epoch>0&&this.now-this.alertQueue[0].epoch*1000>120000?'报警记录待核实':'收到设备报警').fontSize(this.readableSize(13)).fontColor(RED)
        Text(this.alertQueue.length?alarmTitle(this.alertQueue[0].code):'设备报警').fontSize(this.readableSize(26)).fontWeight(FontWeight.Bold).fontColor(INK).textAlign(TextAlign.Center)
        Text(this.careName+'的居家终端，请及时查看并联系现场核实。').fontSize(this.readableSize(13)).fontColor(SUB).lineHeight(this.readableLine(22)).textAlign(TextAlign.Center)
        Text(this.alertQueue.length?alarmTime(this.alertQueue[0]):'').fontSize(this.readableSize(11)).fontColor(SUB)
        if(this.alertQueue.length>1){Text('还有 '+(this.alertQueue.length-1)+' 条提醒待查看').fontSize(this.readableSize(12)).fontColor(AMBER)}
      }.width('100%').onClick(()=>{this.openAlert();})
      Button('查看并处理').width('100%').height(this.elderMode ? 60 : 48).backgroundColor(RED).borderRadius(14).fontSize(this.readableSize(15)).onClick(()=>{this.openAlert();})
      Text('稍后查看').fontSize(this.readableSize(13)).fontColor(SUB).padding(8).onClick(()=>{this.alertQueue=this.alertQueue.slice(1);})
      Text(this.demo?'此提醒仅为本地演示，不操作真实设备':'查看提醒不会自动确认报警或消音').fontSize(this.readableSize(10)).fontColor(SUB).textAlign(TextAlign.Center)
    }.padding(26).width('90%').backgroundColor(Color.White).borderRadius(28)
  }
  @Builder
  navItem(index: number, icon: Resource, title: string) {
    Column({space:5}) { Image(icon).width(22).height(22).opacity(this.tab===index?1:0.45); Text(title).fontSize(this.readableSize(10)).fontWeight(this.tab===index?FontWeight.Bold:FontWeight.Normal).fontColor(this.tab===index?TEAL:SUB) }.height(this.elderMode ? 60 : 54).justifyContent(FlexAlign.Center).layoutWeight(1).onClick(()=>{this.navigate(index);})
  }
  @Builder
  settingsSheet() {
    Column({space:14}) {
      Row() { Text('连接后端').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).onClick(()=>{this.settingsOpen=false;this.passwordDraft='';}) }.width('100%')
      Text('登录后查询真实设备；不会把云账号密钥放进 App。').fontSize(this.readableSize(12)).fontColor(SUB).width('100%').lineHeight(this.readableLine(20))
      TextInput({text:this.serverDraft,placeholder:'https://backend.example.com'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.serverDraft=value;})
      Text('使用公网服务器地址').fontSize(this.readableSize(12)).fontColor(TEAL).width('100%').onClick(()=>{this.serverDraft='https://backend.example.com';})
      TextInput({text:this.userDraft,placeholder:'用户名'}).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.userDraft=value;})
      if (this.loggedIn) { Text('本次登录密码可点右侧眼睛查看；退出或重启后清除。').fontSize(this.readableSize(11)).fontColor(SUB).width('100%') }
      TextInput({text:this.passwordDraft,placeholder:'后端登录密码'}).type(InputType.Password).height(this.elderMode ? 60 : 48).fontSize(this.readableSize(14)).backgroundColor(BG).borderRadius(12).onChange((value:string)=>{this.passwordDraft=value;})
      if (this.error) { Text(this.error).fontSize(this.readableSize(12)).fontColor(RED).width('100%') }
      Button(this.loginBusy?'正在登录…':'登录并同步').width('100%').height(this.elderMode ? 60 : 48).backgroundColor(TEAL).borderRadius(14).enabled(!this.loginBusy&&this.passwordDraft.length>0).onClick(()=>{this.login();})
      Text('公网服务器使用上方 IP 地址；登录后同步真实设备。USB 演示可改填 https://127.0.0.1:18443，并运行电脑端演示连接脚本。').fontSize(this.readableSize(11)).fontColor(SUB).lineHeight(this.readableLine(19))
    }.padding(24).width('94%').backgroundColor(Color.White).borderRadius(24)
  }
  @Builder
  alarmSheet() {
    Scroll() {
    Column({space:14}) {
      Row() { Text(this.selected?alarmTitle(this.selected.code):'事件详情').fontSize(this.readableSize(23)).fontWeight(FontWeight.Bold).fontColor(INK); Blank(); Text('关闭').fontSize(this.readableSize(13)).fontColor(SUB).onClick(()=>{this.closeAlarm();}) }.width('100%')
      Text(this.selected?alarmTime(this.selected):'').fontSize(this.readableSize(12)).fontColor(SUB).width('100%')
      this.pill({text:this.selected?this.eventLabel(this.selected):'事件详情',color:this.selected&&this.eventActive(this.selected)?RED:TEAL,background:'#F4F7F8'})
      if (this.selected && this.selected.code === 4) { Text('设备上报长时间未检测到活动。可能与静坐、睡眠、离开感应范围或突发意外有关，不能据此确认跌倒。请先联系老人，必要时安排现场查看；远程消音不能代替核实。').fontSize(this.readableSize(13)).fontColor(AMBER).lineHeight(this.readableLine(22)).width('100%') }
      Text('当前终端操作：确认或消音不会删除这条事件记录。').fontSize(this.readableSize(11)).fontColor(SUB).width('100%').lineHeight(this.readableLine(19))
      this.controls()
      Text('处理备注').fontSize(this.readableSize(15)).fontWeight(FontWeight.Bold).fontColor(INK).width('100%')
      ForEach(this.notes,(note:NoteItem,index:number)=>{ Text(`${resolutionLabel(note.resolution)} · ${note.username||'护理人员'}：${note.note}`).fontSize(this.readableSize(12)).fontColor(SUB).padding(10).width('100%').backgroundColor(BG).borderRadius(10) },(_note:NoteItem,index:number)=>index.toString())
      Row({space:8}) { Button('已处理').fontSize(this.readableSize(11)).backgroundColor(this.resolution==='handled'?TEAL:'#E7EDF0').fontColor(this.resolution==='handled'?'#FFFFFF':SUB).enabled(!this.noteSaving).onClick(()=>{this.resolution='handled';}); Button('误报').fontSize(this.readableSize(11)).backgroundColor(this.resolution==='false_alarm'?TEAL:'#E7EDF0').fontColor(this.resolution==='false_alarm'?'#FFFFFF':SUB).enabled(!this.noteSaving).onClick(()=>{this.resolution='false_alarm';}); Button('需跟进').fontSize(this.readableSize(11)).backgroundColor(this.resolution==='follow_up'?TEAL:'#E7EDF0').fontColor(this.resolution==='follow_up'?'#FFFFFF':SUB).enabled(!this.noteSaving).onClick(()=>{this.resolution='follow_up';}) }.width('100%')
      Text('先选择处置结果，再点击“保存处理记录”。成功后关闭详情；只保存这条事件的处理说明，不解除设备报警。').fontSize(this.readableSize(11)).fontColor(SUB).width('100%')
      TextArea({text:this.noteDraft,placeholder:'可选：补充联系家属、现场查看等处理情况…'}).maxLength(2000).enabled(!this.noteSaving).height(90).fontSize(this.readableSize(13)).backgroundColor(BG).onChange((value:string)=>{this.noteDraft=value;})
      Button(this.noteSaving?'正在保存…':'保存处理记录').height(this.elderMode ? 60 : 44).width('100%').backgroundColor(TEAL).enabled(!this.noteSaving&&this.selected!==null).onClick(()=>{this.saveNote();})
      if(this.noteMessage){Text(this.noteMessage).fontSize(this.readableSize(12)).fontColor(this.noteMessage.indexOf('失败')>=0?RED:TEAL).width('100%')}
      if (this.demo) { Text('演示操作不会发送到真实设备').fontSize(this.readableSize(11)).fontColor(AMBER) }
    }.padding(22).width('100%')
    }.width('94%').constraintSize({maxHeight:'84%'}).backgroundColor(Color.White).borderRadius(24).scrollBar(BarState.Off)
  }
  build() {
    Stack({alignContent:Alignment.Center}) {
      Column() {
        if (this.demo || this.simulated) { Text(this.demo?'演示环境 · 所有数据仅用于展示':'模拟后端 · 非真实设备数据').fontSize(this.readableSize(10)).fontColor('#98712E').width('100%').textAlign(TextAlign.Center).padding({top:5,bottom:5}).backgroundColor('#FFF3DE') }
        Scroll(this.pageScroller) {
          Column() { if(this.tab===0){this.overview()} else if(this.tab===1){this.alarmsPage()} else if(this.tab===2){this.trendsPage()} else if(this.tab===3){this.devicePage()} else {this.userPage()} }.padding({left:20,right:20,bottom:24}).width('100%')
        }.layoutWeight(1).scrollBar(BarState.Off).width('100%')
        Row() { this.navItem(0,$r('app.media.home'),'总览'); this.navItem(1,$r('app.media.bell'),'报警'); this.navItem(2,$r('app.media.chart'),'趋势'); this.navItem(3,$r('app.media.device'),'设备'); this.navItem(4,$r('app.media.user'),'我的') }.width('100%').padding({left:12,right:12,top:4,bottom:12}).backgroundColor(Color.White).border({width:{top:0.5},color:'#E5ECEF'})
      }.width('100%').height('100%').backgroundColor(BG).padding({top:28,bottom:18})
      if (this.settingsOpen || this.profileOpen || this.livingOpen || this.selected!==null) {
        Rect().fill('#660B2028').width('100%').height('100%').onClick(()=>{if(!this.loginBusy&&!this.profileSaving&&!this.livingBusy&&!this.noteSaving){this.noteOperation++;this.livingOpen=false;this.settingsOpen=false;this.profileOpen=false;this.selected=null;this.passwordDraft='';}})
        if(this.livingOpen){this.livingSheet()}else if(this.settingsOpen){this.settingsSheet()}else if(this.profileOpen){this.profileSheet()}else{this.alarmSheet()}
      }
      if (this.alertQueue.length > 0 && !this.settingsOpen && !this.profileOpen && !this.livingOpen && this.selected === null) {
        Rect().fill('#880B2028').width('100%').height('100%')
        this.incomingSheet()
      }
      if(this.medicineNotice && this.alertQueue.length===0 && !this.settingsOpen && !this.profileOpen && !this.livingOpen && this.selected===null && this.livingFresh()) {
        Rect().fill('#880B2028').width('100%').height('100%')
        this.medicineSheet()
      }
    }.width('100%').height('100%')
  }
}
