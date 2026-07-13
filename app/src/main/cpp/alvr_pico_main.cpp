#include "alvr_client_core.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <android/log.h>
#include <jni.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <time.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#define LOG_TAG "PicoALVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

#define PI_F 3.14159265358979f
#define IPD_HALF 0.030015f
// Pico Neo 2: 101° diagonal FOV. Using square eye buffer (1664×1664, 1:1 aspect).
// Equal h/v half-angles for square buffer. Increased from theoretical 40.6° to 45°
// to reduce "zoomed in" effect and better fill the headset's actual optical FOV.
// Pico Neo 2: 101° diagonal FOV. Using square eye buffer (1664×1664, 1:1 aspect).
// Equal h/v half-angles for square buffer. Increased from theoretical 40.6° to 45°
// to reduce "zoomed in" effect and better fill the headset's actual optical FOV.
// These are mutable globals so they can be set from Java settings UI at runtime.
static float g_fovVHalfDeg = 55.0f;
static float g_fovHHalfDeg = 55.0f;
// PicoVR SDK tracking origin is at the initial head position (Y≈0).
// SteamVR/ALVR expects floor-relative coordinates. Add standing height offset.
static float g_standingHeight = 1.5f;

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#define EGL_NATIVE_BUFFER_ANDROID 0x3140

// ===== EGL/GL extensions =====
typedef void* EGLImageKHR;
typedef EGLClientBuffer (*PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)(const void*);
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
typedef void (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, EGLImageKHR);

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBuffer = nullptr;
static PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES = nullptr;

static void loadEglExtensions() {
    g_eglGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");
    g_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    g_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    g_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    LOGI("EGL ext: nativeBuf=%p createImg=%p destroyImg=%p imgTarget=%p",
         g_eglGetNativeClientBuffer, g_eglCreateImageKHR,
         g_eglDestroyImageKHR, g_glEGLImageTargetTexture2DOES);
}

// ===== Path IDs =====
// Match ALVR common/src/inputs.rs: Pico Neo 3 profile (closest to Pico Neo 2 controller)
// Has system+menu+trigger_click+squeeze_click for both hands
static uint64_t HEAD_ID = 0, LEFT_HAND_ID = 0, RIGHT_HAND_ID = 0;
static uint64_t PICO_NEO3_PROFILE_ID = 0;
static uint64_t PATH_LEFT_SYSTEM=0, PATH_LEFT_MENU=0;
static uint64_t PATH_LEFT_TRIGGER_CLICK=0, PATH_LEFT_TRIGGER_VALUE=0;
static uint64_t PATH_LEFT_SQUEEZE_CLICK=0, PATH_LEFT_SQUEEZE_VALUE=0;
static uint64_t PATH_LEFT_X_CLICK=0, PATH_LEFT_Y_CLICK=0;
static uint64_t PATH_LEFT_THUMBSTICK_X=0, PATH_LEFT_THUMBSTICK_Y=0, PATH_LEFT_THUMBSTICK_CLICK=0, PATH_LEFT_THUMBSTICK_TOUCH=0;
static uint64_t PATH_RIGHT_SYSTEM=0, PATH_RIGHT_MENU=0;
static uint64_t PATH_RIGHT_TRIGGER_CLICK=0, PATH_RIGHT_TRIGGER_VALUE=0;
static uint64_t PATH_RIGHT_SQUEEZE_CLICK=0, PATH_RIGHT_SQUEEZE_VALUE=0;
static uint64_t PATH_RIGHT_A_CLICK=0, PATH_RIGHT_B_CLICK=0;
static uint64_t PATH_RIGHT_THUMBSTICK_X=0, PATH_RIGHT_THUMBSTICK_Y=0, PATH_RIGHT_THUMBSTICK_CLICK=0, PATH_RIGHT_THUMBSTICK_TOUCH=0;

static void initPathIds() {
    HEAD_ID = alvr_path_string_to_id("/user/head");
    LEFT_HAND_ID = alvr_path_string_to_id("/user/hand/left");
    RIGHT_HAND_ID = alvr_path_string_to_id("/user/hand/right");
    // ALVR: interaction_profile!(PICO_NEO3, "bytedance/pico_neo3")
    // → "/interaction_profiles/bytedance/pico_neo3_controller"
    PICO_NEO3_PROFILE_ID = alvr_path_string_to_id("/interaction_profiles/bytedance/pico_neo3_controller");
    #define PID(id, path) id = alvr_path_string_to_id(path)
    // Left: system, menu, trigger, squeeze, x, y, thumbstick
    PID(PATH_LEFT_SYSTEM, "/user/hand/left/input/system/click");
    PID(PATH_LEFT_MENU, "/user/hand/left/input/menu/click");
    PID(PATH_LEFT_TRIGGER_CLICK, "/user/hand/left/input/trigger/click");
    PID(PATH_LEFT_TRIGGER_VALUE, "/user/hand/left/input/trigger/value");
    PID(PATH_LEFT_SQUEEZE_CLICK, "/user/hand/left/input/squeeze/click");
    PID(PATH_LEFT_SQUEEZE_VALUE, "/user/hand/left/input/squeeze/value");
    PID(PATH_LEFT_X_CLICK, "/user/hand/left/input/x/click");
    PID(PATH_LEFT_Y_CLICK, "/user/hand/left/input/y/click");
    PID(PATH_LEFT_THUMBSTICK_X, "/user/hand/left/input/thumbstick/x");
    PID(PATH_LEFT_THUMBSTICK_Y, "/user/hand/left/input/thumbstick/y");
    PID(PATH_LEFT_THUMBSTICK_CLICK, "/user/hand/left/input/thumbstick/click");
    PID(PATH_LEFT_THUMBSTICK_TOUCH, "/user/hand/left/input/thumbstick/touch");
    // Right: system, menu, trigger, squeeze, a, b, thumbstick
    PID(PATH_RIGHT_SYSTEM, "/user/hand/right/input/system/click");
    PID(PATH_RIGHT_MENU, "/user/hand/right/input/menu/click");
    PID(PATH_RIGHT_TRIGGER_CLICK, "/user/hand/right/input/trigger/click");
    PID(PATH_RIGHT_TRIGGER_VALUE, "/user/hand/right/input/trigger/value");
    PID(PATH_RIGHT_SQUEEZE_CLICK, "/user/hand/right/input/squeeze/click");
    PID(PATH_RIGHT_SQUEEZE_VALUE, "/user/hand/right/input/squeeze/value");
    PID(PATH_RIGHT_A_CLICK, "/user/hand/right/input/a/click");
    PID(PATH_RIGHT_B_CLICK, "/user/hand/right/input/b/click");
    PID(PATH_RIGHT_THUMBSTICK_X, "/user/hand/right/input/thumbstick/x");
    PID(PATH_RIGHT_THUMBSTICK_Y, "/user/hand/right/input/thumbstick/y");
    PID(PATH_RIGHT_THUMBSTICK_CLICK, "/user/hand/right/input/thumbstick/click");
    PID(PATH_RIGHT_THUMBSTICK_TOUCH, "/user/hand/right/input/thumbstick/touch");
    #undef PID
}

// ===== Shared data between ALVR thread and render thread =====

struct TrackingData {
    std::mutex mtx;
    AlvrQuat hmdOri = {};
    float hmdPos[3] = {};
    bool leftConn = false;
    float leftOri[4] = {};
    float leftPos[3] = {};
    int leftTrigger = 0;
    int leftTouchpad[2] = {};
    bool leftButtons[8] = {};
    int leftBattery = 0;
    bool rightConn = false;
    float rightOri[4] = {};
    float rightPos[3] = {};
    int rightTrigger = 0;
    int rightTouchpad[2] = {};
    bool rightButtons[8] = {};
    int rightBattery = 0;
};

struct FrameData {
    std::mutex mtx;
    uint64_t timestampNs = 0;
    void* hardwareBuffer = nullptr;
    bool hasNewFrame = false;
    std::atomic<bool> consumed{true};
};

struct StreamState {
    std::mutex mtx;
    bool startPending = false;
    bool stopPending = false;
    int viewWidth = 0;
    int viewHeight = 0;
    bool active = false;  // ALVR thread's view of streaming
};

static TrackingData g_tracking;
static FrameData g_frame;
static StreamState g_stream;
static std::thread g_alvrThread;
static std::atomic<bool> g_alvrRunning{false};

// ===== Render thread context =====
struct RenderCtx {
    int viewWidth = 0, viewHeight = 0;
    bool glInit = false;
    bool streaming = false;
    GLuint streamTex[2] = {0, 0};
    GLuint blitProgram = 0;
    GLuint blitVao = 0;
    // Current frame being rendered
    uint64_t curTs = 0;
    void* curBuf = nullptr;
    bool hasFrame = false;      // true once first frame arrives, stays true until stream stops
    bool newFrameThisRender = false;  // true if onFrameBegin received a new frame this render cycle
};
static RenderCtx CTX;

// ===== Utility =====
static int64_t bootTimeNano() {
    struct timespec r = {};
    clock_gettime(CLOCK_BOOTTIME, &r);
    return (int64_t)r.tv_sec * 1000000000LL + r.tv_nsec;
}
// PicoVR SDK orientation conventions:
// - HMD: returns device orientation in tracking space, pass directly to ALVR (no transform)
// - Controllers: PicoVR uses Z-forward (left-handed), OpenVR uses Z-backward (right-handed).
//   Converting from left-handed to right-handed (flip Z axis) requires:
//   q' = {-qx, -qy, qz, qw} (negate x and y, keep z and w)
//   This fixes both forward direction and yaw rotation.
static AlvrQuat fixControllerOri(AlvrQuat q) { return {-q.x, -q.y, q.z, q.w}; }

// ===== Blit shader (for GL_TEXTURE_EXTERNAL_OES) =====
static const char *VS =
    "#version 300 es\nout vec2 vTC;\n"
    "void main(){\n"
    " vec2 p=vec2(float((gl_VertexID==1)||(gl_VertexID==3)),"
    "float((gl_VertexID==2)||(gl_VertexID==3)));\n"
    " vTC=p; gl_Position=vec4(p*2.0-1.0,0.0,1.0);\n}\n";
static const char *FS =
    "#version 300 es\n#extension GL_OES_EGL_image_external_essl3:enable\n"
    "precision highp float;\n"
    "in vec2 vTC;\nuniform samplerExternalOES uTex;\nuniform float uEyeOffset;\nout vec4 o;\n"
    "void main(){vec2 uv=vec2(vTC.x*0.5+uEyeOffset,1.0-vTC.y); o=texture(uTex,uv);}\n";

static GLuint mkShader(GLenum t, const char* s) {
    GLuint sh = glCreateShader(t);
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512]; glGetShaderInfoLog(sh, 512, nullptr, l); LOGE("Shader: %s", l); glDeleteShader(sh); return 0; }
    return sh;
}
static GLuint mkProgram(const char* vs, const char* fs) {
    GLuint v = mkShader(GL_VERTEX_SHADER, vs), f = mkShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if(v)glDeleteShader(v); if(f)glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char l[512]; glGetProgramInfoLog(p,512,nullptr,l); LOGE("Link: %s",l); glDeleteProgram(p); return 0; }
    return p;
}

static void initBlitShader() {
    if (CTX.blitProgram) return;
    CTX.blitProgram = mkProgram(VS, FS);
    if (!CTX.blitProgram) { LOGE("Blit program failed"); return; }
    glGenVertexArrays(1, &CTX.blitVao);
    LOGI("Blit shader OK (prog=%u vao=%u)", CTX.blitProgram, CTX.blitVao);
}

// Do NOT call glBindFramebuffer - PicoVR SDK has already bound the eye FBO.
// Set viewport based on eye index: left=0, right=viewWidth offset.
static void blitExtTex(GLuint tex, int eye) {
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glViewport(0, 0, CTX.viewWidth, CTX.viewHeight);
    glUseProgram(CTX.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glUniform1i(glGetUniformLocation(CTX.blitProgram, "uTex"), 0);
    // Left eye: offset=0.0 (samples U=[0, 0.5]), Right eye: offset=0.5 (samples U=[0.5, 1.0])
    glUniform1f(glGetUniformLocation(CTX.blitProgram, "uEyeOffset"), eye == 0 ? 0.0f : 0.5f);
    glBindVertexArray(CTX.blitVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

// Associate AHardwareBuffer with OES textures via EGLImage
static void assocHWB(void* hwbuf, GLuint tex[2]) {
    if (!g_eglCreateImageKHR || !g_eglGetNativeClientBuffer || !g_glEGLImageTargetTexture2DOES || !hwbuf) {
        LOGE("assocHWB: ext not loaded or buf null (cb=%p ci=%p it=%p buf=%p)",
             g_eglGetNativeClientBuffer, g_eglCreateImageKHR, g_glEGLImageTargetTexture2DOES, hwbuf);
        return;
    }
    EGLClientBuffer cb = g_eglGetNativeClientBuffer(hwbuf);
    if (!cb) { LOGE("eglGetNativeClientBuffer failed"); return; }
    EGLDisplay dpy = eglGetCurrentDisplay();
    for (int i = 0; i < 2; i++) {
        EGLImageKHR img = g_eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, nullptr);
        if (!img) { LOGE("eglCreateImageKHR fail eye=%d err=0x%x", i, eglGetError()); continue; }
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex[i]);
        g_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
        // Re-apply texture parameters after EGLImage association.
        // Some GPU drivers reset filtering to GL_NEAREST after glEGLImageTargetTexture2DOES,
        // causing blocky/mosaic-like rendering.
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_eglDestroyImageKHR(dpy, img);
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    LOGI("assocHWB OK tex=[%u,%u]", tex[0], tex[1]);
}

// ===== Controller button helpers =====
static AlvrButtonValue mkBin(bool v) { AlvrButtonValue r={}; r.tag=ALVR_BUTTON_VALUE_BINARY; r.binary=v; return r; }
static AlvrButtonValue mkScl(float v) { AlvrButtonValue r={}; r.tag=ALVR_BUTTON_VALUE_SCALAR; r.scalar=v; return r; }

// Snapshot of tracking data (copied under lock, no mutex)
struct TrackSnap {
    AlvrQuat hmdOri; float hmdPos[3];
    bool leftConn; float leftOri[4]; float leftPos[3]; int leftTrigger; int leftTouchpad[2]; bool leftButtons[8]; int leftBattery;
    bool rightConn; float rightOri[4]; float rightPos[3]; int rightTrigger; int rightTouchpad[2]; bool rightButtons[8]; int rightBattery;
};

static void sendButtons(bool left, const TrackSnap& t) {
    const bool* b = left ? t.leftButtons : t.rightButtons;
    int trig = left ? t.leftTrigger : t.rightTrigger;
    const int* tp = left ? t.leftTouchpad : t.rightTouchpad;
    float tv = trig / 255.0f;

    // Log button state when any button/trigger/touchpad is active
    bool anyActive = trig > 0 || b[0] || b[1] || b[2] || b[3] || b[4] || b[5];
    bool tpActive = (tp[0] != 0 && tp[0] != 128) || (tp[1] != 0 && tp[1] != 128);
    if (anyActive || tpActive) {
        LOGI("%s buttons: trig=%d b=[%d,%d,%d,%d,%d,%d] tp=[%d,%d]",
             left ? "L" : "R", trig,
             (int)b[0], (int)b[1], (int)b[2], (int)b[3], (int)b[4], (int)b[5],
             tp[0], tp[1]);
    }

    // Button mapping (Pico Neo 2 → ALVR Pico Neo 3 profile):
    //   b[0]=Home  → system/click
    //   b[1]=App   → menu/click
    //   b[2]=Click → thumbstick/click
    //   b[3]=AX     → x/click (left) or a/click (right)
    //   b[4]=BY     → y/click (left) or b/click (right)
    //   b[5]=Grip   → squeeze/click + squeeze/value
    //   trigger(0-255) → trigger/value + trigger/click
    if (left) {
        alvr_send_button(PATH_LEFT_SYSTEM, mkBin(b[0]));
        alvr_send_button(PATH_LEFT_MENU, mkBin(b[1]));
        alvr_send_button(PATH_LEFT_THUMBSTICK_CLICK, mkBin(b[2]));
        alvr_send_button(PATH_LEFT_X_CLICK, mkBin(b[3]));
        alvr_send_button(PATH_LEFT_Y_CLICK, mkBin(b[4]));
        alvr_send_button(PATH_LEFT_SQUEEZE_CLICK, mkBin(b[5]));
        alvr_send_button(PATH_LEFT_SQUEEZE_VALUE, mkScl(b[5] ? 1.0f : 0.0f));
        alvr_send_button(PATH_LEFT_TRIGGER_VALUE, mkScl(tv));
        alvr_send_button(PATH_LEFT_TRIGGER_CLICK, mkBin(trig > 0));
        // Pico touchpad: tp[0]=Y axis (128=center, <128=down, >128=up), tp[1]=X axis
        // Swap to match OpenXR: X=tp[1], Y=tp[0]. No negation needed.
        alvr_send_button(PATH_LEFT_THUMBSTICK_X, mkScl((tp[1]-128)/128.0f));
        alvr_send_button(PATH_LEFT_THUMBSTICK_Y, mkScl((tp[0]-128)/128.0f));
        alvr_send_button(PATH_LEFT_THUMBSTICK_TOUCH, mkBin(tp[0]!=0||tp[1]!=0));
    } else {
        alvr_send_button(PATH_RIGHT_SYSTEM, mkBin(b[0]));
        alvr_send_button(PATH_RIGHT_MENU, mkBin(b[1]));
        alvr_send_button(PATH_RIGHT_THUMBSTICK_CLICK, mkBin(b[2]));
        alvr_send_button(PATH_RIGHT_A_CLICK, mkBin(b[3]));
        alvr_send_button(PATH_RIGHT_B_CLICK, mkBin(b[4]));
        alvr_send_button(PATH_RIGHT_SQUEEZE_CLICK, mkBin(b[5]));
        alvr_send_button(PATH_RIGHT_SQUEEZE_VALUE, mkScl(b[5] ? 1.0f : 0.0f));
        alvr_send_button(PATH_RIGHT_TRIGGER_VALUE, mkScl(tv));
        alvr_send_button(PATH_RIGHT_TRIGGER_CLICK, mkBin(trig > 0));
        alvr_send_button(PATH_RIGHT_THUMBSTICK_X, mkScl((tp[1]-128)/128.0f));
        alvr_send_button(PATH_RIGHT_THUMBSTICK_Y, mkScl((tp[0]-128)/128.0f));
        alvr_send_button(PATH_RIGHT_THUMBSTICK_TOUCH, mkBin(tp[0]!=0||tp[1]!=0));
    }
}

static void sendViewParams() {
    // AlvrFov values are angles in RADIANS (not tangents!).
    // ALVR server does tan(fov.left) internally to build projection matrix.
    // See: alvr/common/src/primitives.rs: "Field of view in radians"
    // See: alvr/graphics/src/lib.rs: projection_from_fov() does tan(fov.left)
    float h_half_rad = g_fovHHalfDeg * (PI_F / 180.0f);
    float v_half_rad = g_fovVHalfDeg * (PI_F / 180.0f);
    AlvrViewParams vp[2] = {};
    vp[0].pose.orientation = {0,0,0,1}; vp[0].pose.position[0] = -IPD_HALF;
    vp[0].fov = {-h_half_rad, h_half_rad, v_half_rad, -v_half_rad};
    vp[1].pose.orientation = {0,0,0,1}; vp[1].pose.position[0] = IPD_HALF;
    vp[1].fov = {-h_half_rad, h_half_rad, v_half_rad, -v_half_rad};
    alvr_send_view_params(vp);
    LOGI("View params sent: hFov=%.1f vFov=%.1f rad(h=%.3f v=%.3f)",
         g_fovHHalfDeg*2, g_fovVHalfDeg*2, h_half_rad, v_half_rad);
}

// Send tracking from shared data (called on ALVR thread)
static bool g_leftProfileSent = false;
static bool g_rightProfileSent = false;

static void sendTracking() {
    TrackSnap t = {};
    {
        std::lock_guard<std::mutex> lk(g_tracking.mtx);
        t.hmdOri = g_tracking.hmdOri;
        memcpy(t.hmdPos, g_tracking.hmdPos, sizeof(t.hmdPos));
        t.leftConn = g_tracking.leftConn;
        memcpy(t.leftOri, g_tracking.leftOri, sizeof(t.leftOri));
        memcpy(t.leftPos, g_tracking.leftPos, sizeof(t.leftPos));
        t.leftTrigger = g_tracking.leftTrigger;
        memcpy(t.leftTouchpad, g_tracking.leftTouchpad, sizeof(t.leftTouchpad));
        memcpy(t.leftButtons, g_tracking.leftButtons, sizeof(t.leftButtons));
        t.leftBattery = g_tracking.leftBattery;
        t.rightConn = g_tracking.rightConn;
        memcpy(t.rightOri, g_tracking.rightOri, sizeof(t.rightOri));
        memcpy(t.rightPos, g_tracking.rightPos, sizeof(t.rightPos));
        t.rightTrigger = g_tracking.rightTrigger;
        memcpy(t.rightTouchpad, g_tracking.rightTouchpad, sizeof(t.rightTouchpad));
        memcpy(t.rightButtons, g_tracking.rightButtons, sizeof(t.rightButtons));
        t.rightBattery = g_tracking.rightBattery;
    }

    auto now = bootTimeNano();
    std::vector<AlvrDeviceMotion> motions;
    AlvrDeviceMotion hm = {};
    hm.device_id = HEAD_ID;
    hm.pose.orientation = t.hmdOri;  // Raw orientation, no inverse
    hm.pose.position[0] = t.hmdPos[0]; hm.pose.position[1] = t.hmdPos[1] + g_standingHeight; hm.pose.position[2] = t.hmdPos[2];
    motions.push_back(hm);
    if (t.leftConn) {
        AlvrDeviceMotion m = {};
        m.device_id = LEFT_HAND_ID;
        m.pose.orientation = fixControllerOri({t.leftOri[0],t.leftOri[1],t.leftOri[2],t.leftOri[3]});
        m.pose.position[0]=t.leftPos[0]; m.pose.position[1]=t.leftPos[1] + g_standingHeight; m.pose.position[2]=t.leftPos[2];
        motions.push_back(m);
    }
    if (t.rightConn) {
        AlvrDeviceMotion m = {};
        m.device_id = RIGHT_HAND_ID;
        m.pose.orientation = fixControllerOri({t.rightOri[0],t.rightOri[1],t.rightOri[2],t.rightOri[3]});
        m.pose.position[0]=t.rightPos[0]; m.pose.position[1]=t.rightPos[1] + g_standingHeight; m.pose.position[2]=t.rightPos[2];
        motions.push_back(m);
    }
    alvr_send_tracking(now, motions.data(), motions.size(), nullptr, nullptr);

    // Periodic tracking log
    static std::atomic<int> g_trackCount{0};
    int tn = ++g_trackCount;
    if (tn % 200 == 1) {
        LOGI("sendTracking: n=%d left=%d right=%d", tn, (int)t.leftConn, (int)t.rightConn);
        LOGI("  hmdOri=(%.2f,%.2f,%.2f,%.2f) hmdPos=(%.3f,%.3f,%.3f)",
             t.hmdOri.x, t.hmdOri.y, t.hmdOri.z, t.hmdOri.w,
             t.hmdPos[0], t.hmdPos[1], t.hmdPos[2]);
        if (t.leftConn) {
            auto f = fixControllerOri({t.leftOri[0],t.leftOri[1],t.leftOri[2],t.leftOri[3]});
            LOGI("  leftOri raw=(%.2f,%.2f,%.2f,%.2f) fixed=(%.2f,%.2f,%.2f,%.2f) pos=(%.3f,%.3f,%.3f)",
                 t.leftOri[0],t.leftOri[1],t.leftOri[2],t.leftOri[3],
                 f.x, f.y, f.z, f.w,
                 t.leftPos[0], t.leftPos[1], t.leftPos[2]);
        }
        if (t.rightConn) {
            auto f = fixControllerOri({t.rightOri[0],t.rightOri[1],t.rightOri[2],t.rightOri[3]});
            LOGI("  rightOri raw=(%.2f,%.2f,%.2f,%.2f) fixed=(%.2f,%.2f,%.2f,%.2f) pos=(%.3f,%.3f,%.3f)",
                 t.rightOri[0],t.rightOri[1],t.rightOri[2],t.rightOri[3],
                 f.x, f.y, f.z, f.w,
                 t.rightPos[0], t.rightPos[1], t.rightPos[2]);
        }
    }
    if (t.leftConn) {
        if (!g_leftProfileSent) {
            alvr_send_active_interaction_profile(LEFT_HAND_ID, PICO_NEO3_PROFILE_ID);
            LOGI("Sent left interaction profile");
            g_leftProfileSent = true;
        }
        alvr_send_battery(LEFT_HAND_ID, t.leftBattery / 100.0f, false);
        sendButtons(true, t);
    } else {
        g_leftProfileSent = false;
    }
    if (t.rightConn) {
        if (!g_rightProfileSent) {
            alvr_send_active_interaction_profile(RIGHT_HAND_ID, PICO_NEO3_PROFILE_ID);
            LOGI("Sent right interaction profile");
            g_rightProfileSent = true;
        }
        alvr_send_battery(RIGHT_HAND_ID, t.rightBattery / 100.0f, false);
        sendButtons(false, t);
    } else {
        g_rightProfileSent = false;
    }
}

// Track decoder state to avoid recreating identical decoders
static AlvrCodec g_lastCodec = (AlvrCodec)999; // sentinel value meaning "none"
static std::vector<uint8_t> g_lastConfigNal;
static bool g_decoderCreated = false;

// Create decoder (called on ALVR thread, thread-safe)
// Only recreates if codec or config NAL has changed (matching OpenXR client behavior)
static void createDecoder(AlvrCodec codec) {
    uint64_t sz = alvr_get_decoder_config(nullptr);
    if (sz == 0) { LOGE("Empty decoder config"); return; }
    std::vector<uint8_t> buf(sz);
    alvr_get_decoder_config(reinterpret_cast<char*>(buf.data()));

    // Skip if codec and config NAL are identical to current decoder
    if (g_decoderCreated && g_lastCodec == codec && g_lastConfigNal == buf) {
        LOGD("Decoder config unchanged (codec=%d), skipping", (int)codec);
        return;
    }

    LOGI("Decoder config: codec=%d (was %d), config_size=%llu",
         (int)codec, (int)g_lastCodec, (unsigned long long)sz);

    // Destroy old decoder first
    if (g_decoderCreated) {
        LOGI("Destroying old decoder");
        alvr_destroy_decoder();
        g_decoderCreated = false;
    }

    AlvrDecoderConfig dc = {};
    dc.codec = codec;
    dc.force_software_decoder = false;
    dc.max_buffering_frames = 3.0f;
    dc.buffering_history_weight = 0.8f;
    dc.config_buffer = buf.data();
    dc.config_buffer_size = sz;
    alvr_create_decoder(dc);
    g_decoderCreated = true;
    g_lastCodec = codec;
    g_lastConfigNal = buf;
    LOGI("Decoder created (codec=%d)", (int)codec);
}

// ===== ALVR thread =====
static void alvrThreadLoop() {
    LOGI("ALVR thread started");

    // Initialize ALVR OpenGL on this thread (thread_local GRAPHICS_CONTEXT)
    alvr_initialize_opengl();
    LOGI("ALVR OpenGL initialized on ALVR thread");

    while (g_alvrRunning.load()) {
        // Poll events
        AlvrEvent ev;
        while (alvr_poll_event(&ev)) {
            switch (ev.tag) {
                case ALVR_EVENT_STREAMING_STARTED: {
                    auto& d = ev.data.streaming_started;
                    LOGI("Streaming started: ALVR frame %dx%d (aspect=%.3f) vs PicoVR eye buffer %dx%d (aspect=%.3f)",
                         d.view_width, d.view_height, (float)d.view_width / (float)d.view_height,
                         CTX.viewWidth, CTX.viewHeight, CTX.viewHeight > 0 ? (float)CTX.viewWidth / (float)CTX.viewHeight : 0.0f);
                    {
                        std::lock_guard<std::mutex> lk(g_stream.mtx);
                        g_stream.startPending = true;
                        g_stream.viewWidth = d.view_width;
                        g_stream.viewHeight = d.view_height;
                        g_stream.active = true;
                    }
                    // Reset interaction profile flags so they are resent on each connection
                    g_leftProfileSent = false;
                    g_rightProfileSent = false;
                    sendViewParams();
                    break;
                }
                case ALVR_EVENT_STREAMING_STOPPED:
                    LOGI("Streaming stopped");
                    // Destroy decoder on stream stop
                    if (g_decoderCreated) {
                        alvr_destroy_decoder();
                        g_decoderCreated = false;
                        g_lastCodec = (AlvrCodec)999;
                        g_lastConfigNal.clear();
                    }
                    // Reset profile flags for next connection
                    g_leftProfileSent = false;
                    g_rightProfileSent = false;
                    {
                        std::lock_guard<std::mutex> lk(g_stream.mtx);
                        g_stream.stopPending = true;
                        g_stream.active = false;
                    }
                    break;
                case ALVR_EVENT_DECODER_CONFIG:
                    createDecoder(ev.data.decoder_config.codec);
                    break;
                case ALVR_EVENT_HUD_MESSAGE_UPDATED:
                    break;
                case ALVR_EVENT_HAPTICS:
                    LOGD("Haptics: %lu", (unsigned long)ev.data.haptics.device_id);
                    break;
                default: break;
            }
        }

        // Send tracking if streaming
        bool isActive = false;
        { std::lock_guard<std::mutex> lk(g_stream.mtx); isActive = g_stream.active; }
        if (isActive) sendTracking();

        // Get frame if streaming and previous frame consumed by render thread
        if (isActive && g_frame.consumed.load()) {
            uint64_t ts = 0;
            void* buf = nullptr;
            if (alvr_get_frame(&ts, &buf)) {
                std::lock_guard<std::mutex> lk(g_frame.mtx);
                g_frame.timestampNs = ts;
                g_frame.hardwareBuffer = buf;
                g_frame.hasNewFrame = true;
                g_frame.consumed.store(false);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Cleanup ALVR OpenGL on this thread
    alvr_pause_opengl();
    alvr_destroy_opengl();
    LOGI("ALVR thread stopped");
}

// ===== JNI =====

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    return JNI_VERSION_1_6;
}

static JavaVM* g_javaVm = nullptr;
static jobject g_javaCtx = nullptr;

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_setStreamConfigNative(
    JNIEnv *env, jobject obj, jfloat fovH, jfloat fovV, jfloat height
) {
    LOGI("setStreamConfigNative: fovH=%.1f fovV=%.1f height=%.2f", fovH, fovV, height);
    g_fovHHalfDeg = fovH;
    g_fovVHalfDeg = fovV;
    g_standingHeight = height;
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_initializeNative(JNIEnv *env, jobject obj) {
    LOGI("initializeNative");
    env->GetJavaVM(&g_javaVm);
    g_javaCtx = env->NewGlobalRef(obj);
    alvr_initialize_android_context((void*)g_javaVm, (void*)g_javaCtx);

    float rr[1] = {72.0f};
    AlvrClientCapabilities caps = {};
    // Use square encoding to match potential square PicoVR SDK eye buffer
    caps.default_view_width = 1920;
    caps.default_view_height = 1920;
    caps.refresh_rates = rr;
    caps.refresh_rates_count = 1;
    // Foveated encoding (FFR) disabled: our blit shader does not implement FFR unpacking.
    // The ALVR OpenXR client uses a StagingRenderer with a complex shader to unpack
    // FFR-compressed frames. Without it, edges look blocky/mosaic-like.
    caps.foveated_encoding = false;
    caps.encoder_high_profile = true;
    caps.prefer_full_range = true;
    caps.preferred_encoding_gamma = 1.0f;
    alvr_initialize(caps);
    initPathIds();
    LOGI("ALVR initialized");

    // Start ALVR thread
    g_alvrRunning.store(true);
    g_alvrThread = std::thread(alvrThreadLoop);
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_initGLNative(JNIEnv *env, jobject obj, jint w, jint h) {
    LOGI("initGLNative: %dx%d", w, h);
    // Store raw dimensions from PicoVR SDK. These may be per-eye (e.g. 1920x2160)
    // or full-screen (e.g. 3840x2160). We use them as-is for viewport.
    CTX.viewWidth = w;
    CTX.viewHeight = h;
    LOGI("PicoVR eye buffer: %dx%d (aspect=%.3f)", w, h, (float)w / (float)h);
    LOGI("initGLNative: raw w=%d h=%d (will use as viewport)", w, h);
    // Create blit shader in PicoVR's EGL context
    initBlitShader();
    loadEglExtensions();
    CTX.glInit = true;
    LOGI("GL initialized on render thread, viewWidth=%d viewHeight=%d", CTX.viewWidth, CTX.viewHeight);
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_deInitGLNative(JNIEnv *env, jobject obj) {
    LOGI("deInitGLNative");
    if (CTX.streamTex[0]) { glDeleteTextures(2, CTX.streamTex); CTX.streamTex[0]=CTX.streamTex[1]=0; }
    if (CTX.blitVao) { glDeleteVertexArrays(1, &CTX.blitVao); CTX.blitVao=0; }
    if (CTX.blitProgram) { glDeleteProgram(CTX.blitProgram); CTX.blitProgram=0; }
    CTX.glInit = false;
    CTX.streaming = false;
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_resumeNative(JNIEnv *env, jobject obj) {
    LOGI("resumeNative");
    alvr_resume();
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_resetStreamStateNative(JNIEnv *env, jobject obj) {
    LOGI("resetStreamStateNative: resetting render state");
    CTX.hasFrame = false;
    CTX.newFrameThisRender = false;
    // Mark frame as consumed so ALVR thread can get next frame
    g_frame.consumed.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_pauseNative(JNIEnv *env, jobject obj) {
    LOGI("pauseNative");
    alvr_pause();
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_destroyNative(JNIEnv *env, jobject obj) {
    LOGI("destroyNative");
    g_alvrRunning.store(false);
    if (g_alvrThread.joinable()) g_alvrThread.join();
    alvr_destroy();
    if (g_javaCtx) { env->DeleteGlobalRef(g_javaCtx); g_javaCtx = nullptr; }
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_onFrameBeginNative(
    JNIEnv *env, jobject obj,
    jfloatArray hmdOri, jfloatArray hmdPos,
    jboolean leftConn, jfloatArray leftOri, jfloatArray leftPos,
    jint leftTrigger, jintArray leftTouchpad, jbooleanArray leftButtons, jint leftBattery,
    jboolean rightConn, jfloatArray rightOri, jfloatArray rightPos,
    jint rightTrigger, jintArray rightTouchpad, jbooleanArray rightButtons, jint rightBattery
) {
    // Update tracking data in shared variable (for ALVR thread to read)
    {
        std::lock_guard<std::mutex> lk(g_tracking.mtx);
        env->GetFloatArrayRegion(hmdOri, 0, 4, (jfloat*)&g_tracking.hmdOri);
        env->GetFloatArrayRegion(hmdPos, 0, 3, g_tracking.hmdPos);
        g_tracking.leftConn = leftConn;
        if (leftConn && leftOri && leftPos && leftTouchpad && leftButtons) {
            env->GetFloatArrayRegion(leftOri, 0, 4, g_tracking.leftOri);
            env->GetFloatArrayRegion(leftPos, 0, 3, g_tracking.leftPos);
            g_tracking.leftTrigger = leftTrigger;
            env->GetIntArrayRegion(leftTouchpad, 0, 2, g_tracking.leftTouchpad);
            env->GetBooleanArrayRegion(leftButtons, 0, 8, (jboolean*)g_tracking.leftButtons);
            g_tracking.leftBattery = leftBattery;
        }

        // Periodic controller state log
        static std::atomic<int> g_fbCount{0};
        int fbn = ++g_fbCount;
        if (fbn % 200 == 1) {
            LOGI("onFrameBegin: n=%d left=%d right=%d leftTrig=%d rightTrig=%d",
                 fbn, (int)leftConn, (int)rightConn, leftTrigger, rightTrigger);
        }
        g_tracking.rightConn = rightConn;
        if (rightConn && rightOri && rightPos && rightTouchpad && rightButtons) {
            env->GetFloatArrayRegion(rightOri, 0, 4, g_tracking.rightOri);
            env->GetFloatArrayRegion(rightPos, 0, 3, g_tracking.rightPos);
            g_tracking.rightTrigger = rightTrigger;
            env->GetIntArrayRegion(rightTouchpad, 0, 2, g_tracking.rightTouchpad);
            env->GetBooleanArrayRegion(rightButtons, 0, 8, (jboolean*)g_tracking.rightButtons);
            g_tracking.rightBattery = rightBattery;
        }
    }

    // Handle streaming state changes from ALVR thread
    {
        std::lock_guard<std::mutex> lk(g_stream.mtx);
        if (g_stream.startPending) {
            g_stream.startPending = false;
            LOGI("Render: creating stream textures");
            glGenTextures(2, CTX.streamTex);
            for (int i = 0; i < 2; i++) {
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, CTX.streamTex[i]);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
            CTX.streaming = true;
        }
        if (g_stream.stopPending) {
            g_stream.stopPending = false;
            CTX.streaming = false;
            if (CTX.streamTex[0]) { glDeleteTextures(2, CTX.streamTex); CTX.streamTex[0]=CTX.streamTex[1]=0; }
        }
    }

    // Check for new frame from ALVR thread
    if (CTX.streaming) {
        bool newFrame = false;
        uint64_t ts = 0;
        void* buf = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_frame.mtx);
            if (g_frame.hasNewFrame) {
                ts = g_frame.timestampNs;
                buf = g_frame.hardwareBuffer;
                g_frame.hasNewFrame = false;
                newFrame = true;
            }
        }
        if (newFrame) {
            CTX.curTs = ts;
            CTX.curBuf = buf;
            CTX.hasFrame = true;
            CTX.newFrameThisRender = true;
            LOGI("New frame: ts=%llu buf=%p", (unsigned long long)ts, buf);
            AlvrViewParams vp[2] = {};
            alvr_report_compositor_start(ts, vp);
            assocHWB(buf, CTX.streamTex);
        }
    }
}

// Do NOT call glBindFramebuffer - PicoVR SDK has already bound the correct eye FBO.
// Use viewport (0,0,w,h) for each eye - SDK binds per-eye FBO before calling us.
static std::atomic<int> g_drawCount{0};
extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_onDrawEyeNative(JNIEnv *env, jobject obj, jint eye) {
    if (!CTX.glInit) return;
    // Always use (0,0) origin - SDK binds a per-eye FBO before calling us.
    glViewport(0, 0, CTX.viewWidth, CTX.viewHeight);
    if (CTX.streaming && CTX.hasFrame && CTX.streamTex[eye]) {
        blitExtTex(CTX.streamTex[eye], eye);
    } else {
        // Black when no frame available (before first frame arrives)
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // Log every 100 frames to verify render loop is running
    int n = ++g_drawCount;
    if (n % 100 == 1) {
        LOGI("onDrawEye: count=%d eye=%d streaming=%d hasFrame=%d tex[%d]=%u",
             n, eye, (int)CTX.streaming, (int)CTX.hasFrame, eye, CTX.streamTex[eye]);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_top_playtbsxys_picostreamer_PicoALVRActivity_onFrameEndNative(JNIEnv *env, jobject obj) {
    // Only submit and release when a NEW frame was received this render cycle.
    // Keep CTX.hasFrame true so we re-render the last frame if no new one arrives.
    if (CTX.streaming && CTX.newFrameThisRender) {
        alvr_report_submit(CTX.curTs, 0);
        CTX.newFrameThisRender = false;
        g_frame.consumed.store(true);
    }
    // Log every 100 frames
    static std::atomic<int> s_frameEndCount{0};
    int n = ++s_frameEndCount;
    if (n % 100 == 1) {
        LOGI("onFrameEnd: count=%d streaming=%d hasFrame=%d newFrame=%d",
             n, (int)CTX.streaming, (int)CTX.hasFrame, (int)CTX.newFrameThisRender);
    }
}
