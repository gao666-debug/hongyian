import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';

// One home can be used by several accounts (owner, caregiver, viewer) on the same phone.
// Only the server session token is kept, never the password; an expired entry asks for the password again.
export interface SavedAccount {
  server: string;
  username: string;
  role: string;
  token: string;
  expiresAt: number;
  devices: number;
  lastUsed: number;
}

const STORE: string = 'yihongan-accounts';
const KEY: string = 'accounts';
const LIMIT: number = 8;

export class Accounts {
  static async load(context: common.UIAbilityContext): Promise<SavedAccount[]> {
    try {
      const store = await preferences.getPreferences(context, { name: STORE });
      const raw = await store.get(KEY, '') as string;
      if (!raw) { return []; }
      const parsed = JSON.parse(raw) as SavedAccount[];
      if (!Array.isArray(parsed)) { return []; }
      return parsed.filter((item: SavedAccount): boolean =>
        !!item && typeof item.username === 'string' && typeof item.server === 'string' && typeof item.token === 'string');
    } catch (error) { return []; }
  }

  private static async write(context: common.UIAbilityContext, list: SavedAccount[]): Promise<void> {
    try {
      const store = await preferences.getPreferences(context, { name: STORE });
      await store.put(KEY, JSON.stringify(list));
      await store.flush();
    } catch (error) { /* Switching still works within this session. */ }
  }

  static async save(context: common.UIAbilityContext, entry: SavedAccount): Promise<SavedAccount[]> {
    const list = (await Accounts.load(context)).filter((item: SavedAccount): boolean =>
      !(item.username === entry.username && item.server === entry.server)).map((item:SavedAccount):SavedAccount=>({
        server:item.server,username:item.username,role:item.role,token:'',expiresAt:0,devices:item.devices,lastUsed:item.lastUsed
      }));
    list.unshift(entry);
    const kept = list.slice(0, LIMIT);
    await Accounts.write(context, kept);
    return kept;
  }

  static async remove(context: common.UIAbilityContext, server: string, username: string): Promise<SavedAccount[]> {
    const list = (await Accounts.load(context)).filter((item: SavedAccount): boolean =>
      !(item.username === username && item.server === server));
    await Accounts.write(context, list);
    return list;
  }

  static async touch(context: common.UIAbilityContext, server: string, username: string): Promise<SavedAccount[]> {
    const list = await Accounts.load(context);
    const next = list.map((item: SavedAccount): SavedAccount =>
      item.username === username && item.server === server ? {
        server: item.server, username: item.username, role: item.role, token: item.token,
        expiresAt: item.expiresAt, devices: item.devices, lastUsed: Date.now()
      } : item);
    await Accounts.write(context, next);
    return next;
  }

  static usable(entry: SavedAccount, now: number): boolean {
    return !!entry.token && entry.expiresAt > now;
  }
}
