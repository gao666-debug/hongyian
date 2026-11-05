export interface ChatContact { username: string; since?: string; }
export interface ChatMessage { id: string; conversation_id: string; sender: string; kind: string; body: string; duration?: number; created_at: string; }
export interface Conversation { conversation_id: string; kind: string; title: string; owner: string; members: string[]; member_count: number; unread: number; last_message?: ChatMessage; last_message_at?: string; }
export interface ChatContacts { items: ChatContact[]; }
export interface ChatConversations { items: Conversation[]; }
export interface ChatHistory { items: ChatMessage[]; has_more: boolean; next_before?: number; }
export interface ChatInvite { code: string; expires_at: string; }

export function mergeMessages(old: ChatMessage[], incoming: ChatMessage[]): ChatMessage[] {
  const result: ChatMessage[] = old.slice();
  incoming.forEach((item: ChatMessage) => {
    const index = result.findIndex((row: ChatMessage) => row.id === item.id);
    if (index < 0) { result.push(item); } else { result[index] = item; }
  });
  return result.sort((a: ChatMessage, b: ChatMessage) => a.created_at.localeCompare(b.created_at) || a.id.localeCompare(b.id));
}
export function messagePreview(item?: ChatMessage): string {
  if (!item) { return '还没有消息，打个招呼吧'; }
  return item.kind === 'image' ? '[图片]' : item.kind === 'voice' ? '[语音]' : item.body;
}
