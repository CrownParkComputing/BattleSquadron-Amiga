/* First-stage native host for the original Battle Squadron loader.
 *
 * The 68000 still executes the byte-exact LOADER and its BOND depacker. This
 * shim supplies flat chip RAM, named-file loading, beam position, interrupts,
 * CIA defaults, and the OCS blitter subset used by the loader. */
#include "amiga.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

uint8_t chip[CHIP_SIZE];
uint32_t framebuf[SCREEN_W * SCREEN_H];
uint8_t joy_state[2];
long bs_frame_no;
long bs_blit_count;
long bs_file_load_count;
long bs_copper_moves;
long bs_nonblack_pixels;
long bs_audio_writes;

static char data_path[512];
static bool stopped;
static int cur_line;
static uint16_t dmacon, intena, intreq;
static uint32_t cop1lc;
static bool video_enabled;
static uint32_t bplpt[6], sprpt[8];
static uint16_t bplcon0, bplcon1, bplcon2;
static int16_t bpl1mod, bpl2mod;
static uint16_t color[32];
static uint16_t diwstrt = 0x2c81, diwstop = 0x2cc1;
static uint16_t ddfstrt = 0x0038, ddfstop = 0x00d0;
static uint32_t cop_pc;
static int cop_wait_line;

static uint8_t kbd_queue[32];
static int kbd_head, kbd_tail;
static uint8_t kbd_sdr;
static bool kbd_pending;

typedef struct {
    uint32_t lc, lc_play, pos, nbytes_play;
    uint16_t lenlatch, period, volume;
    double fraction;
    bool on;
} AudioChannel;
static AudioChannel audio[4];

#define AUDIO_RATE 44100
#define AUDIO_RING_FRAMES 32768
static int16_t audio_ring[AUDIO_RING_FRAMES * 2];
static int audio_write_pos, audio_read_pos;
/* The frontend's audio callback is the sole consumer and the emulation
 * thread is the sole producer.  Keep the occupancy atomic so callbacks can
 * drain the ring without taking a lock around the 68000 core. */
static _Atomic int audio_fill;

static uint16_t bltcon0, bltcon1, bltafwm, bltalwm;
static uint32_t bltpt[4];             /* A, B, C, D */
static int16_t bltmod[4];
static uint16_t bltdat[3];            /* A, B, C */
static bool blt_zero = true;

static void copper_start(void);

static void audio_dma_update(uint16_t old_dmacon)
{
    for (int channel = 0; channel < 4; channel++) {
        bool was_on = (old_dmacon & 0x0200) &&
                      (old_dmacon & (1u << channel));
        bool is_on = (dmacon & 0x0200) &&
                     (dmacon & (1u << channel));
        AudioChannel *state = &audio[channel];
        if (is_on && !was_on) {
            state->lc_play = state->lc;
            state->pos = 0;
            state->fraction = 0;
            state->nbytes_play = (uint32_t)state->lenlatch * 2;
            state->on = true;
        } else if (!is_on) {
            state->on = false;
        }
    }
}

static struct {
    uint16_t ta_latch, ta;
    bool ta_on, oneshot;
    uint8_t icr_mask, icr_flags;
    int frac;
} ciab;
static long audio_timer_calls;
static bool audio_timer_active;
static uint32_t audio_timer_return_pc;
static uint32_t audio_timer_regs[16];
static uint32_t audio_timer_sr;
static int audio_timer_pending;
#define AUDIO_RETURN_STUB 0x00f0

static uint16_t rw(uint32_t address)
{
    address &= CHIP_SIZE - 1;
    return ((uint16_t)chip[address] << 8) |
           chip[(address + 1) & (CHIP_SIZE - 1)];
}

static void ww(uint32_t address, uint16_t value)
{
    address &= CHIP_SIZE - 1;
    chip[address] = value >> 8;
    chip[(address + 1) & (CHIP_SIZE - 1)] = (uint8_t)value;
}

static uint32_t rl(uint32_t address)
{
    return ((uint32_t)rw(address) << 16) | rw(address + 2);
}

static void irq_update(void)
{
    static const uint8_t level[14] = {
        1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6
    };
    int active_level = 0;
    if (intena & 0x4000) {
        uint16_t active = intena & intreq & 0x3fff;
        for (int bit = 0; bit < 14; bit++)
            if ((active & (1u << bit)) && level[bit] > active_level)
                active_level = level[bit];
    }
    m68k_set_irq(active_level);
}

static uint16_t minterm(uint8_t function, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t d = 0;
    if (function & 0x01) d |= (uint16_t)(~a & ~b & ~c);
    if (function & 0x02) d |= (uint16_t)(~a & ~b &  c);
    if (function & 0x04) d |= (uint16_t)(~a &  b & ~c);
    if (function & 0x08) d |= (uint16_t)(~a &  b &  c);
    if (function & 0x10) d |= (uint16_t)( a & ~b & ~c);
    if (function & 0x20) d |= (uint16_t)( a & ~b &  c);
    if (function & 0x40) d |= (uint16_t)( a &  b & ~c);
    if (function & 0x80) d |= (uint16_t)( a &  b &  c);
    return d;
}

static void blit(uint16_t size)
{
    int height = (size >> 6) & 0x3ff;
    int width = size & 0x3f;
    if (!height) height = 1024;
    if (!width) width = 64;
    int ashift = (bltcon0 >> 12) & 15;
    int bshift = (bltcon1 >> 12) & 15;
    bool usea = bltcon0 & 0x0800;
    bool useb = bltcon0 & 0x0400;
    bool usec = bltcon0 & 0x0200;
    bool used = bltcon0 & 0x0100;
    bool descending = bltcon1 & 2;
    int step = descending ? -2 : 2;

    blt_zero = true;
    for (int y = 0; y < height; y++) {
        uint32_t aprevious = 0, bprevious = 0;
        for (int x = 0; x < width; x++) {
            uint16_t araw = usea ? rw(bltpt[0]) : bltdat[0];
            uint16_t braw = useb ? rw(bltpt[1]) : bltdat[1];
            uint16_t c = usec ? rw(bltpt[2]) : bltdat[2];
            if (x == 0) araw &= bltafwm;
            if (x == width - 1) araw &= bltalwm;
            uint16_t a, b;
            if (!descending) {
                a = (uint16_t)(((aprevious << 16) | araw) >> ashift);
                b = (uint16_t)(((bprevious << 16) | braw) >> bshift);
            } else {
                a = ashift ? (uint16_t)((((uint32_t)araw << 16) |
                                         aprevious) >> (16 - ashift)) : araw;
                b = bshift ? (uint16_t)((((uint32_t)braw << 16) |
                                         bprevious) >> (16 - bshift)) : braw;
            }
            aprevious = araw;
            bprevious = braw;
            uint16_t d = minterm(bltcon0 & 0xff, a, b, c);
            if (d) blt_zero = false;
            if (usea) bltpt[0] += step;
            if (useb) bltpt[1] += step;
            if (usec) bltpt[2] += step;
            if (used) {
                ww(bltpt[3], d);
                bltpt[3] += step;
            }
        }
        for (int channel = 0; channel < 4; channel++) {
            bool used_channel = channel == 0 ? usea : channel == 1 ? useb :
                                channel == 2 ? usec : used;
            if (used_channel)
                bltpt[channel] += descending ? -bltmod[channel]
                                             : bltmod[channel];
        }
    }
    bs_blit_count++;
    intreq |= 0x0040;
    irq_update();
}

static uint16_t custom_read(uint32_t reg)
{
    switch (reg) {
    case 0x002: {
        uint16_t value = dmacon & 0x07ff;
        if (blt_zero) value |= 0x2000;
        return value;
    }
    case 0x004: return (cur_line >> 8) & 7;
    case 0x006: return (cur_line & 0xff) << 8;
    case 0x00a:
    case 0x00c: {
        uint8_t state = joy_state[reg == 0x00a ? 0 : 1];
        int up = state & 1;
        int down = (state >> 1) & 1;
        int left = (state >> 2) & 1;
        int right = (state >> 3) & 1;
        return (uint16_t)((left << 9) | ((up ^ left) << 8) |
                          (right << 1) | (down ^ right));
    }
    case 0x016: {
        uint16_t value = 0xffff;       /* second buttons are active low */
        if (joy_state[0] & 0x20) value &= (uint16_t)~0x0004;
        if (joy_state[1] & 0x20) value &= (uint16_t)~0x0040;
        return value;
    }
    case 0x01c: return intena & 0x7fff;
    case 0x01e: return intreq & 0x7fff;
    default: return 0;
    }
}

static void setclr(uint16_t *reg, uint16_t value)
{
    if (value & 0x8000) *reg |= value & 0x7fff;
    else *reg &= (uint16_t)~(value & 0x7fff);
}

static void custom_write(uint32_t reg, uint16_t value)
{
    switch (reg) {
    case 0x040: bltcon0 = value; break;
    case 0x042: bltcon1 = value; break;
    case 0x044: bltafwm = value; break;
    case 0x046: bltalwm = value; break;
    case 0x048: bltpt[2] = (bltpt[2] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x04a: bltpt[2] = (bltpt[2] & 0xffff0000) | (value & 0xfffe); break;
    case 0x04c: bltpt[1] = (bltpt[1] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x04e: bltpt[1] = (bltpt[1] & 0xffff0000) | (value & 0xfffe); break;
    case 0x050: bltpt[0] = (bltpt[0] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x052: bltpt[0] = (bltpt[0] & 0xffff0000) | (value & 0xfffe); break;
    case 0x054: bltpt[3] = (bltpt[3] & 0xffff) | ((uint32_t)value << 16); break;
    case 0x056: bltpt[3] = (bltpt[3] & 0xffff0000) | (value & 0xfffe); break;
    case 0x058: blit(value); break;
    case 0x060: bltmod[2] = (int16_t)value; break;
    case 0x062: bltmod[1] = (int16_t)value; break;
    case 0x064: bltmod[0] = (int16_t)value; break;
    case 0x066: bltmod[3] = (int16_t)value; break;
    case 0x070: bltdat[2] = value; break;
    case 0x072: bltdat[1] = value; break;
    case 0x074: bltdat[0] = value; break;
    case 0x080: cop1lc = (cop1lc & 0xffff) | ((uint32_t)value << 16); break;
    case 0x082: cop1lc = (cop1lc & 0xffff0000) | (value & 0xfffe); break;
    case 0x088: copper_start(); break;
    case 0x08e: diwstrt = value; break;
    case 0x090: diwstop = value; break;
    case 0x092: ddfstrt = value; break;
    case 0x094: ddfstop = value; break;
    case 0x096: {
        uint16_t old_dmacon = dmacon;
        setclr(&dmacon, value);
        audio_dma_update(old_dmacon);
        break;
    }
    case 0x09a: setclr(&intena, value); irq_update(); break;
    case 0x09c: setclr(&intreq, value); irq_update(); break;
    case 0x100: bplcon0 = value; break;
    case 0x102: bplcon1 = value; break;
    case 0x104: bplcon2 = value; break;
    case 0x108: bpl1mod = (int16_t)value; break;
    case 0x10a: bpl2mod = (int16_t)value; break;
    default:
        if (reg >= 0x0e0 && reg < 0x0f8) {
            int plane = (reg - 0x0e0) / 4;
            if (reg & 2)
                bplpt[plane] = (bplpt[plane] & 0xffff0000) |
                                (value & 0xfffe);
            else
                bplpt[plane] = (bplpt[plane] & 0xffff) |
                                ((uint32_t)value << 16);
        } else if (reg >= 0x120 && reg < 0x140) {
            int sprite = (reg - 0x120) / 4;
            if (reg & 2)
                sprpt[sprite] = (sprpt[sprite] & 0xffff0000) |
                                (value & 0xfffe);
            else
                sprpt[sprite] = (sprpt[sprite] & 0xffff) |
                                ((uint32_t)value << 16);
        } else if (reg >= 0x180 && reg < 0x1c0) {
            color[(reg - 0x180) / 2] = value & 0x0fff;
        } else if (reg >= 0x0a0 && reg < 0x0e0) {
            AudioChannel *state = &audio[(reg - 0x0a0) / 16];
            bs_audio_writes++;
            switch (reg & 15) {
            case 0: state->lc = (state->lc & 0xffff) |
                                ((uint32_t)value << 16); break;
            case 2: state->lc = (state->lc & 0xffff0000) |
                                (value & 0xfffe); break;
            case 4: state->lenlatch = value; break;
            case 6: state->period = value ? value : 1; break;
            case 8: state->volume = value & 0x7f; break;
            default: break;
            }
        }
        break;
    }
}

static void copper_start(void)
{
    cop_pc = cop1lc & (CHIP_SIZE - 1);
    cop_wait_line = cop1lc ? 0 : -1;
}

static void copper_run_line(int line)
{
    if (cop_wait_line < 0 || line < cop_wait_line) return;
    for (int guard = 0; guard < 4096; guard++) {
        uint16_t first = rw(cop_pc);
        uint16_t second = rw(cop_pc + 2);
        if (!(first & 1)) {
            cop_pc = (cop_pc + 4) & (CHIP_SIZE - 1);
            custom_write(first & 0x01fe, second);
            bs_copper_moves++;
        } else if (!(second & 1)) {
            if (first == 0xffff && second == 0xfffe) {
                cop_wait_line = -1;
                return;
            }
            int target = (first >> 8) & 0xff;
            int mask = ((second >> 8) & 0x7f) | 0x80;
            if (((line & 0xff) & mask) >= (target & mask)) {
                cop_pc = (cop_pc + 4) & (CHIP_SIZE - 1);
            } else {
                cop_wait_line = (line & 0x100) | target;
                if (cop_wait_line <= line) cop_wait_line += 0x100;
                return;
            }
        } else {
            /* SKIP is not used by the observed lists; advance safely. */
            cop_pc = (cop_pc + 4) & (CHIP_SIZE - 1);
        }
    }
    cop_wait_line = -1;
}

static uint32_t rgb4(uint16_t value)
{
    uint32_t red = (value >> 8) & 15;
    uint32_t green = (value >> 4) & 15;
    uint32_t blue = value & 15;
    return 0xff000000u | (blue * 17 << 16) | (green * 17 << 8) | red * 17;
}

static int display_vstop(void)
{
    int start = (diwstrt >> 8) & 0xff;
    int stop = (diwstop >> 8) & 0xff;
    if (stop <= start) stop += 0x100;
    return stop;
}

static int fetch_bytes(void)
{
    int start = ddfstrt & 0xfc;
    int stop = ddfstop & 0xfc;
    int words = stop >= start ? ((stop - start) >> 3) + 1 : 20;
    if (words < 1) words = 1;
    if (words > 25) words = 25;
    return words * 2;
}

static int playfield_bit(uint32_t pointer, int source_x)
{
    int byte = source_x >= 0 ? source_x / 8 : -((-source_x + 7) / 8);
    int bit_in_byte = source_x - byte * 8;
    return (chip[(pointer + (uint32_t)byte) & (CHIP_SIZE - 1)] >>
            (7 - bit_in_byte)) & 1;
}

static void render_line(int line)
{
    int vstart = (diwstrt >> 8) & 0xff;
    int y = line - vstart;
    if (line < vstart || line >= display_vstop() || y < 0 || y >= SCREEN_H)
        return;
    uint32_t *output = &framebuf[y * SCREEN_W];
    int depth = (bplcon0 >> 12) & 7;
    int bytes = fetch_bytes();
    /* A scrolling playfield starts DMA one fetch slot early (usually $30
     * instead of $38), supplying the word that shifts into the left edge.
     * Position fetched data relative to the normal display origin; merely
     * counting the extra word but drawing it at x=0 causes a 16-pixel jump
     * whenever the game rolls its fine scroll back to the next coarse word. */
    int fetch_lead = (0x38 - (ddfstrt & 0xfc)) * 2;
    if (depth > 6) depth = 6;
    bool dma = (dmacon & 0x0300) == 0x0300;
    if (!dma || !depth) {
        uint32_t background = rgb4(color[0]);
        for (int x = 0; x < SCREEN_W; x++) output[x] = background;
        return;
    }

    bool dual = (bplcon0 & 0x0400) != 0;
    bool pf2_priority = (bplcon2 & 0x0040) != 0;
    for (int x = 0; x < SCREEN_W; x++) {
        int index = 0;
        if (!dual) {
            int source_x = x + fetch_lead - (bplcon1 & 15);
            for (int plane = 0; plane < depth; plane++)
                index |= playfield_bit(bplpt[plane], source_x) << plane;
        } else {
            int pf1 = 0, pf2 = 0;
            for (int plane = 0; plane < depth; plane++) {
                int scroll = (plane & 1) ? ((bplcon1 >> 4) & 15)
                                         : (bplcon1 & 15);
                int source_x = x + fetch_lead - scroll;
                int value = playfield_bit(bplpt[plane], source_x);
                if (plane & 1) pf2 |= value << (plane >> 1);
                else pf1 |= value << (plane >> 1);
            }
            if (pf2_priority)
                index = pf2 ? 8 + pf2 : pf1;
            else
                index = pf1 ? pf1 : (pf2 ? 8 + pf2 : 0);
        }
        output[x] = rgb4(color[index & 31]);
        if (index) bs_nonblack_pixels++;
    }
    for (int plane = 0; plane < depth; plane++)
        bplpt[plane] += bytes + ((plane & 1) ? bpl2mod : bpl1mod);
}

typedef struct {
    bool active, attached;
    int hstart;
    uint16_t low, high;
} SpriteLine;

static SpriteLine sprite_line(int number, int line)
{
    SpriteLine result = {0};
    uint32_t pointer = sprpt[number] & (CHIP_SIZE - 1);
    if (!pointer) return result;
    for (int guard = 0; guard < 64; guard++) {
        uint16_t pos = rw(pointer);
        uint16_t ctl = rw(pointer + 2);
        if (!pos && !ctl) break;
        int vstart = (pos >> 8) | ((ctl & 4) << 6);
        int vstop = (ctl >> 8) | ((ctl & 2) << 7);
        int rows = vstop - vstart;
        if (rows <= 0 || rows > 300) break;
        if (line >= vstart && line < vstop) {
            uint32_t data = pointer + 4 + (uint32_t)(line - vstart) * 4;
            result.active = true;
            result.attached = (number & 1) && (ctl & 0x0080);
            result.hstart = ((pos & 0xff) << 1) | (ctl & 1);
            result.low = rw(data);
            result.high = rw(data + 2);
            return result;
        }
        pointer = (pointer + 4 + (uint32_t)rows * 4) &
                  (CHIP_SIZE - 1);
    }
    return result;
}

static void render_sprites_line(int line)
{
    int y = line - ((diwstrt >> 8) & 0xff);
    if (y < 0 || y >= SCREEN_H) return;

    /* Sprite colours can change in the copper list.  Compose each scanline
     * while that line's palette is live rather than colouring the whole
     * sprite with the palette left at the end of the frame. */
    for (int pair = 3; pair >= 0; pair--) {
        uint8_t pixels[2][SCREEN_W] = {{0}};
        SpriteLine lines[2] = {
            sprite_line(pair * 2, line),
            sprite_line(pair * 2 + 1, line)
        };
        for (int which = 0; which < 2; which++) {
            if (!lines[which].active) continue;
            for (int bit = 0; bit < 16; bit++) {
                int x = lines[which].hstart - (diwstrt & 0xff) + bit;
                if (x < 0 || x >= SCREEN_W) continue;
                pixels[which][x] =
                    ((lines[which].low >> (15 - bit)) & 1) |
                    (((lines[which].high >> (15 - bit)) & 1) << 1);
            }
        }
        int bank = 16 + pair * 4;
        for (int x = 0; x < SCREEN_W; x++) {
            if (lines[1].attached) {
                int index = pixels[0][x] | (pixels[1][x] << 2);
                if (index) {
                    framebuf[y * SCREEN_W + x] = rgb4(color[16 + index]);
                    bs_nonblack_pixels++;
                }
            } else {
                int index = pixels[1][x];
                if (index) {
                    framebuf[y * SCREEN_W + x] = rgb4(color[bank + index]);
                    bs_nonblack_pixels++;
                }
                index = pixels[0][x];
                if (index) {
                    framebuf[y * SCREEN_W + x] = rgb4(color[bank + index]);
                    bs_nonblack_pixels++;
                }
            }
        }
    }
}

static uint8_t cia_read(uint32_t address)
{
    unsigned reg = (address >> 8) & 15;
    if (address >= 0xbfe000) {
        if (reg == 0) {
            uint8_t value = 0xff;
            if (joy_state[0] & 0x10) value &= (uint8_t)~0x40;
            if (joy_state[1] & 0x10) value &= (uint8_t)~0x80;
            return value;
        }
        if (reg == 12) return kbd_sdr;
        return 0xff;
    }
    if (reg == 13) {
        uint8_t value = ciab.icr_flags;
        if (value & ciab.icr_mask) value |= 0x80;
        ciab.icr_flags = 0;
        return value;
    }
    return 0xff;
}

static void cia_write(uint32_t address, uint8_t value)
{
    if (address >= 0xbfe000) {
        unsigned reg = (address >> 8) & 15;
        if (reg == 12) {
            kbd_sdr = value;
            kbd_pending = false;
        } else if (reg == 14 && (value & 0x40)) {
            kbd_pending = false;
        }
        return;
    }
    switch ((address >> 8) & 15) {
    case 4: ciab.ta_latch = (ciab.ta_latch & 0xff00) | value; break;
    case 5:
        ciab.ta_latch = (ciab.ta_latch & 0x00ff) | ((uint16_t)value << 8);
        ciab.ta = ciab.ta_latch;
        if (ciab.oneshot) ciab.ta_on = true;
        break;
    case 13:
        if (value & 0x80) ciab.icr_mask |= value & 0x7f;
        else ciab.icr_mask &= (uint8_t)~(value & 0x7f);
        break;
    case 14:
        ciab.oneshot = (value & 8) != 0;
        if (value & 1) ciab.ta_on = true;
        if (value & 0x10) ciab.ta = ciab.ta_latch;
        break;
    default: break;
    }
}

void amiga_key_event(uint8_t rawcode, bool up)
{
    int next = (kbd_tail + 1) % (int)sizeof kbd_queue;
    if (next == kbd_head) return;
    kbd_queue[kbd_tail] = rawcode | (up ? 0x80 : 0);
    kbd_tail = next;
}

static void kbd_pump(void)
{
    if (kbd_pending || kbd_head == kbd_tail) return;
    uint8_t code = kbd_queue[kbd_head];
    kbd_head = (kbd_head + 1) % (int)sizeof kbd_queue;
    uint8_t inverted = (uint8_t)~code;
    kbd_sdr = (uint8_t)((inverted << 1) | (inverted >> 7));
    kbd_pending = true;
    intreq |= 0x0008;
    irq_update();
}

#define PAULA_CLOCK 3546895.0
static double audio_filter_left, audio_filter_right;
static unsigned long long audio_energy;
static bool audio_transition_muted;

static void audio_mix(int16_t *output, int frames)
{
    for (int frame = 0; frame < frames; frame++) {
        int32_t left = 0, right = 0;
        for (int channel = 0; channel < 4; channel++) {
            AudioChannel *state = &audio[channel];
            if (!state->on || state->period < 8 || !state->nbytes_play)
                continue;
            state->fraction += PAULA_CLOCK /
                               ((double)state->period * AUDIO_RATE);
            while (state->fraction >= 1.0) {
                state->fraction -= 1.0;
                if (++state->pos >= state->nbytes_play) {
                    state->pos = 0;
                    state->lc_play = state->lc;
                    state->nbytes_play = (uint32_t)state->lenlatch * 2;
                    intreq |= (uint16_t)(0x0080 << channel);
                }
            }
            int8_t sample = (int8_t)chip[(state->lc_play + state->pos) &
                                         (CHIP_SIZE - 1)];
            if (audio_transition_muted) continue;
            int volume = state->volume > 64 ? 64 : state->volume;
            int32_t value = sample * volume;
            if (channel == 0 || channel == 3) left += value;
            else right += value;
        }
        double mixed_left = left * 0.75 + right * 0.25;
        double mixed_right = right * 0.75 + left * 0.25;
        audio_filter_left += 0.45 * (mixed_left - audio_filter_left);
        audio_filter_right += 0.45 * (mixed_right - audio_filter_right);
        int32_t left_out = (int32_t)(audio_filter_left * 1.8);
        int32_t right_out = (int32_t)(audio_filter_right * 1.8);
        if (left_out > 32767) left_out = 32767;
        if (left_out < -32768) left_out = -32768;
        if (right_out > 32767) right_out = 32767;
        if (right_out < -32768) right_out = -32768;
        output[frame * 2] = (int16_t)left_out;
        output[frame * 2 + 1] = (int16_t)right_out;
        audio_energy += (unsigned)(left_out < 0 ? -left_out : left_out);
        audio_energy += (unsigned)(right_out < 0 ? -right_out : right_out);
    }
    irq_update();
}

int amiga_audio_fill(void)
{
    return atomic_load_explicit(&audio_fill, memory_order_acquire);
}

void amiga_audio_frame(void)
{
    enum { FRAMES = AUDIO_RATE / 50 };
    if (atomic_load_explicit(&audio_fill, memory_order_acquire) + FRAMES >
        AUDIO_RING_FRAMES) return;
    int16_t mixed[FRAMES * 2];
    audio_mix(mixed, FRAMES);
    for (int frame = 0; frame < FRAMES; frame++) {
        audio_ring[audio_write_pos * 2] = mixed[frame * 2];
        audio_ring[audio_write_pos * 2 + 1] = mixed[frame * 2 + 1];
        audio_write_pos = (audio_write_pos + 1) % AUDIO_RING_FRAMES;
    }
    atomic_fetch_add_explicit(&audio_fill, FRAMES, memory_order_release);
}

int amiga_audio_pull(int16_t *output, int frames)
{
    int available = atomic_load_explicit(&audio_fill, memory_order_acquire);
    int copied = frames < available ? frames : available;
    for (int frame = 0; frame < copied; frame++) {
        output[frame * 2] = audio_ring[audio_read_pos * 2];
        output[frame * 2 + 1] = audio_ring[audio_read_pos * 2 + 1];
        audio_read_pos = (audio_read_pos + 1) % AUDIO_RING_FRAMES;
    }
    if (copied)
        atomic_fetch_sub_explicit(&audio_fill, copied, memory_order_release);
    for (int frame = copied; frame < frames; frame++) {
        output[frame * 2] = 0;
        output[frame * 2 + 1] = 0;
    }
    return copied;
}

static void ciab_tick(void)
{
    if (!ciab.ta_on || !ciab.ta_latch) return;
    ciab.frac += CYCLES_PER_LINE;
    int ticks = ciab.frac / 10;
    ciab.frac %= 10;
    while (ticks > 0) {
        if (ciab.ta > ticks) {
            ciab.ta -= ticks;
            break;
        }
        ticks -= ciab.ta;
        ciab.ta = ciab.ta_latch;
        ciab.icr_flags |= 1;
        if (ciab.oneshot) {
            ciab.ta_on = false;
            ticks = 0;
        }
        if (ciab.icr_mask & 1) {
            /* The WHDLoad slave turns the game's low-memory callback at $8
             * into the CIA-B timer service. Reproduce that wrapper here:
             * acknowledge CIAB, then enter the callback as a normal JSR
             * because both music handlers deliberately finish with RTS. */
            if (audio_timer_pending < 4) audio_timer_pending++;
            ciab.icr_flags = 0;
        }
    }
}

static void service_audio_timer(void)
{
    uint32_t handler = rl(8);
    if (audio_timer_active || handler < 0x100 || handler >= CHIP_SIZE)
        return;
    /* The callback ends in RTS, but it is logically an interrupt handler.
     * In particular its arithmetic changes the CCR.  Snapshot the interrupted
     * CPU state and restore it after the callback; otherwise the first branch
     * back in the game observes the music player's flags and wanders into
     * bitmap data. */
    for (int reg = 0; reg < 16; reg++)
        audio_timer_regs[reg] = m68k_get_reg(NULL, M68K_REG_D0 + reg);
    audio_timer_sr = m68k_get_reg(NULL, M68K_REG_SR);
    audio_timer_return_pc = m68k_get_reg(NULL, M68K_REG_PC);
    uint32_t stack = m68k_get_reg(NULL, M68K_REG_A7) - 4;
    ww(stack, 0);
    ww(stack + 2, AUDIO_RETURN_STUB);
    m68k_set_reg(M68K_REG_A7, stack);
    m68k_set_reg(M68K_REG_PC, handler);
    /* A real CIA-B interrupt enters with supervisor mode and masks lower
     * priority interrupts.  Without this, a queued keyboard/PORTS interrupt
     * can pre-empt the synthetic callback during the title-to-game load and
     * leave the music player stuck. */
    m68k_set_reg(M68K_REG_SR, audio_timer_sr | 0x2700);
    audio_timer_active = true;
    audio_timer_calls++;
    for (int slice = 0; slice < 16 && audio_timer_active; slice++)
        m68k_execute(200000);
    for (int reg = 0; reg < 16; reg++)
        m68k_set_reg(M68K_REG_D0 + reg, audio_timer_regs[reg]);
    m68k_set_reg(M68K_REG_SR, audio_timer_sr);
    m68k_set_reg(M68K_REG_PC, audio_timer_return_pc);
    irq_update();
    if (audio_timer_active) {
        fprintf(stderr, "native: audio callback at $%06x did not return\n",
                handler);
        stopped = true;
    }
}

unsigned int m68k_read_memory_8(unsigned int address)
{
    address &= 0xffffff;
    if (address < CHIP_SIZE) return chip[address];
    if ((address & 0xfff000) == 0xdff000) {
        uint16_t value = custom_read(address & 0x1fe);
        return address & 1 ? value & 0xff : value >> 8;
    }
    if (address >= 0xbfd000 && address < 0xbff000)
        return cia_read(address);
    return 0;
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    address &= 0xffffff;
    if (address + 1 < CHIP_SIZE)
        return ((unsigned)chip[address] << 8) | chip[address + 1];
    if ((address & 0xfff000) == 0xdff000)
        return custom_read(address & 0x1fe);
    return 0;
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return (m68k_read_memory_16(address) << 16) |
           m68k_read_memory_16(address + 2);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    address &= 0xffffff;
    if (address < CHIP_SIZE) {
        chip[address] = (uint8_t)value;
    } else if (address >= 0xbfd000 && address < 0xbff000) {
        cia_write(address, (uint8_t)value);
    } else if ((address & 0xfff000) == 0xdff000) {
        custom_write(address & 0x1fe, (value << 8) | (value & 0xff));
    }
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    address &= 0xffffff;
    if (address + 1 < CHIP_SIZE) {
        chip[address] = value >> 8;
        chip[address + 1] = (uint8_t)value;
    } else if ((address & 0xfff000) == 0xdff000) {
        custom_write(address & 0x1fe, (uint16_t)value);
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    m68k_write_memory_16(address, value >> 16);
    m68k_write_memory_16(address + 2, value);
}

unsigned int m68k_read_disassembler_8(unsigned int address)
{ return m68k_read_memory_8(address); }
unsigned int m68k_read_disassembler_16(unsigned int address)
{ return m68k_read_memory_16(address); }
unsigned int m68k_read_disassembler_32(unsigned int address)
{ return m68k_read_memory_32(address); }

static void load_named_file(void)
{
    uint32_t destination = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t requested = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t name_address = m68k_get_reg(NULL, M68K_REG_A1);
    char name[7];
    for (int i = 0; i < 6; i++) name[i] = (char)chip[name_address + i];
    name[6] = 0;
    if (!strcmp(name, "LODGAM")) {
        /* LODGAM replaces the title/speech audio overlay.  Stop its CIA
         * timer and Paula channels before the code underneath the old driver
         * is overwritten; otherwise one latched sample becomes the sustained
         * tone heard throughout the transforming screen. */
        uint16_t old_dmacon = dmacon;
        dmacon &= (uint16_t)~0x000f;
        audio_dma_update(old_dmacon);
        ciab.ta_on = false;
        audio_timer_pending = 0;
        intreq &= (uint16_t)~0x0780;
        audio_transition_muted = true;
        irq_update();
        fprintf(stderr, "native: title audio stopped for game transition\n");
    }
    char path[640];
    snprintf(path, sizeof path, "%s/%s", data_path, name);
    fprintf(stderr, "native: frame %ld load %s (%u bytes at $%06x)\n",
            bs_frame_no, name, requested, destination);
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        stopped = true;
        return;
    }
    if (destination >= CHIP_SIZE || requested > CHIP_SIZE - destination) {
        fprintf(stderr, "load %s outside chip RAM: $%x + %u\n",
                name, destination, requested);
        fclose(file);
        stopped = true;
        return;
    }
    size_t got = fread(chip + destination, 1, requested, file);
    fclose(file);
    if (got != requested) {
        fprintf(stderr, "short load %s: wanted %u, got %zu\n",
                name, requested, got);
        stopped = true;
        return;
    }
    if (audio_transition_muted && !strcmp(name, "LODS0S")) {
        /* The game-side sound/sample module is resident again.  Subsequent
         * DMACON/timer writes start the new driver from a clean state. */
        audio_transition_muted = false;
        fprintf(stderr, "native: game audio transition complete\n");
    }
    bs_file_load_count++;
    uint32_t stack = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t return_pc = rl(stack);
    m68k_set_reg(M68K_REG_A7, stack + 4);
    m68k_set_reg(M68K_REG_PC, return_pc);
}

void bs_instr_hook(unsigned int pc)
{
    /* Development-only oracle checkpoint for the genuine recompilation.
     * The reference runner may print a requested state, but translated code
     * never calls back into it and the release target does not link Musashi. */
    {
        static int initialized, emitted;
        static unsigned parity_hit = 1, parity_seen;
        static uint32_t parity_pc, parity_word_address;
        static uint16_t parity_word_value;
        static int parity_word_enabled;
        if (!initialized) {
            const char *value = getenv("BS_PARITY_PC");
            parity_pc = value ? (uint32_t)strtoul(value, NULL, 16) : UINT32_MAX;
            value = getenv("BS_PARITY_HIT");
            if (value) {
                unsigned requested = (unsigned)strtoul(value, NULL, 0);
                if (requested) parity_hit = requested;
            }
            value = getenv("BS_PARITY_WORD");
            unsigned match_address, match_value;
            if (value && sscanf(value, "%x:%x", &match_address,
                                &match_value) == 2 &&
                match_address + 1 < CHIP_SIZE) {
                parity_word_address = match_address;
                parity_word_value = (uint16_t)match_value;
                parity_word_enabled = 1;
            }
            initialized = 1;
        }
        uint16_t observed_word = parity_word_enabled
            ? (uint16_t)((chip[parity_word_address] << 8) |
                         chip[parity_word_address + 1])
            : 0;
        if (!emitted && pc == parity_pc &&
            (!parity_word_enabled || observed_word == parity_word_value) &&
            ++parity_seen == parity_hit) {
            emitted = 1;
            fprintf(stderr, "REF-STATE pc=%06x sr=%04x", pc,
                    (unsigned)m68k_get_reg(NULL, M68K_REG_SR));
            for (int i = 0; i < 8; i++)
                fprintf(stderr, " d%d=%08x", i,
                        (unsigned)m68k_get_reg(NULL, M68K_REG_D0 + i));
            for (int i = 0; i < 8; i++)
                fprintf(stderr, " a%d=%08x", i,
                        (unsigned)m68k_get_reg(NULL, M68K_REG_A0 + i));
            fprintf(stderr, "\n");
            const char *range = getenv("BS_PARITY_RANGE");
            unsigned start, length;
            if (range && sscanf(range, "%x:%x", &start, &length) == 2 &&
                start <= CHIP_SIZE && length <= CHIP_SIZE - start) {
                uint32_t hash = UINT32_C(2166136261);
                for (unsigned i = 0; i < length; i++) {
                    hash ^= chip[start + i];
                    hash *= UINT32_C(16777619);
                }
                fprintf(stderr, "REF-MEM start=%06x length=%x fnv=%08x\n",
                        start, length, hash);
                if (getenv("BS_PARITY_DUMP")) {
                    for (unsigned i = 0; i < length; i++) {
                        if ((i & 15) == 0)
                            fprintf(stderr, "REF-BYTES %06x:", start + i);
                        fprintf(stderr, " %02x", chip[start + i]);
                        if ((i & 15) == 15 || i + 1 == length)
                            fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    if (audio_timer_active && pc == AUDIO_RETURN_STUB) {
        audio_timer_active = false;
        m68k_end_timeslice();
        return;
    }
    if (pc == 0xead0) load_named_file();
    if (pc == 0x0370) stopped = true;       /* loader's terminal ILLEGAL */
}

void amiga_run_frame(void)
{
    if (video_enabled) {
        uint32_t background = rgb4(color[0]);
        for (int pixel = 0; pixel < SCREEN_W * SCREEN_H; pixel++)
            framebuf[pixel] = background;
        copper_start();
    }
    for (cur_line = 0; cur_line < LINES_PER_FRAME && !stopped; cur_line++) {
        if (video_enabled && (dmacon & 0x0280) == 0x0280)
            copper_run_line(cur_line);
        m68k_execute(CYCLES_PER_LINE);
        ciab_tick();
        if (video_enabled) {
            render_line(cur_line);
            if ((dmacon & 0x0220) == 0x0220)
                render_sprites_line(cur_line);
        }
    }
    while (audio_timer_pending > 0 && !stopped) {
        audio_timer_pending--;
        service_audio_timer();
    }
    kbd_pump();
    intreq |= 0x0020;
    irq_update();
    bs_frame_no++;
}

bool amiga_stopped(void) { return stopped; }

void amiga_report(void)
{
    char instruction[128];
    unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
    m68k_disassemble(instruction, pc, M68K_CPU_TYPE_68000);
    fprintf(stderr,
            "native: frames=%ld pc=$%06x files=%ld blits=%ld audio=%ld "
            "energy=%llu ticks=%ld copper=%ld pixels=%ld dmacon=$%04x "
            "intena=$%04x "
            "intreq=$%04x next=%s\n",
            bs_frame_no, pc, bs_file_load_count, bs_blit_count,
            bs_audio_writes, audio_energy, audio_timer_calls,
            bs_copper_moves,
            bs_nonblack_pixels, dmacon,
            intena, intreq, instruction);
    fprintf(stderr,
            "  video: bplcon0=$%04x bplcon1=$%04x bplcon2=$%04x "
            "ddf=$%04x-$%04x diw=$%04x-$%04x\n",
            bplcon0, bplcon1, bplcon2, ddfstrt, ddfstop,
            diwstrt, diwstop);
    for (int channel = 0; channel < 4; channel++) {
        AudioChannel *state = &audio[channel];
        fprintf(stderr,
                "  aud%d: on=%d lc=$%06x play=$%06x pos=%u bytes=%u "
                "len=%u per=%u vol=%u\n",
                channel, state->on, state->lc, state->lc_play, state->pos,
                state->nbytes_play, state->lenlatch, state->period,
                state->volume);
    }
}

void amiga_init(const char *directory)
{
    memset(chip, 0, sizeof chip);
    memset(framebuf, 0, sizeof framebuf);
    memset(&ciab, 0, sizeof ciab);
    memset(bplpt, 0, sizeof bplpt);
    memset(sprpt, 0, sizeof sprpt);
    memset(color, 0, sizeof color);
    memset(bltpt, 0, sizeof bltpt);
    memset(bltmod, 0, sizeof bltmod);
    memset(bltdat, 0, sizeof bltdat);
    memset(audio, 0, sizeof audio);
    memset(audio_ring, 0, sizeof audio_ring);
    memset(joy_state, 0, sizeof joy_state);
    kbd_head = kbd_tail = 0;
    kbd_sdr = 0xff;
    kbd_pending = false;
    stopped = false;
    cur_line = 0;
    bs_frame_no = bs_blit_count = bs_file_load_count = 0;
    bs_copper_moves = bs_nonblack_pixels = bs_audio_writes = 0;
    audio_write_pos = audio_read_pos = 0;
    atomic_store_explicit(&audio_fill, 0, memory_order_release);
    audio_filter_left = audio_filter_right = 0;
    audio_energy = 0;
    audio_timer_calls = 0;
    audio_timer_active = false;
    audio_timer_return_pc = 0;
    audio_timer_pending = 0;
    audio_transition_muted = false;
    dmacon = 0x03c0;                  /* WHDLoad slave handoff state */
    intena = 0x4000;
    intreq = 0;
    cop1lc = cop_pc = 0;
    cop_wait_line = -1;
    bplcon0 = bplcon1 = bplcon2 = 0;
    bpl1mod = bpl2mod = 0;
    diwstrt = 0x2c81; diwstop = 0x2cc1;
    ddfstrt = 0x0038; ddfstop = 0x00d0;
    bltcon0 = bltcon1 = 0;
    blt_zero = true;
    snprintf(data_path, sizeof data_path, "%s", directory);
    char loader[640];
    snprintf(loader, sizeof loader, "%s/LOADER", data_path);
    FILE *file = fopen(loader, "rb");
    if (!file) { perror(loader); exit(1); }
    size_t got = fread(chip + 0x100, 1, CHIP_SIZE - 0x100, file);
    fclose(file);
    if (got != 67584) {
        fprintf(stderr, "unexpected LOADER size: %zu\n", got);
        exit(1);
    }
    ww(0, 0x0001); ww(2, 0x0000);     /* SSP = $10000 */
    ww(4, 0x0000); ww(6, 0x0100);     /* PC = $100 */
    bltafwm = bltalwm = 0xffff;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
}

void amiga_enable_video(bool enabled)
{
    video_enabled = enabled;
}

int amiga_blitter_selftest(void)
{
    int failures = 0;
#define CHECK(condition, name) do {                                      \
    if (!(condition)) {                                                  \
        fprintf(stderr, "blitter self-test: FAIL %s\n", name);          \
        failures++;                                                      \
    }                                                                    \
} while (0)
#define RESET_BLITTER() do {                                             \
    memset(chip + 0x1000, 0, 0x1000);                                   \
    bltcon0 = bltcon1 = 0;                                               \
    bltafwm = bltalwm = 0xffff;                                          \
    memset(bltpt, 0, sizeof bltpt);                                      \
    memset(bltmod, 0, sizeof bltmod);                                    \
    memset(bltdat, 0, sizeof bltdat);                                    \
    blt_zero = true;                                                     \
} while (0)

    /* Straight A -> D copy. */
    RESET_BLITTER();
    ww(0x1000, 0xabcd);
    bltcon0 = 0x09f0;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    blit((1 << 6) | 1);
    CHECK(rw(0x1100) == 0xabcd, "A-to-D copy");

    /* Cookie-cut: D = (A & B) | (~A & C). */
    RESET_BLITTER();
    ww(0x1000, 0xff00);
    ww(0x1100, 0x1234);
    ww(0x1200, 0x5678);
    bltcon0 = 0x0fca;
    bltpt[0] = 0x1000;
    bltpt[1] = 0x1100;
    bltpt[2] = 0x1200;
    bltpt[3] = 0x1300;
    blit((1 << 6) | 1);
    CHECK(rw(0x1300) == 0x1278, "cookie-cut minterm");

    /* BZERO readback semantics: A AND NOT C, first nonzero then zero. */
    RESET_BLITTER();
    ww(0x1000, 0xf000);
    ww(0x1200, 0x0fff);
    bltcon0 = 0x0a50;
    bltpt[0] = 0x1000;
    bltpt[2] = 0x1200;
    blit((1 << 6) | 1);
    CHECK(!blt_zero, "BZERO clear");
    ww(0x1000, 0x0f00);
    ww(0x1200, 0xffff);
    bltpt[0] = 0x1000;
    bltpt[2] = 0x1200;
    blit((1 << 6) | 1);
    CHECK(blt_zero, "BZERO set");

    /* First/last-word masks are applied before the A shifter. */
    RESET_BLITTER();
    ww(0x1000, 0xaaaa);
    ww(0x1002, 0xbbbb);
    bltcon0 = 0x09f0;
    bltafwm = 0x00ff;
    bltalwm = 0xff00;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    blit((1 << 6) | 2);
    CHECK(rw(0x1100) == 0x00aa && rw(0x1102) == 0xbb00,
          "first/last masks");

    /* A/D modulos advance between rows. */
    RESET_BLITTER();
    ww(0x1000, 0x1111);
    ww(0x1002, 0x2222);
    ww(0x1004, 0x3333);
    bltcon0 = 0x09f0;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    bltmod[0] = 2;
    bltmod[3] = 2;
    blit((2 << 6) | 1);
    CHECK(rw(0x1100) == 0x1111 && rw(0x1104) == 0x3333,
          "row modulos");

    /* Ascending A shift carries the previous source word. */
    RESET_BLITTER();
    ww(0x1000, 0x1234);
    ww(0x1002, 0x5678);
    bltcon0 = 0x49f0;
    bltpt[0] = 0x1000;
    bltpt[3] = 0x1100;
    blit((1 << 6) | 2);
    CHECK(rw(0x1100) == 0x0123 && rw(0x1102) == 0x4567,
          "ascending A shift");

    /* Descending traversal copies words from high to low addresses. */
    RESET_BLITTER();
    ww(0x1000, 0x1357);
    ww(0x1002, 0x2468);
    bltcon0 = 0x09f0;
    bltcon1 = 0x0002;
    bltpt[0] = 0x1002;
    bltpt[3] = 0x1102;
    blit((1 << 6) | 2);
    CHECK(rw(0x1100) == 0x1357 && rw(0x1102) == 0x2468,
          "descending copy");

#undef RESET_BLITTER
#undef CHECK
    if (failures) {
        fprintf(stderr, "blitter self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "blitter self-test: PASS (8 cases)\n");
    return 0;
}

int amiga_video_selftest(void)
{
    int failures = 0;
#define VCHECK(condition, name) do {                                    \
    if (!(condition)) {                                                 \
        fprintf(stderr, "video self-test: FAIL %s\n", name);           \
        failures++;                                                     \
    }                                                                   \
} while (0)
    memset(chip, 0, sizeof chip);
    memset(framebuf, 0, sizeof framebuf);
    memset(bplpt, 0, sizeof bplpt);
    memset(sprpt, 0, sizeof sprpt);
    memset(color, 0, sizeof color);
    dmacon = 0x03a0;
    diwstrt = 0x2c81; diwstop = 0x2cc1;
    ddfstrt = 0x0038; ddfstop = 0x00d0;
    bplcon0 = 0x1000;
    bplcon2 = 0;
    bpl1mod = bpl2mod = 0;
    bplpt[0] = 0x2000;
    color[1] = 0xf00;
    chip[0x2000] = 0x80;
    render_line(0x2c);
    VCHECK(framebuf[0] == rgb4(0xf00), "bitplane set pixel");
    VCHECK(framebuf[1] == rgb4(0x000), "bitplane clear pixel");
    VCHECK(bplpt[0] == 0x2000u + (uint32_t)fetch_bytes(),
           "bitplane DMA advance");

    memset(framebuf, 0, sizeof framebuf);
    bplpt[0] = 0x2000;
    bplcon1 = 3;
    render_line(0x2c);
    VCHECK(framebuf[0] == rgb4(0x000), "fine-scroll leading pixel");
    VCHECK(framebuf[3] == rgb4(0xf00), "fine-scroll pixel delay");
    bplcon1 = 0;

    /* Battle Squadron's smooth-scroll setup fetches a word early.  With a
     * 15-pixel delay, source pixel one must already reach visible pixel zero;
     * otherwise the map blanks/jumps at every 16-pixel coarse rollover. */
    memset(framebuf, 0, sizeof framebuf);
    memset(chip + 0x2000, 0, 64);
    bplpt[0] = 0x2000;
    ddfstrt = 0x0030;
    ddfstop = 0x00d0;
    bplcon1 = 15;
    chip[0x2000] = 0x40;
    render_line(0x2c);
    VCHECK(framebuf[0] == rgb4(0xf00), "early-fetch fine-scroll lead");
    bplcon1 = 0;
    ddfstrt = 0x0038;

    ww(0x1000, 0x0180); ww(0x1002, 0x000f);
    ww(0x1004, 0xffff); ww(0x1006, 0xfffe);
    cop1lc = 0x1000;
    color[0] = 0;
    bs_copper_moves = 0;
    copper_start();
    copper_run_line(0);
    VCHECK(color[0] == 0x00f, "copper MOVE");
    VCHECK(bs_copper_moves == 1, "copper MOVE count");
    VCHECK(cop_wait_line == -1, "copper end marker");

    memset(framebuf, 0, sizeof framebuf);
    memset(sprpt, 0, sizeof sprpt);
    sprpt[0] = 0x3000;
    sprpt[1] = 0x3100;
    ww(0x3000, 0x2c40); ww(0x3002, 0x2e01);
    ww(0x3004, 0x8000); ww(0x3006, 0x8000); /* even value 3 */
    ww(0x3008, 0x8000); ww(0x300a, 0x8000);
    ww(0x3100, 0x2c40); ww(0x3102, 0x2e81); /* odd ATTACH */
    ww(0x3104, 0x0000); ww(0x3106, 0x8000); /* odd value 2 */
    ww(0x3108, 0x0000); ww(0x310a, 0x8000);
    color[27] = 0xabc; /* 3 | (2 << 2) = 11; attached bank starts at 16 */
    render_sprites_line(0x2c);
    color[27] = 0xdef;
    render_sprites_line(0x2d);
    VCHECK(framebuf[0] == rgb4(0xabc), "attached sprite palette");
    VCHECK(framebuf[1] == 0, "attached sprite transparent pixel");
    VCHECK(framebuf[SCREEN_W] == rgb4(0xdef),
           "scanline sprite palette change");

#undef VCHECK
    if (failures) {
        fprintf(stderr, "video self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "video self-test: PASS (12 cases)\n");
    return 0;
}

int amiga_input_selftest(void)
{
    int failures = 0;
#define ICHECK(condition, name) do {                                    \
    if (!(condition)) {                                                 \
        fprintf(stderr, "input self-test: FAIL %s\n", name);           \
        failures++;                                                     \
    }                                                                   \
} while (0)
    memset(joy_state, 0, sizeof joy_state);
    joy_state[0] = 0x01;
    ICHECK(custom_read(0x00a) == 0x0100, "up quadrature");
    joy_state[0] = 0x02;
    ICHECK(custom_read(0x00a) == 0x0001, "down quadrature");
    joy_state[0] = 0x04;
    ICHECK(custom_read(0x00a) == 0x0300, "left quadrature");
    joy_state[0] = 0x08;
    ICHECK(custom_read(0x00a) == 0x0003, "right quadrature");
    joy_state[0] = 0x30;
    ICHECK((cia_read(0xbfe001) & 0x40) == 0, "primary fire active low");
    ICHECK((custom_read(0x016) & 0x0004) == 0,
           "secondary fire active low");

    kbd_head = kbd_tail = 0;
    kbd_pending = false;
    intreq = 0;
    amiga_key_event(0x44, false);
    kbd_pump();
    ICHECK(kbd_sdr == 0x77, "raw Return serial encoding");
    ICHECK((intreq & 0x0008) != 0, "keyboard PORTS interrupt");
    cia_write(0xbfec01, 0);
    ICHECK(!kbd_pending, "keyboard handshake");

#undef ICHECK
    if (failures) {
        fprintf(stderr, "input self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "input self-test: PASS (9 cases)\n");
    return 0;
}

int amiga_audio_selftest(void)
{
    int failures = 0;
#define ACHECK(condition, name) do {                                    \
    if (!(condition)) {                                                 \
        fprintf(stderr, "audio self-test: FAIL %s\n", name);           \
        failures++;                                                     \
    }                                                                   \
} while (0)
    memset(audio, 0, sizeof audio);
    memset(audio_ring, 0, sizeof audio_ring);
    audio_write_pos = audio_read_pos = 0;
    atomic_store_explicit(&audio_fill, 0, memory_order_release);
    audio_filter_left = audio_filter_right = 0;
    dmacon = intreq = 0;
    chip[0x3000] = 100;
    chip[0x3001] = (uint8_t)-100;
    chip[0x3002] = 80;
    chip[0x3003] = (uint8_t)-80;
    custom_write(0x0a0, 0);
    custom_write(0x0a2, 0x3000);
    custom_write(0x0a4, 2);
    custom_write(0x0a6, 124);
    custom_write(0x0a8, 64);
    custom_write(0x096, 0x8201);
    ACHECK(audio[0].on, "DMA start");
    ACHECK(audio[0].nbytes_play == 4, "sample length latch");
    int16_t mixed[64 * 2];
    audio_mix(mixed, 64);
    long left_energy = 0, right_energy = 0;
    for (int frame = 0; frame < 64; frame++) {
        left_energy += labs(mixed[frame * 2]);
        right_energy += labs(mixed[frame * 2 + 1]);
    }
    ACHECK(left_energy > 0, "non-silent output");
    ACHECK(left_energy > right_energy, "Paula stereo panning");
    ACHECK((intreq & 0x0080) != 0, "sample-loop interrupt");
    amiga_audio_frame();
    ACHECK(amiga_audio_fill() == AUDIO_RATE / 50, "per-frame buffering");
    int16_t pulled[100 * 2];
    ACHECK(amiga_audio_pull(pulled, 100) == 100, "ring pull");
    custom_write(0x096, 0x0001);
    ACHECK(!audio[0].on, "DMA stop");

#undef ACHECK
    if (failures) {
        fprintf(stderr, "audio self-test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "audio self-test: PASS (8 cases)\n");
    return 0;
}
