/* ==========================================================================
 *  MM -- physical memory manager
 *
 *  A boundary-tag allocator that lives entirely inside the emulated physical
 *  RAM heap window (0x00100000 .. 0x01FFFFFF).  Blocks carry both a header and
 *  a footer so that freeing can coalesce *backwards* in O(1); forward
 *  coalescing happens on the free list.  Every block records its owning PID,
 *  its allocation site and a 0xDEADBEEF magic that is verified on free, on
 *  realloc and during the integrity sweep (:memverify in the shell).
 * ========================================================================== */

#define MM_MAGIC_ALLOC 0xDEADBEEFu
#define MM_MAGIC_FREE  0xF4EEF4EEu
#define MM_MAGIC_FOOT  0xB16B00B5u
#define MM_ALIGN       16
#define MM_MIN_SPLIT   48

typedef struct mm_block {
    u32  magic;                 /* ALLOC / FREE                              */
    u32  size;                  /* payload bytes, 16-byte aligned            */
    u32  pid;                   /* owning process (0 = kernel)               */
    u32  flags;                 /* MM_FLAG_*                                 */
    struct mm_block *prev;
    struct mm_block *next;      /* physical neighbour (not free list)        */
    u32  checksum;              /* header integrity                          */
    u32  tag;                   /* static string id for leaks/spray reports  */
    u32  seq;                   /* allocation sequence number                */
} mm_block_t;

typedef struct {
    u32 size;                   /* payload size                              */
    u32 magic;                  /* footer magic                              */
} mm_foot_t;

#define MM_FOOT(b) ((mm_foot_t *)((u8 *)(b) + sizeof(mm_block_t) + (b)->size))

enum { MM_FLAG_FREE = 1, MM_FLAG_KERNEL = 2, MM_FLAG_GUARDED = 4 };

typedef struct {
    mm_block_t *head;           /* first block in the heap                   */
    mm_block_t *tail;
    mm_block_t *free_list;      /* singly linked through next_free           */
    u32  total;                 /* heap bytes available to the allocator     */
    u32  heap_bytes;            /* whole heap window (headers included)      */
    u32  used;                  /* bytes handed out (payload)                */
    u32  used_overhead;         /* headers+footers+slack                     */
    u32  blocks_alloc;
    u32  blocks_free;
    u32  alloc_calls;
    u32  free_calls;
    u32  fail_calls;
    u32  seq;
    u32  peak_used;
    u32  guard_hits;
    u32  free_fragmentation;    /* percent of free space in blocks < 512 B   */
    u32  scan_hits;             /* times a scan had to skip a corrupt block  */
    int  initialised;
} mm_state_t;

/* public interface */
void  mm_init(void);
void *mm_kmalloc(u32 size);
void *mm_kcalloc(u32 n, u32 size);
void *mm_krealloc(void *ptr, u32 size);
int   mm_kfree(void *ptr);
void *mm_alloc_tagged(u32 size, u32 tag);
void  mm_set_current_pid(u32 pid);
u32   mm_current_pid(void);
u32   mm_bytes_owned_by(u32 pid);
u32   mm_block_count_for_pid(u32 pid);
u32   mm_release_owner(u32 pid);

static mm_state_t g_mm;
static u32 g_mm_current_pid;

/* next_free lives in the block's flags-overlapped space: we keep a separate
 * pointer in the footer area is not possible, so free blocks chain through
 * their payload's first bytes. */
static mm_block_t *mm_next_free(mm_block_t *b) {
    mm_block_t *n;
    kmemcpy(&n, (u8 *)b + sizeof(mm_block_t), sizeof(n));
    return n;
}
static void mm_set_next_free(mm_block_t *b, mm_block_t *n) {
    kmemcpy((u8 *)b + sizeof(mm_block_t), &n, sizeof(n));
}

static u32 mm_checksum(const mm_block_t *b) {
    return b->size ^ b->magic ^ b->pid ^ (u32)(uintptr_t)b->next ^ (u32)(uintptr_t)b->prev;
}

static void mm_write_tags(mm_block_t *b, u32 size, u32 magic) {
    b->magic = magic;
    b->size = size;
    b->checksum = mm_checksum(b);
    {
        mm_foot_t *f = MM_FOOT(b);
        f->size = size;
        f->magic = MM_MAGIC_FOOT;
    }
}

void mm_set_current_pid(u32 pid) { g_mm_current_pid = pid; }
u32  mm_current_pid(void) { return g_mm_current_pid; }
mm_state_t *mm_state(void) { return &g_mm; }

void mm_init(void) {
    u8 *base = g_phys_ram + HEAP_BASE;
    mm_block_t *b = (mm_block_t *)base;
    kmemset(&g_mm, 0, sizeof(g_mm));
    kmemset(base, 0, HEAP_SIZE);
    b->pid = 0;
    b->flags = MM_FLAG_FREE | MM_FLAG_KERNEL;
    b->prev = NULL;
    b->next = NULL;
    b->tag = 0;
    b->seq = 0;
    mm_write_tags(b, HEAP_SIZE - sizeof(mm_block_t) - sizeof(mm_foot_t), MM_MAGIC_FREE);
    b->checksum = mm_checksum(b);
    g_mm.head = g_mm.tail = b;
    g_mm.free_list = b;
    mm_set_next_free(b, NULL);
    g_mm.total = b->size;
    g_mm.heap_bytes = HEAP_SIZE;
    g_mm.blocks_free = 1;
    g_mm.initialised = 1;
    klog("mm: heap window 0x%08X-0x%08X (%u KiB) initialised",
         (unsigned)(HEAP_BASE), (unsigned)(HEAP_BASE + HEAP_SIZE),
         (unsigned)(HEAP_SIZE / 1024));
    klog("mm: boundary-tag allocator online, 1 free block of %u KiB",
         (unsigned)(g_mm.total / 1024));
}

static void mm_recompute(void) {
    mm_block_t *b;
    u32 used = 0, overhead = 0, blocks_alloc = 0, blocks_free = 0;
    u32 small_free = 0, total_free = 0;
    for (b = g_mm.head; b; b = b->next) {
        if (b->flags & MM_FLAG_FREE) {
            blocks_free++;
            overhead += sizeof(mm_block_t) + sizeof(mm_foot_t);
            total_free += b->size;
            if (b->size < 512) small_free += b->size;
        } else {
            used += b->size;
            overhead += sizeof(mm_block_t) + sizeof(mm_foot_t);
            blocks_alloc++;
        }
    }
    g_mm.used = used;
    g_mm.used_overhead = overhead;
    g_mm.blocks_alloc = blocks_alloc;
    g_mm.blocks_free = blocks_free;
    g_mm.free_fragmentation = total_free ? (u32)((u64)small_free * 100u / total_free) : 0;
    if (used > g_mm.peak_used) g_mm.peak_used = used;
}

/* Splits b so that the first part has exactly `want` payload bytes.  The
 * remainder becomes a free block and is inserted after b. */
static void mm_split(mm_block_t *b, u32 want) {
    u32 remain;
    mm_block_t *nb;
    if (b->size < want + sizeof(mm_block_t) + sizeof(mm_foot_t) + MM_MIN_SPLIT) return;
    remain = b->size - want - sizeof(mm_block_t) - sizeof(mm_foot_t);
    nb = (mm_block_t *)((u8 *)b + sizeof(mm_block_t) + want + sizeof(mm_foot_t));
    nb->pid = 0;
    nb->flags = MM_FLAG_FREE;
    nb->prev = b;
    nb->next = b->next;
    nb->tag = 0;
    nb->seq = 0;
    if (b->next) b->next->prev = nb;
    else g_mm.tail = nb;
    b->next = nb;
    mm_write_tags(nb, remain, MM_MAGIC_FREE);
    /* b shrinks */
    mm_write_tags(b, want, MM_MAGIC_ALLOC);
    /* push nb to the free list head */
    mm_set_next_free(nb, g_mm.free_list);
    g_mm.free_list = nb;
}

static void mm_free_list_remove(mm_block_t *victim) {
    mm_block_t *prev = NULL, *cur = g_mm.free_list;
    while (cur) {
        if (cur == victim) {
            if (prev) mm_set_next_free(prev, mm_next_free(cur));
            else g_mm.free_list = mm_next_free(cur);
            mm_set_next_free(cur, NULL);
            return;
        }
        prev = cur;
        cur = mm_next_free(cur);
    }
}

void *mm_alloc_tagged(u32 size, u32 tag) {
    mm_block_t *b;
    u32 want;
    if (!g_mm.initialised) return NULL;
    if (size == 0) size = 1;
    want = (size + MM_ALIGN - 1) & ~(u32)(MM_ALIGN - 1);
    for (b = g_mm.free_list; b; b = mm_next_free(b)) {
#ifdef AETHER_MM_DEBUG
        if ((u8 *)b < (u8 *)g_mm.head || (u8 *)b >= (u8 *)g_mm.head + HEAP_SIZE) {
            fprintf(stderr, "[mm] CORRUPT free_list node %p outside heap, want=%u\n",
                    (void *)b, (unsigned)want);
            abort();
        }
#endif
        if (b->magic != MM_MAGIC_FREE) { g_mm.scan_hits++; continue; }
        if (b->size >= want) break;
    }
    if (!b) {
        g_mm.fail_calls++;
        return NULL;
    }
    mm_free_list_remove(b);
    mm_split(b, want);
    b->flags = MM_FLAG_KERNEL;
    b->pid = g_mm_current_pid;
    b->tag = tag;
    b->seq = ++g_mm.seq;
    mm_write_tags(b, b->size, MM_MAGIC_ALLOC);
    g_mm.alloc_calls++;
    mm_recompute();
    return (u8 *)b + sizeof(mm_block_t);
}

/* --------------------------------------------------------------------------
 *  Heap integrity check
 *
 *  mm_kfree() has to keep the block chain and the free list consistent with each
 *  other: a block that is merged away must also leave the free list, otherwise
 *  the list ends up holding pointers to the interior of another block and a
 *  later allocation can overlap live data.  That failure mode is invisible until
 *  something crashes far away, so the allocator verifies both structures on
 *  every allocation when AETHER_MM_CHECK is defined.
 *
 *      tools/debug_build.sh -DAETHER_MM_CHECK
 * -------------------------------------------------------------------------- */
#ifdef AETHER_MM_CHECK
static void mm_panic_freeze(void);

void mm_check(const char *where) {
    mm_block_t *b = g_mm.head;
    int n = 0;
    {
        mm_block_t *q = g_mm.free_list;
        int k = 0;
        while (q) {
            if ((u8 *)q < (u8 *)g_mm.head || (u8 *)q >= (u8 *)g_mm.head + HEAP_SIZE ||
                ((size_t)q & 7) || q->magic != MM_MAGIC_FREE ||
                !(q->flags & MM_FLAG_FREE)) {
                klog("mm: %s: bad free-list node %d at %p (magic %08x flags %x)",
                     where, k, (void *)q, (unsigned)q->magic, (unsigned)q->flags);
                mm_panic_freeze();
            }
            if (++k > 4096) {
                klog("mm: %s: free list does not terminate", where);
                mm_panic_freeze();
            }
            q = mm_next_free(q);
        }
    }
    while (b) {
        int is_free = (b->flags & MM_FLAG_FREE) != 0;
        u32 want = is_free ? MM_MAGIC_FREE : MM_MAGIC_ALLOC;
        if (b->magic != want || MM_FOOT(b)->magic != MM_MAGIC_FOOT ||
            MM_FOOT(b)->size != b->size) {
            klog("mm: %s: block %d at %p corrupt (size %u flags %x magic %08x)",
                 where, n, (void *)b, (unsigned)b->size, (unsigned)b->flags,
                 (unsigned)b->magic);
            mm_panic_freeze();
        }
        if (++n > 4096) {
            klog("mm: %s: block chain does not terminate", where);
            mm_panic_freeze();
        }
        b = b->next;
    }
}
#else
void mm_check(const char *where) { (void)where; }
#endif

static void mm_panic_freeze(void) {
    /* A corrupted heap cannot be recovered, so take the machine down through
       the real panic screen; the reason is already in dmesg. */
    extern void kernel_panic(const char *reason, u32 eip, u32 pid);
    kernel_panic("heap integrity check failed", 0x0043A55E, mm_current_pid());
    for (;;) { }
}

void *mm_kmalloc(u32 size) {
    void *p;
#ifdef AETHER_MM_CHECK
    mm_check("alloc");
#endif
    p = mm_alloc_tagged(size, 0);
#ifdef AETHER_MM_CHECK
    if (!p) klog("mm: allocation of %u bytes failed", (unsigned)size);
#endif
    return p;
}

void *mm_kcalloc(u32 n, u32 size) {
    u32 total = n * size;
    void *p = mm_alloc_tagged(total, 0);
    if (p) kmemset(p, 0, total);
    return p;
}

void *mm_krealloc(void *ptr, u32 size) {
    mm_block_t *b;
    void *np;
    if (!ptr) return mm_kmalloc(size);
    b = (mm_block_t *)((u8 *)ptr - sizeof(mm_block_t));
    if (b->magic != MM_MAGIC_ALLOC) { return NULL; }
    if (b->size >= size) return ptr;
    np = mm_kmalloc(size);
    if (!np) return NULL;
    kmemcpy(np, ptr, b->size);
    mm_kfree(ptr);
    return np;
}

int mm_kfree(void *ptr) {
    mm_block_t *b, *n;
    if (!ptr) return 0;
    b = (mm_block_t *)((u8 *)ptr - sizeof(mm_block_t));
    if (b->magic != MM_MAGIC_ALLOC) {
        g_mm.guard_hits++;
        klog("mm: double free / corruption detected at %p (magic 0x%08X)",
             ptr, (unsigned)b->magic);
        return -1;
    }
    if (MM_FOOT(b)->magic != MM_MAGIC_FOOT || MM_FOOT(b)->size != b->size) {
        g_mm.guard_hits++;
        klog("mm: footer corruption at %p", ptr);
    }
    g_mm.free_calls++;
    b->flags = MM_FLAG_FREE;
    b->pid = 0;
    b->tag = 0;
    mm_write_tags(b, b->size, MM_MAGIC_FREE);
    /* Coalescing merges neighbouring blocks into one, so every block that is
       absorbed has to leave the free list first.  Leaving a merged-away block
       on the list made the list hold interior pointers (and eventually a
       cycle), after which an allocation could be handed a block that overlapped
       a live one -- the corruption that used to show up as a free-list node
       pointing into the string "AetherOS". */
    /* backward coalesce */
    if (b->prev && (b->prev->flags & MM_FLAG_FREE) && b->prev->magic == MM_MAGIC_FREE) {
        mm_block_t *p = b->prev;
        mm_free_list_remove(p);
        p->next = b->next;
        if (b->next) b->next->prev = p;
        else g_mm.tail = p;
        mm_write_tags(p, p->size + b->size + sizeof(mm_block_t) + sizeof(mm_foot_t),
                      MM_MAGIC_FREE);
        b = p;
    }
    /* forward coalesce */
    n = b->next;
    if (n && (n->flags & MM_FLAG_FREE) && n->magic == MM_MAGIC_FREE) {
        mm_free_list_remove(n);
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else g_mm.tail = b;
        mm_write_tags(b, b->size + n->size + sizeof(mm_block_t) + sizeof(mm_foot_t),
                      MM_MAGIC_FREE);
    }
    mm_set_next_free(b, g_mm.free_list);
    g_mm.free_list = b;
    mm_recompute();
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Integrity + telemetry                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    u32 blocks_total;
    u32 blocks_used;
    u32 blocks_free;
    u32 bytes_used;
    u32 bytes_free;
    u32 largest_free;
    u32 frag_pct;
    u32 errors;
} mm_report_t;

int mm_verify(mm_report_t *rep) {
    mm_block_t *b;
    u32 prev_end = 0;
    u32 errors = 0, largest = 0, free_bytes = 0;
    u32 used_blocks = 0, free_blocks = 0;
    for (b = g_mm.head; b; b = b->next) {
        u32 expect_link = prev_end;
        if (b->magic != MM_MAGIC_ALLOC && b->magic != MM_MAGIC_FREE) {
            errors++;
            break;
        }
        if (b->checksum != mm_checksum(b)) errors++;
        if (MM_FOOT(b)->magic != MM_MAGIC_FOOT) errors++;
        if (expect_link && (u32)(uintptr_t)b != expect_link) errors++;
        if (b->flags & MM_FLAG_FREE) { free_blocks++; free_bytes += b->size; if (b->size > largest) largest = b->size; }
        else used_blocks++;
        prev_end = (u32)(uintptr_t)((u8 *)b + sizeof(mm_block_t) + b->size + sizeof(mm_foot_t));
        if (!b->next) break;
    }
    if (rep) {
        rep->blocks_total = used_blocks + free_blocks;
        rep->blocks_used = used_blocks;
        rep->blocks_free = free_blocks;
        rep->bytes_used = g_mm.used;
        rep->bytes_free = free_bytes;
        rep->largest_free = largest;
        rep->frag_pct = g_mm.free_fragmentation;
        rep->errors = errors;
    }
    return errors == 0;
}

u32 mm_bytes_owned_by(u32 pid) {
    mm_block_t *b;
    u32 total = 0;
    for (b = g_mm.head; b; b = b->next) {
        if (!(b->flags & MM_FLAG_FREE) && b->pid == pid) total += b->size;
    }
    return total;
}

u32 mm_block_count_for_pid(u32 pid) {
    mm_block_t *b;
    u32 n = 0;
    for (b = g_mm.head; b; b = b->next)
        if (!(b->flags & MM_FLAG_FREE) && b->pid == pid) n++;
    return n;
}

/* Releases every block owned by a process (called by the scheduler on exit). */
u32 mm_release_owner(u32 pid) {
    mm_block_t *b;
    u32 freed = 0;
    for (b = g_mm.head; b; b = b->next) {
        if (!(b->flags & MM_FLAG_FREE) && b->pid == pid) {
            b->flags = MM_FLAG_FREE;
            b->pid = 0;
            b->tag = 0;
            mm_write_tags(b, b->size, MM_MAGIC_FREE);
            mm_set_next_free(b, g_mm.free_list);
            g_mm.free_list = b;
            freed++;
        }
    }
    if (freed) {
        /* one coalescing sweep merges everything we just released */
        mm_block_t *p = g_mm.head;
        while (p) {
            if ((p->flags & MM_FLAG_FREE) && p->next && (p->next->flags & MM_FLAG_FREE)) {
                mm_block_t *n = p->next;
                p->next = n->next;
                if (n->next) n->next->prev = p;
                else g_mm.tail = p;
                mm_write_tags(p, p->size + n->size + sizeof(mm_block_t) + sizeof(mm_foot_t),
                              MM_MAGIC_FREE);
                continue;
            }
            p = p->next;
        }
        mm_recompute();
    }
    return freed;
}

/* Iteration support for the visual heap inspector. */
int mm_block_at(u32 index, mm_block_t **out) {
    mm_block_t *b = g_mm.head;
    u32 i = 0;
    while (b) {
        if (i == index) { *out = b; return 1; }
        i++;
        b = b->next;
    }
    *out = NULL;
    return 0;
}

u32 mm_heap_base(void) { return HEAP_BASE; }
u32 mm_heap_size(void) { return HEAP_SIZE; }
u32 mm_used(void) { return g_mm.used; }
u32 mm_total(void) { return g_mm.total; }
/* Payload bytes handed out plus the header/footer overhead of every block:
   the honest "how much of the heap is committed" figure the tray and the
   system monitor display. */
u32 mm_committed(void) { return g_mm.used + g_mm.used_overhead; }
u32 mm_heap_pct(void) {
    u32 t = g_mm.used + g_mm.used_overhead;
    u32 h = g_mm.heap_bytes ? g_mm.heap_bytes : 1;
    return (u32)((u64)t * 100u / h);
}
u32 mm_free_bytes(void) { return g_mm.total > g_mm.used ? g_mm.total - g_mm.used : 0; }
