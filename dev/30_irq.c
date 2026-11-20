/* ==========================================================================
 *  IRQ -- interrupt controller (PIC/IDT simulation)
 *
 *  The host's timer/keyboard/mouse hardware is translated into IRQ lines that
 *  the kernel services exactly the way a real x86 kernel would: the timer
 *  raises IRQ0 at a fixed frequency, the keyboard controller raises IRQ1 for
 *  every scancode and IRQ12 carries PS/2 mouse packets.  Devices enqueue
 *  their payload here; the scheduler/shell drain the queues.
 * ========================================================================== */

#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_MOUSE     12

#define IRQ_TIMER_HZ  1000                 /* 1 kHz monotonic tick            */
#define KBDQ_SIZE     256
#define MOUSEQ_SIZE   256

typedef struct {
    u32  vector;            /* 0x20 + IRQ                                  */
    u32  count;             /* interrupts taken                            */
    const char *name;
    const char *device;
} irq_desc_t;

typedef struct {
    u8  scancode;           /* raw set-1 style scancode                    */
    u8  ascii;              /* translated character (0 if non-printable)   */
    u8  mods;               /* MOD_* bitmask at the time of the event      */
    u8  pressed;            /* 1 = make, 0 = break                         */
    u32 tick;
} kbd_event_t;

typedef struct {
    i32 dx, dy;             /* delta since the previous packet             */
    i32 x, y;               /* absolute position in screen pixels          */
    i32 dz;                 /* wheel delta in notches                      */
    u8  buttons;            /* MOUSE_L / MOUSE_R / MOUSE_M                 */
    u32 tick;
} mouse_event_t;

enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_CAPS = 8, MOD_FN = 16 };
enum { MOUSE_L = 1, MOUSE_R = 2, MOUSE_M = 4 };

typedef struct {
    irq_desc_t   desc[3];
    u32          ticks;              /* IRQ0 count                            */
    u32          hz;
    u64          tick_epoch_us;
    u32          irq_nesting;

    kbd_event_t  kbdq[KBDQ_SIZE];
    volatile u32 kbd_head, kbd_tail;
    u8           kbd_mods;
    u8           kbd_last_char;
    u32          kbd_events;

    mouse_event_t mq[MOUSEQ_SIZE];
    volatile u32  m_head, m_tail;
    i32           mx, my;
    i32           mdx, mdy;
    i32           mdz;
    u8            mbuttons;
    u8            mbuttons_prev;
    u32           mouse_events;

    int           timer_enabled;
    int           irq_mask;         /* bitmask of masked IRQs                */
} irq_ctrl_t;

static irq_ctrl_t g_irq;

/* -------------------------------------------------------------------------- */
/*  scancode translation tables (set 1)                                      */
/* -------------------------------------------------------------------------- */

static const char kbd_map_lo[128] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,   0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,0,0,0,0,0,0,0,0,0, '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char kbd_map_hi[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,   0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,0,0,0,0,0,0,0,0,0, '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* Host virtual-key -> set-1 scancode is handled in irq_translate_key(). */

void irq_init(void) {
    kmemset(&g_irq, 0, sizeof(g_irq));
    g_irq.desc[0].vector = 0x20 + IRQ_TIMER;    g_irq.desc[0].name = "IRQ0";  g_irq.desc[0].device = "PIT 8253 timer";
    g_irq.desc[1].vector = 0x20 + IRQ_KEYBOARD; g_irq.desc[1].name = "IRQ1";  g_irq.desc[1].device = "i8042 keyboard";
    g_irq.desc[2].vector = 0x20 + IRQ_MOUSE;    g_irq.desc[2].name = "IRQ12"; g_irq.desc[2].device = "PS/2 auxiliary";
    g_irq.hz = IRQ_TIMER_HZ;
    g_irq.timer_enabled = 1;
    g_irq.tick_epoch_us = now_us();
    g_irq.mx = SCREEN_W / 2;
    g_irq.my = SCREEN_H / 2;
    klog("irq: PIC remapped to 0x20..0x2F, IDT vectors installed");
    klog("irq: IRQ0 timer armed at %u Hz", (unsigned)g_irq.hz);
    klog("irq: IRQ1 keyboard controller and IRQ12 PS/2 mouse online");
}

irq_ctrl_t *irq_state(void) { return &g_irq; }
u32 irq_ticks(void) { return g_irq.ticks; }
u32 irq_hz(void) { return g_irq.hz; }
void irq_mask(int irq) { g_irq.irq_mask |= (1 << irq); }
void irq_unmask(int irq) { g_irq.irq_mask &= ~(1 << irq); }
int irq_uptime_seconds(void) { return (int)(g_irq.ticks / (g_irq.hz ? g_irq.hz : 1000)); }

/* -------------------------------------------------------------------------- */
/*  IRQ0: timer                                                              */
/* -------------------------------------------------------------------------- */

/* Polls the host monotonic clock and issues as many IRQ0s as are due. Returns
 * the number of ticks generated (bounded so a stalled frame cannot explode). */
int irq_timer_poll(void) {
    u64 now = now_us();
    u64 elapsed = now - g_irq.tick_epoch_us;
    u64 due = (elapsed * (u64)g_irq.hz) / 1000000ULL;
    int produced = 0;
    if (!g_irq.timer_enabled) {
        g_irq.tick_epoch_us = now;
        return 0;
    }
    while (g_irq.ticks < due && produced < 32) {
        g_irq.ticks++;
        g_irq.desc[0].count++;
        produced++;
    }
    if (produced == 32) {
        /* host stall: resynchronise rather than fall behind forever */
        g_irq.tick_epoch_us = now;
        g_irq.ticks = (u32)due;
    }
    return produced;
}

/* -------------------------------------------------------------------------- */
/*  IRQ1: keyboard                                                           */
/* -------------------------------------------------------------------------- */

/* Maps a host virtual-key code to an i8042-style set-1 scancode. */
static u8 host_vk_to_scancode(int vk) {
    switch (vk) {
    case 0x08: return 0x0E; case 0x09: return 0x0F; case 0x0D: return 0x1C;
    case 0x10: return 0x2A; case 0x11: return 0x1D; case 0x12: return 0x38;
    case 0x13: return 0x3A; case 0x1B: return 0x01; case 0x20: return 0x39;
    case 0x25: return 0x4B; case 0x26: return 0x48; case 0x27: return 0x4D;
    case 0x28: return 0x50; case 0x2D: return 0x52; case 0x2E: return 0x53;
    case 0x24: return 0x47; case 0x23: return 0x4F; case 0x21: return 0x49;
    case 0x22: return 0x51; case 0x2C: return 0x37; case 0x91: return 0x46;
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z') {
        static const u8 letters[26] = {
            0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
            0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C
        };
        return letters[vk - 'A'];
    }
    if (vk >= '0' && vk <= '9') {
        static const u8 digits[10] = {0x0B,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A};
        return digits[vk - '0'];
    }
    if (vk >= 0x60 && vk <= 0x69) return (u8)(0x52 + (vk - 0x60));
    if (vk >= 0x70 && vk <= 0x7B) return (u8)(0x3B + (vk - 0x70));
    return 0;
}

static char translate_scancode(u8 sc, u8 mods) {
    int shifted = (mods & MOD_SHIFT) != 0;
    int caps = (mods & MOD_CAPS) != 0;
    char c = 0;
    if (sc < 0x80) c = shifted ? kbd_map_hi[sc] : kbd_map_lo[sc];
    if (c >= 'a' && c <= 'z') {
        if (caps ^ shifted) c = (char)(c - 32);
    } else if (c >= 'A' && c <= 'Z') {
        if (caps && !shifted) c = (char)(c + 32);
    }
    return c;
}

/* ascii_override: 0 = derive the glyph from the scancode and modifiers,
   IRQ_GLYPH_NONE = this transition carries no glyph at all, anything else = the
   glyph the host already resolved. */
#define IRQ_GLYPH_NONE (-1)

void irq_keyboard_push(u8 scancode, int pressed, int ascii_override) {
    kbd_event_t *e;
    u32 next;
    if (g_irq.irq_mask & (1 << IRQ_KEYBOARD)) return;
    /* modifier tracking */
    if (pressed) {
        switch (scancode) {
        case 0x2A: case 0x36: g_irq.kbd_mods |= MOD_SHIFT; break;
        case 0x1D: g_irq.kbd_mods |= MOD_CTRL; break;
        case 0x38: g_irq.kbd_mods |= MOD_ALT; break;
        case 0x3A: g_irq.kbd_mods ^= MOD_CAPS; break;
        default: break;
        }
    } else {
        switch (scancode) {
        case 0x2A: case 0x36: g_irq.kbd_mods &= (u8)~MOD_SHIFT; break;
        case 0x1D: g_irq.kbd_mods &= (u8)~MOD_CTRL; break;
        case 0x38: g_irq.kbd_mods &= (u8)~MOD_ALT; break;
        default: break;
        }
    }
    next = (g_irq.kbd_head + 1) % KBDQ_SIZE;
    if (next == g_irq.kbd_tail) return;      /* controller buffer full */
    e = &g_irq.kbdq[g_irq.kbd_head];
    e->scancode = scancode;
    e->pressed = (u8)(pressed ? 1 : 0);
    e->mods = g_irq.kbd_mods;
    if (ascii_override == IRQ_GLYPH_NONE) e->ascii = 0;
    else if (ascii_override) e->ascii = (u8)ascii_override;
    else e->ascii = (u8)(pressed ? translate_scancode(scancode, g_irq.kbd_mods) : 0);
    e->tick = g_irq.ticks;
    g_irq.kbd_head = next;
    g_irq.kbd_events++;
    g_irq.desc[1].count++;
}

int irq_keyboard_pop(kbd_event_t *out) {
    if (g_irq.kbd_tail == g_irq.kbd_head) return 0;
    *out = g_irq.kbdq[g_irq.kbd_tail];
    g_irq.kbd_tail = (g_irq.kbd_tail + 1) % KBDQ_SIZE;
    return 1;
}

u8 irq_kbd_mods(void) { return g_irq.kbd_mods; }
u32 irq_kbd_count(void) { return g_irq.kbd_events; }

/* -------------------------------------------------------------------------- */
/*  IRQ12: PS/2 mouse                                                        */
/* -------------------------------------------------------------------------- */

void irq_mouse_push(i32 abs_x, i32 abs_y, i32 dz, u8 buttons) {
    mouse_event_t *e;
    u32 next;
    if (g_irq.irq_mask & (1 << IRQ_MOUSE)) return;
    abs_x = k_clamp(abs_x, 0, SCREEN_W - 1);
    abs_y = k_clamp(abs_y, 0, SCREEN_H - 1);
    next = (g_irq.m_head + 1) % MOUSEQ_SIZE;
    if (next == g_irq.m_tail) return;
    e = &g_irq.mq[g_irq.m_head];
    e->dx = abs_x - g_irq.mx;
    e->dy = abs_y - g_irq.my;
    e->x = abs_x;
    e->y = abs_y;
    e->dz = dz;
    e->buttons = buttons;
    e->tick = g_irq.ticks;
    g_irq.mdx += e->dx;
    g_irq.mdy += e->dy;
    g_irq.mdz += dz;
    g_irq.mx = abs_x;
    g_irq.my = abs_y;
    g_irq.mbuttons_prev = g_irq.mbuttons;
    g_irq.mbuttons = buttons;
    g_irq.m_head = next;
    g_irq.mouse_events++;
    g_irq.desc[2].count++;
}

int irq_mouse_pop(mouse_event_t *out) {
    if (g_irq.m_tail == g_irq.m_head) return 0;
    *out = g_irq.mq[g_irq.m_tail];
    g_irq.m_tail = (g_irq.m_tail + 1) % MOUSEQ_SIZE;
    return 1;
}

void irq_mouse_pos(int *x, int *y) { if (x) *x = g_irq.mx; if (y) *y = g_irq.my; }
void irq_mouse_set_pos(int x, int y) {
    g_irq.mx = k_clamp(x, 0, SCREEN_W - 1);
    g_irq.my = k_clamp(y, 0, SCREEN_H - 1);
}
u8 irq_mouse_buttons(void) { return g_irq.mbuttons; }
u32 irq_mouse_count(void) { return g_irq.mouse_events; }

/* Accumulated deltas since the last call (used by the cursor integrator). */
void irq_mouse_take_delta(int *dx, int *dy, int *dz) {
    if (dx) *dx = g_irq.mdx;
    if (dy) *dy = g_irq.mdy;
    if (dz) *dz = g_irq.mdz;
    g_irq.mdx = g_irq.mdy = g_irq.mdz = 0;
}

/* -------------------------------------------------------------------------- */
/*  Host event fan-out: HAL events -> IRQ lines                              */
/* -------------------------------------------------------------------------- */

u8 irq_buttons_from_host(int host) {
    u8 b = 0;
    if (host & 1) b |= MOUSE_L;
    if (host & 2) b |= MOUSE_R;
    if (host & 4) b |= MOUSE_M;
    return b;
}

void irq_host_event(const hal_event_t *ev) {
    switch (ev->type) {
    case HAL_EV_KEYDOWN: {
        u8 sc = host_vk_to_scancode(ev->key);
        /* Deliver the transition only: no glyph.
           A GUI host resolves the character itself (Shift, CapsLock, dead keys,
           keyboard layout) and sends it as a separate HAL_EV_CHAR right after
           this event, exactly like Win32's WM_CHAR following WM_KEYDOWN.  If
           this call also derived a glyph from the scancode, every keystroke
           produced two characters -- "abc" typed as "aabbcc".  Electron relies
           on this; the Win32 chassis still derives its glyph from WM_CHAR, and
           the headless chassis sends only HAL_EV_CHAR, so neither is affected.
           Apps distinguish the two by `ch`: text arrives with ch != 0, while a
           shortcut arrives carrying only the key code. */
        if (sc) irq_keyboard_push(sc, 1, IRQ_GLYPH_NONE);
        break;
    }
    case HAL_EV_KEYUP: {
        u8 sc = host_vk_to_scancode(ev->key);
        if (sc) irq_keyboard_push(sc, 0, 0);
        break;
    }
    case HAL_EV_CHAR: {
        if (ev->ch >= 32 && ev->ch < 127) {
            /* the host already applied modifiers; deliver the final glyph */
            irq_keyboard_push(0x00, 1, (char)ev->ch);
        } else if (ev->ch == 13) {
            irq_keyboard_push(0x1C, 1, '\n');
        } else if (ev->ch == 8) {
            irq_keyboard_push(0x0E, 1, '\b');
        } else if (ev->ch == 9) {
            irq_keyboard_push(0x0F, 1, '\t');
        }
        break;
    }
    case HAL_EV_MOUSEMOVE:
        irq_mouse_push(ev->mx, ev->my, 0, irq_buttons_from_host(ev->buttons));
        break;
    case HAL_EV_MOUSEDOWN:
        irq_mouse_push(ev->mx, ev->my, 0, irq_buttons_from_host(ev->buttons));
        break;
    case HAL_EV_MOUSEUP:
        irq_mouse_push(ev->mx, ev->my, 0, irq_buttons_from_host(ev->buttons));
        break;
    case HAL_EV_WHEEL:
        irq_mouse_push(g_irq.mx, g_irq.my, ev->wheel / 120, g_irq.mbuttons);
        break;
    case HAL_EV_QUIT:
        hal_request_quit();
        break;
    default:
        break;
    }
}

/* Expose the two mouse helpers the compositor needs without duplicating the
 * button-bitmask definitions. */
int input_mouse_left(void) { return (g_irq.mbuttons & MOUSE_L) != 0; }
int input_mouse_right(void) { return (g_irq.mbuttons & MOUSE_R) != 0; }
