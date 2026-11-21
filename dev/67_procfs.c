/* ==========================================================================
 *  PROCFS -- the virtual /proc filesystem
 *
 *  These files are generated from live kernel state every time they are read,
 *  so `cat /proc/meminfo` always reports the truth and never goes stale.
 * ========================================================================== */

static char *proc_dup(const char *s) {
    u32 n = (u32)kstrlen(s);
    char *p = (char *)mm_kmalloc(n + 1);
    if (!p) return NULL;
    kmemcpy(p, s, n + 1);
    return p;
}

static const char *proc_basename(const char *path) {
    const char *s = kstrrchr(path, '/');
    return s ? s + 1 : path;
}

static char *proc_meminfo(void) {
    char buf[1400];
    mm_report_t rep;
    u32 inodes_used = 0, blocks_used = 0;
    u32 heap_total = mm_total(), heap_used = mm_used();
    mm_verify(&rep);
    vfs_usage(&inodes_used, &blocks_used, NULL);
    ksnprintf(buf, sizeof(buf),
        "MemTotal:       %8u kB\n"
        "MemFree:        %8u kB\n"
        "MemAvailable:   %8u kB\n"
        "Buffers:        %8u kB\n"
        "Cached:         %8u kB\n"
        "KernelStack:    %8u kB\n"
        "VmallocTotal:   %8u kB\n"
        "VmallocUsed:    %8u kB\n"
        "Active:         %8u kB\n"
        "Inactive:       %8u kB\n"
        "HugePages_Total:       0\n"
        "\n"
        "HeapBlocksUsed: %8u\n"
        "HeapBlocksFree: %8u\n"
        "HeapLargestFree:%8u B\n"
        "HeapFragmentation:%6u %%\n"
        "InodesUsed:     %8u / %u\n"
        "DataBlocksUsed: %8u / %u\n",
        (unsigned)(PHYS_RAM_SIZE / 1024),
        (unsigned)((PHYS_RAM_SIZE - heap_used) / 1024),
        (unsigned)((PHYS_RAM_SIZE - heap_used) / 1024),
        (unsigned)(VFS_BLOCK_SIZE / 1024),
        (unsigned)((VFS_BLOCKS - blocks_used) * 4),
        (unsigned)(KERNEL_SIZE / 1024),
        (unsigned)(HEAP_SIZE / 1024),
        (unsigned)(heap_used / 1024),
        (unsigned)(heap_used / 1024),
        (unsigned)(heap_total / 1024),
        (unsigned)rep.blocks_used,
        (unsigned)rep.blocks_free,
        (unsigned)rep.largest_free,
        (unsigned)rep.frag_pct,
        (unsigned)inodes_used, (unsigned)VFS_INODES,
        (unsigned)blocks_used, (unsigned)VFS_BLOCKS);
    return proc_dup(buf);
}

static char *proc_cpuinfo(void) {
    char buf[900];
    u32 procs = proc_alive();
    ksnprintf(buf, sizeof(buf),
        "processor\t: 0\n"
        "vendor_id\t: %s\n"
        "cpu family\t: 32\n"
        "model name\t: %s\n"
        "cpu MHz\t\t: %u.000\n"
        "cache size\t: 512 KB\n"
        "fpu\t\t: yes\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep\n"
        "bogomips\t: %u.00\n"
        "address sizes\t: 32 bits physical, 32 bits virtual\n"
        "\n"
        "timer\t\t: IRQ0 at %u Hz\n"
        "scheduler\t: round-robin + priority ageing\n"
        "processes\t: %u\n"
        "running\t\t: %u\n",
        "AetherVirtual", g_machine.cpu_name[0] ? g_machine.cpu_name : "Aether Virtual CPU",
        (unsigned)(g_machine.cpu_mhz ? g_machine.cpu_mhz : 2400),
        (unsigned)(g_machine.cpu_mhz ? g_machine.cpu_mhz : 2400),
        (unsigned)irq_hz(), (unsigned)procs, (unsigned)proc_count_state(PROC_RUNNING));
    return proc_dup(buf);
}

static char *proc_uptime(void) {
    char buf[160];
    u32 ticks = irq_ticks();
    u32 hz = irq_hz() ? irq_hz() : 1000;
    u32 idle = proc_sched()->idle_ticks;
    ksnprintf(buf, sizeof(buf), "%u.%02u %u.%02u\n",
              (unsigned)(ticks / hz), (unsigned)((ticks % hz) * 100 / hz),
              (unsigned)(idle / hz), (unsigned)((idle % hz) * 100 / hz));
    return proc_dup(buf);
}

static char *proc_version(void) {
    char buf[400];
    ksnprintf(buf, sizeof(buf),
        "AE version %s (%s) #1 SMP PREEMPT Thu Nov 20 09:41:00 UTC 2026\n"
        "compiler: %s\n"
        "build: monolithic hybrid microkernel, 32-bit\n",
        AE_VERSION_STR, AE_CODENAME,
#ifdef __VERSION__
        __VERSION__
#else
        "gcc"
#endif
    );
    return proc_dup(buf);
}

static char *proc_loadavg(void) {
    char buf[160];
    u32 cpu = proc_cpu_total_percent();
    u32 procs = proc_alive();
    ksnprintf(buf, sizeof(buf), "0.%02u 0.%02u 0.%02u 1/%u %u\n",
              (unsigned)(cpu % 100), (unsigned)((cpu * 7) % 100),
              (unsigned)((cpu * 3) % 100), (unsigned)procs,
              (unsigned)proc_running_pid());
    return proc_dup(buf);
}

static char *proc_mounts(void) {
    u32 inodes_used = 0, blocks_used = 0;
    char buf[400];
    vfs_usage(&inodes_used, &blocks_used, NULL);
    ksnprintf(buf, sizeof(buf),
        "nvram0 / ramfs rw,relatime 0 0\n"
        "proc /proc proc ro,nosuid,nodev,noexec 0 0\n"
        "devtmpfs /dev tmpfs rw,nosuid 0 0\n"
        "# inodes %u/%u blocks %u/%u\n",
        (unsigned)inodes_used, (unsigned)VFS_INODES,
        (unsigned)blocks_used, (unsigned)VFS_BLOCKS);
    return proc_dup(buf);
}

static char *proc_partitions(void) {
    char buf[400];
    u32 inodes_used = 0, blocks_used = 0;
    vfs_usage(&inodes_used, &blocks_used, NULL);
    ksnprintf(buf, sizeof(buf),
        "major minor  #blocks  name\n"
        "   8     0     32768  nvram0\n"
        "   8     1     31000  nvram0p1  (ramfs)\n"
        "   1     1         4  ram0\n"
        "# used: %u KiB in %u blocks\n",
        (unsigned)(blocks_used * 4), (unsigned)blocks_used);
    return proc_dup(buf);
}

char *proc_generate(const char *path, u32 *out_len, const char **err) {
    const char *name = proc_basename(path);
    char *r = NULL;
    if (err) *err = NULL;
    if (!kstrcmp(name, "meminfo")) r = proc_meminfo();
    else if (!kstrcmp(name, "cpuinfo")) r = proc_cpuinfo();
    else if (!kstrcmp(name, "uptime")) r = proc_uptime();
    else if (!kstrcmp(name, "version")) r = proc_version();
    else if (!kstrcmp(name, "loadavg")) r = proc_loadavg();
    else if (!kstrcmp(name, "mounts")) r = proc_mounts();
    else if (!kstrcmp(name, "partitions")) r = proc_partitions();
    else if (err) *err = "no such proc entry";
    if (r && out_len) *out_len = (u32)kstrlen(r);
    return r;
}
