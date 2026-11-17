// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Keyboard and mouse for Dreams (src/runtime_pad.c and parts; docs/dreams/INPUT.md):
 *   - the Dreams profile's keys (Space = Cross, Backspace = Circle, Left Shift = L3, not a Circle
 *     hold) and mouse buttons (Left = R1, Right = R2) while the window holds the mouse;
 *   - mouse motion turns the reported orientation (imp) and leaves the right stick alone; the
 *     rotation is composed with the sensors' and reported as angular velocity; ResetOrientation
 *     clears it; DREAMS_MOUSE_MODE=camera gives it to the right stick instead (separate process);
 *   - while the game's text box takes the typing (runtime_ime_typing) keys and mouse buttons don't
 *     reach the pad, nor after it closes while a key is still held (mouse motion still moves the imp);
 *   - the synthetic motion diagnostic's pose (DREAMS_PAD_SYNTH_MOTION).
 * Keyboard/mouse state is faked by redirecting SDL_GetKeyboardState/SDL_GetMouseState (the source
 * is included). Built by tests/run-dreams-tests.sh. */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <SDL3/SDL.h>
static bool fake_keys[SDL_SCANCODE_COUNT];
static SDL_MouseButtonFlags fake_buttons;
static const bool *fake_keyboard_state(int *n) { if (n) *n=SDL_SCANCODE_COUNT; return fake_keys; }
static SDL_MouseButtonFlags fake_mouse_state(float *x, float *y) { if (x) *x=0; if (y) *y=0; return fake_buttons; }
static Uint32 fake_was_init(Uint32 flags) { return flags; } /* "video is up": the keyboard counts */
#define SDL_GetKeyboardState fake_keyboard_state
#define SDL_GetMouseState fake_mouse_state
#define SDL_WasInit fake_was_init
#include "../src/runtime_pad.c"
#undef SDL_WasInit

static double fake_dx, fake_dy;
static int fake_captured=1, typing;
int bbgpu_overlay_captures_input(void) { return 0; }
void bbgpu_mouse_take(double *dx, double *dy, int *up, int *down) { *dx=fake_dx; *dy=fake_dy; fake_dx=fake_dy=0; *up=*down=0; }
int bbgpu_mouse_captured(void) { return fake_captured; }
void bbgpu_mouse_look_enable(int enabled) { (void)enabled; }
void bbgpu_mouse_capture(void) {}
int bbgpu_camera_look(float yaw, float pitch) { (void)yaw; (void)pitch; return 0; }
void bbgpu_filter_pad(uint32_t *buttons, const uint8_t *sticks) { (void)buttons; (void)sticks; }
int bbgpu_pointer_wanted(void) { return 0; }
int runtime_memory_low_info(uintptr_t address, int *prot, uintptr_t *end) { (void)address; (void)prot; (void)end; return 0; }
int bbgpu_video_active(void) { return 0; }
int bbgpu_world_active(void) { return 1; }
unsigned long long bbgpu_world_us(void) { return 0; }
int bbgpu_video_skip(void) { return 0; }
int runtime_avplayer_active(void) { return 0; }
int runtime_avplayer_skip(void) { return 0; }
int bbgpu_free_camera(void) { return 0; }
int runtime_ime_typing(void) { return typing; }
void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads) { (void)pad_buffer; (void)reads; }
uint64_t runtime_monotonic_to_process_us(uint64_t monotonic_us) { return monotonic_us; }
uintptr_t runtime_libc_base;
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#define NEAR(a,b,eps) (fabsf((a)-(b))<(eps))

static PadData read_pad(void) {
    PadData d;
    usleep(2000);
    assert(pad_read(PAD_HANDLE,&d,1)==1);
    return d;
}
/* Yaw and pitch of a reported orientation (q = yaw about y, then pitch about x). */
static void yaw_pitch(const PadData *d, float *yaw, float *pitch) {
    const float *q=d->orientation; /* forward of the controller (-z) turned by q */
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    const float fx=-(2.0f*(x*z+w*y)), fy=-(2.0f*(y*z-w*x)), fz=-(1.0f-2.0f*(x*x+y*y));
    *yaw=atan2f(-fx,-fz); *pitch=asinf(fy);
}

static void camera_mode_child(void) {
    setenv("DREAMS_MOUSE_MODE","camera",1);
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==PAD_HANDLE);
    (void)read_pad();
    fake_dx=400; fake_dy=0;
    PadData d=read_pad();
    assert(d.right_x>200 && NEAR(d.orientation[3],1.0f,1e-6f));
    _exit(0);
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    setenv("DREAMS_PAD_SENSORS","0",1); /* no motion thread: only the mouse turns the controller */
    unsetenv("BB_CONFIG"); unsetenv("DREAMS_INPUT_PROFILE"); unsetenv("DREAMS_MOUSE_MODE");
    /* The synthetic pose: yaw then pitch, recovered. */
    PadData p={0};
    synth_pose(0.3f,-0.2f,p.orientation);
    float yaw, pitch;
    yaw_pitch(&p,&yaw,&pitch);
    assert(NEAR(yaw,0.3f,1e-4f) && NEAR(pitch,-0.2f,1e-4f));

    const pid_t child=fork();
    if (!child) camera_mode_child();
    int status=0;
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);

    assert(pad_init()==0 && pad_open(1,0,0,NULL)==PAD_HANDLE);
    PadData d=read_pad();
    assert(dreams_profile && mouse_to_imp);
    assert(d.buttons==0 && d.orientation[3]==1.0f);
    /* Keys: Space = Cross, Backspace = Circle, Left Shift = L3 (no Circle), W = left stick up. */
    fake_keys[SDL_SCANCODE_SPACE]=1; d=read_pad(); assert(d.buttons==BTN_CROSS); fake_keys[SDL_SCANCODE_SPACE]=0;
    fake_keys[SDL_SCANCODE_BACKSPACE]=1; d=read_pad(); assert(d.buttons==BTN_CIRCLE); fake_keys[SDL_SCANCODE_BACKSPACE]=0;
    fake_keys[SDL_SCANCODE_LSHIFT]=1; d=read_pad(); assert(d.buttons==BTN_L3); fake_keys[SDL_SCANCODE_LSHIFT]=0;
    fake_keys[SDL_SCANCODE_W]=1; for (int i=0;i<60;++i) d=read_pad(); assert(d.left_y<10); fake_keys[SDL_SCANCODE_W]=0;
    for (int i=0;i<60;++i) d=read_pad();
    assert(d.left_y==128);
    /* Mouse buttons while held: Left = R1, Right = R2 (and its analog value). */
    fake_buttons=SDL_BUTTON_LMASK; d=read_pad(); assert(d.buttons==BTN_R1);
    fake_buttons=SDL_BUTTON_RMASK; d=read_pad(); assert(d.buttons==BTN_R2 && d.r2==255);
    fake_buttons=0; fake_captured=0;
    fake_buttons=SDL_BUTTON_LMASK; d=read_pad(); assert(d.buttons==0); /* not held: not the game's */
    fake_buttons=0; fake_captured=1;
    /* Mouse motion: 100 px right = yaw right (negative about y) by 100 * sensitivity; 50 px up =
     * pitch up. The right stick stays. */
    const float k=mouse_imp_sens();
    fake_dx=100; d=read_pad();
    yaw_pitch(&d,&yaw,&pitch);
    printf("mouse 100 px right -> yaw %.4f rad (expected %.4f), angular velocity y %.2f rad/s\n",yaw,-100*k,d.angular_velocity[1]);
    assert(NEAR(yaw,-100*k,1e-4f) && NEAR(pitch,0,1e-4f) && d.angular_velocity[1]<-1.0f && d.right_x==128);
    fake_dy=-50; d=read_pad();
    yaw_pitch(&d,&yaw,&pitch);
    assert(NEAR(yaw,-100*k,1e-4f) && NEAR(pitch,50*k,1e-4f) && d.angular_velocity[0]>1.0f);
    d=read_pad(); /* no motion: rate back to 0, pose kept */
    assert(d.angular_velocity[0]==0.0f && d.angular_velocity[1]==0.0f);
    /* Pitch is limited to 80 degrees. */
    fake_dy=-100000; d=read_pad(); yaw_pitch(&d,&yaw,&pitch);
    assert(NEAR(pitch,MOUSE_PITCH_MAX,1e-3f));
    /* Composed with the sensors' orientation (here a 90 degree roll). */
    sensors.q[2]=sinf((float)M_PI_4); sensors.q[3]=cosf((float)M_PI_4);
    d=read_pad();
    float qm[4], expect[4];
    synth_pose(mouse_rot.yaw,mouse_rot.pitch,qm);
    quat_mul(sensors.q,qm,expect);
    for (int i=0;i<4;++i) assert(NEAR(d.orientation[i],expect[i],1e-6f));
    /* scePadResetOrientation clears both. */
    assert(pad_reset_orientation(PAD_HANDLE)==0);
    d=read_pad(); assert(d.orientation[3]==1.0f);

    /* The text box takes the keys; after it, held keys stay off until released. */
    typing=1; fake_keys[SDL_SCANCODE_W]=1; fake_keys[SDL_SCANCODE_RETURN]=1;
    for (int i=0;i<30;++i) { d=read_pad(); assert(d.buttons==0 && d.left_y==128); }
    typing=0; fake_keys[SDL_SCANCODE_W]=0;
    for (int i=0;i<5;++i) { d=read_pad(); assert(d.buttons==0); } /* Enter still held */
    fake_keys[SDL_SCANCODE_RETURN]=0; d=read_pad();
    fake_keys[SDL_SCANCODE_RETURN]=1; d=read_pad(); assert(d.buttons==BTN_CROSS);
    fake_keys[SDL_SCANCODE_RETURN]=0;
    /* Mouse buttons pause too (the bound buttons go through the same key table), mouse motion not. */
    typing=1; fake_buttons=SDL_BUTTON_LMASK; d=read_pad(); assert(d.buttons==0); fake_buttons=0;
    fake_dx=-100; d=read_pad(); yaw_pitch(&d,&yaw,&pitch); assert(NEAR(yaw,100*k,1e-4f)); typing=0;
    puts("pad-input-test passed");
    return 0;
}
