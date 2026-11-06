#include "local_access.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void safety_lock(void){}void safety_unlock(void){}unsigned safety_boot_id(void){return 42;}
static const char *a="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *b="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
int main(void){
 unsigned id,again;local_access_init();assert(!local_pair_identity(a,100));assert(local_pair_request("short",100,&id)==1);
 assert(!local_pair_request(a,100,&id));assert(local_pair_pending(100)==id);assert(!local_pair_identity(a,100));
 assert(!local_pair_request(a,110,&again)&&id==again);assert(local_pair_request(b,120,&again)==4);
 assert(!local_pair_approve(200));assert(local_pair_identity(a,200)==id);assert(local_pair_count(200)==1);
 assert(!local_pair_identity(b,200));assert(!local_pair_identity(a,1800200));assert(!local_pair_count(1800200));
 assert(!local_pair_request(b,1800201,&id));assert(local_pair_pending(1860201)==0);assert(local_pair_approve(1860201)==1);
 assert(!local_pair_request(a,1860202,&id));assert(!local_pair_approve(1860202));local_pair_cancel(b);assert(local_pair_identity(a,1860202));
 local_pair_cancel(a);assert(!local_pair_identity(a,1860202));
 assert(!local_message_board(2,0,100));assert(local_message_board(1,0,101)==4);
 assert(local_message_reply(0,1,1,"req",0,100,&id)==1);assert(local_message_reply(1,1,99,"req",0,100,&id)==2);
 assert(!local_message_reply(1,1,1,"req",0,100,&id));assert(!local_message_reply(1,1,1,"req",0,100,&again)&&id==again);
 assert(local_message_reply(1,2,1,"req",0,100,&again)==4);assert(!local_message_reply(2,2,1,"req",0,100,&again)&&id!=again);
 assert(local_message_seen(again,0)==2);assert(!local_message_seen(1,0));
 int selected,board,reply,seen;local_message_view(&selected,&board,&reply,&seen);assert(board==2&&reply==2&&!seen);
 local_message_confirm();local_message_view(&selected,&board,&reply,&seen);assert(seen);
 local_message_select();local_message_view(&selected,&board,&reply,&seen);assert(selected==2);
 char buf[4096];assert(!local_messages_json(buf,sizeof(buf)));assert(strstr(buf,"\"boot_id\":42"));assert(!strstr(buf,"req"));assert(local_messages_json(buf,8)==-1);
 assert(!local_message_board(3,0,2200));local_message_view(&selected,&board,&reply,&seen);assert(board==3&&reply==0&&!seen);
 unsigned published_id,published_revision;while(local_message_cloud_next(buf,sizeof(buf),&published_id,&published_revision)>0)local_message_cloud_done(published_id,published_revision);
 for(unsigned i=0;i<25;i++){assert(!local_message_board(1,1700000000,4400+i*2100));while(local_message_cloud_next(buf,sizeof(buf),&published_id,&published_revision)>0)local_message_cloud_done(published_id,published_revision);}
 assert(local_message_reply(1,1,1,"evicted",0,0,&id)==2);
 local_access_init();assert(!local_message_board(1,0,0));
 unsigned event_id,revision;assert(local_message_cloud_next(buf,sizeof(buf),&event_id,&revision)==1);assert(strstr(buf,"contact_event"));assert(event_id==1&&revision==1);
 assert(local_message_cloud_next(buf,sizeof(buf),&id,&again)==1);local_message_cloud_done(event_id,revision);assert(!local_message_cloud_next(buf,sizeof(buf),&id,&again));
 assert(!local_contact_validate("{\"action\":\"seen\",\"boot_id\":42,\"message_id\":1}"));
 assert(local_contact_validate("{\"action\":\"seen\",\"boot_id\":43,\"message_id\":1}")==4);
 assert(!local_contact_execute("{\"action\":\"seen\",\"boot_id\":42,\"message_id\":1}",0,1));
 assert(local_message_cloud_next(buf,sizeof(buf),&id,&again)==1&&again==2);local_message_cloud_done(id,1);assert(local_message_cloud_next(buf,sizeof(buf),&id,&again)==1);local_message_cloud_done(id,2);
 assert(!local_contact_execute("{\"action\":\"reply\",\"boot_id\":42,\"message_id\":1,\"code\":2,\"request_id\":\"cloud-reply\"}",0,1));
 local_message_confirm();assert(local_message_cloud_next(buf,sizeof(buf),&id,&again)==1);assert(strstr(buf,"\"seen\":true"));
 local_access_init();assert(!local_pair_count(0));assert(!local_messages_json(buf,sizeof(buf)));assert(strstr(buf,"\"items\":[]"));
 local_access_init();assert(local_text_valid("你好 😀"));assert(!local_text_valid(" \n\t"));assert(!local_text_valid("\xc0\x80"));assert(!local_text_valid("\xed\xa0\x80"));
 char longtext[124];for(int i=0;i<41;i++)memcpy(longtext+i*3,"中",3);longtext[123]=0;assert(!local_text_valid(longtext));longtext[120]=0;assert(local_text_valid(longtext));
 assert(!local_message_text(1,0,0,"你好 <script> \"\\","NFC网页","text-one",0,0,&id));
 assert(!local_message_text(2,0,0,"你好 <script> \"\\","NFC网页","text-one",0,0,&again)&&id==again);
 assert(local_message_text(1,0,0,"changed","NFC网页","text-one",0,0,&again)==4);
 assert(!local_contact_execute("{\"action\":\"chat\",\"boot_id\":42,\"message_id\":0,\"code\":4,\"request_id\":\"family-text\",\"text\":\"家人回复 😀\",\"author\":\"alice\"}",0,0));
 local_message_view(&selected,&board,&reply,&seen);assert(reply==4&&!seen);local_message_confirm();local_message_view(&selected,&board,&reply,&seen);assert(seen);
 char escaped[8192];assert(!local_messages_json(escaped,sizeof(escaped)));assert(strstr(escaped,"家人回复"));
 local_access_init();for(unsigned i=0;i<16;i++)assert(!local_message_text(1,0,0,"waiting","NFC网页",(snprintf(buf,sizeof(buf),"q-%u",i),buf),0,0,&id));
 assert(local_message_text(1,0,0,"full","NFC网页","overflow",0,0,&id)==3);
 puts("PASS NFC text: strict UTF-8 and limits, JSON escaping, stable retries, unsolicited family text, read confirmation and no unsent overwrite");
 puts("PASS physical pairing: deny before approval, expiry, cancellation, isolation, reboot; local messages: dedup, ACK direction, ring bounds");
}
