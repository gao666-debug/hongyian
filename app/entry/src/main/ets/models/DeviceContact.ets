export interface DeviceContactItem {
  cursor: number; boot_id: number; message_id: number; revision: number;
  epoch: number; direction: string; code: number; reply_to: number; seen: boolean; received_at: number;
}
export interface DeviceContactFeed { items: DeviceContactItem[]; next_cursor: number; has_more: boolean; }
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
  const board = ['','我一切正常','请给我打电话','需要生活协助'];
  const family = ['','已收到消息','正在赶来','已联系人员协助'];
  return (item.direction === 'board' ? board : family)[item.code] || '暂不支持的消息';
}
