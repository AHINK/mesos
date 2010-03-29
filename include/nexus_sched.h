/* Nexus. */

#include <stdint.h>
#include <nexus.h>

#ifndef NEXUS_SCHED_H
#define NEXUS_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif



struct nexus_sched {
  // Human-readable framework name.
  const char* framework_name;

  // Executor information.
  struct nexus_exec_info exec_info;

  // Callbacks.
  void (*registered) (struct nexus_sched*, framework_id);
  void (*slot_offer) (struct nexus_sched*,
                      offer_id,
                      struct nexus_slot*,
                      int);
  void (*slot_offer_rescinded) (struct nexus_sched*, offer_id);
  void (*status_update) (struct nexus_sched*, struct nexus_task_status*);
  void (*framework_message) (struct nexus_sched*,
                             struct nexus_framework_message*);
  void (*slave_lost) (struct nexus_sched*, slave_id);
  void (*error) (struct nexus_sched*, int, const char*);

  // Opaque data to associate extra info with scheduler.
  void* data;
};

int nexus_sched_init(struct nexus_sched*);
int nexus_sched_destroy(struct nexus_sched*);

int nexus_sched_reg(struct nexus_sched*, const char*);

int nexus_sched_unreg(struct nexus_sched*);

int nexus_sched_send_message(struct nexus_sched*,
                             struct nexus_framework_message*);

int nexus_sched_kill_task(struct nexus_sched*, task_id);

int nexus_sched_reply_to_offer(struct nexus_sched*,
                               offer_id,
                               struct nexus_task_desc*,
                               int,
                               const char*);

int nexus_sched_revive_offers(struct nexus_sched*);

int nexus_sched_join(struct nexus_sched*);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_SCHED_H */
