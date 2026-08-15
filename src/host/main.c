#include "amiga.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long parse_frames(const char *text)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < 0) {
        fprintf(stderr, "invalid frame count: %s\n", text);
        exit(2);
    }
    return value;
}

static int dump_frame(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return 1;
    }
    fprintf(file, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int pixel = 0; pixel < SCREEN_W * SCREEN_H; pixel++) {
        uint32_t value = framebuf[pixel];
        fputc(value & 0xff, file);
        fputc((value >> 8) & 0xff, file);
        fputc((value >> 16) & 0xff, file);
    }
    if (fclose(file)) {
        perror(path);
        return 1;
    }
    fprintf(stderr, "native: dumped %s\n", path);
    return 0;
}

static void write_le16(FILE *file, unsigned value)
{
    fputc(value & 0xff, file);
    fputc((value >> 8) & 0xff, file);
}

static void write_le32(FILE *file, unsigned value)
{
    write_le16(file, value);
    write_le16(file, value >> 16);
}

static void write_wav_header(FILE *file, unsigned frames)
{
    unsigned bytes = frames * 4;
    rewind(file);
    fwrite("RIFF", 1, 4, file); write_le32(file, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, file); write_le32(file, 16);
    write_le16(file, 1); write_le16(file, 2);
    write_le32(file, 44100); write_le32(file, 44100 * 4);
    write_le16(file, 4); write_le16(file, 16);
    fwrite("data", 1, 4, file); write_le32(file, bytes);
}

int main(int argc, char **argv)
{
    const char *data_dir = "original/whdload/BattleSquadron/data";
    long frames = 10;
    long video_from = -1;
    long expect_files = 0;
    long expect_blits = 0;
    const char *dump = NULL;
    bool mix_audio = false;
    bool autofire = false;
    long dump_state = 0;
    const char *audio_dump_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = parse_frames(argv[++i]);
        } else if (!strcmp(argv[i], "--expect-files") && i + 1 < argc) {
            expect_files = parse_frames(argv[++i]);
        } else if (!strcmp(argv[i], "--expect-blits") && i + 1 < argc) {
            expect_blits = parse_frames(argv[++i]);
        } else if (!strcmp(argv[i], "--video-from") && i + 1 < argc) {
            video_from = parse_frames(argv[++i]);
        } else if (!strcmp(argv[i], "--video")) {
            video_from = 0;
        } else if (!strcmp(argv[i], "--dump-frame") && i + 1 < argc) {
            dump = argv[++i];
        } else if (!strcmp(argv[i], "--mix-audio")) {
            mix_audio = true;
        } else if (!strcmp(argv[i], "--dump-state") && i + 1 < argc) {
            dump_state = parse_frames(argv[++i]);
        } else if (!strcmp(argv[i], "--autofire")) {
            autofire = true;
        } else if (!strcmp(argv[i], "--dump-audio") && i + 1 < argc) {
            audio_dump_path = argv[++i];
            mix_audio = true;
        } else if (!strcmp(argv[i], "--selftest")) {
            int failed = amiga_blitter_selftest();
            failed |= amiga_video_selftest();
            failed |= amiga_input_selftest();
            failed |= amiga_audio_selftest();
            return failed;
        } else {
            fprintf(stderr,
                    "usage: %s [--data DIR] [--frames N] "
                    "[--expect-files N] [--expect-blits N] "
                    "[--video|--video-from N] [--dump-frame FILE] "
                    "[--mix-audio] [--dump-audio FILE] [--autofire] [--dump-state N] "
                    "[--selftest]\n",
                    argv[0]);
            return 2;
        }
    }

    amiga_init(data_dir);
    FILE *audio_dump = NULL;
    unsigned audio_frames = 0;
    if (audio_dump_path) {
        audio_dump = fopen(audio_dump_path, "wb+");
        if (!audio_dump) { perror(audio_dump_path); return 1; }
        write_wav_header(audio_dump, 0);
    }
    while (bs_frame_no < frames && !amiga_stopped()) {
        if (autofire)
            joy_state[1] = (bs_frame_no % 100) < 20 ? 0x10 : 0;
        if (video_from >= 0 && bs_frame_no >= video_from)
            amiga_enable_video(true);
        amiga_run_frame();
        if (dump_state && bs_frame_no % dump_state == 0) {
            /* Reference values for the recompilation to diff against: the
             * player record and every live active-object slot. */
            printf("state frame=%ld t1078=%u p1 x=%u y=%u s38=%u c48=%u "
                   "d49=%u inv52=%u lives56=%u wpn60=%u\n",
                   bs_frame_no,
                   (chip[0x1078] << 8) | chip[0x1079],
                   (chip[0x4e40] << 8) | chip[0x4e41],
                   (chip[0x4e42] << 8) | chip[0x4e43],
                   chip[0x4e3c + 38], chip[0x4e3c + 48], chip[0x4e3c + 49],
                   (chip[0x4e3c + 52] << 8) | chip[0x4e3c + 53],
                   chip[0x4e3c + 56],
                   (chip[0x4e3c + 60] << 8) | chip[0x4e3c + 61]);
            for (unsigned slot = 0; slot < 18; slot++) {
                unsigned object = 0x2e040 + slot * 0x50;
                unsigned x = (chip[object] << 8) | chip[object + 1];
                if (!x) continue;
                printf("  obj %2u x=%4u y=%4u type=$%02X limit19=%3u "
                       "state25=%3u health28=%3u status30=%3u final33=%3u\n",
                       slot, x, (chip[object + 2] << 8) | chip[object + 3],
                       chip[object + 17], chip[object + 19],
                       chip[object + 25], chip[object + 28],
                       chip[object + 30], chip[object + 33]);
            }
            for (unsigned slot = 0; slot < 12; slot++) {
                unsigned record = 0x2dc80 + slot * 0x50;
                unsigned x = (chip[record] << 8) | chip[record + 1];
                if (!x) continue;
                printf("  hos %2u x=%4u y=%4u type=$%02X f29=%3u f30=$%02X "
                       "dmg62=%3u f63=%3u\n",
                       slot, x, (chip[record + 2] << 8) | chip[record + 3],
                       chip[record + 31], chip[record + 29], chip[record + 30],
                       chip[record + 62], chip[record + 63]);
            }
            fflush(stdout);
        }
        if (mix_audio) {
            int16_t audio[882 * 2];
            amiga_audio_frame();
            amiga_audio_pull(audio, 882);
            if (audio_dump) {
                fwrite(audio, sizeof audio[0], 882 * 2, audio_dump);
                audio_frames += 882;
            }
        }
    }
    amiga_report();
    if (audio_dump) {
        write_wav_header(audio_dump, audio_frames);
        fclose(audio_dump);
        fprintf(stderr, "native: dumped %s (%u frames)\n",
                audio_dump_path, audio_frames);
    }
    if (dump && dump_frame(dump)) return 1;
    if (amiga_stopped()) return 1;
    if (bs_file_load_count < expect_files) {
        fprintf(stderr, "native: expected at least %ld file loads, got %ld\n",
                expect_files, bs_file_load_count);
        return 1;
    }
    if (bs_blit_count < expect_blits) {
        fprintf(stderr, "native: expected at least %ld blits, got %ld\n",
                expect_blits, bs_blit_count);
        return 1;
    }
    return 0;
}
