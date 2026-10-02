/* tools/vr_probe/gdx_openxr_probe.c — M0-A falsification probe for the OpenXR GL path.
 *
 * Proves, against the real runtime, the exact chain G-Diffuser's VR layer will depend on:
 *   SDL2 window + desktop GL 4.1 core context (what libultraship's gfx_sdl2.cpp creates)
 *   -> XR_KHR_opengl_enable session with the Xlib/GLX binding
 *   -> stereo swapchains -> clear left eye dim red, right eye dim blue -> xrEndFrame
 * and prints the capability block docs/VR_ARCHITECTURE.md asks for. No game code or data.
 *
 * Build on the device (Linux):
 *   cc -O2 -o gdx_openxr_probe gdx_openxr_probe.c $(pkg-config --cflags --libs sdl2 gl x11 openxr)
 * Run:  DISPLAY=:0 ./gdx_openxr_probe [seconds]
 */
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFN_glCheckFramebufferStatus)(GLenum);

static XrInstance gInstance = XR_NULL_HANDLE;

#define XR_CHECK(call)                                                                       \
    do {                                                                                     \
        XrResult r_ = (call);                                                                \
        if (XR_FAILED(r_)) {                                                                 \
            char s_[XR_MAX_RESULT_STRING_SIZE] = "?";                                        \
            if (gInstance != XR_NULL_HANDLE) xrResultToString(gInstance, r_, s_);           \
            fprintf(stderr, "[openxr-probe] FAIL %s -> %d (%s) at line %d\n", #call, r_, s_, \
                    __LINE__);                                                               \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

static const char* GlFormatName(int64_t f) {
    switch (f) {
        case 0x8C43: return "GL_SRGB8_ALPHA8";
        case 0x8058: return "GL_RGBA8";
        case 0x881A: return "GL_RGBA16F";
        case 0x8C3A: return "GL_R11F_G11F_B10F";
        case 0x8F97: return "GL_RGBA8_SNORM";
        case 0x81A5: return "GL_DEPTH_COMPONENT16";
        case 0x81A6: return "GL_DEPTH_COMPONENT24";
        case 0x8CAC: return "GL_DEPTH_COMPONENT32F";
        case 0x88F0: return "GL_DEPTH24_STENCIL8";
        case 0x8CAD: return "GL_DEPTH32F_STENCIL8";
        default: return "other";
    }
}

static const char* StateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

static double NowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
    const double runSeconds = (argc > 1) ? atof(argv[1]) : 8.0;

    /* --- SDL window + GL 4.1 core, same attributes as libultraship gfx_sdl2.cpp --- */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[openxr-probe] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window* window = SDL_CreateWindow("gdx_openxr_probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 320,
                                          240, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == NULL) {
        fprintf(stderr, "[openxr-probe] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    if (glctx == NULL) {
        fprintf(stderr, "[openxr-probe] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);
    const char* subsystem = wm.subsystem == SDL_SYSWM_X11 ? "x11" : wm.subsystem == SDL_SYSWM_WAYLAND ? "wayland" : "other";

    Display* xDisplay = glXGetCurrentDisplay();
    GLXContext glxContext = glXGetCurrentContext();
    GLXDrawable glxDrawable = glXGetCurrentDrawable();

    printf("[sdl] video_driver=%s wm_subsystem=%s\n", SDL_GetCurrentVideoDriver(), subsystem);
    printf("[gl] vendor=%s\n[gl] renderer=%s\n[gl] version=%s\n", (const char*)glGetString(GL_VENDOR),
           (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    printf("[gl] glx_display=%p glx_context=%p glx_drawable=0x%lx\n", (void*)xDisplay, (void*)glxContext,
           (unsigned long)glxDrawable);
    if (xDisplay == NULL || glxContext == NULL) {
        fprintf(stderr, "[openxr-probe] current context is not GLX (EGL?) - Xlib binding unusable\n");
        return 1;
    }

    GLXFBConfig fbConfig = NULL;
    {
        int fbId = 0;
        glXQueryContext(xDisplay, glxContext, GLX_FBCONFIG_ID, &fbId);
        int attrs[] = { GLX_FBCONFIG_ID, fbId, None };
        int n = 0;
        GLXFBConfig* cfgs = glXChooseFBConfig(xDisplay, DefaultScreen(xDisplay), attrs, &n);
        if (cfgs != NULL && n > 0) {
            fbConfig = cfgs[0];
        }
    }

    /* --- Instance --- */
    const char* exts[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "G-Diffuser OpenXR probe");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "G-Diffuser");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    XR_CHECK(xrCreateInstance(&ici, &gInstance));

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    XR_CHECK(xrGetInstanceProperties(gInstance, &ip));
    printf("[openxr]\nruntime=%s\nversion=%u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
           XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId;
    XR_CHECK(xrGetSystem(gInstance, &sgi, &systemId));
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    XR_CHECK(xrGetSystemProperties(gInstance, systemId, &sp));
    printf("system=%s vendor=%u max_swapchain=%ux%u max_layers=%u orientation_tracking=%u position_tracking=%u\n",
           sp.systemName, sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
           sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount,
           sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking);

    /* --- View configuration --- */
    uint32_t vcCount = 0;
    XR_CHECK(xrEnumerateViewConfigurations(gInstance, systemId, 0, &vcCount, NULL));
    XrViewConfigurationType vcs[8];
    XR_CHECK(xrEnumerateViewConfigurations(gInstance, systemId, vcCount > 8 ? 8 : vcCount, &vcCount, vcs));
    printf("view_configs=");
    for (uint32_t i = 0; i < vcCount && i < 8; i++) {
        printf("%s%d", i ? "," : "", (int)vcs[i]);
    }
    printf(" (2=PRIMARY_STEREO)\n");

    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(gInstance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &viewCount, vcv));
    for (uint32_t i = 0; i < viewCount; i++) {
        printf("eye%u recommended=%ux%u max=%ux%u samples=%u/%u\n", i, vcv[i].recommendedImageRectWidth,
               vcv[i].recommendedImageRectHeight, vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight,
               vcv[i].recommendedSwapchainSampleCount, vcv[i].maxSwapchainSampleCount);
    }

    uint32_t blendCount = 0;
    XrEnvironmentBlendMode blends[4];
    XR_CHECK(xrEnumerateEnvironmentBlendModes(gInstance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4,
                                              &blendCount, blends));
    printf("blend_modes=");
    for (uint32_t i = 0; i < blendCount; i++) {
        printf("%s%d", i ? "," : "", (int)blends[i]);
    }
    printf(" (1=OPAQUE)\n");

    /* --- GL requirements --- */
    PFN_xrGetOpenGLGraphicsRequirementsKHR getGlReqs = NULL;
    XR_CHECK(xrGetInstanceProcAddr(gInstance, "xrGetOpenGLGraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction*)&getGlReqs));
    XrGraphicsRequirementsOpenGLKHR glReqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    XR_CHECK(getGlReqs(gInstance, systemId, &glReqs));
    printf("gl_requirements min=%u.%u max=%u.%u\n", XR_VERSION_MAJOR(glReqs.minApiVersionSupported),
           XR_VERSION_MINOR(glReqs.minApiVersionSupported), XR_VERSION_MAJOR(glReqs.maxApiVersionSupported),
           XR_VERSION_MINOR(glReqs.maxApiVersionSupported));

    /* --- Session (Xlib/GLX binding) --- */
    XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
    binding.xDisplay = xDisplay;
    binding.visualid = 0;
    binding.glxFBConfig = fbConfig;
    binding.glxDrawable = glxDrawable;
    binding.glxContext = glxContext;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = systemId;
    XrSession session;
    XR_CHECK(xrCreateSession(gInstance, &sci, &session));
    printf("graphics_binding=opengl_xlib session=created\n");

    uint32_t fmtCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &fmtCount, NULL));
    int64_t fmts[64];
    XR_CHECK(xrEnumerateSwapchainFormats(session, fmtCount > 64 ? 64 : fmtCount, &fmtCount, fmts));
    printf("swapchain_formats=");
    int64_t chosen = 0;
    for (uint32_t i = 0; i < fmtCount && i < 64; i++) {
        printf("%s0x%llx(%s)", i ? "," : "", (long long)fmts[i], GlFormatName(fmts[i]));
        if (chosen == 0 && (fmts[i] == 0x8C43 || fmts[i] == 0x8058)) {
            chosen = fmts[i];
        }
    }
    printf("\n");
    if (chosen == 0) {
        chosen = fmts[0];
    }
    printf("swapchain_format=%s\n", GlFormatName(chosen));

    uint32_t spaceCount = 0;
    XrReferenceSpaceType spaces[8];
    XR_CHECK(xrEnumerateReferenceSpaces(session, 8, &spaceCount, spaces));
    printf("reference_spaces=");
    for (uint32_t i = 0; i < spaceCount; i++) {
        printf("%s%d", i ? "," : "", (int)spaces[i]);
    }
    printf(" (1=VIEW 2=LOCAL 3=STAGE)\n[/openxr]\n");

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace localSpace;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &localSpace));

    /* --- Swapchains --- */
    XrSwapchain swapchains[2];
    XrSwapchainImageOpenGLKHR images[2][8];
    uint32_t imageCounts[2];
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        scci.format = chosen;
        scci.sampleCount = 1;
        scci.width = vcv[e].recommendedImageRectWidth;
        scci.height = vcv[e].recommendedImageRectHeight;
        scci.faceCount = 1;
        scci.arraySize = 1;
        scci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(session, &scci, &swapchains[e]));
        for (int i = 0; i < 8; i++) {
            images[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            images[e][i].next = NULL;
        }
        XR_CHECK(xrEnumerateSwapchainImages(swapchains[e], 8, &imageCounts[e],
                                            (XrSwapchainImageBaseHeader*)images[e]));
        printf("[swapchain] eye%d %ux%u images=%u\n", e, scci.width, scci.height, imageCounts[e]);
    }

    PFN_glGenFramebuffers glGenFramebuffers_ = (PFN_glGenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
    PFN_glBindFramebuffer glBindFramebuffer_ = (PFN_glBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    PFN_glFramebufferTexture2D glFramebufferTexture2D_ =
        (PFN_glFramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    PFN_glCheckFramebufferStatus glCheckFramebufferStatus_ =
        (PFN_glCheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    GLuint fbo;
    glGenFramebuffers_(1, &fbo);

    /* --- Frame loop --- */
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    int running = 0;
    int quit = 0;
    long framesWaited = 0, framesRendered = 0, framesLocated = 0;
    XrTime firstPdt = 0, lastPdt = 0, lastPeriod = 0;
    double startWall = 0.0;
    double sumBeginToEndMs = 0.0;
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

    while (!quit) {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(gInstance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrEventDataSessionStateChanged* sc = (const XrEventDataSessionStateChanged*)&ev;
                state = sc->state;
                printf("[state] %s at t=%.3fs\n", StateName(state), startWall > 0 ? NowSeconds() - startWall : 0.0);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xrBeginSession(session, &sbi));
                    running = 1;
                    startWall = NowSeconds();
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(xrEndSession(session));
                    running = 0;
                    quit = 1;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = 1;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = 1;
            }
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        }
        if (!running) {
            SDL_Delay(10);
            if (startWall == 0.0) {
                static double waitStart = 0.0;
                if (waitStart == 0.0) waitStart = NowSeconds();
                if (NowSeconds() - waitStart > 15.0) {
                    printf("[openxr-probe] session never became READY within 15s\n");
                    break;
                }
            }
            continue;
        }
        if (NowSeconds() - startWall > runSeconds) {
            XR_CHECK(xrRequestExitSession(session));
            running = 0; /* keep polling until STOPPING */
            /* fallthrough loop continues to drain events */
            while (!quit) {
                XrEventDataBuffer ev2 = { XR_TYPE_EVENT_DATA_BUFFER };
                while (xrPollEvent(gInstance, &ev2) == XR_SUCCESS) {
                    if (ev2.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                        const XrEventDataSessionStateChanged* sc = (const XrEventDataSessionStateChanged*)&ev2;
                        printf("[state] %s\n", StateName(sc->state));
                        if (sc->state == XR_SESSION_STATE_STOPPING) {
                            XR_CHECK(xrEndSession(session));
                        } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_IDLE) {
                            quit = 1;
                        }
                    }
                    ev2.type = XR_TYPE_EVENT_DATA_BUFFER;
                }
                SDL_Delay(5);
            }
            break;
        }

        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XR_CHECK(xrWaitFrame(session, &fwi, &fs));
        framesWaited++;
        if (firstPdt == 0) firstPdt = fs.predictedDisplayTime;
        lastPdt = fs.predictedDisplayTime;
        lastPeriod = fs.predictedDisplayPeriod;

        const double t0 = NowSeconds();
        XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
        XR_CHECK(xrBeginFrame(session, &fbi));

        XrCompositionLayerProjectionView projViews[2];
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        const XrCompositionLayerBaseHeader* layers[1];
        uint32_t layerCount = 0;

        if (fs.shouldRender) {
            XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = localSpace;
            XrViewState vs = { XR_TYPE_VIEW_STATE };
            uint32_t located = 0;
            XR_CHECK(xrLocateViews(session, &vli, &vs, 2, &located, views));
            if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
                framesLocated++;
            }
            for (int e = 0; e < 2; e++) {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XR_CHECK(xrAcquireSwapchainImage(swapchains[e], &ai, &idx));
                XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                XR_CHECK(xrWaitSwapchainImage(swapchains[e], &wi));

                glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[e][idx].image, 0);
                if (framesRendered == 0 && glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    printf("[openxr-probe] eye%d FBO incomplete\n", e);
                }
                glViewport(0, 0, vcv[e].recommendedImageRectWidth, vcv[e].recommendedImageRectHeight);
                if (e == 0) {
                    glClearColor(0.18f, 0.04f, 0.04f, 1.0f); /* dim: full-intensity fills are painful in-headset */
                } else {
                    glClearColor(0.04f, 0.06f, 0.18f, 1.0f);
                }
                glClear(GL_COLOR_BUFFER_BIT);
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);

                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                XR_CHECK(xrReleaseSwapchainImage(swapchains[e], &ri));

                projViews[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                projViews[e].pose = views[e].pose;
                projViews[e].fov = views[e].fov;
                projViews[e].subImage.swapchain = swapchains[e];
                projViews[e].subImage.imageRect.extent.width = (int32_t)vcv[e].recommendedImageRectWidth;
                projViews[e].subImage.imageRect.extent.height = (int32_t)vcv[e].recommendedImageRectHeight;
            }
            layer.space = localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
            layerCount = 1;
            framesRendered++;
            if (framesRendered == 1) {
                printf("[views] eye0 fov L%.3f R%.3f U%.3f D%.3f pos(%.4f,%.4f,%.4f)\n", views[0].fov.angleLeft,
                       views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown, views[0].pose.position.x,
                       views[0].pose.position.y, views[0].pose.position.z);
                printf("[views] eye1 fov L%.3f R%.3f U%.3f D%.3f pos(%.4f,%.4f,%.4f)\n", views[1].fov.angleLeft,
                       views[1].fov.angleRight, views[1].fov.angleUp, views[1].fov.angleDown, views[1].pose.position.x,
                       views[1].pose.position.y, views[1].pose.position.z);
            }
        }

        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layerCount;
        fei.layers = layers;
        XR_CHECK(xrEndFrame(session, &fei));
        sumBeginToEndMs += (NowSeconds() - t0) * 1000.0;
    }

    const double spanS = (double)(lastPdt - firstPdt) * 1e-9;
    printf("[frames] waited=%ld rendered=%ld pose_valid=%ld display_period=%.3fms rate=%.2fHz begin_to_end_avg=%.3fms\n",
           framesWaited, framesRendered, framesLocated, (double)lastPeriod * 1e-6,
           (spanS > 0 && framesWaited > 1) ? (double)(framesWaited - 1) / spanS : 0.0,
           framesWaited ? sumBeginToEndMs / (double)framesWaited : 0.0);

    for (int e = 0; e < 2; e++) {
        xrDestroySwapchain(swapchains[e]);
    }
    xrDestroySpace(localSpace);
    xrDestroySession(session);
    xrDestroyInstance(gInstance);
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("[openxr-probe] %s\n", framesRendered > 0 ? "PASS" : "NO FRAMES RENDERED");
    return framesRendered > 0 ? 0 : 2;
}
