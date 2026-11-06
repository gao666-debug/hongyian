/* SPDX-License-Identifier: Apache-2.0 */
#include "local_access.h"
#include "safety_runtime.h"
#include <ctype.h>
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#define PAIR_TTL 1800000ULL
#define REQUEST_TTL 60000ULL
#define MESSAGE_COUNT 16
typedef struct {char token[65];uint64_t until;unsigned identity;} session_t;
static session_t sessions[4];
static char pending_token[65];
static unsigned pending_id,serial;
static uint64_t pending_until,next_request;
typedef struct {unsigned id,epoch,client,reply_to;int code,reply,seen;unsigned revision,published;char request[65];} message_t;
static message_t messages[MESSAGE_COUNT];
static unsigned message_serial,message_count;
static int selection;
static uint64_t next_board_message;
static int valid_token(const char *token){
 if(!token||strlen(token)!=64)return 0;
 for(int i=0;i<64;i++)if(!isxdigit((unsigned char)token[i]))return 0;
 return 1;
}
static void expire(uint64_t now){
 if(pending_id && now>=pending_until){pending_id=0;pending_token[0]=0;}
 for(int i=0;i<4;i++)if(sessions[i].identity && now>=sessions[i].until)memset(&sessions[i],0,sizeof(sessions[i]));
}
void local_access_init(void){
 safety_lock();memset(sessions,0,sizeof(sessions));memset(messages,0,sizeof(messages));
 pending_token[0]=0;pending_id=serial=message_serial=message_count=0;
 pending_until=next_request=next_board_message=0;selection=1;safety_unlock();
}
unsigned local_pair_identity(const char *token,uint64_t now){
 if(!valid_token(token))return 0;unsigned result=0;
 safety_lock();expire(now);
 for(int i=0;i<4;i++)if(sessions[i].identity&&!strcmp(token,sessions[i].token)){result=sessions[i].identity;break;}
 safety_unlock();return result;
}
int local_pair_request(const char *token,uint64_t now,unsigned *id){
 if(!valid_token(token)||!id)return 1;
 safety_lock();expire(now);
 if(pending_id&&!strcmp(token,pending_token)){*id=pending_id;safety_unlock();return 0;}
 if(pending_id||now<next_request){safety_unlock();return 4;}
 int free_slot=0;for(int i=0;i<4;i++)if(!sessions[i].identity)free_slot=1;
 if(!free_slot){safety_unlock();return 3;}
 snprintf(pending_token,sizeof(pending_token),"%s",token);pending_id=++serial;
 pending_until=now+REQUEST_TTL;next_request=now+10000;*id=pending_id;safety_unlock();return 0;
}
unsigned local_pair_pending(uint64_t now){safety_lock();expire(now);unsigned id=pending_id;safety_unlock();return id;}
unsigned local_pair_count(uint64_t now){safety_lock();expire(now);unsigned n=0;for(int i=0;i<4;i++)if(sessions[i].identity)n++;safety_unlock();return n;}
int local_pair_approve(uint64_t now){
 safety_lock();expire(now);if(!pending_id){safety_unlock();return 1;}
 for(int i=0;i<4;i++)if(!sessions[i].identity){
  snprintf(sessions[i].token,sizeof(sessions[i].token),"%s",pending_token);
  sessions[i].identity=pending_id;sessions[i].until=now+PAIR_TTL;
  pending_id=0;pending_token[0]=0;safety_unlock();return 0;
 }
 safety_unlock();return 3;
}
void local_pair_revoke(void){safety_lock();memset(sessions,0,sizeof(sessions));pending_token[0]=0;pending_id=0;safety_unlock();}
void local_pair_cancel(const char *token){
 if(!valid_token(token))return;
 safety_lock();for(int i=0;i<4;i++)if(!strcmp(token,sessions[i].token))memset(&sessions[i],0,sizeof(sessions[i]));
 if(pending_id&&!strcmp(token,pending_token)){pending_id=0;pending_token[0]=0;}
 safety_unlock();
}
static message_t *append(void){
 if(message_count==MESSAGE_COUNT){memmove(messages,messages+1,sizeof(messages[0])*(MESSAGE_COUNT-1));message_count--;}
 message_t *m=&messages[message_count++];memset(m,0,sizeof(*m));m->id=++message_serial;m->revision=1;return m;
}
int local_message_board(int code,unsigned epoch,uint64_t now){
 if(code<1||code>3)return 1;safety_lock();if(now<next_board_message){safety_unlock();return 4;}
 message_t *m=append();m->code=code;m->epoch=epoch;next_board_message=now+2000;safety_unlock();return 0;
}
static int request_valid(const char *s){
 if(!s||!*s||strlen(s)>64)return 0;
 for(;*s;s++)if(!isalnum((unsigned char)*s)&&*s!='-'&&*s!='_')return 0;
 return 1;
}
int local_message_reply(unsigned client,int code,unsigned to,const char *request,unsigned epoch,uint64_t now,unsigned *id){
 (void)now;if(!client||code<1||code>3||!to||!request_valid(request)||!id)return 1;
 safety_lock();int found=0;
 for(unsigned i=0;i<message_count;i++){
  message_t *m=&messages[i];
  if(m->reply&&m->client==client&&!strcmp(request,m->request)){
   if(m->code!=code||m->reply_to!=to){safety_unlock();return 4;}
   *id=m->id;safety_unlock();return 0;
  }
  if(m->id==to&&!m->reply)found=1;
 }
 if(!found){safety_unlock();return 2;}
 message_t *m=append();m->reply=1;m->client=client;m->reply_to=to;m->code=code;m->epoch=epoch;
 snprintf(m->request,sizeof(m->request),"%s",request);*id=m->id;safety_unlock();return 0;
}
int local_message_seen(unsigned id,int from_board){
 int found=0;safety_lock();for(unsigned i=0;i<message_count;i++)if(messages[i].id==id&&messages[i].reply==!!from_board){if(!messages[i].seen){messages[i].seen=1;messages[i].revision++;}found=1;break;}
 safety_unlock();return found?0:2;
}
void local_message_select(void){safety_lock();selection=selection%3+1;safety_unlock();}
void local_message_view(int *selected,int *board,int *reply,int *confirmed){
 safety_lock();*selected=selection;*board=*reply=*confirmed=0;
 unsigned latest=0;
 for(unsigned i=0;i<message_count;i++)if(!messages[i].reply){*board=messages[i].code;latest=messages[i].id;}
 for(unsigned i=0;i<message_count;i++)if(messages[i].reply&&messages[i].reply_to==latest){*reply=messages[i].code;*confirmed=messages[i].seen;}
 safety_unlock();
}
void local_message_confirm(void){
 safety_lock();unsigned latest=0;
 for(unsigned i=0;i<message_count;i++)if(!messages[i].reply)latest=messages[i].id;
 for(unsigned i=message_count;i>0;i--)if(messages[i-1].reply&&messages[i-1].reply_to==latest){if(!messages[i-1].seen){messages[i-1].seen=1;messages[i-1].revision++;}break;}
 safety_unlock();
}
int local_messages_json(char *out,int n){
 unsigned boot=safety_boot_id();int used=snprintf(out,n,"{\"boot_id\":%u,\"volatile\":true,\"items\":[",boot);
 if(used<0||used>=n)return -1;safety_lock();
 for(unsigned i=0;i<message_count;i++){
  message_t *m=&messages[i];int count=snprintf(out+used,n-used,"%s{\"id\":%u,\"epoch\":%u,\"direction\":\"%s\",\"code\":%d,\"reply_to\":%u,\"seen\":%s}",i?",":"",m->id,m->epoch,m->reply?"family":"board",m->code,m->reply_to,m->seen?"true":"false");
  if(count<0||count>=n-used){safety_unlock();return -1;}used+=count;
 }
 safety_unlock();if(n-used<3)return -1;strcpy(out+used,"]}");return 0;
}

static int positive(cJSON *v){return cJSON_IsNumber(v)&&v->valuedouble>=1&&v->valuedouble<=2147483647&&v->valuedouble==v->valueint;}
static int contact_parse(const char *payload,int *code,unsigned *message,int *reply,char *request){
 cJSON *j=cJSON_ParseWithOpts(payload,NULL,1),*a=cJSON_GetObjectItemCaseSensitive(j,"action"),*b=cJSON_GetObjectItemCaseSensitive(j,"boot_id"),*m=cJSON_GetObjectItemCaseSensitive(j,"message_id"),*c=cJSON_GetObjectItemCaseSensitive(j,"code"),*r=cJSON_GetObjectItemCaseSensitive(j,"request_id");
 int result=1;
 if(!cJSON_IsObject(j)||!cJSON_IsString(a)||!positive(b)||!positive(m))goto end;
 if((unsigned)b->valueint!=safety_boot_id()){result=4;goto end;}
 if(!strcmp(a->valuestring,"seen")&&(cJSON_GetArraySize(j)==3||(cJSON_GetArraySize(j)==5&&cJSON_IsNumber(c)&&c->valuedouble==0&&cJSON_IsString(r)&&!r->valuestring[0]))){*reply=0;*code=0;*message=m->valueint;request[0]=0;result=0;}
 else if(!strcmp(a->valuestring,"reply")&&cJSON_GetArraySize(j)==5&&positive(c)&&c->valueint<=3&&cJSON_IsString(r)&&request_valid(r->valuestring)){
  *reply=1;*code=c->valueint;*message=m->valueint;snprintf(request,65,"%s",r->valuestring);result=0;
 }
 end:cJSON_Delete(j);return result;
}
int local_contact_validate(const char *payload){int code,reply;unsigned message;char req[65];return contact_parse(payload,&code,&message,&reply,req);}
int local_contact_execute(const char *payload,unsigned epoch,uint64_t now){
 int code,reply;unsigned message,id;char req[65];int result=contact_parse(payload,&code,&message,&reply,req);if(result)return result;
 return reply?local_message_reply(0xffffffffU,code,message,req,epoch,now,&id):local_message_seen(message,0);
}
int local_message_cloud_next(char *out,int size,unsigned *id,unsigned *revision){
 message_t copy;int found=0;unsigned boot=safety_boot_id();
 safety_lock();for(unsigned i=0;i<message_count;i++)if(messages[i].published!=messages[i].revision){copy=messages[i];found=1;break;}safety_unlock();
 if(!found)return 0;
 int n=snprintf(out,size,"{\"name\":\"contact_event\",\"id\":\"contact:%u:%u:%u\",\"content\":{\"schema\":1,\"boot_id\":%u,\"message_id\":%u,\"revision\":%u,\"epoch\":%u,\"direction\":\"%s\",\"code\":%d,\"reply_to\":%u,\"seen\":%s,\"request_id\":\"%s\"}}",boot,copy.id,copy.revision,boot,copy.id,copy.revision,copy.epoch,copy.reply?"family":"board",copy.code,copy.reply_to,copy.seen?"true":"false",copy.request);
 if(n<0||n>=size)return -1;*id=copy.id;*revision=copy.revision;return 1;
}
void local_message_cloud_done(unsigned id,unsigned revision){safety_lock();for(unsigned i=0;i<message_count;i++)if(messages[i].id==id){if(revision<=messages[i].revision&&revision>messages[i].published)messages[i].published=revision;break;}safety_unlock();}
