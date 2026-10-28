import { AlarmEvent, alarmKey } from './Models';

export function validAlarm(item: AlarmEvent): boolean {
  if (!item) { return false; }
  return Number.isInteger(item.boot_id) && item.boot_id >= 0 &&
    Number.isInteger(item.event_id) && item.event_id >= 0 &&
    Number.isInteger(item.code) && item.code >= 1 && item.code <= 5 &&
    (item.phase === 'start' || item.phase === 'end');
}

export function mergeAlarms(current: AlarmEvent[], incoming: AlarmEvent[]): AlarmEvent[] {
  const records = new Map<string, AlarmEvent>();
  current.concat(incoming).filter(validAlarm).forEach((item: AlarmEvent) => {
    const key = alarmKey(item);
    const old = records.get(key);
    if (!old || old.phase !== 'end' || item.phase === 'end') { records.set(key, item); }
  });
  return Array.from(records.values()).sort((a: AlarmEvent, b: AlarmEvent) =>
    b.epoch - a.epoch || b.boot_id - a.boot_id || b.event_id - a.event_id).slice(0, 100);
}

/* Session-local deduplication: polling and reconnects must not repeat a popup.
 * End records are processed first, so historical/late starts cannot reopen one. */
export class AlarmInbox {
  private seen: Set<string> = new Set<string>();
  private ended: Set<string> = new Set<string>();
  accept(items: AlarmEvent[]): AlarmEvent[] {
    items.filter((item: AlarmEvent) => validAlarm(item) && item.phase === 'end').forEach((item: AlarmEvent) => {
      this.ended.add(alarmKey(item));
      this.seen.add(alarmKey(item));
    });
    const fresh: AlarmEvent[] = [];
    items.filter((item: AlarmEvent) => validAlarm(item) && item.phase === 'start').forEach((item: AlarmEvent) => {
      const key = alarmKey(item);
      if (!this.seen.has(key) && !this.ended.has(key)) { fresh.push(item); }
      this.seen.add(key);
    });
    return fresh;
  }
}
