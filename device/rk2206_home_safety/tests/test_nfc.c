#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nfc_tag.h"
#include "ndef.h"
#include "rtdUri.h"
#include "nfcForum.h"
uint8_t nfcPageBuffer[NFC_PAGE_SIZE];
NT3HerrNo errNo;
static uint8_t memory[128];
static int init_result,init_calls,writes,store_calls,fail_page=-1;
unsigned int nfc_init(void){init_calls++;return init_result;}
bool nfc_store_uri_http(RecordPosEnu position,uint8_t *suffix){
 NDEFDataStr data;store_calls++;prepareUrihttp(&data,position,suffix);return NT3HwriteRecord(&data);
}
bool NT3HWriteUserData(uint8_t page,const uint8_t *data){
 writes++;if(page==fail_page)return false;
 assert(page<8);memcpy(memory+page*16,data,16);return true;
}
bool NT3HReadUserData(uint8_t page){assert(page<8);memcpy(nfcPageBuffer,memory+page*16,16);return true;}
bool NT3HReadHeaderNfc(uint8_t *end,uint8_t *header){*end=memory[1];*header=memory[2];return true;}
bool NT3HWriteHeaderNfc(uint8_t end,uint8_t header){memory[1]=end;memory[2]=header;return true;}
int main(void){
 memset(memory,0xaa,sizeof(memory));nfc_tag_init();
 assert(writes==0);assert(nfc_tag_set_ipv4("192.168.2.1"));
 const char *suffix="192.168.2.1:8080/";
 assert(init_calls==1&&store_calls==1&&writes>1);
 assert(memory[0]==0x03&&memory[1]==strlen(suffix)+5); /* NDEF TLV length. */
 assert(memory[2]==0xd1&&memory[3]==1&&memory[4]==strlen(suffix)+1&&memory[5]=='U');
 assert(memory[6]==0x03); /* http://, not http://www. */
 assert(!memcmp(memory+7,suffix,strlen(suffix)));
 assert(memory[7+strlen(suffix)]==0xfe);
 char url[128];snprintf(url,sizeof(url),"http://%.*s",(int)memory[4]-1,memory+7);
 assert(!strcmp(url,"http://192.168.2.1:8080/"));
 int old_writes=writes;
 assert(nfc_tag_set_ipv4("192.168.2.1"));assert(writes==old_writes);
 assert(!nfc_tag_set_ipv4(""));assert(!nfc_tag_set_ipv4("1.2.3.999"));
 assert(!nfc_tag_set_ipv4("1.2.3.4/path"));assert(!nfc_tag_set_ipv4("0.0.0.0"));
 assert(!nfc_tag_set_ipv4("127.0.0.1"));assert(writes==old_writes);
 assert(nfc_tag_set_ipv4("10.125.126.36"));
 assert(!memcmp(memory+7,"10.125.126.36:8080/",18));
 fail_page=0;assert(!nfc_tag_set_ipv4("10.1.2.3"));
 fail_page=-1;assert(nfc_tag_set_ipv4("10.1.2.3"));
 assert(!memcmp(memory+7,"10.1.2.3:8080/",13));
 writes=store_calls=0;init_result=1;nfc_tag_init();assert(!writes&&!store_calls);
 init_result=0;fail_page=0;writes=0;
 assert(!nfc_store_uri_http(NDEFFirstPos,(uint8_t *)suffix));assert(writes==1);
 puts("PASS NFC: real encoded URL, single first/last URI record, old-message replacement, init/write failures");
}
