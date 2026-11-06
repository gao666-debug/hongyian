export interface DeviceContactItem {
  cursor: number; boot_id: number; message_id: number; revision: number;
  epoch: number; direction: string; code: number; reply_to: number; seen: boolean; received_at: number; text?:string;author?:string;request_id?:string;
}
export interface DeviceContactFeed { items: DeviceContactItem[]; next_cursor: number; has_more: boolean; text_supported?:boolean;boot_id?:number; }
export function mergeDeviceContacts(previous: DeviceContactItem[], incoming: DeviceContactItem[]): DeviceContactItem[] {
  const items = previous.slice();
  incoming.forEach((item: DeviceContactItem) => {
    const index = items.findIndex((old: DeviceContactItem) => old.boot_id === item.boot_id && old.message_id === item.message_id);
    if (index < 0) { items.push(item); }
    else if (item.revision > items[index].revision) { items[index] = item; }
  });
  return items.sort((a: DeviceContactItem,b: DeviceContactItem) => a.cursor - b.cursor).slice(-200);
}
export function contactText(item: DeviceContactItem): string {
  if(item.code===4){return item.text||'文字内容未同步';}
  const board = ['','我一切正常','请给我打电话','需要生活协助'];
  const family = ['','已收到消息','正在赶来','已联系人员协助'];
  return (item.direction === 'board' ? board : family)[item.code] || '暂不支持的消息';
}

export function chatBytes(text:string):number {
  let bytes=0;for(let i=0;i<text.length;i++){const c=text.charCodeAt(i);if(c<128){bytes++;}else if(c<2048){bytes+=2;}else if(c>=0xD800&&c<=0xDBFF){const next=text.charCodeAt(i+1);if(!(next>=0xDC00&&next<=0xDFFF)){return Infinity;}bytes+=4;i++;}else if(c>=0xDC00&&c<=0xDFFF){return Infinity;}else{bytes+=3;}}return bytes;
}
export function validChatText(text:string):boolean{return !!text.trim()&&chatBytes(text)<=120&&!/[\u0000-\u0008\u000B-\u001F\u007F]/.test(text);}
