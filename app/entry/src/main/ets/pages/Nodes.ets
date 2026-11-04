import { router } from '@kit.ArkUI';
import { CareTheme } from '../theme/CareTheme';
import { DeviceSession } from '../services/DeviceSession';
import { Envelope } from '../models/Models';
import { DistributedFamily, DistributedNode, EnvironmentProperties, EnvironmentShadow, EnvironmentHealth,
  WearReading, WearHour, WearFall, WearHours, WearFalls, sensorText, motionText, roomDoorText,
  watchVital, heartText, environmentNodes, readingFresh, reportAge, hourLabel } from '../models/Distributed';

interface NodeRoute { kind?: string; }
interface NodeMetric { label: string; value: string; unit: string; symbol: Resource; }
interface NodeInformation { label: string; value: string; }

@Entry
@Component
struct Nodes {
  @State mode: string = 'environment';
  @State nodes: DistributedNode[] = [];
  @State environmentId: string = '';
  @State environment: EnvironmentProperties = {};
  @State environmentStamp: string = '';
  @State environmentOnline: boolean | null = null;
  @State watch: WearReading | null = null;
  @State hours: WearHour[] = [];
  @State falls: WearFall[] = [];
  @State environmentError: string = '';
  @State watchError: string = '';
  @State hoursError: string = '';
  @State fallsError: string = '';
  @State catalogMessage: string = '';
  @State busy: boolean = false;
  @State now: number = Date.now();
  @State authenticated: boolean = false;
  private active: boolean = false;
  private generation: number = 0;
  private timer: number = -1;

  aboutToAppear(): void {
    const params = router.getParams() as NodeRoute;
    this.mode = params && params.kind === 'wearable' ? 'wearable' : 'environment';
    this.active = true;
    this.authenticated = DeviceSession.backend.token !== '';
    if (this.authenticated) { this.loadCatalog(); }
    this.timer = setInterval(() => { this.now = Date.now(); if (this.authenticated) { this.refresh(); } }, 6000);
  }
  aboutToDisappear(): void {
    this.active = false; this.generation++;
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
  }
  private async loadCatalog(): Promise<void> {
    const api = DeviceSession.backend;
    const token = api.token;
    try {
      const family = await api.refreshHousehold();
      if (!this.active || token !== api.token) { return; }
      this.nodes = environmentNodes(family.devices || []);
      this.catalogMessage = '按设备独立采集，互不覆盖';
    } catch (_) {
      if (!this.active || token !== api.token) { return; }
      this.nodes = environmentNodes(api.family.devices);
      this.catalogMessage = this.nodes.length ? '使用已授权设备清单，正在等待恢复连接' : '无法取得带类型的设备清单，请更新后端并重新登录';
    }
    if (!this.environmentId) { this.environmentId = this.nodes.length ? this.nodes[0].device_id : ''; }
    this.refresh();
  }
  private nodePath(device: string, suffix: string): string {
    return '/api/v1/devices/' + encodeURIComponent(device) + '/' + suffix;
  }
  private current(operation: number): boolean { return this.active && operation === this.generation; }
  private async readEnvironment(operation: number): Promise<void> {
    const id = this.environmentId;
    if (!id) { return; }
    const api = DeviceSession.backend;
    // Read each endpoint independently; missing health must not erase a valid shadow.
    let shadow: EnvironmentShadow | null = null;
    let health: EnvironmentHealth | null = null;
    let shadowError: string = '';
    let healthError: string = '';
    await Promise.all([
      api.request(this.nodePath(id,'shadow')).then((r: Envelope) => { shadow = r.data as EnvironmentShadow; })
        .catch((e: Error) => { shadowError = e.message; }),
      api.request(this.nodePath(id,'health')).then((r: Envelope) => { health = r.data as EnvironmentHealth; })
        .catch((e: Error) => { healthError = e.message; })
    ]);
    if (!this.current(operation) || id !== this.environmentId) { return; }
    const snapshot = shadow as EnvironmentShadow | null;
    const diagnostic = health as EnvironmentHealth | null;
    const props = snapshot ? snapshot.properties : null;
    if (props && (props.light !== undefined || props.smoke !== undefined || props.body !== undefined || props.door !== undefined)) {
      this.environment = props;
      this.environmentStamp = snapshot && snapshot.updated_at ? snapshot.updated_at : '';
    } else if (diagnostic) {
      this.environment = diagnostic;
      this.environmentStamp = diagnostic.epoch && diagnostic.epoch > 1600000000 ? new Date(diagnostic.epoch * 1000).toISOString() : '';
    } else { this.environment = {}; this.environmentStamp = ''; }
    this.environmentOnline = snapshot && typeof snapshot.online === 'boolean' ? snapshot.online : null;
    this.environmentError = shadowError && healthError ? '环境节点读取失败，请检查设备、网络与后端授权' :
      !props && !diagnostic ? '已连接后端，等待此环境节点上报' : '';
  }
  private async readWatch(operation: number): Promise<void> {
    const api = DeviceSession.backend;
    // 1.4.0 and 1.4.1 both expose wearable data through the primary device.
    const path = api.path('watch');
    await Promise.all([
      api.request(path).then((r: Envelope) => {
        if (this.current(operation)) { this.watch = r.data as WearReading; this.watchError = ''; }
      }).catch((e: Error) => {
        if (this.current(operation)) { this.watchError = '暂未收到手表数据，请确认设备已联网并绑定到当前家庭'; }
      }),
      api.request(path + '/hours?limit=12').then((r: Envelope) => {
        if (this.current(operation)) { this.hours = ((r.data as WearHours).items || []).slice().sort((a: WearHour,b: WearHour) => a.hour.localeCompare(b.hour)); this.hoursError = ''; }
      }).catch((e: Error) => { if (this.current(operation)) { this.hoursError = '小时统计暂不可用'; } }),
      api.request(path + '/falls?limit=20').then((r: Envelope) => {
        if (this.current(operation)) { this.falls = (r.data as WearFalls).items || []; this.fallsError = ''; }
      }).catch((e: Error) => { if (this.current(operation)) { this.fallsError = '跌倒记录暂不可用'; } })
    ]);
  }
  private async refresh(): Promise<void> {
    if (!this.active || !this.authenticated || this.busy) { return; }
    const operation = this.generation; this.busy = true;
    try {
      if (this.mode === 'wearable') { await this.readWatch(operation); }
      else { await this.readEnvironment(operation); }
    } finally { if (this.current(operation)) { this.busy = false; this.now = Date.now(); } }
  }
  private selectMode(mode: string): void {
    if (mode === this.mode) { return; }
    this.generation++; this.busy = false; this.mode = mode; this.refresh();
  }
  private selectNode(id: string): void {
    if (id === this.environmentId) { return; }
    this.generation++; this.busy = false; this.environmentId = id;
    this.environment = {}; this.environmentStamp = ''; this.environmentError = ''; this.environmentOnline = null;
    this.refresh();
  }
  private watchValue(field: string): string {
    if (!this.watch) { return '待上报'; }
    if (field === 'heart') { return heartText(this.watch); }
    if (field === 'oxygen') { return watchVital(this.watch.spo2,'',100); }
    return sensorText(this.watch.steps,'');
  }
  private stamp(): string { return this.mode === 'wearable' ? this.watch && this.watch.received_at ? this.watch.received_at : '' : this.environmentStamp; }
  private timeText(stamp: string): string { const at = Date.parse(stamp); return Number.isFinite(at) ? new Date(at).toLocaleString() : '采集时间待确认'; }
  private fallText(): string {
    if (!this.watch) { return '等待上报'; }
    return this.watch.fall_active === true || this.watch.fall_state === 2 ? '疑似跌倒 · 请联系核实' :
      this.watch.fall_state === 1 ? '动作异常 · 观察中' : '本次未报告跌倒';
  }
  private barHeight(hour: WearHour): number { return hour.hr && typeof hour.hr.avg === 'number' && hour.hr.avg > 0 ? Math.min(110, Math.max(6, hour.hr.avg / 180 * 110)) : 3; }
  private hourlyText(hour: WearHour): string { return hour.hr && typeof hour.hr.avg === 'number' && hour.hr.avg > 0 ? hour.hr.avg.toFixed(0) : '—'; }

  @Builder
  metric($$: NodeMetric) {
    Column({space:9}) {
      Image($$.symbol).width(23).height(23).objectFit(ImageFit.Contain)
      Text($$.label).fontSize(13).fontColor(CareTheme.secondary)
      Text($$.value).fontSize(25).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink).maxLines(1).minFontSize(16).maxFontSize(25)
      Text($$.unit).fontSize(11).fontColor(CareTheme.secondary)
    }.padding({top:20,bottom:20,left:8,right:8}).layoutWeight(1).backgroundColor(Color.White).borderRadius(16)
  }
  @Builder
  information($$: NodeInformation) {
    Row() { Text($$.label).fontSize(14).fontColor(CareTheme.secondary); Blank(); Text($$.value).fontSize(14).fontColor(CareTheme.ink).textAlign(TextAlign.End).layoutWeight(1) }
      .width('100%').padding({top:14,bottom:14}).border({width:{bottom:0.5},color:CareTheme.line})
  }
  @Builder
  environmentView() {
    if (this.nodes.length) {
      Scroll() {
        Row({space:8}) {
          ForEach(this.nodes,(node: DistributedNode) => {
            Button(node.name || '环境节点').fontSize(13).backgroundColor(node.device_id === this.environmentId ? CareTheme.primary : Color.White)
              .fontColor(node.device_id === this.environmentId ? Color.White : CareTheme.primary).onClick(() => { this.selectNode(node.device_id); })
          },(node: DistributedNode) => node.device_id)
        }
      }.scrollable(ScrollDirection.Horizontal).width('100%').scrollBar(BarState.Off)
      Row({space:12}) {
        this.metric({label:'光照采样',value:sensorText(this.environment.light,''),unit:'mV · ADC 原始电压',symbol:$r('app.media.sun')})
        this.metric({label:'烟雾采样',value:sensorText(this.environment.smoke,''),unit:'mV · ADC 原始电压',symbol:$r('app.media.flame')})
      }.width('100%')
      Column() {
        this.information({label:'人体活动',value:motionText(this.environment.body)})
        this.information({label:'房门状态',value:roomDoorText(this.environment.door)})
        if (this.environment.temp !== null && this.environment.temp !== undefined) { this.information({label:'温度',value:sensorText(this.environment.temp,' °C',1)}) }
        if (this.environment.humi !== null && this.environment.humi !== undefined) { this.information({label:'湿度',value:sensorText(this.environment.humi,' %',1)}) }
        this.information({label:'云端登记状态',value:this.environmentOnline === true ? '在线' : this.environmentOnline === false ? '离线（以采集时间辅助核实）' : '待确认'})
      }.padding({left:18,right:18}).width('100%').backgroundColor(Color.White).borderRadius(16)
      if (this.environmentError) { Text(this.environmentError).fontSize(13).fontColor(CareTheme.warmInk).width('100%') }
      Text('光照与烟雾目前是传感器原始电压，不等同于照度或烟雾浓度；不据此给出安全结论。PIR 检测活动，不能确认静止人员是否在场。')
        .fontSize(12).fontColor(CareTheme.secondary).lineHeight(21).width('100%')
      Text('设备 ' + this.environmentId).fontSize(10).fontColor(CareTheme.secondary).width('100%').wordBreak(WordBreak.BREAK_ALL)
    } else {
      Column({space:12}) {
        Image($r('app.media.device')).width(40).height(40)
        Text('环境节点尚未绑定').fontSize(20).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink)
        Text('后端登录返回的设备清单中需要包含星闪环境节点，且当前账号具有查看权限。').fontSize(14).fontColor(CareTheme.secondary).lineHeight(23)
      }.padding(24).width('100%').backgroundColor(Color.White).borderRadius(18)
    }
  }
  @Builder
  wearableView() {
    Row({space:10}) {
      this.metric({label:'心率',value:this.watchValue('heart'),unit:'次 / 分',symbol:$r('app.media.activity')})
      this.metric({label:'血氧',value:this.watchValue('oxygen'),unit:'百分比 %',symbol:$r('app.media.droplet')})
      this.metric({label:'累计步数',value:this.watchValue('steps'),unit:'设备计步器',symbol:$r('app.media.chart')})
    }.width('100%')
    if (this.watchError) { Text(this.watchError).fontSize(13).fontColor(CareTheme.warmInk).width('100%') }
    Column() {
      this.information({label:'电量',value:this.watch&&this.watch.battery_percent!==null&&this.watch.battery_percent!==undefined?sensorText(this.watch.battery_percent,'%'):'未采集'})
      this.information({label:'跌倒监测',value:this.fallText()})
      this.information({label:'累计判定次数',value:this.watch ? sensorText(this.watch.fall_count,' 次') : '待上报'})
      this.information({label:'活动强度',value:this.watch ? sensorText(this.watch.dynamic_mg,' mg') : '待上报'})
      Text('设备没有测到心率或血氧时显示“待测量”。步数为设备累计值，重启可能归零；跌倒结果需要人工联系核实。')
        .fontSize(12).fontColor(CareTheme.secondary).lineHeight(21).width('100%').padding({top:12,bottom:16})
    }.padding({left:18,right:18}).width('100%').backgroundColor(Color.White).borderRadius(16)
    Row() { Text('心率小时趋势').fontSize(19).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink); Blank(); Text('最近 12 桶').fontSize(12).fontColor(CareTheme.secondary) }.width('100%')
    if (this.hours.length) {
      Scroll() {
        Row({space:12}) {
          ForEach(this.hours,(hour: WearHour) => {
            Column({space:7}) {
              Text(this.hourlyText(hour)).fontSize(11).fontColor(CareTheme.primary)
              Column() { Blank(); Rect().fill(hour.hr && hour.hr.n ? CareTheme.primary : CareTheme.line).width(17).height(this.barHeight(hour)).radius(4) }.height(112)
              Text(hourLabel(hour.hour)).fontSize(10).fontColor(CareTheme.secondary)
              Text('+' + sensorText(hour.steps_delta,'')).fontSize(10).fontColor(CareTheme.secondary)
            }.width(38)
          },(hour: WearHour) => hour.hour + ':' + (hour.hr ? hour.hr.avg : '') + ':' + hour.steps_delta)
        }.padding(16)
      }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).width('100%').backgroundColor(Color.White).borderRadius(16)
      Text('上方为有效心率均值，下方为该小时步数增量；没有测量的时段不补零。').fontSize(11).fontColor(CareTheme.secondary).width('100%')
    } else { Text(this.hoursError || '还没有小时统计，真实上报后会自动出现。').fontSize(13).fontColor(CareTheme.secondary).padding(20).width('100%').backgroundColor(Color.White).borderRadius(16) }
    Text('跌倒判定记录').fontSize(19).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink).width('100%')
    Column() {
      ForEach(this.falls,(fall: WearFall) => {
        Column({space:6}) {
          Text('疑似跌倒 · 第 ' + fall.fall_count + ' 次').fontSize(15).fontColor(CareTheme.alarm).width('100%')
          Text(this.timeText(fall.detected_at)).fontSize(12).fontColor(CareTheme.secondary).width('100%')
          Text('当时心率 ' + watchVital(fall.heart_rate,' 次/分',240) + ' · 血氧 ' + watchVital(fall.spo2,' %',100)).fontSize(12).fontColor(CareTheme.secondary).width('100%')
        }.padding({top:15,bottom:15}).width('100%').border({width:{bottom:0.5},color:CareTheme.line})
      },(fall: WearFall) => fall.key || fall.detected_at + ':' + fall.fall_count)
      if (!this.falls.length) { Text(this.fallsError || '暂无跌倒判定记录').fontSize(13).fontColor(CareTheme.secondary).padding({top:20,bottom:20}).width('100%') }
    }.padding({left:18,right:18}).width('100%').backgroundColor(Color.White).borderRadius(16)
    Text('此处展示手表原始判定记录。统一报警处理与抓拍联动尚未接通，查看记录不会解除手表报警。').fontSize(12).fontColor(CareTheme.secondary).lineHeight(21).width('100%')
  }
  build() {
    Column() {
      Row() {
        Button('‹ 返回').fontSize(15).fontColor(CareTheme.primary).backgroundColor(Color.Transparent).onClick(() => { router.back(); })
        Blank(); Text('颐鸿安').fontSize(15).fontWeight(FontWeight.Medium).fontColor(CareTheme.primary)
      }.width('100%').padding({left:12,right:24,top:8,bottom:8})
      Scroll() {
        Column({space:18}) {
          Column({space:7}) {
            Text(this.mode === 'wearable' ? '随身健康' : '房间环境').fontSize(29).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).width('100%')
            Text(this.mode === 'wearable' ? '星闪手表 · 把关心带在身边' : '星闪环境节点 · 多个房间，独立守护').fontSize(14).fontColor(CareTheme.secondary).width('100%')
          }.width('100%')
          Row({space:8}) {
            Button('房间环境').layoutWeight(1).fontSize(15).backgroundColor(this.mode === 'environment' ? CareTheme.primary : Color.White).fontColor(this.mode === 'environment' ? Color.White : CareTheme.primary).onClick(() => { this.selectMode('environment'); })
            Button('随身手表').layoutWeight(1).fontSize(15).backgroundColor(this.mode === 'wearable' ? CareTheme.primary : Color.White).fontColor(this.mode === 'wearable' ? Color.White : CareTheme.primary).onClick(() => { this.selectMode('wearable'); })
          }.width('100%')
          if (!this.authenticated) {
            Text('请返回“设备 → 连接后端服务”登录，再查看真实节点数据。这里不会用演示值代替实测。').fontSize(15).fontColor(CareTheme.secondary).lineHeight(25).padding(24).backgroundColor(Color.White).borderRadius(18)
          } else {
            Row({space:8}) {
              Circle().fill(readingFresh(this.stamp(),this.now) ? CareTheme.primary : CareTheme.warmInk).width(7).height(7)
              Text(reportAge(this.stamp(),this.now)).fontSize(12).fontColor(CareTheme.secondary).layoutWeight(1)
              Button(this.busy ? '读取中' : '刷新').fontSize(12).fontColor(CareTheme.primary).backgroundColor(Color.Transparent).enabled(!this.busy).onClick(() => { this.refresh(); })
            }.width('100%')
            if (this.mode === 'wearable') { this.wearableView() } else { this.environmentView() }
            Text('前台每 6 秒查询 · 新读数取决于设备上报周期').fontSize(11).fontColor(CareTheme.secondary).width('100%')
            Text(this.catalogMessage).fontSize(11).fontColor(CareTheme.secondary).width('100%')
          }
        }.padding({left:22,right:22,top:12,bottom:32}).width('100%')
      }.layoutWeight(1).width('100%').scrollBar(BarState.Off)
    }.width('100%').height('100%').backgroundColor(CareTheme.background).padding({top:24,bottom:16})
  }
}
