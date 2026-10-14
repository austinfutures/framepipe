// rt.hpp — the real-time knobs each experiment turns on, one function per knob.
//
// Every function returns false and fills `err` on failure; callers treat a failure
// as fatal. An experiment labelled "SCHED_FIFO" that silently ran under CFS would
// produce a wrong table, so we never fall back quietly.
#pragma once

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace fp {

// SCHED_FIFO: a runnable FIFO thread preempts every SCHED_OTHER (CFS) thread on its
// CPU immediately and runs until it blocks or a higher-priority RT thread wakes.
// Needs root or CAP_SYS_NICE (or RLIMIT_RTPRIO). Side effect worth knowing: RT
// threads get zero timer slack, so clock_nanosleep wakes them on time instead of up
// to 50 us late.
inline bool set_fifo(int prio, std::string& err) {
  sched_param sp{};
  sp.sched_priority = prio;
  int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
  if (rc != 0) {
    err = std::string("SCHED_FIFO prio ") + std::to_string(prio) + ": " + strerror(rc) +
          " (run as root / CAP_SYS_NICE)";
    return false;
  }
  return true;
}

// CPU pinning: keeps the thread's caches/TLB warm and removes migration latency.
// It does NOT keep other tasks off that CPU — that's isolation (isolcpus / cpusets /
// keeping the load generator elsewhere), which is a separate experiment.
inline bool pin_to_cpu(int cpu, std::string& err) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (rc != 0) {
    err = "pin to CPU " + std::to_string(cpu) + ": " + strerror(rc);
    return false;
  }
  return true;
}

// mlockall: lock every current and future page of this process in RAM, so the
// kernel can never page out (and later major-fault back in) our code, heap, stacks
// or the shared ring. Needs root or CAP_IPC_LOCK (or a big RLIMIT_MEMLOCK).
inline bool lock_all_memory(std::string& err) {
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    err = std::string("mlockall: ") + strerror(errno) + " (run as root / CAP_IPC_LOCK)";
    return false;
  }
  return true;
}

// Touch a chunk of stack now so the hot loop never takes a stack page fault.
inline void prefault_stack() {
  constexpr size_t kBytes = 256 * 1024;
  volatile unsigned char buf[kBytes];
  for (size_t i = 0; i < kBytes; i += 4096) buf[i] = 0;
  (void)buf[0];  // one read: tells the compiler the array is used (silences a warning)
}

// Optional knob: timer slack for SCHED_OTHER threads (default 50 us).
inline bool set_timer_slack(long ns, std::string& err) {
  if (prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0) != 0) {
    err = std::string("PR_SET_TIMERSLACK: ") + strerror(errno);
    return false;
  }
  return true;
}

// Thread names show up in perf sched / bpftrace / top. All of ours start with "fp_"
// so the tracing scripts can filter on that prefix. Max 15 chars.
inline void set_thread_name(const char* name) { pthread_setname_np(pthread_self(), name); }

}  // namespace fp
