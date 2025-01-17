
// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "common.bpf.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define MAX_SYSCALLS 512
/*
typedef struct activity {
  uint64_t histogram[MAX_SYSCALLS];
} activity_t;

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key_size,  sizeof(pid_t));
    __type(value_size, sizeof(activity_t));
    __uint(max_entries, 4096);
} activities SEC(".maps");

// used in order to manipulate objects bigger than the 512 bytes stack limit
struct  {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key_size, sizeof(int));
    __type(value_size, sizeof(activity_t));
    __uint(max_entries, 1);
} memory SEC(".maps");
*/


typedef struct activity {
  uint64_t histogram[MAX_SYSCALLS];
  uint64_t cgroupid;
} activity_t;

struct  {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key,pid_t);
    __type(value, activity_t);
    __uint(max_entries, 4096);
} activities SEC(".maps/activities");

// used in order to manipulate objects bigger than the 512 bytes stack limit
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, int);
    __type(value, activity_t);
    __uint(max_entries, 1);
} memory SEC(".maps/memory");

// struct trace_event_raw_sys_enter {
//         struct trace_entry ent;
//         long int id;
//         long unsigned int args[6];
//         char __data[0];
// };
SEC("tracepoint/sys_enter")
int sys_enter(struct trace_event_raw_sys_enter *ctx) {
  pid_t tgid = bpf_get_current_pid_tgid() >> 32; 

  activity_t *activity = bpf_map_lookup_elem(&activities, &tgid);
  // if we can't find it, initialize it
  if (activity == NULL) {
    u32 key = 0;
    activity = bpf_map_lookup_elem(&memory, &key);
    if (!activity) {
      LOG_ERROR("can't get activity_t memory for %d", tgid);
      return 0;
    }
    // We want to always start with a 0 initialized activity_t, so
    // we copy it over the activities map before making changes.
    bpf_map_update_elem(&activities, &tgid, activity, BPF_ANY);
    activity = bpf_map_lookup_elem(&activities, &tgid);
    if (activity == NULL)
      return 0;
  }

  // TODO:
  // The following code is susceptible to critical sections when multiple
  // processors access the same pid row at the same time. Also, userspace
  // might read a partial update.
  // The best solution would be using bpf_spin_lock, but it's not supported
  // in tracepoints. A possible solution would be using CPU arrays or perf
  // events, but for now we're ignoring the problem.

  // limit the index to avoid "unbounded memory access, make sure to bounds
  // check any array access into a map"
  uint32_t syscall_number = ctx->id & (MAX_SYSCALLS - 1);
  activity->histogram[syscall_number]++;
  activity->cgroupid = bpf_get_current_cgroup_id();
  bpf_map_update_elem(&activities, &tgid, activity, BPF_ANY);

  return 0;
}

// When a process exits, we cleanup the activities map
// FIXME: since activity check is poll based, we'll generate
// no events for short-lived processes.

// This is attached to tracepoint:sched:sched_process_exit
SEC("raw_tracepoint/sched_process_exit")
int BPF_PROG(sched_process_exit, struct task_struct *p) {
  pid_t tgid;
  
  if (!is_thread(&tgid)) {
    bpf_map_delete_elem(&activities, &tgid);
  }
  return 0;
}
