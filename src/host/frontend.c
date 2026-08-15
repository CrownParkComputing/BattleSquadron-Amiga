#include "amiga.h"
#include "raylib.h"

#include <stdio.h>
#include <string.h>

#ifdef PLATFORM_ANDROID
#include <android/api-level.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

/* Exported by raylib's Android platform layer (not yet in raylib.h). */
extern struct android_app *GetAndroidApp(void);
#endif

enum {
    RAW_SPACE = 0x40,
    RAW_RETURN = 0x44,
    RAW_ESCAPE = 0x45,
    RAW_F1 = 0x50
};

static void map_raw_key(int key, uint8_t raw)
{
    if (IsKeyPressed(key)) amiga_key_event(raw, false);
    if (IsKeyReleased(key)) amiga_key_event(raw, true);
}

static void audio_callback(void *buffer, unsigned int frames)
{
    amiga_audio_pull((int16_t *)buffer, (int)frames);
}

#ifdef PLATFORM_ANDROID
static void request_pal_frame_rate(void)
{
    struct android_app *app = GetAndroidApp();
    int result = -1;
    if (app && app->window && android_get_device_api_level() >= 30)
        result = ANativeWindow_setFrameRate(
            app->window, 50.0f,
            ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    TraceLog(LOG_WARNING, "ANDROID: requested PAL 50 Hz presentation (%d)",
             result);
}
#endif

static uint8_t keyboard_stick(bool arrows)
{
    uint8_t state = 0;
    if (IsKeyDown(arrows ? KEY_UP : KEY_W)) state |= 0x01;
    if (IsKeyDown(arrows ? KEY_DOWN : KEY_S)) state |= 0x02;
    if (IsKeyDown(arrows ? KEY_LEFT : KEY_A)) state |= 0x04;
    if (IsKeyDown(arrows ? KEY_RIGHT : KEY_D)) state |= 0x08;
    if (arrows) {
        if (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_LEFT_CONTROL) ||
            IsKeyDown(KEY_ENTER)) state |= 0x10;
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_LEFT_SHIFT)) state |= 0x20;
    } else {
        if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_C)) state |= 0x10;
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_TAB)) state |= 0x20;
    }
    return state;
}

static uint8_t gamepad_stick(int pad)
{
    if (!IsGamepadAvailable(pad)) return 0;
    uint8_t state = 0;
    float x = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X);
    float y = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y);
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_UP) || y < -0.35f)
        state |= 0x01;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN) || y > 0.35f)
        state |= 0x02;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT) || x < -0.35f)
        state |= 0x04;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || x > 0.35f)
        state |= 0x08;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_MIDDLE_RIGHT) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
        state |= 0x10;
    if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_UP) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_TRIGGER_1) ||
        IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
        state |= 0x20;
    static uint8_t previous[4];
    if (pad < 4 && state != previous[pad]) {
#ifdef PLATFORM_ANDROID
        TraceLog(LOG_WARNING, "ANDROID: controller %d state=$%02x axes=(%.2f,%.2f)",
                 pad, state, x, y);
#else
        fprintf(stderr, "controller %d state=$%02x axes=(%.2f,%.2f)\n",
                pad, state, x, y);
#endif
        previous[pad] = state;
    }
    return state;
}

static void update_input(void)
{
    joy_state[1] = keyboard_stick(true) | gamepad_stick(0);
    joy_state[0] = keyboard_stick(false) | gamepad_stick(1);
    map_raw_key(KEY_SPACE, RAW_SPACE);
    map_raw_key(KEY_ENTER, RAW_RETURN);
    for (int number = 0; number < 10; number++)
        map_raw_key(KEY_F1 + number, (uint8_t)(RAW_F1 + number));
}

static Rectangle fit_screen(void)
{
    float scale_x = GetScreenWidth() / (float)SCREEN_W;
    float scale_y = GetScreenHeight() / (float)SCREEN_H;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    float width = SCREEN_W * scale;
    float height = SCREEN_H * scale;
    return (Rectangle){(GetScreenWidth() - width) * 0.5f,
                       (GetScreenHeight() - height) * 0.5f,
                       width, height};
}

static void draw_splash(Texture2D logo)
{
    float scale_x = GetScreenWidth() * 0.84f / logo.width;
    float scale_y = GetScreenHeight() * 0.78f / logo.height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    float width = logo.width * scale;
    float height = logo.height * scale;
    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(logo,
                   (Rectangle){0, 0, (float)logo.width, (float)logo.height},
                   (Rectangle){(GetScreenWidth() - width) * 0.5f,
                               (GetScreenHeight() - height) * 0.5f,
                               width, height},
                   (Vector2){0, 0}, 0, WHITE);
    EndDrawing();
}

int main(int argc, char **argv)
{
#ifdef PLATFORM_ANDROID
    const char *data = "data";
#else
    const char *data = "original/whdload/BattleSquadron/data";
#endif
    if (argc == 3 && !strcmp(argv[1], "--data")) data = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--data DIR]\n", argv[0]);
        return 2;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_W * 3, SCREEN_H * 3,
               "Battle Squadron - Amiga native runner");
    SetExitKey(KEY_ESCAPE);
    SetTargetFPS(50);
#ifdef PLATFORM_ANDROID
    request_pal_frame_rate();
#endif
    for (int pad = 0; pad < 4; pad++) {
        if (IsGamepadAvailable(pad)) {
#ifdef PLATFORM_ANDROID
            TraceLog(LOG_WARNING, "ANDROID: controller %d: %s", pad,
                     GetGamepadName(pad));
#else
            fprintf(stderr, "controller %d: %s\n", pad,
                    GetGamepadName(pad));
#endif
        }
    }

#ifdef PLATFORM_ANDROID
    const char *logo_path = "retro-recomp.png";
#else
    const char *logo_path = "assets/retro-recomp.png";
#endif
    Texture2D splash = LoadTexture(logo_path);

    amiga_init(data);
    /* The original loader spends several seconds in display-independent
     * delay loops. Run those at host speed, then turn on exact scanout. */
    int16_t audio_buffer[1024 * 2];
    /* Only accelerate the silent loader.  Paula becomes audible near frame
     * 168; stopping at 140 preserves the complete spoken "Welcome to Battle
     * Squadron" sample as well as the title music. */
    while (bs_frame_no < 140 && !amiga_stopped()) {
        amiga_run_frame();
        /* Advance Paula with the emulated clock during the fast boot, but
         * discard those samples rather than playing several seconds late. */
        amiga_audio_frame();
        amiga_audio_pull(audio_buffer, 882);
    }
    double splash_until = GetTime() + 1.5;
    while (GetTime() < splash_until && !WindowShouldClose())
        draw_splash(splash);
    UnloadTexture(splash);
    if (WindowShouldClose()) {
        CloseWindow();
        return 0;
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

    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(256);
    AudioStream stream = LoadAudioStream(44100, 16, 2);
    SetAudioStreamCallback(stream, audio_callback);
    bool audio_started = false;

#ifdef PLATFORM_ANDROID
    /* Let EGL present at the device-selected rate (ideally the requested
     * 50 Hz).  The emulation clock below remains exactly PAL-paced even when
     * the panel can only scan at 60 Hz. */
    SetTargetFPS(120);
    double next_emulated_frame = GetTime();
    double next_diagnostic = next_emulated_frame + 5.0;
    long rendered_frames = 0;
#endif

    while (!WindowShouldClose() && !amiga_stopped()) {
        update_input();
#ifdef PLATFORM_ANDROID
        double now = GetTime();
        if (now - next_emulated_frame > 0.080)
            next_emulated_frame = now;
        int frames_run = 0;
        while (now + 0.0005 >= next_emulated_frame && frames_run < 4) {
            amiga_run_frame();
            amiga_audio_frame();
            next_emulated_frame += 1.0 / 50.0;
            frames_run++;
        }
#else
        amiga_run_frame();
        amiga_audio_frame();
#endif
        if (!audio_started && amiga_audio_fill() >= 1764) {
            /* Two PAL frames cover normal device callback jitter without the
             * long push queue that sounds echoey on Android/OpenSL. */
            PlayAudioStream(stream);
            audio_started = true;
        }
#ifdef PLATFORM_ANDROID
        if (frames_run)
#endif
        UpdateTexture(texture, framebuf);
        BeginDrawing();
        ClearBackground(BLACK);
        Rectangle destination = fit_screen();
        DrawTexturePro(texture,
                       (Rectangle){0, 0, SCREEN_W, SCREEN_H},
                       destination, (Vector2){0, 0}, 0, WHITE);
        EndDrawing();
#ifdef PLATFORM_ANDROID
        rendered_frames++;
        now = GetTime();
        if (now >= next_diagnostic) {
            TraceLog(LOG_WARNING,
                     "ANDROID: clock emulated=%ld rendered=%ld audio_fill=%d",
                     bs_frame_no, rendered_frames, amiga_audio_fill());
            next_diagnostic = now + 5.0;
        }
#endif
    }

    if (audio_started) StopAudioStream(stream);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    UnloadTexture(texture);
    CloseWindow();
    amiga_report();
    return amiga_stopped() ? 1 : 0;
}
