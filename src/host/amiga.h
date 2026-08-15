#ifndef BATTLE_SQUADRON_AMIGA_H
#define BATTLE_SQUADRON_AMIGA_H

#include <stdbool.h>
#include <stdint.h>

#define CHIP_SIZE 0x80000
#define SCREEN_W 320
#define SCREEN_H 256
#define LINES_PER_FRAME 312
#define CYCLES_PER_LINE 455

/* Musashi's callback is function-like, which modern CMake deliberately does
 * not pass as a command-line definition.  This header is force-included for
 * every core translation unit, so keep the shared hook spelling here. */
#ifndef M68K_INSTRUCTION_CALLBACK
#define M68K_INSTRUCTION_CALLBACK(pc) bs_instr_hook(pc)
#endif

extern uint8_t chip[CHIP_SIZE];
extern uint32_t framebuf[SCREEN_W * SCREEN_H];
extern long bs_frame_no;
extern long bs_blit_count;
extern long bs_file_load_count;
extern long bs_copper_moves;
extern long bs_nonblack_pixels;
extern long bs_audio_writes;
extern uint8_t joy_state[2];

void amiga_init(const char *data_dir);
void amiga_run_frame(void);
void amiga_enable_video(bool enabled);
void amiga_key_event(uint8_t rawcode, bool up);
void amiga_audio_frame(void);
int amiga_audio_pull(int16_t *output, int frames);
int amiga_audio_fill(void);
void bs_instr_hook(unsigned int pc);
int amiga_blitter_selftest(void);
int amiga_video_selftest(void);
int amiga_input_selftest(void);
int amiga_audio_selftest(void);
bool amiga_stopped(void);
void amiga_report(void);

unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

#endif
