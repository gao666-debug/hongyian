export function localIpv4(value:string):boolean {
  if(!/^\d{1,3}(\.\d{1,3}){3}$/.test(value)){return false;}
  const parts=value.split('.');if(parts.some((s:string)=>s.length>1&&s.startsWith('0'))){return false;}
  const p=parts.map((s:string)=>Number(s));
  if(p.some((n:number)=>n<0||n>255)||p[3]===0||p[3]===255){return false;}
  return p[0]===10||(p[0]===192&&p[1]===168)||(p[0]===172&&p[1]>=16&&p[1]<=31);
}
// Links carry no credentials. The target device must pass household authorization.
export class DeviceLink {
  static pending:string='';
  static localIp:string='';
  static capture(uri:string):boolean {
    const m=/^yihongan:\/\/device\?device_id=([A-Za-z0-9_%.-]+)(?:&local_ip=([0-9.]+))?$/.exec(uri);
    if(!m){return false;}
    try{const id=decodeURIComponent(m[1]);if(!/^[A-Za-z0-9_-]{1,128}$/.test(id)||(m[2]&&!localIpv4(m[2]))){return false;}DeviceLink.pending=id;DeviceLink.localIp=m[2]||'';return true;}catch(_){return false;}
  }
}
