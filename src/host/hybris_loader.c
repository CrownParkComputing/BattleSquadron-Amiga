#include "hybris_loader.h"

#include "amiga.h"
#include "hybris_files.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Intercept the cracked loader's trackloader and serve files instead.
 *
 * The routine at HYBRIS_READ_TRACKS is entered with the start track in D0 and
 * the track count in D1; its caller has already stored the destination in the
 * long at HYBRIS_DEST_PTR, and HYBRIS_FILE_PTR points at the descriptor of
 * the file being loaded, whose length sits at descriptor+2.  Those four facts
 * are the whole contract -- verified against the loader's own dispatch table,
 * which names the same (track, count) pairs the extracted disk map derives.
 *
 * Serving it here means no MFM decode, no DSKLEN DMA and no drive stepping
 * ever runs: the same shape as Battle Squadron's named-file hook. */
#define HYBRIS_READ_TRACKS 0x00eed8
#define HYBRIS_DEST_PTR    0x00f130
#define HYBRIS_FILE_PTR    0x002e28

long hybris_load_count;
long hybris_load_bytes;

static bool trace;

static uint32_t peek32(uint32_t address)
{
    return ((uint32_t)chip[address] << 24) | ((uint32_t)chip[address + 1] << 16)
         | ((uint32_t)chip[address + 2] << 8) | chip[address + 3];
}

/* The intro overlay is a table of eight-character slots, each with its own
 * screen position: "DSI-LOGO" "PRESENTS" "HYBRIS  " "BY      " "QUARTEX!"
 * "MADEBY  " "MARTIN  " "PEDERSEN".  Eight characters per slot is the whole
 * budget, so the replacement is written to read across the eight lines as
 * they appear one after another. */
static const struct { const char *from, *to; } INTRO_TEXT[] = {
    { "DSI-LOGO", "RETRO   " },
    { "PRESENTS", "RECOMPS " },
    { "HYBRIS  ", "PRESENT " },
    { "BY      ", "IN      " },   /* this slot sits furthest right: keep it
                                   * short, the original word here was "BY" */
    { "QUARTEX!", "2026    " },
    { "MADEBY  ", "NATIVE  " },
    { "MARTIN  ", "WIN LNX " },
    { "PEDERSEN", "IOS ANDR" },
};

/* The table is PACKED inside the executable, so it does not exist until the
 * decruncher has run -- which is why this sweeps chip RAM once, on the first
 * file load, rather than patching the file or the image at load time. */
static void rewrite_intro_text(void)
{
    size_t count = sizeof INTRO_TEXT / sizeof INTRO_TEXT[0];
    int replaced = 0;
    for (uint32_t at = 0; at + 8 <= CHIP_SIZE; at++)
        for (size_t i = 0; i < count; i++)
            if (!memcmp(chip + at, INTRO_TEXT[i].from, 8)) {
                memcpy(chip + at, INTRO_TEXT[i].to, 8);
                fprintf(stderr, "hybris: intro slot $%06x \"%s\" -> \"%s\"\n",
                        at, INTRO_TEXT[i].from, INTRO_TEXT[i].to);
                replaced++;
                at += 7;
                break;
            }
    if (!replaced) fprintf(stderr, "hybris: intro text not found to rewrite\n");
}

/* $C65E is the BSET #5 the SPACE key reaches, $C686 the one a POT0DAT
 * change reaches: the two routes to the same action bit. */
long hybris_action_key, hybris_action_pot;

static void hybris_hook(unsigned int pc)
{
    if (pc == 0x00c65e) { hybris_action_key++; return; }
    if (pc == 0x00c686) { hybris_action_pot++; return; }
    if (pc != HYBRIS_READ_TRACKS) return;

    int track = (int)(m68k_get_reg(NULL, M68K_REG_D0) & 0xff);
    uint32_t tracks = m68k_get_reg(NULL, M68K_REG_D1) & 0xffff;
    uint32_t destination = peek32(HYBRIS_DEST_PTR);
    uint32_t descriptor = peek32(HYBRIS_FILE_PTR);
    uint32_t declared = descriptor + 6 <= CHIP_SIZE ? peek32(descriptor + 2) : 0;

    /* The loader would write whole tracks and stop at the file's declared
     * length; serve exactly that many bytes. */
    uint32_t length = tracks * HYBRIS_TRACK_BYTES;
    if (declared && declared < length) length = declared;

    const char *id = hybris_files_id(track);
    if (!id || destination >= CHIP_SIZE || length > CHIP_SIZE - destination) {
        fprintf(stderr, "hybris: unserviceable read, track %d x%u -> $%06x "
                "(%u bytes)\n", track, tracks, destination, length);
        amiga_stop();
        return;
    }

    uint32_t served = hybris_files_read(track, chip + destination, length);
    if (served != length) {
        fprintf(stderr, "hybris: file %s served %u of %u bytes from track %d\n",
                id, served, length, track);
        amiga_stop();
        return;
    }
    if (!hybris_load_count) rewrite_intro_text();
    hybris_load_count++;
    hybris_load_bytes += served;
    if (trace)
        fprintf(stderr, "hybris: load %s (track %d x%u, %u bytes) -> $%06x\n",
                id, track, tracks, served, destination);

    amiga_return_from_hook();
}

/* Re-sign the loader.  The cracked disk carries a 24-character signature in
 * the payload the decruncher copies down to $100; replacing it in RAM after
 * the hunks load leaves the extracted file byte-identical to the disk, and
 * the replacement is the same length so nothing moves. */
static const char CRACK_SIGNATURE[] = "CRACKED BY QUARTEX (ROB)";
static const char OUR_SIGNATURE[]   = "DECOMPED BY RETRO RECOMP";

static void resign_loader(void)
{
    size_t length = sizeof CRACK_SIGNATURE - 1;
    for (uint32_t at = 0; at + length < CHIP_SIZE; at++) {
        if (memcmp(chip + at, CRACK_SIGNATURE, length)) continue;
        memcpy(chip + at, OUR_SIGNATURE, length);
        fprintf(stderr, "hybris: signature at $%06x replaced with \"%s\"\n",
                at, OUR_SIGNATURE);
        return;
    }
}

/* The armoured alien's artwork.  Its frames sit together in the loaded data,
 * five planes each (four colour, the fifth an inverted mask), every plane
 * padded to 128 bytes.  Claiming the range means the blitter draws none of
 * it and the frontend paints a replacement at the same screen rectangle --
 * at whatever colour depth and resolution it likes, because that is no
 * longer the chipset's business. */
#define ALIEN_ART_LOW   0x018000
#define ALIEN_ART_HIGH  0x018600
#define ALIEN_ID        0

bool hybris_loader_install(const char *directory)
{
    if (!hybris_files_load(directory)) return false;
    if (getenv("HYBRIS_REMASTER"))
        amiga_register_replacement(ALIEN_ART_LOW, ALIEN_ART_HIGH, ALIEN_ID);
    resign_loader();
    trace = getenv("HYBRIS_TRACE_LOAD") != NULL;
    amiga_set_pc_hook(hybris_hook);
    return true;
}
