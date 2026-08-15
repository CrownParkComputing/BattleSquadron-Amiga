#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

uint8_t bs_recomp_read8(const BsRecomp *machine, uint32_t address)
{
    if (address < BS_RECOMP_MEMORY_SIZE) return machine->memory[address];
    if (address >= 0xbfd000 && address < 0xbfe000)
        return machine->ciab[(address >> 8) & 15];
    if (address >= 0xbfe000 && address < 0xbff000)
        return machine->ciaa[(address >> 8) & 15];
    if ((address & 0xfff000) == 0xdff000) {
        uint16_t value = machine->custom[(address & 0x1fe) >> 1];
        return (address & 1) ? (uint8_t)value : (uint8_t)(value >> 8);
    }
    return 0;
}

uint16_t bs_recomp_read16(const BsRecomp *machine, uint32_t address)
{
    return ((uint16_t)bs_recomp_read8(machine, address) << 8) |
           bs_recomp_read8(machine, address + 1);
}

uint32_t bs_recomp_read32(const BsRecomp *machine, uint32_t address)
{
    return ((uint32_t)bs_recomp_read16(machine, address) << 16) |
           bs_recomp_read16(machine, address + 2);
}

void bs_recomp_write8(BsRecomp *machine, uint32_t address, uint8_t value)
{
    if (address < BS_RECOMP_MEMORY_SIZE) {
        machine->memory[address] = value;
    } else if (address >= 0xbfd000 && address < 0xbfe000) {
        machine->ciab[(address >> 8) & 15] = value;
    } else if (address >= 0xbfe000 && address < 0xbff000) {
        machine->ciaa[(address >> 8) & 15] = value;
    } else if ((address & 0xfff000) == 0xdff000) {
        unsigned index = (address & 0x1fe) >> 1;
        uint16_t old = machine->custom[index];
        machine->custom[index] = (address & 1)
            ? (uint16_t)((old & 0xff00) | value)
            : (uint16_t)((old & 0x00ff) | ((uint16_t)value << 8));
    }
}

void bs_recomp_write16(BsRecomp *machine, uint32_t address, uint16_t value)
{
    if (address + 1 < BS_RECOMP_MEMORY_SIZE) {
        machine->memory[address] = (uint8_t)(value >> 8);
        machine->memory[address + 1] = (uint8_t)value;
    } else if ((address & 0xfff000) == 0xdff000) {
        unsigned reg = address & 0x1fe;
        uint16_t *state = NULL;
        if (reg == 0x096) state = &machine->dmacon;
        else if (reg == 0x09a) state = &machine->intena;
        else if (reg == 0x09c) state = &machine->intreq;
        if (state) {
            if (value & 0x8000) *state |= value & 0x7fff;
            else *state &= (uint16_t)~(value & 0x7fff);
            machine->custom[reg >> 1] = *state;
        } else {
            machine->custom[reg >> 1] = value;
        }
        if (machine->custom_write_hook)
            machine->custom_write_hook(machine->custom_write_user,
                                       (uint16_t)reg, value);
    }
}

void bs_recomp_write32(BsRecomp *machine, uint32_t address, uint32_t value)
{
    bs_recomp_write16(machine, address, (uint16_t)(value >> 16));
    bs_recomp_write16(machine, address + 2, (uint16_t)value);
}

int bs_recomp_init(BsRecomp *machine, const char *data_directory)
{
    if (!machine || !data_directory) return BS_RECOMP_ERROR;
    memset(machine, 0, sizeof *machine);
    if (snprintf(machine->data_directory, sizeof machine->data_directory,
                 "%s", data_directory) >= (int)sizeof machine->data_directory) {
        snprintf(machine->error, sizeof machine->error,
                 "data-directory path is too long");
        return BS_RECOMP_ERROR;
    }
    char path[640];
    snprintf(path, sizeof path, "%s/LOADER", data_directory);
    size_t loader_size = 0;
    uint8_t *loader = read_file(path, &loader_size);
    if (!loader || loader_size > BS_RECOMP_MEMORY_SIZE - 0x100) {
        free(loader);
        snprintf(machine->error, sizeof machine->error,
                 "could not load byte-exact LOADER");
        return BS_RECOMP_ERROR;
    }
    memcpy(machine->memory + 0x100, loader, loader_size);
    int error = bs_modules_parse(loader, loader_size, machine->modules,
                                 BS_MODULE_COUNT_MAX, &machine->module_count);
    free(loader);
    if (error) {
        snprintf(machine->error, sizeof machine->error,
                 "module table: %s", bs_overlay_error(error));
        return BS_RECOMP_ERROR;
    }
    /* State observed at the original GameBootstrap entry after the hardware
     * takeover.  The host replaces that platform-only $100-$370 path, while
     * the translated game begins from an identical CPU contract. */
    machine->cpu.pc = 0x400;
    machine->cpu.sr = 0x2700;
    machine->cpu.a[0] = 0x80;
    machine->cpu.a[1] = 0x150;
    machine->cpu.a[7] = 0x10000;
    /* CIA-A port A idles high.  Its fire-button inputs are active low, so a
     * zeroed register would read as both buttons permanently held. */
    machine->ciaa[0] = 0xff;
    return BS_RECOMP_OK;
}

void bs_recomp_enable_live_input(BsRecomp *machine, int enabled)
{
    if (!machine) return;
    machine->live_input_enabled = enabled != 0;
    if (enabled && machine->cpu.a[5] != 0)
        bs_recomp_write8(machine, machine->cpu.a[5] - 28516, 0);
}

void bs_recomp_set_input(BsRecomp *machine, unsigned player, uint8_t state)
{
    if (!machine || player >= 2) return;
    machine->input[player] = state & 0x3f;
    /* $9AC6/$9AE6 take player one's fire from CIA-A port A bit 7 and player
     * two's from bit 6, both active low.  $D16 reads the same two bits to
     * start a game out of the attract demo, so the host has to drive them
     * rather than only the per-player direction mask. */
    uint8_t mask = player == 0 ? 0x80 : 0x40;
    if (state & BS_INPUT_FIRE) machine->ciaa[0] &= (uint8_t)~mask;
    else machine->ciaa[0] |= mask;
}

void bs_recomp_set_external_playfield_restore(BsRecomp *machine, int enabled)
{
    if (machine) machine->external_playfield_restore = enabled != 0;
}

void bs_recomp_set_custom_write_hook(BsRecomp *machine,
                                     BsCustomWriteHook hook, void *user)
{
    if (!machine) return;
    machine->custom_write_hook = hook;
    machine->custom_write_user = user;
}

void bs_recomp_set_audio_sample_hook(BsRecomp *machine,
                                     BsAudioSampleHook hook, void *user)
{
    if (!machine) return;
    machine->audio_sample_hook = hook;
    machine->audio_sample_user = user;
}

static int load_module(BsRecomp *machine, uint32_t descriptor_address)
{
    if (descriptor_address < 0x1980 ||
        (descriptor_address - 0x1980) % 24) {
        snprintf(machine->error, sizeof machine->error,
                 "invalid module descriptor $%06x", descriptor_address);
        return BS_RECOMP_ERROR;
    }
    size_t index = (descriptor_address - 0x1980) / 24;
    if (index >= machine->module_count) {
        snprintf(machine->error, sizeof machine->error,
                 "module descriptor $%06x is outside the table",
                 descriptor_address);
        return BS_RECOMP_ERROR;
    }
    size_t runtime_size = 0;
    int error = bs_module_load(machine->data_directory,
                               &machine->modules[index], machine->memory,
                               sizeof machine->memory, &runtime_size);
    if (error) {
        snprintf(machine->error, sizeof machine->error, "%s: %s",
                 machine->modules[index].name, bs_overlay_error(error));
        return BS_RECOMP_ERROR;
    }
    machine->file_load_count++;
    /* The demonstrated bootstrap calls all return with XNZVC clear. */
    machine->cpu.sr &= 0xffe0;
    return BS_RECOMP_OK;
}

/* Exact C translation of Clear32Words at $1C9E.  Despite its historic name,
 * the routine clears one word in every longword, advances A0 by 128 bytes,
 * and leaves D0.w at $FFFF after DBF. */
static void clear32words(BsRecomp *machine)
{
    machine->cpu.d[0] = 31;
    for (int i = 0; i < 32; i++) {
        bs_recomp_write16(machine, machine->cpu.a[0], 0);
        machine->cpu.a[0] += 4;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[0] - 1);
    }
}

static void palette_step(BsRecomp *machine)
{
    uint16_t d1 = (uint16_t)machine->cpu.d[1];
    uint16_t d2 = (uint16_t)machine->cpu.d[2];
    uint16_t d4 = (uint16_t)machine->cpu.d[4];
    uint16_t d5 = (uint16_t)machine->cpu.d[5];
    uint16_t d6 = (uint16_t)machine->cpu.d[6];
    uint16_t d7 = (uint16_t)machine->cpu.d[7];
    if ((uint8_t)d5 == (uint8_t)d1) {
        d5 = (uint16_t)(15u << (d6 & 15));
        d2 &= d5;
        d7 = (uint16_t)(d7 << (d6 & 15));
        if (d2 >= d7) {
            d4 &= d5;
            if (d2 != d4) {
                d5 = (uint16_t)(1u << (d6 & 15));
                uint16_t value = bs_recomp_read16(machine,
                                                   machine->cpu.a[0]);
                bs_recomp_write16(machine, machine->cpu.a[0],
                                  (uint16_t)(value + d5));
            }
        }
        d7 = (uint16_t)(d7 >> (d6 & 15));
    }
    d5 = (uint16_t)((d5 & 0xff00) | (uint8_t)(d5 - 1));
    d6 = (uint16_t)((d6 & 0xff00) | (uint8_t)(d6 + 4));
    machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) | d2;
    machine->cpu.d[4] = (machine->cpu.d[4] & 0xffff0000) | d4;
    machine->cpu.d[5] = (machine->cpu.d[5] & 0xffff0000) | d5;
    machine->cpu.d[6] = (machine->cpu.d[6] & 0xffff0000) | d6;
    machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) | d7;
}

/* Translation of $1C2C/$1C7A. Raster waits are a platform scheduling seam;
 * the deterministic palette expansion and its architectural register result
 * remain identical to the original routine. */
static void expand_palette(BsRecomp *machine)
{
    machine->cpu.a[0] = machine->cpu.a[2];
    clear32words(machine);
    machine->cpu.d[3] = 47;
    machine->cpu.d[1] = 2;
    machine->cpu.d[7] = 15;
    for (int row = 0; row < 48; row++) {
        machine->cpu.d[0] = 31;
        machine->cpu.a[0] = machine->cpu.a[2];
        machine->cpu.a[3] = machine->cpu.a[1];
        for (int entry = 0; entry < 32; entry++) {
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                bs_recomp_read16(machine, machine->cpu.a[3]);
            machine->cpu.a[3] += 2;
            machine->cpu.d[4] = (machine->cpu.d[4] & 0xffff0000) |
                bs_recomp_read16(machine, machine->cpu.a[0]);
            machine->cpu.d[5] = 2;
            machine->cpu.d[6] = 0;
            palette_step(machine);
            palette_step(machine);
            palette_step(machine);
            machine->cpu.a[0] += 4;
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                (uint16_t)(machine->cpu.d[0] - 1);
        }
        uint16_t d1 = (uint16_t)(machine->cpu.d[1] - 1);
        machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | d1;
        if (d1 == 0xffff) {
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | 2;
            machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) |
                                (uint16_t)(machine->cpu.d[7] - 1);
        }
        machine->cpu.d[3] = (machine->cpu.d[3] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[3] - 1);
    }
    /* Final SUBQ.W #1,D7 is 0-1: X,N,C set; Z,V clear. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x19);
}

/* Translation of $1CAE/$1CEE.  The original performs the fade over 48
 * raster-synchronised passes.  Raster synchronisation is a host scheduling
 * boundary here; all memory and architectural register effects are kept. */
static void darken_palette(BsRecomp *machine)
{
    machine->cpu.d[3] = 47;
    machine->cpu.d[1] = 2;
    for (int row = 0; row < 48; row++) {
        machine->cpu.a[0] = machine->cpu.a[1];
        machine->cpu.d[0] = 31;
        for (int entry = 0; entry < 32; entry++) {
            uint16_t d2 = bs_recomp_read16(machine, machine->cpu.a[0]);
            uint8_t d5 = 2;
            uint8_t d6 = 8;
            for (int component = 0; component < 3; component++) {
                if ((uint8_t)machine->cpu.d[1] == d5) {
                    uint16_t d7 = (uint16_t)(15u << (d6 & 15));
                    d2 &= d7;
                    if (d2 != 0) {
                        d7 = (uint16_t)(1u << (d6 & 15));
                        uint16_t value = bs_recomp_read16(machine,
                                                           machine->cpu.a[0]);
                        bs_recomp_write16(machine, machine->cpu.a[0],
                                          (uint16_t)(value - d7));
                    }
                    machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) |
                                        d7;
                }
                d5 = (uint8_t)(d5 - 1);
                d6 = (uint8_t)(d6 - 4);
            }
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) | d2;
            /* MOVEQ clears the upper 24 bits before the byte operations. */
            machine->cpu.d[5] = d5;
            machine->cpu.d[6] = d6;
            machine->cpu.a[0] += 4;
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                (uint16_t)(machine->cpu.d[0] - 1);
        }
        uint16_t d1 = (uint16_t)(machine->cpu.d[1] - 1);
        machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | d1;
        if (d1 == 0xffff)
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | 2;
        machine->cpu.d[3] = (machine->cpu.d[3] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[3] - 1);
    }
    /* This is the exact CCR restored by the reference execution after the
     * fade's raster-synchronised interrupt activity. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x10);
}

/* Direct equivalent of loader $1358. */
static void clear_longwords(BsRecomp *machine, uint16_t counter)
{
    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | counter;
    do {
        bs_recomp_write32(machine, machine->cpu.a[0], 0);
        machine->cpu.a[0] += 4;
        counter--;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | counter;
    } while (counter != 0xffff);
}

/* Direct equivalent of loader $1360. */
static void copy_longwords(BsRecomp *machine, uint16_t counter)
{
    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | counter;
    do {
        bs_recomp_write32(machine, machine->cpu.a[0],
                          bs_recomp_read32(machine, machine->cpu.a[1]));
        machine->cpu.a[0] += 4;
        machine->cpu.a[1] += 4;
        counter--;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | counter;
    } while (counter != 0xffff);
}

/* Loader $138C: relocate the Y coordinate in each 20-byte object record. */
static int adjust_object_chain(BsRecomp *machine)
{
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t field = machine->cpu.a[0] + 2;
        bs_recomp_write16(machine, field,
                          (uint16_t)(bs_recomp_read16(machine, field) + 0x16));
        machine->cpu.a[0] += 0x14;
        if (bs_recomp_read16(machine, machine->cpu.a[0]) == 0)
            return BS_RECOMP_OK;
    }
    snprintf(machine->error, sizeof machine->error,
             "unterminated object chain at $%06x", machine->cpu.a[0]);
    return BS_RECOMP_ERROR;
}

static int adjust_s0f_chains(BsRecomp *machine)
{
    static const uint32_t starts[] = {0x2e508, 0x2e582, 0x2e5e8, 0x2e676};
    for (size_t i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        machine->cpu.a[0] = starts[i];
        if (adjust_object_chain(machine)) return BS_RECOMP_ERROR;
    }
    return BS_RECOMP_OK;
}

/* Loader $109E-$1272.  This is the shared title/game state initializer; its
 * palette preset is selected by the game-mode word at $A752. */
static void initialise_game_state(BsRecomp *machine)
{
    bs_recomp_write8(machine, 0x4e9f, bs_recomp_read8(machine, 0xaaa5));
    bs_recomp_write8(machine, 0x4fa9, bs_recomp_read8(machine, 0xaab1));
    bs_recomp_write8(machine, 0x4e63, 0xff);
    bs_recomp_write8(machine, 0x4f6d, bs_recomp_read8(machine, 0xaad5));
    bs_recomp_write16(machine, 0x47ce, bs_recomp_read16(machine, 0xa750));
    bs_recomp_write8(machine, 0x2022, 0);
    bs_recomp_write8(machine, 0x2023, 0);

    bs_recomp_write16(machine, 0xdff096, 0x8400);
    bs_recomp_write32(machine, 0xdff084, 0xc6b2);
    machine->cpu.d[1] = 0xc052;
    bs_recomp_write16(machine, 0xc04c, (uint16_t)machine->cpu.d[1]);
    machine->cpu.d[1] = (machine->cpu.d[1] << 16) |
                        (machine->cpu.d[1] >> 16);
    bs_recomp_write16(machine, 0xc048, (uint16_t)machine->cpu.d[1]);
    bs_recomp_write32(machine, 0x141a, 0x14f6);
    bs_recomp_write32(machine, 0x9c38, 0x14ea);
    bs_recomp_write16(machine, 0x9c3c, 0);

    static const uint8_t colour_presets[][7] = {
        {0x23, 0x23, 0x23, 0x37, 0x4b, 0x23, 0x32},
        {0x32, 0x32, 0x32, 0x4b, 0x64, 0x32, 0x46},
        {0x4b, 0x4b, 0x4b, 0x6e, 0x7d, 0x4b, 0x64},
    };
    uint16_t mode = bs_recomp_read16(machine, 0xa752);
    unsigned preset = mode < 2 ? mode : 2;
    for (int i = 0; i < 7; i++)
        bs_recomp_write8(machine, 0x7768 + i, colour_presets[preset][i]);

    bs_recomp_write32(machine, 0x7906, 0x2000);
    bs_recomp_write32(machine, 0x7902, 0x28000);
    bs_recomp_write8(machine, 0x5f22, 0x0a);
    bs_recomp_write8(machine, 0x5f23, 0x32);
    bs_recomp_write16(machine, 0x6ffe, 0);
    bs_recomp_write16(machine, 0x9c26, 0);
    bs_recomp_write32(machine, 0x9c2e, 0x4a000);
    bs_recomp_write32(machine, 0x2b1a, 0x17400);
    bs_recomp_write16(machine, 0x4106, 0);
    bs_recomp_write8(machine, 0x1d0a, 0);
    bs_recomp_write8(machine, 0x6ffd, 0);

    static const uint32_t clear_words[] = {
        0x107a, 0x790e, 0x7914, 0xa142, 0x9c3e,
        0x7834, 0x7770, 0x5c2a, 0xa14c, 0x9c42, 0x79e0,
    };
    for (size_t i = 0; i < sizeof clear_words / sizeof clear_words[0]; i++)
        bs_recomp_write16(machine, clear_words[i], 0);
    static const uint32_t clear_bytes[] = {
        0x6ffc, 0x79de, 0x1d0a, 0x141e, 0x197e, 0x7000, 0x197f,
    };
    for (size_t i = 0; i < sizeof clear_bytes / sizeof clear_bytes[0]; i++)
        bs_recomp_write8(machine, clear_bytes[i], 0);

    bs_recomp_write32(machine, 0xc052, 0x01005200);
    bs_recomp_write16(machine, 0x9c24, 0x0130);
    bs_recomp_write32(machine, 0x9c28, 0x65000);
    bs_recomp_write16(machine, 0x9c2c, 0);
    bs_recomp_write16(machine, 0x9c32, 0x0100);
    machine->cpu.a[0] = bs_recomp_read32(machine, 0x9c38);
    bs_recomp_write32(machine, 0x7550,
                      bs_recomp_read32(machine, machine->cpu.a[0]));
    bs_recomp_write16(machine, 0x9c36, 0);
    bs_recomp_write8(machine, 0x5f33, 0);

    machine->cpu.a[0] = 0xc8ba;
    clear_longwords(machine, 0x00bf);
    machine->cpu.a[0] = 0x4976;
    clear_longwords(machine, 0x004f);
    machine->cpu.a[0] = 0x2e040;
    clear_longwords(machine, 0x011f);
    machine->cpu.a[0] = 0x2dc80;
    clear_longwords(machine, 0x00ef);
    /* Final CLR.L sets Z and clears NZVC while preserving X. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

/* Loader $AADC keyboard-help redraw, reached once when the title wait resets
 * the raw-key latch. */
static void redraw_keyboard_help(BsRecomp *machine)
{
    machine->cpu.a[2] = 0xaa9e;
    machine->cpu.d[1] = (machine->cpu.d[1] & 0xffffff00) | 0x5f;
    machine->cpu.d[5] = 4;
    for (int key = 0; key < 5; key++) {
        machine->cpu.a[0] = 0x62000 +
                            bs_recomp_read16(machine, machine->cpu.a[2]);
        machine->cpu.a[1] = 0x23b38;
        uint32_t source_offset = machine->cpu.a[2] +
            (bs_recomp_read16(machine, machine->cpu.a[2] + 6) ? 10 : 8);
        machine->cpu.a[1] += bs_recomp_read16(machine, source_offset);
        machine->cpu.d[6] = 4;
        for (int plane = 0; plane < 5; plane++) {
            machine->cpu.a[3] = machine->cpu.a[0];
            uint16_t rows = bs_recomp_read16(machine, machine->cpu.a[2] + 2);
            uint16_t width = bs_recomp_read16(machine, machine->cpu.a[2] + 4);
            machine->cpu.d[0] = rows;
            for (unsigned row = 0; row <= rows; row++) {
                machine->cpu.d[4] = (uint16_t)(width - 1);
                for (unsigned column = 0; column < width; column++)
                    bs_recomp_write8(machine, machine->cpu.a[3]++,
                                     bs_recomp_read8(machine,
                                                     machine->cpu.a[1]++));
                machine->cpu.d[4] = 0xffff;
                machine->cpu.a[3] += 40 - width;
                machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                    (uint16_t)(machine->cpu.d[0] - 1);
            }
            machine->cpu.a[0] += 0x1f40;
            machine->cpu.d[6] = (machine->cpu.d[6] & 0xffff0000) |
                                (uint16_t)(machine->cpu.d[6] - 1);
        }
        machine->cpu.a[2] += 12;
        machine->cpu.d[1] = (machine->cpu.d[1] & 0xffffff00) |
                            (uint8_t)(machine->cpu.d[1] - 2);
        machine->cpu.d[5] = (machine->cpu.d[5] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[5] - 1);
    }
}

static void disable_speech_entrypoints(BsRecomp *machine)
{
    machine->cpu.a[0] = 0x246f0;
    machine->cpu.d[0] = 16;
    for (int i = 0; i < 17; i++) {
        bs_recomp_write16(machine, machine->cpu.a[0], 0x4e75);
        machine->cpu.a[0] += 6;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[0] - 1);
    }
}

/* Loader $1288 via $3722/$2B32: initialise one 266-byte player object from
 * its animation template, then clear the trailing runtime workspace. */
static void initialise_player_object(BsRecomp *machine)
{
    machine->cpu.d[1] = bs_recomp_read16(machine, machine->cpu.a[4] + 58);
    machine->cpu.d[1] = (uint16_t)machine->cpu.d[1] * 24u;
    machine->cpu.d[2] = bs_recomp_read16(machine, machine->cpu.a[4] + 60);
    machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                        (uint16_t)(machine->cpu.d[2] << 2);
    machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                        (uint16_t)(machine->cpu.d[1] + machine->cpu.d[2]);
    machine->cpu.a[2] = 0x2024 + (int16_t)machine->cpu.d[1];
    machine->cpu.a[1] = bs_recomp_read32(machine, machine->cpu.a[2]);
    uint32_t source = machine->cpu.a[1];
    uint32_t object = machine->cpu.a[4];
    bs_recomp_write16(machine, object + 28, bs_recomp_read16(machine, source));
    bs_recomp_write16(machine, object + 58, bs_recomp_read16(machine, source + 2));
    bs_recomp_write32(machine, object + 30, bs_recomp_read32(machine, source + 4));
    bs_recomp_write32(machine, object + 12, bs_recomp_read32(machine, source + 8));
    bs_recomp_write32(machine, object + 16, bs_recomp_read32(machine, source + 12));
    bs_recomp_write32(machine, object + 20, bs_recomp_read32(machine, source + 16));
    bs_recomp_write32(machine, object + 62, bs_recomp_read32(machine, source + 20));
    bs_recomp_write32(machine, object + 24, source + 24);

    static const uint8_t byte_fields[] = {90, 91, 97, 41, 100, 49, 57, 44, 96};
    for (size_t i = 0; i < sizeof byte_fields; i++)
        bs_recomp_write8(machine, object + byte_fields[i], 0);
    bs_recomp_write8(machine, object + 56,
                     bs_recomp_read8(machine, machine->cpu.a[5] + 10059));
    bs_recomp_write16(machine, object + 66, 3);
    bs_recomp_write8(machine, object + 38, 0x96);
    bs_recomp_write16(machine, object + 8, 0x1e);
    bs_recomp_write16(machine, object + 52, 0x12c);
    bs_recomp_write16(machine, object + 68, 0x80);
    bs_recomp_write16(machine, object + 70, 0x80);
    bs_recomp_write16(machine, object + 72, 0);
    bs_recomp_write32(machine, object + 76,
                      bs_recomp_read32(machine, object + 80));
    bs_recomp_write16(machine, object + 118, 0);
    bs_recomp_write16(machine, object, bs_recomp_read16(machine, object + 54));
    bs_recomp_write16(machine, object + 2, 0x200);
    bs_recomp_write8(machine, object + 48, 0x91);
    bs_recomp_write16(machine, object + 10, 6);
    bs_recomp_write16(machine, object + 46, 0);
    bs_recomp_write32(machine, object + 106, 0x30303030);
    bs_recomp_write32(machine, object + 110, 0x30303030);
    bs_recomp_write32(machine, object + 114, 0x30303030);
    bs_recomp_write32(machine, object + 92, 0);
    if (bs_recomp_read8(machine, object + 39) == 0) {
        bs_recomp_write8(machine, object + 38, 0xff);
        bs_recomp_write16(machine, object + 68, 0);
        for (int offset = 0; offset <= 6; offset += 2)
            bs_recomp_write16(machine, object + offset, 0x03e7);
    }
    machine->cpu.a[0] = object + 122;
    clear_longwords(machine, 35);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x04);
}

/* Loader $1276: the real-game entry resets the persistent player fields
 * before falling through the shared $1288 object initialiser. */
static void initialise_live_player(BsRecomp *machine, uint32_t object)
{
    machine->cpu.a[4] = object;
    bs_recomp_write8(machine, object + 45, 0);
    bs_recomp_write16(machine, object + 120, 0);
    bs_recomp_write16(machine, object + 58,
                      bs_recomp_read16(machine, machine->cpu.a[5] + 10060));
    bs_recomp_write16(machine, object + 60, 0);
    initialise_player_object(machine);
}

/* Loader $2A5E/$2AA2 shared capacity-bar expansion. */
static void expand_capacity_bars(BsRecomp *machine, int second_player)
{
    for (int i = 0; i < 4; i++)
        bs_recomp_write32(machine, 0x2a4e + i * 4, 0x01480148);
    machine->cpu.a[0] = second_player ? 0xc0aa : 0xc076;
    uint32_t count_address = machine->cpu.a[5] -
                             (second_player ? 12408 : 12674);
    int16_t counter = (int16_t)(bs_recomp_read16(machine, count_address) - 1);
    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                        (uint16_t)counter;
    if (counter >= 0) {
        if (counter > 7) counter = 7;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                            (uint16_t)counter;
        machine->cpu.a[1] = second_player ? 0x2a5e : 0x2a4e;
        for (int i = 0; i <= counter; i++) {
            if (second_player) {
                machine->cpu.a[1] -= 2;
                bs_recomp_write16(machine, machine->cpu.a[1], 0x0140);
            } else {
                bs_recomp_write16(machine, machine->cpu.a[1], 0x0140);
                machine->cpu.a[1] += 2;
            }
        }
    }
    machine->cpu.d[0] = 12;
    for (int record = 0; record < 13; record++) {
        machine->cpu.a[1] = 0x2a4e;
        static const uint8_t offsets[] = {0, 8, 12, 20, 24, 32, 36, 44};
        for (size_t i = 0; i < sizeof offsets; i++) {
            bs_recomp_write16(machine, machine->cpu.a[0] + offsets[i],
                              bs_recomp_read16(machine, machine->cpu.a[1]));
            machine->cpu.a[1] += 2;
        }
        machine->cpu.a[0] += 0x7c;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[0] - 1);
    }
    machine->cpu.sr &= 0xffe0;
}

static void draw_life_icon_pass(BsRecomp *machine)
{
    machine->cpu.a[2] = 0x4448;
    machine->cpu.a[3] = machine->cpu.a[2];
    if ((uint8_t)machine->cpu.d[5] != 0) {
        machine->cpu.d[5] = (machine->cpu.d[5] & 0xffffff00) |
                            (uint8_t)(machine->cpu.d[5] - 1);
        machine->cpu.a[2] += 0x10;
    }
    if ((uint8_t)machine->cpu.d[5] != 0) {
        machine->cpu.d[5] = (machine->cpu.d[5] & 0xffffff00) |
                            (uint8_t)(machine->cpu.d[5] - 1);
        machine->cpu.a[3] = machine->cpu.a[2];
    }
    machine->cpu.d[0] = 8;
    for (int row = 0; row < 9; row++) {
        bs_recomp_write8(machine, machine->cpu.a[0],
                         bs_recomp_read8(machine, machine->cpu.a[2]++));
        bs_recomp_write8(machine, machine->cpu.a[0] + 4,
                         bs_recomp_read8(machine, machine->cpu.a[2] + 8));
        bs_recomp_write8(machine, machine->cpu.a[0] + 1,
                         bs_recomp_read8(machine, machine->cpu.a[3]++));
        bs_recomp_write8(machine, machine->cpu.a[0] + 5,
                         bs_recomp_read8(machine, machine->cpu.a[3] + 8));
        machine->cpu.a[0] += 0x70;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[0] - 1);
    }
}

static void draw_life_icons(BsRecomp *machine, int second_player)
{
    machine->cpu.a[0] = second_player ? 0xb3d8 : 0xb394;
    machine->cpu.d[5] = (machine->cpu.d[5] & 0xffffff00) |
        bs_recomp_read8(machine, machine->cpu.a[5] -
                        (second_player ? 12418 : 12684));
    draw_life_icon_pass(machine);
    machine->cpu.a[0] -= 0x3e4;
    draw_life_icon_pass(machine);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

static void copy_tile_column(BsRecomp *machine, uint32_t source,
                             uint32_t destination)
{
    for (int plane = 0; plane < 5; plane++)
        bs_recomp_write16(machine, destination + (uint32_t)plane * 0x6000,
                          bs_recomp_read16(machine,
                                           source + (uint32_t)plane * 0x20));
}

/* Loader $9C44-$9EA4.  The two copy blits in its 24-row loop are expressed
 * as direct five-plane word copies; this is native rendering data movement,
 * not a chipset/interpreter call. */
static int scroll_frame(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];
    uint16_t hold = bs_recomp_read16(machine, base + 7234);
    int update_map = 1;
    if (hold) {
        hold--;
        bs_recomp_write16(machine, base + 7234, hold);
        if (hold) update_map = 0;
        else {
            bs_recomp_write8(machine, base - 4100,
                             (uint8_t)~bs_recomp_read8(machine, base - 4100));
            bs_recomp_write16(machine, base + 7230, 0);
            bs_recomp_write16(machine, base + 7232,
                              bs_recomp_read16(machine, base + 7228));
            bs_recomp_write16(machine, base + 7222, 0);
            update_map = 0;
        }
    }
    if (update_map) {
        bs_recomp_write32(machine, base - 9266,
                          bs_recomp_read32(machine, base + 7208));
        bs_recomp_write16(machine, base - 9268,
                          bs_recomp_read16(machine, base + 7218));
        machine->cpu.d[1] &= 0xffff0000;
        machine->cpu.d[3] = 1;
        if ((int8_t)bs_recomp_read8(machine, base - 12702) >= 0) {
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                (uint16_t)(machine->cpu.d[1] +
                           bs_recomp_read16(machine, base - 12740) - 0x100);
            machine->cpu.d[3] = (machine->cpu.d[3] & 0xffffff00) |
                                (uint8_t)(machine->cpu.d[3] + 1);
        }
        if ((int8_t)bs_recomp_read8(machine, base - 12436) >= 0) {
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                (uint16_t)(machine->cpu.d[1] +
                           bs_recomp_read16(machine, base - 12474) - 0x100);
            machine->cpu.d[3] = (machine->cpu.d[3] & 0xffffff00) |
                                (uint8_t)(machine->cpu.d[3] + 1);
        }
        if ((uint8_t)machine->cpu.d[3] == 1) {
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | 0x130;
        } else {
            uint16_t d1 = (uint16_t)machine->cpu.d[1] >>
                          ((uint8_t)machine->cpu.d[3] & 63);
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) | d1;
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                                ((uint16_t)machine->cpu.d[2] >> 1);
            d1 = (uint16_t)(d1 + (uint16_t)machine->cpu.d[2] + 0x100);
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | d1;
        }
        int16_t target = (int16_t)machine->cpu.d[1];
        int16_t current = (int16_t)bs_recomp_read16(machine, base + 7204);
        if (target != current)
            current += target > current ? 1 : -1;
        bs_recomp_write16(machine, base + 7204, (uint16_t)current);
        bs_recomp_write16(machine, base + 7222, 1);

        if (!(bs_recomp_read8(machine, base - 4099) == 0x0e &&
              bs_recomp_read16(machine, base + 7206) == 0x00f0)) {
            if (bs_recomp_read16(machine, 0xa14c) != 0 ||
                ((int8_t)bs_recomp_read8(machine, base - 1570) < 0 &&
                 (bs_recomp_read16(machine, base + 7206) == 0x1fd6 ||
                  bs_recomp_read16(machine, base + 7206) == 0x0c1c))) {
                bs_recomp_write16(machine, base + 7222, 0);
                update_map = 0;
            } else {
                bs_recomp_write16(machine, base + 7206,
                    (uint16_t)(bs_recomp_read16(machine, base + 7206) + 1));
                bs_recomp_write16(machine, base + 7218,
                    (uint16_t)(bs_recomp_read16(machine, base + 7218) - 1));
                int16_t phase = (int16_t)(bs_recomp_read16(machine,
                                                           base + 7212) - 2);
                bs_recomp_write16(machine, base + 7212, (uint16_t)phase);
                if (phase < 0) {
                    bs_recomp_write16(machine, base + 7212, 0x1e);
                    uint32_t map = bs_recomp_read32(machine, base + 7214) - 0x30;
                    bs_recomp_write32(machine, base + 7214, map);
                    if (map < 0x44000) {
                        bs_recomp_write32(machine, base + 7214, 0x49fd0);
                        bs_recomp_write16(machine, base + 7206, 1);
                    }
                }
                uint32_t screen = bs_recomp_read32(machine, base + 7208) - 0x30;
                if (screen < 0x62000) {
                    screen = 0x64fd0;
                    bs_recomp_write16(machine, base + 7218, 0xff);
                }
                bs_recomp_write32(machine, base + 7208, screen);

                bs_recomp_write16(machine, machine->cpu.a[6] + 102, 0x5ffe);
                bs_recomp_write16(machine, machine->cpu.a[6] + 100, 0x001e);
                bs_recomp_write16(machine, machine->cpu.a[6] + 64, 0x09f0);
                bs_recomp_write16(machine, machine->cpu.a[6] + 66, 0);
                bs_recomp_write32(machine, machine->cpu.a[6] + 68, 0xffffffff);
                machine->cpu.a[2] = bs_recomp_read32(machine, base + 7214);
                machine->cpu.a[0] = screen;
                machine->cpu.a[4] = screen + 0x3000;
                machine->cpu.a[3] = 0x4a000 +
                                     bs_recomp_read16(machine, base + 7212);
                machine->cpu.d[0] = 23;
                for (int row = 0; row < 24; row++) {
                    /* MOVEQ #0,D1 precedes each map-word fetch.  Retaining
                     * the prior frame's upper word corrupts every terrain
                     * column after the first one. */
                    machine->cpu.d[1] = bs_recomp_read16(machine,
                                                          machine->cpu.a[2]);
                    machine->cpu.a[2] += 2;
                    machine->cpu.d[1] += machine->cpu.d[1];
                    machine->cpu.a[1] = machine->cpu.a[3] + machine->cpu.d[1];
                    copy_tile_column(machine, machine->cpu.a[1],
                                     machine->cpu.a[0]);
                    copy_tile_column(machine, machine->cpu.a[1],
                                     machine->cpu.a[4]);
                    machine->cpu.a[0] += 2;
                    machine->cpu.a[4] += 2;
                    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                        (uint16_t)(machine->cpu.d[0] - 1);
                }
            }
        } else {
            bs_recomp_write16(machine, base + 7222, 0);
            update_map = 0;
        }
    }

    machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                        bs_recomp_read16(machine, base + 7204);
    machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                        (uint16_t)machine->cpu.d[1];
    uint16_t fine = (uint16_t)(machine->cpu.d[1] - 0x100);
    fine = (uint16_t)((fine >> 3) & 0xfffe);
    /* $9C44 is entered from a masked raster poll whose value is below
     * $10000.  MOVE.W retains the physical D1 upper word, but that caller
     * contract makes it zero.  Express the proven value directly at this
     * host scheduling seam so unrelated translated interrupt state cannot
     * leak into the Copper bitplane address. */
    machine->cpu.d[1] = fine;
    machine->cpu.d[1] += bs_recomp_read32(machine, base + 7208);
    machine->cpu.a[0] = 0xb278;
    machine->cpu.d[0] = 4;
    for (int plane = 0; plane < 5; plane++) {
        bs_recomp_write16(machine, machine->cpu.a[0],
                          (uint16_t)(machine->cpu.d[1] >> 16));
        bs_recomp_write16(machine, machine->cpu.a[0] + 4,
                          (uint16_t)machine->cpu.d[1]);
        machine->cpu.d[1] += 0x6000;
        machine->cpu.a[0] += 8;
        machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[0] - 1);
    }
    uint16_t scroll = (uint16_t)(15 - (uint16_t)machine->cpu.d[2]) & 15;
    machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | scroll;
    machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) | (scroll << 4);
    machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                        ((uint16_t)machine->cpu.d[1] |
                         (uint16_t)machine->cpu.d[2]);
    bs_recomp_write16(machine, 0xb26c, (uint16_t)machine->cpu.d[1]);
    machine->cpu.sr &= 0xffe0;
    return BS_RECOMP_OK;
}

/* Native translation of the real new-game path $842/$852 and $926-$A9E.
 * This must not be replaced by a map-pointer seed: the attract path warms
 * the same ring while spawning demo objects, whereas $998 deliberately
 * performs terrain-only scrolls and palette convergence. */
/* $926-$A9E.  The new-game state build-up and its 126-iteration transition.
 * It is reached from the title's $842 setup edge and from LAB_D52 when a fire
 * button starts a game out of the attract demo, so the overlay installation
 * that precedes it belongs to each caller rather than here. */
/* LODGAM $247C8.  The same channel-state shape as the LODMUS init, with its
 * own descriptor table and two flag bytes the music driver does not set. */
static void init_gameplay_channel(BsRecomp *machine, uint32_t state)
{
    static const uint8_t clear_bytes[] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x39,
        0x35, 0x37, 0x36, 0x3b, 0x3c,
    };
    for (size_t i = 0; i < sizeof clear_bytes; i++)
        bs_recomp_write8(machine, state + clear_bytes[i], 0);
    bs_recomp_write8(machine, state + 0x3a, 1);
    bs_recomp_write8(machine, state + 0x3d, 0);
    bs_recomp_write16(machine, state + 0x28, 0);
    bs_recomp_write16(machine, state + 0x2a, 0);
    bs_recomp_write16(machine, state + 0x2c, 0);
    bs_recomp_write32(machine, state + 0x14, 0);
    bs_recomp_write32(machine, state + 0x18, 0);
    bs_recomp_write32(machine, state + 0x1c, 0);

    bs_recomp_write32(machine, state + 4, 0x25504);
    uint32_t source = bs_recomp_read32(machine, 0x25504);
    /* State +0 is the channel's Paula register base, so this seeds AUDxLC and
     * AUDxLEN and silences AUDxVOL. */
    uint32_t paula = bs_recomp_read32(machine, state);
    bs_recomp_write32(machine, paula, bs_recomp_read32(machine, source));
    bs_recomp_write16(machine, paula + 4,
                      bs_recomp_read16(machine, source + 4));
    bs_recomp_write16(machine, paula + 8, 0);

    uint32_t sequence = bs_recomp_read32(machine, state + 8);
    bs_recomp_write32(machine, state + 0x0c, sequence);
    bs_recomp_write32(machine, state + 0x10,
                      bs_recomp_read32(machine, sequence));
    bs_recomp_write16(machine, state + 0x20,
                      bs_recomp_read16(machine, sequence + 6));
    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
        (uint16_t)(bs_recomp_read16(machine, sequence + 0x0a) - 1);
    bs_recomp_write16(machine, state + 0x22, (uint16_t)machine->cpu.d[0]);
}

static const uint32_t bs_gameplay_channels[4] = {
    0x252a4, 0x252e2, 0x25320, 0x2535e,
};

/* LODGAM $24C6E via the $246F0 table entry.  Four overlays share that load
 * address, so the resident jump decides whether this is the audio system at
 * all.  It arms the CIA-B timer A interrupt that clocks the sequencer and
 * enables all four audio DMA channels. */
static int gameplay_audio_init(BsRecomp *machine)
{
    if (bs_recomp_read16(machine, 0x246f0) != 0x4ef9 ||
        bs_recomp_read32(machine, 0x246f2) != 0x00024c6e) {
        snprintf(machine->error, sizeof machine->error,
                 "the resident $246F0 entry is not LODGAM's AudioSystemInit");
        return BS_RECOMP_UNTRANSLATED;
    }
    for (size_t i = 0; i < 4; i++)
        init_gameplay_channel(machine, bs_gameplay_channels[i]);
    bs_recomp_write16(machine, 0xdff09a, 0x4000);
    bs_recomp_write8(machine, 0xbfde00, 0x00);
    bs_recomp_write8(machine, 0xbfd400, 0x00);
    bs_recomp_write8(machine, 0xbfd500, 0x31);
    bs_recomp_write8(machine, 0xbfdd00, 0x81);
    bs_recomp_write8(machine, 0xbfde00, 0x11);
    bs_recomp_write32(machine, 0x000008, 0x24f34);
    bs_recomp_write16(machine, 0xdff09a, 0xc000);
    bs_recomp_write16(machine, 0xdff096, 0x800f);
    /* The closing MOVE.W writes $800F: N set, ZVC clear. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x08);
    return BS_RECOMP_OK;
}

/* LODGAM $24DDE.  The $2471A table entry reaches it with track one.  The
 * request is only latched when no fade or pending change is already running. */
static void select_music(BsRecomp *machine, uint16_t track)
{
    const uint32_t state = 0x251f8;
    bs_recomp_write8(machine, state + 2, 0);
    if (bs_recomp_read8(machine, state + 3) != 0) {
        bs_recomp_write8(machine, state + 1,
                         bs_recomp_read8(machine, state + 3));
        bs_recomp_write8(machine, state + 3, 0);
    }
    uint8_t busy = bs_recomp_read8(machine, state + 4);
    if (busy != 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                                     (busy & 0x80 ? 0x08 : 0));
        return;
    }
    uint16_t pending = bs_recomp_read16(machine, state + 10);
    if (pending != 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                                     (pending & 0x8000 ? 0x08 : 0));
        return;
    }
    bs_recomp_write16(machine, state + 8, track);
    bs_recomp_write8(machine, state + 0, 1);
    if (track == 1) {
        /* The CMPI.W that takes the branch leaves Z set. */
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
        return;
    }
    bs_recomp_write16(machine, state + 10, 1);
    machine->cpu.sr &= 0xfff0;
}

static int run_new_game_sequence(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];
    machine->cpu.a[0] = 0xb2a0;
    clear32words(machine);
    bs_recomp_write32(machine, base + 13182, 0xfffffffe);
    bs_recomp_write8(machine, base - 28516, 0);
    machine->cpu.a[0] = 0x5e000;
    clear_longwords(machine, 0x0fff);
    initialise_game_state(machine);

    bs_recomp_write8(machine, base + 19942, 7);
    bs_recomp_write8(machine, base + 20038, 0x1f);
    bs_recomp_write8(machine, base - 2732, 0);
    if (bs_recomp_read8(machine, base - 12701) &
        bs_recomp_read8(machine, base - 12435))
        bs_recomp_write8(machine, base - 2732, 0xff);
    initialise_live_player(machine, 0x4e3c);
    initialise_live_player(machine, 0x4f46);
    expand_capacity_bars(machine, 0);
    expand_capacity_bars(machine, 1);
    draw_life_icons(machine, 0);
    draw_life_icons(machine, 1);
    bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xb256);

    /* $998-$A44.  There are 126 transition iterations.  The final sixteen
     * use the original taper table at $107C; DBF means each value is one
     * less than the number of pixel scrolls performed. */
    bs_recomp_write16(machine, base + 12892, 0xa490);
    bs_recomp_write16(machine, base + 12896, 0xa7b0);
    uint16_t component = 2;
    uint16_t intensity = 15;
    for (int outer = 125; outer >= 0; outer--) {
        bs_recomp_write8(machine, 0xb25c,
                         (uint8_t)(bs_recomp_read8(machine, 0xb25c) - 1));
        bs_recomp_write8(machine, 0xb260,
                         (uint8_t)(bs_recomp_read8(machine, 0xb260) + 1));
        uint16_t scroll_count = outer >= 16
            ? 7 : bs_recomp_read16(machine, 0x107c + (uint32_t)outer * 2);
        for (unsigned pass = 0; pass <= scroll_count; pass++)
            if (scroll_frame(machine)) return BS_RECOMP_ERROR;

        if (!(outer & 1)) {
            machine->cpu.a[0] = 0xb2a0;
            machine->cpu.a[3] = bs_recomp_read32(machine, base + 7224) + 12;
            for (int colour = 0; colour < 32; colour++) {
                machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                    bs_recomp_read16(machine, machine->cpu.a[3]);
                machine->cpu.a[3] += 2;
                machine->cpu.d[4] = (machine->cpu.d[4] & 0xffff0000) |
                    bs_recomp_read16(machine, machine->cpu.a[0]);
                machine->cpu.d[5] = 2;
                machine->cpu.d[6] = 0;
                machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) |
                                    intensity;
                machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                                    component;
                palette_step(machine);
                palette_step(machine);
                palette_step(machine);
                machine->cpu.a[0] += 4;
            }
            if (component == 0) {
                component = 2;
                if (intensity) intensity--;
            } else {
                component--;
            }
        }
        if (bs_recomp_read32(machine, base + 7214) < 0x49df0)
            bs_recomp_write32(machine, base + 7214, 0x49fc8);
    }

    bs_recomp_write16(machine, base + 7206, 0x00a0);
    bs_recomp_write32(machine, base + 7214, 0x49e20);
    /* $A56/$A5C: bring up the gameplay audio system and ask it for track one.
     * The CIA-B timer interrupt this arms is not dispatched yet, so the
     * sequencer does not advance and Paula stays silent. */
    if (gameplay_audio_init(machine)) return BS_RECOMP_ERROR;
    select_music(machine, 1);
    /* $A62-$A86.  Both entries into this sequence leave bit 7 of the audio
     * state byte set, so the two conditional LODGAM calls are live rather
     * than skipped as they were while audio was deferred. */
    if (bs_recomp_read8(machine, base - 26245) & 0x80) {
        bs_recomp_write8(machine, base - 26245,
                         (uint8_t)(bs_recomp_read8(machine, base - 26245) &
                                   0x7f));
        if (bs_recomp_read8(machine, base + 10941) == 0)
            /* $24708 -> $24D06: flip the sequencer's alternating bit. */
            bs_recomp_write8(machine, 0x251fd,
                (uint8_t)(bs_recomp_read8(machine, 0x251fd) ^ 0x01));
        if (bs_recomp_read8(machine, base + 10953) == 0)
            /* $24702 -> $24CFC: ask the timer interrupt to reset channels. */
            bs_recomp_write16(machine, 0x24e32, 1);
    }
    bs_recomp_write8(machine, base - 26244,
                     bs_recomp_read8(machine, base + 10941));
    bs_recomp_write8(machine, base - 26243,
                     bs_recomp_read8(machine, base + 10953));
    bs_recomp_write8(machine, 0xbfec01, 0);
    bs_recomp_write16(machine, machine->cpu.a[6] + 150, 0x8020);
    machine->cpu.pc = 0xaa0;
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
    return BS_RECOMP_OK;
}

int bs_recomp_start_new_game(BsRecomp *machine)
{
    if (!machine || machine->cpu.pc != 0x7d0 || machine->cpu.a[5] == 0) {
        if (machine)
            snprintf(machine->error, sizeof machine->error,
                     "new game requested outside the translated setup edge");
        return BS_RECOMP_ERROR;
    }
    const uint32_t base = machine->cpu.a[5];

    /* $842/$852: install the gameplay audio overlay and stage-zero scenery
     * data before any of the new-game state refers to them. */
    if (load_module(machine, 0x1aa0) || load_module(machine, 0x19f8))
        return BS_RECOMP_ERROR;
    bs_recomp_write8(machine, base - 26245, 0x81);
    bs_recomp_write8(machine, base - 26246, 0xff);

    return run_new_game_sequence(machine);
}

static void update_empty_entity_pool(BsRecomp *machine)
{
    bs_recomp_write32(machine, machine->cpu.a[5] - 8594, 0x5c2a);
    machine->cpu.a[4] = 0x2e040;
    machine->cpu.d[3] = 17;
    for (int i = 0; i < 18; i++) {
        uint8_t flags = bs_recomp_read8(machine, machine->cpu.a[4] + 31);
        bs_recomp_write8(machine, machine->cpu.a[4] + 31,
                         (uint8_t)(flags & ~(1u << 5)));
        machine->cpu.a[4] += 0x40;
        machine->cpu.d[3] = (machine->cpu.d[3] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[3] - 1);
    }
    machine->cpu.a[1] = bs_recomp_read32(machine, machine->cpu.a[5] - 8594);
    bs_recomp_write16(machine, machine->cpu.a[1], 0);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

static void set_dreg_word(uint32_t *reg, uint16_t value);
static void set_dreg_byte(uint32_t *reg, uint8_t value);

static int16_t signed_word_sub(uint16_t left, uint16_t right)
{
    return (int16_t)(uint16_t)(left - right);
}

static uint16_t absolute_word(int16_t value)
{
    return value < 0 ? (uint16_t)(-(int32_t)value) : (uint16_t)value;
}

static uint8_t next_wave_random(BsRecomp *machine);

static void write_render_record(BsRecomp *machine, uint32_t *cursor,
                                uint16_t x, uint16_t y, uint16_t height,
                                uint16_t width, uint32_t source,
                                uint16_t modulo, uint16_t x_offset)
{
    bs_recomp_write16(machine, *cursor + 0, x);
    bs_recomp_write16(machine, *cursor + 2, y);
    bs_recomp_write16(machine, *cursor + 4, height);
    bs_recomp_write16(machine, *cursor + 6, width);
    bs_recomp_write32(machine, *cursor + 8, source);
    bs_recomp_write16(machine, *cursor + 12, modulo);
    bs_recomp_write16(machine, *cursor + 14, x_offset);
    *cursor += 16;
}

/* Loader LAB_75E4/LAB_7608.  Keep this allocator independent of the map
 * scheduler: several live enemy state machines call the same routine. */
static uint32_t allocate_enemy_projectile(BsRecomp *machine,
                                          uint16_t relative_x,
                                          uint16_t relative_y,
                                          uint8_t type, uint32_t script)
{
    const uint32_t base = machine->cpu.a[5];
    if (bs_recomp_read8(machine, base - 16120) != 0 &&
        bs_recomp_read8(machine, base - 28516) == 0)
        return 0;

    uint32_t record = 0x2dff0;
    int slot = 11;
    while (slot >= 0 && bs_recomp_read16(machine, record) != 0) {
        record -= 0x50;
        slot--;
    }
    if (slot < 0) return 0;

    uint32_t descriptor = 0xcd7a + (uint32_t)type * 0x20;
    uint16_t x = relative_x;
    if (x >= 0x0320)
        x = (uint16_t)(x - 0x03e8 + 0x0100);
    else
        x = (uint16_t)(x + bs_recomp_read16(machine, base + 7204));
    uint16_t y = (uint16_t)(relative_y + 0x0100);

    bs_recomp_write8(machine, record + 31, type);
    bs_recomp_write16(machine, record + 0, x);
    bs_recomp_write16(machine, record + 2, 0);
    bs_recomp_write16(machine, record + 4, y);
    bs_recomp_write16(machine, record + 6, 0);
    bs_recomp_write32(machine, record + 8, 0);
    bs_recomp_write32(machine, record + 12, script);
    bs_recomp_write32(machine, record + 16,
                      bs_recomp_read32(machine, descriptor + 4));
    bs_recomp_write32(machine, record + 20,
                      bs_recomp_read32(machine, descriptor + 8));
    bs_recomp_write16(machine, record + 24,
                      bs_recomp_read16(machine, descriptor + 12));
    if (bs_recomp_read8(machine, base - 2732) == 0) {
        uint8_t armour = bs_recomp_read8(machine, record + 24);
        uint8_t reduction = (uint8_t)((armour + 1) >> 2);
        bs_recomp_write8(machine, record + 24,
                         (uint8_t)(armour - reduction));
    }
    bs_recomp_write8(machine, record + 26, 0);
    bs_recomp_write8(machine, record + 27,
                     bs_recomp_read8(machine, descriptor + 26));
    bs_recomp_write8(machine, record + 28,
                     bs_recomp_read8(machine, descriptor + 27));
    bs_recomp_write8(machine, record + 29, 0);
    bs_recomp_write8(machine, record + 30, 0);
    bs_recomp_write32(machine, record + 32,
                      bs_recomp_read32(machine, descriptor + 16));
    bs_recomp_write32(machine, record + 36,
                      bs_recomp_read32(machine, descriptor + 20));
    bs_recomp_write32(machine, record + 40, 0);
    uint16_t height = bs_recomp_read16(machine, descriptor);
    uint16_t width = bs_recomp_read16(machine, descriptor + 2);
    bs_recomp_write16(machine, record + 50, height);
    bs_recomp_write16(machine, record + 52, width);
    bs_recomp_write16(machine, record + 68,
                      (uint16_t)((width - 1) << 4));
    bs_recomp_write16(machine, record + 48,
                      (uint16_t)(0x30 - width * 2));
    uint16_t plane_stride = (uint16_t)((width * 2 - 2) * height);
    bs_recomp_write16(machine, record + 44, plane_stride);
    bs_recomp_write16(machine, record + 46,
                      (uint16_t)(plane_stride * 6));
    bs_recomp_write16(machine, record + 54,
                      bs_recomp_read16(machine, descriptor + 24));
    bs_recomp_write8(machine, record + 57, 0);
    bs_recomp_write8(machine, record + 62, 0);
    bs_recomp_write8(machine, record + 63, 0);
    bs_recomp_write32(machine, record + 64,
                      bs_recomp_read32(machine, descriptor + 28));
    machine->cpu.a[0] = record;
    machine->cpu.a[2] = descriptor;
    return record;
}

/* Covered native path through loader $5F34-$6FFA for active type-$20,
 * terrain-mode-zero objects.  This is the first live gameplay object path:
 * target selection, direction animation, clipping and render-list emission
 * are all direct C operations on the original object ABI. */
static int update_type20_mode0_pool(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];
    uint32_t render_cursor = 0x5c2a;
    bs_recomp_write32(machine, base - 8594, render_cursor);
    machine->cpu.a[4] = 0x2e040;
    machine->cpu.d[3] = 17;

    for (int slot = 0; slot < 18; slot++) {
        uint32_t object = machine->cpu.a[4];
        uint8_t flags = bs_recomp_read8(machine, object + 31);
        flags &= (uint8_t)~0x20;
        bs_recomp_write8(machine, object + 31, flags);
        if (bs_recomp_read16(machine, object) != 0) {
            flags ^= 0x08;
            bs_recomp_write8(machine, object + 31, flags);
            uint8_t type = bs_recomp_read8(machine, object + 17);
            if (bs_recomp_read16(machine, base + 7228) != 0) {
                snprintf(machine->error, sizeof machine->error,
                         "untranslated active object path: slot=%d type=$%02x "
                         "mode=%u state=%u limit=%u armour=%u status=%u",
                         slot, bs_recomp_read8(machine, object + 17),
                         bs_recomp_read16(machine, base + 7228),
                         bs_recomp_read8(machine, object + 25),
                         bs_recomp_read8(machine, object + 19),
                         bs_recomp_read8(machine, object + 24),
                         bs_recomp_read8(machine, object + 30));
                return BS_RECOMP_UNTRANSLATED;
            }

            uint16_t y = (uint16_t)(bs_recomp_read16(machine, object + 2) +
                                     bs_recomp_read16(machine, base + 7222));
            bs_recomp_write16(machine, object + 2, y);
            uint16_t x = bs_recomp_read16(machine, object);
            uint16_t d1 = 0, d2 = 0;
            int skip_object_behaviour = 0;

            /* LAB_686C-$699C: shared damage mailbox, hit-flash and death
             * animation for object types $20 and above.  Collision is run
             * later in the frame, so byte 24 is deliberately consumed here
             * on the following update just like the original. */
            if (type >= 0x20) {
                uint8_t state = bs_recomp_read8(machine, object + 25);
                uint8_t live_limit = bs_recomp_read8(machine, object + 19);
                if (state >= live_limit) {
                    uint8_t final_state = bs_recomp_read8(machine,
                                                           object + 33);
                    if (state < final_state &&
                        !(bs_recomp_read8(machine, base - 28551) & 2)) {
                        state++;
                        bs_recomp_write8(machine, object + 25, state);
                    }
                    skip_object_behaviour = 1;
                } else {
                    uint8_t damage = bs_recomp_read8(machine, object + 24);
                    if (damage != 0) {
                        bs_recomp_write8(machine, object + 24, 0);
                        if (type == 0x22) {
                            bs_recomp_write8(machine, object + 30,
                                (uint8_t)(bs_recomp_read8(machine,
                                                          object + 30) + 1));
                        } else {
                            bs_recomp_write8(machine, object + 30, 3);
                        }
                        int16_t health = (int8_t)bs_recomp_read8(
                            machine, object + 28);
                        health -= damage;
                        bs_recomp_write8(machine, object + 28,
                                         (uint8_t)health);
                        if (health < 0) {
                            bs_recomp_write8(machine, object + 31,
                                bs_recomp_read8(machine, object + 31) | 0x04);
                            bs_recomp_write8(machine, object + 25, live_limit);
                            skip_object_behaviour = 1;
                        }
                    }

                    uint8_t status = bs_recomp_read8(machine, object + 30);
                    if (!skip_object_behaviour && status != 0) {
                        status--;
                        bs_recomp_write8(machine, object + 30, status);
                        if (type == 0x22) {
                            bs_recomp_write8(machine, object + 25,
                                             status ? (uint8_t)(6 - status)
                                                    : 0);
                        } else {
                            bs_recomp_write8(machine, object + 25,
                                bs_recomp_read8(machine,
                                    object + ((status & 1) ? 34 : 35)));
                        }
                        skip_object_behaviour = 1;
                    }
                }
            }

            if (skip_object_behaviour) {
                /* Rendering/clipping below still uses the selected flash or
                 * death frame, matching the common LAB_6EE4 tail. */
            } else if (type == 0x01) {
                /* LAB_6170: opening ground launcher.  It rises through nine
                 * animation states, then emits the type-$07 ground shot once
                 * live play (rather than attract playback) is active. */
                if (bs_recomp_read8(machine, base - 28551) & 2) {
                    uint8_t state = bs_recomp_read8(machine, object + 25);
                    if (state == 0) {
                        if (y >= 0x0100) {
                            uint8_t timer = (uint8_t)(
                                bs_recomp_read8(machine, object + 43) - 1);
                            bs_recomp_write8(machine, object + 43, timer);
                            if (timer == 0)
                                bs_recomp_write8(machine, object + 25, 1);
                        }
                    } else if (state < 9) {
                        bs_recomp_write8(machine, object + 25,
                                         (uint8_t)(state + 1));
                    } else if (bs_recomp_read8(machine, object + 36) == 0) {
                        bs_recomp_write8(machine, object + 36, 0xff);
                        if (bs_recomp_read8(machine, base - 28516) == 0) {
                            uint32_t saved_d3 = machine->cpu.d[3];
                            allocate_enemy_projectile(machine,
                                (uint16_t)(x -
                                    bs_recomp_read16(machine, base + 7204) +
                                    0x000e),
                                (uint16_t)(y - 0x00e1), 7,
                                machine->cpu.d[4]);
                            machine->cpu.d[3] = saved_d3;
                        }
                    }
                }
            } else if (type == 0x20) {
            bs_recomp_write16(machine, base - 14398,
                              (uint16_t)(x + 0x13));
            bs_recomp_write16(machine, base - 14396,
                              (uint16_t)(y + 0x10));

            set_dreg_word(&machine->cpu.d[0],
                          (uint16_t)machine->cpu.d[3]);
            d1 = absolute_word(signed_word_sub(
                (uint16_t)(bs_recomp_read16(machine, base - 12736) + 0x0c),
                bs_recomp_read16(machine, base - 14398)));
            d2 = absolute_word(signed_word_sub(
                (uint16_t)(bs_recomp_read16(machine, base - 12734) + 0x10),
                bs_recomp_read16(machine, base - 14396)));
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | d1;
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) | d2;
            set_dreg_word(&machine->cpu.d[5], (uint16_t)(d1 + d2));
            uint16_t d3 = absolute_word(signed_word_sub(
                (uint16_t)(bs_recomp_read16(machine, base - 12470) + 0x0c),
                bs_recomp_read16(machine, base - 14398)));
            uint16_t d4 = absolute_word(signed_word_sub(
                (uint16_t)(bs_recomp_read16(machine, base - 12468) + 0x10),
                bs_recomp_read16(machine, base - 14396)));
            set_dreg_word(&machine->cpu.d[3], d3);
            set_dreg_word(&machine->cpu.d[4], d4);
            set_dreg_word(&machine->cpu.d[6], (uint16_t)(d3 + d4));
            set_dreg_word(&machine->cpu.d[3],
                          (uint16_t)machine->cpu.d[0]);

            bs_recomp_write8(machine, object + 42, 0);
            if ((uint16_t)machine->cpu.d[5] <
                (uint16_t)machine->cpu.d[6]) {
                d1 = bs_recomp_read16(machine, base - 12736);
                d2 = bs_recomp_read16(machine, base - 12734);
                bs_recomp_write8(machine, object + 42, 1);
            } else {
                d1 = bs_recomp_read16(machine, base - 12470);
                d2 = bs_recomp_read16(machine, base - 12468);
                bs_recomp_write8(machine, object + 42, 2);
            }
            machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) | d1;
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) | d2;

            set_dreg_byte(&machine->cpu.d[5], 0);
            d1 = (uint16_t)(d1 - 6 - x);
            if (d1 == 0) d1++;
            if (d1 & 0x8000) machine->cpu.d[5] = 4;
            d2 = (uint16_t)(-(int16_t)d2 + y + 6);
            int32_t dividend = (int16_t)d2 * 64;
            d1 |= 1;
            int16_t quotient = (int16_t)(dividend / (int16_t)d1);
            set_dreg_word(&machine->cpu.d[1], d1);
            set_dreg_word(&machine->cpu.d[2], (uint16_t)quotient);

            machine->cpu.d[4] = 0;
            if (quotient < 0x009a) machine->cpu.d[4] = 1;
            if (quotient < 0x001a) machine->cpu.d[4] = 2;
            if (quotient < -26) machine->cpu.d[4] = 3;
            if (quotient < -154) machine->cpu.d[4] = 4;
            set_dreg_byte(&machine->cpu.d[4],
                          (uint8_t)(machine->cpu.d[4] +
                                    (uint8_t)machine->cpu.d[5]));
            set_dreg_word(&machine->cpu.d[4],
                          (uint16_t)machine->cpu.d[4] & 7);
            set_dreg_word(&machine->cpu.d[5],
                          bs_recomp_read8(machine, object + 25));
            set_dreg_word(&machine->cpu.d[5],
                          (uint16_t)(-(int16_t)machine->cpu.d[5] +
                                     (uint16_t)machine->cpu.d[4]));
            int16_t direction_delta = (int16_t)machine->cpu.d[5];
            uint8_t delta = bs_recomp_read8(machine,
                                             (uint32_t)(0x5f02 +
                                                        direction_delta));
            if (delta != 0) {
                uint8_t state = (uint8_t)(
                    bs_recomp_read8(machine, object + 25) + delta);
                state &= 7;
                bs_recomp_write8(machine, object + 25, state);
                bs_recomp_write8(machine, object + 35, state);
                bs_recomp_write8(machine, object + 36,
                                  bs_recomp_read8(machine, base - 8414));
                if (bs_recomp_read8(machine, object + 37) < 2)
                    bs_recomp_write8(machine, object + 37, 1);
            }
            } else if (type == 0x25 || type == 0x26) {
                uint8_t timer = bs_recomp_read8(machine, object + 36);
                if (!(timer & 0x80)) {
                    if (timer == 0) {
                        uint32_t saved_d3 = machine->cpu.d[3];
                        uint8_t random = next_wave_random(machine);
                        machine->cpu.d[1] = random;
                        machine->cpu.d[3] = saved_d3;
                        if (random & 0x40) {
                            bs_recomp_write8(machine, object + 36, 0xff);
                        } else {
                            timer = (uint8_t)((random & 0x3f) + 0x40);
                            bs_recomp_write8(machine, object + 36, timer);
                        }
                    }
                    timer = bs_recomp_read8(machine, object + 36);
                    if (!(timer & 0x80)) {
                        timer--;
                        bs_recomp_write8(machine, object + 36, timer);
                        if (timer == 0) {
                            bs_recomp_write8(machine, object + 36, 0xff);
                            bs_recomp_write8(machine, object + 31,
                                bs_recomp_read8(machine, object + 31) | 0x20);
                            uint32_t impact = bs_recomp_read32(machine, object);
                            impact += type == 0x25
                                ? UINT32_C(0x00020007)
                                : UINT32_C(0x00140009);
                            bs_recomp_write32(machine, object + 38, impact);
                        }
                    }
                }
            } else if (type == 0x22) {
                uint8_t animation = (uint8_t)(
                    bs_recomp_read8(machine, object + 43) - 1);
                bs_recomp_write8(machine, object + 43, animation);
                if (animation == 0) {
                    bs_recomp_write8(machine, object + 43, 8);
                    bs_recomp_write8(machine, object + 25,
                        bs_recomp_read8(machine, object + 25) ? 0 : 1);
                }
            } else if (type == 0x21) {
                /* Type $21 has no mode-zero behaviour before clipping. */
            } else if (type == 0x27) {
                /* $6D44-$6DCA.  The object watches a 32-pixel box around
                 * itself and counts consecutive frames in which every live
                 * player sits inside it; the tenth toggles the state byte at
                 * -4100.  Any live player outside, or no live player at all,
                 * resets the count. */
                if ((int16_t)bs_recomp_read16(machine, object + 2) >= 0x01fc)
                    bs_recomp_write16(machine, base + 7230, 0);
                bs_recomp_write8(machine, object + 31,
                    (uint8_t)(bs_recomp_read8(machine, object + 31) | 0x04));
                uint8_t phase =
                    (uint8_t)(bs_recomp_read8(machine, base - 28551) & 0x1f);
                bs_recomp_write8(machine, object + 25,
                                 (int8_t)phase < 8 ? 0 : 1);

                uint16_t left = (uint16_t)(bs_recomp_read16(machine, object) -
                                           0x10);
                uint16_t right = (uint16_t)(bs_recomp_read16(machine, object) +
                                            0x10);
                uint16_t top = (uint16_t)(
                    bs_recomp_read16(machine, object + 2) - 0x10);
                uint16_t bottom = (uint16_t)(
                    bs_recomp_read16(machine, object + 2) + 0x10);
                uint8_t tally = 0;
                /* $6DCE, run for each player record in turn. */
                static const uint32_t watched[] = {0x4e3c, 0x4f46};
                for (unsigned index = 0; index < 2; index++) {
                    uint32_t player = watched[index];
                    uint8_t life = bs_recomp_read8(machine, player + 38);
                    if (life >= 0xaf || life == 0x64) continue;
                    tally = (uint8_t)(tally + 1);
                    int16_t px = (int16_t)bs_recomp_read16(machine,
                                                            player + 4);
                    int16_t py = (int16_t)bs_recomp_read16(machine,
                                                            player + 6);
                    /* The last edge is a strict compare, unlike the other
                     * three. */
                    int inside = (int16_t)left <= px && (int16_t)right >= px &&
                                 (int16_t)top <= py && (int16_t)bottom > py;
                    if (!inside) tally |= 0x80;
                }
                if ((tally & 0x80) || tally == 0) {
                    bs_recomp_write8(machine, base - 8397, 0);
                } else {
                    uint8_t held = (uint8_t)(
                        bs_recomp_read8(machine, base - 8397) + 1);
                    bs_recomp_write8(machine, base - 8397, held);
                    if ((int8_t)held >= 0x0a)
                        bs_recomp_write8(machine, base - 4100,
                            (uint8_t)~bs_recomp_read8(machine, base - 4100));
                }
                set_dreg_word(&machine->cpu.d[1], left);
                set_dreg_word(&machine->cpu.d[2], top);
                set_dreg_word(&machine->cpu.d[5], right);
                set_dreg_word(&machine->cpu.d[6], bottom);
                set_dreg_byte(&machine->cpu.d[4], tally);
            } else {
                snprintf(machine->error, sizeof machine->error,
                         "untranslated active object path: slot=%d type=$%02x "
                         "mode=%u state=%u",
                         slot, type, bs_recomp_read16(machine, base + 7228),
                         bs_recomp_read8(machine, object + 25));
                return BS_RECOMP_UNTRANSLATED;
            }

            x = bs_recomp_read16(machine, object);
            y = bs_recomp_read16(machine, object + 2);
            uint16_t height = bs_recomp_read16(machine, object + 6);
            if (y >= 0x0200 || (uint16_t)(y + height) < 0x0100) {
                bs_recomp_write16(machine, object, 0);
                goto next_object;
            }
            uint16_t x2 = (uint16_t)(x +
                                      bs_recomp_read16(machine, object + 20));
            bs_recomp_write16(machine, object + 48, x2);
            x2 = (uint16_t)(x2 +
                            bs_recomp_read16(machine, object + 22));
            bs_recomp_write16(machine, object + 50, x2);
            bs_recomp_write16(machine, object + 52, y);
            bs_recomp_write16(machine, object + 54,
                              (uint16_t)(y + height));

            if (y < 0x0200 && (uint16_t)(y + height) > 0x0100) {
                uint16_t state = bs_recomp_read8(machine, object + 25);
                machine->cpu.d[1] = (uint32_t)state *
                                     bs_recomp_read16(machine, object + 26);
                machine->cpu.a[2] = bs_recomp_read32(machine, object + 12) +
                                    machine->cpu.d[1];
                d1 = x;
                d2 = y;
                uint16_t visible = height;
                uint16_t bottom = (uint16_t)(d2 + visible);
                if (bottom > 0x0200) visible = (uint16_t)(0x0200 - d2);
                if (d2 < 0x0100) {
                    uint16_t clipped = (uint16_t)(0x0100 - d2);
                    visible = (uint16_t)(height - clipped);
                    machine->cpu.a[2] +=
                        (uint32_t)clipped *
                        bs_recomp_read16(machine, object + 8) * 2;
                    d2 = 0x0100;
                }
                bottom = (uint16_t)(d2 + visible);
                if (d2 < 0x0180 && bottom > 0x0180) {
                    uint16_t first = (uint16_t)(0x0180 - d2);
                    write_render_record(machine, &render_cursor, d1, d2,
                        first, bs_recomp_read16(machine, object + 8),
                        machine->cpu.a[2],
                        bs_recomp_read16(machine, object + 10),
                        bs_recomp_read16(machine, object + 4));
                    machine->cpu.a[2] +=
                        (uint32_t)first *
                        bs_recomp_read16(machine, object + 8) * 2;
                    write_render_record(machine, &render_cursor, d1, 0x0180,
                        (uint16_t)(bottom - 0x0180),
                        bs_recomp_read16(machine, object + 8),
                        machine->cpu.a[2],
                        bs_recomp_read16(machine, object + 10),
                        bs_recomp_read16(machine, object + 4));
                } else {
                    write_render_record(machine, &render_cursor, d1, d2,
                        visible, bs_recomp_read16(machine, object + 8),
                        machine->cpu.a[2],
                        bs_recomp_read16(machine, object + 10),
                        bs_recomp_read16(machine, object + 4));
                }
                set_dreg_word(&machine->cpu.d[0], visible);
                set_dreg_word(&machine->cpu.d[1], d1);
                set_dreg_word(&machine->cpu.d[2], d2);
                set_dreg_word(&machine->cpu.d[4], bottom);
                machine->cpu.a[1] = render_cursor;
                bs_recomp_write32(machine, base - 8594, render_cursor);
            }
        }

next_object:
        machine->cpu.a[4] += 0x40;
        set_dreg_word(&machine->cpu.d[3],
                      (uint16_t)(machine->cpu.d[3] - 1));
    }
    machine->cpu.a[1] = bs_recomp_read32(machine, base - 8594);
    bs_recomp_write16(machine, machine->cpu.a[1], 0);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
    return BS_RECOMP_OK;
}

static void draw_object_render_list(BsRecomp *machine, int lower_half)
{
    const uint32_t base = machine->cpu.a[5];
    machine->cpu.a[4] = 0x5c2a;
    bs_recomp_write16(machine, machine->cpu.a[6] + 66, 0);
    bs_recomp_write16(machine, machine->cpu.a[6] + 64, 0x09f0);
    bs_recomp_write16(machine, machine->cpu.a[6] + 100, 0);
    bs_recomp_write32(machine, machine->cpu.a[6] + 68, 0xffffffff);
    while (bs_recomp_read16(machine, machine->cpu.a[4]) != 0) {
        uint16_t screen_y = bs_recomp_read16(machine,
                                              machine->cpu.a[4] + 2);
        if ((!lower_half && screen_y < 0x0180) ||
            (lower_half && screen_y >= 0x0180)) {
            uint16_t x = (uint16_t)(
                bs_recomp_read16(machine, machine->cpu.a[4]) - 0x0100);
            uint16_t y = (uint16_t)(screen_y - 0x0100);
            uint16_t height = bs_recomp_read16(machine,
                                                machine->cpu.a[4] + 4);
            uint16_t width = bs_recomp_read16(machine,
                                               machine->cpu.a[4] + 6);
            uint32_t source = bs_recomp_read32(machine,
                                                machine->cpu.a[4] + 8);
            uint16_t source_plane_stride = bs_recomp_read16(
                machine, machine->cpu.a[4] + 12);
            uint16_t destination_modulo = bs_recomp_read16(
                machine, machine->cpu.a[4] + 14);
            bs_recomp_write16(machine, machine->cpu.a[6] + 102,
                              destination_modulo);
            set_dreg_word(&machine->cpu.d[1], x);
            machine->cpu.d[2] = (uint32_t)y * 0x30;
            set_dreg_word(&machine->cpu.d[1],
                          (uint16_t)machine->cpu.d[1] >> 3);
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                                (uint16_t)(machine->cpu.d[2] +
                                           (uint16_t)machine->cpu.d[1]);
            uint32_t destination = bs_recomp_read32(machine, base + 7208) +
                                   (uint16_t)machine->cpu.d[2];
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                (uint16_t)((height << 6) + width);
            set_dreg_word(&machine->cpu.d[5], source_plane_stride);
            set_dreg_word(&machine->cpu.d[6], 0x6000);
            machine->cpu.d[7] = 4;
            for (int plane = 0; plane < 5; plane++) {
                machine->cpu.a[2] = source;
                machine->cpu.a[1] = destination;
                for (uint16_t row = 0; row < height; row++) {
                    for (uint16_t word = 0; word < width; word++)
                        bs_recomp_write16(machine,
                            destination + (uint32_t)row * 0x30 + word * 2,
                            bs_recomp_read16(machine,
                                source + (uint32_t)row * width * 2 + word * 2));
                }
                source += source_plane_stride;
                destination += 0x6000;
                machine->cpu.a[2] = source;
                machine->cpu.a[1] = destination;
                set_dreg_word(&machine->cpu.d[7],
                              (uint16_t)(machine->cpu.d[7] - 1));
            }
        }
        machine->cpu.a[4] += 0x10;
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

/* Loader $5BD2/$5BFE -> $99E2.  Dynamic BOB/projectile draws affect only
 * the visible copy of the 256-line terrain ring.  The other $3000-byte half
 * remains clean; the alternating $7770/$7834 rectangle list copies that
 * clean twin back over last frame's dirty rectangles before anything new is
 * drawn.  Omitting this pass permanently burns enemies into the map. */
static void copy_clean_ring_rectangle(BsRecomp *machine, uint32_t record)
{
    const uint32_t base = machine->cpu.a[5];
    uint16_t x = (uint16_t)(bs_recomp_read16(machine, record) - 0x100);
    uint16_t y = (uint16_t)(bs_recomp_read16(machine, record + 2) - 0x100);
    uint16_t height = bs_recomp_read16(machine, record + 4);
    uint16_t width_words = bs_recomp_read16(machine, record + 6);
    if (!height || !width_words || width_words > 24 || y >= 256) return;

    uint32_t destination = bs_recomp_read32(machine, base - 9266) +
                           (uint32_t)y * 0x30 + (x >> 3);
    uint32_t source = destination;
    uint16_t old_ring_row = bs_recomp_read16(machine, base - 9268);
    uint16_t first_height = height;
    uint16_t second_height = 0;
    uint32_t second_destination = 0, second_source = 0;
    uint32_t bottom = (uint32_t)y + old_ring_row + height;
    if (bottom < 0x100) {
        source += 0x3000;
    } else if ((uint32_t)y + old_ring_row >= 0x100) {
        source -= 0x3000;
    } else {
        first_height = (uint16_t)(0x100 - y - old_ring_row);
        if (first_height > height) first_height = height;
        source += 0x3000;
        second_height = (uint16_t)(height - first_height);
        second_destination = 0x65000 + (x >> 3);
        second_source = second_destination - 0x3000;
    }

    const uint16_t spans[2] = {first_height, second_height};
    const uint32_t destinations[2] = {destination, second_destination};
    const uint32_t sources[2] = {source, second_source};
    for (unsigned span = 0; span < 2; span++) {
        for (unsigned plane = 0; plane < 5 && spans[span]; plane++) {
            uint32_t plane_source = sources[span] + plane * 0x6000;
            uint32_t plane_destination = destinations[span] +
                                         plane * 0x6000;
            for (uint16_t row = 0; row < spans[span]; row++) {
                for (uint16_t byte = 0; byte < width_words * 2u; byte++)
                    bs_recomp_write8(machine,
                        plane_destination + (uint32_t)row * 0x30 + byte,
                        bs_recomp_read8(machine,
                            plane_source + (uint32_t)row * 0x30 + byte));
            }
        }
    }
    bs_recomp_write16(machine, machine->cpu.a[6] + 100,
                      (uint16_t)(0x30 - width_words * 2u));
    bs_recomp_write16(machine, machine->cpu.a[6] + 102,
                      (uint16_t)(0x30 - width_words * 2u));
}

static void restore_previous_draws(BsRecomp *machine, int lower_half)
{
    const uint32_t base = machine->cpu.a[5];
    uint32_t cursor = bs_recomp_read32(machine, base - 1796);
    machine->cpu.a[4] = cursor;
    for (unsigned guard = 0; guard < 128; guard++, cursor += 8) {
        if (bs_recomp_read16(machine, cursor) == 0) break;
        uint16_t y = bs_recomp_read16(machine, cursor + 2);
        if ((!lower_half && y < 0x0180) ||
            (lower_half && y >= 0x0180))
            copy_clean_ring_rectangle(machine, cursor);
        machine->cpu.a[4] = cursor + 8;
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

/* LAB_55D8's five-plane cookie-cut blit, expressed as direct bitmap writes.
 * A3 is the shared one-bit cookie mask, A2 is the first colour plane, and
 * record+44 is the distance between colour planes.  The original blitter
 * minterm $CA is: mask ? source : destination. */
static void draw_projectile_cookie_cut(BsRecomp *machine,
                                       uint32_t projectile,
                                       uint32_t data_source,
                                       uint32_t mask_source)
{
    const uint32_t base = machine->cpu.a[5];
    int x = (int16_t)bs_recomp_read16(machine, projectile) - 0x100;
    int y = (int16_t)bs_recomp_read16(machine, projectile + 4) - 0x100;
    unsigned height = bs_recomp_read16(machine, projectile + 50);
    unsigned width_pixels = bs_recomp_read16(machine, projectile + 68);
    unsigned plane_stride = bs_recomp_read16(machine, projectile + 44);
    if (height == 0 || width_pixels == 0 || plane_stride == 0) return;

    unsigned source_row_bytes = (width_pixels + 7) >> 3;
    uint32_t playfield = bs_recomp_read32(machine, base + 7208);
    for (unsigned row = 0; row < height; row++) {
        int destination_y = y + (int)row;
        if (destination_y < 0 || destination_y >= 256) continue;
        for (unsigned column = 0; column < width_pixels; column++) {
            int destination_x = x + (int)column;
            if (destination_x < 0 || destination_x >= 384) continue;
            uint32_t source_byte = (uint32_t)row * source_row_bytes +
                                   (column >> 3);
            uint8_t bit = (uint8_t)(0x80u >> (column & 7));
            if (!(bs_recomp_read8(machine, mask_source + source_byte) & bit))
                continue;
            uint32_t destination_byte =
                (uint32_t)destination_y * 0x30 +
                (unsigned)destination_x / 8;
            uint8_t destination_bit =
                (uint8_t)(0x80u >> ((unsigned)destination_x & 7));
            for (unsigned plane = 0; plane < 5; plane++) {
                uint8_t value = bs_recomp_read8(machine,
                    data_source + plane * plane_stride + source_byte);
                uint32_t address = playfield + plane * 0x6000 +
                                   destination_byte;
                uint8_t old = bs_recomp_read8(machine, address);
                bs_recomp_write8(machine, address,
                    (value & bit) ? (uint8_t)(old | destination_bit)
                                  : (uint8_t)(old & ~destination_bit));
            }
        }
    }
}

static int update_enemy_projectile_pool(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];
    machine->cpu.a[4] = 0x2dc80;
    machine->cpu.d[0] = 11;
    for (int slot = 0; slot < 12; slot++) {
        uint32_t projectile = machine->cpu.a[4];
        if (bs_recomp_read16(machine, projectile) != 0) {
            uint16_t y = bs_recomp_read16(machine, projectile + 4);
            int selected_half = y <= 0x0146
                ? bs_recomp_read8(machine, base - 1792) == 0
                : bs_recomp_read8(machine, base - 1792) != 0;
            uint8_t type = bs_recomp_read8(machine, projectile + 31);
            int selected_pass = bs_recomp_read8(machine, base - 1791) != 0 ||
                                type == 0x0d || type == 7 || type == 3;
            uint8_t flags = bs_recomp_read8(machine, projectile + 30);
            if (selected_half && selected_pass && !(flags & 0x40)) {
                flags |= 0x40;
                bs_recomp_write8(machine, projectile + 30, flags);
                if (type != 0 && type != 1 && type != 3 && type != 4 &&
                    type != 6 && type != 7 && type != 8) {
                    snprintf(machine->error, sizeof machine->error,
                             "untranslated live projectile type $%02x at "
                             "$%06x", type, projectile);
                    return BS_RECOMP_UNTRANSLATED;
                }

                uint16_t height = bs_recomp_read16(machine,
                                                    projectile + 50);
                if (type == 0) {
                    /* LAB_9752: script-driven stage projectile.  A zero
                     * duration fetches the next velocity command; $FF/0
                     * chains to another script and $FF/1 terminates it. */
                    if (bs_recomp_read8(machine, projectile + 8) == 0) {
                        machine->cpu.a[1] =
                            bs_recomp_read32(machine, projectile + 12);
                        for (int guard = 0; guard < 256; guard++) {
                            uint8_t command = bs_recomp_read8(
                                machine, machine->cpu.a[1]);
                            if (command != 0xff) break;
                            uint8_t operation = bs_recomp_read8(
                                machine, machine->cpu.a[1] + 1);
                            if (operation == 1) {
                                bs_recomp_write16(machine, projectile, 0);
                                goto next_projectile;
                            }
                            if (operation != 0) break;
                            machine->cpu.a[1] = bs_recomp_read32(
                                machine, machine->cpu.a[1] + 2);
                            if (guard == 255) {
                                snprintf(machine->error,
                                         sizeof machine->error,
                                         "projectile script chain did not "
                                         "terminate at $%06x", projectile);
                                return BS_RECOMP_ERROR;
                            }
                        }
                        bs_recomp_write32(machine, projectile + 8,
                            bs_recomp_read32(machine, machine->cpu.a[1]));
                        machine->cpu.a[1] += 4;
                        bs_recomp_write32(machine, projectile + 12,
                                          machine->cpu.a[1]);
                    }

                    uint8_t direction = bs_recomp_read8(machine,
                                                         projectile + 9);
                    uint32_t vector = 0xccf2 +
                                      (uint32_t)(direction & 0x1f) * 4;
                    int32_t scale = (int32_t)((direction & 0xe0) >> 2) + 8;
                    int32_t velocity_x =
                        (int16_t)bs_recomp_read16(machine, vector) * scale;
                    int32_t velocity_y =
                        (int16_t)bs_recomp_read16(machine, vector + 2) * scale;
                    bs_recomp_write32(machine, projectile,
                        bs_recomp_read32(machine, projectile) +
                            (uint32_t)velocity_x);
                    bs_recomp_write32(machine, projectile + 4,
                        bs_recomp_read32(machine, projectile + 4) +
                            (uint32_t)velocity_y);

                    uint8_t turn_timer = bs_recomp_read8(machine,
                                                          projectile + 11);
                    if (turn_timer != 0) {
                        turn_timer--;
                        bs_recomp_write8(machine, projectile + 11,
                                         turn_timer);
                        if (turn_timer == 0) {
                            bs_recomp_write8(machine, projectile + 9,
                                (uint8_t)(direction + bs_recomp_read8(
                                    machine, projectile + 10)));
                            uint32_t script = bs_recomp_read32(
                                machine, projectile + 12);
                            bs_recomp_write8(machine, projectile + 11,
                                bs_recomp_read8(machine, script - 1));
                        }
                    }
                    bs_recomp_write8(machine, projectile + 8,
                        (uint8_t)(bs_recomp_read8(machine,
                                                  projectile + 8) - 1));
                    bs_recomp_write8(machine, projectile + 63,
                                     (direction & 0x1f) >> 1);
                    bs_recomp_write32(machine, projectile + 36, 0x17500);
                    if (bs_recomp_read8(machine, projectile + 62) != 0) {
                        bs_recomp_write8(machine, projectile + 29, 8);
                        bs_recomp_write8(machine, projectile + 30,
                            (uint8_t)((bs_recomp_read8(machine,
                                projectile + 30) & ~0x20) | 0x80));
                        bs_recomp_write32(machine, projectile + 36, 0x11090);
                        bs_recomp_write32(machine, projectile + 32, 0x11310);
                        bs_recomp_write16(machine, projectile + 50, 0x20);
                        height = 0x20;
                    }
                    uint16_t frame_stride = bs_recomp_read16(
                        machine, projectile + 46);
                    machine->cpu.a[2] = bs_recomp_read32(
                        machine, projectile + 36) +
                        (uint32_t)bs_recomp_read8(machine,
                            projectile + 63) * frame_stride;
                    machine->cpu.a[3] = machine->cpu.a[2] + frame_stride -
                        bs_recomp_read16(machine, projectile + 44);
                } else if (type == 1) {
                    /* LAB_95D8: the common aimed enemy shot.  Coordinates
                     * and velocities remain 16.16 fixed point, exactly as
                     * they are stored in the original 80-byte record. */
                    machine->cpu.a[2] =
                        bs_recomp_read16(machine, base + 7228)
                            ? 0x792e : 0x7916;
                    y = bs_recomp_read16(machine, projectile + 4);
                    flags = bs_recomp_read8(machine, projectile + 30);
                    if (!(flags & 0x01) && y >= 0x0100) {
                        flags |= 0x21;
                        bs_recomp_write16(machine, projectile + 58,
                            (uint16_t)(bs_recomp_read16(machine,
                                                       projectile) + 11));
                        bs_recomp_write16(machine, projectile + 60,
                            (uint16_t)(y + 32));
                    } else if (!(flags & 0x02) && y >= 0x01a0) {
                        flags |= 0x22;
                        bs_recomp_write16(machine, projectile + 58,
                            (uint16_t)(bs_recomp_read16(machine,
                                                       projectile) + 11));
                        bs_recomp_write16(machine, projectile + 60,
                            (uint16_t)(y + 32));
                    }
                    bs_recomp_write8(machine, projectile + 30, flags);

                    int32_t velocity = (int32_t)bs_recomp_read32(
                        machine, projectile + 12);
                    uint16_t aimed_y = (uint16_t)(y - 0x1e);
                    uint16_t player_one_y = bs_recomp_read16(
                        machine, base - 12734);
                    uint16_t player_two_y = bs_recomp_read16(
                        machine, base - 12468);
                    if (aimed_y >= player_one_y &&
                        aimed_y >= player_two_y) {
                        if (velocity != 0) {
                            int32_t brake = 0x800;
                            if (velocity > 0) brake = -brake;
                            velocity += brake;
                        }
                    } else {
                        uint16_t x = bs_recomp_read16(machine, projectile);
                        uint16_t player_one_x = bs_recomp_read16(
                            machine, base - 12736);
                        uint16_t player_two_x = bs_recomp_read16(
                            machine, base - 12470);
                        uint16_t distance_one = player_one_x >= x
                            ? (uint16_t)(player_one_x - x)
                            : (uint16_t)(x - player_one_x);
                        uint16_t distance_two = player_two_x >= x
                            ? (uint16_t)(player_two_x - x)
                            : (uint16_t)(x - player_two_x);
                        uint16_t target_x;
                        if (distance_one >= distance_two) {
                            target_x = player_two_x;
                            if (player_two_y < player_one_y)
                                target_x = player_one_x;
                        } else {
                            target_x = player_one_x;
                            if (player_one_y < player_two_y)
                                target_x = player_two_x;
                        }
                        int32_t maximum = (int32_t)bs_recomp_read32(
                            machine, machine->cpu.a[2] + 16);
                        int32_t acceleration = (int32_t)bs_recomp_read32(
                            machine, machine->cpu.a[2] + 20);
                        if (target_x >= x) {
                            if (velocity != maximum)
                                velocity += acceleration;
                        } else {
                            maximum = -maximum;
                            if (velocity != maximum)
                                velocity -= acceleration;
                        }
                    }
                    bs_recomp_write32(machine, projectile + 12,
                                      (uint32_t)velocity);

                    uint8_t damage = bs_recomp_read8(machine,
                                                      projectile + 62);
                    if (damage != 0) {
                        bs_recomp_write8(machine, projectile + 62, 0);
                        int16_t armour = (int8_t)bs_recomp_read8(
                            machine, projectile + 24);
                        armour -= damage;
                        bs_recomp_write8(machine, projectile + 24,
                                         (uint8_t)armour);
                        if (armour < 0) {
                            bs_recomp_write16(machine, projectile, 0);
                            goto next_projectile;
                        }
                        bs_recomp_write8(machine, projectile + 57, 6);
                    }

                    bs_recomp_write32(machine, projectile,
                        bs_recomp_read32(machine, projectile) +
                            (uint32_t)velocity);
                    int32_t signed_velocity = (int32_t)bs_recomp_read32(
                        machine, projectile + 12);
                    uint8_t sprite;
                    if (signed_velocity < (int32_t)bs_recomp_read32(
                            machine, machine->cpu.a[2]))
                        sprite = 0;
                    else if (signed_velocity < (int32_t)bs_recomp_read32(
                                 machine, machine->cpu.a[2] + 4))
                        sprite = 1;
                    else if (signed_velocity < (int32_t)bs_recomp_read32(
                                 machine, machine->cpu.a[2] + 8))
                        sprite = 2;
                    else if (signed_velocity < (int32_t)bs_recomp_read32(
                                 machine, machine->cpu.a[2] + 12))
                        sprite = 3;
                    else
                        sprite = 4;
                    uint8_t flash = bs_recomp_read8(machine,
                                                     projectile + 57);
                    if (flash != 0) {
                        flash--;
                        bs_recomp_write8(machine, projectile + 57, flash);
                        if (!(flash & 0x02)) sprite += 5;
                    }
                    bs_recomp_write8(machine, projectile + 63, sprite);
                    y = (uint16_t)(y + bs_recomp_read16(
                        machine, machine->cpu.a[2] + 16));
                    bs_recomp_write16(machine, projectile + 4, y);
                    if (y >= 0x0200) {
                        bs_recomp_write16(machine, projectile, 0);
                        goto next_projectile;
                    }
                    uint16_t frame_stride = bs_recomp_read16(
                        machine, projectile + 46);
                    machine->cpu.a[2] = bs_recomp_read32(
                        machine, projectile + 36) +
                        (uint32_t)sprite * frame_stride;
                    machine->cpu.a[3] = machine->cpu.a[2] + frame_stride -
                        bs_recomp_read16(machine, projectile + 44);
                } else if (type == 3) {
                flags |= 0x08;
                bs_recomp_write8(machine, projectile + 30, flags);
                if (bs_recomp_read8(machine, projectile + 8) == 0) {
                    machine->cpu.a[1] =
                        bs_recomp_read32(machine, projectile + 12);
                    bs_recomp_write32(machine, projectile + 8,
                                      bs_recomp_read32(machine,
                                                       machine->cpu.a[1]));
                    machine->cpu.a[1] += 4;
                    bs_recomp_write32(machine, projectile + 12,
                                      machine->cpu.a[1]);
                    if (bs_recomp_read16(machine, projectile + 8) == 0) {
                        bs_recomp_write16(machine, projectile, 0);
                        goto next_projectile;
                    }
                }

                uint8_t old_phase =
                    bs_recomp_read8(machine, projectile + 10);
                uint8_t phase = (uint8_t)(old_phase +
                    bs_recomp_read8(machine, projectile + 11));
                bs_recomp_write8(machine, projectile + 10, phase);
                height = phase & 0x3f;
                bs_recomp_write16(machine, projectile + 50, height);
                machine->cpu.a[2] = 0x36000;
                machine->cpu.a[3] = 0x36280;
                if (phase & 0x40) {
                    if ((int8_t)bs_recomp_read8(machine,
                                                 projectile + 8) < 0) {
                        machine->cpu.a[2] += 0x900;
                        machine->cpu.a[3] += 0x900;
                    } else {
                        machine->cpu.a[2] += 0x600;
                        machine->cpu.a[3] += 0x600;
                    }
                    bs_recomp_write8(machine, projectile + 30,
                        bs_recomp_read8(machine, projectile + 30) | 0x80);
                    bs_recomp_write8(machine, projectile + 62, 0);
                } else {
                    bs_recomp_write8(machine, projectile + 30,
                        bs_recomp_read8(machine, projectile + 30) &
                        (uint8_t)~0x80);
                    uint8_t damage = bs_recomp_read8(machine,
                                                      projectile + 62);
                    if (damage != 0) {
                        bs_recomp_write8(machine, projectile + 62, 0);
                        bs_recomp_write8(machine, projectile + 57, 4);
                        int16_t armour = (int8_t)bs_recomp_read8(
                            machine, projectile + 24);
                        armour -= damage;
                        bs_recomp_write8(machine, projectile + 24,
                                         (uint8_t)armour);
                        if (armour < 0) {
                            bs_recomp_write8(machine, projectile + 26, 0x0f);
                            bs_recomp_write8(machine, projectile + 30,
                                (uint8_t)((bs_recomp_read8(machine,
                                    projectile + 30) & ~0x20) | 0x80));
                            bs_recomp_write8(machine, projectile + 10,
                                             old_phase);
                            bs_recomp_write32(machine, projectile + 36,
                                              0x11090);
                            bs_recomp_write32(machine, projectile + 32,
                                              0x11310);
                        }
                    }
                    uint8_t flash = bs_recomp_read8(machine,
                                                     projectile + 57);
                    if (flash != 0) {
                        flash--;
                        bs_recomp_write8(machine, projectile + 57, flash);
                        if (flash & 1) {
                            machine->cpu.a[2] += 0x300;
                            machine->cpu.a[3] += 0x300;
                        }
                    }
                }

                int16_t step = (int8_t)bs_recomp_read8(machine,
                                                        projectile + 9);
                if ((int8_t)bs_recomp_read8(machine, projectile + 8) < 0)
                    bs_recomp_write16(machine, projectile,
                        (uint16_t)(bs_recomp_read16(machine, projectile) +
                                   step));
                else
                    bs_recomp_write16(machine, projectile + 4,
                        (uint16_t)(bs_recomp_read16(machine,
                                                    projectile + 4) + step));
                bs_recomp_write16(machine, projectile + 4,
                    (uint16_t)(bs_recomp_read16(machine, projectile + 4) +
                               bs_recomp_read16(machine, base + 7222)));
                int8_t curve = (int8_t)bs_recomp_read8(machine,
                                                        projectile + 8);
                curve += curve < 0 ? 1 : -1;
                bs_recomp_write8(machine, projectile + 8, (uint8_t)curve);
                } else if (type == 7) {
                    /* LAB_87E4: expanding ground shot.  Its height grows a
                     * scanline at a time and the five-plane terrain probe
                     * holds it against solid map pixels before the normal
                     * impact/death animation. */
                    y = (uint16_t)(bs_recomp_read16(machine,
                                                    projectile + 4) +
                                     bs_recomp_read16(machine, base + 7222));
                    bs_recomp_write16(machine, projectile + 4, y);
                    flags = (uint8_t)(bs_recomp_read8(machine,
                                                       projectile + 30) |
                                      0x08);
                    bs_recomp_write8(machine, projectile + 30, flags);
                    if (y >= 0x0200) {
                        bs_recomp_write16(machine, projectile, 0);
                        goto next_projectile;
                    }

                    uint8_t sprite = bs_recomp_read8(machine,
                                                      projectile + 63);
                    if (sprite >= 3) {
                        if (!(bs_recomp_read8(machine, base - 28551) & 2)) {
                            sprite++;
                            bs_recomp_write8(machine, projectile + 63,
                                             sprite);
                            if (sprite >= 11) {
                                bs_recomp_write16(machine, projectile, 0);
                                goto next_projectile;
                            }
                        }
                    } else {
                        uint8_t growth = bs_recomp_read8(machine,
                                                          projectile + 27);
                        if (growth < 0x20) {
                            growth++;
                            bs_recomp_write8(machine, projectile + 27,
                                             growth);
                            bs_recomp_write8(machine, projectile + 51,
                                             growth);
                        }
                        sprite = 0;

                        int16_t terrain_y = (int16_t)y - 0x0101;
                        int16_t terrain_x = (int16_t)
                            bs_recomp_read16(machine, projectile) - 0x00fe;
                        if (terrain_y >= 0 && terrain_x >= 0) {
                            uint32_t probe =
                                bs_recomp_read32(machine, base + 7208) +
                                (uint32_t)terrain_y * 0x30 +
                                ((uint16_t)terrain_x >> 3);
                            int solid = 0;
                            for (int plane = 0; plane < 5; plane++) {
                                uint8_t value = bs_recomp_read8(
                                    machine, probe + (uint32_t)plane * 0x6000);
                                if (value != 0 && value != 0xff) {
                                    solid = 1;
                                    break;
                                }
                            }
                            if (!solid) {
                                y--;
                                bs_recomp_write16(machine, projectile + 4, y);
                                if (!(bs_recomp_read8(machine,
                                                       base - 28551) & 2))
                                    sprite++;
                            }
                        }

                        uint8_t damage = bs_recomp_read8(machine,
                                                          projectile + 62);
                        if (damage != 0) {
                            bs_recomp_write8(machine, projectile + 62, 0);
                            bs_recomp_write8(machine, projectile + 57, 4);
                            int16_t armour = (int8_t)bs_recomp_read8(
                                machine, projectile + 24);
                            armour -= damage;
                            bs_recomp_write8(machine, projectile + 24,
                                             (uint8_t)armour);
                            if (armour < 0) {
                                bs_recomp_write32(machine, projectile + 36,
                                                  0x10790);
                                sprite = 3;
                                flags = (uint8_t)((flags | 0x80) & ~0x20);
                            }
                        }
                        uint8_t flash = bs_recomp_read8(machine,
                                                         projectile + 57);
                        if (sprite < 3 && flash != 0) {
                            flash--;
                            bs_recomp_write8(machine, projectile + 57,
                                             flash);
                            if (flash & 1) sprite = 2;
                        }

                        flags &= (uint8_t)~0x20;
                        if (growth >= 0x14) {
                            uint8_t timer = bs_recomp_read8(machine,
                                                             projectile + 28);
                            if (timer == 0) {
                                timer = bs_recomp_read8(machine,
                                                        base - 2199);
                                flags |= 0x20;
                                bs_recomp_write16(machine, projectile + 58,
                                    (uint16_t)(bs_recomp_read16(machine,
                                                               projectile) +
                                               12));
                                bs_recomp_write16(machine, projectile + 60,
                                                  (uint16_t)(y + 10));
                            }
                            timer--;
                            bs_recomp_write8(machine, projectile + 28,
                                             timer);
                        }
                        bs_recomp_write8(machine, projectile + 30, flags);
                        bs_recomp_write8(machine, projectile + 63, sprite);
                    }
                    height = bs_recomp_read16(machine, projectile + 50);
                    uint16_t frame_stride = bs_recomp_read16(
                        machine, projectile + 46);
                    machine->cpu.a[2] = bs_recomp_read32(
                        machine, projectile + 36) +
                        (uint32_t)sprite * frame_stride;
                    machine->cpu.a[3] = machine->cpu.a[2] + frame_stride -
                        bs_recomp_read16(machine, projectile + 44);
                } else if (type == 6) {
                    /* LAB_890C: oscillating vertical launcher/projectile. */
                    uint8_t sprite = bs_recomp_read8(machine,
                                                      projectile + 63);
                    if (sprite >= 5) {
                        if (!(bs_recomp_read8(machine, base - 28551) & 2)) {
                            sprite++;
                            bs_recomp_write8(machine, projectile + 63,
                                             sprite);
                            if (sprite >= 11) {
                                bs_recomp_write16(machine, projectile, 0);
                                goto next_projectile;
                            }
                        }
                    } else {
                        sprite = 0;
                        uint8_t damage = bs_recomp_read8(machine,
                                                          projectile + 62);
                        if (damage != 0) {
                            bs_recomp_write8(machine, projectile + 62, 0);
                            bs_recomp_write8(machine, projectile + 57, 4);
                            int16_t armour = (int8_t)bs_recomp_read8(
                                machine, projectile + 24);
                            armour -= damage;
                            bs_recomp_write8(machine, projectile + 24,
                                             (uint8_t)armour);
                            if (armour < 0) {
                                bs_recomp_write8(machine, projectile + 30,
                                    bs_recomp_read8(machine,
                                                    projectile + 30) | 0x80);
                                sprite = 5;
                            }
                        }
                        uint8_t flash = bs_recomp_read8(machine,
                                                         projectile + 57);
                        if (sprite < 5 && flash != 0) {
                            flash--;
                            bs_recomp_write8(machine, projectile + 57,
                                             flash);
                            sprite = (uint8_t)(4 - flash);
                        }

                        int8_t turn = (int8_t)bs_recomp_read8(
                            machine, projectile + 27);
                        if (turn == 0) {
                            uint8_t random = next_wave_random(machine);
                            int8_t first = (int8_t)(random & 0x0f);
                            if (random & 0x20) first = (int8_t)-first;
                            turn = first;
                            bs_recomp_write8(machine, projectile + 27,
                                             (uint8_t)turn);
                            bs_recomp_write8(machine, projectile + 28,
                                             (uint8_t)-turn);
                        }
                        int32_t vx = (int32_t)bs_recomp_read32(
                            machine, projectile + 8);
                        if (turn < 0) {
                            vx += 0x1000;
                            turn++;
                        } else {
                            vx -= 0x1000;
                            turn--;
                        }
                        if (turn == 0) {
                            int8_t alternate = (int8_t)bs_recomp_read8(
                                machine, projectile + 28);
                            if (alternate != 0) {
                                turn = alternate;
                                bs_recomp_write8(machine, projectile + 28,
                                                 0);
                            }
                        }
                        bs_recomp_write8(machine, projectile + 27,
                                         (uint8_t)turn);
                        bs_recomp_write32(machine, projectile + 8,
                                          (uint32_t)vx);
                        bs_recomp_write32(machine, projectile,
                            bs_recomp_read32(machine, projectile) +
                                (uint32_t)vx);

                        uint8_t timer = bs_recomp_read8(machine,
                                                         projectile + 26);
                        if (timer == 0)
                            timer = bs_recomp_read8(machine, base - 2198);
                        timer--;
                        bs_recomp_write8(machine, projectile + 26, timer);
                        uint16_t impact_x = bs_recomp_read16(machine,
                                                             projectile);
                        uint16_t impact_y = (uint16_t)(
                            bs_recomp_read16(machine, projectile + 4) + 40);
                        if (timer == 16 || timer == 8 || timer == 0) {
                            if (timer == 16) impact_x += 11;
                            else if (timer == 8) {
                                impact_x += 2;
                                bs_recomp_write16(machine, base - 14390,
                                                  0xffe0);
                                bs_recomp_write16(machine, base - 14388,
                                    (uint16_t)((bs_recomp_read16(
                                        machine, base - 28552) & 0x7f) |
                                        0x40));
                            } else {
                                impact_x += 20;
                                bs_recomp_write16(machine, base - 14390,
                                                  0x0020);
                                bs_recomp_write16(machine, base - 14388,
                                    (uint16_t)((bs_recomp_read16(
                                        machine, base - 28552) & 0x7f) |
                                        0x40));
                            }
                            bs_recomp_write16(machine, projectile + 58,
                                              impact_x);
                            bs_recomp_write16(machine, projectile + 60,
                                              impact_y);
                            bs_recomp_write8(machine, projectile + 30,
                                bs_recomp_read8(machine, projectile + 30) |
                                    0x20);
                        }
                        bs_recomp_write32(machine, projectile + 4,
                            bs_recomp_read32(machine, projectile + 4) +
                                UINT32_C(0x00014000));
                        y = bs_recomp_read16(machine, projectile + 4);
                        if (y >= 0x0200) {
                            bs_recomp_write16(machine, projectile, 0);
                            goto next_projectile;
                        }
                    }
                    bs_recomp_write8(machine, projectile + 63, sprite);
                    uint16_t frame_stride = bs_recomp_read16(
                        machine, projectile + 46);
                    machine->cpu.a[2] = bs_recomp_read32(
                        machine, projectile + 36) +
                        (uint32_t)sprite * frame_stride;
                    machine->cpu.a[3] = machine->cpu.a[2] + frame_stride -
                        bs_recomp_read16(machine, projectile + 44);
                } else if (type == 8) {
                    /* LAB_8664: accelerating homing projectile.  The
                     * original chooses one of the live players, steers each
                     * fixed-point velocity component toward a capped value,
                     * and periodically raises an impact/debris marker. */
                    uint8_t damage = bs_recomp_read8(machine,
                                                      projectile + 62);
                    if (damage != 0) {
                        bs_recomp_write8(machine, projectile + 62, 0);
                        int16_t armour = (int8_t)bs_recomp_read8(
                            machine, projectile + 24);
                        armour -= damage;
                        bs_recomp_write8(machine, projectile + 24,
                                         (uint8_t)armour);
                        if (armour < 0) {
                            bs_recomp_write16(machine, projectile, 0);
                            goto next_projectile;
                        }
                        bs_recomp_write8(machine, projectile + 57, 8);
                    }

                    uint16_t x = bs_recomp_read16(machine, projectile);
                    y = bs_recomp_read16(machine, projectile + 4);
                    uint16_t target_x = bs_recomp_read16(
                        machine, base - 12736);
                    uint16_t target_y = bs_recomp_read16(
                        machine, base - 12734);
                    uint8_t delay = bs_recomp_read8(machine,
                                                     projectile + 28);
                    if (delay != 0) {
                        delay--;
                        bs_recomp_write8(machine, projectile + 28, delay);
                        uint16_t player_two_x = bs_recomp_read16(
                            machine, base - 12470);
                        uint16_t player_two_y = bs_recomp_read16(
                            machine, base - 12468);
                        uint32_t first_distance =
                            (target_x >= x ? target_x - x : x - target_x) +
                            (target_y >= y ? target_y - y : y - target_y);
                        uint32_t second_distance =
                            (player_two_x >= x ? player_two_x - x
                                               : x - player_two_x) +
                            (player_two_y >= y ? player_two_y - y
                                               : y - player_two_y);
                        if (second_distance < first_distance) {
                            target_x = player_two_x;
                            target_y = player_two_y;
                        }
                    }

                    int32_t maximum = (int32_t)bs_recomp_read32(
                        machine, base - 1790);
                    int32_t acceleration = (int32_t)bs_recomp_read32(
                        machine, base - 1786);
                    int32_t vx = (int32_t)bs_recomp_read32(
                        machine, projectile + 8);
                    int32_t vy = (int32_t)bs_recomp_read32(
                        machine, projectile + 12);
                    if (target_x < x) {
                        if (vx > -maximum) vx -= acceleration;
                    } else if (vx < maximum) {
                        vx += acceleration;
                    }
                    if (target_y < y) {
                        if (vy > -maximum) vy -= acceleration;
                    } else if (vy < maximum) {
                        vy += acceleration;
                    }
                    bs_recomp_write32(machine, projectile + 8,
                                      (uint32_t)vx);
                    bs_recomp_write32(machine, projectile + 12,
                                      (uint32_t)vy);

                    int32_t abs_x = vx < 0 ? -vx : vx;
                    int32_t abs_y = vy < 0 ? -vy : vy;
                    uint8_t sprite;
                    if (abs_x > abs_y * 2)
                        sprite = vx < 0 ? 8 : 0;
                    else if (abs_y > abs_x * 2)
                        sprite = vy < 0 ? 12 : 4;
                    else if (vx >= 0)
                        sprite = vy < 0 ? 14 : 2;
                    else
                        sprite = vy < 0 ? 10 : 6;
                    bs_recomp_write8(machine, projectile + 63, sprite);

                    uint8_t timer = bs_recomp_read8(machine,
                                                     projectile + 27);
                    if (bs_recomp_read8(machine, projectile + 26) == 0) {
                        bs_recomp_write8(machine, projectile + 26, 0xff);
                        timer = 0x14;
                    }
                    timer--;
                    if (timer == 0) {
                        timer = bs_recomp_read8(machine, base - 2197);
                        bs_recomp_write16(machine, projectile + 58,
                                          (uint16_t)(x + 12));
                        bs_recomp_write16(machine, projectile + 60,
                                          (uint16_t)(y + 12));
                        bs_recomp_write8(machine, projectile + 30,
                            bs_recomp_read8(machine, projectile + 30) |
                                0x20);
                    }
                    bs_recomp_write8(machine, projectile + 27, timer);
                    uint8_t flash = bs_recomp_read8(machine,
                                                     projectile + 57);
                    if (flash != 0)
                        bs_recomp_write8(machine, projectile + 57,
                                         (uint8_t)(flash - 1));

                    bs_recomp_write32(machine, projectile,
                        bs_recomp_read32(machine, projectile) +
                            (uint32_t)vx);
                    bs_recomp_write32(machine, projectile + 4,
                        bs_recomp_read32(machine, projectile + 4) +
                            (uint32_t)vy);
                    x = bs_recomp_read16(machine, projectile);
                    y = bs_recomp_read16(machine, projectile + 4);
                    uint16_t world_left = (uint16_t)(
                        bs_recomp_read16(machine, base + 7204) - 0x20);
                    uint16_t world_right = (uint16_t)(world_left + 0x140);
                    if (x <= world_left || x >= world_right || y >= 0x0200) {
                        bs_recomp_write16(machine, projectile, 0);
                        goto next_projectile;
                    }
                    bs_recomp_write32(machine, projectile + 36, 0x17500);
                    uint16_t frame_stride = bs_recomp_read16(
                        machine, projectile + 46);
                    machine->cpu.a[2] = 0x17500 +
                        (uint32_t)sprite * frame_stride;
                    machine->cpu.a[3] = machine->cpu.a[2] + frame_stride -
                        bs_recomp_read16(machine, projectile + 44);
                } else {
                    /* LAB_8B48: armoured steering projectile.  It shares the
                     * original record ABI and direction frames with type 8,
                     * but takes its speed limits from the $7946/$7952 table. */
                    uint32_t steering = bs_recomp_read16(machine,
                        base + 7228) ? 0x7952 : 0x7946;
                    uint8_t damage = bs_recomp_read8(machine,
                                                      projectile + 62);
                    if (damage != 0) {
                        bs_recomp_write8(machine, projectile + 62, 0);
                        int16_t armour = (int8_t)bs_recomp_read8(
                            machine, projectile + 24);
                        armour -= damage;
                        bs_recomp_write8(machine, projectile + 24,
                                         (uint8_t)armour);
                        if (armour < 0) {
                            bs_recomp_write16(machine, projectile, 0);
                            goto next_projectile;
                        }
                        bs_recomp_write8(machine, projectile + 57, 8);
                    }

                    uint8_t mode = bs_recomp_read8(machine,
                                                    projectile + 26);
                    if (mode == 0) {
                        mode = 1;
                        bs_recomp_write8(machine, projectile + 26, mode);
                        bs_recomp_write32(machine, projectile + 8, 0);
                        bs_recomp_write32(machine, projectile + 12,
                            bs_recomp_read32(machine, steering));
                        uint8_t random = next_wave_random(machine);
                        int8_t turn = (int8_t)(random & 0x0f);
                        if (random & 0x20) turn = (int8_t)-turn;
                        bs_recomp_write8(machine, projectile + 27,
                                         (uint8_t)turn);
                        bs_recomp_write8(machine, projectile + 28,
                                         (uint8_t)-turn);
                    }

                    uint16_t x = bs_recomp_read16(machine, projectile);
                    y = bs_recomp_read16(machine, projectile + 4);
                    uint8_t flags_now = bs_recomp_read8(machine,
                                                         projectile + 30);
                    if (!(flags_now & 1) && y >= 0x0100) {
                        bs_recomp_write8(machine, projectile + 30,
                                         flags_now | 0x21);
                        bs_recomp_write16(machine, projectile + 58,
                                          (uint16_t)(x + 11));
                        bs_recomp_write16(machine, projectile + 60,
                                          (uint16_t)(y + 32));
                    }

                    uint16_t target_x = bs_recomp_read16(
                        machine, base - 12736);
                    uint16_t target_y = bs_recomp_read16(
                        machine, base - 12734);
                    uint16_t player_two_x = bs_recomp_read16(
                        machine, base - 12470);
                    uint16_t player_two_y = bs_recomp_read16(
                        machine, base - 12468);
                    uint32_t first_distance =
                        (target_x >= x ? target_x - x : x - target_x) +
                        (target_y >= y ? target_y - y : y - target_y);
                    uint32_t second_distance =
                        (player_two_x >= x ? player_two_x - x
                                           : x - player_two_x) +
                        (player_two_y >= y ? player_two_y - y
                                           : y - player_two_y);
                    if (second_distance < first_distance) {
                        target_x = player_two_x;
                        target_y = player_two_y;
                    }

                    int32_t maximum = (int32_t)bs_recomp_read32(
                        machine, steering);
                    if (maximum < 0) maximum = -maximum;
                    int32_t acceleration = (int32_t)bs_recomp_read32(
                        machine, steering + 4);
                    if (acceleration < 0) acceleration = -acceleration;
                    int32_t vx = (int32_t)bs_recomp_read32(
                        machine, projectile + 8);
                    int32_t vy = (int32_t)bs_recomp_read32(
                        machine, projectile + 12);
                    if (target_x < x) {
                        vx -= acceleration;
                        if (vx < -maximum) vx = -maximum;
                    } else {
                        vx += acceleration;
                        if (vx > maximum) vx = maximum;
                    }
                    if (target_y < y) {
                        vy -= acceleration;
                        if (vy < -maximum) vy = -maximum;
                    } else {
                        vy += acceleration;
                        if (vy > maximum) vy = maximum;
                    }
                    bs_recomp_write32(machine, projectile + 8,
                                      (uint32_t)vx);
                    bs_recomp_write32(machine, projectile + 12,
                                      (uint32_t)vy);
                    bs_recomp_write32(machine, projectile,
                        bs_recomp_read32(machine, projectile) +
                            (uint32_t)vx);
                    bs_recomp_write32(machine, projectile + 4,
                        bs_recomp_read32(machine, projectile + 4) +
                            (uint32_t)vy);
                    x = bs_recomp_read16(machine, projectile);
                    y = bs_recomp_read16(machine, projectile + 4);

                    int32_t abs_x = vx < 0 ? -vx : vx;
                    int32_t abs_y = vy < 0 ? -vy : vy;
                    uint8_t sprite;
                    if (abs_x > abs_y * 2)
                        sprite = vx < 0 ? 8 : 0;
                    else if (abs_y > abs_x * 2)
                        sprite = vy < 0 ? 12 : 4;
                    else if (vx >= 0)
                        sprite = vy < 0 ? 14 : 2;
                    else
                        sprite = vy < 0 ? 10 : 6;
                    uint8_t flash = bs_recomp_read8(machine,
                                                     projectile + 57);
                    if (flash != 0) {
                        flash--;
                        bs_recomp_write8(machine, projectile + 57, flash);
                        if (flash & 2) sprite = (uint8_t)(sprite + 16);
                    }
                    bs_recomp_write8(machine, projectile + 63, sprite);
                    uint16_t world_left = (uint16_t)(
                        bs_recomp_read16(machine, base + 7204) - 0x20);
                    uint16_t world_right = (uint16_t)(world_left + 0x140);
                    if (x <= world_left || x >= world_right || y >= 0x0200 ||
                        y <= 0x00e0) {
                        bs_recomp_write16(machine, projectile, 0);
                        goto next_projectile;
                    }
                    uint16_t frame_stride = bs_recomp_read16(
                        machine, projectile + 46);
                    machine->cpu.a[2] = bs_recomp_read32(
                        machine, projectile + 36) +
                        (uint32_t)sprite * frame_stride;
                    machine->cpu.a[3] = machine->cpu.a[2] + frame_stride -
                        bs_recomp_read16(machine, projectile + 44);
                }

                y = bs_recomp_read16(machine, projectile + 4);
                if (y >= 0x0200 || height == 0) {
                    bs_recomp_write16(machine, projectile, 0);
                    goto next_projectile;
                }
                uint16_t x = bs_recomp_read16(machine, projectile);
                bs_recomp_write16(machine, projectile + 16,
                    (uint16_t)(x + bs_recomp_read16(machine,
                                                    projectile + 64)));
                bs_recomp_write16(machine, projectile + 18,
                    (uint16_t)(bs_recomp_read16(machine, projectile + 16) +
                               bs_recomp_read16(machine, projectile + 66)));
                bs_recomp_write16(machine, projectile + 20, y);
                bs_recomp_write16(machine, projectile + 22,
                                   (uint16_t)(y + height));
                draw_projectile_cookie_cut(machine, projectile,
                                           machine->cpu.a[2],
                                           machine->cpu.a[3]);

                uint16_t left = x < 0x0100 ? 0x0100 : x;
                uint16_t width = bs_recomp_read16(machine,
                                                   projectile + 52);
                uint16_t right = (uint16_t)(left + (width << 4));
                if (right >= 0x0280)
                    left = (uint16_t)(0x0280 - (right - left));
                uint16_t top = y < 0x0100 ? 0x0100 : y;
                uint16_t bottom = (uint16_t)(top + height);
                if (bottom >= 0x01ff)
                    top = (uint16_t)(0x01ff - height);
                uint32_t cursor = bs_recomp_read32(machine, base - 1800);
                if (top < 0x0180 && (uint16_t)(top + height) > 0x0180) {
                    bs_recomp_write16(machine, cursor + 0, left);
                    bs_recomp_write16(machine, cursor + 2, top);
                    bs_recomp_write16(machine, cursor + 4,
                                      (uint16_t)(0x0180 - top));
                    bs_recomp_write16(machine, cursor + 6, width);
                    cursor += 8;
                    bs_recomp_write16(machine, cursor + 0, left);
                    bs_recomp_write16(machine, cursor + 2, 0x0180);
                    bs_recomp_write16(machine, cursor + 4,
                        (uint16_t)(top + height - 0x0180));
                    bs_recomp_write16(machine, cursor + 6, width);
                    cursor += 8;
                } else {
                    bs_recomp_write16(machine, cursor + 0, left);
                    bs_recomp_write16(machine, cursor + 2, top);
                    bs_recomp_write16(machine, cursor + 4, height);
                    bs_recomp_write16(machine, cursor + 6, width);
                    cursor += 8;
                }
                bs_recomp_write32(machine, base - 1800, cursor);
            }
        }
next_projectile:
        machine->cpu.a[4] += 0x50;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    machine->cpu.a[1] =
        bs_recomp_read32(machine, machine->cpu.a[5] - 1800);
    bs_recomp_write16(machine, machine->cpu.a[1], 0);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
    return BS_RECOMP_OK;
}

/* LAB_9A9E/LAB_9AB6. Recorded mode consumes the byte-exact attract stream;
 * live mode maps a frontend-neutral U/D/L/R/fire/Nova mask to the game's
 * clockwise direction bits (up/right/down/left). */
static int update_player_input(BsRecomp *machine)
{
    if (bs_recomp_read8(machine, machine->cpu.a[5] - 28516) == 0) {
        static const uint32_t players[] = {0x4e3c, 0x4f46};
        for (unsigned player = 0; player < 2; player++) {
            uint8_t host = machine->input[player];
            uint8_t game = 0;
            if (host & BS_INPUT_UP) game |= 0x01;
            if (host & BS_INPUT_RIGHT) game |= 0x02;
            if (host & BS_INPUT_DOWN) game |= 0x04;
            if (host & BS_INPUT_LEFT) game |= 0x08;
            game |= host & (BS_INPUT_FIRE | BS_INPUT_NOVA);
            bs_recomp_write8(machine, players[player] + 44, game);
        }
        machine->cpu.a[4] = 0x4f46;
        machine->cpu.sr &= 0xfff0;
        return BS_RECOMP_OK;
    }
    machine->cpu.a[0] =
        bs_recomp_read32(machine, machine->cpu.a[5] + 6810);
    bs_recomp_write8(machine, machine->cpu.a[5] - 12696,
                     bs_recomp_read8(machine, machine->cpu.a[0]++));
    bs_recomp_write8(machine, machine->cpu.a[5] - 12430,
                     bs_recomp_read8(machine, machine->cpu.a[0]++));
    bs_recomp_write32(machine, machine->cpu.a[5] + 6810,
                      machine->cpu.a[0]);
    /* MOVE.L A0,d16(A5): positive and non-zero, X preserved. */
    machine->cpu.sr &= 0xfff0;
    return BS_RECOMP_OK;
}

static int fire_player_primary(BsRecomp *machine, uint32_t player)
{
    machine->cpu.a[0] = player + 122;
    machine->cpu.a[1] = player + 12;
    machine->cpu.a[3] = machine->cpu.a[1];
    machine->cpu.a[2] = machine->cpu.a[0];
    set_dreg_byte(&machine->cpu.d[1], 0);
    set_dreg_byte(&machine->cpu.d[2], 0);
    machine->cpu.d[0] = 11;
    for (int slot = 0; slot < 12; slot++) {
        int8_t state = (int8_t)bs_recomp_read8(machine,
                                                machine->cpu.a[1]++);
        if (state >= 0 && bs_recomp_read16(machine, machine->cpu.a[2]) != 0) {
            if (state == 0) set_dreg_byte(&machine->cpu.d[1],
                (uint8_t)(machine->cpu.d[1] + 1));
            else set_dreg_byte(&machine->cpu.d[2],
                (uint8_t)(machine->cpu.d[2] + 1));
        }
        machine->cpu.a[2] += 12;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    if ((uint8_t)machine->cpu.d[1] != 0) {
        if ((uint8_t)machine->cpu.d[2] != 0) {
            machine->cpu.sr &= 0xfff0;
            return BS_RECOMP_OK;
        }
        machine->cpu.a[1] = machine->cpu.a[0];
        set_dreg_word(&machine->cpu.d[0],
                      bs_recomp_read16(machine, player + 30));
        set_dreg_word(&machine->cpu.d[1], (uint16_t)machine->cpu.d[0]);
        machine->cpu.d[1] =
            (uint16_t)machine->cpu.d[1] * UINT32_C(12);
        bs_recomp_write32(machine, machine->cpu.a[6] + 84,
                          machine->cpu.a[0]);
        machine->cpu.a[1] += machine->cpu.d[1];
        bs_recomp_write32(machine, machine->cpu.a[6] + 80,
                          machine->cpu.a[1]);
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)((uint16_t)machine->cpu.d[0] << 6));
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] + 6));
        bs_recomp_write16(machine, machine->cpu.a[6] + 88,
                          (uint16_t)machine->cpu.d[0]);
        uint16_t shift_records = bs_recomp_read16(machine, player + 30);
        for (uint32_t byte = 0; byte < (uint32_t)shift_records * 12; byte++)
            bs_recomp_write8(machine, machine->cpu.a[0] + byte,
                bs_recomp_read8(machine,
                    machine->cpu.a[0] + (uint32_t)shift_records * 12 + byte));
        machine->cpu.a[1] = machine->cpu.a[0];
        set_dreg_word(&machine->cpu.d[1],
                      bs_recomp_read16(machine, player + 32));
        if ((uint16_t)machine->cpu.d[1] != 0) {
            snprintf(machine->error, sizeof machine->error,
                     "untranslated staggered primary-fire bank at $%06x",
                     player);
            return BS_RECOMP_UNTRANSLATED;
        }
    }
    machine->cpu.a[1] = bs_recomp_read32(machine, player + 24);
    machine->cpu.d[0] = 11;
    for (int slot = 0; slot < 12; slot++) {
        if (bs_recomp_read8(machine, machine->cpu.a[3]++) == 0) {
            bs_recomp_write32(machine, machine->cpu.a[0],
                              bs_recomp_read32(machine, machine->cpu.a[1]));
            machine->cpu.a[1] += 4;
            bs_recomp_write32(machine, machine->cpu.a[0] + 4,
                              bs_recomp_read32(machine, machine->cpu.a[1]));
            machine->cpu.a[1] += 4;
            bs_recomp_write32(machine, machine->cpu.a[0] + 8,
                              bs_recomp_read32(machine, machine->cpu.a[1]));
            machine->cpu.a[1] += 4;
        }
        machine->cpu.a[0] += 12;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    bs_recomp_write16(machine, player + 46,
                      bs_recomp_read16(machine, player + 28));
    bs_recomp_write8(machine, player + 57, 0x0f);
    set_dreg_word(&machine->cpu.d[1],
                  bs_recomp_read16(machine, player + 58));
    machine->cpu.a[0] = 0x3f40 + (uint16_t)machine->cpu.d[1];
    machine->cpu.d[0] = bs_recomp_read8(machine, machine->cpu.a[0]);
    machine->cpu.a[1] = 0x3ca0;
    machine->cpu.sr &= 0xfff0;
    return BS_RECOMP_OK;
}

/* LAB_3F44 for inactive players and active movement/fire timers. */
static int update_inactive_player_timers(BsRecomp *machine)
{
    static const uint32_t players[] = {0x4e3c, 0x4f46};
    for (size_t i = 0; i < sizeof players / sizeof players[0]; i++) {
        machine->cpu.a[4] = players[i];
        uint8_t marker = bs_recomp_read8(machine, machine->cpu.a[4] + 38);
        if (marker != 0) {
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                              ((marker & 0x80) ? 0x08 : 0));
            continue;
        }
        uint8_t controls = bs_recomp_read8(machine, machine->cpu.a[4] + 44);
        if (controls & 0x20) {
            uint16_t charges =
                bs_recomp_read16(machine, machine->cpu.a[4] + 66);
            if (bs_recomp_read8(machine,
                                machine->cpu.a[5] - 25334) == 0 &&
                charges != 0) {
                bs_recomp_write8(machine,
                                 machine->cpu.a[5] - 27618, 0x32);
                bs_recomp_write16(machine, machine->cpu.a[4] + 66,
                                  (uint16_t)(charges - 1));
                bs_recomp_write8(machine, machine->cpu.a[4] + 90, 0xff);
                bs_recomp_write8(machine,
                                 machine->cpu.a[5] - 25334, 0xff);
                expand_capacity_bars(machine,
                                     machine->cpu.a[4] == 0x4f46);
            }
        }
        set_dreg_byte(&machine->cpu.d[1], controls);
        if (bs_recomp_read8(machine, machine->cpu.a[4] + 90) != 0)
            continue;
        uint16_t cooldown =
            bs_recomp_read16(machine, machine->cpu.a[4] + 46);
        if (cooldown != 0) {
            bs_recomp_write16(machine, machine->cpu.a[4] + 46,
                              (uint16_t)(cooldown - 1));
            uint8_t repeat =
                bs_recomp_read8(machine, machine->cpu.a[4] + 57);
            if (repeat != 0)
                bs_recomp_write8(machine, machine->cpu.a[4] + 57,
                                  (uint8_t)(repeat - 1));
        }
        if (!(controls & 0x10)) {
            bs_recomp_write8(machine, machine->cpu.a[4] + 57, 0);
            machine->cpu.sr =
                (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
        } else if (bs_recomp_read8(machine,
                                    machine->cpu.a[4] + 57) != 0) {
            bs_recomp_write8(machine, machine->cpu.a[4] + 57,
                (uint8_t)(bs_recomp_read8(machine,
                                           machine->cpu.a[4] + 57) - 1));
        } else {
            int fire = fire_player_primary(machine, machine->cpu.a[4]);
            if (fire != BS_RECOMP_OK) return fire;
        }
    }
    return BS_RECOMP_OK;
}

static void update_respawn_mask_player(BsRecomp *machine,
                                       uint32_t player, uint32_t target)
{
    machine->cpu.a[2] = target;
    machine->cpu.a[4] = player;
    machine->cpu.a[3] = bs_recomp_read32(machine, player + 76);

    uint16_t phase = bs_recomp_read16(machine, player + 68);
    set_dreg_word(&machine->cpu.d[1], phase);
    int reveal = phase >= 0x0384;
    if (!reveal) {
        bs_recomp_write16(machine, player + 68, (uint16_t)(phase - 1));
        if (phase > 0x0020) {
            reveal = 1;
        } else {
            if (phase == 0x0020) bs_recomp_write16(machine, player + 72, 0);
            if (bs_recomp_read16(machine, player + 72) == 0x0100) return;
        }
    }

    if (reveal) {
        if (bs_recomp_read16(machine, player + 72) == 0x0100) return;
        uint16_t current = bs_recomp_read16(machine, player + 68);
        set_dreg_word(&machine->cpu.d[1], current);
        uint16_t next = current;
        if (current != 0x03e7) next = (uint16_t)(current + 1);
        if (current == 0x03e7 ||
            next == bs_recomp_read16(machine, player + 70)) {
            bs_recomp_write16(machine, player + 68,
                              (uint16_t)(current - 1));
            bs_recomp_write16(machine, player + 70,
                              (uint16_t)(bs_recomp_read16(machine,
                                                          player + 70) - 1));
            machine->cpu.a[1] = target;
            machine->cpu.d[0] = 95;
            for (int i = 0; i < 96; i++) {
                bs_recomp_write32(machine, machine->cpu.a[1], 0);
                machine->cpu.a[1] += 4;
                set_dreg_word(&machine->cpu.d[0],
                              (uint16_t)(machine->cpu.d[0] - 1));
            }
        }
    }

    machine->cpu.a[1] = 0x17400 + bs_recomp_read16(machine, player + 72);
    if (reveal) machine->cpu.d[7] = 7;
    else machine->cpu.d[0] = 7;
    for (int column = 0; column < 8; column++) {
        set_dreg_word(&machine->cpu.d[1], 0);
        set_dreg_byte(&machine->cpu.d[1],
                      bs_recomp_read8(machine, machine->cpu.a[1]++));
        set_dreg_word(&machine->cpu.d[2], (uint16_t)machine->cpu.d[1]);
        uint16_t bit = (uint16_t)machine->cpu.d[1] & 15;
        uint16_t offset =
            (uint16_t)(((uint16_t)machine->cpu.d[2] & 0xfff0) >> 2);
        set_dreg_word(&machine->cpu.d[1], bit);
        set_dreg_word(&machine->cpu.d[2], offset);
        machine->cpu.a[0] = target + offset;
        machine->cpu.a[6] = machine->cpu.a[3] + offset;
        if (reveal) {
            machine->cpu.d[4] = 0;
            machine->cpu.d[4] |= UINT32_C(1) << bit;
            set_dreg_byte(&machine->cpu.d[1], (uint8_t)(bit + 0x10));
            machine->cpu.d[4] |= UINT32_C(1) << ((bit + 16) & 31);
        } else {
            machine->cpu.d[4] = UINT32_C(0xffffffff);
            machine->cpu.d[4] &= ~(UINT32_C(1) << bit);
            set_dreg_byte(&machine->cpu.d[1], (uint8_t)(bit + 0x10));
            machine->cpu.d[4] &= ~(UINT32_C(1) << ((bit + 16) & 31));
        }
        if (reveal) {
            machine->cpu.d[0] = 5;
            for (int plane = 0; plane < 6; plane++) {
                machine->cpu.d[3] = bs_recomp_read32(machine,
                                                      machine->cpu.a[6]);
                machine->cpu.d[3] &= machine->cpu.d[4];
                bs_recomp_write32(machine, machine->cpu.a[0],
                    bs_recomp_read32(machine, machine->cpu.a[0]) |
                    machine->cpu.d[3]);
                machine->cpu.a[0] += 0x40;
                machine->cpu.a[6] += 0x40;
                set_dreg_word(&machine->cpu.d[0],
                              (uint16_t)(machine->cpu.d[0] - 1));
            }
            set_dreg_word(&machine->cpu.d[7],
                          (uint16_t)(machine->cpu.d[7] - 1));
        } else {
            for (int plane = 0; plane < 6; plane++)
                bs_recomp_write32(machine, machine->cpu.a[0] + plane * 0x40,
                    bs_recomp_read32(machine,
                                     machine->cpu.a[0] + plane * 0x40) &
                    machine->cpu.d[4]);
            set_dreg_word(&machine->cpu.d[0],
                          (uint16_t)(machine->cpu.d[0] - 1));
        }
    }
    bs_recomp_write16(machine, player + 72,
        (uint16_t)(bs_recomp_read16(machine, player + 72) + 8));
    machine->cpu.a[6] = 0xdff000;
    machine->cpu.sr &= 0xfff0;
}

static void update_respawn_masks(BsRecomp *machine)
{
    if (bs_recomp_read16(machine, machine->cpu.a[5] - 12672) != 0)
        update_respawn_mask_player(machine, 0x4e3c, 0xc8ba);
    if (bs_recomp_read16(machine, machine->cpu.a[5] - 12406) != 0)
        update_respawn_mask_player(machine, 0x4f46, 0xca3a);
}

/* LAB_5050/LAB_5070 while both ships are in their respawn countdown. */
static int update_respawning_players(BsRecomp *machine)
{
    static const uint32_t players[] = {0x4e3c, 0x4f46};
    static const uint32_t inputs[] = {0x4e68, 0x4f72};
    for (size_t i = 0; i < sizeof players / sizeof players[0]; i++) {
        uint32_t player = players[i];
        machine->cpu.a[4] = player;
        set_dreg_word(&machine->cpu.d[2], 2);
        set_dreg_byte(&machine->cpu.d[1],
                      bs_recomp_read8(machine, inputs[i]));
        uint16_t invulnerability = bs_recomp_read16(machine, player + 52);
        if (invulnerability != 0)
            bs_recomp_write16(machine, player + 52,
                              (uint16_t)(invulnerability - 1));
        if (bs_recomp_read8(machine, player + 38) == 0xff) continue;

        uint8_t countdown = bs_recomp_read8(machine, player + 48);
        int countdown_path = countdown != 0;
        if (countdown_path) {
            countdown--;
            bs_recomp_write8(machine, player + 48, countdown);
            if (countdown == 0) {
                bs_recomp_write8(machine, player + 38, 0);
                if (bs_recomp_read8(machine, player + 41) != 0)
                    bs_recomp_write8(machine, player + 41, 0);
                else
                    bs_recomp_write8(machine, player + 56,
                        (uint8_t)(bs_recomp_read8(machine, player + 56) - 1));
                draw_life_icons(machine, i != 0);
                expand_capacity_bars(machine, i != 0);
            }
        } else if (bs_recomp_read8(machine, player + 38) != 0) {
            continue;
        }
        if (countdown_path) {
            set_dreg_byte(&machine->cpu.d[1], 0);
            /* With >=60 countdown frames left all movement bits are masked. */
            if (countdown < 0x3c) set_dreg_byte(&machine->cpu.d[1], 1);
        }
        uint8_t controls = (uint8_t)machine->cpu.d[1];
        if ((controls & 1) && bs_recomp_read16(machine, player + 2) > 0x0102)
            bs_recomp_write16(machine, player + 2,
                (uint16_t)(bs_recomp_read16(machine, player + 2) - 2));
        uint16_t frame =
            (uint16_t)(bs_recomp_read16(machine,
                         machine->cpu.a[5] - 28552) & 3);
        set_dreg_word(&machine->cpu.d[3], frame);
        if (controls & 2) {
            if (frame == 0 && bs_recomp_read16(machine, player + 10) < 12)
                bs_recomp_write16(machine, player + 10,
                    (uint16_t)(bs_recomp_read16(machine, player + 10) + 2));
            if (bs_recomp_read16(machine, player) < 0x0200)
                bs_recomp_write16(machine, player,
                    (uint16_t)(bs_recomp_read16(machine, player) + 2));
        }
        if ((controls & 4) &&
            bs_recomp_read16(machine, player + 2) < 0x01e0)
            bs_recomp_write16(machine, player + 2,
                (uint16_t)(bs_recomp_read16(machine, player + 2) + 2));
        if (controls & 8) {
            if (frame == 0 && bs_recomp_read16(machine, player + 10) != 0)
                bs_recomp_write16(machine, player + 10,
                    (uint16_t)(bs_recomp_read16(machine, player + 10) - 2));
            if (bs_recomp_read16(machine, player) > 0x0100)
                bs_recomp_write16(machine, player,
                    (uint16_t)(bs_recomp_read16(machine, player) - 2));
        }
        if (frame == 0 && !(controls & 0x0a)) {
            uint16_t tilt = bs_recomp_read16(machine, player + 10);
            if (tilt < 6) tilt = (uint16_t)(tilt + 2);
            else if (tilt > 6) tilt = (uint16_t)(tilt - 2);
            bs_recomp_write16(machine, player + 10, tilt);
        }
    }

    bs_recomp_write16(machine, 0x4e40,
                      bs_recomp_read16(machine, 0x4e3c));
    bs_recomp_write16(machine, 0x4e42,
                      bs_recomp_read16(machine, 0x4e3e));
    bs_recomp_write16(machine, 0x4f4a,
                      bs_recomp_read16(machine, 0x4f46));
    bs_recomp_write16(machine, 0x4f4c,
                      bs_recomp_read16(machine, 0x4f48));
    set_dreg_word(&machine->cpu.d[1],
                  bs_recomp_read16(machine, machine->cpu.a[5] + 7204));
    set_dreg_word(&machine->cpu.d[1],
                  (uint16_t)(machine->cpu.d[1] - 0x0100));
    bs_recomp_write16(machine, 0x4e40,
        (uint16_t)(bs_recomp_read16(machine, 0x4e40) +
                   (uint16_t)machine->cpu.d[1]));
    bs_recomp_write16(machine, 0x4f4a,
        (uint16_t)(bs_recomp_read16(machine, 0x4f4a) +
                   (uint16_t)machine->cpu.d[1]));
    machine->cpu.sr &= 0xfff0;
    return BS_RECOMP_OK;
}

/* LAB_5534: construct one sprite record and perform its straight source copy.
 * The original uses blitter channel A with a two-word-wide copy; this native
 * form writes the identical record bytes without emulating a blitter. */
static void append_sprite_record(BsRecomp *machine)
{
    uint16_t x = (uint16_t)(machine->cpu.d[1] + 0x008f);
    uint16_t y = (uint16_t)(machine->cpu.d[2] + 0x0026);
    uint16_t height = (uint16_t)machine->cpu.d[4];
    set_dreg_word(&machine->cpu.d[1], x);
    set_dreg_word(&machine->cpu.d[2], y);
    uint32_t header = machine->cpu.a[1];
    bs_recomp_write32(machine, header, 0);
    machine->cpu.a[1] += 4;
    if (y & 0x0100)
        bs_recomp_write8(machine, header + 3,
                         bs_recomp_read8(machine, header + 3) | 4);
    bs_recomp_write8(machine, header, (uint8_t)y);
    unsigned carry = x & 1;
    x >>= 1;
    set_dreg_word(&machine->cpu.d[1], x);
    if (carry)
        bs_recomp_write8(machine, header + 3,
                         bs_recomp_read8(machine, header + 3) | 1);
    bs_recomp_write8(machine, header + 1, (uint8_t)x);
    y = (uint16_t)(y + height);
    set_dreg_word(&machine->cpu.d[2], y);
    if (y & 0x0100)
        bs_recomp_write8(machine, header + 3,
                         bs_recomp_read8(machine, header + 3) | 2);
    bs_recomp_write8(machine, header + 2, (uint8_t)y);

    set_dreg_word(&machine->cpu.d[5],
                  (uint16_t)machine->cpu.d[5] << 2);
    machine->cpu.a[4] = 0xc6b6 + (uint16_t)machine->cpu.d[5];
    uint32_t source = bs_recomp_read32(machine, machine->cpu.a[4]);
    bs_recomp_write32(machine, machine->cpu.a[6] + 80, source);
    bs_recomp_write32(machine, machine->cpu.a[6] + 84,
                      machine->cpu.a[1]);
    for (uint32_t i = 0; i < (uint32_t)height * 4; i++)
        bs_recomp_write8(machine, machine->cpu.a[1] + i,
                         bs_recomp_read8(machine, source + i));
    set_dreg_word(&machine->cpu.d[4], (uint16_t)(height << 2));
    machine->cpu.a[1] += (uint16_t)machine->cpu.d[4];
    set_dreg_word(&machine->cpu.d[4],
                  (uint16_t)machine->cpu.d[4] << 4);
    set_dreg_word(&machine->cpu.d[4],
                  (uint16_t)(machine->cpu.d[4] + 2));
    bs_recomp_write16(machine, machine->cpu.a[6] + 88,
                      (uint16_t)machine->cpu.d[4]);
    machine->cpu.sr &= 0xfff0;
}

static int build_respawning_ship_records(BsRecomp *machine,
                                         uint32_t player,
                                         uint32_t first_list,
                                         uint32_t second_list)
{
    machine->cpu.a[1] = first_list;
    machine->cpu.a[2] = second_list;
    machine->cpu.a[3] = player;
    set_dreg_word(&machine->cpu.d[3], bs_recomp_read16(machine, player + 2));
    machine->cpu.a[0] = player + 122;
    uint16_t display_offset =
        bs_recomp_read16(machine, machine->cpu.a[5] - 11824);
    machine->cpu.a[1] += display_offset;
    machine->cpu.a[2] += display_offset;

    if (bs_recomp_read16(machine, player + 68) != 0 &&
        bs_recomp_read16(machine, machine->cpu.a[5] - 28550) == 0) {
        set_dreg_word(&machine->cpu.d[1], bs_recomp_read16(machine,
                                                            player + 88));
        machine->cpu.d[2] = 24;
        machine->cpu.d[4] = 36;
        set_dreg_word(&machine->cpu.d[5], 0);
        set_dreg_byte(&machine->cpu.d[5], bs_recomp_read8(machine,
                                                           player + 74));
        append_sprite_record(machine);
        uint32_t swap = machine->cpu.a[1];
        machine->cpu.a[1] = machine->cpu.a[2];
        machine->cpu.a[2] = swap;

        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)(bs_recomp_read16(machine, player + 88) +
                                 0x10));
        machine->cpu.d[2] = 24;
        machine->cpu.d[4] = 36;
        set_dreg_word(&machine->cpu.d[5], 0);
        set_dreg_byte(&machine->cpu.d[5], bs_recomp_read8(machine,
                                                           player + 75));
        append_sprite_record(machine);
        swap = machine->cpu.a[1];
        machine->cpu.a[1] = machine->cpu.a[2];
        machine->cpu.a[2] = swap;
    }

    if (bs_recomp_read8(machine, player + 38) >= 0xc8) {
        snprintf(machine->error, sizeof machine->error,
                 "untranslated dying ship record at $%06x", player);
        return BS_RECOMP_UNTRANSLATED;
    }
    machine->cpu.d[0] = 11;
    for (int slot = 0; slot < 12; slot++) {
        uint32_t satellite = machine->cpu.a[0];
        uint16_t x = bs_recomp_read16(machine, satellite);
        if (x != 0) {
            uint16_t y = bs_recomp_read16(machine, satellite + 2);
            uint8_t delay = bs_recomp_read8(machine, satellite + 10);
            if (delay != 0) {
                delay--;
                bs_recomp_write8(machine, satellite + 10, delay);
                if (delay != 0) goto next_satellite;
                x = (uint16_t)(x +
                               bs_recomp_read16(machine, player + 4));
                y = (uint16_t)(y +
                               bs_recomp_read16(machine, player + 6));
                if (y >= (uint16_t)machine->cpu.d[3]) {
                    bs_recomp_write8(machine, satellite + 10, 1);
                    goto next_satellite;
                }
            } else {
                x = (uint16_t)(x +
                               bs_recomp_read16(machine, satellite + 4));
                y = (uint16_t)(y +
                               bs_recomp_read16(machine, satellite + 6));
                if (y >= (uint16_t)machine->cpu.d[3])
                    goto next_satellite;
            }
            set_dreg_word(&machine->cpu.d[1], x);
            set_dreg_word(&machine->cpu.d[2], y);
            if (y <= 0x00f3 || y >= 0x0200 ||
                x <= (uint16_t)machine->cpu.d[6] ||
                x >= (uint16_t)machine->cpu.d[7]) {
                bs_recomp_write16(machine, satellite, 0);
                goto next_satellite;
            }
            bs_recomp_write16(machine, satellite, x);
            bs_recomp_write16(machine, satellite + 2, y);
            set_dreg_word(&machine->cpu.d[1],
                (uint16_t)(x - bs_recomp_read16(machine,
                                      machine->cpu.a[5] + 7204)));
            set_dreg_word(&machine->cpu.d[2], (uint16_t)(y - 0x0100));
            set_dreg_word(&machine->cpu.d[4], 0);
            set_dreg_word(&machine->cpu.d[5], 0);
            set_dreg_byte(&machine->cpu.d[4],
                          bs_recomp_read8(machine, satellite + 8));
            set_dreg_byte(&machine->cpu.d[5],
                          bs_recomp_read8(machine, satellite + 9));
            append_sprite_record(machine);
            uint32_t satellite_swap = machine->cpu.a[1];
            machine->cpu.a[1] = machine->cpu.a[2];
            machine->cpu.a[2] = satellite_swap;
        }
next_satellite:
        machine->cpu.a[0] += 12;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }

    set_dreg_word(&machine->cpu.d[1], bs_recomp_read16(machine, player));
    set_dreg_word(&machine->cpu.d[2], bs_recomp_read16(machine, player + 2));
    set_dreg_word(&machine->cpu.d[1],
                  (uint16_t)(machine->cpu.d[1] - 0x0100));
    set_dreg_word(&machine->cpu.d[2],
                  (uint16_t)(machine->cpu.d[2] - 0x0100));
    set_dreg_word(&machine->cpu.d[4], bs_recomp_read16(machine, player + 8));
    set_dreg_word(&machine->cpu.d[5], bs_recomp_read16(machine, player + 10));
    append_sprite_record(machine);
    uint32_t swap = machine->cpu.a[1];
    machine->cpu.a[1] = machine->cpu.a[2];
    machine->cpu.a[2] = swap;

    set_dreg_word(&machine->cpu.d[1],
                  (uint16_t)(bs_recomp_read16(machine, player) -
                             0x0100 + 0x10));
    set_dreg_word(&machine->cpu.d[2],
                  (uint16_t)(bs_recomp_read16(machine, player + 2) -
                             0x0100));
    set_dreg_word(&machine->cpu.d[4], bs_recomp_read16(machine, player + 8));
    set_dreg_word(&machine->cpu.d[5],
                  (uint16_t)(bs_recomp_read16(machine, player + 10) + 1));
    append_sprite_record(machine);
    swap = machine->cpu.a[1];
    machine->cpu.a[1] = machine->cpu.a[2];
    machine->cpu.a[2] = swap;
    set_dreg_word(&machine->cpu.d[3], 0x8000);
    bs_recomp_write32(machine, machine->cpu.a[1], 0);
    swap = machine->cpu.a[1];
    machine->cpu.a[1] = machine->cpu.a[2];
    machine->cpu.a[2] = swap;
    bs_recomp_write32(machine, machine->cpu.a[1], 0);

    /* LAB_5490 is inactive unless weapon level is exactly three. */
    uint16_t weapon = bs_recomp_read16(machine, player + 58);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                      (weapon < 3 ? 0x09 : (weapon == 3 ? 0x04 : 0)));
    return BS_RECOMP_OK;
}

static int build_ship_sprite_lists(BsRecomp *machine)
{
    set_dreg_word(&machine->cpu.d[6],
                  bs_recomp_read16(machine, machine->cpu.a[5] + 7204));
    set_dreg_word(&machine->cpu.d[7], (uint16_t)machine->cpu.d[6]);
    set_dreg_word(&machine->cpu.d[6],
                  (uint16_t)(machine->cpu.d[6] - 0x10));
    set_dreg_word(&machine->cpu.d[7],
                  (uint16_t)(machine->cpu.d[7] + 0x120));
    int result = build_respawning_ship_records(machine, 0x4e3c,
                                                0x5f000, 0x5f400);
    if (result != BS_RECOMP_OK) return result;
    return build_respawning_ship_records(machine, 0x4f46,
                                          0x5f800, 0x5fc00);
}

static void update_empty_player_burst(BsRecomp *machine)
{
    uint8_t active = bs_recomp_read8(machine,
                                      machine->cpu.a[5] - 25334);
    if (active == 0) {
        machine->cpu.sr =
            (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
        return;
    }
    const uint32_t base = machine->cpu.a[5];
    if (active == 0xff)
        bs_recomp_write32(machine, base - 25338, 0x171b0);
    machine->cpu.a[4] = bs_recomp_read8(machine, base - 12650)
        ? 0x4e3c : 0x4f46;
    active--;
    bs_recomp_write8(machine, base - 25334, active);
    bs_recomp_write8(machine, machine->cpu.a[4] + 90,
        (uint8_t)(bs_recomp_read8(machine, machine->cpu.a[4] + 90) - 1));
    machine->cpu.a[2] = bs_recomp_read32(machine, base - 25338);
    int16_t divisor = (int16_t)bs_recomp_read16(machine, machine->cpu.a[2]);
    machine->cpu.a[2] += 2;
    set_dreg_word(&machine->cpu.d[3], (uint16_t)divisor);
    if (divisor < 0) {
        bs_recomp_write8(machine, base - 25334, 0);
        bs_recomp_write8(machine, machine->cpu.a[4] + 90, 0);
        machine->cpu.a[0] = 0x4976;
        machine->cpu.d[0] = 15;
        for (int slot = 0; slot < 16; slot++) {
            bs_recomp_write32(machine, machine->cpu.a[0], 0);
            bs_recomp_write8(machine, machine->cpu.a[0] + 19, 0);
            machine->cpu.a[0] += 20;
            set_dreg_word(&machine->cpu.d[0],
                          (uint16_t)(machine->cpu.d[0] - 1));
        }
        machine->cpu.sr =
            (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
        return;
    }
    bs_recomp_write32(machine, base - 25338, machine->cpu.a[2]);
    set_dreg_word(&machine->cpu.d[1],
                  bs_recomp_read16(machine, base - 28552));
    set_dreg_byte(&machine->cpu.d[4], (uint8_t)machine->cpu.d[1]);
    uint16_t vector_offset =
        (uint16_t)(((uint16_t)machine->cpu.d[1] & 7) << 2);
    machine->cpu.a[1] = 0x1727a + vector_offset;
    machine->cpu.a[0] = 0x4976;
    machine->cpu.d[0] = 7;
    int32_t odd_divisor = divisor | 1;
    for (int ray = 0; ray < 8; ray++) {
        int16_t vector_x = (int16_t)bs_recomp_read16(machine,
                                                       machine->cpu.a[1]);
        int16_t vector_y = (int16_t)bs_recomp_read16(machine,
                                                       machine->cpu.a[1] + 2);
        int16_t x = (int16_t)(vector_x / odd_divisor);
        int16_t y = (int16_t)(-vector_y / odd_divisor);
        x = (int16_t)(x +
            (int16_t)bs_recomp_read16(machine, machine->cpu.a[4] + 4) + 8);
        y = (int16_t)(y +
            (int16_t)bs_recomp_read16(machine, machine->cpu.a[4] + 6) - 16);
        set_dreg_word(&machine->cpu.d[1], (uint16_t)x);
        set_dreg_word(&machine->cpu.d[2], (uint16_t)y);
        bs_recomp_write16(machine, machine->cpu.a[0], (uint16_t)x);
        bs_recomp_write16(machine, machine->cpu.a[0] + 4, (uint16_t)y);
        bs_recomp_write8(machine, machine->cpu.a[0] + 16, 0x10);
        set_dreg_byte(&machine->cpu.d[4],
                      (uint8_t)machine->cpu.d[4] & 3);
        bs_recomp_write8(machine, machine->cpu.a[0] + 17,
                         (uint8_t)machine->cpu.d[4] + 0x54);
        set_dreg_byte(&machine->cpu.d[4],
                      (uint8_t)(machine->cpu.d[4] + 1));
        machine->cpu.a[1] += 0x20;
        machine->cpu.a[0] += 0x14;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    if (active >= 0xb0 && (active & 0x0f) == 0x0e) {
        machine->cpu.a[0] = machine->cpu.a[4] + 122;
        machine->cpu.a[1] = 0x3eb0;
        for (int i = 0; i < 36; i++) {
            bs_recomp_write32(machine, machine->cpu.a[0],
                              bs_recomp_read32(machine, machine->cpu.a[1]));
            machine->cpu.a[0] += 4;
            machine->cpu.a[1] += 4;
        }
        machine->cpu.d[0] = 0xffff;
    }
    machine->cpu.sr &= 0xfff0;
}

/* LAB_4ADA: keep the 16 effect records sorted by their integer Y coordinate,
 * advance their fixed-point motion, and emit the four interleaved sprite
 * lists.  The compatibility blits in LAB_5534 are direct source copies in
 * append_sprite_record(), so this path contains no chipset emulation. */
static int update_empty_effect_pool(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];
    uint8_t temporary[20];
    for (int pass = 0; pass < 15; pass++) {
        int swapped = 0;
        for (int slot = 0; slot < 15 - pass; slot++) {
            uint32_t left = 0x4976 + (uint32_t)slot * 20;
            uint32_t right = left + 20;
            if (bs_recomp_read16(machine, right) == 0) break;
            if (bs_recomp_read16(machine, left + 4) <=
                bs_recomp_read16(machine, right + 4))
                continue;
            memcpy(temporary, machine->memory + left, sizeof temporary);
            memcpy(machine->memory + left, machine->memory + right,
                   sizeof temporary);
            memcpy(machine->memory + right, temporary, sizeof temporary);
            swapped = 1;
        }
        if (!swapped) break;
    }
    machine->cpu.a[0] = 0x497a;
    set_dreg_byte(&machine->cpu.d[1], 0);
    machine->cpu.d[0] = 15;
    for (int slot = 0; slot < 16; slot++) {
        uint8_t selector = (uint8_t)machine->cpu.d[1];
        bs_recomp_write8(machine, machine->cpu.a[0] + 14, selector);
        if ((int16_t)bs_recomp_read16(machine, machine->cpu.a[0]) < 0x0116 ||
            (int16_t)bs_recomp_read16(machine, machine->cpu.a[0]) >= 0x01e0) {
            bs_recomp_write8(machine, machine->cpu.a[0] + 14,
                             selector | 8);
            selector = (uint8_t)((selector + 4) & 7);
        } else {
            selector = (uint8_t)((selector + 4) & 15);
        }
        set_dreg_byte(&machine->cpu.d[1], selector);
        machine->cpu.a[0] += 20;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }

    uint32_t display_offset = bs_recomp_read32(machine, base - 11826);
    static const uint32_t bases[] = {0x5e000, 0x5e400, 0x5e800, 0x5ec00};
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++)
        bs_recomp_write32(machine, 0x4aca + i * 4,
                          bases[i] + display_offset);
    set_dreg_word(&machine->cpu.d[6],
                  bs_recomp_read16(machine, base + 7204));
    set_dreg_word(&machine->cpu.d[7], (uint16_t)machine->cpu.d[6]);
    set_dreg_word(&machine->cpu.d[6],
                  (uint16_t)(machine->cpu.d[6] - 0x10));
    set_dreg_word(&machine->cpu.d[7],
                  (uint16_t)(machine->cpu.d[7] + 0x120));
    set_dreg_word(&machine->cpu.d[3], 0);
    machine->cpu.a[0] = 0x4976;
    machine->cpu.d[0] = 15;
    for (int slot = 0; slot < 16; slot++) {
        machine->cpu.d[1] = bs_recomp_read32(machine, machine->cpu.a[0]);
        if (machine->cpu.d[1] != 0) {
            uint32_t effect = machine->cpu.a[0];
            uint32_t x_fixed = bs_recomp_read32(machine, effect);
            uint32_t y_fixed = bs_recomp_read32(machine, effect + 4);
            uint8_t timer = bs_recomp_read8(machine, effect + 19);
            if (timer != 0) {
                int use_second = 0;
                uint16_t x = (uint16_t)(x_fixed >> 16);
                uint16_t y = (uint16_t)(y_fixed >> 16);
                uint16_t first_distance = (uint16_t)(
                    absolute_word(signed_word_sub(
                        (uint16_t)(bs_recomp_read16(machine,
                                      base - 12736) + 0x0c), x)) +
                    absolute_word(signed_word_sub(
                        (uint16_t)(bs_recomp_read16(machine,
                                      base - 12734) + 0x10), y)));
                uint16_t second_distance = (uint16_t)(
                    absolute_word(signed_word_sub(
                        (uint16_t)(bs_recomp_read16(machine,
                                      base - 12470) + 0x0c), x)) +
                    absolute_word(signed_word_sub(
                        (uint16_t)(bs_recomp_read16(machine,
                                      base - 12468) + 0x10), y)));
                if ((int8_t)bs_recomp_read8(machine, base - 12702) < 0)
                    use_second = 1;
                else if ((int8_t)bs_recomp_read8(machine,
                                                  base - 12436) >= 0 &&
                         second_distance <= first_distance)
                    use_second = 1;
                uint16_t target_x = bs_recomp_read16(machine,
                    base + (use_second ? -12470 : -12736));
                uint16_t target_y = bs_recomp_read16(machine,
                    base + (use_second ? -12468 : -12734));
                int32_t vx = (int32_t)bs_recomp_read32(machine, effect + 8);
                int32_t vy = (int32_t)bs_recomp_read32(machine, effect + 12);
                if ((int16_t)(target_x - x) < 0) {
                    if (vx > -0x18000) vx -= 0x800;
                } else if (vx < 0x18000) {
                    vx += 0x800;
                }
                if ((int16_t)(target_y - y) < 0) {
                    if (vy > -0x18000) vy -= 0x800;
                } else if (vy < 0x18000) {
                    vy += 0x800;
                }
                bs_recomp_write32(machine, effect + 8, (uint32_t)vx);
                bs_recomp_write32(machine, effect + 12, (uint32_t)vy);
                if (!(bs_recomp_read8(machine, base - 28551) & 1)) {
                    timer++;
                    bs_recomp_write8(machine, effect + 19, timer);
                }
                if (timer < 0x19) {
                    if (bs_recomp_read8(machine, base - 28551) & 1)
                        bs_recomp_write16(machine, effect + 4,
                            (uint16_t)(y +
                                bs_recomp_read16(machine, base + 7222)));
                } else {
                    x_fixed += (uint32_t)vx;
                    y_fixed += (uint32_t)vy;
                    bs_recomp_write32(machine, effect, x_fixed);
                    bs_recomp_write32(machine, effect + 4, y_fixed);
                }
            } else {
                x_fixed += bs_recomp_read32(machine, effect + 8);
                y_fixed += bs_recomp_read32(machine, effect + 12);
                bs_recomp_write32(machine, effect, x_fixed);
                bs_recomp_write32(machine, effect + 4, y_fixed);
            }

            uint16_t x = bs_recomp_read16(machine, effect);
            uint16_t y = bs_recomp_read16(machine, effect + 4);
            if (y >= 0x0200 || (timer == 0 && y <= 0x00f3) ||
                (timer == 0 &&
                 (x <= (uint16_t)machine->cpu.d[6] ||
                  x >= (uint16_t)machine->cpu.d[7]))) {
                size_t remaining = (size_t)(15 - slot) * 20;
                memmove(machine->memory + effect,
                        machine->memory + effect + 20, remaining);
                memset(machine->memory + 0x4aa2, 0, 20);
                slot--;
                continue;
            }
            if (y >= 0x00f6) {
                set_dreg_word(&machine->cpu.d[1],
                    (uint16_t)(x - bs_recomp_read16(machine, base + 7204)));
                set_dreg_word(&machine->cpu.d[2], (uint16_t)(y - 0x0100));
                set_dreg_word(&machine->cpu.d[4],
                              bs_recomp_read8(machine, effect + 16));
                set_dreg_word(&machine->cpu.d[5],
                              bs_recomp_read8(machine, effect + 17));
                uint8_t selector = bs_recomp_read8(machine, effect + 18);
                machine->cpu.a[2] = 0x4aca + selector;
                machine->cpu.a[1] =
                    bs_recomp_read32(machine, machine->cpu.a[2]);
                append_sprite_record(machine);
                bs_recomp_write32(machine, machine->cpu.a[2],
                                  machine->cpu.a[1]);
            }
        }
        machine->cpu.a[0] += 20;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        machine->cpu.a[1] = bs_recomp_read32(machine, 0x4aca + i * 4);
        bs_recomp_write32(machine, machine->cpu.a[1], 0);
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
    return BS_RECOMP_OK;
}

/* LAB_55BC on its own.  D1 holds the first bitplane address and D2 the stride
 * between them; both are set by the caller. */
static void set_bitplane_pointers(BsRecomp *machine)
{
    machine->cpu.a[1] = 0x558e;
    machine->cpu.d[0] = 7;
    for (int pointer = 0; pointer < 8; pointer++) {
        machine->cpu.a[0] = bs_recomp_read32(machine, machine->cpu.a[1]);
        machine->cpu.a[1] += 4;
        bs_recomp_write16(machine, machine->cpu.a[0],
                          (uint16_t)(machine->cpu.d[1] >> 16));
        bs_recomp_write16(machine, machine->cpu.a[0] + 4,
                          (uint16_t)machine->cpu.d[1]);
        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)(machine->cpu.d[1] +
                                 (uint16_t)machine->cpu.d[2]));
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    machine->cpu.sr &= 0xfff0;
}

/* LAB_55AE falls straight into LAB_55BC with the gameplay playfield base. */
static void update_sprite_bitplane_pointers(BsRecomp *machine)
{
    machine->cpu.d[1] = UINT32_C(0x0005e000) +
        bs_recomp_read32(machine, machine->cpu.a[5] - 11826);
    set_dreg_word(&machine->cpu.d[2], 0x0400);
    set_bitplane_pointers(machine);
}

/* $3D80C is the second entry of the $3D800 overlay's jump table.  LODCOM and
 * LODMUS share that load address and stop the music in entirely different
 * ways, so the resident vector selects the translation rather than the call
 * site.  Both routines end on a MOVE of a positive, non-zero immediate. */
static int request_music_stop(BsRecomp *machine)
{
    uint32_t target = bs_recomp_read32(machine, 0x3d80e);
    if (bs_recomp_read16(machine, 0x3d80c) == 0x4ef9 && target == 0x3dcd8) {
        /* LODCOM: flag a pending stop in the driver state block at $3DEE8. */
        if (bs_recomp_read8(machine, 0x3deeb) == 0)
            bs_recomp_write8(machine, 0x3deeb,
                             bs_recomp_read8(machine, 0x3dee9));
        bs_recomp_write8(machine, 0x3deea, 1);
        machine->cpu.sr &= 0xfff0;
        return BS_RECOMP_OK;
    }
    if (bs_recomp_read16(machine, 0x3d80c) == 0x4ef9 && target == 0x3dce4) {
        /* LODMUS: stop the CIA-B timer that clocks the sequencer and silence
         * the four audio DMA channels, around a master-interrupt window. */
        bs_recomp_write16(machine, 0xdff09a, 0x4000);
        bs_recomp_write8(machine, 0xbfde00, 0);
        bs_recomp_write16(machine, 0xdff09a, 0xc000);
        bs_recomp_write16(machine, 0xdff096, 0x000f);
        machine->cpu.sr &= 0xfff0;
        return BS_RECOMP_OK;
    }
    snprintf(machine->error, sizeof machine->error,
             "untranslated $3D80C music-stop vector $%06x", target);
    return BS_RECOMP_UNTRANSLATED;
}

typedef struct {
    uint16_t min_x, min_y, max_x, max_y;
} CollisionBox;

/* The original comparisons are signed 68000 word comparisons.  The upper
 * edges are exclusive while the lower edges are inclusive. */
static int collision_boxes_overlap(CollisionBox first, CollisionBox second)
{
    return (int16_t)first.min_y < (int16_t)second.max_y &&
           (int16_t)first.max_y >= (int16_t)second.min_y &&
           (int16_t)first.min_x < (int16_t)second.max_x &&
           (int16_t)first.max_x >= (int16_t)second.min_x;
}

static CollisionBox player_shot_box(BsRecomp *machine, uint32_t player,
                                    uint32_t shot)
{
    CollisionBox box;
    box.min_x = box.max_x = bs_recomp_read16(machine, shot);
    box.min_y = box.max_y = bs_recomp_read16(machine, shot + 2);
    if (bs_recomp_read8(machine, player + 90) != 0) {
        box.max_x = (uint16_t)(box.max_x + 0x30);
        box.max_y = (uint16_t)(box.max_y + 0x30);
        box.min_x = (uint16_t)(box.min_x - 0x20);
        box.min_y = (uint16_t)(box.min_y - 0x20);
    } else {
        box.max_x = (uint16_t)(box.max_x +
                                bs_recomp_read16(machine, player + 62));
        box.max_y = (uint16_t)(box.max_y +
                                bs_recomp_read16(machine, player + 64) + 10);
        box.min_y = (uint16_t)(box.min_y - 10);
    }
    return box;
}

/* LAB_3424/LAB_34FA use a negative shot-damage byte for a penetrating shot.
 * It survives the overlap and contributes two points of damage; ordinary
 * shots are consumed and contribute their unsigned damage byte. */
static uint8_t apply_player_shot_hit(BsRecomp *machine, uint32_t shot,
                                    uint32_t damage_mailbox)
{
    int8_t shot_damage = (int8_t)bs_recomp_read8(machine, shot + 11);
    uint8_t damage = shot_damage < 0 ? 2 : (uint8_t)shot_damage;
    if (shot_damage >= 0) bs_recomp_write16(machine, shot, 0);
    bs_recomp_write8(machine, damage_mailbox,
        (uint8_t)(bs_recomp_read8(machine, damage_mailbox) + damage));
    return damage;
}

/* LAB_34FA: the selected player's twelve live shots against the eighteen
 * scenery/enemy BOB records. */
static void collide_player_shots_with_entities(BsRecomp *machine)
{
    uint32_t player = bs_recomp_read32(machine,
                                        machine->cpu.a[5] - 18624);
    machine->cpu.a[4] = player;
    machine->cpu.a[0] = player + 122;
    machine->cpu.d[7] = 11;
    for (int shot_index = 0; shot_index < 12; shot_index++) {
        uint32_t shot = machine->cpu.a[0];
        set_dreg_word(&machine->cpu.d[1],
                      bs_recomp_read16(machine, shot));
        if ((uint16_t)machine->cpu.d[1] != 0) {
            CollisionBox shot_box = player_shot_box(machine, player, shot);
            set_dreg_word(&machine->cpu.d[1], shot_box.max_x);
            set_dreg_word(&machine->cpu.d[2], shot_box.max_y);
            set_dreg_word(&machine->cpu.d[3], shot_box.min_x);
            set_dreg_word(&machine->cpu.d[4], shot_box.min_y);
            machine->cpu.a[1] = 0x2e040;
            machine->cpu.d[0] = 17;
            for (int entity_index = 0; entity_index < 18; entity_index++) {
                uint32_t entity = machine->cpu.a[1];
                if (bs_recomp_read16(machine, entity) != 0) {
                    CollisionBox entity_box = {
                        bs_recomp_read16(machine, entity + 48),
                        bs_recomp_read16(machine, entity + 52),
                        bs_recomp_read16(machine, entity + 50),
                        bs_recomp_read16(machine, entity + 54),
                    };
                    if (!(bs_recomp_read8(machine, entity + 31) & 0x04) &&
                        collision_boxes_overlap(shot_box, entity_box)) {
                        apply_player_shot_hit(machine, shot, entity + 24);
                        break;
                    }
                }
                machine->cpu.a[1] += 0x40;
                set_dreg_word(&machine->cpu.d[0],
                              (uint16_t)(machine->cpu.d[0] - 1));
            }
        }
        machine->cpu.a[0] += 12;
        set_dreg_word(&machine->cpu.d[7],
                      (uint16_t)(machine->cpu.d[7] - 1));
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

/* LAB_3424: the same player-shot pass against the twelve 80-byte hostile and
 * collectable records.  The projectile updater consumes byte 62 next frame. */
static void collide_player_shots_with_projectiles(BsRecomp *machine)
{
    uint32_t player = bs_recomp_read32(machine,
                                        machine->cpu.a[5] - 18624);
    machine->cpu.a[4] = player;
    machine->cpu.a[0] = player + 122;
    machine->cpu.d[7] = 11;
    for (int shot_index = 0; shot_index < 12; shot_index++) {
        uint32_t shot = machine->cpu.a[0];
        set_dreg_word(&machine->cpu.d[1],
                      bs_recomp_read16(machine, shot));
        if ((uint16_t)machine->cpu.d[1] != 0) {
            CollisionBox shot_box = player_shot_box(machine, player, shot);
            set_dreg_word(&machine->cpu.d[1], shot_box.max_x);
            set_dreg_word(&machine->cpu.d[2], shot_box.max_y);
            set_dreg_word(&machine->cpu.d[3], shot_box.min_x);
            set_dreg_word(&machine->cpu.d[4], shot_box.min_y);
            machine->cpu.a[1] = 0x2dc80;
            machine->cpu.d[0] = 11;
            for (int projectile_index = 0; projectile_index < 12;
                 projectile_index++) {
                uint32_t projectile = machine->cpu.a[1];
                if (bs_recomp_read16(machine, projectile) != 0) {
                    CollisionBox projectile_box = {
                        bs_recomp_read16(machine, projectile + 16),
                        bs_recomp_read16(machine, projectile + 20),
                        bs_recomp_read16(machine, projectile + 18),
                        bs_recomp_read16(machine, projectile + 22),
                    };
                    if (bs_recomp_read8(machine, projectile + 29) == 0 &&
                        !(bs_recomp_read8(machine, projectile + 30) & 0x80) &&
                        collision_boxes_overlap(shot_box, projectile_box)) {
                        apply_player_shot_hit(machine, shot,
                                              projectile + 62);
                        break;
                    }
                }
                machine->cpu.a[1] += 0x50;
                set_dreg_word(&machine->cpu.d[0],
                              (uint16_t)(machine->cpu.d[0] - 1));
            }
        }
        machine->cpu.a[0] += 12;
        set_dreg_word(&machine->cpu.d[7],
                      (uint16_t)(machine->cpu.d[7] - 1));
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

static void hit_player(BsRecomp *machine, uint32_t player)
{
    bs_recomp_write8(machine, player + 38, 0x64);
    bs_recomp_write8(machine, player + 49, 0x46);
    bs_recomp_write16(machine, player + 52, 0x270f);
    bs_recomp_write16(machine, player + 2,
        (uint16_t)(bs_recomp_read16(machine, player + 2) - 0x0c));
}

/* LAB_369A.  HUD/sound notifications remain separate, but the original
 * collectable, Nova and weapon-state mutations happen atomically here. */
static void collect_pickup(BsRecomp *machine, uint32_t player,
                           uint32_t pickup)
{
    uint8_t subtype = bs_recomp_read8(machine, pickup + 28);
    bs_recomp_write16(machine, pickup, 0);
    if (subtype == 0x0a) {
        uint16_t charges = bs_recomp_read16(machine, player + 66);
        if (charges < 8) bs_recomp_write16(machine, player + 66,
                                            (uint16_t)(charges + 1));
        return;
    }

    for (int shot = 0; shot < 12; shot++)
        bs_recomp_write16(machine, player + 122 + (uint32_t)shot * 12, 0);
    bs_recomp_write8(machine, player + 59, subtype >> 1);
    uint16_t level = bs_recomp_read16(machine, player + 60);
    if (level < 5) bs_recomp_write16(machine, player + 60,
                                      (uint16_t)(level + 1));
}

static void collide_player_with_effects(BsRecomp *machine)
{
    uint8_t burst = bs_recomp_read8(machine, machine->cpu.a[5] - 25334);
    if (burst != 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                          ((burst & 0x80) ? 0x08 : 0));
        return;
    }

    uint32_t player = bs_recomp_read32(machine,
                                        machine->cpu.a[5] - 18624);
    machine->cpu.a[4] = player;
    uint16_t invulnerability = bs_recomp_read16(machine, player + 52);
    if (invulnerability != 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                          ((invulnerability & 0x8000) ? 0x08 : 0));
        return;
    }

    CollisionBox ship = {
        bs_recomp_read16(machine, player + 4),
        bs_recomp_read16(machine, player + 6),
        (uint16_t)(bs_recomp_read16(machine, player + 4) + 0x19),
        (uint16_t)(bs_recomp_read16(machine, player + 6) + 0x17),
    };
    set_dreg_word(&machine->cpu.d[1], ship.max_x);
    set_dreg_word(&machine->cpu.d[2], ship.max_y);
    set_dreg_word(&machine->cpu.d[3], ship.min_x);
    set_dreg_word(&machine->cpu.d[4], ship.min_y);
    machine->cpu.a[0] = 0x4976;
    for (int effect_index = 0; effect_index < 16; effect_index++) {
        uint32_t effect = machine->cpu.a[0];
        uint16_t x = bs_recomp_read16(machine, effect);
        if (x == 0) break;
        uint16_t y = bs_recomp_read16(machine, effect + 4);
        CollisionBox point = {x, y, x, y};
        if (collision_boxes_overlap(ship, point)) hit_player(machine, player);
        machine->cpu.a[0] += 20;
    }
    machine->cpu.sr &= 0xfff0;
}

static void collide_player_with_projectiles(BsRecomp *machine)
{
    uint32_t player = bs_recomp_read32(machine,
                                        machine->cpu.a[5] - 18624);
    machine->cpu.a[4] = player;
    set_dreg_word(&machine->cpu.d[1],
                  bs_recomp_read16(machine, player + 4));
    set_dreg_word(&machine->cpu.d[2],
                  bs_recomp_read16(machine, player + 6));
    set_dreg_word(&machine->cpu.d[3], (uint16_t)machine->cpu.d[1]);
    set_dreg_word(&machine->cpu.d[4], (uint16_t)machine->cpu.d[2]);
    set_dreg_word(&machine->cpu.d[1],
                  (uint16_t)(machine->cpu.d[1] + 0x19));
    set_dreg_word(&machine->cpu.d[2],
                  (uint16_t)(machine->cpu.d[2] + 0x17));
    set_dreg_word(&machine->cpu.d[3],
                  (uint16_t)(machine->cpu.d[3] + 7));
    set_dreg_word(&machine->cpu.d[4],
                  (uint16_t)(machine->cpu.d[4] + 7));
    machine->cpu.a[0] = 0x2dc80;
    machine->cpu.d[0] = 11;
    for (int projectile = 0; projectile < 12; projectile++) {
        uint32_t record = machine->cpu.a[0];
        uint8_t flags = bs_recomp_read8(machine, record + 30);
        bs_recomp_write8(machine, record + 30,
                         flags & (uint8_t)~0x40);
        if (bs_recomp_read16(machine, record) != 0) {
            CollisionBox ship = {
                (uint16_t)machine->cpu.d[3],
                (uint16_t)machine->cpu.d[4],
                (uint16_t)machine->cpu.d[1],
                (uint16_t)machine->cpu.d[2],
            };
            CollisionBox hostile = {
                bs_recomp_read16(machine, record + 16),
                bs_recomp_read16(machine, record + 20),
                bs_recomp_read16(machine, record + 18),
                bs_recomp_read16(machine, record + 22),
            };
            if (bs_recomp_read8(machine, record + 29) == 0 &&
                !(flags & 0x08) &&
                collision_boxes_overlap(ship, hostile)) {
                uint8_t type = bs_recomp_read8(machine, record + 31);
                if (type == 5) {
                    collect_pickup(machine, player, record);
                } else if (!(type == 6 &&
                             bs_recomp_read8(machine, record + 63) >= 5) &&
                           bs_recomp_read16(machine, player + 52) == 0) {
                    bs_recomp_write8(machine, record + 62,
                        (uint8_t)(bs_recomp_read8(machine, record + 62) +
                                  10));
                    hit_player(machine, player);
                }
            }
        }
        machine->cpu.a[0] += 0x50;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

static void draw_four_text_pairs(BsRecomp *machine)
{
    machine->cpu.d[0] = 3;
    for (int pair = 0; pair < 4; pair++) {
        machine->cpu.a[2] = 0x10550;
        machine->cpu.a[3] = 0x10550;
        set_dreg_word(&machine->cpu.d[1], 0);
        set_dreg_byte(&machine->cpu.d[1],
                      bs_recomp_read8(machine, machine->cpu.a[1]++));
        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)machine->cpu.d[1] * 2);
        set_dreg_word(&machine->cpu.d[2], (uint16_t)machine->cpu.d[1]);
        set_dreg_word(&machine->cpu.d[2],
                      (uint16_t)machine->cpu.d[2] << 2);
        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)(machine->cpu.d[1] +
                                 (uint16_t)machine->cpu.d[2]));
        machine->cpu.a[2] += (uint16_t)machine->cpu.d[1];
        set_dreg_word(&machine->cpu.d[1], 0);
        set_dreg_byte(&machine->cpu.d[1],
                      bs_recomp_read8(machine, machine->cpu.a[1]++));
        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)machine->cpu.d[1] * 2);
        set_dreg_word(&machine->cpu.d[2], (uint16_t)machine->cpu.d[1]);
        set_dreg_word(&machine->cpu.d[2],
                      (uint16_t)machine->cpu.d[2] << 2);
        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)(machine->cpu.d[1] +
                                 (uint16_t)machine->cpu.d[2]));
        machine->cpu.a[3] += (uint16_t)machine->cpu.d[1];
        uint8_t last = 0;
        for (int row = 0; row < 8; row++) {
            bs_recomp_write8(machine,
                              machine->cpu.a[0] + row * 116,
                              bs_recomp_read8(machine, machine->cpu.a[2]));
            machine->cpu.a[2]++;
            last = bs_recomp_read8(machine, machine->cpu.a[3]);
            bs_recomp_write8(machine,
                              machine->cpu.a[0] + row * 116 + 1, last);
            machine->cpu.a[3]++;
        }
        machine->cpu.a[0] += 8;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                          (last == 0 ? 0x04 :
                           ((last & 0x80) ? 0x08 : 0)));
    }
}

static void update_demo_scoreboard(BsRecomp *machine)
{
    bs_recomp_write32(machine, machine->cpu.a[5] - 15160,
                      bs_recomp_read32(machine, machine->cpu.a[5] + 32026));
    bs_recomp_write32(machine, machine->cpu.a[5] - 15156,
                      bs_recomp_read32(machine, machine->cpu.a[5] + 32030));
    machine->cpu.a[0] = 0xb788;
    machine->cpu.a[1] = 0x4ea6;
    draw_four_text_pairs(machine);
    machine->cpu.a[0] = 0xb7cc;
    machine->cpu.a[1] = 0x4fb0;
    draw_four_text_pairs(machine);
    machine->cpu.a[0] = 0xb7a8;
    machine->cpu.a[1] = 0x44c8;
    if (bs_recomp_read8(machine, machine->cpu.a[5] - 28516) != 0) {
        uint16_t counter = (uint16_t)(
            bs_recomp_read16(machine, machine->cpu.a[5] - 28550) + 1);
        bs_recomp_write16(machine, machine->cpu.a[5] - 28550, counter);
        bs_recomp_write32(machine, machine->cpu.a[1], 0x20202020);
        bs_recomp_write32(machine, machine->cpu.a[1] + 4, 0x20202020);
        machine->cpu.a[2] = 0x45b2;
        if (counter < 0x05dc) {
            set_dreg_byte(&machine->cpu.d[1],
                          bs_recomp_read8(machine,
                              machine->cpu.a[5] - 28551));
            set_dreg_byte(&machine->cpu.d[1],
                          (uint8_t)machine->cpu.d[1] & 0x1f);
            if ((uint8_t)machine->cpu.d[1] < 0x18) {
                bs_recomp_write32(machine, machine->cpu.a[1],
                                  bs_recomp_read32(machine,
                                                   machine->cpu.a[2]));
                machine->cpu.a[2] += 4;
                bs_recomp_write32(machine, machine->cpu.a[1] + 4,
                                  bs_recomp_read32(machine,
                                                   machine->cpu.a[2]));
            }
        }
    }
    draw_four_text_pairs(machine);
}

static void update_inactive_credit_state(BsRecomp *machine)
{
    bs_recomp_write8(machine, machine->cpu.a[5] - 16120, 0);
    /* Both demo ships are present and carry a marker below $C8.  The routine
     * therefore skips credit/game-over handling and finishes on player two's
     * marker comparison. */
    uint8_t marker = bs_recomp_read8(machine, 0x4f46 + 38);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                      (marker == 0xc8 ? 0x04 :
                       (marker < 0xc8 ? 0x09 : 0)));
}

static void update_inactive_stage_palette(BsRecomp *machine)
{
    uint16_t stage = bs_recomp_read16(machine, machine->cpu.a[5] + 7228);
    if (stage != 3 &&
        bs_recomp_read8(machine, machine->cpu.a[5] - 27618) == 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
        return;
    }
    /* This branch is reached later in the run and remains fail-closed at its
     * dispatcher edge until its palette animation is parity-covered. */
    machine->cpu.sr &= 0xfff0;
}

static void update_frame_palette_accents(BsRecomp *machine)
{
    set_dreg_word(&machine->cpu.d[1],
                  bs_recomp_read16(machine, machine->cpu.a[5] - 28552));
    set_dreg_word(&machine->cpu.d[1],
                  (uint16_t)machine->cpu.d[1] & 0x000e);
    machine->cpu.a[1] = bs_recomp_read8(machine,
                                        machine->cpu.a[5] - 25334)
        ? 0x1f4e : 0x1f1e;
    machine->cpu.a[1] += (uint16_t)machine->cpu.d[1];
    uint16_t colours[3] = {
        bs_recomp_read16(machine, machine->cpu.a[1]),
        bs_recomp_read16(machine, machine->cpu.a[1] + 16),
        bs_recomp_read16(machine, machine->cpu.a[1] + 32),
    };
    bs_recomp_write16(machine, machine->cpu.a[5] + 13044, colours[0]);
    bs_recomp_write16(machine, machine->cpu.a[5] + 15132, colours[0]);
    bs_recomp_write16(machine, machine->cpu.a[5] + 13048, colours[1]);
    bs_recomp_write16(machine, machine->cpu.a[5] + 15136, colours[1]);
    bs_recomp_write16(machine, machine->cpu.a[5] + 13052, colours[2]);
    bs_recomp_write16(machine, machine->cpu.a[5] + 15140, colours[2]);
    machine->cpu.sr &= 0xfff0;
}

static void spawn_stationary_effect(BsRecomp *machine, uint16_t x,
                                    uint16_t y)
{
    const uint32_t base = machine->cpu.a[5];
    if (bs_recomp_read8(machine, base - 25334) != 0 ||
        ((int8_t)bs_recomp_read8(machine, base - 12702) < 0 &&
         (int8_t)bs_recomp_read8(machine, base - 12436) < 0))
        return;

    unsigned reserved = bs_recomp_read16(machine, base + 10062);
    if (reserved < 16 &&
        bs_recomp_read16(machine, 0x4976 + reserved * 20) != 0)
        return;

    unsigned insertion = 0;
    while (insertion < 16) {
        uint32_t effect = 0x4976 + insertion * 20;
        if (bs_recomp_read16(machine, effect) == 0 ||
            y <= bs_recomp_read16(machine, effect + 4))
            break;
        insertion++;
    }
    if (insertion == 16) return;
    if (insertion < 15) {
        memmove(machine->memory + 0x4976 + (insertion + 1) * 20,
                machine->memory + 0x4976 + insertion * 20,
                (size_t)(15 - insertion) * 20);
    }
    uint32_t effect = 0x4976 + insertion * 20;
    memset(machine->memory + effect, 0, 20);
    bs_recomp_write16(machine, effect, x);
    bs_recomp_write16(machine, effect + 4, y);
    bs_recomp_write32(machine, effect + 8, 0);
    bs_recomp_write32(machine, effect + 12, 0);
    bs_recomp_write32(machine, effect + 16,
        bs_recomp_read8(machine, 0x79de)
            ? UINT32_C(0x0c700018)
            : UINT32_C(0x0c600001));
}

static int scan_pending_impact_flags(BsRecomp *machine)
{
    machine->cpu.a[0] = 0x2dc80;
    machine->cpu.d[0] = 11;
    for (int projectile = 0; projectile < 12; projectile++) {
        uint32_t record = machine->cpu.a[0];
        uint8_t flags = bs_recomp_read8(machine, record + 30);
        if (flags & 0x20) {
            /* LAB_471C/LAB_47D2: consume the one-shot impact marker and
             * insert the resulting debris/explosion into the effect pool.
             * The effect updater owns subsequent movement and rendering. */
            bs_recomp_write8(machine, record + 30,
                             flags & (uint8_t)~0x20);
            if (bs_recomp_read16(machine, record) != 0) {
                uint16_t x = bs_recomp_read16(machine, record + 58);
                uint16_t y = bs_recomp_read16(machine, record + 60);
                bs_recomp_write16(machine, machine->cpu.a[5] - 14398, x);
                bs_recomp_write16(machine, machine->cpu.a[5] - 14396, y);
                bs_recomp_write8(machine, machine->cpu.a[5] - 14384,
                                 bs_recomp_read8(machine, record + 31) == 9
                                     ? 0xff : 0);
                spawn_stationary_effect(machine, x, y);
            }
        }
        machine->cpu.a[0] += 0x50;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    machine->cpu.a[0] = 0x2e040;
    machine->cpu.d[0] = 17;
    for (int entity = 0; entity < 18; entity++) {
        if (bs_recomp_read8(machine, machine->cpu.a[0] + 31) & 0x20) {
            uint8_t type = bs_recomp_read8(machine,
                                            machine->cpu.a[0] + 17);
            if (bs_recomp_read16(machine, machine->cpu.a[0]) == 0) {
                /* The original leaves the flag set on an empty record and
                 * simply advances to the next slot. */
            } else if (type == 0x25 || type == 0x26) {
                uint16_t x = bs_recomp_read16(machine,
                                               machine->cpu.a[0] + 38);
                uint16_t y = bs_recomp_read16(machine,
                                               machine->cpu.a[0] + 40);
                bs_recomp_write16(machine, machine->cpu.a[5] - 14398, x);
                bs_recomp_write16(machine, machine->cpu.a[5] - 14396, y);
                bs_recomp_write8(machine, machine->cpu.a[5] - 14384, 0xff);
                spawn_stationary_effect(machine, x, y);
            } else {
                snprintf(machine->error, sizeof machine->error,
                         "untranslated entity impact type $%02x at $%06x",
                         type, machine->cpu.a[0]);
                return BS_RECOMP_UNTRANSLATED;
            }
        }
        machine->cpu.a[0] += 0x40;
        set_dreg_word(&machine->cpu.d[0],
                      (uint16_t)(machine->cpu.d[0] - 1));
    }
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
    return BS_RECOMP_OK;
}

static void finish_gameplay_frame(BsRecomp *machine)
{
    bs_recomp_write16(machine, machine->cpu.a[5] - 28552,
        (uint16_t)(bs_recomp_read16(machine,
                                     machine->cpu.a[5] - 28552) + 1));
    bs_recomp_write16(machine, machine->cpu.a[5] - 11824, 0);
    if (bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 1)
        bs_recomp_write16(machine, machine->cpu.a[5] - 11824, 0x2000);
    bs_recomp_write32(machine, machine->cpu.a[6] + 68, 0xffffffff);
    bs_recomp_write16(machine, machine->cpu.a[6] + 64, 0x09f0);
    bs_recomp_write16(machine, machine->cpu.a[6] + 66, 0);
    bs_recomp_write16(machine, machine->cpu.a[6] + 100, 0);
    bs_recomp_write16(machine, machine->cpu.a[6] + 102, 0);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

static int update_scheduled_projectiles(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];

    for (;;) {
        machine->cpu.a[1] = bs_recomp_read32(machine, base - 2736);
        set_dreg_word(&machine->cpu.d[1],
                      bs_recomp_read16(machine, base + 7206));
        uint16_t current = (uint16_t)machine->cpu.d[1];
        uint16_t next = bs_recomp_read16(machine, machine->cpu.a[1]);
        if (current < next) {
            /* CMP.W (A1),D1 on the game's positive scroll counters. */
            machine->cpu.sr =
                (uint16_t)((machine->cpu.sr & 0xfff0) | 0x09);
            return BS_RECOMP_OK;
        }
        if (bs_recomp_read16(machine, base + 7222) == 0) {
            machine->cpu.sr =
                (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            return BS_RECOMP_OK;
        }

        set_dreg_word(&machine->cpu.d[1],
                      bs_recomp_read16(machine, machine->cpu.a[1] + 2));
        set_dreg_word(&machine->cpu.d[2],
                      bs_recomp_read16(machine, machine->cpu.a[1] + 4));
        machine->cpu.d[4] =
            bs_recomp_read32(machine, machine->cpu.a[1] + 8);
        set_dreg_word(&machine->cpu.d[3], 0);
        set_dreg_byte(&machine->cpu.d[3],
                      bs_recomp_read8(machine, machine->cpu.a[1] + 6));
        bs_recomp_write32(machine, base - 2736,
                          machine->cpu.a[1] + 12);

        if ((uint16_t)machine->cpu.d[1] == 0xffff) {
            machine->cpu.d[0] = 11;
            machine->cpu.a[0] = 0x2dc80;
            for (int slot = 0; slot < 12; slot++) {
                bs_recomp_write16(machine, machine->cpu.a[0], 0);
                machine->cpu.a[0] += 0x50;
                set_dreg_word(&machine->cpu.d[0],
                              (uint16_t)(machine->cpu.d[0] - 1));
            }
            continue;
        }

        if ((uint8_t)machine->cpu.d[3] == 6) {
            uint8_t random = next_wave_random(machine);
            set_dreg_word(&machine->cpu.d[3], random & 0x7f);
            set_dreg_word(&machine->cpu.d[1],
                          (uint16_t)(machine->cpu.d[3] + 0x40));
            machine->cpu.d[3] = 6;
        }
        if ((uint8_t)machine->cpu.d[3] == 8 ||
            (uint8_t)machine->cpu.d[3] == 1) {
            uint16_t projectile_type = (uint16_t)machine->cpu.d[3];
            uint16_t random_x = next_wave_random(machine);
            set_dreg_word(&machine->cpu.d[1], random_x);
            uint16_t random_y = next_wave_random(machine) & 0x7f;
            set_dreg_word(&machine->cpu.d[3], random_y);
            set_dreg_word(&machine->cpu.d[1],
                          (uint16_t)(machine->cpu.d[1] + random_y - 0x40));
            set_dreg_word(&machine->cpu.d[3], projectile_type);
        }

        /* LAB_75E4: when the game-over gate is clear, find a free record by
         * scanning the twelve-entry projectile pool from the end. */
        if (bs_recomp_read8(machine, base - 16120) != 0 &&
            bs_recomp_read8(machine, base - 28516) == 0)
            continue;
        machine->cpu.a[0] = 0x2dff0;
        machine->cpu.d[0] = 11;
        while (bs_recomp_read16(machine, machine->cpu.a[0]) != 0) {
            machine->cpu.a[0] -= 0x50;
            set_dreg_word(&machine->cpu.d[0],
                          (uint16_t)(machine->cpu.d[0] - 1));
            if ((uint16_t)machine->cpu.d[0] == 0xffff)
                break;
        }
        if ((uint16_t)machine->cpu.d[0] == 0xffff)
            continue;

        uint32_t record = machine->cpu.a[0];
        bs_recomp_write8(machine, record + 31,
                         (uint8_t)machine->cpu.d[3]);
        machine->cpu.d[3] =
            (uint16_t)machine->cpu.d[3] * UINT32_C(0x20);
        machine->cpu.a[2] = 0xcd7a + (uint16_t)machine->cpu.d[3];
        uint16_t x = (uint16_t)machine->cpu.d[1];
        if (x >= 0x0320)
            x = (uint16_t)(x - 0x03e8 + 0x0100);
        else
            x = (uint16_t)(x + bs_recomp_read16(machine, base + 7204));
        set_dreg_word(&machine->cpu.d[1], x);
        uint16_t y = (uint16_t)(machine->cpu.d[2] + 0x0100);
        set_dreg_word(&machine->cpu.d[2], y);
        bs_recomp_write16(machine, record + 0, x);
        bs_recomp_write16(machine, record + 2, 0);
        bs_recomp_write16(machine, record + 4, y);
        bs_recomp_write16(machine, record + 6, 0);
        bs_recomp_write32(machine, record + 8, 0);
        bs_recomp_write32(machine, record + 12, machine->cpu.d[4]);
        bs_recomp_write32(machine, record + 16,
                          bs_recomp_read32(machine, machine->cpu.a[2] + 4));
        bs_recomp_write32(machine, record + 20,
                          bs_recomp_read32(machine, machine->cpu.a[2] + 8));
        bs_recomp_write16(machine, record + 24,
                          bs_recomp_read16(machine, machine->cpu.a[2] + 12));
        if (bs_recomp_read8(machine, base - 2732) == 0) {
            uint8_t reduction =
                (uint8_t)((bs_recomp_read8(machine, record + 24) + 1) >> 2);
            set_dreg_byte(&machine->cpu.d[1], reduction);
            bs_recomp_write8(machine, record + 24,
                (uint8_t)(bs_recomp_read8(machine, record + 24) - reduction));
        }
        bs_recomp_write8(machine, record + 26, 0);
        bs_recomp_write8(machine, record + 27,
                          bs_recomp_read8(machine, machine->cpu.a[2] + 26));
        bs_recomp_write8(machine, record + 28,
                          bs_recomp_read8(machine, machine->cpu.a[2] + 27));
        bs_recomp_write8(machine, record + 29, 0);
        bs_recomp_write8(machine, record + 30, 0);
        bs_recomp_write32(machine, record + 32,
                          bs_recomp_read32(machine, machine->cpu.a[2] + 16));
        bs_recomp_write32(machine, record + 36,
                          bs_recomp_read32(machine, machine->cpu.a[2] + 20));
        bs_recomp_write32(machine, record + 40, 0);
        bs_recomp_write16(machine, record + 50,
                          bs_recomp_read16(machine, machine->cpu.a[2]));
        uint16_t width =
            bs_recomp_read16(machine, machine->cpu.a[2] + 2);
        set_dreg_word(&machine->cpu.d[1], width);
        bs_recomp_write16(machine, record + 52, width);
        uint16_t d2 = (uint16_t)((width - 1) << 4);
        set_dreg_word(&machine->cpu.d[2], d2);
        bs_recomp_write16(machine, record + 68, d2);
        uint16_t doubled_width = (uint16_t)(width + width);
        set_dreg_word(&machine->cpu.d[1],
                      (uint16_t)(0x30 - doubled_width));
        bs_recomp_write16(machine, record + 48,
                          (uint16_t)machine->cpu.d[1]);
        d2 = (uint16_t)(doubled_width - 2);
        machine->cpu.d[2] =
            (uint32_t)d2 * bs_recomp_read16(machine, record + 50);
        bs_recomp_write16(machine, record + 44,
                          (uint16_t)machine->cpu.d[2]);
        set_dreg_word(&machine->cpu.d[2],
            (uint16_t)(((uint16_t)machine->cpu.d[2] << 2) +
                       bs_recomp_read16(machine, record + 44) * 2));
        bs_recomp_write16(machine, record + 46,
                          (uint16_t)machine->cpu.d[2]);
        bs_recomp_write16(machine, record + 54,
                          bs_recomp_read16(machine, machine->cpu.a[2] + 24));
        bs_recomp_write8(machine, record + 57, 0);
        bs_recomp_write8(machine, record + 62, 0);
        bs_recomp_write8(machine, record + 63, 0);
        bs_recomp_write32(machine, record + 64,
                          bs_recomp_read32(machine, machine->cpu.a[2] + 28));
    }
}

static void update_selected_player_score(BsRecomp *machine)
{
    machine->cpu.a[4] =
        (bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 4)
            ? 0x4f46 : 0x4e3c;
    set_dreg_word(&machine->cpu.d[1],
                  bs_recomp_read16(machine, machine->cpu.a[4] + 106));
    if ((uint16_t)machine->cpu.d[1] == 0x3030) {
        set_dreg_byte(&machine->cpu.d[1],
                      bs_recomp_read8(machine, machine->cpu.a[4] + 108));
        set_dreg_byte(&machine->cpu.d[2],
                      bs_recomp_read8(machine, machine->cpu.a[4] + 116));
    }
    bs_recomp_write32(machine, machine->cpu.a[4] + 114,
                      bs_recomp_read32(machine, machine->cpu.a[4] + 106));
    machine->cpu.sr &= 0xfff0;
}

static void update_hud_palette(BsRecomp *machine, uint32_t player,
                               uint32_t table, uint32_t destination,
                               uint16_t default_a, uint16_t default_b)
{
    machine->cpu.a[2] = table;
    machine->cpu.a[3] = destination;
    machine->cpu.a[4] = player;
    set_dreg_word(&machine->cpu.d[5], default_a);
    set_dreg_word(&machine->cpu.d[6], default_b);
    if (bs_recomp_read16(machine, player + 52) != 0) {
        if (bs_recomp_read16(machine, player + 52) >= 0x32 ||
            (bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 1)) {
            uint16_t offset = (uint16_t)(
                bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 0x0e);
            set_dreg_word(&machine->cpu.d[1], offset);
            machine->cpu.a[2] += offset;
            set_dreg_word(&machine->cpu.d[5],
                          bs_recomp_read16(machine, machine->cpu.a[2]));
            set_dreg_word(&machine->cpu.d[6],
                          bs_recomp_read16(machine, machine->cpu.a[2] + 16));
        }
    } else if ((uint16_t)machine->cpu.d[5] ==
               bs_recomp_read16(machine, destination)) {
        return;
    }
    static const uint16_t offsets[] = {
        0, 4, 16, 20, 32, 36, 48, 52, 64, 68, 80, 84, 320, 324,
    };
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i += 2) {
        bs_recomp_write16(machine, destination + offsets[i],
                          (uint16_t)machine->cpu.d[5]);
        bs_recomp_write16(machine, destination + offsets[i + 1],
                          (uint16_t)machine->cpu.d[6]);
    }
}

static void build_player_hud_list(BsRecomp *machine, uint32_t player,
                                  uint32_t list, uint32_t sprite,
                                  uint32_t colour_words, uint16_t bias)
{
    machine->cpu.a[1] = list;
    machine->cpu.a[4] = player;
    machine->cpu.a[3] = sprite;
    machine->cpu.a[0] = colour_words;
    set_dreg_word(&machine->cpu.d[3], bias);

    uint16_t d1 = (uint16_t)(bs_recomp_read8(machine, player + 59) & 3);
    d1 <<= 4;
    set_dreg_word(&machine->cpu.d[1], d1);
    machine->cpu.a[0] += d1;
    machine->cpu.a[2] = 0x272e + (uint32_t)(d1 << 1);
    d1 = (uint16_t)(bs_recomp_read16(machine,
                                     machine->cpu.a[5] - 28552) & 6);
    d1 <<= 2;
    set_dreg_word(&machine->cpu.d[1], d1);
    machine->cpu.a[2] += d1;
    bs_recomp_write16(machine, machine->cpu.a[0],
                      bs_recomp_read16(machine, machine->cpu.a[2]));
    bs_recomp_write16(machine, machine->cpu.a[0] + 4,
                      bs_recomp_read16(machine, machine->cpu.a[2] + 2));
    bs_recomp_write16(machine, machine->cpu.a[0] + 8,
                      bs_recomp_read16(machine, machine->cpu.a[2] + 4));

    uint16_t d4;
    if (bs_recomp_read8(machine, player + 90) != 0) {
        machine->cpu.d[4] = 10;
        d4 = 10;
    } else {
        if (bs_recomp_read16(machine, player + 68) != 0) {
            machine->cpu.a[2] = 0x2706;
            uint16_t d2 = (uint16_t)(bias + 0x0b);
            uint32_t charge = bs_recomp_read32(machine, player + 76);
            if (charge >= 0x00010c10) {
                machine->cpu.a[2] = 0x271a;
                d2 = (uint16_t)(d2 + 0x0a);
            }
            if (charge >= 0x00010f10) {
                machine->cpu.a[2] = 0x271a;
                d2 = (uint16_t)(d2 + 0x0a);
            }
            machine->cpu.d[0] = 9;
            for (int item = 0; item < 10; item++) {
                bs_recomp_write16(machine, machine->cpu.a[1],
                                  bs_recomp_read16(machine,
                                                   machine->cpu.a[2]));
                machine->cpu.a[2] += 2;
                machine->cpu.a[1] += 2;
                bs_recomp_write16(machine, machine->cpu.a[1], d2++);
                machine->cpu.a[1] += 2;
                set_dreg_word(&machine->cpu.d[0],
                              (uint16_t)(machine->cpu.d[0] - 1));
            }
            set_dreg_word(&machine->cpu.d[2], d2);
        }
        d4 = (uint16_t)(bs_recomp_read16(machine, player + 58) + 6);
        set_dreg_word(&machine->cpu.d[4], d4);
    }
    d4 = (uint16_t)(d4 + bias);
    set_dreg_word(&machine->cpu.d[4], d4);
    uint16_t d2 = bs_recomp_read16(machine, player + 6);
    set_dreg_word(&machine->cpu.d[2], d2);
    if (bs_recomp_read8(machine, player + 49) == 0) {
        d1 = (uint16_t)(5 + bias);
        set_dreg_word(&machine->cpu.d[1], d1);
        uint8_t marker = bs_recomp_read8(machine, player + 38);
        if (marker == 0 || marker == 0x96) {
            if (marker == 0 && d2 >= 0x116) {
                bs_recomp_write16(machine, machine->cpu.a[1], d2);
                machine->cpu.a[1] += 2;
                bs_recomp_write16(machine, machine->cpu.a[1], d1);
                machine->cpu.a[1] += 2;
                d1 = d4;
            }
            d1 <<= 4;
            set_dreg_word(&machine->cpu.d[1], d1);
            machine->cpu.a[2] = 0x2178 + d1;
            bs_recomp_write16(machine, sprite + 2,
                              bs_recomp_read16(machine, machine->cpu.a[2]));
            bs_recomp_write16(machine, sprite + 6,
                              bs_recomp_read16(machine,
                                               machine->cpu.a[2] + 4));
            bs_recomp_write16(machine, sprite + 10,
                              bs_recomp_read16(machine,
                                               machine->cpu.a[2] + 8));
            d2 = (uint16_t)(d2 + 0x1b);
            set_dreg_word(&machine->cpu.d[2], d2);
            if (d2 < 0x1f0) {
                bs_recomp_write16(machine, machine->cpu.a[1], d2);
                machine->cpu.a[1] += 2;
                d1 = (uint16_t)((bs_recomp_read16(
                    machine, machine->cpu.a[5] - 28552) >> 2) & 7);
                if (d1 > 4) d1 = (uint16_t)(8 - d1);
                d1 = (uint16_t)(d1 + bias);
                set_dreg_word(&machine->cpu.d[1], d1);
                bs_recomp_write16(machine, machine->cpu.a[1], d1);
                machine->cpu.a[1] += 2;
                d2 = (uint16_t)(d2 + 3);
                set_dreg_word(&machine->cpu.d[2], d2);
                if (d2 < 0x1f0) {
                    bs_recomp_write16(machine, machine->cpu.a[1], d2);
                    machine->cpu.a[1] += 2;
                    bs_recomp_write16(machine, machine->cpu.a[1], d4);
                    machine->cpu.a[1] += 2;
                }
            }
        }
    }
    bs_recomp_write16(machine, machine->cpu.a[1], 0);
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
}

static void rebuild_hud_copper(BsRecomp *machine)
{
    update_hud_palette(machine, 0x4e3c, 0x27ae, 0x217c,
                       0x0889, 0x0225);
    update_hud_palette(machine, 0x4f46, 0x27ce, 0x240c,
                       0x0c94, 0x0521);
    build_player_hud_list(machine, 0x4e3c, 0x2696, 0xb302, 0x21d8, 0);
    build_player_hud_list(machine, 0x4f46, 0x26ce, 0xb312, 0x2468, 41);

    machine->cpu.a[0] = 0xbb46;
    machine->cpu.a[1] = 0x2696;
    machine->cpu.a[2] = 0x26ce;
    set_dreg_byte(&machine->cpu.d[4], 0);
    while (bs_recomp_read16(machine, machine->cpu.a[1]) != 0 ||
           bs_recomp_read16(machine, machine->cpu.a[2]) != 0) {
        uint16_t d2, d3;
        if (bs_recomp_read16(machine, machine->cpu.a[1]) == 0 ||
            (bs_recomp_read16(machine, machine->cpu.a[2]) != 0 &&
             bs_recomp_read16(machine, machine->cpu.a[1]) >=
                 bs_recomp_read16(machine, machine->cpu.a[2]))) {
            d2 = bs_recomp_read16(machine, machine->cpu.a[2]);
            d3 = bs_recomp_read16(machine, machine->cpu.a[2] + 2);
            machine->cpu.a[2] += 4;
        } else {
            d2 = bs_recomp_read16(machine, machine->cpu.a[1]);
            d3 = bs_recomp_read16(machine, machine->cpu.a[1] + 2);
            machine->cpu.a[1] += 4;
        }
        d2 = (uint16_t)(d2 - 0x00da);
        if ((d2 & 0x0100) && (uint8_t)machine->cpu.d[4] == 0) {
            set_dreg_byte(&machine->cpu.d[4], 0xff);
            bs_recomp_write32(machine, machine->cpu.a[0], 0xffdffffe);
            machine->cpu.a[0] += 4;
        }
        bs_recomp_write32(machine, machine->cpu.a[0],
                          ((uint32_t)(uint8_t)d2 << 24) | 0x0001ff00);
        machine->cpu.a[0] += 4;
        d3 = (uint16_t)(d3 << 4);
        machine->cpu.a[4] = 0x2176 + d3;
        bs_recomp_write32(machine, machine->cpu.a[0],
                          bs_recomp_read32(machine, machine->cpu.a[4]));
        bs_recomp_write32(machine, machine->cpu.a[0] + 4,
                          bs_recomp_read32(machine, machine->cpu.a[4] + 4));
        bs_recomp_write32(machine, machine->cpu.a[0] + 8,
                          bs_recomp_read32(machine, machine->cpu.a[4] + 8));
        machine->cpu.a[0] += 12;
        machine->cpu.a[4] += 8;
        set_dreg_word(&machine->cpu.d[2], d2);
        set_dreg_word(&machine->cpu.d[3], d3);
    }
    if ((uint8_t)machine->cpu.d[4] == 0) {
        bs_recomp_write32(machine, machine->cpu.a[0], 0xffdffffe);
        machine->cpu.a[0] += 4;
    }
    machine->cpu.d[1] = 0x0000c052;
    bs_recomp_write16(machine, machine->cpu.a[0], 0x0084);
    machine->cpu.a[0] += 2;
    bs_recomp_write16(machine, machine->cpu.a[0], 0xc052);
    machine->cpu.a[0] += 2;
    bs_recomp_write16(machine, machine->cpu.a[0], 0x0086);
    machine->cpu.a[0] += 2;
    bs_recomp_write16(machine, machine->cpu.a[0], 0xc052);
    machine->cpu.a[0] += 2;
    bs_recomp_write32(machine, machine->cpu.a[0], 0x008a0000);
    machine->cpu.a[0] += 4;
    machine->cpu.sr &= 0xfff0;
}

static void set_dreg_word(uint32_t *reg, uint16_t value)
{
    *reg = (*reg & UINT32_C(0xffff0000)) | value;
}

static void set_dreg_byte(uint32_t *reg, uint8_t value)
{
    *reg = (*reg & UINT32_C(0xffffff00)) | value;
}

/* Loader $2B1E.  The original deliberately advances the low byte of its
 * sample pointer in-place, so keep that self-modifying data-table contract
 * even though no translated instruction bytes are ever fetched. */
static uint8_t next_wave_random(BsRecomp *machine)
{
    uint32_t pointer = bs_recomp_read32(machine, 0x2b1a);
    uint8_t value = bs_recomp_read8(machine, pointer);
    bs_recomp_write8(machine, 0x2b1d,
                     (uint8_t)(bs_recomp_read8(machine, 0x2b1d) + 1));
    machine->cpu.d[3] = value;
    return value;
}

/* Loader $32F4-$33D0.  Allocate one of the eighteen 64-byte scenery object
 * records and materialise its 48-byte template.  This is plain host-side
 * object construction; it does not invoke the Amiga blitter or a 68000
 * interpreter.  A return value of one is the template-$27 early return. */
static int allocate_wave_object(BsRecomp *machine, uint32_t template,
                                uint16_t map_counter, uint32_t map_cursor,
                                uint16_t map_word)
{
    const uint32_t base = machine->cpu.a[5];
    machine->cpu.a[2] = template;
    machine->cpu.a[4] = 0x2e040;
    machine->cpu.d[7] = 17;
    while (bs_recomp_read16(machine, machine->cpu.a[4]) != 0) {
        machine->cpu.a[4] += 0x40;
        uint16_t d7 = (uint16_t)(machine->cpu.d[7] - 1);
        set_dreg_word(&machine->cpu.d[7], d7);
        if (d7 == 0xffff) {
            if (bs_recomp_read16(machine, base + 7228) != 1)
                return 1;
            if (map_word == 0x00a0)
                bs_recomp_write16(machine, map_cursor - 2, 0x0320);
            else if (map_word == 0x0230)
                bs_recomp_write16(machine, map_cursor - 2, 0x04b0);
            return 0;
        }
    }

    uint16_t d1 = (uint16_t)(23 - map_counter);
    d1 = (uint16_t)((d1 << 4) + 0x0100);
    set_dreg_word(&machine->cpu.d[1], d1);
    bs_recomp_write16(machine, machine->cpu.a[4], d1);
    bs_recomp_write32(machine, machine->cpu.a[4] + 6,
                      bs_recomp_read32(machine, template + 6));

    uint16_t d2 = (uint16_t)(0x0100 -
                             bs_recomp_read16(machine,
                                              machine->cpu.a[4] + 6));
    set_dreg_word(&machine->cpu.d[2], d2);
    bs_recomp_write16(machine, machine->cpu.a[4] + 2, d2);
    d2 = bs_recomp_read16(machine, machine->cpu.a[4] + 8);
    d2 = (uint16_t)(d2 + d2);
    set_dreg_word(&machine->cpu.d[2], d2);
    set_dreg_word(&machine->cpu.d[1], d2);
    machine->cpu.d[2] = (uint32_t)d2 *
                        bs_recomp_read16(machine, machine->cpu.a[4] + 6);
    bs_recomp_write16(machine, machine->cpu.a[4] + 10,
                      (uint16_t)machine->cpu.d[2]);
    set_dreg_word(&machine->cpu.d[2],
                  (uint16_t)(((uint16_t)machine->cpu.d[2] << 2) +
                             bs_recomp_read16(machine,
                                              machine->cpu.a[4] + 10)));
    bs_recomp_write16(machine, machine->cpu.a[4] + 26,
                      (uint16_t)machine->cpu.d[2]);
    set_dreg_word(&machine->cpu.d[1],
                  (uint16_t)(0x0030 - (uint16_t)machine->cpu.d[1]));
    bs_recomp_write16(machine, machine->cpu.a[4] + 4,
                      (uint16_t)machine->cpu.d[1]);

    bs_recomp_write32(machine, machine->cpu.a[4] + 12,
                      bs_recomp_read32(machine, template + 12));
    bs_recomp_write32(machine, machine->cpu.a[4] + 16,
                      bs_recomp_read32(machine, template + 16));
    bs_recomp_write32(machine, machine->cpu.a[4] + 20,
                      bs_recomp_read32(machine, template + 20));
    bs_recomp_write16(machine, machine->cpu.a[4] + 24,
                      bs_recomp_read16(machine, template + 24));
    bs_recomp_write32(machine, machine->cpu.a[4] + 28,
                      bs_recomp_read32(machine, template + 28));
    if (bs_recomp_read8(machine, base - 2732) == 0) {
        uint8_t d1b = bs_recomp_read8(machine, machine->cpu.a[4] + 28);
        set_dreg_byte(&machine->cpu.d[1], d1b);
        d1b = (uint8_t)(d1b + 1);
        set_dreg_byte(&machine->cpu.d[1], d1b);
        d1b >>= 2;
        set_dreg_byte(&machine->cpu.d[1], d1b);
        bs_recomp_write8(machine, machine->cpu.a[4] + 28,
                         (uint8_t)(bs_recomp_read8(machine,
                                                   machine->cpu.a[4] + 28) -
                                   d1b));
    }
    bs_recomp_write32(machine, machine->cpu.a[4] + 32,
                      bs_recomp_read32(machine, template + 32));
    bs_recomp_write16(machine, machine->cpu.a[4] + 36,
                      bs_recomp_read16(machine, template + 36));
    bs_recomp_write8(machine, machine->cpu.a[4] + 42, 0);
    uint8_t random = next_wave_random(machine);
    random &= bs_recomp_read8(machine, template + 43);
    random = (uint8_t)(random + 1);
    set_dreg_byte(&machine->cpu.d[3], random);
    bs_recomp_write8(machine, machine->cpu.a[4] + 43, random);
    bs_recomp_write32(machine, machine->cpu.a[4] + 44,
                      bs_recomp_read32(machine, template + 44));
    return bs_recomp_read8(machine, machine->cpu.a[4] + 17) == 0x27;
}

/* Loader $3078-$33D0: scan the newly exposed terrain column and instantiate
 * any matching scenery/enemy templates.  The first eligible column is frame
 * 256 of the transformed-game bootstrap, so this is also the first native
 * object-spawn boundary in the genuine recompilation. */
static void spawn_wave_objects(BsRecomp *machine)
{
    const uint32_t base = machine->cpu.a[5];
    uint16_t value = bs_recomp_read16(machine, base + 7222);
    if (value == 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x04);
        return;
    }
    value = bs_recomp_read16(machine, base + 7212);
    if (value != 0) {
        machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) |
                                     ((value & 0x8000) ? 0x08 : 0));
        return;
    }
    if ((int16_t)bs_recomp_read16(machine, base + 8514) >= 0x7530)
        return;

    if (bs_recomp_read16(machine, base + 7228) == 0 &&
        bs_recomp_read16(machine, base + 7230) == 0) {
        uint16_t progress = bs_recomp_read16(machine, base + 7206);
        if (progress == 0x0f10 &&
            !(bs_recomp_read8(machine, base - 4099) & 0x02)) {
            machine->cpu.d[0] =
                (machine->cpu.d[0] & UINT32_C(0xffff0000)) | 0x000b;
            machine->cpu.d[6] = 1;
            bs_recomp_write16(machine, base + 7230, 1);
            (void)allocate_wave_object(machine, 0x2fb8,
                                       (uint16_t)machine->cpu.d[0], 0, 0);
            goto done;
        }
        if (progress == 0x1490 &&
            !(bs_recomp_read8(machine, base - 4099) & 0x04)) {
            machine->cpu.d[0] =
                (machine->cpu.d[0] & UINT32_C(0xffff0000)) | 0x0009;
            machine->cpu.d[6] = 2;
            bs_recomp_write16(machine, base + 7230, 2);
            (void)allocate_wave_object(machine, 0x2fb8,
                                       (uint16_t)machine->cpu.d[0], 0, 0);
            goto done;
        }
        if (progress == 0x1dd0 &&
            !(bs_recomp_read8(machine, base - 4099) & 0x08)) {
            machine->cpu.d[0] =
                (machine->cpu.d[0] & UINT32_C(0xffff0000)) | 0x000c;
            machine->cpu.d[6] = 3;
            bs_recomp_write16(machine, base + 7230, 3);
            (void)allocate_wave_object(machine, 0x2fb8,
                                       (uint16_t)machine->cpu.d[0], 0, 0);
            goto done;
        }
    }

    machine->cpu.a[1] = bs_recomp_read32(machine, base + 7214) - 0x30;
    machine->cpu.d[0] = 23;
    for (;;) {
        uint16_t map_counter = (uint16_t)machine->cpu.d[0];
        uint16_t map_word = bs_recomp_read16(machine, machine->cpu.a[1]);
        machine->cpu.a[1] += 2;
        set_dreg_word(&machine->cpu.d[1], map_word);
        uint32_t template = 0;
        uint16_t mode = bs_recomp_read16(machine, base + 7228);
        uint16_t progress = bs_recomp_read16(machine, base + 7206);
        if (mode == 0) {
            if (map_word == 0x6180) template = 0x2da8;
            else if (map_word == 0x5640) {
                if (bs_recomp_read16(machine, machine->cpu.a[1] + 0x30) ==
                    0x5cd0) template = 0x2e68;
            } else if (map_word == 0x0280) {
                if (progress >= 0x03e8 && progress < 0x1f40)
                    template = 0x2b98;
            } else if (map_word == 0x8020) template = 0x2ec8;
            else if (map_word == 0x5d20) template = 0x2ef8;
            else if (map_word == 0x5d70) template = 0x2f28;
            else if (map_word == 0x5910) template = 0x2f58;
            else if (map_word == 0x59b0) template = 0x2f88;
        } else if (mode == 1) {
            if (map_word == 0x5c80) template = 0x2dd8;
            else if (map_word == 0x00a0) template = 0x2fe8;
            else if (map_word == 0x0230) template = 0x3018;
            else if (map_word == 0x0d70) template = 0x2c28;
            else if (map_word == 0x92e0 && progress < 0x19c8)
                template = 0x3048;
        } else if (mode == 2) {
            if (map_word == 0x7260) template = 0x2bc8;
            else if (map_word == 0x9420) template = 0x2e98;
            else if (map_word == 0x0cd0) template = 0x2e08;
        } else {
            if (map_word == 0x8c00) template = 0x2b68;
            else if (map_word == 0x9420) template = 0x2e38;
            else if (map_word == 0x92e0) {
                if (progress < 0x0dde && next_wave_random(machine) < 0x40)
                    template = 0x2bf8;
            } else if (map_word == 0x3a70) template = 0x2c58;
            else if (map_word == 0x4bf0) template = 0x2c88;
            else if (map_word == 0x3ed0) template = 0x2cb8;
            else if (map_word == 0x3340) template = 0x2ce8;
            else if (map_word == 0x4e70) template = 0x2d18;
            else if (map_word == 0x4290) template = 0x2d48;
            else if (map_word == 0x3520) template = 0x2d78;
        }
        if (template && allocate_wave_object(machine, template, map_counter,
                                             machine->cpu.a[1], map_word))
            goto done;
        uint16_t counter = (uint16_t)(machine->cpu.d[0] - 1);
        set_dreg_word(&machine->cpu.d[0], counter);
        if (counter == 0xffff) break;
    }

done:
    /* The first-spawn parity edge exits on the byte type comparison with
     * N and C set.  Later translated callers will refine divergent exits as
     * they become covered by their own boundary fixtures. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
}

/* LODTXT $7FB36-$7FCBC.  Frame/raster delay loops are intentionally host
 * scheduling seams.  The six text screens, font compositing, palette fades,
 * table cursor and complete return-state contract are translated directly. */
static int text_overlay_entry(BsRecomp *machine)
{
    machine->cpu.a[4] = 0x7fad0;
    machine->cpu.d[7] = 5;
    for (int page = 0; page < 6; page++) {
        machine->cpu.a[1] = bs_recomp_read32(machine, machine->cpu.a[4]);
        machine->cpu.a[4] += 4;
        bs_recomp_write32(machine, 0x7fb30, machine->cpu.a[4]);

        machine->cpu.a[0] = 0x44000;
        machine->cpu.d[2] = 3;
        for (int plane = 0; plane < 4; plane++) {
            clear_longwords(machine, 0x00c7);
            machine->cpu.d[1] = 0x009f;
            for (int row = 0; row < 160; row++) {
                bs_recomp_write32(machine, machine->cpu.a[0], 0);
                machine->cpu.a[0] += 4;
                bs_recomp_write8(machine, machine->cpu.a[0]++, 0);
                machine->cpu.d[0] = 0x001d;
                for (int column = 0; column < 30; column++)
                    bs_recomp_write8(machine, machine->cpu.a[0]++,
                                     bs_recomp_read8(machine,
                                                     machine->cpu.a[1]++));
                machine->cpu.d[0] = 0xffff;
                bs_recomp_write8(machine, machine->cpu.a[0]++, 0);
                bs_recomp_write32(machine, machine->cpu.a[0], 0);
                machine->cpu.a[0] += 4;
                machine->cpu.d[1] = (machine->cpu.d[1] & 0xffff0000) |
                                    (uint16_t)(machine->cpu.d[1] - 1);
            }
            clear_longwords(machine, 0x00c7);
            machine->cpu.d[2] = (machine->cpu.d[2] & 0xffff0000) |
                                (uint16_t)(machine->cpu.d[2] - 1);
        }
        machine->cpu.a[0] = 0x4bd00;
        clear_longwords(machine, 0x07cf);

        uint32_t saved_d7 = machine->cpu.d[7];
        uint32_t saved_a1 = machine->cpu.a[1];
        machine->cpu.a[2] = 0xae28;
        machine->cpu.a[1] = 0x193a;
        expand_palette(machine);

        machine->cpu.a[4] = bs_recomp_read32(machine, 0x7fb30);
        machine->cpu.a[1] = bs_recomp_read32(machine, machine->cpu.a[4]);
        machine->cpu.a[4] += 4;
        bs_recomp_write32(machine, 0x7fb30, machine->cpu.a[4]);
        machine->cpu.d[5] = 0;
        machine->cpu.d[6] = 0x004b;

        int terminated = 0;
        for (int line = 0; line < 256; line++) {
            machine->cpu.a[0] = 0x7f000;
            machine->cpu.d[0] = 9;
            for (int i = 0; i < 10; i++) {
                bs_recomp_write32(machine, machine->cpu.a[0], 0x20202020);
                machine->cpu.a[0] += 4;
                machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                    (uint16_t)(machine->cpu.d[0] - 1);
            }
            machine->cpu.a[0] = 0x44000;
            machine->cpu.d[2] = bs_recomp_read8(machine, machine->cpu.a[1]++);
            machine->cpu.d[2] *= 40;
            machine->cpu.a[0] += (uint16_t)machine->cpu.d[2];
            machine->cpu.a[2] = 0x7f000;
            machine->cpu.d[1] = bs_recomp_read8(machine, machine->cpu.a[1]++);
            machine->cpu.a[2] += (uint16_t)machine->cpu.d[1];
            for (;;) {
                uint8_t character = bs_recomp_read8(machine,
                                                     machine->cpu.a[1]++);
                machine->cpu.d[1] = (machine->cpu.d[1] & 0xffffff00) |
                                    character;
                if (!character) break;
                bs_recomp_write8(machine, machine->cpu.a[2]++, character);
            }

            machine->cpu.a[2] = 0x7f000;
            machine->cpu.d[7] = 59;
            for (int character_index = 0; character_index < 40;
                 character_index++) {
                machine->cpu.d[1] = bs_recomp_read8(machine,
                                                     machine->cpu.a[2]++);
                machine->cpu.a[3] = 0x10550 +
                                     (uint16_t)machine->cpu.d[1] * 10;
                machine->cpu.d[0] = 9;
                for (int glyph_row = 0; glyph_row < 10; glyph_row++) {
                    uint8_t current = bs_recomp_read8(machine,
                                                       machine->cpu.a[3]++);
                    uint8_t previous = bs_recomp_read8(machine,
                                                        machine->cpu.a[3] - 2);
                    machine->cpu.d[1] = (machine->cpu.d[1] & 0xffffff00) |
                                        current;
                    uint8_t shifted = (uint8_t)((previous >> 1) | current);
                    machine->cpu.d[2] = (machine->cpu.d[2] & 0xffffff00) |
                                        shifted;
                    bs_recomp_write8(machine, machine->cpu.a[0] + 0x7d00,
                                     shifted);
                    uint8_t background = bs_recomp_read8(machine,
                                                          machine->cpu.a[0]);
                    bs_recomp_write8(machine, machine->cpu.a[0],
                                     (uint8_t)((background & ~shifted) |
                                               current));
                    machine->cpu.a[0] += 40;
                    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                        (uint16_t)(machine->cpu.d[0] - 1);
                }
                machine->cpu.a[0] -= 0x18f;
                machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) |
                                    (uint16_t)(machine->cpu.d[7] - 1);
            }
            /* The remaining 20 DBF iterations are display-only delay. */
            machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) | 0xffff;
            if (bs_recomp_read16(machine, machine->cpu.a[1]) == 0xffff) {
                terminated = 1;
                break;
            }
        }
        if (!terminated) {
            snprintf(machine->error, sizeof machine->error,
                     "unterminated LODTXT page %d", page);
            return BS_RECOMP_ERROR;
        }

        machine->cpu.a[1] = 0xae28;
        darken_palette(machine);
        machine->cpu.d[7] = saved_d7;
        machine->cpu.a[1] = saved_a1;
        machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) |
                            (uint16_t)(machine->cpu.d[7] - 1);
    }
    /* TST.B $7FB34 on the non-skipped path supplies Z at the return edge. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x14);
    return BS_RECOMP_OK;
}

static void init_music_channel(BsRecomp *machine, uint32_t state_address)
{
    static const uint8_t clear_bytes[] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x39,
        0x35, 0x37, 0x36, 0x3b, 0x3c,
    };
    for (size_t i = 0; i < sizeof clear_bytes; i++)
        bs_recomp_write8(machine, state_address + clear_bytes[i], 0);
    bs_recomp_write16(machine, state_address + 0x28, 0);
    bs_recomp_write16(machine, state_address + 0x2a, 0);
    bs_recomp_write16(machine, state_address + 0x2c, 0);
    bs_recomp_write32(machine, state_address + 0x14, 0);
    bs_recomp_write32(machine, state_address + 0x18, 0);
    bs_recomp_write32(machine, state_address + 0x1c, 0);

    uint32_t table = 0x3e118;
    bs_recomp_write32(machine, state_address + 4, table);
    uint32_t source = bs_recomp_read32(machine, table);
    uint32_t destination = bs_recomp_read32(machine, state_address);
    bs_recomp_write32(machine, destination,
                      bs_recomp_read32(machine, source));
    bs_recomp_write16(machine, destination + 4,
                      bs_recomp_read16(machine, source + 4));
    uint32_t sequence = bs_recomp_read32(machine, state_address + 8);
    bs_recomp_write32(machine, state_address + 0x0c, sequence);
    bs_recomp_write32(machine, state_address + 0x10,
                      bs_recomp_read32(machine, sequence));
    bs_recomp_write16(machine, state_address + 0x20,
                      bs_recomp_read16(machine, sequence + 6));
    machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
        (uint16_t)(bs_recomp_read16(machine, sequence + 0x0a) - 1);
    bs_recomp_write16(machine, state_address + 0x22,
                      (uint16_t)machine->cpu.d[0]);
}

/* LODMUS $3D800/$3D830/$3DC98: channel state, CIA-B timer and Paula DMA. */
static void music_overlay_entry(BsRecomp *machine)
{
    static const uint32_t states[] = {0x3df3c, 0x3df7a, 0x3dfb8, 0x3dff6};
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++)
        init_music_channel(machine, states[i]);
    bs_recomp_write16(machine, 0xdff09a, 0x4000);
    bs_recomp_write8(machine, 0xbfde00, 0x00);
    bs_recomp_write8(machine, 0xbfd400, 0x00);
    bs_recomp_write8(machine, 0xbfd500, 0x25);
    bs_recomp_write8(machine, 0xbfdd00, 0x81);
    bs_recomp_write8(machine, 0xbfde00, 0x11);
    bs_recomp_write32(machine, 0x000008, 0x3dda2);
    bs_recomp_write16(machine, 0xdff09a, 0xc000);
    bs_recomp_write16(machine, 0xdff096, 0x800f);
    /* Last MOVE.W writes $800F: N set, ZVC clear; X was cleared by init. */
    machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x08);
}

int bs_recomp_run(BsRecomp *machine, long max_steps)
{
    if (!machine || max_steps < 0) return BS_RECOMP_ERROR;
    while (max_steps-- > 0) {
        machine->translated_steps++;
        switch (machine->cpu.pc) {
        case 0x400: machine->cpu.a[5] = 0x8000; machine->cpu.pc = 0x406; break;
        case 0x406: machine->cpu.a[6] = 0xdff000; machine->cpu.pc = 0x40c; break;
        case 0x40c: machine->cpu.a[7] = 0x10000; machine->cpu.pc = 0x412; break;
        case 0x412:
            bs_recomp_write32(machine, machine->cpu.a[5] - 31748, 0x5e000);
            machine->cpu.pc = 0x41a;
            break;
        case 0x41a:
            bs_recomp_write16(machine, machine->cpu.a[6] + 150, 0x20);
            machine->cpu.pc = 0x420;
            break;
        case 0x420: machine->cpu.a[0] = 0xaf14; machine->cpu.pc = 0x426; break;
        case 0x426: clear32words(machine); machine->cpu.pc = 0x42a; break;
        case 0x42a:
            bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xaeca);
            machine->cpu.pc = 0x432;
            break;
        case 0x432: machine->cpu.a[4] = 0x1b48; machine->cpu.pc = 0x438; break;
        case 0x438:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x43c;
            break;
        case 0x43c: machine->cpu.a[2] = 0xaf14; machine->cpu.pc = 0x442; break;
        case 0x442: machine->cpu.a[1] = 0x18fa; machine->cpu.pc = 0x448; break;
        case 0x448: expand_palette(machine); machine->cpu.pc = 0x44c; break;
        case 0x44c: machine->cpu.a[4] = 0x19f8; machine->cpu.pc = 0x452; break;
        case 0x452:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x456;
            break;
        case 0x456: machine->cpu.a[4] = 0x1a70; machine->cpu.pc = 0x45c; break;
        case 0x45c:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x460;
            break;
        case 0x460: machine->cpu.a[4] = 0x1a88; machine->cpu.pc = 0x466; break;
        case 0x466:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x46a;
            break;
        case 0x46a:
            music_overlay_entry(machine);
            if (machine->audio_sample_hook) {
                /* Exact LODMUS speech descriptor at overlay +$BEC.  It
                 * consumes the complete LODSPE payload as signed Paula PCM.
                 * The host owns presentation only; sample identity and
                 * timing remain a translated game event. */
                static const BsAudioSampleEvent welcome = {
                    .address = 0x246f0,
                    .length_words = 0x17cd,
                    .period = 0x01ac,
                    .volume = 64,
                    .channel = 0,
                };
                machine->audio_sample_hook(machine->audio_sample_user,
                                           &welcome);
            }
            machine->cpu.pc = 0x470;
            break;
        case 0x470: machine->cpu.a[4] = 0x1980; machine->cpu.pc = 0x476; break;
        case 0x476:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x47a;
            break;
        case 0x47a: bs_recomp_write32(machine, 0x1737a, 0x1358); machine->cpu.pc = 0x484; break;
        case 0x484: bs_recomp_write32(machine, 0x1737e, 0x1034); machine->cpu.pc = 0x48e; break;
        case 0x48e: bs_recomp_write32(machine, 0x17382, 0xae28); machine->cpu.pc = 0x498; break;
        case 0x498: bs_recomp_write32(machine, 0x17386, 0x193a); machine->cpu.pc = 0x4a2; break;
        case 0x4a2: bs_recomp_write32(machine, 0x1738a, 0x1c2c); machine->cpu.pc = 0x4ac; break;
        case 0x4ac: bs_recomp_write32(machine, 0x1738e, 0x1cae); machine->cpu.pc = 0x4b6; break;
        case 0x4b6: machine->cpu.a[4] = 0x1998; machine->cpu.pc = 0x4bc; break;
        case 0x4bc:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x4c0;
            break;
        case 0x4c0: machine->cpu.a[4] = 0x1b78; machine->cpu.pc = 0x4c6; break;
        case 0x4c6:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x4ca;
            break;
        case 0x4ca: machine->cpu.a[1] = 0xaf14; machine->cpu.pc = 0x4d0; break;
        case 0x4d0: darken_palette(machine); machine->cpu.pc = 0x4d4; break;
        case 0x4d4: machine->cpu.a[0] = 0xae28; machine->cpu.pc = 0x4da; break;
        case 0x4da: clear32words(machine); machine->cpu.pc = 0x4de; break;
        case 0x4de:
            bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xadde);
            machine->cpu.pc = 0x4e6;
            break;
        case 0x4e6:
            if (text_overlay_entry(machine))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x4ec;
            break;
        case 0x4ec: machine->cpu.a[1] = 0x30000; machine->cpu.pc = 0x4f2; break;
        case 0x4f2: machine->cpu.a[0] = 0x62000; machine->cpu.pc = 0x4f8; break;
        case 0x4f8: machine->cpu.d[0] = 0x289f; machine->cpu.pc = 0x4fc; break;
        case 0x4fc: copy_longwords(machine, 0x289f); machine->cpu.pc = 0x500; break;
        case 0x500: machine->cpu.a[1] = 0xafb6; machine->cpu.pc = 0x506; break;
        case 0x506: machine->cpu.a[0] = 0xaeea; machine->cpu.pc = 0x50c; break;
        case 0x50c: machine->cpu.d[0] = 9; machine->cpu.pc = 0x510; break;
        case 0x510: copy_longwords(machine, 9); machine->cpu.pc = 0x514; break;
        case 0x514:
            bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xaeca);
            machine->cpu.pc = 0x51c;
            break;
        case 0x51c: machine->cpu.a[2] = 0xaf14; machine->cpu.pc = 0x522; break;
        case 0x522: machine->cpu.a[1] = 0x18fa; machine->cpu.pc = 0x528; break;
        case 0x528: expand_palette(machine); machine->cpu.pc = 0x52c; break;
        case 0x52c: machine->cpu.a[4] = 0x19e0; machine->cpu.pc = 0x532; break;
        case 0x532:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x536;
            break;
        case 0x536:
            if (adjust_s0f_chains(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x53a;
            break;
        case 0x53a: machine->cpu.a[4] = 0x1a10; machine->cpu.pc = 0x540; break;
        case 0x540:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x544;
            break;
        case 0x544: machine->cpu.a[4] = 0x1b60; machine->cpu.pc = 0x54a; break;
        case 0x54a:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x54e;
            break;
        case 0x54e: machine->cpu.a[0] = 0xfce6; machine->cpu.pc = 0x554; break;
        case 0x554:
            if (adjust_object_chain(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x558;
            break;
        case 0x558: machine->cpu.a[0] = 0x9ea6; machine->cpu.pc = 0x55e; break;
        case 0x55e:
            if (adjust_object_chain(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x562;
            break;
        case 0x562: machine->cpu.a[0] = 0x9f34; machine->cpu.pc = 0x568; break;
        case 0x568:
            if (adjust_object_chain(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x56c;
            break;
        case 0x56c: machine->cpu.a[0] = 0x9fc2; machine->cpu.pc = 0x572; break;
        case 0x572:
            if (adjust_object_chain(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x576;
            break;
        case 0x576: machine->cpu.a[0] = 0xa064; machine->cpu.pc = 0x57c; break;
        case 0x57c:
            if (adjust_object_chain(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x580;
            break;
        case 0x580: machine->cpu.a[1] = 0xaf14; machine->cpu.pc = 0x586; break;
        case 0x586: darken_palette(machine); machine->cpu.pc = 0x58a; break;
        case 0x58a:
            bs_recomp_write16(machine, machine->cpu.a[6] + 150, 0x20);
            machine->cpu.pc = 0x590;
            break;
        case 0x590:
            bs_recomp_write16(machine, machine->cpu.a[5] + 7232,
                              bs_recomp_read16(machine,
                                               machine->cpu.a[5] + 7228));
            machine->cpu.pc = 0x596;
            break;
        case 0x596: machine->cpu.a[4] = 0x19b0; machine->cpu.pc = 0x59c; break;
        case 0x59c:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x5a0;
            break;
        case 0x5a0:
            initialise_game_state(machine);
            machine->cpu.pc = 0x5a4;
            break;
        case 0x5a4:
            bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xafde);
            machine->cpu.pc = 0x5ac;
            break;
        case 0x5ac: machine->cpu.a[1] = 0x17fa; machine->cpu.pc = 0x5b2; break;
        case 0x5b2: machine->cpu.a[2] = 0xb028; machine->cpu.pc = 0x5b8; break;
        case 0x5b8: expand_palette(machine); machine->cpu.pc = 0x5bc; break;
        case 0x5bc:
            machine->cpu.pc = bs_recomp_read16(machine,
                                                machine->cpu.a[5] + 7232)
                              ? 0x5c2 : 0x5da;
            break;
        case 0x5da:
            bs_recomp_write32(machine, machine->cpu.a[5] - 25430, 0xb028);
            machine->cpu.pc = 0x5e2;
            break;
        case 0x5e2:
            bs_recomp_write8(machine, machine->cpu.a[5] + 10970, 0xff);
            machine->cpu.pc = 0x5e8;
            break;
        case 0x5e8:
            bs_recomp_write8(machine, 0xbfec01, 0);
            machine->cpu.pc = 0x5ee;
            break;
        case 0x5ee:
            bs_recomp_write8(machine, machine->cpu.a[5] - 28516, 0);
            machine->cpu.pc = 0x5f2;
            break;
        case 0x5f2:
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | 0x01f3;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x10);
            machine->cpu.pc = 0x5f6;
            break;
        case 0x5f6:
            /* The first no-input iteration observes the freshly cleared key
             * latch, redraws the key legend, then the remaining 499 timed
             * iterations have no architectural side effects. */
            redraw_keyboard_help(machine);
            bs_recomp_write8(machine, machine->cpu.a[5] + 10970,
                             bs_recomp_read8(machine, 0xbfec01));
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | 0xffff;
            machine->cpu.d[1] &= 0xffffff00;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
            machine->cpu.pc = 0x64c;
            break;
        case 0x64c:
            machine->cpu.pc = bs_recomp_read8(machine,
                                               machine->cpu.a[5] - 26245)
                              ? 0x652 : 0x67e;
            break;
        case 0x67e:
            disable_speech_entrypoints(machine);
            /* LODJOY replaces the title driver immediately after this
             * edge.  End its CIA/Paula ownership before loading over it;
             * otherwise a latched byte becomes the familiar continuous
             * tone during the terrain transition. */
            bs_recomp_write16(machine, 0xdff096, 0x000f);
            bs_recomp_write8(machine, 0xbfde00, 0x00);
            machine->cpu.pc = 0x690;
            break;
        case 0x690: machine->cpu.a[4] = 0x19c8; machine->cpu.pc = 0x696; break;
        case 0x696:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x69a;
            break;
        case 0x69a: machine->cpu.a[0] = 0xac58; machine->cpu.pc = 0x6a0; break;
        case 0x6a0: clear32words(machine); machine->cpu.pc = 0x6a4; break;
        case 0x6a4: machine->cpu.a[1] = 0xb028; machine->cpu.pc = 0x6aa; break;
        case 0x6aa: darken_palette(machine); machine->cpu.pc = 0x6ae; break;
        case 0x6ae:
            bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xac16);
            machine->cpu.pc = 0x6b6;
            break;
        case 0x6b6: machine->cpu.a[2] = 0xac58; machine->cpu.pc = 0x6bc; break;
        case 0x6bc: machine->cpu.a[1] = 0x179a; machine->cpu.pc = 0x6c2; break;
        case 0x6c2: expand_palette(machine); machine->cpu.pc = 0x6c6; break;
        case 0x6c6:
            bs_recomp_write32(machine, machine->cpu.a[5] - 25430, 0xac58);
            machine->cpu.pc = 0x6ce;
            break;
        case 0x6ce:
            machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) | 0x01f4;
            machine->cpu.pc = 0x6d2;
            break;
        case 0x6d2:
            /* 501 raster-timed, no-input display iterations. */
            machine->cpu.d[7] = (machine->cpu.d[7] & 0xffff0000) | 0xffff;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x10);
            machine->cpu.pc = 0x6fa;
            break;
        case 0x6fa: machine->cpu.a[1] = 0xac58; machine->cpu.pc = 0x700; break;
        case 0x700: darken_palette(machine); machine->cpu.pc = 0x704; break;
        case 0x704: machine->cpu.a[0] = 0xb2a0; machine->cpu.pc = 0x70a; break;
        case 0x70a: clear32words(machine); machine->cpu.pc = 0x70e; break;
        case 0x70e:
            bs_recomp_write32(machine, machine->cpu.a[5] + 13182,
                              0xfffffffe);
            machine->cpu.pc = 0x716;
            break;
        case 0x716:
            bs_recomp_write32(machine, machine->cpu.a[6] + 128, 0xb256);
            machine->cpu.pc = 0x71e;
            break;
        case 0x71e: machine->cpu.a[0] = 0x5e000; machine->cpu.pc = 0x724; break;
        case 0x724:
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | 0x0fff;
            machine->cpu.pc = 0x728;
            break;
        case 0x728: clear_longwords(machine, 0x0fff); machine->cpu.pc = 0x72c; break;
        case 0x72c:
            machine->cpu.a[7] -= 2;
            bs_recomp_write16(machine, machine->cpu.a[7],
                              bs_recomp_read16(machine,
                                               machine->cpu.a[5] + 10066));
            machine->cpu.pc = 0x730;
            break;
        case 0x730:
            bs_recomp_write16(machine, machine->cpu.a[5] + 10066, 1);
            machine->cpu.pc = 0x736;
            break;
        case 0x736:
            initialise_game_state(machine);
            machine->cpu.pc = 0x73a;
            break;
        case 0x73a:
            bs_recomp_write8(machine, machine->cpu.a[5] - 2732, 0xff);
            machine->cpu.pc = 0x740;
            break;
        case 0x740:
            bs_recomp_write16(machine, machine->cpu.a[5] + 10066,
                              bs_recomp_read16(machine, machine->cpu.a[7]));
            machine->cpu.a[7] += 2;
            machine->cpu.pc = 0x744;
            break;
        case 0x744:
            bs_recomp_write16(machine, machine->cpu.a[5] - 14386, 0x0200);
            machine->cpu.pc = 0x74a;
            break;
        case 0x74a:
            bs_recomp_write8(machine, machine->cpu.a[5] + 19942, 3);
            machine->cpu.pc = 0x750;
            break;
        case 0x750:
            bs_recomp_write8(machine, machine->cpu.a[5] + 20038, 0x17);
            machine->cpu.pc = 0x756;
            break;
        case 0x756:
            bs_recomp_write16(machine, machine->cpu.a[5] - 12682, 3);
            machine->cpu.pc = 0x75c;
            break;
        case 0x75c:
            bs_recomp_write16(machine, machine->cpu.a[5] - 12680, 5);
            machine->cpu.pc = 0x762;
            break;
        case 0x762:
            bs_recomp_write8(machine, machine->cpu.a[5] - 12695, 0);
            machine->cpu.pc = 0x766;
            break;
        case 0x766:
            bs_recomp_write8(machine, machine->cpu.a[5] - 12620, 0);
            machine->cpu.pc = 0x76a;
            break;
        case 0x76a:
            bs_recomp_write16(machine, machine->cpu.a[5] - 12416, 2);
            machine->cpu.pc = 0x770;
            break;
        case 0x770:
            bs_recomp_write16(machine, machine->cpu.a[5] - 12414, 4);
            machine->cpu.pc = 0x776;
            break;
        case 0x776:
            bs_recomp_write8(machine, machine->cpu.a[5] - 12435, 0xff);
            machine->cpu.pc = 0x77c;
            break;
        case 0x77c:
            bs_recomp_write8(machine, machine->cpu.a[5] - 12429, 0);
            machine->cpu.pc = 0x780;
            break;
        case 0x780:
            bs_recomp_write8(machine, machine->cpu.a[5] - 12354, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0x784;
            break;
        case 0x784: machine->cpu.a[4] = 0x4e3c; machine->cpu.pc = 0x78a; break;
        case 0x78a:
            initialise_player_object(machine);
            machine->cpu.pc = 0x78e;
            break;
        case 0x78e: machine->cpu.a[4] = 0x4f46; machine->cpu.pc = 0x794; break;
        case 0x794:
            initialise_player_object(machine);
            machine->cpu.pc = 0x798;
            break;
        case 0x798:
            expand_capacity_bars(machine, 0);
            machine->cpu.pc = 0x79c;
            break;
        case 0x79c:
            expand_capacity_bars(machine, 1);
            machine->cpu.pc = 0x7a0;
            break;
        case 0x7a0:
            draw_life_icons(machine, 0);
            machine->cpu.pc = 0x7a4;
            break;
        case 0x7a4:
            draw_life_icons(machine, 1);
            machine->cpu.pc = 0x7a8;
            break;
        case 0x7a8:
            bs_recomp_write32(machine, machine->cpu.a[5] + 6810, 0x22f80);
            machine->cpu.pc = 0x7b0;
            break;
        case 0x7b0:
            bs_recomp_write8(machine, machine->cpu.a[5] - 4099, 0x0e);
            machine->cpu.pc = 0x7b6;
            break;
        case 0x7b6:
            bs_recomp_write32(machine, machine->cpu.a[5] - 2736, 0xd3be);
            machine->cpu.pc = 0x7be;
            break;
        case 0x7be:
            bs_recomp_write16(machine, machine->cpu.a[5] + 7206, 0x0ea0);
            machine->cpu.pc = 0x7c4;
            break;
        case 0x7c4:
            bs_recomp_write32(machine, machine->cpu.a[5] + 7214, 0x47420);
            machine->cpu.pc = 0x7cc;
            break;
        case 0x7cc:
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | 0x00ff;
            machine->cpu.sr &= 0xffe0;
            machine->cpu.pc = 0x7d0;
            break;
        case 0x7d0:
            machine->cpu.a[7] -= 2;
            bs_recomp_write16(machine, machine->cpu.a[7],
                              (uint16_t)machine->cpu.d[0]);
            if (scroll_frame(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0x7d8;
            break;
        case 0x7d8:
            update_empty_entity_pool(machine);
            machine->cpu.pc = 0x7dc;
            break;
        case 0x7dc:
            spawn_wave_objects(machine);
            machine->cpu.pc = 0x7e0;
            break;
        case 0x7e0:
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) |
                                bs_recomp_read16(machine, machine->cpu.a[7]);
            machine->cpu.a[7] += 2;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                (((uint16_t)machine->cpu.d[0] == 0) ? 0x04 :
                 (((uint16_t)machine->cpu.d[0] & 0x8000) ? 0x08 : 0)));
            machine->cpu.pc = 0x7e2;
            break;
        case 0x7e2: {
            uint16_t counter = (uint16_t)(machine->cpu.d[0] - 1);
            machine->cpu.d[0] = (machine->cpu.d[0] & 0xffff0000) | counter;
            machine->cpu.pc = counter == 0xffff ? 0x7e6 : 0x7d0;
            break;
        }
        case 0x7e6:
            machine->cpu.a[1] =
                bs_recomp_read32(machine, machine->cpu.a[5] + 7224) + 0x0c;
            machine->cpu.a[2] = 0xb2a0;
            expand_palette(machine);
            machine->cpu.pc = 0x7f8;
            break;
        case 0x7f8:
            /* The three raw words at $7F8 are MOVE.B #$FF,-28516(A5).
             * IRA intentionally left the opcode undecoded. */
            bs_recomp_write8(machine, machine->cpu.a[5] - 28516, 0xff);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0x7fe;
            break;
        case 0x7fe:
            bs_recomp_write16(machine, machine->cpu.a[5] - 28550, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0x802;
            break;
        case 0x802:
            bs_recomp_write8(machine, 0xbfec01, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0x808;
            break;
        case 0x808:
            bs_recomp_write32(machine, machine->cpu.a[5] + 13182,
                              0x2835fffe);
            machine->cpu.pc = 0x810;
            break;
        case 0x810:
            bs_recomp_write16(machine, machine->cpu.a[6] + 150, 0x8020);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0x816;
            break;
        case 0x816:
            machine->cpu.pc = 0xaa0;
            break;
        case 0xaa0:
            bs_recomp_write16(machine, machine->cpu.a[5] + 12988, 0x0b10);
            machine->cpu.sr &= 0xfff0;
            machine->cpu.a[1] = 0x7834;
            machine->cpu.a[2] = 0x7770;
            if (bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 0x02) {
                uint32_t temporary = machine->cpu.a[1];
                machine->cpu.a[1] = machine->cpu.a[2];
                machine->cpu.a[2] = temporary;
                machine->cpu.sr = (uint16_t)(machine->cpu.sr & 0xfffb);
            } else {
                machine->cpu.sr = (uint16_t)(machine->cpu.sr | 0x04);
            }
            machine->cpu.pc = 0xabc;
            break;
        case 0xabc: {
            bs_recomp_write32(machine, machine->cpu.a[5] - 1800,
                              machine->cpu.a[1]);
            bs_recomp_write32(machine, machine->cpu.a[5] - 1796,
                              machine->cpu.a[2]);
            uint8_t branch = bs_recomp_read8(machine,
                                              machine->cpu.a[5] - 26242);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                              (branch == 0 ? 0x04 :
                               ((branch & 0x80) ? 0x08 : 0)));
            machine->cpu.pc = branch == 0 ? 0xb34 : 0xaca;
            break;
        }
        case 0xb34:
            /* Raster-window polling is a host frame-scheduling seam.  The
             * original MOVE.L/ANDI.L sequence also leaves D1's upper word
             * clear before $9C44.  Omitting that architectural result made
             * the long pointer addition inherit the music interrupt's
             * $00010000 and display unrelated chip RAM as terrain. */
            machine->cpu.d[1] = 0x00007e00;
            if (scroll_frame(machine)) return BS_RECOMP_ERROR;
            machine->cpu.pc = 0xb54;
            break;
        case 0xb54: {
            int update = update_type20_mode0_pool(machine);
            if (update != BS_RECOMP_OK) return update;
            machine->cpu.pc = 0xb58;
            break;
        }
        case 0xb58:
            /* Raster-window polling is a host frame-scheduling seam. */
            machine->cpu.d[1] = 0x00011b00;
            machine->cpu.sr &= 0xfff0;
            machine->cpu.pc = 0xb6a;
            break;
        case 0xb6a:
            machine->cpu.a[4] =
                bs_recomp_read32(machine, machine->cpu.a[5] - 1796);
            bs_recomp_write16(machine, machine->cpu.a[6] + 66, 0);
            bs_recomp_write16(machine, machine->cpu.a[6] + 64, 0x09f0);
            bs_recomp_write32(machine, machine->cpu.a[6] + 68, 0xffffffff);
            if (!machine->external_playfield_restore)
                restore_previous_draws(machine, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xb6e;
            break;
        case 0xb6e:
            draw_object_render_list(machine, 0);
            /* The reference's raster-time music interrupt leaves D1's upper
             * word set and clears D2 at this boundary; retain that scheduler
             * boundary contract without executing guest interrupt code. */
            machine->cpu.d[1] |= UINT32_C(0x00010000);
            machine->cpu.d[2] = 0;
            machine->cpu.pc = 0xb72;
            break;
        case 0xb72:
            bs_recomp_write8(machine, machine->cpu.a[5] - 1792, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xb76;
            break;
        case 0xb76:
            bs_recomp_write8(machine, machine->cpu.a[5] - 1791, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xb7a;
            break;
        case 0xb7a: {
            int projectiles = update_enemy_projectile_pool(machine);
            if (projectiles != BS_RECOMP_OK) return projectiles;
            machine->cpu.pc = 0xb7e;
            break;
        }
        case 0xb7e:
            bs_recomp_write8(machine, machine->cpu.a[5] - 1791,
                (uint8_t)~bs_recomp_read8(machine,
                                           machine->cpu.a[5] - 1791));
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0xb82;
            break;
        case 0xb82: {
            int projectiles = update_enemy_projectile_pool(machine);
            if (projectiles != BS_RECOMP_OK) return projectiles;
            machine->cpu.pc = 0xb86;
            break;
        }
        case 0xb86:
            /* End-of-window raster poll; equality with $12600 sets Z. */
            machine->cpu.d[1] = 0x00012600;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xba0;
            break;
        case 0xba0:
            if (bs_recomp_read16(machine, machine->cpu.a[5] + 8514) == 0 &&
                (int16_t)bs_recomp_read16(machine,
                                           machine->cpu.a[5] - 28550) <
                    0x05dc) {
                machine->cpu.sr =
                    (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
                machine->cpu.pc = 0xbae;
            } else {
                machine->cpu.pc = 0xbb2;
            }
            break;
        case 0xbae:
            rebuild_hud_copper(machine);
            machine->cpu.pc = 0xbb2;
            break;
        case 0xbb2:
            machine->cpu.a[4] =
                bs_recomp_read32(machine, machine->cpu.a[5] - 1796);
            bs_recomp_write16(machine, machine->cpu.a[6] + 66, 0);
            bs_recomp_write16(machine, machine->cpu.a[6] + 64, 0x09f0);
            bs_recomp_write32(machine, machine->cpu.a[6] + 68, 0xffffffff);
            if (!machine->external_playfield_restore)
                restore_previous_draws(machine, 1);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xbb6;
            break;
        case 0xbb6:
            draw_object_render_list(machine, 1);
            machine->cpu.pc = 0xbba;
            break;
        case 0xbba:
            bs_recomp_write8(machine, machine->cpu.a[5] - 1792,
                (uint8_t)~bs_recomp_read8(machine,
                                           machine->cpu.a[5] - 1792));
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0xbbe;
            break;
        case 0xbbe:
            bs_recomp_write8(machine, machine->cpu.a[5] - 1791, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xbc2;
            break;
        case 0xbc2: {
            int projectiles = update_enemy_projectile_pool(machine);
            if (projectiles != BS_RECOMP_OK) return projectiles;
            machine->cpu.pc = 0xbc6;
            break;
        }
        case 0xbc6:
            bs_recomp_write8(machine, machine->cpu.a[5] - 1791,
                (uint8_t)~bs_recomp_read8(machine,
                                           machine->cpu.a[5] - 1791));
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0xbca;
            break;
        case 0xbca: {
            int projectiles = update_enemy_projectile_pool(machine);
            if (projectiles != BS_RECOMP_OK) return projectiles;
            machine->cpu.pc = 0xbce;
            break;
        }
        case 0xbce:
            machine->cpu.d[1] = 0x0000d800;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
            machine->cpu.pc = 0xbe8;
            break;
        case 0xbe8:
            bs_recomp_write16(machine, machine->cpu.a[5] - 28552,
                (uint16_t)(bs_recomp_read16(machine,
                                             machine->cpu.a[5] - 28552) + 1));
            bs_recomp_write16(machine, machine->cpu.a[5] - 11824, 0);
            if (bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 1)
                bs_recomp_write16(machine, machine->cpu.a[5] - 11824,
                                  0x2000);
            bs_recomp_write32(machine, machine->cpu.a[6] + 68, 0xffffffff);
            bs_recomp_write16(machine, machine->cpu.a[6] + 64, 0x09f0);
            bs_recomp_write16(machine, machine->cpu.a[6] + 66, 0);
            bs_recomp_write16(machine, machine->cpu.a[6] + 100, 0);
            bs_recomp_write16(machine, machine->cpu.a[6] + 102, 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xbec;
            break;
        case 0xbec:
            if (bs_recomp_read16(machine, machine->cpu.a[5] + 8514) == 0 &&
                (int16_t)bs_recomp_read16(machine,
                                           machine->cpu.a[5] - 28550) <
                    0x05dc) {
                machine->cpu.sr =
                    (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
                machine->cpu.pc = 0xbfa;
            } else {
                machine->cpu.pc = 0xc18;
            }
            break;
        case 0xbfa: {
            int input = update_player_input(machine);
            if (input != BS_RECOMP_OK) return input;
            machine->cpu.pc = 0xc00;
            break;
        }
        case 0xc00: {
            int timers = update_inactive_player_timers(machine);
            if (timers != BS_RECOMP_OK) return timers;
            machine->cpu.pc = 0xc04;
            break;
        }
        case 0xc04:
            update_respawn_masks(machine);
            machine->cpu.pc = 0xc08;
            break;
        case 0xc08: {
            int players = update_respawning_players(machine);
            if (players != BS_RECOMP_OK) return players;
            machine->cpu.pc = 0xc0c;
            break;
        }
        case 0xc0c: {
            int lists = build_ship_sprite_lists(machine);
            if (lists != BS_RECOMP_OK) return lists;
            machine->cpu.pc = 0xc10;
            break;
        }
        case 0xc10:
            update_empty_player_burst(machine);
            machine->cpu.pc = 0xc14;
            break;
        case 0xc14: {
            int effects = update_empty_effect_pool(machine);
            if (effects != BS_RECOMP_OK) return effects;
            machine->cpu.pc = 0xc18;
            break;
        }
        case 0xc18:
            update_sprite_bitplane_pointers(machine);
            machine->cpu.pc = 0xc1c;
            break;
        case 0xc1c: {
            uint16_t active = bs_recomp_read16(machine,
                                                machine->cpu.a[5] + 8514);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                              (active == 0 ? 0x04 :
                               ((active & 0x8000) ? 0x08 : 0)));
            machine->cpu.pc = 0xc22;
            break;
        }
        case 0xc22:
            if (bs_recomp_read8(machine, machine->cpu.a[5] - 28551) & 2) {
                bs_recomp_write32(machine, machine->cpu.a[5] - 18624,
                                  0x4f46);
                bs_recomp_write32(machine, machine->cpu.a[5] - 18620,
                                  0x4fb8);
            } else {
                bs_recomp_write32(machine, machine->cpu.a[5] - 18624,
                                  0x4e3c);
                bs_recomp_write32(machine, machine->cpu.a[5] - 18620,
                                  0x4eae);
            }
            machine->cpu.sr &= 0xfff0;
            machine->cpu.pc = 0xc4c;
            break;
        case 0xc4c:
            collide_player_shots_with_projectiles(machine);
            machine->cpu.pc = 0xc50;
            break;
        case 0xc50:
            collide_player_shots_with_entities(machine);
            machine->cpu.pc = 0xc54;
            break;
        case 0xc54:
            collide_player_with_effects(machine);
            machine->cpu.pc = 0xc58;
            break;
        case 0xc58:
            collide_player_with_projectiles(machine);
            machine->cpu.pc = 0xc5c;
            break;
        case 0xc5c:
            update_demo_scoreboard(machine);
            machine->cpu.pc = 0xc60;
            break;
        case 0xc60:
            update_inactive_credit_state(machine);
            machine->cpu.pc = 0xc64;
            break;
        case 0xc64:
            if (bs_recomp_read16(machine, machine->cpu.a[5] + 8514) == 0 &&
                (int16_t)bs_recomp_read16(machine,
                                           machine->cpu.a[5] - 28550) <
                    0x05dc) {
                machine->cpu.sr =
                    (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
                machine->cpu.pc = 0xc72;
            } else {
                machine->cpu.pc = 0xc7e;
            }
            break;
        case 0xc72:
            rebuild_hud_copper(machine);
            machine->cpu.pc = 0xc76;
            break;
        case 0xc76:
            update_inactive_stage_palette(machine);
            machine->cpu.pc = 0xc7a;
            break;
        case 0xc7a:
            update_frame_palette_accents(machine);
            machine->cpu.pc = 0xc7e;
            break;
        case 0xc7e: {
            int impacts = scan_pending_impact_flags(machine);
            if (impacts != BS_RECOMP_OK) return impacts;
            machine->cpu.pc = 0xc82;
            break;
        }
        case 0xc82: {
            uint8_t demo = bs_recomp_read8(machine,
                                            machine->cpu.a[5] - 28516);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                              (demo == 0 ? 0x04 :
                               ((demo & 0x80) ? 0x08 : 0)));
            machine->cpu.pc = 0xc86;
            break;
        }
        case 0xc86:
            /* Raster wait exits as the beam drops below $A000. */
            machine->cpu.d[1] = 0;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
            machine->cpu.pc = 0xc98;
            break;
        case 0xc98:
            finish_gameplay_frame(machine);
            machine->cpu.pc = 0xc9c;
            break;
        case 0xc9c:
            if (bs_recomp_read16(machine, machine->cpu.a[5] + 8514) == 0 &&
                (int16_t)bs_recomp_read16(machine,
                                           machine->cpu.a[5] - 28550) <
                    0x05dc) {
                machine->cpu.sr =
                    (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
                machine->cpu.pc = 0xcaa;
            } else {
                machine->cpu.pc = 0xcbe;
            }
            break;
        case 0xcaa:
            update_respawn_masks(machine);
            machine->cpu.pc = 0xcae;
            break;
        case 0xcae: {
            int players = update_respawning_players(machine);
            if (players != BS_RECOMP_OK) return players;
            machine->cpu.pc = 0xcb2;
            break;
        }
        case 0xcb2: {
            int lists = build_ship_sprite_lists(machine);
            if (lists != BS_RECOMP_OK) return lists;
            machine->cpu.pc = 0xcb6;
            break;
        }
        case 0xcb6:
            update_empty_player_burst(machine);
            machine->cpu.pc = 0xcba;
            break;
        case 0xcba: {
            int effects = update_empty_effect_pool(machine);
            if (effects != BS_RECOMP_OK) return effects;
            machine->cpu.pc = 0xcbe;
            break;
        }
        case 0xcbe: {
            uint16_t active = bs_recomp_read16(machine,
                                                machine->cpu.a[5] + 8514);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                              (active == 0 ? 0x04 :
                               ((active & 0x8000) ? 0x08 : 0)));
            machine->cpu.pc = 0xcda;
            break;
        }
        case 0xcda: {
            int schedule = update_scheduled_projectiles(machine);
            if (schedule != BS_RECOMP_OK) return schedule;
            machine->cpu.pc = 0xcde;
            break;
        }
        case 0xcde:
            spawn_wave_objects(machine);
            machine->cpu.pc = 0xce2;
            break;
        case 0xce2:
            update_selected_player_score(machine);
            machine->cpu.pc = 0xce6;
            break;
        case 0xce6:
            machine->cpu.d[1] = 0x3e00;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x09);
            machine->cpu.pc = 0xd00;
            break;
        case 0xd00:
            update_sprite_bitplane_pointers(machine);
            /* The reference's CIA music tick lands on this raster boundary.
             * Preserve its architectural result without executing guest
             * interrupt code in the native dispatcher. */
            machine->cpu.d[1] = 0x00050000;
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) | 0x15);
            machine->cpu.pc = 0xd04;
            break;
        case 0xd04: {
            uint16_t active = bs_recomp_read16(machine,
                                                machine->cpu.a[5] + 8514);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) |
                              (machine->cpu.sr & 0x10) |
                              (active == 0 ? 0x04 :
                               ((active & 0x8000) ? 0x08 : 0)));
            machine->cpu.pc = 0xd0a;
            break;
        }
        case 0xd0a: {
            uint8_t active = bs_recomp_read8(machine,
                                              machine->cpu.a[5] - 4100);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) |
                              (machine->cpu.sr & 0x10) |
                              (active == 0 ? 0x04 :
                               ((active & 0x80) ? 0x08 : 0)));
            machine->cpu.pc = 0xd0e;
            break;
        }
        case 0xd0e:
            bs_recomp_write32(machine, machine->cpu.a[5] + 13182,
                              0x2835fffe);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xffe0) |
                                          (machine->cpu.sr & 0x10));
            machine->cpu.pc = 0xd16;
            break;
        case 0xd16:
            /* $D16-$D4E.  In demo mode either fire button starts a game via
             * LAB_D52; otherwise the demo runs to its $0FA0-frame expiry and
             * fades back to the title at LAB_58A.  Both button reads alias to
             * CIA-A port A, whose inputs are active low. */
            if (bs_recomp_read8(machine, machine->cpu.a[5] - 28516) == 0) {
                /* LAB_D98.  Only LAB_DFA's normal live-game continuation is
                 * translated; hardware quit buttons are a frontend concern and
                 * the game-over/loading edges remain fail-closed. */
                if (bs_recomp_read8(machine,
                                    machine->cpu.a[5] - 4096) != 0) {
                    snprintf(machine->error, sizeof machine->error,
                             "untranslated game-over path at $000d16");
                    return BS_RECOMP_UNTRANSLATED;
                }
                machine->cpu.sr =
                    (uint16_t)((machine->cpu.sr & 0xffe0) | 0x04);
                machine->cpu.pc = 0xaa0;
            } else if ((bs_recomp_read8(machine, 0xbfe003) & 0x80) == 0 ||
                       (bs_recomp_read8(machine, 0xbfe001) & 0x40) == 0) {
                /* $D1E/$D28: either fire button starts a game. */
                machine->cpu.pc = 0xd52;
            } else if ((int16_t)bs_recomp_read16(machine,
                                                 machine->cpu.a[5] - 28550) <
                       0x0fa0) {
                machine->cpu.sr =
                    (uint16_t)((machine->cpu.sr & 0xffe0) | 0x19);
                machine->cpu.pc = 0xaa0;
            } else {
                /* $D3C-$D4E: the expired demo darkens its palette and
                 * restarts the title sequence at LAB_58A. */
                bs_recomp_write32(machine, machine->cpu.a[5] + 13182,
                                  0xfffffffe);
                machine->cpu.a[1] = 0xb2a0;
                darken_palette(machine);
                machine->cpu.pc = 0x58a;
            }
            break;
        /* LAB_D52.  A fire button during the attract demo stops the music,
         * repoints the bitplanes at the title buffer, installs the gameplay
         * overlays and starts a real game at $926. */
        case 0xd52: {
            int status = request_music_stop(machine);
            if (status != BS_RECOMP_OK) return status;
            machine->cpu.pc = 0xd58;
            break;
        }
        case 0xd58:
            machine->cpu.d[1] = 0xc8b6;
            set_dreg_word(&machine->cpu.d[2], 0);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x04);
            machine->cpu.pc = 0xd60;
            break;
        case 0xd60:
            set_bitplane_pointers(machine);
            machine->cpu.pc = 0xd64;
            break;
        case 0xd64: machine->cpu.a[4] = 0x1aa0; machine->cpu.pc = 0xd6a; break;
        case 0xd6a:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0xd6e;
            break;
        case 0xd6e:
            bs_recomp_write8(machine, machine->cpu.a[5] - 26245, 0x81);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0xd74;
            break;
        case 0xd74: machine->cpu.a[4] = 0x19f8; machine->cpu.pc = 0xd7a; break;
        case 0xd7a:
            if (load_module(machine, machine->cpu.a[4]))
                return BS_RECOMP_ERROR;
            machine->cpu.pc = 0xd7e;
            break;
        case 0xd7e: {
            uint8_t value = (uint8_t)~bs_recomp_read8(machine,
                                                      machine->cpu.a[5] - 26246);
            bs_recomp_write8(machine, machine->cpu.a[5] - 26246, value);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) |
                                         (value == 0 ? 0x04 : 0) |
                                         (value & 0x80 ? 0x08 : 0));
            machine->cpu.pc = 0xd82;
            break;
        }
        case 0xd82:
            bs_recomp_write32(machine, machine->cpu.a[5] + 13182, 0xfffffffe);
            machine->cpu.sr = (uint16_t)((machine->cpu.sr & 0xfff0) | 0x08);
            machine->cpu.pc = 0xd8a;
            break;
        case 0xd8a: machine->cpu.a[1] = 0xb2a0; machine->cpu.pc = 0xd90; break;
        case 0xd90:
            darken_palette(machine);
            machine->cpu.pc = 0xd94;
            break;
        case 0xd94: machine->cpu.pc = 0x926; break;
        case 0x926:
            if (run_new_game_sequence(machine)) return BS_RECOMP_ERROR;
            break;
        default:
            machine->translated_steps--;
            return BS_RECOMP_UNTRANSLATED;
        }
    }
    return BS_RECOMP_OK;
}
