// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Pad motion (src/runtime_pad/sensors.c through src/runtime_pad.c): the integration math, tilt
 * correction and bias, then the whole path with an SDL virtual gamepad that has a gyro and an
 * accelerometer: scePadRead reports its angular velocity, acceleration in g and the integrated
 * orientation; scePadResetOrientation; a pad without sensors reads as lying still.
 * Built by tests/run-dreams-tests.sh (SDL3, dummy video driver). */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

int bbgpu_overlay_captures_input(void) { return 0; }
void bbgpu_mouse_take(double *dx, double *dy, int *up, int *down) { *dx=*dy=0; *up=*down=0; }
int bbgpu_mouse_captured(void) { return 0; }
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
void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads) { (void)pad_buffer; (void)reads; }
/* runtime_kernel.c's version subtracts the process start; here: the start is "now" at first call,
 * so the game-facing timestamp must come out small (seconds since start, not since boot). */
uint64_t runtime_monotonic_to_process_us(uint64_t monotonic_us) {
    static uint64_t start;
    if (!start) start=monotonic_us;
    return monotonic_us-start;
}
uintptr_t runtime_libc_base;
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

static float angle_of(const float q[4]) { return 2.0f*acosf(fminf(1.0f,fabsf(q[3]))); }
#define NEAR(a,b,eps) (fabsf((a)-(b))<(eps))

static void math_tests(void) {
    /* 90 deg/s about y for one second, in 1 ms steps: a quarter turn about y. */
    float q[4]={0,0,0,1};
    const float w[3]={0,(float)M_PI_2,0}, flat[3]={0,1,0};
    for (int i=0;i<1000;++i) sensors_integrate(q,w,flat,0.001f,0);
    assert(NEAR(q[1],sinf((float)M_PI_4),1e-3f) && NEAR(q[3],cosf((float)M_PI_4),1e-3f) && NEAR(q[0],0,1e-4f));
    /* Gaps over a second are ignored (ref CalculateOrientation). */
    float before[4]; memcpy(before,q,sizeof(q));
    sensors_integrate(q,w,flat,2.0f,0);
    assert(!memcmp(before,q,sizeof(q)));
    /* Tilt correction: an orientation rolled 30 degrees while the accelerometer says "flat"
     * converges back to level within a few seconds; without it, it stays. */
    const float zero[3]={0,0,0};
    float r[4]={0,0,sinf(0.2618f),cosf(0.2618f)}, kept[4];
    memcpy(kept,r,sizeof(r));
    for (int i=0;i<10000;++i) sensors_integrate(r,zero,flat,0.001f,1); /* time constant 1/TILT_GAIN */
    float up[3]; up_in_frame(r,up);
    printf("tilt correction: roll 30 deg -> %.2f deg after 10 s (up %.3f %.3f %.3f)\n",angle_of(r)*57.2958f,up[0],up[1],up[2]);
    assert(angle_of(r)<0.02f && up[1]>0.999f);
    for (int i=0;i<1000;++i) sensors_integrate(kept,zero,flat,0.001f,0);
    assert(NEAR(angle_of(kept),0.5236f,1e-3f));
    /* Tilt correction leaves yaw alone (gravity says nothing about it). */
    float y[4]={0,sinf(0.5f),0,cosf(0.5f)};
    for (int i=0;i<3000;++i) sensors_integrate(y,zero,flat,0.001f,1);
    assert(NEAR(angle_of(y),1.0f,1e-3f));
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    math_tests();
    setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6190");
    assert(SDL_Init(SDL_INIT_GAMEPAD)); /* no video: the motion thread pumps (--cpu-only path) */
    PadData d;
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==PAD_HANDLE);
    /* No controller: lying still. */
    assert(pad_read(PAD_HANDLE,&d,1)==1);
    assert(d.orientation[3]==1.0f && d.acceleration[1]==1.0f && d.angular_velocity[0]==0.0f);

    SDL_VirtualJoystickSensorDesc sens[2]={{SDL_SENSOR_GYRO,500.0f},{SDL_SENSOR_ACCEL,500.0f}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1; desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="Dreams motion test pad"; desc.vendor_id=0x1d50; desc.product_id=0x6190;
    desc.nsensors=2; desc.sensors=sens;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id);
    SDL_Joystick *joy=SDL_OpenJoystick(id);
    assert(joy);
    /* Wait for the motion thread to take the pad and turn its sensors on. */
    for (int i=0;i<200 && !sensors.has_gyro;++i) usleep(5000);
    assert(sensors.has_gyro && sensors.has_accel);
    /* Flat and still, then turning about y at 1 rad/s for 0.5 s. */
    const float accel[3]={0.0f,SDL_STANDARD_GRAVITY,0.0f}, still[3]={0,0,0}, turn[3]={0.0f,1.0f,0.0f};
    for (int i=0;i<20;++i) {
        assert(SDL_SendJoystickVirtualSensorData(joy,SDL_SENSOR_ACCEL,SDL_GetTicksNS(),accel,3));
        assert(SDL_SendJoystickVirtualSensorData(joy,SDL_SENSOR_GYRO,SDL_GetTicksNS(),still,3));
        usleep(2000);
    }
    assert(pad_reset_orientation(PAD_HANDLE)==0);
    assert(pad_read(PAD_HANDLE,&d,1)==1);
    assert(NEAR(d.acceleration[1],1.0f,1e-4f) && NEAR(d.orientation[3],1.0f,1e-3f));
    const uint64_t t0=now_us();
    while (now_us()-t0<500000) {
        assert(SDL_SendJoystickVirtualSensorData(joy,SDL_SENSOR_GYRO,SDL_GetTicksNS(),turn,3));
        usleep(1000);
    }
    assert(pad_read(PAD_HANDLE,&d,1)==1);
    assert(NEAR(d.angular_velocity[1],1.0f,1e-4f));
    assert(SDL_SendJoystickVirtualSensorData(joy,SDL_SENSOR_GYRO,SDL_GetTicksNS(),still,3));
    usleep(20000);
    assert(pad_read(PAD_HANDLE,&d,1)==1);
    const float yaw=2.0f*atan2f(d.orientation[1],d.orientation[3]);
    printf("pad: turned 1 rad/s for 0.5 s -> yaw %.3f rad (q %.3f %.3f %.3f %.3f), %lu samples\n",yaw,
           d.orientation[0],d.orientation[1],d.orientation[2],d.orientation[3],sensors.samples);
    assert(yaw>0.40f && yaw<0.60f && NEAR(d.orientation[0],0,0.01f) && NEAR(d.orientation[2],0,0.01f));
    assert(d.angular_velocity[1]==0.0f);
    /* The game's settings. */
    assert(pad_tilt_correction(PAD_HANDLE,1)==0 && sensors.tilt);
    assert(pad_bias_correction(PAD_HANDLE,1)==0 && sensors.bias_correction);
    assert(pad_deadband(PAD_HANDLE,1)==0 && sensors.deadband);
    assert(pad_tilt_correction(7,1)==ERR_INVALID_HANDLE);
    const uint8_t rgb[4]={255,0,64,0};
    assert(pad_light_bar(PAD_HANDLE,rgb)==0 && pad_light_bar(PAD_HANDLE,NULL)==ERR_INVALID_ARG);
    assert(runtime_pad_resolve("scePadSetAngularVelocityBiasCorrectionState") && runtime_pad_resolve("scePadSetLightBar"));
    runtime_pad_report();
    puts("pad-motion-test passed");
    return 0;
}
