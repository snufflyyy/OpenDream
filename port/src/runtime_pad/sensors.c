// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* runtime_pad.c part (Dreams port): the controller's real motion sensors.
 * #included by runtime_pad.c after motion.c (one translation unit: it shares that file's statics,
 * quat_mul and up_in_frame); not compiled on its own.
 *
 * Dreams' default control scheme is "Motion": the imp (the editor's cursor) follows the
 * DualShock 4 / DualSense orientation. scePadRead/ReadState report, as the PS4 does:
 *   angular_velocity  rad/s in the controller's frame (x right, y up, z towards the player),
 *   acceleration      in g, same frame (flat on a table: (0, 1, 0)),
 *   orientation       quaternion (x, y, z, w) from integrating the angular velocity since the pad
 *                     was opened or scePadResetOrientation, controller frame -> world.
 * SDL3 reports gamepad sensors in the same axes (SDL_sensor.h: gyro rad/s, accel m/s^2), so only
 * the accelerometer is scaled (1 / SDL_STANDARD_GRAVITY). The integration is shadPS4's
 * (ref/shadps4/src/input/controller.cpp CalculateOrientation: q += 0.5 q (x) omega dt,
 * normalised); shadPS4 passes SDL's values straight through and scales accel by 0.098.
 *
 * Sampling: SDL keeps only the latest sensor report and Dreams reads the pad once a frame, so a
 * runtime thread ("PadMotion") samples the sensors every 2 ms and integrates there; pad reads copy
 * the result. The window thread pumps SDL events (vcore/shim/window.cpp); without a window
 * (--cpu-only) this thread calls SDL_UpdateGamepads itself.
 *
 * The game's motion settings:
 *   scePadSetTiltCorrectionState(true)            orientation pulled towards the measured gravity
 *                                                 (complementary filter, Mahony-style proportional
 *                                                 term, gain TILT_GAIN): pitch/roll stop drifting;
 *   scePadSetAngularVelocityBiasCorrectionState   gyro bias learned while the pad lies still and
 *                                                 subtracted;
 *   scePadSetAngularVelocityDeadbandState         |omega| below DEADBAND reported as 0.
 * PS4 defaults (all off until the game asks) are assumed.
 *
 * Without a gyro (keyboard and mouse, Xbox-style pads) the controller is reported lying still
 * (identity orientation, 1 g up) apart from the mouse.
 *
 * The mouse moves the imp (docs/dreams/INPUT.md, notes/input-research.md): Dreams moves the imp by
 * the *change* of the orientation between its updates (eboot 0x771ac0..0x772342), so mouse motion
 * (while the window holds the mouse) is turned into a rotation of the controller: x into yaw about
 * its y axis, y into pitch about its x axis (shadPS4's mouse gyro, input/input_mouse.cpp
 * EmulateGyro, has the same signs). It is kept apart from the sensors' orientation and composed
 * after it (reported = sensors (x) mouse), so a DualShock 4 and the mouse work together and tilt
 * correction never pulls the mouse's pitch back; its rate is added to the angular velocity (Dreams
 * counts that as the pad being used). DREAMS_MOUSE_IMP_SENS (radians per pixel, default
 * MOUSE_IMP_SENS) scales it; DREAMS_MOUSE_MODE=camera gives the mouse to bbport's camera look (the
 * right stick) instead. bbport's spray-can emulation (motion.c, BB_PAD_MOTION=1) still overrides all
 * of this when on. DREAMS_PAD_SENSORS=0 ignores the controller's sensors. */
#define TILT_GAIN 0.6f          /* rad/s per unit of gravity direction error */
#define DEADBAND 0.0087f        /* 0.5 degree per second */
#define MOUSE_IMP_SENS 0.0004f  /* radians per pixel: about 1:1 at 1080p with Imp Sensitivity 6 (agent run 2026-11-15: 10 degrees of yaw moved the imp ~480 px) */
#define MOUSE_PITCH_MAX 1.4f    /* radians (80 degrees) */

static struct {
    SDL_JoystickID pad;        /* the gamepad whose sensors are on (0: none); an id, not the
                                * pointer: a reconnected pad may get the old pointer back */
    int has_gyro, has_accel, announced;
    float gyro[3], accel[3];   /* PS4 units, after bias and deadband */
    float raw_gyro[3];
    float q[4];                /* x y z w */
    float bias[3];
    uint64_t still_since, last_us;
    int tilt, bias_correction, deadband;
    int thread_started;
    unsigned long samples;
} sensors={.q={0,0,0,1}, .accel={0,1,0}};

static void sensors_reset_orientation(void) {
    sensors.q[0]=sensors.q[1]=sensors.q[2]=0.0f; sensors.q[3]=1.0f;
}
static void synth_pose(float yaw, float pitch, float q[4]);

/* One integration step: body-frame angular velocity `w` (rad/s) for `dt` seconds, then (tilt
 * correction) a proportional pull of the predicted gravity direction towards the measured one. */
static void sensors_integrate(float q[4], const float w_in[3], const float accel[3], float dt, int tilt) {
    if (dt<=0.0f || dt>1.0f) return; /* ref CalculateOrientation: a gap over a second is skipped */
    float w[3]={w_in[0],w_in[1],w_in[2]};
    if (tilt) {
        const float n=sqrtf(accel[0]*accel[0]+accel[1]*accel[1]+accel[2]*accel[2]);
        if (n>0.5f && n<1.5f) { /* near 1 g: the accelerometer measures gravity, not a shake */
            const float a[3]={accel[0]/n,accel[1]/n,accel[2]/n};
            float v[3];
            up_in_frame(q,v); /* where the orientation says "up" is, in the controller's frame */
            /* e = a x v: rotating by +e moves the estimate's up towards the measured one */
            const float e[3]={a[1]*v[2]-a[2]*v[1],a[2]*v[0]-a[0]*v[2],a[0]*v[1]-a[1]*v[0]};
            for (int i=0;i<3;++i) w[i]+=TILT_GAIN*e[i];
        }
    }
    const float omega[4]={w[0],w[1],w[2],0.0f};
    float dq[4];
    quat_mul(q,omega,dq);
    for (int i=0;i<4;++i) q[i]+=0.5f*dq[i]*dt;
    const float norm=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (norm>0.0f) for (int i=0;i<4;++i) q[i]/=norm;
    else { q[0]=q[1]=q[2]=0.0f; q[3]=1.0f; }
}

/* A sample of the sensors (raw SDL units in), PS4 units kept; called under lock. */
static void sensors_feed(const float gyro_rad[3], const float accel_ms2[3], uint64_t now) {
    const float g=SDL_STANDARD_GRAVITY;
    for (int i=0;i<3;++i) { sensors.raw_gyro[i]=gyro_rad[i]; sensors.accel[i]=sensors.has_accel ? accel_ms2[i]/g : (i==1); }
    /* Bias: learned while the pad lies still (small rotation, 1 g) for half a second. */
    const float rate=sqrtf(gyro_rad[0]*gyro_rad[0]+gyro_rad[1]*gyro_rad[1]+gyro_rad[2]*gyro_rad[2]);
    const float an=sqrtf(sensors.accel[0]*sensors.accel[0]+sensors.accel[1]*sensors.accel[1]+sensors.accel[2]*sensors.accel[2]);
    if (rate<0.1f && fabsf(an-1.0f)<0.05f) {
        if (!sensors.still_since) sensors.still_since=now;
        if (now-sensors.still_since>500000) for (int i=0;i<3;++i) sensors.bias[i]+=0.01f*(gyro_rad[i]-sensors.bias[i]);
    } else sensors.still_since=0;
    for (int i=0;i<3;++i) {
        float w=gyro_rad[i]-(sensors.bias_correction ? sensors.bias[i] : 0.0f);
        if (sensors.deadband && fabsf(w)<DEADBAND) w=0.0f;
        sensors.gyro[i]=w;
    }
    const float dt=sensors.last_us && now>sensors.last_us ? (float)(now-sensors.last_us)*1e-6f : 0.0f;
    sensors.last_us=now;
    sensors_integrate(sensors.q,sensors.gyro,sensors.accel,dt,sensors.tilt);
    ++sensors.samples;
    /* DREAMS_PAD_SENSOR_TRACE=1: the first 20 samples, then one every 2 s (raw SDL units in,
     * what the game reads out). */
    static int trace=-1; static uint64_t last_trace;
    if (trace<0) { const char *e=bb_env("DREAMS_PAD_SENSOR_TRACE"); trace=e && e[0]=='1'; }
    if (trace && (sensors.samples<=20 || now-last_trace>=2000000)) {
        last_trace=now;
        printf("Pad sensors %lu: gyro %+.3f %+.3f %+.3f rad/s, accel %+.2f %+.2f %+.2f m/s2 -> "
               "angular velocity %+.3f %+.3f %+.3f, accel %+.3f %+.3f %+.3f g, orientation %+.3f %+.3f %+.3f %+.3f "
               "(tilt %d bias %d deadband %d)\n",sensors.samples,gyro_rad[0],gyro_rad[1],gyro_rad[2],accel_ms2[0],accel_ms2[1],
               accel_ms2[2],sensors.gyro[0],sensors.gyro[1],sensors.gyro[2],sensors.accel[0],sensors.accel[1],sensors.accel[2],
               sensors.q[0],sensors.q[1],sensors.q[2],sensors.q[3],sensors.tilt,sensors.bias_correction,sensors.deadband);
    }
}

static int sensors_wanted(void) {
    static int on=-1;
    if (on<0) { const char *e=bb_env("DREAMS_PAD_SENSORS"); on=!(e && e[0]=='0'); }
    return on;
}
/* Turns the sensors on for a newly chosen gamepad; called under lock. */
static void sensors_attach(SDL_Gamepad *g) {
    const SDL_JoystickID id=g ? SDL_GetGamepadID(g) : 0;
    if (id==sensors.pad) return;
    sensors.pad=id;
    sensors.has_gyro=sensors.has_accel=0;
    sensors.last_us=0; sensors.still_since=0;
    memset(sensors.bias,0,sizeof(sensors.bias));
    sensors_reset_orientation();
    if (!g || !sensors_wanted()) return;
    sensors.has_gyro=SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_GYRO,true);
    sensors.has_accel=SDL_GamepadHasSensor(g,SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_ACCEL,true);
    const char *path=SDL_GetGamepadPath(g);
    printf("Runtime: pad motion: using \"%s\" (%s, SDL id %u): gyro %s (%.0f Hz), accelerometer %s%s\n",SDL_GetGamepadName(g),
           path ? path : "no path",(unsigned)id,
           sensors.has_gyro ? "enabled" : SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO) ? "FAILED to enable" : "none",
           sensors.has_gyro ? SDL_GetGamepadSensorDataRate(g,SDL_SENSOR_GYRO) : 0.0f,
           sensors.has_accel ? "enabled" : SDL_GamepadHasSensor(g,SDL_SENSOR_ACCEL) ? "FAILED to enable" : "none",
           sensors.has_gyro ? " (DREAMS_PAD_SENSOR_TRACE=1 prints samples)" : "");
    if (!sensors.has_gyro && SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO)) printf("Runtime: pad motion: %s\n",SDL_GetError());
    if (!sensors.has_gyro) printf("Runtime: pad motion: no gyro: the controller is reported lying still "
                                  "(the mouse moves the imp; or choose Dreams' Left & Right Sticks scheme)\n");
}
/* Under lock: the current gamepad's sensors read and integrated. */
static void sensors_poll(uint64_t now) {
    SDL_Gamepad *g=current_gamepad();
    sensors_attach(g);
    if (!g || !sensors.has_gyro) return;
    float gyro[3]={0,0,0}, accel[3]={0,SDL_STANDARD_GRAVITY,0};
    if (!SDL_GetGamepadSensorData(g,SDL_SENSOR_GYRO,gyro,3)) return;
    if (sensors.has_accel) SDL_GetGamepadSensorData(g,SDL_SENSOR_ACCEL,accel,3);
    sensors_feed(gyro,accel,now);
}
/* DREAMS_PAD_SYNTH_MOTION=<amplitude deg>[,<period s>[,<delay s>]] (diagnostics, agent runs without
 * a controller; notes/input-research.md): the controller's motion is replaced by a synthetic
 * figure eight, yaw = A sin(2 pi t/T) and pitch = A/2 sin(4 pi t/T), starting <delay> s after the pad
 * opened. The orientation is set in closed form (no drift), the angular velocity is its derivative
 * in the controller's frame and the accelerometer reads gravity in that frame. */
static void synth_pose(float yaw, float pitch, float q[4]) {
    const float qy[4]={0.0f,sinf(0.5f*yaw),0.0f,cosf(0.5f*yaw)}, qp[4]={sinf(0.5f*pitch),0.0f,0.0f,cosf(0.5f*pitch)};
    quat_mul(qy,qp,q); /* yaw about the world's up, then pitch about the controller's right */
}
static int synth_motion(uint64_t now) {
    static int on=-1; static float amp, period=4.0f, delay=0.0f; static uint64_t t0;
    if (on<0) {
        const char *e=bb_env("DREAMS_PAD_SYNTH_MOTION");
        on=e && *e && atof(e)>0.0;
        if (on) {
            amp=(float)atof(e)*3.14159265f/180.0f;
            const char *c=strchr(e,',');
            if (c) { period=(float)atof(c+1); c=strchr(c+1,','); if (c) delay=(float)atof(c+1); }
            if (period<=0.1f) period=4.0f;
            t0=now;
            printf("Runtime: pad motion: SYNTHETIC figure eight, %.1f deg, period %.1f s, from %.1f s (DREAMS_PAD_SYNTH_MOTION)\n",
                   (double)(amp*180.0f/3.14159265f),(double)period,(double)delay);
        }
    }
    if (!on) return 0;
    float t=(float)(now-t0)*1e-6f-delay;
    if (t<0.0f) t=0.0f;
    const float w=2.0f*3.14159265f/period;
    const float yaw=amp*sinf(w*t), pitch=0.5f*amp*sinf(2.0f*w*t);
    const float dyaw=amp*w*cosf(w*t), dpitch=amp*w*cosf(2.0f*w*t);
    synth_pose(yaw,pitch,sensors.q);
    /* Body rates of q = yaw(world y) * pitch(body x): pitch rate on x, the yaw rate seen in the
     * pitched frame on y and z. */
    sensors.gyro[0]=dpitch; sensors.gyro[1]=dyaw*cosf(pitch); sensors.gyro[2]=-dyaw*sinf(pitch);
    up_in_frame(sensors.q,sensors.accel);
    sensors.has_gyro=sensors.has_accel=1;
    ++sensors.samples;
    return 1;
}
static void *sensors_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(),"PadMotion");
    for (;;) {
        pthread_mutex_lock(&lock);
        if (sdl_ready>=0 && !SDL_WasInit(SDL_INIT_VIDEO) && SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_UpdateGamepads(); /* no window pumps events */
        if (!synth_motion(now_us())) sensors_poll(now_us());
        const int active=sensors.has_gyro;
        pthread_mutex_unlock(&lock);
        usleep(active ? 2000 : 20000);
    }
    return NULL;
}
static void sensors_start(void) {
    if (sensors.thread_started || !sensors_wanted()) return;
    pthread_t t;
    if (!pthread_create(&t,NULL,sensors_thread,NULL)) { pthread_detach(t); sensors.thread_started=1; }
}

/* The mouse's rotation of the controller (see the top of the file); under lock. */
static struct { float yaw, pitch, rate[3]; } mouse_rot;
static void sensors_mouse_reset(void) { memset(&mouse_rot,0,sizeof mouse_rot); }
static float mouse_imp_sens(void) {
    static float k=-1.0f;
    if (k<0.0f) {
        const char *e=bb_env("DREAMS_MOUSE_IMP_SENS");
        k=e && *e && atof(e)>0.0 ? (float)atof(e) : MOUSE_IMP_SENS;
    }
    return k;
}
/* Called from sample_host under lock with the mouse motion since the last read; takes it (dx, dy
 * zeroed) when the mouse moves the imp. dt: seconds since the last read. */
static void sensors_mouse(double *dx, double *dy, int captured, float dt, int to_imp, int invert_y) {
    mouse_rot.rate[0]=mouse_rot.rate[1]=mouse_rot.rate[2]=0.0f;
    if (!to_imp || !captured || (*dx==0 && *dy==0)) return;
    const float k=mouse_imp_sens();
    const float dyaw=-(float)*dx*k, dpitch=(invert_y ? 1.0f : -1.0f)*(float)*dy*k;
    mouse_rot.yaw+=dyaw;
    if (mouse_rot.yaw>(float)M_PI) mouse_rot.yaw-=2.0f*(float)M_PI;
    if (mouse_rot.yaw<-(float)M_PI) mouse_rot.yaw+=2.0f*(float)M_PI;
    const float pitch=fmaxf(-MOUSE_PITCH_MAX,fminf(MOUSE_PITCH_MAX,mouse_rot.pitch+dpitch));
    /* Rate over at least 1/120 s (Dreams reads the pad several times a frame) and at most 20 rad/s
     * (Dreams' activity measure only needs it to be clearly non-zero). */
    const float span=fmaxf(dt,1.0f/120.0f);
    mouse_rot.rate[0]=fmaxf(-20.0f,fminf(20.0f,(pitch-mouse_rot.pitch)/span));
    mouse_rot.rate[1]=fmaxf(-20.0f,fminf(20.0f,dyaw/span));
    mouse_rot.pitch=pitch;
    *dx=*dy=0;
}

/* The state the game reads: the sensors' orientation, then the mouse's rotation. */
static void sensors_report(PadData *d) {
    float qm[4], q[4];
    synth_pose(mouse_rot.yaw,mouse_rot.pitch,qm);
    quat_mul(sensors.q,qm,q);
    /* The mouse's yaw rate seen in its pitched frame (as synth_motion's). */
    const float w_mouse[3]={mouse_rot.rate[0],mouse_rot.rate[1]*cosf(mouse_rot.pitch),-mouse_rot.rate[1]*sinf(mouse_rot.pitch)};
    for (int i=0;i<4;++i) d->orientation[i]=q[i];
    for (int i=0;i<3;++i) { d->acceleration[i]=sensors.accel[i]; d->angular_velocity[i]=sensors.gyro[i]+w_mouse[i]; }
}
