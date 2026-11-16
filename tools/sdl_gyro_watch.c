/* Gyro sanity check outside the game: waits (up to 180 s) for Cross/A on the first SDL gamepad,
 * then records gyro + accel for 10 s while the user moves the controller and prints min/max per
 * axis and the largest angular speed seen. A still controller shows ~0.01 rad/s; any real
 * movement shows > 1 rad/s.
 * Build: gcc tools/sdl_gyro_watch.c $(pkg-config --cflags --libs sdl3) -lm -o port/out/tools/sdl-gyro-watch */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { printf("init: %s\n", SDL_GetError()); return 1; }
    SDL_Delay(500);
    SDL_UpdateGamepads();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (n < 1) { printf("no gamepad\n"); return 1; }
    SDL_Gamepad *g = SDL_OpenGamepad(ids[0]);
    printf("pad: %s (%s) gyro=%d accel=%d\n", SDL_GetGamepadName(g), SDL_GetGamepadPath(g),
           SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO), SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL));
    if (!SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true) ||
        !SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true)) {
        printf("enable sensors failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("waiting for Cross/A ...\n");
    fflush(stdout);
    Uint64 deadline = SDL_GetTicks() + 180000;
    while (!SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) {
        if (SDL_GetTicks() > deadline) { printf("timed out waiting for Cross\n"); return 2; }
        SDL_Delay(10);
        SDL_UpdateGamepads();
    }
    printf("recording 10 s: move the controller now\n");
    fflush(stdout);
    float gmin[3] = {1e9f, 1e9f, 1e9f}, gmax[3] = {-1e9f, -1e9f, -1e9f};
    float amin[3] = {1e9f, 1e9f, 1e9f}, amax[3] = {-1e9f, -1e9f, -1e9f};
    float peak = 0;
    int samples = 0, buttons = 0;
    Uint64 end = SDL_GetTicks() + 10000;
    while (SDL_GetTicks() < end) {
        SDL_Delay(5);
        SDL_UpdateGamepads();
        float gy[3], ac[3];
        if (!SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, gy, 3)) continue;
        SDL_GetGamepadSensorData(g, SDL_SENSOR_ACCEL, ac, 3);
        for (int i = 0; i < 3; ++i) {
            if (gy[i] < gmin[i]) gmin[i] = gy[i];
            if (gy[i] > gmax[i]) gmax[i] = gy[i];
            if (ac[i] < amin[i]) amin[i] = ac[i];
            if (ac[i] > amax[i]) amax[i] = ac[i];
        }
        float s = sqrtf(gy[0] * gy[0] + gy[1] * gy[1] + gy[2] * gy[2]);
        if (s > peak) peak = s;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) ++buttons;
        ++samples;
    }
    printf("samples %d (cross held in %d)\n", samples, buttons);
    for (int i = 0; i < 3; ++i)
        printf("axis %d: gyro %+.3f .. %+.3f rad/s   accel %+.2f .. %+.2f m/s2\n", i, gmin[i], gmax[i],
               amin[i], amax[i]);
    printf("peak angular speed %.3f rad/s -> %s\n", peak,
           peak > 1.0f ? "MOVEMENT SEEN (SDL gyro works)" : "NO MOVEMENT SEEN");
    return 0;
}
