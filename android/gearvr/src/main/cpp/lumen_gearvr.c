// Lumen Gear VR client, native half: VrApi session and frame submission.
//
// MediaCodec decodes into a SurfaceTexture owned by this render thread. Once
// per display frame we latch the newest decoded image (updateTexImage), find
// the head pose the PC rendered *that* image with (the decoder stamps each
// output with its frame's id, and the Java side tells us the pose per id),
// copy it into a VrApi texture swap chain and submit one projection layer:
// left eye = left half, right eye = right half, rendered from that pose with
// the PC's field of view. TimeWarp then reprojects it to the current head pose
// and corrects the lenses.
//
// Latching ourselves matters: when the compositor latched the decoder's
// surface directly, the pose we reported could be a frame off from the image
// it actually showed, and reprojecting with the wrong pose looked like heavy
// jitter whenever the head moved.
//
// Threads:
//   - render thread (here): VrApi, GL, SurfaceTexture, submit at vsync
//   - Java UI thread: lifecycle and surface events, under gApp.mutex
//   - Java decoder thread: nativeSetFramePose / nativeSetVideoSize
//   - Java pose thread: nativeGetHeadPose (VrApi allows any thread in VR mode)

#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <android/log.h>
#include <android/bitmap.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65 // OES_EGL_image_external
#endif

#include "VrApi.h"
#include "VrApi_Helpers.h"

#define LOG_TAG "LumenGearVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct {
    long long timestampNs; // surface timestamp of the decoded image = frame id
    float pose[4];         // w, x, y, z
    float tanHalfFov[2];   // x, y
    float bounds[4];       // uMin, vMin, uMax, vMax: picture area in each eye's half (v down)
} FramePose;

#define POSE_RING 32
// Starting head pose prediction, until the round trip has been measured: about
// what a Galaxy S7 on 5 GHz Wi-Fi measures.
#define kDefaultPredictMs 70.0f
#define SENT_RING 512 // ~2 s of sent poses at 250 Hz

// A head pose sent to the PC, kept to measure how long until it comes back as
// a displayed frame's render pose.
typedef struct {
    double sampleTime; // vrapi_GetTimeInSeconds() when the sensor was read
    float q[4];        // w, x, y, z as sent
} SentPose;

static struct {
    JavaVM *vm;
    jobject activity; // global ref
    jmethodID onVideoSurface;

    pthread_t thread;
    bool threadStarted;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    // Guarded by mutex.
    ANativeWindow *window;
    bool resumed;
    bool quit;
    bool recenterRequested;
    ovrMobile *ovr;
    FramePose poses[POSE_RING]; // recent rendered frames, by timestamp
    int nextPose;
    SentPose sent[SENT_RING];
    int nextSent;
    float predictSeconds; // how far ahead to predict poses sent to the PC
    int videoWidth, videoHeight;
    uint32_t *statusPixels; // RGBA, from nativeSetStatus
    int statusPixelsWidth, statusPixelsHeight;
    bool statusDirty;

    // Render thread only.
    EGLDisplay display;
    EGLContext context;
    EGLSurface pbuffer;
    ovrJava java;
    GLuint oesTexture;
    jobject surfaceTexture; // global ref
    jmethodID updateTexImage, getTimestamp, getTransformMatrix;
    jfloatArray transformArray; // global ref
    GLuint program;
    GLint uTexMatrix;
    ovrTextureSwapChain *chain;
    int chainWidth, chainHeight, chainLength, chainIndex;
    GLuint *framebuffers;
    long long latchedTimestamp;
    bool haveFrame;
    double lastLatchTime; // vrapi time of the last new video frame
    ovrTextureSwapChain *statusChain; // status panel (StatusPanel.java), shown without video
    int statusWidth, statusHeight;
    FramePose shown; // pose of the image in the swap chain buffer we submit
    double latencySum, latencyMax, lastReport;
    int latencyCount, unmatched;
    double shownSampleTime; // when the latched frame's pose was sampled; 0 = unknown or already counted
    double displaySum;      // pose sampled -> first displayed, summed over this second
    int displayCount;
    float adaptivePredictMs; // follows the measured pose -> display time
    int suggestedEyeWidth, suggestedEyeHeight; // VrApi's, once initialized
    float suggestedFovX;
} gApp = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .predictSeconds = kDefaultPredictMs / 1000.0f,
    .adaptivePredictMs = kDefaultPredictMs,
};

// ---- EGL: a GLES 3 context for VrApi and the copy pass.

static bool EglCreate(void) {
    gApp.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(gApp.display, NULL, NULL);
    const EGLint configAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                    EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(gApp.display, configAttribs, &config, 1, &count) || count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    gApp.context = eglCreateContext(gApp.display, config, EGL_NO_CONTEXT, contextAttribs);
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    gApp.pbuffer = eglCreatePbufferSurface(gApp.display, config, surfaceAttribs);
    if (gApp.context == EGL_NO_CONTEXT || gApp.pbuffer == EGL_NO_SURFACE ||
        !eglMakeCurrent(gApp.display, gApp.pbuffer, gApp.pbuffer, gApp.context)) {
        LOGE("EGL context setup failed: 0x%x", eglGetError());
        return false;
    }
    return true;
}

static void EglDestroy(void) {
    if (gApp.display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(gApp.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gApp.pbuffer != EGL_NO_SURFACE)
        eglDestroySurface(gApp.display, gApp.pbuffer);
    if (gApp.context != EGL_NO_CONTEXT)
        eglDestroyContext(gApp.display, gApp.context);
    eglTerminate(gApp.display);
    gApp.display = EGL_NO_DISPLAY;
}

// ---- Copy pass: external (decoder) texture -> swap chain image.

static const char *kVertexShader =
    "#version 300 es\n"
    "uniform mat4 uTexMatrix;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    // Fullscreen quad from the vertex id (triangle strip of 4).\n"
    "    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "    vUv = (uTexMatrix * vec4(p, 0.0, 1.0)).xy;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *kFragmentShader =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES uTexture;\n"
    "in vec2 vUv;\n"
    "out vec4 outColor;\n"
    "void main() { outColor = texture(uTexture, vUv); }\n";

static GLuint CompileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
    }
    return shader;
}

static bool CreateCopyProgram(void) {
    gApp.program = glCreateProgram();
    glAttachShader(gApp.program, CompileShader(GL_VERTEX_SHADER, kVertexShader));
    glAttachShader(gApp.program, CompileShader(GL_FRAGMENT_SHADER, kFragmentShader));
    glLinkProgram(gApp.program);
    GLint ok = 0;
    glGetProgramiv(gApp.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGE("program link failed");
        return false;
    }
    gApp.uTexMatrix = glGetUniformLocation(gApp.program, "uTexMatrix");
    glUseProgram(gApp.program);
    glUniform1i(glGetUniformLocation(gApp.program, "uTexture"), 0);
    return true;
}

static void DestroySwapChain(void) {
    if (!gApp.chain)
        return;
    glDeleteFramebuffers(gApp.chainLength, gApp.framebuffers);
    free(gApp.framebuffers);
    gApp.framebuffers = NULL;
    vrapi_DestroyTextureSwapChain(gApp.chain);
    gApp.chain = NULL;
}

// (Re)creates the swap chain at the video's size.
static bool EnsureSwapChain(int width, int height) {
    if (gApp.chain && gApp.chainWidth == width && gApp.chainHeight == height)
        return true;
    DestroySwapChain();
    gApp.chain = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_RGBA8, width, height, 1, 3);
    if (!gApp.chain) {
        LOGE("vrapi_CreateTextureSwapChain3 %dx%d failed", width, height);
        return false;
    }
    gApp.chainWidth = width;
    gApp.chainHeight = height;
    gApp.chainLength = vrapi_GetTextureSwapChainLength(gApp.chain);
    gApp.chainIndex = 0;
    gApp.framebuffers = calloc((size_t)gApp.chainLength, sizeof(GLuint));
    glGenFramebuffers(gApp.chainLength, gApp.framebuffers);
    for (int i = 0; i < gApp.chainLength; i++) {
        GLuint texture = vrapi_GetTextureSwapChainHandle(gApp.chain, i);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, gApp.framebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("swap chain %dx%d, %d buffers", width, height, gApp.chainLength);
    return true;
}

// Finds which sent pose a frame was rendered with and records how long ago
// that pose was sampled. Only counts unambiguous matches: while the head is
// still, many sent poses are identical and the age can't be told. Returns
// when the matched pose was sampled, or 0 without a match.
static double MeasureLatency(const float q[4], double now) {
    double sampleTime = 0;
    double best = 0, second = 0;
    int bestIndex = -1;
    pthread_mutex_lock(&gApp.mutex);
    for (int i = 0; i < SENT_RING; i++) {
        const SentPose *s = &gApp.sent[i];
        if (s->sampleTime == 0)
            continue;
        double dot = fabs((double)s->q[0] * q[0] + (double)s->q[1] * q[1] + (double)s->q[2] * q[2] +
                          (double)s->q[3] * q[3]);
        if (dot > best) {
            second = best;
            best = dot;
            bestIndex = i;
        } else if (dot > second) {
            second = dot;
        }
    }
    // Within ~0.01 deg of the render pose, and the runner-up clearly farther.
    if (bestIndex >= 0 && 1.0 - best < 2e-8 && 1.0 - second > 1e-6) {
        sampleTime = gApp.sent[bestIndex].sampleTime;
        double age = now - sampleTime;
        gApp.latencySum += age;
        if (age > gApp.latencyMax)
            gApp.latencyMax = age;
        gApp.latencyCount++;
    } else {
        gApp.unmatched++;
    }
    pthread_mutex_unlock(&gApp.mutex);
    return sampleTime;
}

// Prediction never goes outside this range, whatever was measured.
#define kMinPredictMs 20.0f
#define kMaxPredictMs 120.0f

// Once a second: log latency stats and update the head pose prediction.
//
// Prediction follows the measured time from sampling a head pose to showing
// the frame rendered with it, so the PC renders (and draws head-locked things
// like the dashboard cursor) where the head will be at display time, even as
// Wi-Fi gets slower or faster. It moves 30% of the way to each second's
// average, so one slow second doesn't throw it. adb shell setprop
// debug.lumen.predict_ms N fixes it at N ms instead (0 = no prediction);
// set it to "" to go back to adaptive.
static void PeriodicWork(double now) {
    if (now - gApp.lastReport < 1.0)
        return;
    gApp.lastReport = now;
    if (gApp.displayCount >= 5) {
        float measuredMs = (float)(1000.0 * gApp.displaySum / gApp.displayCount);
        float next = gApp.adaptivePredictMs + 0.3f * (measuredMs - gApp.adaptivePredictMs);
        gApp.adaptivePredictMs = fminf(kMaxPredictMs, fmaxf(kMinPredictMs, next));
    }
    char value[PROP_VALUE_MAX] = {0};
    bool fixed = __system_property_get("debug.lumen.predict_ms", value) > 0;
    float predictMs = fixed ? strtof(value, NULL) : gApp.adaptivePredictMs;
    pthread_mutex_lock(&gApp.mutex);
    gApp.predictSeconds = predictMs / 1000.0f;
    if (gApp.latencyCount > 0)
        __android_log_print(ANDROID_LOG_INFO, "LumenLatency",
                            "head pose sampled -> frame latched: avg %.1f ms, max %.1f ms; -> displayed: avg %.1f ms "
                            "(%d frames matched, %d ambiguous); predicting %.0f ms (%s)",
                            1000.0 * gApp.latencySum / gApp.latencyCount, 1000.0 * gApp.latencyMax,
                            gApp.displayCount > 0 ? 1000.0 * gApp.displaySum / gApp.displayCount : 0.0,
                            gApp.latencyCount, gApp.unmatched, predictMs, fixed ? "fixed" : "adaptive");
    else
        __android_log_print(ANDROID_LOG_INFO, "LumenLatency", "no frames matched (head still?), predicting %.0f ms (%s)",
                            predictMs, fixed ? "fixed" : "adaptive");
    gApp.latencySum = gApp.latencyMax = 0;
    gApp.latencyCount = gApp.unmatched = 0;
    gApp.displaySum = 0;
    gApp.displayCount = 0;
    pthread_mutex_unlock(&gApp.mutex);
}

// Latches the newest decoded image. If it's new, copies it into the next swap
// chain buffer and looks up the pose it was rendered with. Returns whether a
// frame is ready to show (new or the previous one).
static bool LatchFrame(JNIEnv *env) {
    pthread_mutex_lock(&gApp.mutex);
    int width = gApp.videoWidth, height = gApp.videoHeight;
    pthread_mutex_unlock(&gApp.mutex);
    if (width <= 0 || height <= 0 || !EnsureSwapChain(width, height))
        return gApp.haveFrame;

    (*env)->CallVoidMethod(env, gApp.surfaceTexture, gApp.updateTexImage);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return gApp.haveFrame;
    }
    long long timestamp = (*env)->CallLongMethod(env, gApp.surfaceTexture, gApp.getTimestamp);
    if (timestamp == 0 || timestamp == gApp.latchedTimestamp)
        return gApp.haveFrame; // nothing new: TimeWarp reprojects the last one again

    // The pose for exactly this image.
    FramePose pose = {0};
    pthread_mutex_lock(&gApp.mutex);
    for (int i = 0; i < POSE_RING; i++)
        if (gApp.poses[i].timestampNs == timestamp)
            pose = gApp.poses[i];
    pthread_mutex_unlock(&gApp.mutex);
    if (pose.timestampNs == 0)
        return gApp.haveFrame; // shouldn't happen; keep showing the previous frame
    gApp.latchedTimestamp = timestamp;

    float texMatrix[16];
    (*env)->CallVoidMethod(env, gApp.surfaceTexture, gApp.getTransformMatrix, gApp.transformArray);
    (*env)->GetFloatArrayRegion(env, gApp.transformArray, 0, 16, texMatrix);

    gApp.chainIndex = (gApp.chainIndex + 1) % gApp.chainLength;
    glBindFramebuffer(GL_FRAMEBUFFER, gApp.framebuffers[gApp.chainIndex]);
    // Every pixel is overwritten: don't load the old contents (tile-based GPU).
    const GLenum attachment = GL_COLOR_ATTACHMENT0;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
    glViewport(0, 0, gApp.chainWidth, gApp.chainHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(gApp.program);
    glUniformMatrix4fv(gApp.uTexMatrix, 1, GL_FALSE, texMatrix);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gApp.oesTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    gApp.shown = pose;
    gApp.haveFrame = true;
    gApp.lastLatchTime = vrapi_GetTimeInSeconds();
    gApp.shownSampleTime = MeasureLatency(pose.pose, vrapi_GetTimeInSeconds());
    return true;
}

// Creates the SurfaceTexture the decoder renders into and hands its Surface
// to Java.
static bool CreateVideoSurface(JNIEnv *env) {
    glGenTextures(1, &gApp.oesTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gApp.oesTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    jclass stClass = (*env)->FindClass(env, "android/graphics/SurfaceTexture");
    jmethodID stInit = (*env)->GetMethodID(env, stClass, "<init>", "(I)V");
    jobject st = (*env)->NewObject(env, stClass, stInit, (jint)gApp.oesTexture);
    gApp.surfaceTexture = (*env)->NewGlobalRef(env, st);
    gApp.updateTexImage = (*env)->GetMethodID(env, stClass, "updateTexImage", "()V");
    gApp.getTimestamp = (*env)->GetMethodID(env, stClass, "getTimestamp", "()J");
    gApp.getTransformMatrix = (*env)->GetMethodID(env, stClass, "getTransformMatrix", "([F)V");
    gApp.transformArray = (*env)->NewGlobalRef(env, (*env)->NewFloatArray(env, 16));

    jclass surfaceClass = (*env)->FindClass(env, "android/view/Surface");
    jmethodID surfaceInit =
        (*env)->GetMethodID(env, surfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    jobject surface = (*env)->NewObject(env, surfaceClass, surfaceInit, st);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("could not create the video SurfaceTexture");
        return false;
    }
    (*env)->CallVoidMethod(env, gApp.activity, gApp.onVideoSurface, surface);
    return true;
}

static void DestroyVideoSurface(JNIEnv *env) {
    if (gApp.surfaceTexture) {
        jclass stClass = (*env)->GetObjectClass(env, gApp.surfaceTexture);
        jmethodID release = (*env)->GetMethodID(env, stClass, "release", "()V");
        (*env)->CallVoidMethod(env, gApp.surfaceTexture, release);
        (*env)->DeleteGlobalRef(env, gApp.surfaceTexture);
        gApp.surfaceTexture = NULL;
    }
    if (gApp.transformArray) {
        (*env)->DeleteGlobalRef(env, gApp.transformArray);
        gApp.transformArray = NULL;
    }
    glDeleteTextures(1, &gApp.oesTexture);
}

// ---- VR mode (called with gApp.mutex held).

static void EnterVrMode(void) {
    ovrModeParms parms = vrapi_DefaultModeParms(&gApp.java);
    parms.Flags |= VRAPI_MODE_FLAG_NATIVE_WINDOW;
    parms.Flags &= ~VRAPI_MODE_FLAG_RESET_WINDOW_FULLSCREEN; // the SurfaceView is already fullscreen
    parms.Display = (size_t)gApp.display;
    parms.WindowSurface = (size_t)gApp.window;
    parms.ShareContext = (size_t)gApp.context;
    gApp.ovr = vrapi_EnterVrMode(&parms);
    if (!gApp.ovr) {
        LOGE("vrapi_EnterVrMode failed");
        return;
    }
    // CPU 3: the decoder feed, networking and pose thread are CPU work; at
    // level 2 they stalled. GPU work is one copy per frame.
    vrapi_SetClockLevels(gApp.ovr, 3, 2);
    vrapi_SetPerfThread(gApp.ovr, VRAPI_PERF_THREAD_TYPE_MAIN, (uint32_t)gettid());
    LOGI("entered VR mode");
}

static void LeaveVrMode(void) {
    vrapi_LeaveVrMode(gApp.ovr);
    gApp.ovr = NULL;
    pthread_cond_broadcast(&gApp.cond);
    LOGI("left VR mode");
}

// ---- Frame submission.

// Maps tan-angle directions to texture coordinates for one eye: its half of
// the side-by-side frame, and within that the picture area (games may render
// into part of their texture). The PC rendered each eye with a symmetric
// frustum of tan(half FOV) = tanX, tanY.
static ovrMatrix4f EyeTexCoordsFromTanAngles(int eye, const FramePose *frame) {
    const float toDegrees = 180.0f / (float)M_PI;
    ovrMatrix4f projection =
        ovrMatrix4f_CreateProjectionFov(2.0f * atanf(frame->tanHalfFov[0]) * toDegrees,
                                        2.0f * atanf(frame->tanHalfFov[1]) * toDegrees, 0.0f, 0.0f, 0.1f, 0.0f);
    ovrMatrix4f m = ovrMatrix4f_TanAngleMatrixFromProjection(&projection);
    const float *b = frame->bounds;
    // The compositor divides rows 0 and 1 by row 2 (which yields 1 for a
    // direction with z = -1), so adding a multiple of row 2 adds a constant.
    // u -> half offset + picture area: 0.5 eye + 0.5 (uMin + u (uMax - uMin)).
    const float uScale = 0.5f * (b[2] - b[0]), uOffset = 0.5f * (float)eye + 0.5f * b[0];
    // v: the copied texture has the image's top row at v = 1, so a picture
    // spanning rows vMin..vMax from the top covers v = 1 - vMax .. 1 - vMin.
    const float vScale = b[3] - b[1], vOffset = 1.0f - b[3];
    for (int c = 0; c < 4; c++) {
        m.M[0][c] = uScale * m.M[0][c] + uOffset * m.M[2][c];
        m.M[1][c] = vScale * m.M[1][c] + vOffset * m.M[2][c];
    }
    return m;
}

// Uploads a new status bitmap, if any, into the status swap chain.
static void UpdateStatusTexture(void) {
    pthread_mutex_lock(&gApp.mutex);
    if (!gApp.statusDirty || !gApp.statusPixels) {
        pthread_mutex_unlock(&gApp.mutex);
        return;
    }
    int width = gApp.statusPixelsWidth, height = gApp.statusPixelsHeight;
    if (!gApp.statusChain || gApp.statusWidth != width || gApp.statusHeight != height) {
        if (gApp.statusChain)
            vrapi_DestroyTextureSwapChain(gApp.statusChain);
        gApp.statusChain = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_RGBA8, width, height, 1, 1);
        gApp.statusWidth = width;
        gApp.statusHeight = height;
    }
    if (gApp.statusChain) {
        glBindTexture(GL_TEXTURE_2D, vrapi_GetTextureSwapChainHandle(gApp.statusChain, 0));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Clamp: everything around the panel samples its black edge.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gApp.statusPixels);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    gApp.statusDirty = false;
    pthread_mutex_unlock(&gApp.mutex);
}

// A head-locked panel ~50 degrees wide in the middle of the view, showing the
// status bitmap. Both eyes see the same image (flat, at "infinity").
static ovrLayerProjection2 StatusLayer(double displayTime) {
    ovrLayerProjection2 layer = vrapi_DefaultLayerProjection2();
    // Head-locked: give it the head pose at display time, so TimeWarp has
    // nothing to correct and it stays put in the view.
    ovrTracking2 tracking = vrapi_GetPredictedTracking2(gApp.ovr, displayTime);
    layer.HeadPose = tracking.HeadPose;
    const float fovX = 50.0f, fovY = fovX * (float)gApp.statusHeight / (float)gApp.statusWidth;
    ovrMatrix4f projection = ovrMatrix4f_CreateProjectionFov(fovX, fovY, 0.0f, 0.0f, 0.1f, 0.0f);
    ovrMatrix4f m = ovrMatrix4f_TanAngleMatrixFromProjection(&projection);
    // The bitmap's top row was uploaded first (v = 0): flip v.
    for (int c = 0; c < 4; c++)
        m.M[1][c] = -m.M[1][c] + m.M[2][c];
    for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++) {
        layer.Textures[eye].ColorSwapChain = gApp.statusChain;
        layer.Textures[eye].SwapChainIndex = 0;
        layer.Textures[eye].TexCoordsFromTanAngles = m;
    }
    return layer;
}

static void SubmitFrame(long long frameIndex, bool haveFrame) {
    const double displayTime = vrapi_GetPredictedDisplayTime(gApp.ovr, frameIndex);
    // Without video for 2 s (none yet, or the PC stopped), show the status
    // panel instead, so it's clear the app is running and what it waits for.
    bool videoLive = haveFrame && vrapi_GetTimeInSeconds() - gApp.lastLatchTime < 2.0;

    ovrLayerProjection2 layer;
    if (videoLive) {
        // The first time this frame goes on screen: how long since its pose
        // was sampled (drives the prediction, see PeriodicWork).
        if (gApp.shownSampleTime > 0) {
            gApp.displaySum += displayTime - gApp.shownSampleTime;
            gApp.displayCount++;
            gApp.shownSampleTime = 0;
        }
        const FramePose *frame = &gApp.shown;
        layer = vrapi_DefaultLayerProjection2();
        // The pose this image was rendered with; TimeWarp reprojects from
        // here to the pose at display time.
        layer.HeadPose.Pose.Orientation.w = frame->pose[0];
        layer.HeadPose.Pose.Orientation.x = frame->pose[1];
        layer.HeadPose.Pose.Orientation.y = frame->pose[2];
        layer.HeadPose.Pose.Orientation.z = frame->pose[3];
        for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++) {
            layer.Textures[eye].ColorSwapChain = gApp.chain;
            layer.Textures[eye].SwapChainIndex = gApp.chainIndex;
            layer.Textures[eye].TexCoordsFromTanAngles = EyeTexCoordsFromTanAngles(eye, frame);
            // Never sample outside this eye's picture.
            const float *b = frame->bounds;
            layer.Textures[eye].TextureRect.x = 0.5f * (float)eye + 0.5f * b[0];
            layer.Textures[eye].TextureRect.y = 1.0f - b[3];
            layer.Textures[eye].TextureRect.width = 0.5f * (b[2] - b[0]);
            layer.Textures[eye].TextureRect.height = b[3] - b[1];
        }
    } else {
        UpdateStatusTexture();
        layer = gApp.statusChain ? StatusLayer(displayTime) : vrapi_DefaultLayerBlackProjection2();
    }

    const ovrLayerHeader2 *layers[] = {&layer.Header};
    ovrSubmitFrameDescription2 desc = {0};
    desc.SwapInterval = 1;
    desc.FrameIndex = (uint64_t)frameIndex;
    desc.DisplayTime = displayTime;
    desc.LayerCount = 1;
    desc.Layers = layers;
    vrapi_SubmitFrame2(gApp.ovr, &desc); // paces us to the display
}

// ---- Render thread.

static void *RenderThread(void *arg) {
    (void)arg;
    prctl(PR_SET_NAME, (long)"LumenRender", 0, 0, 0);
    JNIEnv *env = NULL;
    (*gApp.vm)->AttachCurrentThread(gApp.vm, &env, NULL);
    gApp.java.Vm = gApp.vm;
    gApp.java.Env = env;
    gApp.java.ActivityObject = gApp.activity;

    ovrInitParms initParms = vrapi_DefaultInitParms(&gApp.java);
    initParms.GraphicsAPI = VRAPI_GRAPHICS_API_OPENGL_ES_3;
    if (vrapi_Initialize(&initParms) != VRAPI_INITIALIZE_SUCCESS) {
        LOGE("vrapi_Initialize failed");
        (*gApp.vm)->DetachCurrentThread(gApp.vm);
        return NULL;
    }
    pthread_mutex_lock(&gApp.mutex);
    gApp.suggestedEyeWidth = vrapi_GetSystemPropertyInt(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH);
    gApp.suggestedEyeHeight = vrapi_GetSystemPropertyInt(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT);
    gApp.suggestedFovX = vrapi_GetSystemPropertyFloat(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X);
    pthread_mutex_unlock(&gApp.mutex);
    LOGI("VrApi %s; suggested eye FOV %.1f x %.1f deg, eye texture %d x %d", vrapi_GetVersionString(),
         vrapi_GetSystemPropertyFloat(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X),
         vrapi_GetSystemPropertyFloat(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_Y),
         vrapi_GetSystemPropertyInt(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH),
         vrapi_GetSystemPropertyInt(&gApp.java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT));

    if (EglCreate() && CreateCopyProgram() && CreateVideoSurface(env)) {
        long long frameIndex = 1;
        for (;;) {
            pthread_mutex_lock(&gApp.mutex);
            while (!gApp.quit && !(gApp.resumed && gApp.window)) {
                if (gApp.ovr)
                    LeaveVrMode();
                pthread_cond_wait(&gApp.cond, &gApp.mutex);
            }
            if (gApp.quit) {
                if (gApp.ovr)
                    LeaveVrMode();
                pthread_mutex_unlock(&gApp.mutex);
                break;
            }
            if (!gApp.ovr)
                EnterVrMode();
            if (gApp.ovr && gApp.recenterRequested) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                vrapi_RecenterPose(gApp.ovr);
#pragma clang diagnostic pop
                LOGI("recentered");
            }
            gApp.recenterRequested = false;
            ovrMobile *ovr = gApp.ovr;
            pthread_mutex_unlock(&gApp.mutex);

            if (!ovr) {
                usleep(100 * 1000); // EnterVrMode failed; retry later
                continue;
            }
            bool haveFrame = LatchFrame(env);
            SubmitFrame(frameIndex++, haveFrame);
            PeriodicWork(vrapi_GetTimeInSeconds());
        }
    }

    DestroySwapChain();
    if (gApp.statusChain) {
        vrapi_DestroyTextureSwapChain(gApp.statusChain);
        gApp.statusChain = NULL;
    }
    DestroyVideoSurface(env);
    EglDestroy();
    vrapi_Shutdown();
    (*gApp.vm)->DetachCurrentThread(gApp.vm);
    return NULL;
}

// ---- JNI entry points (com.lumen.gearvr.GearVrActivity).

#define JNI_FN(name) Java_com_lumen_gearvr_GearVrActivity_##name

JNIEXPORT void JNICALL JNI_FN(nativeOnCreate)(JNIEnv *env, jclass cls, jobject activity) {
    (*env)->GetJavaVM(env, &gApp.vm);
    gApp.activity = (*env)->NewGlobalRef(env, activity);
    gApp.onVideoSurface =
        (*env)->GetMethodID(env, (*env)->GetObjectClass(env, activity), "onVideoSurface", "(Landroid/view/Surface;)V");
    gApp.display = EGL_NO_DISPLAY;
    gApp.quit = false;
    gApp.threadStarted = pthread_create(&gApp.thread, NULL, RenderThread, NULL) == 0;
}

JNIEXPORT void JNICALL JNI_FN(nativeOnResume)(JNIEnv *env, jclass cls) {
    pthread_mutex_lock(&gApp.mutex);
    gApp.resumed = true;
    pthread_cond_broadcast(&gApp.cond);
    pthread_mutex_unlock(&gApp.mutex);
}

JNIEXPORT void JNICALL JNI_FN(nativeOnPause)(JNIEnv *env, jclass cls) {
    pthread_mutex_lock(&gApp.mutex);
    gApp.resumed = false;
    pthread_cond_broadcast(&gApp.cond);
    pthread_mutex_unlock(&gApp.mutex);
}

JNIEXPORT void JNICALL JNI_FN(nativeOnSurfaceCreated)(JNIEnv *env, jclass cls, jobject surface) {
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    pthread_mutex_lock(&gApp.mutex);
    gApp.window = window;
    pthread_cond_broadcast(&gApp.cond);
    pthread_mutex_unlock(&gApp.mutex);
}

JNIEXPORT void JNICALL JNI_FN(nativeOnSurfaceDestroyed)(JNIEnv *env, jclass cls) {
    // The window must not be used after this returns: wait until the render
    // thread has left VR mode.
    pthread_mutex_lock(&gApp.mutex);
    ANativeWindow *window = gApp.window;
    gApp.window = NULL;
    pthread_cond_broadcast(&gApp.cond);
    while (gApp.ovr && gApp.threadStarted)
        pthread_cond_wait(&gApp.cond, &gApp.mutex);
    pthread_mutex_unlock(&gApp.mutex);
    if (window)
        ANativeWindow_release(window);
}

JNIEXPORT void JNICALL JNI_FN(nativeOnDestroy)(JNIEnv *env, jclass cls) {
    pthread_mutex_lock(&gApp.mutex);
    gApp.quit = true;
    pthread_cond_broadcast(&gApp.cond);
    pthread_mutex_unlock(&gApp.mutex);
    if (gApp.threadStarted)
        pthread_join(gApp.thread, NULL);
    gApp.threadStarted = false;
    (*env)->DeleteGlobalRef(env, gApp.activity);
    gApp.activity = NULL;
}

// A decoded frame was just rendered to the video surface. MediaCodec stamps
// it with presentationTimeUs * 1000, and the Java side uses the frame's
// receive time as presentationTimeUs.
JNIEXPORT void JNICALL JNI_FN(nativeSetFramePose)(JNIEnv *env, jclass cls, jlong presentationTimeUs, jfloat w,
                                                  jfloat x, jfloat y, jfloat z, jfloat tanX, jfloat tanY,
                                                  jfloat uMin, jfloat vMin, jfloat uMax, jfloat vMax) {
    // Fall back to the whole texture if the bounds don't make sense.
    if (!(uMax > uMin && vMax > vMin && uMin >= 0.0f && vMin >= 0.0f && uMax <= 1.0f && vMax <= 1.0f)) {
        uMin = vMin = 0.0f;
        uMax = vMax = 1.0f;
    }
    pthread_mutex_lock(&gApp.mutex);
    gApp.poses[gApp.nextPose] =
        (FramePose){(long long)presentationTimeUs * 1000, {w, x, y, z}, {tanX, tanY}, {uMin, vMin, uMax, vMax}};
    gApp.nextPose = (gApp.nextPose + 1) % POSE_RING;
    pthread_mutex_unlock(&gApp.mutex);
}

JNIEXPORT void JNICALL JNI_FN(nativeSetVideoSize)(JNIEnv *env, jclass cls, jint width, jint height) {
    pthread_mutex_lock(&gApp.mutex);
    gApp.videoWidth = width;
    gApp.videoHeight = height;
    pthread_mutex_unlock(&gApp.mutex);
}

// Head orientation (w, x, y, z) to send to the PC, or false when not in VR
// mode. Predicted ahead (see PeriodicWork), so the PC renders (and
// draws head-locked things like the dashboard cursor) where the head will be
// when the frame is shown; TimeWarp corrects any prediction error.
JNIEXPORT jboolean JNICALL JNI_FN(nativeGetHeadPose)(JNIEnv *env, jclass cls, jfloatArray out) {
    pthread_mutex_lock(&gApp.mutex);
    bool ok = gApp.ovr != NULL;
    ovrQuatf q = {0, 0, 0, 1};
    if (ok) {
        double now = vrapi_GetTimeInSeconds();
        double target = gApp.predictSeconds > 0 ? now + gApp.predictSeconds : 0.0; // 0 = latest sample
        ovrTracking2 tracking = vrapi_GetPredictedTracking2(gApp.ovr, target);
        q = tracking.HeadPose.Pose.Orientation;
        gApp.sent[gApp.nextSent] = (SentPose){now, {q.w, q.x, q.y, q.z}};
        gApp.nextSent = (gApp.nextSent + 1) % SENT_RING;
    }
    pthread_mutex_unlock(&gApp.mutex);
    if (ok) {
        jfloat values[4] = {q.w, q.x, q.y, q.z};
        (*env)->SetFloatArrayRegion(env, out, 0, 4, values);
    }
    return ok;
}

// What to ask SteamVR to render (width, height, horizontal FOV per eye), or
// false until VrApi is up. 1.5x VrApi's suggested eye texture (what Oculus
// apps use for sharp text; 1536 on the Galaxy S7), and its FOV plus 2 degrees
// of margin for reprojection.
JNIEXPORT jboolean JNICALL JNI_FN(nativeGetDisplayRequest)(JNIEnv *env, jclass cls, jfloatArray out) {
    pthread_mutex_lock(&gApp.mutex);
    jfloat values[3] = {1.5f * gApp.suggestedEyeWidth, 1.5f * gApp.suggestedEyeHeight, gApp.suggestedFovX + 2.0f};
    bool ok = gApp.suggestedEyeWidth > 0 && gApp.suggestedEyeHeight > 0 && gApp.suggestedFovX > 0;
    pthread_mutex_unlock(&gApp.mutex);
    if (ok)
        (*env)->SetFloatArrayRegion(env, out, 0, 3, values);
    return ok;
}

JNIEXPORT void JNICALL JNI_FN(nativeRecenter)(JNIEnv *env, jclass cls) {
    pthread_mutex_lock(&gApp.mutex);
    gApp.recenterRequested = true;
    pthread_mutex_unlock(&gApp.mutex);
}

// New status panel bitmap (ARGB_8888 = RGBA bytes); copied, uploaded on the
// render thread when it's shown.
JNIEXPORT void JNICALL JNI_FN(nativeSetStatus)(JNIEnv *env, jclass cls, jobject bitmap) {
    AndroidBitmapInfo info;
    void *pixels = NULL;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
        return;
    pthread_mutex_lock(&gApp.mutex);
    size_t size = (size_t)info.width * info.height * 4;
    if (!gApp.statusPixels || gApp.statusPixelsWidth != (int)info.width || gApp.statusPixelsHeight != (int)info.height) {
        free(gApp.statusPixels);
        gApp.statusPixels = malloc(size);
        gApp.statusPixelsWidth = (int)info.width;
        gApp.statusPixelsHeight = (int)info.height;
    }
    for (uint32_t y = 0; y < info.height; y++)
        memcpy((uint8_t *)gApp.statusPixels + (size_t)y * info.width * 4, (uint8_t *)pixels + (size_t)y * info.stride,
               (size_t)info.width * 4);
    gApp.statusDirty = true;
    pthread_mutex_unlock(&gApp.mutex);
    AndroidBitmap_unlockPixels(env, bitmap);
}
