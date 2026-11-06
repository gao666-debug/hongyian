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
typedef struct {unsigned id,epoch,client,reply_to;int code,reply,seen;unsigned revision,published;char request[65],text[LOCAL_TEXT_BYTES+1],author[33];} message_t;
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
 if(message_count==MESSAGE_COUNT&&messages[0].published!=messages[0].revision)return NULL;
 if(message_count==MESSAGE_COUNT){memmove(messages,messages+1,sizeof(messages[0])*(MESSAGE_COUNT-1));message_count--;}
 message_t *m=&messages[message_count++];memset(m,0,sizeof(*m));m->id=++message_serial;m->revision=1;return m;
}
int local_message_board(int code,unsigned epoch,uint64_t now){
 if(code<1||code>3)return 1;safety_lock();if(now<next_board_message){safety_unlock();return 4;}
 message_t *m=append();if(!m){safety_unlock();return 3;}m->code=code;m->epoch=epoch;next_board_message=now+2000;safety_unlock();return 0;
}
static int request_valid(const char *s){
 if(!s||!*s||strlen(s)>64)return 0;
 for(;*s;s++)if(!isalnum((unsigned char)*s)&&*s!='-'&&*s!='_')return 0;
 return 1;
}

int local_text_valid(const char *text){
 if(!text||!*text||strlen(text)>LOCAL_TEXT_BYTES)return 0;
 const unsigned char *p=(const unsigned char*)text;int visible=0;
 while(*p){unsigned u;int count;
  if(*p<0x80){u=*p;count=1;if(u<32&&u!=10&&u!=9)return 0;if(u==127)return 0;}
  else if(*p>=0xc2&&*p<=0xdf){u=*p&31;count=2;}
  else if(*p>=0xe0&&*p<=0xef){u=*p&15;count=3;}
  else if(*p>=0xf0&&*p<=0xf4){u=*p&7;count=4;}else return 0;
  for(int i=1;i<count;i++){if(!p[i]||p[i]<0x80||p[i]>0xbf)return 0;u=(u<<6)|(p[i]&63);}
  if((count==2&&u<0x80)||(count==3&&u<0x800)||(count==4&&u<0x10000)||(u>=0xd800&&u<=0xdfff)||u>0x10ffff)return 0;
  int white=u==9||u==10||u==32||u==0x85||u==0xa0||u==0x1680||(u>=0x2000&&u<=0x200a)||u==0x2028||u==0x2029||u==0x202f||u==0x205f||u==0x3000;
  if(!white)visible=1;p+=count;
 }return visible;
}
int local_message_text(unsigned client,int family,unsigned to,const char *text,const char *author,const char *request,unsigned epoch,uint64_t now,unsigned *id){
 (void)now;if(!client||!local_text_valid(text)||!author||!local_text_valid(author)||strlen(author)>32||!request_valid(request)||!id)return 1;
 safety_lock();int found=to==0;
 for(unsigned i=0;i<message_count;i++){
  message_t *m=&messages[i];if((m->client==client||!family)&&m->reply==!!family&&!strcmp(m->request,request)){
   if(m->code!=4||m->reply_to!=to||strcmp(m->text,text)||strcmp(m->author,author)){safety_unlock();return 4;}
   *id=m->id;safety_unlock();return 0;
  }
  if(m->id==to&&!m->reply)found=1;
 }
 if(!found){safety_unlock();return 2;}
 message_t *m=append();if(!m){safety_unlock();return 3;}
 m->code=4;m->reply=!!family;m->reply_to=to;m->client=client;m->epoch=epoch;
 snprintf(m->text,sizeof(m->text),"%s",text);snprintf(m->author,sizeof(m->author),"%s",author);snprintf(m->request,sizeof(m->request),"%s",request);*id=m->id;safety_unlock();return 0;
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
 message_t *m=append();if(!m){safety_unlock();return 3;}m->reply=1;m->client=client;m->reply_to=to;m->code=code;m->epoch=epoch;
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
 for(unsigned i=0;i<message_count;i++)if(messages[i].reply&&(messages[i].reply_to==latest||messages[i].reply_to==0)){*reply=messages[i].code;*confirmed=messages[i].seen;}
 safety_unlock();
}
void local_message_confirm(void){
 safety_lock();unsigned latest=0;
 for(unsigned i=0;i<message_count;i++)if(!messages[i].reply)latest=messages[i].id;
 for(unsigned i=message_count;i>0;i--)if(messages[i-1].reply&&(messages[i-1].reply_to==latest||messages[i-1].reply_to==0)){if(!messages[i-1].seen){messages[i-1].seen=1;messages[i-1].revision++;}break;}
 safety_unlock();
}

static cJSON *message_json(const message_t *m,int cloud,unsigned boot){
 cJSON *j=cJSON_CreateObject();if(!j)return NULL;
 cJSON_AddNumberToObject(j,cloud?"message_id":"id",m->id);cJSON_AddNumberToObject(j,"epoch",m->epoch);
 cJSON_AddStringToObject(j,"direction",m->reply?"family":"board");cJSON_AddNumberToObject(j,"code",m->code);
 cJSON_AddNumberToObject(j,"reply_to",m->reply_to);cJSON_AddBoolToObject(j,"seen",m->seen);
 if(cloud){cJSON_AddNumberToObject(j,"schema",1);cJSON_AddNumberToObject(j,"boot_id",boot);cJSON_AddNumberToObject(j,"revision",m->revision);cJSON_AddStringToObject(j,"request_id",m->request);}
 else cJSON_AddBoolToObject(j,"uploaded",m->published==m->revision);
 if(m->code==4){cJSON_AddStringToObject(j,"text",m->text);cJSON_AddStringToObject(j,"author",m->author);}
 if(cJSON_GetArraySize(j)!=(cloud?10:7)+(m->code==4?2:0)){cJSON_Delete(j);return NULL;}
 return j;
}
int local_messages_json(char *out,int n){
 unsigned boot=safety_boot_id();cJSON *j=cJSON_CreateObject(),*a=cJSON_CreateArray();if(!j||!a){cJSON_Delete(j);cJSON_Delete(a);return -1;}
 cJSON_AddNumberToObject(j,"boot_id",boot);cJSON_AddBoolToObject(j,"volatile",1);cJSON_AddItemToObject(j,"items",a);
 int complete=1;safety_lock();for(unsigned i=0;i<message_count;i++){cJSON *item=message_json(&messages[i],0,boot);if(!item){complete=0;break;}cJSON_AddItemToArray(a,item);}safety_unlock();if(!complete){cJSON_Delete(j);return -1;}
 int ok=cJSON_PrintPreallocated(j,out,n,0);cJSON_Delete(j);return ok?0:-1;
}
static int positive(cJSON *v){return cJSON_IsNumber(v)&&v->valuedouble>=1&&v->valuedouble<=2147483647&&v->valuedouble==v->valueint;}

typedef struct {int code,family;unsigned message;char request[65],text[LOCAL_TEXT_BYTES+1],author[33];} contact_request_t;
static int contact_parse(const char *payload,contact_request_t *out){
 if(!payload||strstr(payload,"\\u0000"))return 1;memset(out,0,sizeof(*out));
 cJSON *j=cJSON_ParseWithOpts(payload,NULL,1),*a=cJSON_GetObjectItemCaseSensitive(j,"action"),*b=cJSON_GetObjectItemCaseSensitive(j,"boot_id"),*m=cJSON_GetObjectItemCaseSensitive(j,"message_id"),*c=cJSON_GetObjectItemCaseSensitive(j,"code"),*r=cJSON_GetObjectItemCaseSensitive(j,"request_id"),*t=cJSON_GetObjectItemCaseSensitive(j,"text"),*author=cJSON_GetObjectItemCaseSensitive(j,"author");int result=1;
 if(!cJSON_IsObject(j)||!cJSON_IsString(a)||!positive(b)||!cJSON_IsNumber(m)||m->valuedouble<0||m->valuedouble>2147483647||m->valuedouble!=m->valueint)goto end;
 if((unsigned)b->valueint!=safety_boot_id()){result=4;goto end;}out->message=m->valueint;
 if(!strcmp(a->valuestring,"seen")&&out->message&&(cJSON_GetArraySize(j)==3||(cJSON_GetArraySize(j)==5&&cJSON_IsNumber(c)&&c->valuedouble==0&&cJSON_IsString(r)&&!r->valuestring[0]))){result=0;goto end;}
 if(strcmp(a->valuestring,"reply")&&strcmp(a->valuestring,"chat"))goto end;
 if(!positive(c)||c->valueint>4||!cJSON_IsString(r)||!request_valid(r->valuestring))goto end;
 if(!strcmp(a->valuestring,"reply")&&!out->message)goto end;
 if(!strcmp(a->valuestring,"chat")&&(out->message||c->valueint!=4))goto end;
 if(c->valueint==4){if(cJSON_GetArraySize(j)!=7||!cJSON_IsString(t)||!local_text_valid(t->valuestring)||!cJSON_IsString(author)||!local_text_valid(author->valuestring)||strlen(author->valuestring)>32)goto end;
  snprintf(out->text,sizeof(out->text),"%s",t->valuestring);snprintf(out->author,sizeof(out->author),"%s",author->valuestring);
 }else if(cJSON_GetArraySize(j)!=5)goto end;
 out->code=c->valueint;out->family=1;snprintf(out->request,sizeof(out->request),"%s",r->valuestring);result=0;
 end:cJSON_Delete(j);return result;
}
int local_contact_validate(const char *payload){contact_request_t r;return contact_parse(payload,&r);}
int local_contact_execute(const char *payload,unsigned epoch,uint64_t now){
 contact_request_t r;unsigned id;int result=contact_parse(payload,&r);if(result)return result;
 if(!r.family)return local_message_seen(r.message,0);
 if(r.code==4)return local_message_text(0xffffffffU,1,r.message,r.text,r.author,r.request,epoch,now,&id);
 return local_message_reply(0xffffffffU,r.code,r.message,r.request,epoch,now,&id);
}
int local_message_cloud_next(char *out,int size,unsigned *id,unsigned *revision){
 message_t copy;int found=0;unsigned boot=safety_boot_id();safety_lock();for(unsigned i=0;i<message_count;i++)if(messages[i].published!=messages[i].revision){copy=messages[i];found=1;break;}safety_unlock();if(!found)return 0;
 cJSON *j=cJSON_CreateObject();if(!j)return -1;char key[64];snprintf(key,sizeof(key),"contact:%u:%u:%u",boot,copy.id,copy.revision);
 cJSON_AddStringToObject(j,"name","contact_event");cJSON_AddStringToObject(j,"id",key);cJSON *content=message_json(&copy,1,boot);if(!content){cJSON_Delete(j);return -1;}cJSON_AddItemToObject(j,"content",content);
 int ok=cJSON_PrintPreallocated(j,out,size,0);cJSON_Delete(j);if(!ok)return -1;*id=copy.id;*revision=copy.revision;return 1;
}
void local_message_cloud_done(unsigned id,unsigned revision){safety_lock();for(unsigned i=0;i<message_count;i++)if(messages[i].id==id){if(revision<=messages[i].revision&&revision>messages[i].published)messages[i].published=revision;break;}safety_unlock();}
