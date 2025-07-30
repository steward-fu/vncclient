// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/input.h>

#include <wayland-server.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
  
#include <EGL/egl.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
  
#include <signal.h>
#include <rfb/rfbclient.h>

#define DEBUG       0
#define MAX_FB      2
#define LCD_W       540
#define LCD_H       960
#define SCREEN_W    960
#define SCREEN_H    540
#define TP_PATH     "/dev/input/event7"
#define KEY_PATH    "/dev/input/event1"

#if DEBUG
    #define debug(...) printf("[VNC] "__VA_ARGS__)
#else
    #define debug(...) (void)0
#endif

typedef struct {
    int x;
    int y;
    int pressure;
} touch_data;

typedef enum {
    FILTER_PIXEL = 0,
    FILTER_BLUR
} sfos_filter_t;

typedef struct {
    struct wl_shell *shell;
    struct wl_region *region;
    struct wl_display *display;
    struct wl_surface *surface;
    struct wl_registry *registry;
    struct wl_egl_window *window;
    struct wl_compositor *compositor;
    struct wl_shell_surface *shell_surface;

    struct {
        EGLConfig config;
        EGLContext context;
        EGLDisplay display;
        EGLSurface surface;

        GLuint tex;
        GLuint vert_shader;
        GLuint frag_shader;
        GLuint prog_obj;

        GLint pos;
        GLint coord;
        GLint sampler;
    } egl;
    
    struct {
        int w;
        int h;
        int bpp;
        int size;
    } info;

    struct {
        int running;
        pthread_t id[2];
    } thread;

    int init;
    int flip;
    int ready;

    uint8_t *data;
    uint32_t *pixels[MAX_FB];
} wayland;

static int alt = 0;
static int shift = 0;
static wayland wl = { 0 };
static rfbClient *cl = NULL;
static sfos_filter_t filter = 0;
static touch_data tp[10] = { 0 };

EGLint surf_cfg[] = {
    EGL_SURFACE_TYPE,
    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,
    8,
    EGL_GREEN_SIZE,
    8,
    EGL_BLUE_SIZE,
    8,
    EGL_ALPHA_SIZE,
    8,
    EGL_NONE
};

EGLint ctx_cfg[] = {
    EGL_CONTEXT_CLIENT_VERSION, 
    2, 
    EGL_NONE
};

GLfloat fb_vertices[] = {
   -1.0f,  1.0f, 0.0f, 0.0f,  0.0f,
   -1.0f, -1.0f, 0.0f, 0.0f,  1.0f,
    1.0f, -1.0f, 0.0f, 1.0f,  1.0f,
    1.0f,  1.0f, 0.0f, 1.0f,  0.0f
};

GLushort indices[] = {
    0, 1, 2, 0, 2, 3
};

static const char *vert_shader_code =
    "attribute vec4 vert_pos;                                           \n"
    "attribute vec2 vert_coord;                                         \n"
    "varying vec2 frag_coord;                                           \n"
    "void main()                                                        \n"
    "{                                                                  \n"
    "    gl_Position = vert_pos;                                        \n"
    "    frag_coord = vert_coord;                                       \n"
    "}                                                                  \n";

static const char *frag_shader_code =
    "precision mediump float;                                           \n"
    "varying vec2 frag_coord;                                           \n"
    "uniform int frag_swap_color;                                       \n"
    "uniform float frag_aspect;                                         \n"
    "uniform float frag_angle;                                          \n"
    "uniform sampler2D frag_sampler;                                    \n"
    "const vec2 HALF = vec2(0.5);                                       \n"
    "void main()                                                        \n"
    "{                                                                  \n"
    "    vec3 tex;                                                      \n"
    "    float aSin = sin(frag_angle);                                  \n"
    "    float aCos = cos(frag_angle);                                  \n"
    "    vec2 tc = frag_coord;                                          \n"
    "    mat2 rotMat = mat2(aCos, -aSin, aSin, aCos);                   \n"
    "    mat2 scaleMat = mat2(frag_aspect, 0.0, 0.0, 1.0);              \n"
    "    mat2 scaleMatInv = mat2(1.0 / frag_aspect, 0.0, 0.0, 1.0);     \n"
    "    tc -= HALF.xy;                                                 \n"
    "    tc = scaleMatInv * rotMat * scaleMat * tc;                     \n"
    "    tc += HALF.xy;                                                 \n"
    "    tex = texture2D(frag_sampler, tc).bgr;                         \n"
    "    gl_FragColor = vec4(tex, 1.0);                                 \n"
    "}                                                                  \n";

static void cb_handle(void* dat, struct wl_registry* reg, uint32_t id, const char* intf, uint32_t ver)
{
    if (strcmp(intf, "wl_compositor") == 0) {
        wl.compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 1);
    }
    else if (strcmp(intf, "wl_shell") == 0) {
        wl.shell = wl_registry_bind(reg, id, &wl_shell_interface, 1);
    }
}

static void cb_remove(void* dat, struct wl_registry* reg, uint32_t id)
{
}

static struct wl_registry_listener cb_global = {
    .global = cb_handle,
    .global_remove = cb_remove
};

static void* display_handler(void* pParam)
{
    debug("%s++\n", __func__);

    while (wl.thread.running) {
        if (wl.init && wl.ready) {
            wl_display_dispatch(wl.display);
        }
        else {
            usleep(1000);
        }
    }

    debug("%s--\n", __func__);
    return NULL;
}

static rfbKeySym key2rfbKeySym(int key, int val)
{
    switch (key) {
    case KEY_1:
        if (shift) {
            return XK_exclam;
        }

        if (alt) {
            return XK_bar;
        }

        return XK_KP_1;
    case KEY_2:
        return shift ? XK_at : XK_KP_2;
    case KEY_3:
        return shift ? XK_numbersign : XK_KP_3;
    case KEY_4:
        return shift ? XK_dollar : XK_KP_4;
    case KEY_5:
        return shift ? XK_percent : XK_KP_5;
    case KEY_6:
        return shift ? XK_asciicircum : XK_KP_6;
    case KEY_7:
        return shift ? XK_ampersand : XK_KP_7;
    case KEY_8:
        return shift ? XK_asterisk : XK_KP_8;
    case KEY_9:
        if (shift) {
            return XK_parenleft;
        }

        if (alt) {
            return XK_braceleft;
        }

        return XK_KP_9;
    case KEY_0:
        if (shift) {
            return XK_parenright;
        }

        if (alt) {
            return XK_braceright;
        }

        return XK_KP_0;
    case KEY_A:
        return shift ? XK_A : XK_a;
    case KEY_B:
        return shift ? XK_B : XK_b;
    case KEY_C:
        return shift ? XK_C : XK_c;
    case KEY_D:
        return shift ? XK_D : XK_d;
    case KEY_E:
        return shift ? XK_E : XK_e;
    case KEY_F:
        return shift ? XK_F : XK_f;
    case KEY_G:
        return shift ? XK_G : XK_g;
    case KEY_H:
        return shift ? XK_H : XK_h;
    case KEY_I:
        return shift ? XK_I : XK_i;
    case KEY_J:
        return shift ? XK_J : XK_j;
    case KEY_K:
        if (shift) {
            return XK_K;
        }

        if (alt) {
            SendPointerEvent(cl, tp[0].x, tp[0].y, val ? rfbButton1Mask : 0);
            return 0;
        }
        return XK_k;
    case KEY_L:
        if (shift) {
            return XK_L;
        }

        if (alt) {
            SendPointerEvent(cl, tp[0].x, tp[0].y, val ? rfbButton3Mask : 0);
            return 0;
        }
        return XK_l;
    case KEY_M:
        return shift ? XK_M : XK_m;
    case KEY_N:
        return shift ? XK_N : XK_n;
    case KEY_O:
        if (shift) {
            return XK_O;
        }

        if (alt) {
            return XK_bracketleft;
        }

        return XK_o;
    case KEY_P:
        if (shift) {
            return XK_P;
        }

        if (alt) {
            return XK_bracketright;
        }

        return XK_p;
    case KEY_Q:
        return shift ? XK_Q : XK_q;
    case KEY_R:
        return shift ? XK_R : XK_r;
    case KEY_S:
        return shift ? XK_S : XK_s;
    case KEY_T:
        return shift ? XK_T : XK_t;
    case KEY_U:
        return shift ? XK_U : XK_u;
    case KEY_V:
        return shift ? XK_V : XK_v;
    case KEY_W:
        return shift ? XK_W : XK_w;
    case KEY_X:
        return shift ? XK_X : XK_x;
    case KEY_Y:
        return shift ? XK_Y : XK_y;
    case KEY_Z:
        return shift ? XK_Z : XK_z;
    case KEY_UP:
        if (shift) {
            return XK_Page_Up;
        }
        return XK_Up;
    case KEY_DOWN:
        if (shift) {
            return XK_Page_Down;

        }
        return XK_Down;
    case KEY_RIGHT:
        if (shift) {
            return XK_Begin;
        }

        return XK_Right;
    case KEY_LEFT:
        if (shift) {
            return XK_End;
        }

        return XK_Left;
    case KEY_ENTER:
        return XK_KP_Enter;
    case KEY_BACKSPACE:
        return XK_BackSpace;
    case KEY_TAB:
        return XK_Tab;
    case KEY_COMMA:
        if (shift) {
            return XK_semicolon;

        }

        if (alt) {
            return XK_less;
        }

        return XK_comma;

    case KEY_DOT:
        if (shift) {
            return XK_colon;
        }

        if (alt) {
            return XK_greater;
        }

        return XK_period;

    case KEY_SPACE:
        return XK_space;
    case KEY_GRAVE:
        return shift ? XK_quotedbl : XK_apostrophe;
    case KEY_MINUS:
        if (shift) {
            return XK_underscore;
        }

        if (alt) {
            return XK_asciitilde;
        }

        return XK_minus;
    case KEY_EQUAL:
        return shift ? XK_plus : XK_equal;
    case KEY_SLASH:
        if (shift) {
            return XK_question;
        }

        if (alt) {
            return XK_backslash;
        }

        return XK_slash;
    case KEY_LEFTSHIFT:
        return XK_Control_L;
    case KEY_RIGHTALT:
        alt = val;
        break;
    case KEY_LEFTCTRL:
    case KEY_CAPSLOCK:
        shift = val;
        return XK_Shift_L;
    }

    return 0;
}

static void* input_handler(void* pParam)
{
    static int rcnt = 0;

    int r = 0;
    int tp_id = 0;
    int tp_valid = 0;
    int fd[2] = { -1, -1 };
    struct input_event ev = { 0 };
    const char *tp_path = TP_PATH;
    const char *key_path = KEY_PATH;

    debug("%s++\n", __func__);

    fd[0] = open(key_path, O_RDONLY);
    if (fd[0] < 0) {
        debug("%s, failed to open %s\n", __func__, key_path);
        return NULL;
    }

    fd[1] = open(tp_path, O_RDONLY);
    if (fd[1] < 0) {
        debug("%s, failed to open %s\n", __func__, tp_path);
        return NULL;
    }

    fcntl(fd[0], F_SETFL, O_NONBLOCK);
    fcntl(fd[1], F_SETFL, O_NONBLOCK);

    while (wl.thread.running) {
        if (read(fd[0], &ev, sizeof(struct input_event)) > 0) {
            if (ev.type == EV_KEY) {
                debug("Key, code:%d, value:%d\n", ev.code, ev.value);

                r = key2rfbKeySym(ev.code, ev.value);
                if (r > 0) {
                    debug("Send key %d=%d\n", r, !!ev.value);
                    SendKeyEvent(cl, r, !!ev.value);
                }
            }
        }

        if (read(fd[1], &ev, sizeof(struct input_event)) > 0) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_TRACKING_ID) {
                    tp_valid = 1;
                    tp_id = ev.value;
                }
                else if (ev.code == ABS_MT_POSITION_X) {
                    tp_valid = 1;
                    tp[tp_id].y = SCREEN_H - (((float)ev.value / 1000.0) * SCREEN_H);
                }
                else if (ev.code == ABS_MT_POSITION_Y) {
                    tp_valid = 1;
                    tp[tp_id].x = ((float)ev.value / 1000.0) * SCREEN_W;
                }
                else if (ev.code == ABS_MT_PRESSURE) {
                    tp_valid = 1;
                    tp[tp_id].pressure = ev.value;
                }
            }
            else if (ev.type == EV_SYN) {
                if ((ev.code == ABS_Z) && (ev.value == 0)) {
                    if (tp_valid) {
                        tp_valid = 0;
                        rcnt = 0;

                        SendPointerEvent(cl, tp[0].x, tp[0].y, !alt ? rfbButton1Mask : 0);
                        debug("Touch ID=%d, X=%d, Y=%d, Pressure=%d\n", tp_id, tp[tp_id].x, tp[tp_id].y, tp[tp_id].pressure);
                    }
                }
                else {
                    rcnt += 1;
                    if ((rcnt == 2) && !alt) {
                        SendPointerEvent(cl, tp[0].x, tp[0].y, 0);
                    }
                }
            }
        }

        usleep(1000);
    }
    close(fd[0]);
    close(fd[1]);

    debug("%s--\n", __func__);
    return NULL;
}

static void cb_ping(void* dat, struct wl_shell_surface* shell_surf, uint32_t serial)
{
    wl_shell_surface_pong(shell_surf, serial);
}

static void cb_configure(void* dat, struct wl_shell_surface* shell_surf, uint32_t edges, int32_t w, int32_t h)
{
}

static void cb_popup_done(void* dat, struct wl_shell_surface* shell_surf)
{
}

static const struct wl_shell_surface_listener cb_shell_surf = {
    cb_ping,
    cb_configure,
    cb_popup_done
};

void egl_free(void)
{
    wl.init = 0;
    wl.ready = 0;
    eglDestroySurface(wl.egl.display, wl.egl.surface);
    eglDestroyContext(wl.egl.display, wl.egl.context);
    wl_egl_window_destroy(wl.window);
    eglTerminate(wl.egl.display);

    glDeleteShader(wl.egl.vert_shader);
    glDeleteShader(wl.egl.frag_shader);
    glDeleteProgram(wl.egl.prog_obj);
}

void wl_free(void)
{
    wl.init = 0;
    wl.ready = 0;
    wl_shell_surface_destroy(wl.shell_surface);
    wl_shell_destroy(wl.shell);
    wl_surface_destroy(wl.surface);
    wl_compositor_destroy(wl.compositor);
    wl_registry_destroy(wl.registry);
    wl_display_disconnect(wl.display);

    free(wl.data);
    wl.data = NULL;
}

void wl_create(void)
{
    wl.display = wl_display_connect(NULL);
    wl.registry = wl_display_get_registry(wl.display);

    wl_registry_add_listener(wl.registry, &cb_global, NULL);
    wl_display_dispatch(wl.display);
    wl_display_roundtrip(wl.display);

    wl.surface = wl_compositor_create_surface(wl.compositor);
    wl.shell_surface = wl_shell_get_shell_surface(wl.shell, wl.surface);
    wl_shell_surface_set_toplevel(wl.shell_surface);
    wl_shell_surface_add_listener(wl.shell_surface, &cb_shell_surf, NULL);
    
    wl.region = wl_compositor_create_region(wl.compositor);
    wl_region_add(wl.region, 0, 0, LCD_W, LCD_H);
    wl_surface_set_opaque_region(wl.surface, wl.region);
    wl.window = wl_egl_window_create(wl.surface, LCD_W, LCD_H);

    debug("%s, wl.display=%p\n", __func__, wl.display);
    debug("%s, wl.registry=%p\n", __func__, wl.registry);
    debug("%s, wl.surface=%p\n", __func__, wl.surface);
    debug("%s, wl.shell=%p\n", __func__, wl.shell);
    debug("%s, wl.shell_surface=%p\n", __func__, wl.shell_surface);
    debug("%s, wl.region=%p\n", __func__, wl.region);

    wl.data = malloc(LCD_W * LCD_H * sizeof(uint32_t) * 2);
    memset(wl.data, 0, LCD_W * LCD_H * sizeof(uint32_t) * 2);
}

void egl_create(void)
{
    EGLint cnt = 0;
    EGLint major = 0;
    EGLint minor = 0;
    EGLConfig cfg = 0;

    wl.egl.display = eglGetDisplay((EGLNativeDisplayType)wl.display);
    eglInitialize(wl.egl.display, &major, &minor);
    eglGetConfigs(wl.egl.display, NULL, 0, &cnt);
    eglChooseConfig(wl.egl.display, surf_cfg, &cfg, 1, &cnt);
    wl.egl.surface = eglCreateWindowSurface(wl.egl.display, cfg, wl.window, NULL);
    wl.egl.context = eglCreateContext(wl.egl.display, cfg, EGL_NO_CONTEXT, ctx_cfg);
    eglMakeCurrent(wl.egl.display, wl.egl.surface, wl.egl.surface, wl.egl.context);

    debug("%s, egl_display=%p\n", __func__, wl.egl.display);
    debug("%s, egl_window=%p\n", __func__, wl.window);
    debug("%s, egl_surface=%p\n", __func__, wl.egl.surface);
    debug("%s, egl_context=%p\n", __func__, wl.egl.context);

    wl.egl.vert_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(wl.egl.vert_shader, 1, &vert_shader_code, NULL);
    glCompileShader(wl.egl.vert_shader);

    GLint compiled = 0;
    glGetShaderiv(wl.egl.vert_shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint len = 0;
        glGetShaderiv(wl.egl.vert_shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            char* info = malloc(sizeof(char) * len);
            glGetShaderInfoLog(wl.egl.vert_shader, len, NULL, info);
            debug("%s, failed to compile vert_shader: %s\n", __func__, info);
            free(info);
        }
    }

    wl.egl.frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(wl.egl.frag_shader, 1, &frag_shader_code, NULL);
    glCompileShader(wl.egl.frag_shader);
    
    glGetShaderiv(wl.egl.frag_shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint len = 0;
        glGetShaderiv(wl.egl.frag_shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            char* info = malloc(sizeof(char) * len);
            glGetShaderInfoLog(wl.egl.frag_shader, len, NULL, info);
            debug("%s, failed to compile frag_Shader: %s\n", __func__, info);
            free(info);
        }
    }

    wl.egl.prog_obj = glCreateProgram();
    glAttachShader(wl.egl.prog_obj, wl.egl.vert_shader);
    glAttachShader(wl.egl.prog_obj, wl.egl.frag_shader);
    glLinkProgram(wl.egl.prog_obj);
    glUseProgram(wl.egl.prog_obj);

    wl.egl.pos = glGetAttribLocation(wl.egl.prog_obj, "vert_pos");
    wl.egl.coord = glGetAttribLocation(wl.egl.prog_obj, "vert_coord");
    wl.egl.sampler = glGetUniformLocation(wl.egl.prog_obj, "frag_sampler");
    glUniform1f(glGetUniformLocation(wl.egl.prog_obj, "frag_angle"), 90 * (3.1415 * 2.0) / 360.0);
    glUniform1f(glGetUniformLocation(wl.egl.prog_obj, "frag_aspect"), 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glGenTextures(1, &wl.egl.tex);
    glBindTexture(GL_TEXTURE_2D, wl.egl.tex);

    if (filter == FILTER_PIXEL) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glViewport(0, 0, LCD_W, LCD_H);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(wl.egl.pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), fb_vertices);
    glVertexAttribPointer(wl.egl.coord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), &fb_vertices[3]);
    glEnableVertexAttribArray(wl.egl.pos);
    glEnableVertexAttribArray(wl.egl.coord);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(wl.egl.sampler, 0);

    debug("%s, texture=0x%x\n", __func__, wl.egl.tex);
    debug("%s, sampler=0x%x\n", __func__, wl.egl.sampler);
    debug("%s, position=0x%x\n", __func__, wl.egl.pos);
    debug("%s, coord=0x%x\n", __func__, wl.egl.coord);
}

static void* draw_handler(void* pParam)
{
    debug("%s++\n", __func__);
    return NULL;
}

static rfbBool vnc_resize(rfbClient *client)
{
    debug("%s()\n", __func__);
    client->width = SCREEN_W;
    client->height = SCREEN_H;
    client->frameBuffer = (uint8_t *)wl.pixels[0];
    client->format.bitsPerPixel = 32;
    client->format.redShift = 16;
    client->format.greenShift = 8;
    client->format.blueShift = 0;
    client->format.redMax = 255;
    client->format.greenMax = 255;
    client->format.blueMax = 255;
    SetFormatAndEncodings(client);

    return TRUE;
}

static void vnc_update(rfbClient *cl, int x, int y, int w, int h)
{
    wl.flip = 1;
}

static void vnc_cleanup(rfbClient *cl)
{
    if (cl) {
        rfbClientCleanup(cl);
    }
}

char* vnc_password(rfbClient *client)
{
    debug("%s()\n", __func__);

    return strdup("xxxxxxxx");
}

int main(int argc, char *argv[])
{
    int r = 0;

    debug("%s\n", __func__);

    if (argc != 2) {
        printf("Usage:\n  %s ip:port\n", argv[0]);
        return -1;
    }

    wl_create();
    egl_create();

    filter = FILTER_PIXEL;
    memset(wl.data, 0, wl.info.size * sizeof(uint32_t));
    wl.pixels[0] = (uint32_t *)wl.data;
    wl.pixels[1] = (uint32_t *)(wl.data + wl.info.size);

    wl.flip = 0;
    wl.info.w = SCREEN_W;
    wl.info.h = SCREEN_H;
    wl.info.bpp = 32;
    wl.info.size = wl.info.w * wl.info.h * (wl.info.bpp / 8);

    wl.init = 1;
    wl.ready = 1;
    wl.thread.running = 1;
    pthread_create(&wl.thread.id[0], NULL, display_handler, NULL);
    pthread_create(&wl.thread.id[1], NULL, input_handler, NULL);

    while (wl.init == 0) {
        usleep(100000);
    }

    cl = rfbGetClient(8, 3, 4);
    cl->MallocFrameBuffer = vnc_resize;
    cl->canHandleNewFBSize = TRUE;
    cl->GotFrameBufferUpdate = vnc_update;
    cl->GetPassword = vnc_password;
    cl->listenPort = LISTEN_PORT_OFFSET;
    cl->listen6Port = LISTEN_PORT_OFFSET;
    if (!rfbInitClient(cl, &argc, argv)) {
        vnc_cleanup(cl);
        return -1;
    }

    debug("running...\n");
    while (1) {
        r = WaitForMessage(cl, 500);
        if (r < 0) {
            break;
        }

        if (r) {
            if (!HandleRFBServerMessage(cl)) {
                break;
            }
        }

        if (wl.ready && wl.flip) {
            wl.flip = 0;
            glVertexAttribPointer(wl.egl.pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), fb_vertices);
            glVertexAttribPointer(wl.egl.coord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), &fb_vertices[3]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, wl.pixels[0]);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
            eglSwapBuffers(wl.egl.display, wl.egl.surface);
        }
    }
    vnc_cleanup(cl);

    debug("exit...\n");
    wl.thread.running = 0;
    pthread_join(wl.thread.id[0], NULL);
    pthread_join(wl.thread.id[1], NULL);
    egl_free();
    wl_free();

    return 0;
}

