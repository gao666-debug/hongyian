import { http, webSocket } from '@kit.NetworkKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { Envelope, LoginData, Push, Ticket } from '../models/Models';

export class Backend {
  base: string = '';
  device: string = 'demo-device';
  token: string = '';
  caPath: string = '';
  private publicCaPath: string = '';
  private readonly publicBase: string = 'https://backend.example.com';
  private socket: webSocket.WebSocket | null = null;
  private timer: number = -1;
  private generation: number = 0;
  private notify: ((message: Push) => void) | null = null;
  private link: ((online: boolean) => void) | null = null;

  private async installCertificate(context: common.UIAbilityContext, name: string): Promise<string> {
    try {
      const data = await context.resourceManager.getRawFileContent(name);
      const path = context.cacheDir + '/' + name;
      const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      try { fs.writeSync(file.fd, data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength)); }
      finally { fs.closeSync(file); }
      return path;
    } catch (_) { return ''; }
  }
  async prepare(context: common.UIAbilityContext): Promise<void> {
    this.caPath = await this.installCertificate(context, 'development-ca.crt');
    this.publicCaPath = await this.installCertificate(context, 'qbot-public.crt');
  }
  private certificatePath(): string {
    if (this.base === this.publicBase) {
      if (this.publicCaPath === '') { throw new Error('服务器信任证书加载失败，请重新启动 App'); }
      return this.publicCaPath;
    }
    return this.caPath;
  }
  path(suffix: string): string { return `/api/v1/devices/${encodeURIComponent(this.device)}/${suffix}`; }
  async request(path: string, method: http.RequestMethod = http.RequestMethod.GET, body: string = ''): Promise<Envelope> {
    const certificate = this.certificatePath();
    const client = http.createHttp();
    const headers: Record<string, string> = { 'Content-Type': 'application/json', 'Cache-Control': 'no-cache, no-store' };
    if (this.token !== '') { headers['Authorization'] = 'Bearer ' + this.token; }
    const options: http.HttpRequestOptions = {
      method: method, header: headers, usingCache: false, connectTimeout: 8000, readTimeout: 45000,
      expectDataType: http.HttpDataType.STRING
    };
    if (certificate !== '') { options.caPath = certificate; }
    if (body !== '') { options.extraData = body; }
    try {
      const url = this.base + path + (path.includes('?') ? '&' : '?') + '_ts=' + Date.now();
      const response = await client.request(url, options);
      const envelope = JSON.parse(response.result as string) as Envelope;
      if (response.responseCode === 401) { throw new Error('登录已过期，请重新登录'); }
      if (response.responseCode >= 400 || envelope.code !== 0) {
        throw new Error(envelope.message || `请求失败 ${response.responseCode}`);
      }
      return envelope;
    } finally { client.destroy(); }
  }
  async login(base: string, username: string, password: string): Promise<void> {
    const clean = base.trim().replace(/\/+$/, '');
    if (!clean.startsWith('https://')) { throw new Error('后端地址需要使用 HTTPS'); }
    this.stopPush(); this.base = clean; this.token = '';
    const response = await this.request('/api/v1/auth/login', http.RequestMethod.POST, JSON.stringify({ username: username, password: password }));
    this.token = (response.data as LoginData).access_token;
    if (!this.token) { throw new Error('登录响应缺少令牌'); }
  }
  async logout(): Promise<void> {
    this.stopPush();
    try { if (this.token) { await this.request('/api/v1/auth/logout', http.RequestMethod.POST, '{}'); } }
    finally { this.token = ''; }
  }
  stopPush(): void {
    this.generation++;
    if (this.timer !== -1) { clearTimeout(this.timer); this.timer = -1; }
    const old = this.socket; this.socket = null;
    if (old) { old.close().catch(() => {}); }
    if (this.link) { this.link(false); }
  }
  startPush(notify: (message: Push) => void, link: (online: boolean) => void): void {
    this.stopPush(); this.notify = notify; this.link = link;
    this.openPush(this.generation);
  }
  private retry(generation: number): void {
    if (generation !== this.generation || this.token === '' || this.timer !== -1) { return; }
    this.timer = setTimeout(() => { this.timer = -1; this.openPush(generation); }, 5000);
  }
  private async openPush(generation: number): Promise<void> {
    try {
      const reply = await this.request(`/api/v1/ws-ticket?device_id=${encodeURIComponent(this.device)}`, http.RequestMethod.POST, '{}');
      if (generation !== this.generation || !this.token) { return; }
      const ticket = (reply.data as Ticket).ticket;
      const socket = webSocket.createWebSocket(); this.socket = socket;
      socket.on('open', () => { if (generation === this.generation && this.link) { this.link(true); } });
      socket.on('message', (_error: Error, raw: string | ArrayBuffer) => {
        if (generation !== this.generation || typeof raw !== 'string') { return; }
        try {
          const message = JSON.parse(raw) as Push;
          if (message.type === 'ping') { socket.send('pong').catch(() => {}); }
          else if (this.notify) { this.notify(message); }
        } catch (_) { /* Ignore unsupported frames. */ }
      });
      socket.on('close', () => {
        if (generation !== this.generation || this.socket !== socket) { return; }
        this.socket = null; if (this.link) { this.link(false); } this.retry(generation);
      });
      socket.on('error', () => {
        if (generation !== this.generation || this.socket !== socket) { return; }
        this.socket = null; socket.close().catch(() => {}); if (this.link) { this.link(false); } this.retry(generation);
      });
      const options: webSocket.WebSocketRequestOptions = {};
      const certificate = this.certificatePath();
      if (certificate !== '') { options.caPath = certificate; }
      await socket.connect(this.base.replace(/^https:/, 'wss:') + `/api/v1/ws?device_id=${encodeURIComponent(this.device)}&ticket=${encodeURIComponent(ticket)}`, options);
    } catch (_) { if (generation === this.generation) { if (this.link) { this.link(false); } this.retry(generation); } }
  }
}
