import { router, promptAction } from '@kit.ArkUI';
import { http } from '@kit.NetworkKit';
import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { ChatMedia, ChatAsset, UploadedChatAsset } from '../services/ChatMedia';
import { CareTheme } from '../theme/CareTheme';
import { DeviceSession } from '../services/DeviceSession';
import { Push } from '../models/Models';
import { ChatContact, ChatMessage, Conversation, ChatContacts, ChatConversations, ChatHistory, ChatInvite, mergeMessages, messagePreview } from '../models/Messaging';

@Entry
@Component
struct Messages {
  @State conversations: Conversation[] = [];
  @State contacts: ChatContact[] = [];
  @State messages: ChatMessage[] = [];
  @State selected: Conversation | null = null;
  @State draft: string = '';
  @State status: string = '';
  @State syncStatus: string = '';
  @State tab: number = 0;
  @State loading: boolean = false;
  @State sending: boolean = false;
  @State managing: boolean = false;
  @State online: boolean = false;
  @State authorized: boolean = true;
  @State invite: string = '';
  @State code: string = '';
  @State groupName: string = '';
  @State picked: string[] = [];
  @State hasMore: boolean = false;
  @State unseen: boolean = false;
  @State recording: boolean = false;
  @State recordBusy: boolean = false;
  @State recordSeconds: number = 0;
  @State attachment: string = '';
  @State assetPreview: image.PixelMap | undefined = undefined;
  @State mediaBusy: boolean = false;
  @State thumbnails: Record<string, image.PixelMap> = {};
  @State playing: string = '';
  @State groupOpen: boolean = false;
  @State rename: string = '';
  @State addName: string = '';
  private media: ChatMedia = new ChatMedia();
  private asset: ChatAsset | null = null;
  private assetId: string = '';
  private assetKey: string = '';
  private assetCid: string = '';
  private recordTimer: number = -1;
  private recordStarted: number = 0;
  private before: number = 0;
  private gapBefore: number = 0;
  private gapStop: string = '';
  private active: boolean = false;
  private generation: number = 0;
  private timer: number = -1;
  private token: string = '';
  private retryBody: string = '';
  private retryKey: string = '';
  private scroller: Scroller = new Scroller();

  onPageShow(): void {
    if (this.active) { return; }
    this.active = true; this.token = DeviceSession.backend.token;
    this.authorized = this.token !== '';
    if (!this.authorized) { this.status = '请返回账户页面登录后使用消息'; return; }
    this.refresh();
    DeviceSession.backend.startPush((event: Push) => {
      if (event.type === 'message' || event.type === 'resync_required') { this.refresh(); }
      if (event.type === 'alarm') { promptAction.showToast({ message: '收到新报警，请返回报警页查看', duration: 3000 }); }
    }, (connected: boolean) => { if (this.active) { this.online = connected; } });
    this.timer = setInterval(() => { this.refresh(); }, 4000);
  }
  onPageHide(): void {
    this.active = false; this.generation++; this.loading = false;
    DeviceSession.backend.stopPush();
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
    this.clearMedia();
  }
  aboutToDisappear(): void { this.onPageHide(); }
  onBackPress(): boolean { if (this.selected) { this.back(); return true; } return false; }
  private clearMedia(): void {
    this.media.close(); this.recording = false; this.recordBusy = false; this.recordSeconds = 0; this.playing = '';
    if (this.recordTimer !== -1) { clearInterval(this.recordTimer); this.recordTimer = -1; }
    this.asset = null; this.attachment = ''; this.assetId = ''; this.assetKey = ''; this.assetCid = '';
    if (this.assetPreview) { this.assetPreview.release(); this.assetPreview = undefined; }
    Object.keys(this.thumbnails).forEach((key: string) => { this.thumbnails[key].release(); }); this.thumbnails = {};
  }
  private current(op: number): boolean {
    if (this.token !== DeviceSession.backend.token || !this.token) {
      this.authorized = false; this.conversations = []; this.contacts = []; this.messages = [];
      this.selected = null; this.draft = ''; this.invite = ''; this.code = ''; this.picked = [];
      this.status = '账号已失效或切换，请返回重新登录';
      this.clearMedia();
      return false;
    }
    return this.active && op === this.generation;
  }
  private path(cid: string): string { return '/api/v1/conversations/' + encodeURIComponent(cid); }
  private title(item: Conversation): string { return item.kind === 'direct' && item.member_count < 2 ? '历史会话 · 对方已不可用' : item.title || '家庭会话'; }
  private writable(): boolean { return !!this.selected && (this.selected.kind === 'group' || this.selected.member_count >= 2); }
  private error(error: Error): void {
    this.current(this.generation);
    if (this.authorized) { this.status = error.message.includes('404') ? '服务器消息接口尚未更新' : error.message; }
  }
  private async refresh(): Promise<void> {
    if (!this.active || !this.authorized || this.loading) { return; }
    const op = this.generation; this.loading = true;
    try {
      const api = DeviceSession.backend;
      const result = await Promise.all([api.request('/api/v1/conversations', http.RequestMethod.GET, '', 10000), api.request('/api/v1/contacts', http.RequestMethod.GET, '', 10000)]);
      if (!this.current(op)) { return; }
      this.conversations = (result[0].data as ChatConversations).items || [];
      this.contacts = (result[1].data as ChatContacts).items || [];
      const chosen = this.selected;
      if (chosen) {
        const updated = this.conversations.find((item: Conversation) => item.conversation_id === chosen.conversation_id);
        if (!updated) { this.selected = null; this.messages = []; this.status = '已退出会话或成员权限已变化'; return; }
        this.selected = updated;
        const response = await api.request(this.path(chosen.conversation_id) + '/messages?limit=100', http.RequestMethod.GET, '', 10000);
        if (!this.current(op)) { return; }
        const history = response.data as ChatHistory;
        const firstLoad = this.messages.length === 0;
        const followLatest = firstLoad || this.scroller.isAtEnd();
        const previousLast = this.messages.length ? this.messages[this.messages.length - 1].created_at : '';
        if (!firstLoad && !this.gapBefore && history.has_more && history.items.length && previousLast < history.items[0].created_at) {
          this.gapBefore = history.next_before || 0; this.gapStop = previousLast;
        }
        this.messages = mergeMessages(this.messages, history.items || []);
        if (firstLoad) { this.hasMore = history.has_more; this.before = history.next_before || 0; }
        // Bridge disconnections with >100 messages without silently skipping
        // the middle. Cap each cycle, then resume its cursor on the next poll.
        for (let page = 0; page < 3 && this.gapBefore > 0; page++) {
          const older = await api.request(this.path(chosen.conversation_id) + '/messages?limit=100&before=' + this.gapBefore, http.RequestMethod.GET, '', 10000);
          if (!this.current(op)) { return; }
          const bridge = older.data as ChatHistory;
          this.messages = mergeMessages(this.messages, bridge.items || []);
          if (!bridge.has_more || !bridge.items.length || bridge.items[0].created_at <= this.gapStop) { this.gapBefore = 0; }
          else { this.gapBefore = bridge.next_before || 0; }
        }
        if (!this.gapBefore && followLatest) { await api.request(this.path(chosen.conversation_id) + '/read', http.RequestMethod.POST, '{}', 10000); }
        if (followLatest) { this.scrollBottom(); this.unseen = false; }
        else if (history.items.length && previousLast < history.items[history.items.length - 1].created_at) { this.unseen = true; }
      }
      if (this.current(op)) { this.syncStatus = this.gapBefore ? '正在补齐离线期间的消息，请保持页面开启' : ''; }
    } catch (error) { if (this.current(op)) { this.syncStatus = '消息同步失败，将自动重试：' + (error as Error).message; } }
    finally { if (op === this.generation) { this.loading = false; } }
  }
  private open(item: Conversation): void {
    this.clearMedia(); this.groupOpen = false;
    this.generation++; this.loading = false; this.selected = item; this.messages = [];
    this.gapBefore = 0; this.gapStop = ''; this.unseen = false;
    this.draft = ''; this.retryKey = ''; this.retryBody = ''; this.hasMore = false; this.before = 0; this.refresh();
  }
  private scrollBottom(): void { setTimeout(() => { if (this.active && this.selected) { this.scroller.scrollEdge(Edge.Bottom); } }, 80); }
  private async send(): Promise<void> {
    if (this.sending || !this.selected || !this.draft.trim()) { return; }
    const op = this.generation; const cid = this.selected.conversation_id; const body = this.draft.trim();
    if (body !== this.retryBody || !this.retryKey) { this.retryKey = 'chat-' + Date.now() + '-' + Math.random().toString(36).slice(2); this.retryBody = body; }
    this.sending = true;
    try {
      const reply = await DeviceSession.backend.request(this.path(cid) + '/messages', http.RequestMethod.POST, JSON.stringify({ body: body, client_id: this.retryKey }), 10000);
      if (!this.current(op)) { return; }
      this.messages = mergeMessages(this.messages, [reply.data as ChatMessage]);
      if (this.draft.trim() === body) { this.draft = ''; }
      this.retryKey = ''; this.retryBody = ''; this.status = ''; this.scrollBottom();
    } catch (error) { if (this.current(op)) { this.error(error as Error); } }
    finally { this.sending = false; }
  }
  private async older(): Promise<void> {
    if (!this.selected || !this.hasMore || this.loading) { return; }
    const op = this.generation; const cid = this.selected.conversation_id; this.loading = true;
    try {
      const reply = await DeviceSession.backend.request(this.path(cid) + '/messages?limit=30&before=' + this.before, http.RequestMethod.GET, '', 10000);
      if (!this.current(op)) { return; }
      const history = reply.data as ChatHistory;
      this.messages = mergeMessages(this.messages, history.items || []); this.before = history.next_before || 0; this.hasMore = history.has_more;
    } catch (error) { if (this.current(op)) { this.error(error as Error); } }
    finally { if (op === this.generation) { this.loading = false; } }
  }
  private async manage(action: string, target: string = ''): Promise<void> {
    if (this.managing || !this.authorized) { return; }
    const op = this.generation; this.managing = true;
    try {
      const api = DeviceSession.backend;
      if (action === 'invite') {
        const reply = await api.request('/api/v1/contacts/invite', http.RequestMethod.POST, '{}');
        if (this.current(op)) { const item = reply.data as ChatInvite; this.invite = item.code + ' · 7天内有效，仅用一次'; }
      } else if (action === 'accept') {
        await api.request('/api/v1/contacts/accept', http.RequestMethod.POST, JSON.stringify({ code: this.code.trim() }));
        if (this.current(op)) { this.code = ''; }
      } else if (action === 'direct') {
        const reply = await api.request('/api/v1/conversations/direct', http.RequestMethod.POST, JSON.stringify({ peer: target }));
        if (this.current(op)) { this.open(reply.data as Conversation); }
      } else if (action === 'group') {
        const reply = await api.request('/api/v1/conversations', http.RequestMethod.POST, JSON.stringify({ name: this.groupName.trim(), members: this.picked }));
        if (this.current(op)) { this.groupName = ''; this.picked = []; this.open(reply.data as Conversation); }
      } else if (action === 'remove') {
        const choice = await promptAction.showDialog({ title: '删除好友？', message: '保留已有聊天记录，重新添加好友后才能继续发送。', buttons: [{ text: '取消', color: CareTheme.secondary }, { text: '删除好友', color: CareTheme.alarm }] });
        if (choice.index !== 1 || !this.current(op)) { return; }
        await api.request('/api/v1/contacts/' + encodeURIComponent(target), http.RequestMethod.DELETE);
      }
      if (this.current(op)) { this.status = ''; this.refresh(); }
    } catch (error) { if (this.current(this.generation)) { this.error(error as Error); } }
    finally { this.managing = false; }
  }
  private back(): void {
    if (this.selected) { this.clearMedia(); this.groupOpen = false; this.generation++; this.selected = null; this.messages = []; this.draft = ''; this.loading = false; this.refresh(); }
    else { router.back(); }
  }
  private async chooseImage(): Promise<void> {
    if (this.mediaBusy || this.recording || this.recordBusy || !this.selected) { return; }
    const op = this.generation; const cid = this.selected.conversation_id; this.mediaBusy = true;
    try {
      const asset = await this.media.chooseImage(getContext(this) as common.UIAbilityContext);
      if (!asset || !this.current(op)) { return; }
      this.asset = asset; this.assetCid = cid; this.assetId = ''; this.assetKey = 'media-' + Date.now() + '-' + Math.random().toString(36).slice(2);
      this.attachment = '已选图片 · 点击发送后上传';
      const bitmap = await this.media.thumbnail(asset.bytes);
      if (!this.current(op)) { bitmap.release(); return; }
      if (this.assetPreview) { this.assetPreview.release(); } this.assetPreview = bitmap;
    } catch (error) { if (this.current(op)) { this.error(error as Error); } }
    finally { this.mediaBusy = false; }
  }
  private async toggleRecord(): Promise<void> {
    if (this.recordBusy || this.mediaBusy || !this.selected) { return; }
    const op = this.generation; this.recordBusy = true;
    try {
      if (!this.recording) {
        await this.media.startRecord(getContext(this) as common.UIAbilityContext);
        if (!this.current(op)) { await this.media.cancelRecord(); return; }
        this.recording = true; this.recordStarted = Date.now(); this.recordSeconds = 0;
        this.recordTimer = setInterval(() => {
          this.recordSeconds = Math.min(60, Math.floor((Date.now() - this.recordStarted) / 1000));
          if (this.recordSeconds >= 60) { this.toggleRecord(); }
        }, 250);
      } else {
        this.recording = false;
        if (this.recordTimer !== -1) { clearInterval(this.recordTimer); this.recordTimer = -1; }
        const asset = await this.media.finishRecord();
        if (!this.current(op) || !this.selected) { return; }
        this.asset = asset; this.assetCid = this.selected.conversation_id; this.assetId = ''; this.assetKey = 'voice-' + Date.now() + '-' + Math.random().toString(36).slice(2);
        this.attachment = '语音 ' + Math.round(asset.duration || 0) + ' 秒 · 点击发送后上传';
        if (this.assetPreview) { this.assetPreview.release(); this.assetPreview = undefined; }
      }
    } catch (error) { this.recording = false; if (this.recordTimer !== -1) { clearInterval(this.recordTimer); this.recordTimer = -1; } if (this.current(op)) { this.error(error as Error); } }
    finally { this.recordBusy = false; }
  }
  private discard(): void {
    this.asset = null; this.assetId = ''; this.assetKey = ''; this.assetCid = ''; this.attachment = '';
    if (this.assetPreview) { this.assetPreview.release(); this.assetPreview = undefined; }
  }
  private async sendAttachment(): Promise<void> {
    const chosen = this.asset;
    if (!chosen || this.mediaBusy || !this.selected || this.assetCid !== this.selected.conversation_id) { return; }
    const op = this.generation; const cid = this.assetCid; this.mediaBusy = true;
    try {
      if (!this.assetId) {
        const uploaded = await this.media.upload(DeviceSession.backend, cid, chosen);
        if (!this.current(op)) { return; } this.assetId = uploaded.id;
      }
      const reply = await DeviceSession.backend.request(this.path(cid) + '/messages/media', http.RequestMethod.POST, JSON.stringify({ media_id: this.assetId, client_id: this.assetKey }), 15000);
      if (!this.current(op)) { return; }
      this.messages = mergeMessages(this.messages, [reply.data as ChatMessage]); this.discard(); this.status = ''; this.scrollBottom();
    } catch (error) { if (this.current(op)) { this.status = '发送尚未确认，可重试；请勿重复选择附件。' + (error as Error).message; } }
    finally { this.mediaBusy = false; }
  }
  private async openAttachment(item: ChatMessage): Promise<void> {
    if (this.mediaBusy || this.recording || this.recordBusy) { return; }
    if (item.kind === 'voice' && this.playing === item.id) { this.media.stopPlayback(); this.playing = ''; return; }
    const op = this.generation; this.mediaBusy = true;
    try {
      const bytes = await this.media.download(DeviceSession.backend, item.body);
      if (!this.current(op)) { return; }
      if (item.kind === 'image') {
        const bitmap = await this.media.thumbnail(bytes);
        if (!this.current(op)) { bitmap.release(); return; }
        const keys = Object.keys(this.thumbnails);
        if (keys.length >= 6) { keys.forEach((key: string) => { this.thumbnails[key].release(); }); this.thumbnails = {}; }
        const next: Record<string, image.PixelMap> = {};
        Object.keys(this.thumbnails).forEach((key: string) => { next[key] = this.thumbnails[key]; });
        next[item.id] = bitmap; this.thumbnails = next;
      } else {
        this.playing = item.id;
        await this.media.play(getContext(this) as common.UIAbilityContext, bytes, (message: string) => { if (this.current(op)) { this.playing = ''; if (message) { this.status = message; } } });
      }
    } catch (error) { this.playing = ''; if (this.current(op)) { this.error(error as Error); } }
    finally { this.mediaBusy = false; }
  }
  private async groupAction(action: string, target: string = ''): Promise<void> {
    if (this.managing || !this.selected) { return; }
    const op = this.generation; const cid = this.selected.conversation_id; this.managing = true;
    try {
      const choice = await promptAction.showDialog({ title: action === 'leave' ? '退出群聊？' : action === 'remove' ? '移除这位成员？' : action === 'add' ? '邀请成员加入群聊？' : '更改群名？', message: '成员变化不会删除已有聊天记录。请核对账号，避免把私人会话分享给无关人员。', buttons: [{ text: '取消', color: CareTheme.secondary }, { text: '确认', color: CareTheme.primary }] });
      if (choice.index !== 1 || !this.current(op)) { return; }
      const api = DeviceSession.backend;
      if (action === 'rename') { await api.request(this.path(cid), http.RequestMethod.PATCH, JSON.stringify({ name: this.rename.trim() })); }
      else if (action === 'add') { await api.request(this.path(cid) + '/members', http.RequestMethod.POST, JSON.stringify({ username: this.addName.trim() })); }
      else { await api.request(this.path(cid) + '/members/' + encodeURIComponent(action === 'leave' ? api.username : target), http.RequestMethod.DELETE); }
      if (!this.current(op)) { return; }
      this.rename = ''; this.addName = ''; this.status = '';
      if (action === 'leave') { this.back(); } else { this.refresh(); }
    } catch (error) { if (this.current(op)) { this.error(error as Error); } }
    finally { this.managing = false; }
  }
  @Builder
  groupPanel() {
    Column({ space: 12 }) {
      Text('群成员 · ' + (this.selected ? this.selected.member_count : 0) + '/10').fontSize(18).fontColor(CareTheme.ink).width('100%')
      ForEach(this.selected ? this.selected.members : [], (name: string) => {
        Row({ space: 8 }) {
          Text(name + (this.selected && name === this.selected.owner ? ' · 群主' : '')).fontSize(15).fontColor(CareTheme.ink).layoutWeight(1)
          if (this.selected && this.selected.owner === DeviceSession.backend.username && name !== this.selected.owner) {
            Button('移除').height(44).fontColor(CareTheme.secondary).backgroundColor(Color.Transparent).enabled(!this.managing).onClick(() => { this.groupAction('remove', name); })
          }
        }.width('100%')
      }, (name: string) => name)
      if (this.selected && this.selected.owner === DeviceSession.backend.username) {
        TextInput({ placeholder: '新的群名称', text: this.rename }).maxLength(32).height(48).onChange((value: string) => { this.rename = value; })
        Button('更改群名').enabled(!this.managing && this.rename.trim().length > 0).height(44).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).onClick(() => { this.groupAction('rename'); })
        TextInput({ placeholder: '要加入的账号', text: this.addName }).maxLength(64).height(48).onChange((value: string) => { this.addName = value; })
        Button('添加群成员').enabled(!this.managing && this.addName.trim().length > 0 && this.selected.member_count < 10).height(44).backgroundColor(CareTheme.softBlue).fontColor(CareTheme.primary).onClick(() => { this.groupAction('add'); })
        Text('群主暂不能退出或解散群；群成员可查看群内历史，请谨慎邀请。').fontSize(13).fontColor(CareTheme.secondary).lineHeight(21)
      } else {
        Button('退出群聊').enabled(!this.managing && DeviceSession.backend.username !== '').height(44).fontColor(CareTheme.alarm).backgroundColor(Color.Transparent).onClick(() => { this.groupAction('leave'); })
      }
    }.padding(16).width('100%').backgroundColor(Color.White).borderRadius(16)
  }
  @Builder
  list() {
    Row({ space: 10 }) {
      Button('会话').layoutWeight(1).height(46).backgroundColor(this.tab === 0 ? CareTheme.primary : Color.White).fontColor(this.tab === 0 ? Color.White : CareTheme.primary).onClick(() => { this.tab = 0; })
      Button('好友与群聊').layoutWeight(1).height(46).backgroundColor(this.tab === 1 ? CareTheme.primary : Color.White).fontColor(this.tab === 1 ? Color.White : CareTheme.primary).onClick(() => { this.tab = 1; })
    }.width('100%')
    if (this.tab === 0) {
      Column() {
        ForEach(this.conversations, (item: Conversation) => {
          Row({ space: 12 }) {
            Text(item.kind === 'group' ? '群' : '信').fontSize(18).fontColor(CareTheme.primary).width(44).height(44).textAlign(TextAlign.Center).backgroundColor(CareTheme.softBlue).borderRadius(12)
            Column({ space: 6 }) {
              Text(this.title(item)).fontSize(18).fontColor(CareTheme.ink).width('100%')
              Text(messagePreview(item.last_message)).fontSize(14).fontColor(CareTheme.secondary).maxLines(2).width('100%')
            }.layoutWeight(1)
            if (item.unread) { Text(item.unread > 99 ? '99+' : item.unread.toString()).fontSize(13).fontColor(Color.White).padding(7).backgroundColor(CareTheme.alarm).borderRadius(12) }
          }.padding({ top: 16, bottom: 16 }).width('100%').onClick(() => { this.open(item); })
          Divider().color(CareTheme.line)
        }, (item: Conversation) => item.conversation_id)
        if (!this.conversations.length) { Text('还没有会话\n从“好友与群聊”添加家人，开始交流').fontSize(16).fontColor(CareTheme.secondary).lineHeight(26).padding({ top: 36, bottom: 36 }) }
      }.padding({ left: 16, right: 16 }).backgroundColor(Color.White).borderRadius(16).width('100%')
    } else {
      Column({ space: 14 }) {
        Text('邀请家人').fontSize(19).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink).width('100%')
        Text('双方先使用各自账号登录。邀请码仅建立好友关系，不授予设备控制权限。').fontSize(14).fontColor(CareTheme.secondary).lineHeight(22)
        Button('生成我的邀请码').enabled(!this.managing).height(48).width('100%').backgroundColor(CareTheme.primary).onClick(() => { this.manage('invite'); })
        if (this.invite) { Text(this.invite).fontSize(18).fontColor(CareTheme.primary).copyOption(CopyOptions.LocalDevice).width('100%') }
        TextInput({ placeholder: '输入对方邀请码', text: this.code }).maxLength(16).height(48).onChange((value: string) => { this.code = value; })
        Button('添加好友').enabled(!this.managing && this.code.trim().length >= 4).height(48).width('100%').fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(() => { this.manage('accept'); })
      }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
      Column({ space: 12 }) {
        Text('好友').fontSize(19).fontColor(CareTheme.ink).width('100%')
        ForEach(this.contacts, (item: ChatContact) => {
          Row({ space: 8 }) {
            Checkbox({ name: item.username }).select(this.picked.includes(item.username)).selectedColor(CareTheme.primary).onChange((value: boolean) => { this.picked = value ? this.picked.concat([item.username]) : this.picked.filter((name: string) => name !== item.username); })
            Text(item.username).fontSize(16).fontColor(CareTheme.ink).layoutWeight(1)
            Button('聊天').height(44).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).enabled(!this.managing).onClick(() => { this.manage('direct', item.username); })
            Button('删除').height(44).fontColor(CareTheme.secondary).backgroundColor(Color.Transparent).enabled(!this.managing).onClick(() => { this.manage('remove', item.username); })
          }.width('100%')
        }, (item: ChatContact) => item.username)
        if (!this.contacts.length) { Text('添加好友后，可单聊或勾选好友建群').fontSize(14).fontColor(CareTheme.secondary).width('100%') }
        Divider().color(CareTheme.line)
        TextInput({ placeholder: '群聊名称', text: this.groupName }).maxLength(32).height(48).onChange((value: string) => { this.groupName = value; })
        Button('与选中的 ' + this.picked.length + ' 位好友建群').enabled(!this.managing && this.picked.length > 0 && this.picked.length <= 9 && this.groupName.trim().length > 0).height(48).width('100%').backgroundColor(CareTheme.primary).onClick(() => { this.manage('group'); })
      }.padding(18).backgroundColor(Color.White).borderRadius(16).width('100%')
    }
  }
  build() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Button('返回').height(44).fontColor(CareTheme.primary).backgroundColor(Color.Transparent).onClick(() => { this.back(); })
        Text(this.selected ? this.title(this.selected) : '家人消息').fontSize(24).fontWeight(FontWeight.Medium).fontColor(CareTheme.ink).layoutWeight(1).maxLines(2)
        Button('刷新').height(44).enabled(!this.loading && this.authorized).fontColor(CareTheme.primary).backgroundColor(Color.Transparent).onClick(() => { this.refresh(); })
      }.width('100%')
      Text(this.online ? '在线同步 · 消息实时更新' : '连接恢复中 · 前台每4秒补拉消息').fontSize(13).fontColor(CareTheme.secondary).width('100%')
      if (this.status) { Text(this.status).fontSize(14).fontColor(CareTheme.warmInk).width('100%').lineHeight(22) }
      if (this.syncStatus) { Text(this.syncStatus).fontSize(13).fontColor(CareTheme.warmInk).width('100%').lineHeight(21) }
      if (!this.authorized) { Text('请返回“我的 → 账户与连接”重新登录').fontSize(16).fontColor(CareTheme.secondary).padding(24) }
      else if (this.selected) {
        if (!this.writable()) { Text('对方已不在会话中，仅保留历史记录，不能继续发送。').fontSize(14).fontColor(CareTheme.secondary).width('100%') }
        if (this.selected.kind === 'group') {
          Button(this.groupOpen ? '收起群管理' : '群成员与设置').height(44).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(() => { this.groupOpen = !this.groupOpen; })
        }
        Scroll(this.scroller) {
          Column({ space: 14 }) {
            if (this.groupOpen) { this.groupPanel() }
            if (this.hasMore) { Button('加载更早消息').enabled(!this.loading).backgroundColor(Color.Transparent).fontColor(CareTheme.primary).onClick(() => { this.older(); }) }
            if (!this.messages.length) { Text(this.loading ? '正在读取消息…' : '还没有消息，给家人留句话吧').fontSize(15).fontColor(CareTheme.secondary).padding(30) }
            ForEach(this.messages, (item: ChatMessage) => {
              Column({ space: 6 }) {
                Text(item.sender + ' · ' + new Date(item.created_at).toLocaleString()).fontSize(13).fontColor(CareTheme.secondary).width('100%')
                if (item.kind === 'image') {
                  if (this.thumbnails[item.id]) { Image(this.thumbnails[item.id]).width('100%').height(200).objectFit(ImageFit.Contain).borderRadius(12) }
                  else { Button(this.mediaBusy ? '正在读取…' : '查看图片').enabled(!this.mediaBusy).height(48).fontColor(CareTheme.primary).backgroundColor(Color.White).onClick(() => { this.openAttachment(item); }) }
                } else if (item.kind === 'voice') {
                  Button((this.playing === item.id ? '停止播放 · ' : '播放语音 · ') + Math.round(item.duration || 0) + '秒').enabled(!this.mediaBusy).height(48).fontColor(CareTheme.primary).backgroundColor(Color.White).onClick(() => { this.openAttachment(item); })
                } else { Text(item.body).fontSize(17).fontColor(CareTheme.ink).lineHeight(26).copyOption(CopyOptions.LocalDevice).width('100%') }
              }.padding(16).backgroundColor(item.sender === DeviceSession.backend.username ? CareTheme.softBlue : Color.White).borderRadius(16).width('100%')
            }, (item: ChatMessage) => item.id)
          }.width('100%').padding({ bottom: 12 })
        }.layoutWeight(1).width('100%').align(Alignment.TopStart).scrollBar(BarState.Auto)
        if (this.unseen) { Button('有新消息 · 查看最新').height(44).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(() => { this.scrollBottom(); this.unseen = false; }) }
        if (this.attachment) {
          Row({ space: 8 }) {
            if (this.assetPreview) { Image(this.assetPreview).width(52).height(52).objectFit(ImageFit.Contain) }
            Text(this.attachment).fontSize(13).fontColor(CareTheme.secondary).layoutWeight(1)
            Button('取消').height(44).enabled(!this.mediaBusy).fontColor(CareTheme.secondary).backgroundColor(Color.Transparent).onClick(() => { this.discard(); })
            Button(this.mediaBusy ? '发送中' : '发送附件').height(48).enabled(!this.mediaBusy && !this.recording).backgroundColor(CareTheme.primary).onClick(() => { this.sendAttachment(); })
          }.width('100%')
        }
        Row({ space: 10 }) {
          Button('图片').height(44).enabled(this.writable() && !this.mediaBusy && !this.recording && !this.recordBusy).fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(() => { this.chooseImage(); })
          Button(this.recordBusy ? '正在准备…' : this.recording ? '结束录音 ' + this.recordSeconds + '秒' : '录制语音').height(44).enabled(this.writable() && !this.recordBusy && !this.mediaBusy).fontColor(this.recording ? CareTheme.alarm : CareTheme.primary).backgroundColor(CareTheme.softBlue).onClick(() => { this.toggleRecord(); })
          if (this.recording) { Text('最长60秒，结束后确认发送').fontSize(12).fontColor(CareTheme.secondary).layoutWeight(1) }
        }.width('100%')
        Row({ space: 10 }) {
          TextInput({ placeholder: '给家人留句话…', text: this.draft }).maxLength(4000).layoutWeight(1).height(48).onChange((value: string) => { this.draft = value; })
          Button(this.sending ? '发送中' : '发送').enabled(this.writable() && !this.sending && !this.recording && !this.recordBusy && this.draft.trim().length > 0).height(48).backgroundColor(CareTheme.primary).onClick(() => { this.send(); })
        }.width('100%')
      } else {
        Scroll() { Column({ space: 20 }) { this.list() }.width('100%').padding({ bottom: 20 }) }.layoutWeight(1).width('100%').align(Alignment.TopStart)
      }
    }.padding({ left: 20, right: 20, top: 12, bottom: 16 }).width('100%').height('100%').backgroundColor(CareTheme.background)
  }
}
