/* Hybris, playable.
 *
 * Same chipset and same pad handling as the Battle Squadron frontend, but
 * booted purely from files: the loader executable is hunk-loaded and every
 * data load it makes is served from original/hybris/data by the $EED8 hook.
 * Nothing here reads a disk image. */
#include "amiga.h"
#include "exeboot.h"
#include "hybris_loader.h"
#include "pad.h"

#include "raylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The cracked loader spends its first few hundred emulated frames in a
 * bit-stream decruncher with the display off.  Run those at host speed --
 * they are silent and show nothing -- so the window opens on the game
 * rather than on a black screen. */
#define DECRUNCH_FRAMES 320

enum {
    RAW_SPACE  = 0x40,
    RAW_RETURN = 0x44,
    RAW_ESCAPE = 0x45,
    RAW_F1     = 0x50
};

/* An alternative soundtrack: Battle Squadron's music, captured from that
 * port's own Paula output and played instead of Hybris' when toggled.  It
 * shares the one output stream, so the switch is instant and there is never
 * more than one thing making noise. */
static int16_t *track_samples;      /* interleaved stereo */
static long track_frames;
static long track_position;
static volatile bool track_active;

static bool load_track(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t header[44];
    if (fread(header, 1, sizeof header, file) != sizeof header ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        fclose(file); return false;
    }
    long bytes = (long)header[40] | ((long)header[41] << 8) |
                 ((long)header[42] << 16) | ((long)header[43] << 24);
    track_samples = malloc((size_t)bytes);
    if (!track_samples) { fclose(file); return false; }
    size_t got = fread(track_samples, 1, (size_t)bytes, file);
    fclose(file);
    track_frames = (long)got / 4;              /* 16-bit stereo */
    return track_frames > 0;
}

static void audio_callback(void *buffer, unsigned int frames)
{
    int16_t *out = (int16_t *)buffer;
    if (!track_active || !track_frames) {
        amiga_audio_pull(out, (int)frames);
        return;
    }
    for (unsigned int i = 0; i < frames; i++) {
        out[i * 2] = track_samples[track_position * 2];
        out[i * 2 + 1] = track_samples[track_position * 2 + 1];
        if (++track_position >= track_frames) track_position = 0;
    }
}

/* The title screen is the one that opens the wide 336-pixel display window;
 * every other screen uses a different DIWSTRT, so this identifies it without
 * having to guess from pixels. */
#define TITLE_DIWSTRT 0x4378

/* Fire is passed straight through: the game already autofires while the
 * button is held, at a cadence IT controls.  Pulsing the input does not beat
 * that -- measured over identical 3000-frame runs, held / pulsed-every-frame
 * / pulsed-every-12-frames gave 24129 / 24227 / 24002 blits -- and pulsing
 * makes it burst two or three shots and stall, because the pulses fall out
 * of step with the game's own sampling.  Firing faster than the game's rate
 * needs its cooldown patched, not a different input pattern. */
static uint8_t apply_autofire(uint8_t sticks)
{
    return sticks;
}

/* Hybris' two extra actions are KEYS, not joystick buttons: it never reads
 * POTGOR at all.  Its input aggregator at $C654 sets bit 5 of its control
 * word from SPACE and bit 6 from RETURN, and the host's keyboard encoder
 * turns rawcode $40 into the SDR byte $7F and $44 into $77 -- exactly the
 * two values that code compares against.  So a pad button reaches them by
 * pressing the key. */
static void pad_key(bool down, bool *was_down, uint8_t rawcode)
{
    if (down && !*was_down) amiga_key_event(rawcode, false);
    if (!down && *was_down) amiga_key_event(rawcode, true);
    *was_down = down;
}

static bool pad_face(int pad, int js_button, int raylib_button)
{
    if (js_present(pad)) return js_button_down(pad, js_button);
    return IsGamepadAvailable(pad) && IsGamepadButtonDown(pad, raylib_button);
}

/* Directions and fire only -- SPACE and RETURN are left free for the two
 * actions instead of doubling as fire the way Battle Squadron maps them. */
static uint8_t hybris_keyboard(void)
{
    uint8_t state = 0;
    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) state |= 0x01;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) state |= 0x02;
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) state |= 0x04;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) state |= 0x08;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_Z) ||
        IsKeyDown(KEY_LEFT_ALT)) state |= 0x10;
    return state;
}

/* Hybris is a one-player joystick game in port 1, which is JOY1DAT, which is
 * joy_state[1].  Both pads feed it so either one plays. */
static void update_input(void)
{
    uint8_t sticks = hybris_keyboard() | gamepad_stick(0) | gamepad_stick(1);
    /* Take the second button off the shoulder buttons: L1 toggles the
     * soundtrack, and B alone is the second button. */
    sticks &= (uint8_t)~0x20;
    if (pad_face(0, 1, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
        pad_face(1, 1, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
        IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_X))
        sticks |= 0x20;
    sticks = apply_autofire(sticks);
    joy_state[1] = sticks;
    /* The second button is read as a POT0DAT change, not through POTGOR or
     * the fire line, so it has to appear on port 0 as well. */
    joy_state[0] = (uint8_t)(sticks & 0x20);

    /* X and Y on either pad press the two action keys. */
    static bool space_was, return_was;
    bool space_down = pad_face(0, 2, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ||
                      pad_face(1, 2, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
    bool return_down = pad_face(0, 3, GAMEPAD_BUTTON_RIGHT_FACE_UP) ||
                       pad_face(1, 3, GAMEPAD_BUTTON_RIGHT_FACE_UP);
    pad_key(space_down, &space_was, RAW_SPACE);
    pad_key(return_down, &return_was, RAW_RETURN);

    map_raw_key(KEY_SPACE, RAW_SPACE);
    map_raw_key(KEY_ENTER, RAW_RETURN);
    map_raw_key(KEY_ESCAPE, RAW_ESCAPE);
    for (int number = 0; number < 10; number++)
        map_raw_key(KEY_F1 + number, (uint8_t)(RAW_F1 + number));
}

static void draw_message(const char *text)
{
    BeginDrawing();
    ClearBackground(BLACK);
    int width = MeasureText(text, 20);
    DrawText(text, (GetScreenWidth() - width) / 2,
             GetScreenHeight() / 2 - 10, 20, (Color){255, 220, 90, 255});
    EndDrawing();
}

int main(int argc, char **argv)
{
#ifdef PLATFORM_ANDROID
    /* Assets are unpacked at the APK root, so the paths lose their leading
     * directory: original/ and assets/ are the two asset source folders. */
    const char *data = "hybris";
    const char *sprite_folder = "sprites";
    const char *logo_path = "retro-recompilation.png";
    const char *track_path = "battle-squadron-theme.wav";
#else
    const char *data = "original/hybris";
    const char *sprite_folder = "assets/sprites";
    const char *logo_path = "assets/retro-recompilation.png";
    const char *track_path = "assets/battle-squadron-theme.wav";
#endif
    const char *exe = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--data DIR] [--exe HUNKFILE]\n",
                    argv[0]);
            return 2;
        }
    }
    char exe_path[640];
    if (!exe) {
        snprintf(exe_path, sizeof exe_path, "%s/hybris.exe", data);
        exe = exe_path;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_W * 3, SCREEN_H * 3, "Hybris - Amiga native runner");
    SetExitKey(KEY_NULL);            /* ESC belongs to the game */
    SetTargetFPS(50);
    for (int pad = 0; pad < 4; pad++)
        if (IsGamepadAvailable(pad))
            fprintf(stderr, "controller %d: %s\n", pad, GetGamepadName(pad));

    ExeBoot boot;
    if (!exeboot(exe, &boot)) { CloseWindow(); return 1; }
    hybris_set_sprite_folder(sprite_folder);
    if (!hybris_loader_install(data)) { CloseWindow(); return 1; }
    fprintf(stderr, "hybris: %d hunk(s), entry $%06x, ends $%06x\n",
            boot.hunks, boot.entry, boot.end);

    int16_t discard[1024 * 2];
    while (bs_frame_no < DECRUNCH_FRAMES && !amiga_stopped()) {
        amiga_run_frame();
        amiga_audio_frame();
        amiga_audio_pull(discard, 882);
        if ((bs_frame_no & 31) == 0) {
            draw_message("LOADING");
            if (WindowShouldClose()) { CloseWindow(); return 0; }
        }
    }

    amiga_enable_video(true);
    for (int frame = 0; frame < 3 && !amiga_stopped(); frame++)
        amiga_run_frame();

    Image image = {
        .data = framebuf,
        .width = SCREEN_W,
        .height = SCREEN_H,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
    Texture2D texture = LoadTextureFromImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    /* The title screen's window ends well above the bottom of the raster, so
     * the band under it is border the game never draws in -- room for our
     * own mark without covering anything of the original. */
    /* Remastered artwork, drawn over the frame wherever the game would have
     * blitted the original.  24-bit with alpha, at higher resolution than the
     * Amiga could hold. */
    Texture2D art[32] = {0};
    for (int i = 0; i < hybris_sprite_art_count && i < 32; i++) {
        art[i] = LoadTexture(hybris_sprite_art[i].path);
        if (art[i].id) SetTextureFilter(art[i], TEXTURE_FILTER_BILINEAR);
    }

    if (load_track(track_path))
        fprintf(stderr, "alternative soundtrack: %.1fs loaded (L1 toggles)\n",
                track_frames / 44100.0);

    Texture2D logo = LoadTexture(logo_path);
    SetTextureFilter(logo, TEXTURE_FILTER_BILINEAR);

    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(256);
    AudioStream stream = LoadAudioStream(44100, 16, 2);
    SetAudioStreamCallback(stream, audio_callback);
    bool audio_started = false;

    bool paused = false;
    bool start_was_down = false;
    while (!WindowShouldClose() && !amiga_stopped()) {
        js_poll();
        /* P on the keyboard, START (or BACK) on a pad. */
        bool start_down = js_present(0)
            ? (js_button_down(0, 7) || js_button_down(0, 6))
            : (IsGamepadAvailable(0) &&
               (IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_RIGHT) ||
                IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_LEFT)));
        if (IsKeyPressed(KEY_P) || (start_down && !start_was_down))
            paused = !paused;
        start_was_down = start_down;

        if (paused) {
            /* Keep presenting the last frame, but run no emulation and pass
             * no input through, so the ship does not drift while paused. */
            BeginDrawing();
            ClearBackground(BLACK);
            Rectangle where = fit_screen();
            DrawTexturePro(texture, (Rectangle){0, 0, SCREEN_W, SCREEN_H},
                           where, (Vector2){0, 0}, 0, WHITE);
            DrawText("PAUSED  -  P to resume", (int)where.x + 12,
                     (int)where.y + 12, 20, (Color){255, 220, 90, 255});
            EndDrawing();
            continue;
        }

        /* L1 swaps between the game's own music and the alternative. */
        static bool l1_was_down;
        bool l1_down = pad_face(0, 4, GAMEPAD_BUTTON_LEFT_TRIGGER_1) ||
                       pad_face(1, 4, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
        if ((l1_down && !l1_was_down) || IsKeyPressed(KEY_F2)) {
            track_active = !track_active;
            track_position = 0;
            fprintf(stderr, "soundtrack: %s\n",
                    track_active ? "Battle Squadron" : "Hybris");
        }
        l1_was_down = l1_down;

        /* F12 freezes everything about the current frame to disk, so a
         * glitch that only happens while playing can be diagnosed after the
         * fact instead of from a photograph. */
        if (IsKeyPressed(KEY_F12)) {
            FILE *file = fopen("build/hybris_glitch.ppm", "wb");
            if (file) {
                fprintf(file, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
                for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                    uint32_t pixel = framebuf[i];
                    uint8_t rgb[3] = { (uint8_t)(pixel >> 16),
                                       (uint8_t)(pixel >> 8), (uint8_t)pixel };
                    fwrite(rgb, 1, 3, file);
                }
                fclose(file);
            }
            file = fopen("build/hybris_glitch.bin", "wb");
            if (file) { fwrite(chip, 1, CHIP_SIZE, file); fclose(file); }
            uint16_t bplcon0, dmacon, diwstrt, diwstop;
            amiga_display_state(&bplcon0, &dmacon, &diwstrt, &diwstop);
            fprintf(stderr, "glitch dump at frame %ld: bplcon0=$%04x "
                    "(%d planes) dmacon=$%04x diw=$%04x/$%04x\n",
                    bs_frame_no, bplcon0, (bplcon0 >> 12) & 7, dmacon,
                    diwstrt, diwstop);
        }

        /* [ and ] nudge the playfield against the sprites, \ resets: the
         * quickest way to see which alignment matches the real machine. */
        if (IsKeyPressed(KEY_LEFT_BRACKET)) bs_playfield_shift--;
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) bs_playfield_shift++;
        if (IsKeyPressed(KEY_BACKSLASH)) bs_playfield_shift = 0;

        update_input();
        amiga_run_frame();
        amiga_audio_frame();
        if (track_active) {
            /* Nothing is consuming Paula while the other track plays, so
             * drain its ring rather than letting it back up. */
            static int16_t discard_ring[882 * 2];
            amiga_audio_pull(discard_ring, 882);
        }
        if (!audio_started && amiga_audio_fill() >= 1764) {
            PlayAudioStream(stream);
            audio_started = true;
        }
        UpdateTexture(texture, framebuf);
        BeginDrawing();
        ClearBackground(BLACK);
        Rectangle where = fit_screen();
        DrawTexturePro(texture, (Rectangle){0, 0, SCREEN_W, SCREEN_H},
                       where, (Vector2){0, 0}, 0, WHITE);
        /* Replacement sprites: one request per object the game drew. */
        {
            float sx = where.width / (float)SCREEN_W;
            float sy = where.height / (float)SCREEN_H;
            for (int i = 0; i < bs_sprite_draw_count; i++) {
                const BsSpriteDraw *d = &bs_sprite_draws[i];
                int slot = d->id - 1;
                if (slot < 0 || slot >= 32 || !art[slot].id) continue;
                DrawTexturePro(art[slot],
                    (Rectangle){0, 0, (float)art[slot].width,
                                (float)art[slot].height},
                    (Rectangle){where.x + d->x * sx, where.y + d->y * sy,
                                d->width * sx, d->height * sy},
                    (Vector2){0, 0}, 0, WHITE);
            }
        }

        uint16_t diwstrt;
        amiga_display_state(NULL, NULL, &diwstrt, NULL);
        if (logo.id && diwstrt == TITLE_DIWSTRT) {
            /* Strictly inside the border under the picture, measured from
             * where the display window actually ends this frame. */
            int first_row, last_row;
            amiga_display_bounds(&first_row, &last_row);
            float scale_y = where.height / (float)SCREEN_H;
            float band = (float)(SCREEN_H - last_row) - 2.0f;
            if (band > 6.0f) {
                float box_h = band * scale_y * 0.80f;
                float box_w = box_h * logo.width / (float)logo.height;
                float max_w = where.width * 0.70f;
                if (box_w > max_w) {
                    box_w = max_w;
                    box_h = box_w * logo.height / (float)logo.width;
                }
                DrawTexturePro(logo,
                    (Rectangle){0, 0, (float)logo.width, (float)logo.height},
                    (Rectangle){where.x + (where.width - box_w) * 0.5f,
                                where.y + (last_row + 1) * scale_y +
                                    (band * scale_y - box_h) * 0.5f,
                                box_w, box_h},
                    (Vector2){0, 0}, 0, WHITE);
            }
        }

        if (bs_playfield_shift)
            DrawText(TextFormat("playfield %+d  ( [ ] to nudge, \\ resets )",
                                bs_playfield_shift),
                     (int)where.x + 12, (int)where.y + 12, 20,
                     (Color){255, 220, 90, 255});
        EndDrawing();
    }

    if (audio_started) StopAudioStream(stream);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    UnloadTexture(texture);
    if (logo.id) UnloadTexture(logo);
    for (int i = 0; i < hybris_sprite_art_count && i < 32; i++)
        if (art[i].id) UnloadTexture(art[i]);
    CloseWindow();
    printf("hybris: %ld frames, %ld file loads (%ld bytes)\n",
           bs_frame_no, hybris_load_count, hybris_load_bytes);
    return amiga_stopped() ? 1 : 0;
}
