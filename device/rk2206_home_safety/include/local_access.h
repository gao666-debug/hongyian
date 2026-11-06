/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_ACCESS_H
#define LOCAL_ACCESS_H
#include <stdint.h>
void local_access_init(void);
/* Browser creates 32 random bytes; only a physical key can approve. RAM only. */
int local_pair_request(const char *token,uint64_t now,unsigned *request_id);
unsigned local_pair_identity(const char *token,uint64_t now);
unsigned local_pair_pending(uint64_t now);
unsigned local_pair_count(uint64_t now);
int local_pair_approve(uint64_t now);
void local_pair_revoke(void);
void local_pair_cancel(const char *token);
int local_message_board(int code,unsigned epoch,uint64_t now);
int local_message_reply(unsigned client,int code,unsigned reply_to,const char *request_id,unsigned epoch,uint64_t now,unsigned *id);
int local_message_seen(unsigned id,int from_board);
int local_messages_json(char *out,int n);
void local_message_view(int *selected,int *board,int *reply,int *confirmed);
void local_message_select(void);
void local_message_confirm(void);
int local_contact_validate(const char *payload);
int local_contact_execute(const char *payload,unsigned epoch,uint64_t now);
int local_message_cloud_next(char *out,int size,unsigned *id,unsigned *revision);
void local_message_cloud_done(unsigned id,unsigned revision);
#endif
