import { common, abilityAccessCtrl } from '@kit.AbilityKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { media } from '@kit.MediaKit';
import { http } from '@kit.NetworkKit';
import { Backend } from './Backend';
import { Envelope } from '../models/Models';

export interface ChatAsset { bytes: ArrayBuffer; type: string; duration?: number; }
export interface UploadedChatAsset { id: string; kind: string; size: number; duration?: number; }

// Media stays in memory/private cache. No token is put in a URL or public file.
export class ChatMedia {
  private recorder: media.AVRecorder | null = null;
  private recordFile: fs.File | null = null;
  private recordPath: string = '';
  private started: number = 0;
  private recordGeneration: number = 0;
  private player: media.AVPlayer | null = null;
  private playFile: fs.File | null = null;
  private playPath: string = '';
  private playGeneration: number = 0;
  private clients: http.HttpRequest[] = [];

  async chooseImage(context: common.UIAbilityContext): Promise<ChatAsset | null> {
    const result = await new picker.PhotoViewPicker(context).select({ MIMEType: picker.PhotoViewMIMETypes.IMAGE_TYPE, maxSelectNumber: 1 });
    if (!result.photoUris.length) { return null; }
    const file = fs.openSync(result.photoUris[0], fs.OpenMode.READ_ONLY);
    let source: image.ImageSource | null = null;
    let bitmap: image.PixelMap | null = null;
    const packer = image.createImagePacker();
    try {
      source = image.createImageSource(file.fd);
      const info = await source.getImageInfo();
      const scale = Math.min(1, 1280 / Math.max(info.size.width, info.size.height));
      bitmap = await source.createPixelMap({ desiredSize: { width: Math.max(1, Math.round(info.size.width * scale)), height: Math.max(1, Math.round(info.size.height * scale)) } });
      const bytes = await packer.packing(bitmap, { format: 'image/jpeg', quality: 78 });
      if (bytes.byteLength > 8 * 1024 * 1024) { throw new Error('图片仍超过8MB，请选择较小图片'); }
      return { bytes: bytes, type: 'image/jpeg' };
    } finally {
      if (bitmap) { bitmap.release(); } if (source) { source.release(); }
      packer.release(); fs.closeSync(file);
    }
  }
  async upload(api: Backend, cid: string, asset: ChatAsset): Promise<UploadedChatAsset> {
    const token = api.token;
    if (!token || !asset.bytes.byteLength || asset.bytes.byteLength > 8 * 1024 * 1024) { throw new Error('文件无效或登录已失效'); }
    const path = '/api/v1/media?conversation_id=' + encodeURIComponent(cid) + (asset.duration === undefined ? '' : '&duration=' + asset.duration);
    const client = http.createHttp(); this.clients.push(client);
    try {
      const options: http.HttpRequestOptions = { method: http.RequestMethod.POST, header: { Authorization: 'Bearer ' + token, 'Content-Type': asset.type }, extraData: asset.bytes, usingCache: false, expectDataType: http.HttpDataType.STRING, connectTimeout: 8000, readTimeout: 30000 };
      const cert = api.certificatePath(); if (cert) { options.caPath = cert; }
      const response = await client.request(api.base + path, options);
      if (token !== api.token) { throw new Error('账号已切换'); }
      const body = JSON.parse(response.result as string) as Envelope;
      if (response.responseCode >= 400 || body.code !== 0) { throw new Error(body.message || '上传失败，请重试'); }
      return body.data as UploadedChatAsset;
    } finally { client.destroy(); this.clients = this.clients.filter((item: http.HttpRequest) => item !== client); }
  }
  async download(api: Backend, id: string): Promise<ArrayBuffer> {
    const token = api.token; if (!token) { throw new Error('请重新登录'); }
    const client = http.createHttp(); this.clients.push(client);
    try {
      const options: http.HttpRequestOptions = { header: { Authorization: 'Bearer ' + token }, usingCache: false, expectDataType: http.HttpDataType.ARRAY_BUFFER, connectTimeout: 8000, readTimeout: 20000 };
      const cert = api.certificatePath(); if (cert) { options.caPath = cert; }
      const response = await client.request(api.base + '/api/v1/media/' + encodeURIComponent(id), options);
      if (token !== api.token) { throw new Error('账号已切换'); }
      if (response.responseCode !== 200) { throw new Error(response.responseCode === 403 ? '会话权限已变化，无法读取附件' : '附件暂时无法读取'); }
      const bytes = response.result as ArrayBuffer;
      if (!bytes.byteLength || bytes.byteLength > 8 * 1024 * 1024) { throw new Error('附件大小无效'); }
      return bytes;
    } finally { client.destroy(); this.clients = this.clients.filter((item: http.HttpRequest) => item !== client); }
  }
  async thumbnail(bytes: ArrayBuffer): Promise<image.PixelMap> {
    const source = image.createImageSource(bytes);
    try {
      const info = await source.getImageInfo(); const scale = Math.min(1, 800 / Math.max(info.size.width, info.size.height));
      return await source.createPixelMap({ desiredSize: { width: Math.max(1, Math.round(info.size.width * scale)), height: Math.max(1, Math.round(info.size.height * scale)) } });
    } finally { source.release(); }
  }
  async startRecord(context: common.UIAbilityContext): Promise<void> {
    if (this.recorder) { throw new Error('录音正在进行'); }
    const generation = ++this.recordGeneration;
    const permission = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, ['ohos.permission.MICROPHONE']);
    if (generation !== this.recordGeneration) { throw new Error('录音已取消'); }
    if (!permission.authResults.length || permission.authResults.some((result: number) => result !== 0)) { throw new Error('需要麦克风权限才能录制语音，可继续发送文字'); }
    const recorder = await media.createAVRecorder();
    if (generation !== this.recordGeneration) { await recorder.release(); throw new Error('录音已取消'); }
    this.recorder = recorder;
    this.recordPath = context.cacheDir + '/care-chat-record-' + Date.now() + '.m4a';
    try {
      this.recordFile = fs.openSync(this.recordPath, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      await recorder.prepare({ audioSourceType: media.AudioSourceType.AUDIO_SOURCE_TYPE_MIC,
        profile: { audioBitrate: 64000, audioChannels: 1, audioCodec: media.CodecMimeType.AUDIO_AAC, audioSampleRate: 44100, fileFormat: media.ContainerFormatType.CFT_MPEG_4A },
        url: 'fd://' + this.recordFile.fd });
      if (generation !== this.recordGeneration) { throw new Error('录音已取消'); }
      await recorder.start();
      if (generation !== this.recordGeneration) { throw new Error('录音已取消'); }
      this.started = Date.now();
    } catch (error) {
      if (generation === this.recordGeneration) { await this.cancelRecord(); }
      throw new Error((error as Error).message || '无法开始录音');
    }
  }
  async finishRecord(): Promise<ChatAsset> {
    const recorder = this.recorder;
    if (!recorder || !this.started) { throw new Error('录音尚未开始'); }
    const generation = this.recordGeneration;
    const path = this.recordPath;
    const duration = (Date.now() - this.started) / 1000;
    try {
      await recorder.stop();
      if (generation !== this.recordGeneration) { throw new Error('录音已取消'); }
      if (duration < 1) { throw new Error('录音不足1秒，请重新录制'); }
      if (duration > 60.5) { throw new Error('录音超过60秒，请缩短后重新录制'); }
      const size = fs.statSync(path).size;
      if (!size || size > 8 * 1024 * 1024) { throw new Error('录音文件无效'); }
      const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
      try {
        const bytes = new ArrayBuffer(size); const count = fs.readSync(file.fd, bytes);
        if (count !== size) { throw new Error('录音读取不完整'); }
        return { bytes: bytes, type: 'audio/mp4', duration: Math.min(60, duration) };
      } finally { fs.closeSync(file); }
    } finally { if (generation === this.recordGeneration) { await this.cancelRecord(); } }
  }
  async cancelRecord(): Promise<void> {
    this.recordGeneration++; this.started = 0;
    const recorder = this.recorder; this.recorder = null;
    const file = this.recordFile; this.recordFile = null;
    const path = this.recordPath; this.recordPath = '';
    if (recorder) { try { await recorder.release(); } catch (_) {} }
    if (file) { try { fs.closeSync(file); } catch (_) {} }
    if (path) { try { fs.unlinkSync(path); } catch (_) {} }
  }
  stopPlayback(): void {
    this.playGeneration++;
    const player = this.player; this.player = null;
    const file = this.playFile; this.playFile = null; const path = this.playPath; this.playPath = '';
    const cleanup = (): void => { if (file) { try { fs.closeSync(file); } catch (_) {} } if (path) { try { fs.unlinkSync(path); } catch (_) {} } };
    if (player) { player.release().then(cleanup).catch(cleanup); } else { cleanup(); }
  }
  async play(context: common.UIAbilityContext, bytes: ArrayBuffer, onDone: (error: string) => void): Promise<void> {
    this.stopPlayback(); const generation = this.playGeneration;
    const player = await media.createAVPlayer();
    if (generation !== this.playGeneration) { await player.release(); return; }
    this.player = player;
    this.playPath = context.cacheDir + '/care-chat-play-' + Date.now() + '.audio';
    this.playFile = fs.openSync(this.playPath, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    fs.writeSync(this.playFile.fd, bytes);
    const fail = (message: string): void => { if (generation === this.playGeneration) { this.stopPlayback(); onDone(message); } };
    player.on('stateChange', (state: string) => {
      if (generation !== this.playGeneration) { return; }
      if (state === 'initialized') { player.prepare().catch(() => { fail('语音无法解析，请检查录音格式'); }); }
      else if (state === 'prepared') { player.play().catch(() => { fail('无法播放语音'); }); }
      else if (state === 'completed') { fail(''); }
    });
    player.on('error', () => { fail('语音播放失败'); });
    player.fdSrc = { fd: this.playFile.fd, offset: 0, length: bytes.byteLength };
  }
  close(): void {
    this.clients.forEach((client: http.HttpRequest) => { client.destroy(); }); this.clients = [];
    this.cancelRecord(); this.stopPlayback();
  }
}
