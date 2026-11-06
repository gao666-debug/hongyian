export interface CareActual {
  enabled:boolean;sleep_enabled:boolean;start:number;end:number;quiet:number;revision:number;
  noact_ms:number;mode:number;time_synced:boolean;pause_seconds:number;pause_revision:number;
}
export interface CareSnapshot {supported:boolean;actual:CareActual|null;boot_id:number|null;fw?:string;updated_at:number|null;age_seconds:number|null;stale:boolean;}
export interface CarePolicyRequest {boot_id:number;enabled:boolean;sleep_enabled:boolean;start:number;end:number;quiet:number;noact_ms:number;expected_revision:number;client_request_id:string;}
export interface CarePauseRequest {boot_id:number;minutes:number;expected_pause_revision:number;client_request_id:string;}
export function careFresh(value:CareSnapshot|null):boolean {
  if(!value||value.supported!==true||!value.actual||value.boot_id===null||!Number.isInteger(value.boot_id)||value.boot_id<=0||value.stale!==false||value.age_seconds===null||!Number.isFinite(value.age_seconds)||value.age_seconds<0||value.age_seconds>90){return false;}
  const a=value.actual;return typeof a.enabled==='boolean'&&typeof a.sleep_enabled==='boolean'&&typeof a.time_synced==='boolean'&&Number.isInteger(a.revision)&&a.revision>0&&Number.isInteger(a.pause_revision)&&a.pause_revision>0&&Number.isInteger(a.noact_ms)&&a.noact_ms>=10000&&a.noact_ms<=86400000&&Number.isInteger(a.start)&&a.start>=0&&a.start<1440&&Number.isInteger(a.end)&&a.end>=0&&a.end<1440&&[0,1].includes(a.quiet)&&[0,1,2].includes(a.mode)&&Number.isInteger(a.pause_seconds)&&a.pause_seconds>=0&&a.pause_seconds<=86400;

}
export function carePolicyMatches(value:CareSnapshot|null,p:CarePolicyRequest):boolean {
  if(!careFresh(value)||!value||!value.actual||value.boot_id!==p.boot_id){return false;}
  const a=value.actual;return a.revision===p.expected_revision+1&&a.enabled===p.enabled&&a.sleep_enabled===p.sleep_enabled&&a.start===p.start&&a.end===p.end&&a.quiet===p.quiet&&a.noact_ms===p.noact_ms;
}
export function carePauseMatches(value:CareSnapshot|null,p:CarePauseRequest):boolean {
  if(!careFresh(value)||!value||!value.actual||value.boot_id!==p.boot_id){return false;}
  const a=value.actual;return a.pause_revision===p.expected_pause_revision+1&&(p.minutes===0?a.pause_seconds===0:a.pause_seconds>0&&a.pause_seconds<=p.minutes*60);
}
export function careMinute(text:string):number {if(!/^([01]\d|2[0-3]):[0-5]\d$/.test(text)){return -1;}return Number(text.slice(0,2))*60+Number(text.slice(3));}
export function careTime(value:number):string{return String(Math.floor(value/60)).padStart(2,'0')+':'+String(value%60).padStart(2,'0');}
