/* ==========================================================================
 *  PROC -- process control blocks, scheduler, and the syscall dispatcher
 *
 *  Up to 64 PCBs live in the kernel structure region of physical RAM.  The
 *  scheduler is a real round-robin with priority weighting, driven from the
 *  IRQ0 tick: every tick it charges the running process, every N ticks it
 *  preempts, and once per second it recomputes cpu_percent for the whole
 *  table.  Kernel daemons (init, kthreadd, compositor, sysmon) are PCBs too.
 * ========================================================================== */

enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_BLOCKED,
    PROC_ZOMBIE
};

#define MAX_PROCS     64
#define MAX_IPC_Q     32
#define PROC_NAME_MAX 32

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi, esp, ebp, eip, eflags;
    u32 cr3;
    u32 step;
    u32 stack_top;
} proc_context_t;

typedef struct {
    u32            used;
    u32            pid;
    u32            ppid;
    char           name[PROC_NAME_MAX];
    u32            state;
    u32            priority;        /* 0..31, higher runs more often       */
    u32            base_priority;
    u32            cpu_ticks;
    u32            last_ticks;
    u32            cpu_percent;
    u32            mem_used;
    u32            window_id;
    u32            entry_tick;
    u32            sleep_until;
    u32            quantum;
    u32            quantum_left;
    u32            work_budget;     /* ticks of work left after an event   */
    u32            duty_ticks;      /* ticks of work per activation        */
    u32            duty_run;        /* ticks used in the current activation */
    u32            rest_ticks;      /* ticks slept between activations      */
    u32            exit_code;
    u32            vm_entry;        /* bytecode VM entry point             */
    u32            vm_ip;
    u32            vm_sp;
    proc_context_t ctx;
    u32            flags;           /* PROC_FLAG_*                         */
    u32            ipc_sent;
    u32            ipc_recv;
    int            ipc_in;          /* ring of peer pids waiting on us     */
    int            ipc_q[MAX_IPC_Q];
    int            ipc_head, ipc_count;
    char           cwd[128];
} pcb_t;

enum { PROC_FLAG_KERNEL = 1, PROC_FLAG_APP = 2, PROC_FLAG_GUI = 4, PROC_FLAG_VM = 8,
       PROC_FLAG_IDLE = 16, PROC_FLAG_DAEMON = 32 };

typedef struct {
    pcb_t pcb[MAX_PROCS];
    u32   next_pid;
    u32   running_pid;
    u32   tick_count;
    u32   hw_tick_seen;         /* last IRQ0 tick folded into the scheduler  */
    u32   acct_interval;        /* ticks between CPU% samples                */
    u32   switch_count;
    u32   last_cpu_total;
    u32   total_switches_this_sec;
    u32   context_saves;
    u32   spawns;
    u32   kills;
    u32   zombie_reaped;
    u32   uptime_ticks;
    u32   idle_ticks;
    u32   sched_integrity_errors;
    int   initialised;
    u32   syscall_counts[32];
    u32   syscall_total;
} sched_t;

static sched_t g_sched;

pcb_t *proc_pcb(u32 pid) {
    if (pid >= MAX_PROCS) return NULL;
    if (!g_sched.pcb[pid].used) return NULL;
    return &g_sched.pcb[pid];
}

sched_t *proc_sched(void) { return &g_sched; }

const char *proc_state_name(u32 state) {
    switch (state) {
    case PROC_READY:    return "READY";
    case PROC_RUNNING:  return "RUNNING";
    case PROC_SLEEPING: return "SLEEPING";
    case PROC_BLOCKED:  return "BLOCKED";
    case PROC_ZOMBIE:   return "ZOMBIE";
    default:            return "UNUSED";
    }
}

void proc_init(void) {
    kmemset(&g_sched, 0, sizeof(g_sched));
    g_sched.next_pid = 1;
    g_sched.initialised = 1;
    g_sched.acct_interval = 100;   /* CPU% sampled every 100 ms */
    g_sched.last_cpu_total = irq_ticks();
    klog("proc: PCB table at 0x%08X, %u slots, quantum-based round robin",
         (unsigned)(uintptr_t)g_sched.pcb, (unsigned)MAX_PROCS);
}

static void proc_seed_context(pcb_t *p) {
    u32 base = HEAP_BASE + 0x1000 + p->pid * 0x2000;
    kmemset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.eip = p->vm_entry;
    p->ctx.esp = base + 0x1800;
    p->ctx.ebp = p->ctx.esp;
    p->ctx.eflags = 0x202;
    p->ctx.eax = p->pid;
    p->ctx.cr3 = 0x40000 + p->pid * 0x1000;
    p->ctx.stack_top = p->ctx.esp;
}

u32 proc_spawn(const char *name, u32 ppid, u32 priority, u32 flags, u32 vm_entry) {
    u32 pid;
    pcb_t *p;
    for (pid = 1; pid < MAX_PROCS; pid++)
        if (!g_sched.pcb[pid].used) break;
    if (pid >= MAX_PROCS) return 0;
    p = &g_sched.pcb[pid];
    kmemset(p, 0, sizeof(*p));
    p->used = 1;
    p->pid = pid;
    p->ppid = ppid;
    kstrncpy(p->name, name ? name : "process", PROC_NAME_MAX - 1);
    p->state = PROC_READY;
    p->priority = priority;
    p->base_priority = priority;
    p->entry_tick = irq_ticks();
    p->window_id = 0;
    p->vm_entry = vm_entry;
    p->quantum = 4 + priority / 4;
    p->quantum_left = p->quantum;
    p->flags = flags;
    p->exit_code = -1;
    proc_seed_context(p);
    kstrcpy(p->cwd, vfs_cwd());
    g_sched.spawns++;
    if (pid >= g_sched.next_pid) g_sched.next_pid = pid + 1;
    return pid;
}

int proc_kill(u32 pid, u32 code) {
    pcb_t *p = proc_pcb(pid);
    if (!p) return -1;
    if (pid == 1) return -2;                      /* init is protected       */
    if (p->flags & PROC_FLAG_KERNEL && !(p->flags & PROC_FLAG_APP)) return -3;
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    g_sched.kills++;
    klog("proc: signal SIGKILL delivered to pid %u (%s)", (unsigned)pid, p->name);
    return 0;
}

void proc_exit(u32 pid, u32 code) {
    pcb_t *p = proc_pcb(pid);
    if (!p) return;
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
}

static void proc_reap(u32 pid) {
    pcb_t *p = proc_pcb(pid);
    if (!p) return;
    mm_set_current_pid(pid);
    mm_release_owner(pid);
    mm_set_current_pid(0);
    p->mem_used = 0;
    p->work_budget = 8;     /* one slice of startup work */
    kmemset(p, 0, sizeof(*p));
    g_sched.zombie_reaped++;
}

u32 proc_sleep_ms(u32 pid, u32 ms) {
    pcb_t *p = proc_pcb(pid);
    if (!p) return 0;
    p->state = PROC_SLEEPING;
    p->sleep_until = irq_ticks() + (ms * irq_hz()) / 1000;
    return p->sleep_until;
}

/* Input-driven scheduling: handing a process a slice of work time also makes
   it runnable again, which is how blocked GUI apps get serviced. */
void proc_wake_ticks(u32 pid, u32 ticks) {
    pcb_t *p = proc_pcb(pid);
    if (!p) return;
    if (p->state == PROC_ZOMBIE || p->state == PROC_UNUSED) return;
    if (p->state == PROC_SLEEPING) return;
    p->work_budget += ticks;
    if (p->work_budget > 64) p->work_budget = 64;
    if (p->state == PROC_BLOCKED) p->state = PROC_READY;
}

void proc_wake(u32 pid) {
    pcb_t *p = proc_pcb(pid);
    if (p && p->state == PROC_SLEEPING) p->state = PROC_READY;
}

u32 proc_count_state(u32 state) {
    u32 i, n = 0;
    for (i = 1; i < MAX_PROCS; i++)
        if (g_sched.pcb[i].used && g_sched.pcb[i].state == state) n++;
    return n;
}

u32 proc_alive(void) {
    u32 i, n = 0;
    for (i = 1; i < MAX_PROCS; i++)
        if (g_sched.pcb[i].used && g_sched.pcb[i].state != PROC_ZOMBIE) n++;
    return n;
}

/* -------------------------------------------------------------------------- */
/*  scheduler: called from the IRQ0 handler                                 */
/* -------------------------------------------------------------------------- */

static void sched_choose(void) {
    u32 start = g_sched.running_pid ? g_sched.running_pid : 1;
    u32 i;
    u32 best = 0;
    u32 best_score = 0;
    u32 now = irq_ticks();

    /* wake sleepers */
    for (i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = &g_sched.pcb[i];
        if (!p->used) continue;
        if (p->state == PROC_SLEEPING && (i32)(now - p->sleep_until) >= 0) {
            p->state = PROC_READY;
            klog("proc: pid %u (%s) woke from sleep", (unsigned)i, p->name);
        }
        if (p->state == PROC_ZOMBIE) {
            proc_reap(i);
            if (g_sched.running_pid == i) g_sched.running_pid = 0;
        }
    }

    /* Priority-weighted round robin: walk the table starting after the
     * current process and take the highest-priority runnable candidate.  Ties
     * are broken in scan order, which gives every equally-ranked process a
     * turn.  A running process that still has quantum left and no strictly
     * better competitor keeps the CPU, so short bursts are not preempted. */
    for (i = 1; i < MAX_PROCS; i++) {
        u32 pid = ((start - 1 + i) % (MAX_PROCS - 1)) + 1;
        pcb_t *p = &g_sched.pcb[pid];
        u32 score;
        if (!p->used || p->state != PROC_READY) continue;
        score = 1u + p->priority * 4u;
        if (p->quantum_left > 0) score += 2u;
        if (score > best_score) {
            best_score = score;
            best = pid;
        }
    }
    if (best && g_sched.running_pid && best != g_sched.running_pid) {
        pcb_t *cur = proc_pcb(g_sched.running_pid);
        if (cur && cur->state == PROC_RUNNING && cur->quantum_left > 0) {
            pcb_t *cand = proc_pcb(best);
            if (cand && cand->priority <= cur->priority) best = g_sched.running_pid;
        }
    }
    if (!best) {
        /* nothing runnable: keep the idle process (pid 2 kthreadd) running */
        pcb_t *k = proc_pcb(2);
        if (k && k->state == PROC_READY) best = 2;
    }
    if (best != g_sched.running_pid) {
        if (g_sched.running_pid) {
            pcb_t *old = proc_pcb(g_sched.running_pid);
            if (old && old->state == PROC_RUNNING) {
                old->state = PROC_READY;
                old->quantum_left = old->quantum;
                old->ctx.eip += 4;              /* save program counter        */
                g_sched.context_saves++;
            }
        }
        g_sched.running_pid = best;
        g_sched.switch_count++;
        g_sched.total_switches_this_sec++;
        if (best) {
            pcb_t *np = proc_pcb(best);
            np->state = PROC_RUNNING;
        }
    }
}

void proc_tick_one(void);

/* Give a blocked process CPU time to service an event.  Called by the window
   manager whenever keyboard, mouse or timer input is delivered to a window. */
void proc_wake_ticks(u32 pid, u32 ticks);

/* Event-driven service process: `work` ticks of CPU then `rest` ticks asleep.
   Used for the kernel daemons so an idle machine reports an idle load. */
void proc_set_duty(u32 pid, u32 work, u32 rest) {
    pcb_t *p = proc_pcb(pid);
    if (!p) return;
    p->flags |= PROC_FLAG_DAEMON;
    p->duty_ticks = work;
    p->rest_ticks = rest;
    p->duty_run = 0;
}

void proc_tick(void) {
    if (!g_sched.initialised) return;
    /* The scheduler is driven by IRQ0: catch up with the hardware tick counter
       so accounting really is per timer tick, not per rendered frame. */
    {
        u32 hw = irq_ticks();
        u32 budget = 64;
        while (g_sched.hw_tick_seen != hw && budget--) {
            g_sched.hw_tick_seen++;
            proc_tick_one();
        }
        if (budget == 0) g_sched.hw_tick_seen = hw;
    }
}

void proc_tick_one(void) {
    pcb_t *p;
    u32 pid;
    if (!g_sched.initialised) return;
    g_sched.tick_count++;
    g_sched.uptime_ticks++;
    /* Reschedule every tick when the running process is blocked or has spent
       its quantum; otherwise keep the current owner for up to 4 ticks. */
    if (!g_sched.running_pid || (g_sched.tick_count & 3) == 0) sched_choose();
    pid = g_sched.running_pid;
    p = proc_pcb(pid);
    if (p && p->state == PROC_RUNNING && (p->flags & PROC_FLAG_IDLE)) {
        /* the idle loop spinning is not CPU work: account it as idle time */
        g_sched.idle_ticks++;
    } else if (p && p->state == PROC_RUNNING) {
        p->cpu_ticks++;
        if (p->quantum_left) p->quantum_left--;
        if (p->flags & PROC_FLAG_VM) p->ctx.eip += 1;
        /* Event driven applications block on their input queue: they only own
           the CPU while there is work to do, exactly as a real GUI process
           waiting in a message loop does. */
        if (p->flags & (PROC_FLAG_GUI | PROC_FLAG_APP)) {
            if (p->work_budget) {
                p->work_budget--;
            } else if (p->state == PROC_RUNNING) {
                p->state = PROC_BLOCKED;
                g_sched.running_pid = 0;
                g_sched.idle_ticks++;
            }
        }
        /* Event driven daemons alternate work and rest so the machine has a
           realistic idle load and sleeping processes stay off the run queue. */
        if ((p->flags & PROC_FLAG_DAEMON) && p->duty_ticks) {
            if (++p->duty_run >= p->duty_ticks) {
                p->duty_run = 0;
                p->state = PROC_SLEEPING;
                p->sleep_until = irq_ticks() + p->rest_ticks;
                g_sched.running_pid = 0;
            }
        }
    } else {
        g_sched.idle_ticks++;
    }
    /* CPU accounting, once every 100 ms: percentage over the measured window
       rather than over one second, so the first second after boot already
       reports sane numbers. */
    if (g_sched.tick_count % g_sched.acct_interval == 0) {
        u32 i;
        u32 total_ticks = 0;
        u32 max_prio = 0;
        u32 window = g_sched.acct_interval;
        for (i = 1; i < MAX_PROCS; i++) {
            p = &g_sched.pcb[i];
            if (!p->used) continue;
            {
                u32 delta = p->cpu_ticks - p->last_ticks;
                p->last_ticks = p->cpu_ticks;
                total_ticks += delta;
                p->cpu_percent = (delta * 100u) / (window ? window : 1u);
                if (p->cpu_percent > 100) p->cpu_percent = 100;
                p->mem_used = mm_bytes_owned_by(i);
                /* dynamic priority ageing: starved processes slowly promote */
                if (p->cpu_percent == 0 && p->state == PROC_READY) {
                    if (p->priority < 31) p->priority++;
                } else if (p->cpu_percent > 60 && p->priority > p->base_priority) {
                    p->priority--;
                }
                if (p->priority > max_prio) max_prio = p->priority;
            }
        }
        g_sched.last_cpu_total = total_ticks;
        g_sched.total_switches_this_sec = 0;
        (void)max_prio;
    }
}

u32 proc_cpu_total_percent(void) {
    u32 i, sum = 0;
    for (i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = &g_sched.pcb[i];
        if (p->used && p->state != PROC_ZOMBIE) sum += p->cpu_percent;
    }
    return sum > 100 ? 100 : sum;
}

u32 proc_cpu_busy_ticks(void) {
    u32 i, sum = 0;
    for (i = 1; i < MAX_PROCS; i++)
        if (g_sched.pcb[i].used) sum += g_sched.pcb[i].cpu_ticks;
    return sum;
}

u32 proc_running_pid(void) { return g_sched.running_pid; }
u32 proc_next_free_slot(void) {
    u32 i;
    for (i = 1; i < MAX_PROCS; i++) if (!g_sched.pcb[i].used) return i;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  IPC                                                                      */
/* -------------------------------------------------------------------------- */

int ipc_send(u32 to, u32 from) {
    pcb_t *d = proc_pcb(to);
    if (!d) return -1;
    if (d->ipc_count >= MAX_IPC_Q) return -2;
    d->ipc_q[(d->ipc_head + d->ipc_count) % MAX_IPC_Q] = (int)from;
    d->ipc_count++;
    {
        pcb_t *s = proc_pcb(from);
        if (s) s->ipc_sent++;
    }
    d->state = (d->state == PROC_SLEEPING) ? PROC_READY : d->state;
    return 0;
}

int ipc_recv(u32 pid, u32 *out_from) {
    pcb_t *p = proc_pcb(pid);
    if (!p || p->ipc_count == 0) return -1;
    if (out_from) *out_from = (u32)p->ipc_q[p->ipc_head];
    p->ipc_head = (p->ipc_head + 1) % MAX_IPC_Q;
    p->ipc_count--;
    p->ipc_recv++;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  syscalls                                                                 */
/* -------------------------------------------------------------------------- */

enum {
    SYS_EXIT = 0, SYS_SPAWN, SYS_KILL, SYS_GETPID, SYS_SLEEP,
    SYS_MALLOC, SYS_FREE, SYS_OPEN, SYS_READ, SYS_WRITE, SYS_CLOSE,
    SYS_MKDIR, SYS_UNLINK, SYS_STAT, SYS_IPC_SEND, SYS_IPC_RECV,
    SYS_COUNT
};

static const char *syscall_names[SYS_COUNT] = {
    "exit", "spawn", "kill", "getpid", "sleep",
    "malloc", "free", "open", "read", "write", "close",
    "mkdir", "unlink", "stat", "ipc_send", "ipc_recv"
};

typedef struct {
    long long ret;
    u32       num;
    u32       pid;
    u64       tick_us;
} syscall_trace_t;

static syscall_trace_t g_last_syscall;

const char *syscall_name(int num) {
    if (num < 0 || num >= SYS_COUNT) return "unknown";
    return syscall_names[num];
}

syscall_trace_t *syscall_last(void) { return &g_last_syscall; }

/* ABI: pointers travel as 32-bit offsets into the emulated physical RAM, so
 * user processes can never hand the kernel a wild pointer. */
static void *sys_ptr(int64_t v) {
    u32 off = (u32)v;
    if (v == 0) return NULL;
    if (off >= PHYS_RAM_SIZE) return NULL;
    return g_phys_ram + off;
}

static int64_t sys_mem_offset(const void *p) {
    const u8 *b = (const u8 *)p;
    if (!b) return 0;
    return (int64_t)(b - g_phys_ram);
}

int64_t syscall(int num, int64_t a1, int64_t a2, int64_t a3) {
    u32 pid = g_sched.running_pid ? g_sched.running_pid : 1;
    int64_t ret = -1;
    u64 t0 = now_us();
    u32 prev_pid = mm_current_pid();

    if (num >= 0 && num < SYS_COUNT) g_sched.syscall_counts[num]++;
    g_sched.syscall_total++;
    mm_set_current_pid(pid);

    switch (num) {
    case SYS_EXIT:
        proc_exit(pid, (u32)a1);
        ret = 0;
        break;
    case SYS_SPAWN: {
        const char *name = (const char *)sys_ptr(a1);
        ret = (int64_t)proc_spawn(name ? name : "child", pid, (u32)a2,
                                  PROC_FLAG_APP, (u32)a3);
        break;
    }
    case SYS_KILL:
        ret = (int64_t)proc_kill((u32)a1, (u32)a2);
        break;
    case SYS_GETPID:
        ret = (int64_t)pid;
        break;
    case SYS_SLEEP:
        proc_sleep_ms(pid, (u32)a1);
        ret = 0;
        break;
    case SYS_MALLOC: {
        void *p = mm_kmalloc((u32)a1);
        ret = p ? sys_mem_offset(p) : 0;
        break;
    }
    case SYS_FREE:
        ret = (int64_t)mm_kfree(sys_ptr(a1));
        break;
    case SYS_OPEN: {
        const char *path = (const char *)sys_ptr(a1);
        ret = path ? (int64_t)vfs_open(path, (int)a2) : -1;
        break;
    }
    case SYS_READ: {
        void *buf = sys_ptr(a2);
        ret = buf ? (int64_t)vfs_read((int)a1, buf, (u32)a3) : -1;
        break;
    }
    case SYS_WRITE: {
        const void *buf = sys_ptr(a2);
        ret = buf ? (int64_t)vfs_write((int)a1, buf, (u32)a3) : -1;
        break;
    }
    case SYS_CLOSE:
        ret = (int64_t)vfs_close((int)a1);
        break;
    case SYS_MKDIR: {
        const char *path = (const char *)sys_ptr(a1);
        ret = path ? (int64_t)vfs_mkdir_path(path) : -1;
        break;
    }
    case SYS_UNLINK: {
        const char *path = (const char *)sys_ptr(a1);
        ret = path ? (int64_t)vfs_unlink(path) : -1;
        break;
    }
    case SYS_STAT: {
        const char *path = (const char *)sys_ptr(a1);
        vfs_inode_t *st = (vfs_inode_t *)sys_ptr(a2);
        ret = (path && st) ? (int64_t)vfs_stat_path(path, st) : -1;
        break;
    }
    case SYS_IPC_SEND:
        ret = (int64_t)ipc_send((u32)a1, pid);
        break;
    case SYS_IPC_RECV: {
        u32 from = 0;
        ret = (ipc_recv(pid, &from) == 0) ? (int64_t)from : -1;
        break;
    }
    default:
        klog("syscall: invalid vector %d from pid %u", num, (unsigned)pid);
        ret = -38;      /* -ENOSYS */
        break;
    }
    mm_set_current_pid(prev_pid);
    g_last_syscall.ret = ret;
    g_last_syscall.num = (u32)(num < 0 ? 0 : num);
    g_last_syscall.pid = pid;
    g_last_syscall.tick_us = now_us() - t0;
    return ret;
}

/* -------------------------------------------------------------------------- */
/*  kernel daemon bootstrap                                                  */
/* -------------------------------------------------------------------------- */

u32 proc_boot_daemons(void) {
    u32 init_pid, kthr;
    init_pid = proc_spawn("init", 0, 31, PROC_FLAG_KERNEL | PROC_FLAG_DAEMON, 0);
    kthr = proc_spawn("kthreadd", 1, 8, PROC_FLAG_KERNEL | PROC_FLAG_IDLE, 0);
    /* Give the daemons a duty cycle: 3 ticks of work in every 100. */
    {
        pcb_t *ip = proc_pcb(init_pid);
        if (ip) { ip->duty_ticks = 3; ip->rest_ticks = 97; }
    }
    (void)kthr;
    klog("proc: spawned init (pid %u) as the first user-space process",
         (unsigned)init_pid);
    klog("proc: spawned kthreadd (pid %u) as the idle kernel thread", (unsigned)kthr);
    return init_pid;
}

/* Detailed integrity check exposed via :procverify */
int proc_verify(char *why, size_t whyn) {
    u32 i;
    int errors = 0;
    u32 running = 0;
    for (i = 1; i < MAX_PROCS; i++) {
        pcb_t *p = &g_sched.pcb[i];
        if (!p->used) continue;
        if (p->pid != i) {
            errors++;
            ksnprintf(why, whyn, "pcb[%u].pid=%u mismatch", (unsigned)i, (unsigned)p->pid);
            break;
        }
        if (p->state == PROC_RUNNING) running++;
        if (p->state == PROC_UNUSED && p->used) {
            errors++;
            ksnprintf(why, whyn, "pid %u used but state UNUSED", (unsigned)i);
            break;
        }
        if (p->ppid && !proc_pcb(p->ppid)) {
            errors++;
            ksnprintf(why, whyn, "pid %u orphan (ppid %u gone)", (unsigned)i, (unsigned)p->ppid);
            break;
        }
    }
    if (running > 1) {
        errors++;
        ksnprintf(why, whyn, "%u processes simultaneously RUNNING", (unsigned)running);
    }
    if (!errors && why) ksnprintf(why, whyn, "ok");
    g_sched.sched_integrity_errors = (u32)errors;
    return errors;
}
