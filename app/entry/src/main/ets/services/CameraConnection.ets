import { http, connection } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';

export interface CameraReport {
  firmware: string;
  camera_ready: boolean;
  wifi_connected: boolean;
  ip: string;
  uptime_s: number;
  rssi: number;
}
export interface CameraFound { address: string; report: CameraReport; }

// Only an authenticated, identified camera is accepted as a discovery result.
export class CameraConnection {
  private generation: number = 0;
  cancel(): void { this.generation++; }
  normalize(address: string): string {
    let result=address.trim().replace(/\/+$/, '');
    if (result && !result.includes('://')) { result='http://'+result; }
    if (!/^https?:\/\/[A-Za-z0-9.\-]+(:[0-9]{1,5})?$/.test(result)) {
      throw new Error('地址请填写 IP 或主机名，可包含端口，不需要路径和账号密码');
    }
    const host=result.split('/')[2].split(':')[0];
    const parts=host.split('.');
    const privateIP=parts.length===4 && parts.every((n:string)=>/^\d+$/.test(n) && Number(n)<=255) &&
      (Number(parts[0])===10 || (Number(parts[0])===192 && Number(parts[1])===168) ||
      (Number(parts[0])===172 && Number(parts[1])>=16 && Number(parts[1])<=31));
    if (result.startsWith('http://') && !privateIP && !host.endsWith('.local') && host!=='localhost' && host!=='127.0.0.1') {
      throw new Error('普通 HTTP 仅用于局域网摄像头，外网地址需使用 HTTPS');
    }
    return result;
  }
  async probe(address: string, username: string, password: string): Promise<CameraFound> {
    const base=this.normalize(address);
    const client=http.createHttp();
    const basic=new util.Base64Helper().encodeToStringSync(new util.TextEncoder().encodeInto(username+':'+password));
    try {
      const response=await client.request(base+'/status',{
        method:http.RequestMethod.GET,
        // Arduino ESP32 WebServer skips one character after the header colon.
        // Explicit optional whitespace keeps Basic authentication compatible with its parser.
        header:{'Authorization':' Basic '+basic,'Cache-Control':'no-store'},
        usingProtocol:http.HttpProtocol.HTTP1_1,usingCache:false,usingProxy:false,connectTimeout:3000,readTimeout:6000,expectDataType:http.HttpDataType.STRING
      });
      if (response.responseCode===401) { throw new Error('摄像头账号或密码不正确'); }
      if (response.responseCode!==200) { throw new Error('摄像头暂时无法响应'); }
      const report=JSON.parse(response.result as string) as CameraReport;
      if (!report.firmware || !report.firmware.startsWith('camera-v') || typeof report.camera_ready!=='boolean') {
        throw new Error('这个地址不是已配置的颐鸿安摄像头');
      }
      if (!report.camera_ready) { throw new Error('已找到摄像头，但镜头未就绪，请检查摄像头排线'); }
      return {address:base,report:report};
    } finally { client.destroy(); }
  }
  async find(saved: string, username: string, password: string, progress: (value:string)=>void): Promise<CameraFound> {
    const generation=++this.generation;
    const first: string[]=[];
    if (saved) { first.push(saved); }
    for (const value of ['http://192.168.1.100','http://192.168.1.101','http://homesafety-camera.local']) {
      if (!first.includes(value)) { first.push(value); }
    }
    progress('正在连接上次使用的摄像头…');
    const initial=await this.batch(first,username,password);
    if (generation!==this.generation) { throw new Error('连接已取消'); }
    if (initial) { return initial; }
    const prefixes:string[]=[];
    try {
      const net=await connection.getDefaultNet();
      const properties=await connection.getConnectionProperties(net);
      for (const link of properties.linkAddresses) {
        const ip=link.address.address;
        const parts=ip.split('.');
        if (parts.length===4 && (ip.startsWith('10.') || ip.startsWith('192.168.') || (Number(parts[0])===172 && Number(parts[1])>=16 && Number(parts[1])<=31))) {
          const prefix=parts.slice(0,3).join('.');
          if (!prefixes.includes(prefix)) { prefixes.push(prefix); }
        }
      }
    } catch (error) { throw new Error('未找到摄像头，请连接 GB 热点后重试，或在连接设置中填入地址'); }
    // Two interfaces at most; stop immediately on success or page exit.
    for (const prefix of prefixes.slice(0,2)) {
      for (let start=1; start<=254; start+=12) {
        if (generation!==this.generation) { throw new Error('连接已取消'); }
        progress('正在查找同一网络的摄像头 '+Math.min(start+11,254)+'/254');
        const addresses:string[]=[];
        for (let host=start; host<start+12 && host<=254; host++) { addresses.push('http://'+prefix+'.'+host); }
        const found=await this.batch(addresses,username,password);
        if (generation!==this.generation) { throw new Error('连接已取消'); }
        if (found) { return found; }
      }
    }
    throw new Error('没有找到就绪的摄像头。请确认摄像头通电、连接同一热点，且账号密码正确');
  }
  private async batch(addresses:string[], username:string, password:string): Promise<CameraFound | null> {
    const results=await Promise.all(addresses.map(async (address:string):Promise<CameraFound | null>=>{
      try { return await this.probe(address,username,password); } catch (error) {
        return null;
      }
    }));
    for (const result of results) { if (result) { return result; } }
    return null;
  }
}
