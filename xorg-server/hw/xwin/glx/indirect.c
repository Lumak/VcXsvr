/*
 * File: indirect.c
 * Purpose: A GLX implementation that uses Windows OpenGL library
 *
 * Authors: Alexander Gottwald
 *          Jon TURNEY
 *
 * Copyright (c) Jon TURNEY 2009
 * Copyright (c) Alexander Gottwald 2004
 *
 * Portions of this file are copied from GL/apple/indirect.c,
 * which contains the following copyright:
 *
 * Copyright (c) 2007, 2008, 2009 Apple Inc.
 * Copyright (c) 2004 Torrey T. Lyons. All Rights Reserved.
 * Copyright (c) 2002 Greg Parker. All Rights Reserved.
 *
 * Portions of this file are copied from Mesa's xf86glx.c,
 * which contains the following copyright:
 *
 * Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE ABOVE LISTED COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
  TODO:
  - hook up remaining unimplemented extensions
  - research what guarantees glXWaitX, glXWaitGL are supposed to offer, and implement then
    using GdiFlush and/or glFinish
  - pbuffer clobbering: we don't get async notification, but can we arrange to emit the
    event when we notice it's been clobbered? at the very least, check if it's been clobbered
    before using it?
  - XGetImage() doesn't work on pixmaps; need to do more work to make the format and location
    of the native pixmap compatible
  - implement GLX_EXT_texture_from_pixmap in terms of WGL_ARB_render_texture
    (not quite straightforward as we will have to create a pbuffer and copy the pixmap texture
     into it)
*/

/*
  Assumptions:
  - the __GLXConfig * we get handed back ones we are made (so we can extend the structure
    with privates) and never get created inside the GLX core
*/

/*
  MSDN clarifications:

  It says SetPixelFormat()'s PIXELFORMATDESCRIPTOR pointer argument has no effect
  except on metafiles, this seems to mean that as it's ok to supply NULL if the DC
  is not for a metafile

  wglMakeCurrent ignores the hdc if hglrc is NULL, so wglMakeCurrent(NULL, NULL)
  is used to make no context current

*/

#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif

#include "glwindows.h"
#include <glx/glheader.h>
#include <glx/glxserver.h>
#include <glx/glxutil.h>
#include <glx/extension_string.h>
#include <glx/glxext.h>
#include <GL/glxtokens.h>
#include <glx/glapitable.h>
#include <glx/glapi.h>

#include <winpriv.h>
#include <wgl_ext_api.h>
#include "win.h"
#include <winmsg.h>
#include <winglobals.h>

#define NUM_ELEMENTS(x) (sizeof(x)/ sizeof(x[1]))

/* Not yet in w32api */
#ifndef PFD_SUPPORT_DIRECTDRAW
#define PFD_SUPPORT_DIRECTDRAW   0x00002000
#endif
#ifndef PFD_DIRECT3D_ACCELERATED
#define PFD_DIRECT3D_ACCELERATED 0x00004000
#endif
#ifndef PFD_SUPPORT_COMPOSITION
#define PFD_SUPPORT_COMPOSITION  0x00008000
#endif

/* ---------------------------------------------------------------------- */
/*
 *   structure definitions
 */

typedef struct __GLXWinContext __GLXWinContext;
typedef struct __GLXWinDrawable __GLXWinDrawable;
typedef struct __GLXWinScreen glxWinScreen;
typedef struct __GLXWinConfig GLXWinConfig;

struct __GLXWinContext {
    __GLXcontext base;
    HGLRC ctx;                  /* Windows GL Context */
    HDC hDC;                           /* Windows device context */
    HDC hreadDC;                     /* Windows device read context */
    __GLXWinContext *shareContext;      /* Context with which we will share display lists and textures */
    HWND hwnd;                  /* For detecting when HWND has changed */
    HWND hreadwnd;
    struct _glapi_table *Dispatch;
};

struct __GLXWinDrawable {
    __GLXdrawable base;
    __GLXWinContext *drawContext;

    /* If this drawable is GLX_DRAWABLE_PBUFFER */
    HPBUFFERARB hPbuffer;

    /* If this drawable is GLX_DRAWABLE_PIXMAP */
    HDC dibDC;
    HBITMAP hDIB;
    HBITMAP hOldDIB;            /* original DIB for DC */
    void *pOldBits;             /* original pBits for this drawable's pixmap */
};

struct __GLXWinScreen {
    __GLXscreen base;

    Bool has_WGL_ARB_multisample;
    Bool has_WGL_ARB_pixel_format;
    Bool has_WGL_ARB_pbuffer;
    Bool has_WGL_ARB_render_texture;
    Bool has_WGL_ARB_make_current_read;

    /* wrapped screen functions */
    RealizeWindowProcPtr RealizeWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;
    DestroyWindowProcPtr DestroyWindow;
    CopyWindowProcPtr CopyWindow;
    PositionWindowProcPtr PositionWindow;
};

struct __GLXWinConfig {
    __GLXconfig base;
    int pixelFormatIndex;
};

/* ---------------------------------------------------------------------- */
/*
 * Various debug helpers
 */

#ifdef _DEBUG
void GLWIN_DEBUG_HWND(HWND hwnd)
{
    if (glxWinDebugSettings.dumpHWND) {
        char buffer[1024];
        RECT Rect;
        HDC hDc=GetDC(hwnd);

        if (GetWindowText(hwnd, buffer, sizeof(buffer))==0) *buffer=0;
        GetWindowRect(hwnd,&Rect);

        GLWIN_DEBUG_MSG("Got HWND %p (hdc %p) for window '%s' (%d,%d,%d,%d)", hwnd, hDc, buffer, Rect.left, Rect.top, Rect.right, Rect.bottom);
        ReleaseDC(hwnd,hDc);
    }
}

void GLWIN_HDC_DEBUG_MSG(const char *Message, HDC hDc, HWND hwnd)
{
    char buffer[1024];
    RECT Rect;

    if (GetWindowText(hwnd, buffer, sizeof(buffer))==0) *buffer=0;
    GetWindowRect(hwnd,&Rect);

    GLWIN_DEBUG_MSG("Got HDC %p (hwnd %p) for window '%s' (%d,%d,%d,%d)", hDc, hwnd, buffer, Rect.left, Rect.top, Rect.right, Rect.bottom);

}

glxWinDebugSettingsRec glxWinDebugSettings = { 0, 0, 0, 0, 0, 0 };

static void
glxWinInitDebugSettings(void)
{
    char *envptr;

    envptr = getenv("GLWIN_ENABLE_DEBUG");
    if (envptr != NULL)
        glxWinDebugSettings.enableDebug = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_ENABLE_TRACE");
    if (envptr != NULL)
        glxWinDebugSettings.enableTrace = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_DUMP_PFD");
    if (envptr != NULL)
        glxWinDebugSettings.dumpPFD = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_DUMP_HWND");
    if (envptr != NULL)
        glxWinDebugSettings.dumpHWND = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_DUMP_DC");
    if (envptr != NULL)
        glxWinDebugSettings.dumpDC = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_ENABLE_GLCALL_TRACE");
    if (envptr != NULL)
        glxWinDebugSettings.enableGLcallTrace = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_ENABLE_WGLCALL_TRACE");
    if (envptr != NULL)
        glxWinDebugSettings.enableWGLcallTrace = (atoi(envptr) == 1);

    envptr = getenv("GLWIN_DEBUG_ALL");
    if (envptr != NULL) {
        glxWinDebugSettings.enableDebug = 1;
        glxWinDebugSettings.enableTrace = 1;
        glxWinDebugSettings.dumpPFD = 1;
        glxWinDebugSettings.dumpHWND = 1;
        glxWinDebugSettings.dumpDC = 1;
        glxWinDebugSettings.enableGLcallTrace = 1;
        glxWinDebugSettings.enableWGLcallTrace = 1;
    }
}
#endif

static
const char *
glxWinErrorMessage(void)
{
    static char errorbuffer[1024];
    unsigned int last_error = GetLastError();

    if (!FormatMessage
        (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
         FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, last_error, 0,
         (LPTSTR) &errorbuffer, sizeof(errorbuffer), NULL)) {
        snprintf(errorbuffer, sizeof(errorbuffer), "Unknown error");
    }

    if ((errorbuffer[strlen(errorbuffer) - 1] == '\n') ||
        (errorbuffer[strlen(errorbuffer) - 1] == '\r'))
        errorbuffer[strlen(errorbuffer) - 1] = 0;

    sprintf(errorbuffer + strlen(errorbuffer), " (%08x)", last_error);

    return errorbuffer;
}

static void pfdOut(const PIXELFORMATDESCRIPTOR * pfd);

#ifdef _DEBUG

#define DUMP_PFD_FLAG(flag) \
    if (pfd->dwFlags & flag) { \
        ErrorF("%s%s", pipesym, #flag); \
        pipesym = " | "; \
    }

static void
pfdOut(const PIXELFORMATDESCRIPTOR * pfd)
{
    const char *pipesym = "";   /* will be set after first flag dump */

    ErrorF("PIXELFORMATDESCRIPTOR:\n");
    ErrorF("nSize = %u\n", pfd->nSize);
    ErrorF("nVersion = %u\n", pfd->nVersion);
    ErrorF("dwFlags = %u = {", (unsigned int)pfd->dwFlags);
    DUMP_PFD_FLAG(PFD_DOUBLEBUFFER);
    DUMP_PFD_FLAG(PFD_STEREO);
    DUMP_PFD_FLAG(PFD_DRAW_TO_WINDOW);
    DUMP_PFD_FLAG(PFD_DRAW_TO_BITMAP);
    DUMP_PFD_FLAG(PFD_SUPPORT_GDI);
    DUMP_PFD_FLAG(PFD_SUPPORT_OPENGL);
    DUMP_PFD_FLAG(PFD_GENERIC_FORMAT);
    DUMP_PFD_FLAG(PFD_NEED_PALETTE);
    DUMP_PFD_FLAG(PFD_NEED_SYSTEM_PALETTE);
    DUMP_PFD_FLAG(PFD_SWAP_EXCHANGE);
    DUMP_PFD_FLAG(PFD_SWAP_COPY);
    DUMP_PFD_FLAG(PFD_SWAP_LAYER_BUFFERS);
    DUMP_PFD_FLAG(PFD_GENERIC_ACCELERATED);
    DUMP_PFD_FLAG(PFD_SUPPORT_DIRECTDRAW);
    DUMP_PFD_FLAG(PFD_DIRECT3D_ACCELERATED);
    DUMP_PFD_FLAG(PFD_SUPPORT_COMPOSITION);
    DUMP_PFD_FLAG(PFD_DEPTH_DONTCARE);
    DUMP_PFD_FLAG(PFD_DOUBLEBUFFER_DONTCARE);
    DUMP_PFD_FLAG(PFD_STEREO_DONTCARE);
    ErrorF("}\n");

    ErrorF("iPixelType = %hu = %s\n", pfd->iPixelType,
           (pfd->iPixelType ==
            PFD_TYPE_RGBA ? "PFD_TYPE_RGBA" : "PFD_TYPE_COLORINDEX"));
    ErrorF("cColorBits = %hhu\n", pfd->cColorBits);
    ErrorF("cRedBits = %hhu\n", pfd->cRedBits);
    ErrorF("cRedShift = %hhu\n", pfd->cRedShift);
    ErrorF("cGreenBits = %hhu\n", pfd->cGreenBits);
    ErrorF("cGreenShift = %hhu\n", pfd->cGreenShift);
    ErrorF("cBlueBits = %hhu\n", pfd->cBlueBits);
    ErrorF("cBlueShift = %hhu\n", pfd->cBlueShift);
    ErrorF("cAlphaBits = %hhu\n", pfd->cAlphaBits);
    ErrorF("cAlphaShift = %hhu\n", pfd->cAlphaShift);
    ErrorF("cAccumBits = %hhu\n", pfd->cAccumBits);
    ErrorF("cAccumRedBits = %hhu\n", pfd->cAccumRedBits);
    ErrorF("cAccumGreenBits = %hhu\n", pfd->cAccumGreenBits);
    ErrorF("cAccumBlueBits = %hhu\n", pfd->cAccumBlueBits);
    ErrorF("cAccumAlphaBits = %hhu\n", pfd->cAccumAlphaBits);
    ErrorF("cDepthBits = %hhu\n", pfd->cDepthBits);
    ErrorF("cStencilBits = %hhu\n", pfd->cStencilBits);
    ErrorF("cAuxBuffers = %hhu\n", pfd->cAuxBuffers);
    ErrorF("iLayerType = %hhu\n", pfd->iLayerType);
    ErrorF("bReserved = %hhu\n", pfd->bReserved);
    ErrorF("dwLayerMask = %u\n", (unsigned int)pfd->dwLayerMask);
    ErrorF("dwVisibleMask = %u\n", (unsigned int)pfd->dwVisibleMask);
    ErrorF("dwDamageMask = %u\n", (unsigned int)pfd->dwDamageMask);
    ErrorF("\n");
}

static const char *
visual_class_name(int cls)
{
    switch (cls) {
    case GLX_STATIC_COLOR:
        return "StaticColor";
    case GLX_PSEUDO_COLOR:
        return "PseudoColor";
    case GLX_STATIC_GRAY:
        return "StaticGray";
    case GLX_GRAY_SCALE:
        return "GrayScale";
    case GLX_TRUE_COLOR:
        return "TrueColor";
    case GLX_DIRECT_COLOR:
        return "DirectColor";
    default:
        return "-none-";
    }
}

static const char *
swap_method_name(int mthd)
{
    switch (mthd) {
    case GLX_SWAP_EXCHANGE_OML:
        return "xchg";
    case GLX_SWAP_COPY_OML:
        return "copy";
    case GLX_SWAP_UNDEFINED_OML:
        return "    ";
    default:
        return "????";
    }
}

static void
fbConfigsDump(unsigned int n, __GLXconfig * c)
{
    LogMessage(X_INFO, "%d fbConfigs\n", n);

    if (g_iLogVerbose < 3)
        return;
    ErrorF("%d fbConfigs\n", n);
    ErrorF
        ("pxf vis  fb                      render         Ste                     aux    accum        MS    drawable             Group/\n");
    ErrorF
        ("idx  ID  ID VisualType Depth Lvl RGB CI DB Swap reo  R  G  B  A   Z  S  buf AR AG AB AA  bufs num  W P Pb  Float Trans Caveat\n");
    ErrorF
        ("-----------------------------------------------------------------------------------------------------------------------------\n");

    while (c != NULL) {
        unsigned int i = ((GLXWinConfig *) c)->pixelFormatIndex;

        ErrorF("%3d %3x %3x "
               "%-11s"
               " %3d %3d   %s   %s  %s %s  %s  "
               "%2d %2d %2d %2d  "
               "%2d %2d  "
               "%2d  "
               "%2d %2d %2d %2d"
               "   %2d   %2d"
               "  %s %s %s "
               "    %s   "
               "  %s   "
               "  %d %s"
               "\n",
               i, c->visualID, c->fbconfigID,
               visual_class_name(c->visualType),
               c->rgbBits ? c->rgbBits : c->indexBits,
               c->level,
               (c->renderType & GLX_RGBA_BIT) ? "y" : ".",
               (c->renderType & GLX_COLOR_INDEX_BIT) ? "y" : ".",
               c->doubleBufferMode ? "y" : ".",
               swap_method_name(c->swapMethod),
               c->stereoMode ? "y" : ".",
               c->redBits, c->greenBits, c->blueBits, c->alphaBits,
               c->depthBits, c->stencilBits,
               c->numAuxBuffers,
               c->accumRedBits, c->accumGreenBits, c->accumBlueBits,
               c->accumAlphaBits, c->sampleBuffers, c->samples,
               (c->drawableType & GLX_WINDOW_BIT) ? "y" : ".",
               (c->drawableType & GLX_PIXMAP_BIT) ? "y" : ".",
               (c->drawableType & GLX_PBUFFER_BIT) ? "y" : ".",
               (c->renderType & (GLX_RGBA_FLOAT_BIT_ARB |
                   GLX_RGBA_UNSIGNED_FLOAT_BIT_EXT)) ? "y" : ".",
               (c->transparentPixel != GLX_NONE_EXT) ? "y" : ".",
               c->visualSelectGroup,
               (c->visualRating == GLX_SLOW_VISUAL_EXT) ? "*" : " ");

        c = c->next;
    }
}
#endif

/* ---------------------------------------------------------------------- */
/*
 * Forward declarations
 */

static __GLXscreen *glxWinScreenProbe(ScreenPtr pScreen);
static __GLXcontext *glxWinCreateContext(__GLXscreen * screen,
                                         __GLXconfig * modes,
                                         __GLXcontext * baseShareContext,
                                         unsigned num_attribs,
                                         const uint32_t * attribs, int *error);
static __GLXdrawable *glxWinCreateDrawable(ClientPtr client,
                                           __GLXscreen * screen,
                                           DrawablePtr pDraw,
                                           XID drawId,
                                           int type,
                                           XID glxDrawId, __GLXconfig * conf);

static Bool glxWinRealizeWindow(WindowPtr pWin);
static Bool glxWinUnrealizeWindow(WindowPtr pWin);
static Bool glxWinDestroyWindow(WindowPtr pWin);
static void glxWinCopyWindow(WindowPtr pWindow, DDXPointRec ptOldOrg,
                             RegionPtr prgnSrc);
static Bool glxWinPositionWindow(WindowPtr pWindow, int x, int y);

static HDC glxWinMakeDC(__GLXWinContext * gc, __GLXWinDrawable * draw,
                        HWND * hwnd);
static void glxWinReleaseDC(HWND hwnd, HDC hdc, __GLXWinDrawable * draw);

static void glxWinCreateConfigs(HDC dc, glxWinScreen * screen);
static void glxWinCreateConfigsExt(HDC hdc, glxWinScreen * screen);
static int fbConfigToPixelFormat(__GLXconfig * mode,
                                 PIXELFORMATDESCRIPTOR * pfdret,
                                 int drawableTypeOverride);
static int fbConfigToPixelFormatIndex(HDC hdc, __GLXconfig * mode,
                                      int drawableTypeOverride,
                                      glxWinScreen * winScreen);

/* ---------------------------------------------------------------------- */
/*
 * The GLX provider
 */

__GLXprovider __glXWGLProvider = {
    glxWinScreenProbe,
    "Win32 native WGL",
    NULL
};

void
glxWinPushNativeProvider(void)
{
  if (g_fNativeGl)
      GlxPushProvider(&__glXWGLProvider);
}

/* ---------------------------------------------------------------------- */
/*
 * Screen functions
 */

static void
glxWinScreenDestroy(__GLXscreen * screen)
{
    GLWIN_DEBUG_MSG("glxWinScreenDestroy(%p)", screen);
    __glXScreenDestroy(screen);
    free(screen);
}

static int
glxWinScreenSwapInterval(__GLXdrawable * drawable, int interval)
{
    BOOL ret = wglSwapIntervalEXTWrapper(interval);

    if (!ret) {
        ErrorF("wglSwapIntervalEXT interval %d failed:%s\n", interval,
               glxWinErrorMessage());
    }
    return ret;
}

static LRESULT CALLBACK GlxWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg== WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    else
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

/*
  Report the extensions split and formatted to avoid overflowing a line
 */
static void
glxLogExtensions(const char *prefix, const char *extensions)
{
    int length = 0;
    const char *strl;
    char *str = strdup(extensions);

    if (str == NULL) {
        ErrorF("glxLogExtensions: xalloc error\n");
        return;
    }

    strl = strtok(str, " ");
    if (strl == NULL)
        strl = "";
    winDebug("%s%s", prefix, strl);

    length = strlen(prefix) + strlen(strl);

    while (1) {
        strl = strtok(NULL, " ");
        if (strl == NULL)
            break;

        if (length + strlen(strl) + 1 > 120) {
            winDebug("\n");
            winDebug("%s", prefix);
            length = strlen(prefix);
        }
        else {
            winDebug(" ");
            length++;
        }

        winDebug("%s", strl);
        length = length + strlen(strl);
    }

    winDebug("\n");

    free(str);
}

/* This is called by GlxExtensionInit() asking the GLX provider if it can handle the screen... */
static __GLXscreen *
glxWinScreenProbe(ScreenPtr pScreen)
{
    glxWinScreen *screen;
    const char *gl_extensions;
    const char *gl_renderer;
    const char *wgl_extensions;
    HWND hwnd;
    HDC hdc;
    HGLRC hglrc;

    GLWIN_DEBUG_MSG("glxWinScreenProbe");

#ifdef _DEBUG
    glxWinInitDebugSettings();
#endif

    if (pScreen == NULL)
        return NULL;

    if (!winCheckScreenAiglxIsSupported(pScreen)) {
        LogMessage(X_ERROR,
                   "AIGLX: No native OpenGL in modes with a root window\n");
        return NULL;
    }

    screen = calloc(1, sizeof(glxWinScreen));

    if (NULL == screen)
        return NULL;

    /* Dump out some useful information about the native renderer */

    // create window class
    {
        static wATOM glTestWndClass = 0;

        if (glTestWndClass == 0) {
            WNDCLASSEX wc;
            glTestWndClass=1;
            wc.cbSize = sizeof(WNDCLASSEX);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC ;
            wc.lpfnWndProc = GlxWindowProc;
            wc.cbClsExtra = 0;
            wc.cbWndExtra = 0;
            wc.hInstance = g_hInstance;
            wc.hIcon = 0;
            wc.hCursor = 0;
            wc.hbrBackground = 0;
            wc.lpszMenuName = NULL;
            wc.lpszClassName = WIN_GL_WINDOW_CLASS;
            wc.hIconSm = 0;
            RegisterClassEx(&wc);
        }
    }

    // create an invisible window for a scratch DC
    hwnd = CreateWindowExA(0,
                           WIN_GL_WINDOW_CLASS,
                           "XWin GL Renderer Capabilities Test Window",
                           0, 0, 0, 0, 0, NULL, NULL, g_hInstance,
                           NULL);
    if (hwnd == NULL)
        LogMessage(X_ERROR,
                   "AIGLX: Couldn't create a window for render capabilities testing\n");

    hdc = GetDC(hwnd);

    // we must set a pixel format before we can create a context, just use the first one...
    SetPixelFormat(hdc, 1, NULL);
    hglrc = wglCreateContext(hdc);
    if (!wglMakeCurrent(hdc, hglrc)) {
        DWORD ErrorCode=GetLastError();
        ErrorF("wglMakeCurrent error: %x dc %p ctx %p\n", ErrorCode,hdc,hglrc);
    }

    // initialize wgl extension proc pointers (don't call them before here...)
    // (but we need to have a current context for them to be resolvable)
    wglResolveExtensionProcs();

    winDebug("GL_VERSION:     %s\n", glGetStringWrapperNonstatic(GL_VERSION));
    winDebug("GL_VENDOR:      %s\n", glGetStringWrapperNonstatic(GL_VENDOR));
    gl_renderer = (const char *) glGetStringWrapperNonstatic(GL_RENDERER);
    winDebug("GL_RENDERER:    %s\n", gl_renderer);
    gl_extensions = (const char *) glGetStringWrapperNonstatic(GL_EXTENSIONS);
    wgl_extensions = wglGetExtensionsStringARBWrapper(hdc);
    if (!wgl_extensions)
        wgl_extensions = "";

    if (g_iLogVerbose >= 3) {
        glxLogExtensions("GL_EXTENSIONS:  ", gl_extensions);
        glxLogExtensions("WGL_EXTENSIONS: ", wgl_extensions);
    }

    if (strcasecmp(gl_renderer, "GDI Generic") == 0) {
        free(screen);
        LogMessage(X_ERROR,
                   "AIGLX: Won't use generic native renderer as it is not accelerated\n");
        return NULL;
    }

    // Can you see the problem here?  The extensions string is DC specific
    // Different DCs for windows on a multimonitor system driven by multiple cards
    // might have completely different capabilities.  Of course, good luck getting
    // those screens to be accelerated in XP and earlier...

    {
        //
        // Based on the WGL extensions available, enable various GLX extensions
        // XXX: make this table-driven ?
        //
        __glXInitExtensionEnableBits(screen->base.glx_enable_bits);

        if (strstr(wgl_extensions, "WGL_ARB_make_current_read"))
            screen->has_WGL_ARB_make_current_read = TRUE;
        else
            LogMessage(X_WARNING, "AIGLX: missing WGL_ARB_make_current_read\n");

        if (strstr(gl_extensions, "GL_WIN_swap_hint")) {
            __glXEnableExtension(screen->base.glx_enable_bits,
                                 "GLX_MESA_copy_sub_buffer");
            LogMessage(X_INFO, "AIGLX: enabled GLX_MESA_copy_sub_buffer\n");
        }

        if (strstr(wgl_extensions, "WGL_EXT_swap_control")) {
            __glXEnableExtension(screen->base.glx_enable_bits,
                                 "GLX_SGI_swap_control");
            LogMessage(X_INFO, "AIGLX: enabled GLX_SGI_swap_control\n");
        }

/*       // Hmm?  screen->texOffset */
/*       if (strstr(wgl_extensions, "WGL_ARB_render_texture")) */
/*         { */
/*           __glXEnableExtension(screen->base.glx_enable_bits, "GLX_EXT_texture_from_pixmap"); */
/*           LogMessage(X_INFO, "AIGLX: GLX_EXT_texture_from_pixmap backed by buffer objects\n"); */
/*           screen->has_WGL_ARB_render_texture = TRUE; */
/*         } */

        if (strstr(wgl_extensions, "WGL_ARB_pbuffer"))
            screen->has_WGL_ARB_pbuffer = TRUE;
        else
            LogMessage(X_WARNING, "AIGLX: missing WGL_ARB_pbuffer\n");

        if (strstr(wgl_extensions, "WGL_ARB_multisample"))
            screen->has_WGL_ARB_multisample = TRUE;
        else
            LogMessage(X_WARNING, "AIGLX: missing WGL_ARB_multisample\n");

        screen->base.destroy = glxWinScreenDestroy;
        screen->base.createContext = glxWinCreateContext;
        screen->base.createDrawable = glxWinCreateDrawable;
        screen->base.swapInterval = glxWinScreenSwapInterval;
        screen->base.pScreen = pScreen;

        // Creating the fbConfigs initializes screen->base.fbconfigs and screen->base.numFBConfigs
        if (strstr(wgl_extensions, "WGL_ARB_pixel_format")) {
            glxWinCreateConfigsExt(hdc, screen);

            /*
               Some graphics drivers appear to advertise WGL_ARB_pixel_format,
               but it doesn't work usefully, so we have to be prepared for it
               to fail and fall back to using DescribePixelFormat()
             */
            if (screen->base.numFBConfigs > 0) {
                screen->has_WGL_ARB_pixel_format = TRUE;
            }
        }

        if (screen->base.numFBConfigs <= 0) {
            glxWinCreateConfigs(hdc, screen);
            screen->has_WGL_ARB_pixel_format = FALSE;
        }

        /*
           If we still didn't get any fbConfigs, we can't provide GLX for this screen
         */
        if (screen->base.numFBConfigs <= 0) {
            free(screen);
            LogMessage(X_ERROR,
                       "AIGLX: No fbConfigs could be made from native OpenGL pixel formats\n");
            return NULL;
        }

        /* These will be set by __glXScreenInit */
        screen->base.visuals = NULL;
        screen->base.numVisuals = 0;

        __glXScreenInit(&screen->base, pScreen);
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hglrc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);

#ifdef _DEBUG
    // dump out fbConfigs now fbConfigIds and visualIDs have been assigned
    fbConfigsDump(screen->base.numFBConfigs, screen->base.fbconfigs);
#endif

    /* Wrap RealizeWindow, UnrealizeWindow and CopyWindow on this screen */
    screen->RealizeWindow = pScreen->RealizeWindow;
    pScreen->RealizeWindow = glxWinRealizeWindow;
    screen->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = glxWinUnrealizeWindow;
    screen->CopyWindow = pScreen->CopyWindow;
    pScreen->CopyWindow = glxWinCopyWindow;
    screen->PositionWindow = pScreen->PositionWindow;
    pScreen->PositionWindow = glxWinPositionWindow;
    screen->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = glxWinDestroyWindow;

    return &screen->base;
}

/* ---------------------------------------------------------------------- */
/*
 * Window functions
 */

static Bool
glxWinRealizeWindow(WindowPtr pWin)
{
    Bool result;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    glxWinScreen *screenPriv = (glxWinScreen *) glxGetScreen(pScreen);
    winWindowPriv(pWin);

    GLWIN_DEBUG_MSG("glxWinRealizeWindow");

    /* Allow the window to be created (RootlessRealizeWindow is inside our wrap) */
    pScreen->RealizeWindow = screenPriv->RealizeWindow;
    result = pScreen->RealizeWindow(pWin);
    pScreen->RealizeWindow = glxWinRealizeWindow;
    
    // Check if ze need to move the window\n
    if (pWinPriv->fWglUsed && pWinPriv->hWnd) {
        ShowWindow(pWinPriv->hWnd,SW_SHOWNOACTIVATE);
    }
    return result;
}

static void
glxWinCopyWindow(WindowPtr pWindow, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
    __GLXWinDrawable *pGlxDraw;
    ScreenPtr pScreen = pWindow->drawable.pScreen;
    glxWinScreen *screenPriv = (glxWinScreen *) glxGetScreen(pScreen);

    GLWIN_TRACE_MSG("glxWinCopyWindow pWindow %p", pWindow);

    dixLookupResourceByType((void *) &pGlxDraw, pWindow->drawable.id,
                            __glXDrawableRes, NullClient, DixUnknownAccess);

    /*
       Discard any CopyWindow requests if a GL drawing context is pointing at the window

       For regions which are being drawn by GL, the shadow framebuffer doesn't have the
       correct bits, so we wish to avoid shadow framebuffer damage occuring, which will
       cause those incorrect bits to be transferred to the display....
     */
    if (pGlxDraw && pGlxDraw->drawContext) {
        GLWIN_DEBUG_MSG("glxWinCopyWindow: discarding");
        return;
    }

    GLWIN_DEBUG_MSG("glxWinCopyWindow - passing to hw layer");

    pScreen->CopyWindow = screenPriv->CopyWindow;
    pScreen->CopyWindow(pWindow, ptOldOrg, prgnSrc);
    pScreen->CopyWindow = glxWinCopyWindow;
}

static Bool
glxWinPositionWindow(WindowPtr pWin, int x, int y)
{
    Bool result;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    glxWinScreen *screenPriv = (glxWinScreen *) glxGetScreen(pScreen);
    winWindowPriv(pWin);

    pScreen->PositionWindow = screenPriv->PositionWindow;
    result = pScreen->PositionWindow(pWin, x, y);
    pScreen->PositionWindow = glxWinPositionWindow;

    if (pWinPriv->fWglUsed && pWinPriv->hWnd)
    {
        MoveWindow(pWinPriv->hWnd,
                   pWin->drawable.x,
                   pWin->drawable.y,
                   pWin->drawable.width,
                   pWin->drawable.height,
                   FALSE);
        winDebug("Move window %x,  %x, %d, %d, %d, %d\n",pWinPriv->hWnd,GetParent(pWinPriv->hWnd), pWin->drawable.x, pWin->drawable.y, pWin->drawable.width, pWin->drawable.height);
    }
    return result;
}


static Bool
glxWinUnrealizeWindow(WindowPtr pWin)
{
    Bool result;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    glxWinScreen *screenPriv = (glxWinScreen *) glxGetScreen(pScreen);
    winWindowPriv(pWin);

    GLWIN_DEBUG_MSG("glxWinUnrealizeWindow");

    if (pWinPriv->fWglUsed && pWinPriv->hWnd)
    {
      ShowWindow(pWinPriv->hWnd,SW_HIDE);
      result = TRUE;
    }
    else
    {
      pScreen->UnrealizeWindow = screenPriv->UnrealizeWindow;
      result = pScreen->UnrealizeWindow(pWin);
      pScreen->UnrealizeWindow = glxWinUnrealizeWindow;
    }


    return result;
}

static Bool
glxWinDestroyWindow(WindowPtr pWin)
{
    Bool result;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    glxWinScreen *screenPriv = (glxWinScreen *)glxGetScreen(pScreen);
    __GLXWinDrawable *pGlxDraw;
    winWindowPriv(pWin);

    GLWIN_DEBUG_MSG("glxWinDestroyWindow");

    dixLookupResourceByType((pointer) &pGlxDraw, pWin->drawable.id, __glXDrawableRes, NullClient, DixUnknownAccess);

    if (pGlxDraw && pGlxDraw->drawContext)
    {
      if (pGlxDraw->drawContext->hwnd!=pWinPriv->hWnd)
        ErrorF("Wrong assumption\n");
      glxWinReleaseDC(pGlxDraw->drawContext->hwnd, pGlxDraw->drawContext->hDC, pGlxDraw);
      pGlxDraw->drawContext->hDC=NULL;
      pGlxDraw->drawContext->hwnd=NULL;
    }
    if (pWinPriv->fWglUsed && pWinPriv->hWnd)
    {
      DestroyWindow(pWinPriv->hWnd);
      pWinPriv->hWnd=NULL;
      pWinPriv->fWglUsed=0;
    }

    pScreen->DestroyWindow = screenPriv->DestroyWindow;
    result = pScreen->DestroyWindow(pWin);
    pScreen->DestroyWindow = glxWinDestroyWindow;

    return result;
}

/* ---------------------------------------------------------------------- */
/*
 * Drawable functions
 */

static GLboolean
glxWinDrawableSwapBuffers(ClientPtr client, __GLXdrawable * base)
{
    BOOL ret;
    __GLXWinDrawable *draw = (__GLXWinDrawable *) base;

    /* Swap buffers on the last active context for drawing on the drawable */
    if (draw->drawContext == NULL) {
        GLWIN_TRACE_MSG("glxWinSwapBuffers - no context for drawable");
        return GL_FALSE;
    }

    GLWIN_TRACE_MSG
        ("glxWinSwapBuffers on drawable %p, last context %p (native ctx %p)",
         base, draw->drawContext, draw->drawContext->ctx);

    ret = SwapBuffers(draw->drawContext->hDC);

    if (!ret) {
        ErrorF("wglSwapBuffers failed: %s\n", glxWinErrorMessage());
        return GL_FALSE;
    }

    return GL_TRUE;
}

static void
glxWinDrawableCopySubBuffer(__GLXdrawable * drawable,
                            int x, int y, int w, int h)
{
    glAddSwapHintRectWINWrapperNonstatic(x, y, w, h);
    glxWinDrawableSwapBuffers(NULL, drawable);
}

static void
glxWinDrawableDestroy(__GLXdrawable * base)
{
    __GLXWinDrawable *glxPriv = (__GLXWinDrawable *) base;

    if (glxPriv->hPbuffer)
        if (!wglDestroyPbufferARBWrapper(glxPriv->hPbuffer)) {
            ErrorF("wglDestroyPbufferARB failed: %s\n", glxWinErrorMessage());
        }

    if (glxPriv->dibDC) {
        // restore the default DIB
        SelectObject(glxPriv->dibDC, glxPriv->hOldDIB);

        if (!DeleteDC(glxPriv->dibDC)) {
            ErrorF("DeleteDC failed: %s\n", glxWinErrorMessage());
        }
    }

    if (glxPriv->hDIB) {
        if (!DeleteObject(glxPriv->hDIB)) {
            ErrorF("DeleteObject failed: %s\n", glxWinErrorMessage());
        }

        ((PixmapPtr) glxPriv->base.pDraw)->devPrivate.ptr = glxPriv->pOldBits;
        glxPriv->base.pDraw->pScreen->DestroyPixmap((PixmapPtr)glxPriv->base.pDraw); /* Decrement reference count since we do not use it any more */
    }

    GLWIN_DEBUG_MSG("glxWinDestroyDrawable");
    free(glxPriv);
}

static __GLXdrawable *
glxWinCreateDrawable(ClientPtr client,
                     __GLXscreen * screen,
                     DrawablePtr pDraw,
                     XID drawId, int type, XID glxDrawId, __GLXconfig * conf)
{
    __GLXWinDrawable *glxPriv;

    glxPriv = calloc(1, sizeof *glxPriv);

    if (glxPriv == NULL)
        return NULL;

    if (!__glXDrawableInit
        (&glxPriv->base, screen, pDraw, type, glxDrawId, conf)) {
        free(glxPriv);
        return NULL;
    }

    glxPriv->base.destroy = glxWinDrawableDestroy;
    glxPriv->base.swapBuffers = glxWinDrawableSwapBuffers;
    glxPriv->base.copySubBuffer = glxWinDrawableCopySubBuffer;
    // glxPriv->base.waitX  what are these for?
    // glxPriv->base.waitGL

    GLWIN_DEBUG_MSG("glxWinCreateDrawable %p", glxPriv);

    return &glxPriv->base;
}

/* ---------------------------------------------------------------------- */
/*
 * Texture functions
 */

static
    int
glxWinBindTexImage(__GLXcontext * baseContext,
                   int buffer, __GLXdrawable * pixmap)
{
    ErrorF("glxWinBindTexImage: not implemented\n");
    return FALSE;
}

static
    int
glxWinReleaseTexImage(__GLXcontext * baseContext,
                      int buffer, __GLXdrawable * pixmap)
{
    ErrorF(" glxWinReleaseTexImage: not implemented\n");
    return FALSE;
}

/* ---------------------------------------------------------------------- */
/*
 * Lazy update context implementation
 *
 * WGL contexts are created for a specific HDC, so we cannot create the WGL
 * context in glxWinCreateContext(), we must defer creation until the context
 * is actually used on a specifc drawable which is connected to a native window,
 * pbuffer or DIB
 *
 * The WGL context may be used on other, compatible HDCs, so we don't need to
 * recreate it for every new native window
 *
 * XXX: I wonder why we can't create the WGL context on the screen HDC ?
 * Basically we assume all HDCs are compatible at the moment: if they are not
 * we are in a muddle, there was some code in the old implementation to attempt
 * to transparently migrate a context to a new DC by copying state and sharing
 * lists with the old one...
 */

static Bool
glxWinSetPixelFormat(__GLXWinContext * gc, HDC hdc, int bppOverride,
                     int drawableTypeOverride)
{
    __GLXscreen *screen = gc->base.pGlxScreen;
    glxWinScreen *winScreen = (glxWinScreen *) screen;

    __GLXconfig *config = gc->base.config;
    GLXWinConfig *winConfig = (GLXWinConfig *) config;


    WindowPtr pWin;
    __GLXWinDrawable *drawPriv = (__GLXWinDrawable *)gc->base.drawPriv;
    pWin = (WindowPtr) drawPriv->base.pDraw;
    {
        winWindowPriv(pWin);
        if (pWinPriv->OpenGlWindow)
        {
            ErrorF("Not Setting pixel format to  %d on hdc %x for window %x (not allowed on windows)\n",winConfig->pixelFormatIndex,hdc,pWinPriv->hWnd);
            return TRUE; /* Pixel format is already set on this window so it cannot be changed anymore */
        }
    }
    GLWIN_DEBUG_MSG("glxWinSetPixelFormat: pixelFormatIndex %d", winConfig->pixelFormatIndex);

    /*
    Normally, we can just use the the pixelFormatIndex corresponding
    to the fbconfig which has been specified by the client
    */
    /*
       However, in certain special cases this pixel format will be incompatible with the
       use we are going to put it to, so we need to re-evaluate the pixel format to use:

       1) When PFD_DRAW_TO_BITMAP is set, ChoosePixelFormat() always returns a format with
       the cColorBits we asked for, so we need to ensure it matches the bpp of the bitmap

       2) Applications may assume that visuals selected with glXChooseVisual() work with
       pixmap drawables (there is no attribute to explicitly query for pixmap drawable
       support as there is for glXChooseFBConfig())
       (it's arguable this is an error in the application, but we try to make it work)

       pixmap rendering is always slow for us, so we don't want to choose those visuals
       by default, but if the actual drawable type we're trying to select the context
       on (drawableTypeOverride) isn't supported by the selected fbConfig, reconsider
       and see if we can find a suitable one...
     */
    ErrorF
        ("glxWinSetPixelFormat: having second thoughts: cColorbits %d, bppOveride %d; config->drawableType %d, drawableTypeOverride %d\n",
         (config->redBits + config->greenBits + config->blueBits), bppOverride,
         config->drawableType, drawableTypeOverride);

    if (!winScreen->has_WGL_ARB_pixel_format) {
        PIXELFORMATDESCRIPTOR pfd;
        int pixelFormat;

        /* convert fbConfig to PFD */
        if (fbConfigToPixelFormat(gc->base.config, &pfd, drawableTypeOverride)) {
            ErrorF("glxWinSetPixelFormat: fbConfigToPixelFormat failed\n");
            return FALSE;
        }

#ifdef _DEBUG
        if (glxWinDebugSettings.dumpPFD)
            pfdOut(&pfd);
#endif

        if (bppOverride) {
            GLWIN_DEBUG_MSG("glxWinSetPixelFormat: Forcing bpp from %d to %d\n",
                            pfd.cColorBits, bppOverride);
            pfd.cColorBits = bppOverride;
        }

        pixelFormat = ChoosePixelFormat(hdc, &pfd);
        if (pixelFormat == 0) {
            ErrorF("ChoosePixelFormat error: %s\n", glxWinErrorMessage());
            return FALSE;
        }

        GLWIN_DEBUG_MSG("ChoosePixelFormat: chose pixelFormatIndex %d",
                        pixelFormat);
        ErrorF
            ("ChoosePixelFormat: chose pixelFormatIndex %d (rather than %d as originally planned)\n",
             pixelFormat, winConfig->pixelFormatIndex);

        ErrorF("Setting pixel format 2 to  %d on hdc %x\n",pixelFormat,hdc);
        if (!SetPixelFormat(hdc, pixelFormat, &pfd)) {
            ErrorF("SetPixelFormat error: %s\n", glxWinErrorMessage());
            return FALSE;
        }
    }
    else {
        int pixelFormat =
            fbConfigToPixelFormatIndex(hdc, gc->base.config,
                                       drawableTypeOverride, winScreen);
        if (pixelFormat != 0) {
            GLWIN_DEBUG_MSG("wglChoosePixelFormat: chose pixelFormatIndex %d", pixelFormat);

            ErrorF("Setting pixel format 3 to  %d on hdc %x\n",pixelFormat,hdc);
            if (!SetPixelFormat(hdc, pixelFormat, NULL)) {
                ErrorF("SetPixelFormat error: %s\n", glxWinErrorMessage());
                return FALSE;
            }
            return TRUE;
        }
        else {
            /* There was an error choose some default for the moment */
            PIXELFORMATDESCRIPTOR pfd = { 
                sizeof(PIXELFORMATDESCRIPTOR),   // size of this pfd  
                1,                     // version number  
                PFD_DRAW_TO_WINDOW |   // support window  
                PFD_SUPPORT_OPENGL |   // support OpenGL  
                PFD_DOUBLEBUFFER,      // double buffered  
                PFD_TYPE_RGBA,         // RGBA type  
                24,                    // 24-bit color depth  
                0, 0, 0, 0, 0, 0,      // color bits ignored  
                0,                     // no alpha buffer  
                0,                     // shift bit ignored  
                0,                     // no accumulation buffer  
                0, 0, 0, 0,            // accum bits ignored  
                32,                    // 32-bit z-buffer  
                0,                     // no stencil buffer  
                0,                     // no auxiliary buffer  
                PFD_MAIN_PLANE,        // main layer  
                0,                     // reserved  
                0, 0, 0                // layer masks ignored  
            }; 
            int  iPixelFormat; 
         
            // get the best available match of pixel format for the device context   
            iPixelFormat = ChoosePixelFormat(hdc, &pfd); 
         
            ErrorF("Setting pixel format 4 to  %d on hdc %x\n",iPixelFormat,hdc);
            // make that the pixel format of the device context  
            if (!SetPixelFormat(hdc, iPixelFormat, &pfd)) {
                ErrorF("SetPixelFormat error: %s\n", glxWinErrorMessage());
                return FALSE;
            }
        }
    }
    return TRUE;
}

static HDC
glxWinMakeDC(__GLXWinContext *gc, __GLXWinDrawable *draw, HWND *hwnd)
{
    HDC hdc = NULL;
    *hwnd = NULL;

    if (draw == NULL) {
        GLWIN_TRACE_MSG("No drawable for context %p (native ctx %p)", gc,
                        gc->ctx);
        return NULL;
    }

    switch (draw->base.type) {
    case GLX_DRAWABLE_WINDOW:
    {
        WindowPtr pWin;

        pWin = (WindowPtr) draw->base.pDraw;
        if (pWin == NULL) {
            GLWIN_TRACE_MSG("for drawable %p, no WindowPtr", pWin);
            return NULL;
        }

        *hwnd = winGetWindowInfo(pWin);

        if (*hwnd == NULL) {
            ErrorF("No HWND error: %s\n", glxWinErrorMessage());
            return NULL;
        }

        if (!gc->hDC) {
            winWindowPriv(pWin);

            hdc = GetDC(*hwnd);

            if (hdc == NULL)
                ErrorF("GetDC error: %s: hwnd %x, gc %p, gc->ctx %p ,gc->hwnd %p\n", glxWinErrorMessage(), *hwnd, gc, gc->ctx, gc->hwnd);

            glxWinSetPixelFormat(gc, hdc, 0, GLX_WINDOW_BIT);
            pWinPriv->OpenGlWindow=TRUE; /* Identify it as an opengl window, also used to check if the pixel format is already set */
            gc->ctx = wglCreateContext(hdc);
        }

#ifdef _DEBUG
        if (glxWinDebugSettings.enableTrace)
            GLWIN_DEBUG_HWND(*hwnd);

        GLWIN_TRACE_MSG
            ("for context %p (native ctx %p), hWnd changed from %p to %p",
             gc, gc->ctx, gc->hwnd, *hwnd);
#endif
        if (gc->hwnd!=*hwnd)
            ErrorF("Window changed handle from %x to %x\n", gc->hwnd, *hwnd);

        gc->hwnd = *hwnd;
    }
        break;

    case GLX_DRAWABLE_PBUFFER:
    {
        hdc = wglGetPbufferDCARBWrapper(draw->hPbuffer);

        if (hdc == NULL)
            ErrorF("GetDC (pbuffer) error: %s\n", glxWinErrorMessage());

        gc->ctx = wglCreateContext(hdc);
    }
        break;

    case GLX_DRAWABLE_PIXMAP:
    {
        hdc = draw->dibDC;
#ifdef _DEBUG
        if (glxWinDebugSettings.dumpDC)
            GLWIN_DEBUG_MSG("Got PIXMAP HDC %p for window %p", hdc, *hwnd);
#endif
    }
        break;

    default:
    {
        ErrorF("glxWinMakeDC: tried to makeDC for unhandled drawable type %d\n",
               draw->base.type);
    }
    }

#ifdef _DEBUG
    if (glxWinDebugSettings.dumpDC)
        GLWIN_HDC_DEBUG_MSG("Got HDC %p for window %p", hdc, *hwnd);
#endif

    return hdc;
}

static void
glxWinReleaseDC(HWND hwnd, HDC hdc, __GLXWinDrawable * draw)
{
    switch (draw->base.type) {
    case GLX_DRAWABLE_WINDOW:
    {
        ReleaseDC(hwnd, hdc);
    }
        break;

    case GLX_DRAWABLE_PBUFFER:
    {
        if (!wglReleasePbufferDCARBWrapper(draw->hPbuffer, hdc)) {
            ErrorF("wglReleasePbufferDCARB error: %s\n", glxWinErrorMessage());
        }
    }
        break;

    case GLX_DRAWABLE_PIXMAP:
    {
        // don't release DC, the memory DC lives as long as the bitmap

        // We must ensure that all GDI drawing into the bitmap has completed
        // in case we subsequently access the bits from it
        GdiFlush();
    }
        break;

    default:
    {
        ErrorF
            ("glxWinReleaseDC: tried to releaseDC for unhandled drawable type %d\n",
             draw->base.type);
    }
    }
}

static void
glxWinDeferredCreateContext(__GLXWinContext * gc, __GLXWinDrawable * draw)
{
    HWND hwnd;

    GLWIN_DEBUG_MSG
        ("glxWinDeferredCreateContext: attach context %p to drawable %p", gc,
         draw);

    switch (draw->base.type) {
    case GLX_DRAWABLE_WINDOW:
    {
        WindowPtr pWin = (WindowPtr) draw->base.pDraw;

        if (!(gc->base.config->drawableType & GLX_WINDOW_BIT)) {
            ErrorF
                ("glxWinDeferredCreateContext: tried to attach a context whose fbConfig doesn't have drawableType GLX_WINDOW_BIT to a GLX_DRAWABLE_WINDOW drawable\n");
        }

        if (pWin == NULL) {
            GLWIN_DEBUG_MSG("Deferring until X window is created");
            return;
        }

        GLWIN_DEBUG_MSG("glxWinDeferredCreateContext: pWin %p", pWin);

        if (winGetWindowInfo(pWin) == NULL) {
            GLWIN_DEBUG_MSG("Deferring until native window is created");
            return;
        }
    }
        break;

    case GLX_DRAWABLE_PBUFFER:
    {
        WindowPtr pWin = (WindowPtr) draw->base.pDraw;
        if (draw->hPbuffer == NULL) {
            __GLXscreen *screen;
            glxWinScreen *winScreen;
            int pixelFormat;

            // XXX: which DC are we supposed to use???
            ScreenPtr pScreen = pWin->drawable.pScreen;
            winPrivScreenPtr pWinScreen = winGetScreenPriv(pScreen);
            HDC screenDC=pWinScreen->hdcScreen;

            if (!(gc->base.config->drawableType & GLX_PBUFFER_BIT)) {
                ErrorF
                    ("glxWinDeferredCreateContext: tried to attach a context whose fbConfig doesn't have drawableType GLX_PBUFFER_BIT to a GLX_DRAWABLE_PBUFFER drawable\n");
            }

            screen = gc->base.pGlxScreen;
            winScreen = (glxWinScreen *) screen;

            pixelFormat =
                fbConfigToPixelFormatIndex(screenDC, gc->base.config,
                                           GLX_PBUFFER_BIT, winScreen);
            if (pixelFormat == 0) {
                return;
            }

            draw->hPbuffer =
                wglCreatePbufferARBWrapper(screenDC, pixelFormat,
                                           draw->base.pDraw->width,
                                           draw->base.pDraw->height, NULL);

            if (draw->hPbuffer == NULL) {
                ErrorF("wglCreatePbufferARBWrapper error: %s\n",
                       glxWinErrorMessage());
                return;
            }

            GLWIN_DEBUG_MSG
                ("glxWinDeferredCreateContext: pBuffer %p created for drawable %p",
                 draw->hPbuffer, draw);
        }
    }
        break;

    case GLX_DRAWABLE_PIXMAP:
    {
        if (draw->dibDC == NULL) {
            BITMAPINFOHEADER bmpHeader;
            void *pBits;

            memset(&bmpHeader, 0, sizeof(BITMAPINFOHEADER));
            bmpHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmpHeader.biWidth = draw->base.pDraw->width;
            bmpHeader.biHeight = draw->base.pDraw->height;
            bmpHeader.biPlanes = 1;
            bmpHeader.biBitCount = draw->base.pDraw->bitsPerPixel;
            bmpHeader.biCompression = BI_RGB;

            if (!(gc->base.config->drawableType & GLX_PIXMAP_BIT)) {
                ErrorF
                    ("glxWinDeferredCreateContext: tried to attach a context whose fbConfig doesn't have drawableType GLX_PIXMAP_BIT to a GLX_DRAWABLE_PIXMAP drawable\n");
            }

            draw->dibDC = CreateCompatibleDC(NULL);
            if (draw->dibDC == NULL) {
                ErrorF("CreateCompatibleDC error: %s\n", glxWinErrorMessage());
                return;
            }

            draw->hDIB =
                CreateDIBSection(draw->dibDC, (BITMAPINFO *) &bmpHeader,
                                 DIB_RGB_COLORS, &pBits, 0, 0);
            if (draw->dibDC == NULL) {
                ErrorF("CreateDIBSection error: %s\n", glxWinErrorMessage());
                return;
            }

            // XXX: CreateDIBSection insists on allocating the bitmap memory for us, so we're going to
            // need some jiggery pokery to point the underlying X Drawable's bitmap at the same set of bits
            // so that they can be read with XGetImage as well as glReadPixels, assuming the formats are
            // even compatible ...
            draw->pOldBits = ((PixmapPtr) draw->base.pDraw)->devPrivate.ptr;
            ((PixmapPtr) draw->base.pDraw)->devPrivate.ptr = pBits;

            ((PixmapPtr)draw->base.pDraw)->refcnt++;  /* Increment reference count to be sure it is not freed before the glxdrawable is destroyed */

            // Select the DIB into the DC
            draw->hOldDIB = SelectObject(draw->dibDC, draw->hDIB);
            if (!draw->hOldDIB) {
                ErrorF("SelectObject error: %s\n", glxWinErrorMessage());
            }

            // Set the pixel format of the bitmap
            glxWinSetPixelFormat(gc, draw->dibDC,
                                 draw->base.pDraw->bitsPerPixel,
                                 GLX_PIXMAP_BIT);

            GLWIN_DEBUG_MSG
                ("glxWinDeferredCreateContext: DIB bitmap %p created for drawable %p",
                 draw->hDIB, draw);
        }
    }
        break;

    default:
    {
        ErrorF
            ("glxWinDeferredCreateContext: tried to attach unhandled drawable type %d\n",
             draw->base.type);
        return;
    }
    }

    gc->hDC = glxWinMakeDC(gc, draw, &hwnd);

    if (gc->ctx == NULL) {
        glxWinReleaseDC(hwnd, gc->hDC, draw);
        gc->hDC=0;
      
        ErrorF("wglCreateContext error: %s\n", glxWinErrorMessage());
        return;
    }

    GLWIN_DEBUG_MSG
        ("glxWinDeferredCreateContext: attached context %p to native context %p drawable %p",
         gc, gc->ctx, draw);

    // if the native context was created successfully, shareLists if needed
    if (gc->ctx && gc->shareContext && gc->shareContext->ctx) {
        GLWIN_DEBUG_MSG
            ("glxWinCreateContextReal shareLists with context %p (native ctx %p)",
             gc->shareContext, gc->shareContext->ctx);

        if (!wglShareLists(gc->shareContext->ctx, gc->ctx)) {
            ErrorF("wglShareLists error: %s\n", glxWinErrorMessage());
        }
    }
}

/* ---------------------------------------------------------------------- */
/*
 * Context functions
 */

/* Context manipulation routines should return TRUE on success, FALSE on failure */
static int
glxWinContextMakeCurrent(__GLXcontext * base)
{
    __GLXWinContext *gc = (__GLXWinContext *) base;
    glxWinScreen *scr = (glxWinScreen *)base->pGlxScreen;
    BOOL ret;
    __GLXWinDrawable *drawPriv;

#ifdef _DEBUG
    GLWIN_TRACE_MSG("glxWinContextMakeCurrent context %p (native ctx %p)", gc, gc->ctx);
    glWinCallDelta();
#endif

    /* Keep a note of the last active context in the drawable */
    drawPriv = (__GLXWinDrawable *)gc->base.drawPriv;
    drawPriv->drawContext = gc;

    if (gc->ctx == NULL) {
        glxWinDeferredCreateContext(gc, drawPriv);
    }
    _glapi_set_dispatch(gc->Dispatch);

    if (gc->ctx == NULL) {
        ErrorF("glxWinContextMakeCurrent: Native context is NULL\n");
        drawPriv->drawContext = NULL; /* clear last active context because we return error */
        return FALSE;
    }

    if ((gc->base.readPriv != NULL) && (gc->base.readPriv != gc->base.drawPriv)) {
        /*
         * We enable GLX_SGI_make_current_read unconditionally, but the
         * renderer might not support it. It's fairly rare to use this
         * feature so just error out if it can't work.
         */
        if (!scr->has_WGL_ARB_make_current_read)
            return False;

        /*
           If there is a separate read drawable, create a separate read DC, and
           use the wglMakeContextCurrent extension to make the context current drawing
           to one DC and reading from the other
         */
        gc->hreadDC = glxWinMakeDC(gc, (__GLXWinDrawable *)gc->base.readPriv, &gc->hreadwnd);
        if (gc->hreadDC == NULL) {
            ErrorF("glxWinMakeDC failed for readDC\n");
            drawPriv->drawContext = NULL; /* clear last active context because we return error */
            return FALSE;
        }

        ret = wglMakeContextCurrentARBWrapper(gc->hDC, gc->hreadDC, gc->ctx);
        if (!ret) {
            ErrorF("wglMakeContextCurrentARBWrapper error: %s\n",
                   glxWinErrorMessage());
        }
    }
    else {
        /* Otherwise, just use wglMakeCurrent */
        if (!gc->hDC) {
            /* It probably has been release by loseCurrent, so create it again */
            gc->hDC = glxWinMakeDC(gc, drawPriv, &gc->hwnd);
        }
        ret = wglMakeCurrent(gc->hDC, gc->ctx);
        if (!ret) {
            DWORD ErrorCode=GetLastError();
            ErrorF("wglMakeCurrent error: %x dc %p ctx %p\n", ErrorCode,gc->hDC,gc->ctx);
            if (!ErrorCode) {
                ErrorF("Error code was 0, assuming no error.\n");
                ret=TRUE;
            }
        }
    }

    // apparently make current could fail if the context is current in a different thread,
    // but that shouldn't be able to happen in the current server...
    if (!ret)
      drawPriv->drawContext = NULL; /* clear last active context because we return error */

    return ret;
}

static int
glxWinContextLoseCurrent(__GLXcontext * base)
{
    BOOL ret=TRUE;
    __GLXWinContext *gc = (__GLXWinContext *) base;
    __GLXWinDrawable *drawPriv = (__GLXWinDrawable *)gc->base.drawPriv;

#ifdef _DEBUG
    GLWIN_TRACE_MSG("glxWinContextLoseCurrent context %p (native ctx %p)", gc, gc->ctx);
    glWinCallDelta();
#endif

     /* Clear the last active context in the drawable */
    if (drawPriv) drawPriv->drawContext = NULL;

    if (wglGetCurrentContext()==gc->ctx)
    {
      /* Only do this when we are sure we are currently the active, otherwise we are deactivating the wrong one (this is happening!!!) */
      ret = wglMakeCurrent(NULL, NULL);
      if (!ret)
        ErrorF("glxWinContextLoseCurrent error: %s\n", glxWinErrorMessage());
    }
    else
    {
      return FALSE;
    }

    base->currentClient=NULL;  /* It looks like glx is not doing this */
    _glapi_set_dispatch(NULL);

    return ret;
}

static int
glxWinContextCopy(__GLXcontext * dst_base, __GLXcontext * src_base,
                  unsigned long mask)
{
    __GLXWinContext *dst = (__GLXWinContext *) dst_base;
    __GLXWinContext *src = (__GLXWinContext *) src_base;
    BOOL ret;

    GLWIN_DEBUG_MSG("glxWinContextCopy");

    ret = wglCopyContext(src->ctx, dst->ctx, mask);
    if (!ret) {
        ErrorF("wglCopyContext error: %s\n", glxWinErrorMessage());
    }

    return ret;
}

static void
glxWinContextDestroy(__GLXcontext * base)
{
    __GLXWinContext *gc = (__GLXWinContext *) base;

    if (gc != NULL) {
        __GLXWinDrawable *drawPriv = (__GLXWinDrawable *)gc->base.drawPriv;

        GLWIN_DEBUG_MSG("GLXcontext %p destroyed (native ctx %p)", base,
                        gc->ctx);

        if (gc->ctx) {
            BOOL ret;
            /* It's bad style to delete the context while it's still current */
            if (wglGetCurrentContext() == gc->ctx) {
                wglMakeCurrent(NULL, NULL);
            }

            ret = wglDeleteContext(gc->ctx);

            if (!ret)
                ErrorF("wglDeleteContext error: %s\n", glxWinErrorMessage());
            if (drawPriv && gc->hDC) glxWinReleaseDC(gc->hwnd, gc->hDC, drawPriv);
            if (gc->base.readPriv && gc->hreadDC) glxWinReleaseDC(gc->hreadwnd, gc->hreadDC, (__GLXWinDrawable *)gc->base.readPriv);
            gc->hDC=NULL;
            gc->hreadDC=NULL;
            gc->ctx = NULL;
        }

        /* Clear the last active context in the drawable */
        if (drawPriv) drawPriv->drawContext = NULL;

        free(gc->Dispatch);
        free(gc);
        _glapi_set_dispatch(NULL);
    }
}

static __GLXcontext *
glxWinCreateContext(__GLXscreen * screen,
                    __GLXconfig * modes, __GLXcontext * baseShareContext,
                    unsigned num_attribs, const uint32_t * attribs, int *error)
{
    __GLXWinContext *context;
    __GLXWinContext *shareContext = (__GLXWinContext *) baseShareContext;

    static __GLXtextureFromPixmap glxWinTextureFromPixmap = {
        glxWinBindTexImage,
        glxWinReleaseTexImage
    };

    context = calloc(1, sizeof(__GLXWinContext));

    if (!context)
        return NULL;

    memset(context, 0, sizeof *context);
    context->base.destroy = glxWinContextDestroy;
    context->base.makeCurrent = glxWinContextMakeCurrent;
    context->base.loseCurrent = glxWinContextLoseCurrent;
    context->base.copy = glxWinContextCopy;
    context->base.textureFromPixmap = &glxWinTextureFromPixmap;
    context->base.config = modes;
    context->base.pGlxScreen = screen;

    // actual native GL context creation is deferred until attach()
    //context->ctx = NULL; already done with memset
    context->shareContext = shareContext;

    context->Dispatch=calloc(sizeof(void*), (sizeof(struct _glapi_table) / sizeof(void *) + MAX_EXTENSION_FUNCS));
    _glapi_set_dispatch(context->Dispatch);

    glWinSetupDispatchTable();

    GLWIN_DEBUG_MSG("GLXcontext %p created", context);

    return &(context->base);
}

/* ---------------------------------------------------------------------- */
/*
 * Utility functions
 */

static int
GetShift(int mask)
{
    int shift = 0;

    while ((mask &1) == 0) {
        shift++;
        mask >>=1;
    }
    return shift;
}

static int
fbConfigToPixelFormat(__GLXconfig * mode, PIXELFORMATDESCRIPTOR * pfdret,
                      int drawableTypeOverride)
{
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),  /* size of this pfd */
        1,                      /* version number */
        PFD_SUPPORT_OPENGL,     /* support OpenGL */
        PFD_TYPE_RGBA,          /* RGBA type */
        24,                     /* 24-bit color depth */
        0, 0, 0, 0, 0, 0,       /* color bits ignored */
        0,                      /* no alpha buffer */
        0,                      /* shift bit ignored */
        0,                      /* no accumulation buffer */
        0, 0, 0, 0,             /* accum bits ignored */
        32,                     /* 32-bit z-buffer */
        0,                      /* no stencil buffer */
        0,                      /* no auxiliary buffer */
        PFD_MAIN_PLANE,         /* main layer */
        0,                      /* reserved */
        0, 0, 0                 /* layer masks ignored */
    };

    if ((mode->drawableType | drawableTypeOverride) & GLX_WINDOW_BIT)
        pfd.dwFlags |= PFD_DRAW_TO_WINDOW;      /* support window */

    if ((mode->drawableType | drawableTypeOverride) & GLX_PIXMAP_BIT)
        pfd.dwFlags |= (PFD_DRAW_TO_BITMAP | PFD_SUPPORT_GDI);  /* supports software rendering to bitmap */

    if (mode->stereoMode) {
        pfd.dwFlags |= PFD_STEREO;
    }
    if (mode->doubleBufferMode) {
        pfd.dwFlags |= PFD_DOUBLEBUFFER;
    }

    pfd.cColorBits = mode->redBits + mode->greenBits + mode->blueBits;
    pfd.cRedBits = mode->redBits;
    pfd.cRedShift = GetShift(mode->redMask);
    pfd.cGreenBits = mode->greenBits;
    pfd.cGreenShift = GetShift(mode->greenMask);
    pfd.cBlueBits = mode->blueBits;
    pfd.cBlueShift = GetShift(mode->blueMask);
    pfd.cAlphaBits = mode->alphaBits;
    pfd.cAlphaShift = GetShift(mode->alphaMask);

    if (mode->visualType == GLX_TRUE_COLOR) {
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.dwVisibleMask =
            (pfd.cRedBits << pfd.cRedShift) | (pfd.cGreenBits << pfd.cGreenShift) |
            (pfd.cBlueBits << pfd.cBlueShift) | (pfd.cAlphaBits << pfd.cAlphaShift);
    }
    else {
        pfd.iPixelType = PFD_TYPE_COLORINDEX;
        pfd.dwVisibleMask = mode->transparentIndex;
    }

    pfd.cAccumBits =
        mode->accumRedBits + mode->accumGreenBits + mode->accumBlueBits +
        mode->accumAlphaBits;
    pfd.cAccumRedBits = mode->accumRedBits;
    pfd.cAccumGreenBits = mode->accumGreenBits;
    pfd.cAccumBlueBits = mode->accumBlueBits;
    pfd.cAccumAlphaBits = mode->accumAlphaBits;

    pfd.cDepthBits = mode->depthBits;
    pfd.cStencilBits = mode->stencilBits;
    pfd.cAuxBuffers = mode->numAuxBuffers;

    /* mode->level ? */

    *pfdret = pfd;

    return 0;
}

#define SET_ATTR_VALUE(attr, value) { attribList[i++] = attr; attribList[i++] = value; assert(i < NUM_ELEMENTS(attribList)); }

static int
fbConfigToPixelFormatIndex(HDC hdc, __GLXconfig * mode,
                           int drawableTypeOverride, glxWinScreen * winScreen)
{
    UINT numFormats;
    unsigned int i = 0;

    /* convert fbConfig to attr-value list  */
    int attribList[60];

    SET_ATTR_VALUE(WGL_SUPPORT_OPENGL_ARB, TRUE);
    SET_ATTR_VALUE(WGL_PIXEL_TYPE_ARB,
                   (mode->visualType ==
                    GLX_TRUE_COLOR) ? WGL_TYPE_RGBA_ARB :
                   WGL_TYPE_COLORINDEX_ARB);
    SET_ATTR_VALUE(WGL_COLOR_BITS_ARB,
                   (mode->visualType ==
                    GLX_TRUE_COLOR) ? mode->rgbBits : mode->indexBits);
    SET_ATTR_VALUE(WGL_RED_BITS_ARB, mode->redBits);
    SET_ATTR_VALUE(WGL_GREEN_BITS_ARB, mode->greenBits);
    SET_ATTR_VALUE(WGL_BLUE_BITS_ARB, mode->blueBits);
    SET_ATTR_VALUE(WGL_ALPHA_BITS_ARB, mode->alphaBits);
    SET_ATTR_VALUE(WGL_ACCUM_RED_BITS_ARB, mode->accumRedBits);
    SET_ATTR_VALUE(WGL_ACCUM_GREEN_BITS_ARB, mode->accumGreenBits);
    SET_ATTR_VALUE(WGL_ACCUM_BLUE_BITS_ARB, mode->accumBlueBits);
    SET_ATTR_VALUE(WGL_ACCUM_ALPHA_BITS_ARB, mode->accumAlphaBits);
    SET_ATTR_VALUE(WGL_DEPTH_BITS_ARB, mode->depthBits);
    SET_ATTR_VALUE(WGL_STENCIL_BITS_ARB, mode->stencilBits);
    SET_ATTR_VALUE(WGL_AUX_BUFFERS_ARB, mode->numAuxBuffers);

    if (mode->doubleBufferMode)
        SET_ATTR_VALUE(WGL_DOUBLE_BUFFER_ARB, TRUE);

    if (mode->stereoMode)
        SET_ATTR_VALUE(WGL_STEREO_ARB, TRUE);

    // Some attributes are only added to the list if the value requested is not 'don't care', as exactly matching that is daft..
    if (mode->swapMethod == GLX_SWAP_EXCHANGE_OML)
        SET_ATTR_VALUE(WGL_SWAP_METHOD_ARB, WGL_SWAP_EXCHANGE_ARB);

    if (mode->swapMethod == GLX_SWAP_COPY_OML)
        SET_ATTR_VALUE(WGL_SWAP_METHOD_ARB, WGL_SWAP_COPY_ARB);

    // XXX: this should probably be the other way around, but that messes up drawableTypeOverride
    if (mode->visualRating == GLX_SLOW_VISUAL_EXT)
        SET_ATTR_VALUE(WGL_ACCELERATION_ARB, WGL_NO_ACCELERATION_ARB);

    // must support all the drawable types the mode supports
    if ((mode->drawableType | drawableTypeOverride) & GLX_WINDOW_BIT)
        SET_ATTR_VALUE(WGL_DRAW_TO_WINDOW_ARB, TRUE);

    // XXX: this is a horrible hacky heuristic, in fact this whole drawableTypeOverride thing is a bad idea
    // try to avoid asking for formats which don't exist (by not asking for all when adjusting the config to include the drawableTypeOverride)
    if (drawableTypeOverride == GLX_WINDOW_BIT) {
        if (mode->drawableType & GLX_PIXMAP_BIT)
            SET_ATTR_VALUE(WGL_DRAW_TO_BITMAP_ARB, TRUE);

        if (mode->drawableType & GLX_PBUFFER_BIT)
            if (winScreen->has_WGL_ARB_pbuffer)
                SET_ATTR_VALUE(WGL_DRAW_TO_PBUFFER_ARB, TRUE);
    }
    else {
        if (drawableTypeOverride & GLX_PIXMAP_BIT)
            SET_ATTR_VALUE(WGL_DRAW_TO_BITMAP_ARB, TRUE);

        if (drawableTypeOverride & GLX_PBUFFER_BIT)
            if (winScreen->has_WGL_ARB_pbuffer)
                SET_ATTR_VALUE(WGL_DRAW_TO_PBUFFER_ARB, TRUE);
    }

    SET_ATTR_VALUE(0, 0);       // terminator

    /* choose the first match */
    {
        int pixelFormatIndex;

        if (!wglChoosePixelFormatARBWrapper
            (hdc, attribList, NULL, 1, &pixelFormatIndex, &numFormats)) {
            ErrorF("wglChoosePixelFormat error: %s\n", glxWinErrorMessage());
        }
        else {
            if (numFormats > 0) {
                GLWIN_DEBUG_MSG
                    ("wglChoosePixelFormat: chose pixelFormatIndex %d)",
                     pixelFormatIndex);
                return pixelFormatIndex;
            }
            else
                ErrorF("wglChoosePixelFormat couldn't decide\n");
        }
    }

    return 0;
}

/* ---------------------------------------------------------------------- */

#define BITS_AND_SHIFT_TO_MASK(bits,mask) (((1<<(bits))-1) << (mask))

//
// Create the GLXconfigs using DescribePixelFormat()
//
static void
glxWinCreateConfigs(HDC hdc, glxWinScreen * screen)
{
    GLXWinConfig *c, *result, *prev = NULL;
    int numConfigs = 0;
    int i = 0;
    int n = 0;
    PIXELFORMATDESCRIPTOR pfd;

    GLWIN_DEBUG_MSG("glxWinCreateConfigs");

    screen->base.numFBConfigs = 0;
    screen->base.fbconfigs = NULL;

    // get the number of pixelformats
    numConfigs =
        DescribePixelFormat(hdc, 1, sizeof(PIXELFORMATDESCRIPTOR), NULL);
    LogMessage(X_INFO, "%d pixel formats reported by DescribePixelFormat\n",
               numConfigs);

    /* alloc */
    result = malloc(sizeof(GLXWinConfig) * numConfigs);

    if (NULL == result) {
        return;
    }

    memset(result, 0, sizeof(GLXWinConfig) * numConfigs);
    n = 0;

    /* fill in configs */
    for (i = 0; i < numConfigs; i++) {
        int rc;

        c = &(result[i]);
        c->base.next = NULL;
        c->pixelFormatIndex = i + 1;

        rc = DescribePixelFormat(hdc, i + 1, sizeof(PIXELFORMATDESCRIPTOR),
                                 &pfd);

        if (!rc) {
            ErrorF("DescribePixelFormat failed for index %d, error %s\n", i + 1,
                   glxWinErrorMessage());
            break;
        }

#ifdef _DEBUG
        if (glxWinDebugSettings.dumpPFD)
            pfdOut(&pfd);
#endif

        if (!(pfd.dwFlags & (PFD_DRAW_TO_WINDOW | PFD_DRAW_TO_BITMAP)) ||
            !(pfd.dwFlags & PFD_SUPPORT_OPENGL)) {
            GLWIN_DEBUG_MSG
                ("pixelFormat %d has unsuitable flags 0x%08x, skipping", i + 1,
                 (unsigned int)pfd.dwFlags);
            continue;
        }

        c->base.doubleBufferMode =
            (pfd.dwFlags & PFD_DOUBLEBUFFER) ? GL_TRUE : GL_FALSE;
        c->base.stereoMode = (pfd.dwFlags & PFD_STEREO) ? GL_TRUE : GL_FALSE;

        c->base.redBits = pfd.cRedBits;
        c->base.greenBits = pfd.cGreenBits;
        c->base.blueBits = pfd.cBlueBits;
        c->base.alphaBits = pfd.cAlphaBits;

        c->base.redMask = BITS_AND_SHIFT_TO_MASK(pfd.cRedBits, pfd.cRedShift);
        c->base.greenMask =
            BITS_AND_SHIFT_TO_MASK(pfd.cGreenBits, pfd.cGreenShift);
        c->base.blueMask =
            BITS_AND_SHIFT_TO_MASK(pfd.cBlueBits, pfd.cBlueShift);
        c->base.alphaMask =
            BITS_AND_SHIFT_TO_MASK(pfd.cAlphaBits, pfd.cAlphaShift);

        c->base.rgbBits = pfd.cColorBits;

        if (pfd.iPixelType == PFD_TYPE_COLORINDEX) {
            c->base.indexBits = pfd.cColorBits;
        }
        else {
            c->base.indexBits = 0;
        }

        c->base.accumRedBits = pfd.cAccumRedBits;
        c->base.accumGreenBits = pfd.cAccumGreenBits;
        c->base.accumBlueBits = pfd.cAccumBlueBits;
        c->base.accumAlphaBits = pfd.cAccumAlphaBits;
        //  pfd.cAccumBits;

        c->base.depthBits = pfd.cDepthBits;
        c->base.stencilBits = pfd.cStencilBits;
        c->base.numAuxBuffers = pfd.cAuxBuffers;

        // pfd.iLayerType; // ignored
        c->base.level = 0;
        // pfd.dwLayerMask; // ignored
        // pfd.dwDamageMask;  // ignored

        c->base.visualID = -1;  // will be set by __glXScreenInit()

        /* EXT_visual_rating / GLX 1.2 */
        if (pfd.dwFlags & PFD_GENERIC_FORMAT) {
            c->base.visualRating = GLX_SLOW_VISUAL_EXT;
        }
        else {
            // PFD_GENERIC_ACCELERATED is not considered, so this may be MCD or ICD acclerated...
            c->base.visualRating = GLX_NONE_EXT;
        }

        /* EXT_visual_info / GLX 1.2 */
        if (pfd.iPixelType == PFD_TYPE_COLORINDEX) {
            c->base.visualType = GLX_STATIC_COLOR;
            c->base.transparentRed = GLX_NONE;
            c->base.transparentGreen = GLX_NONE;
            c->base.transparentBlue = GLX_NONE;
            c->base.transparentAlpha = GLX_NONE;
            c->base.transparentIndex = pfd.dwVisibleMask;
            c->base.transparentPixel = GLX_TRANSPARENT_INDEX;
        }
        else {
            c->base.visualType = GLX_TRUE_COLOR;
            c->base.transparentRed =
                (pfd.dwVisibleMask & c->base.redMask) >> pfd.cRedShift;
            c->base.transparentGreen =
                (pfd.dwVisibleMask & c->base.greenMask) >> pfd.cGreenShift;
            c->base.transparentBlue =
                (pfd.dwVisibleMask & c->base.blueMask) >> pfd.cBlueShift;
            c->base.transparentAlpha =
                (pfd.dwVisibleMask & c->base.alphaMask) >> pfd.cAlphaShift;
            c->base.transparentIndex = GLX_NONE;
            c->base.transparentPixel = GLX_TRANSPARENT_RGB;
        }

        /* ARB_multisample / SGIS_multisample */
        c->base.sampleBuffers = 0;
        c->base.samples = 0;

        /* SGIX_fbconfig / GLX 1.3 */
        c->base.drawableType =
            (((pfd.dwFlags & PFD_DRAW_TO_WINDOW) ? GLX_WINDOW_BIT : 0)
             | ((pfd.dwFlags & PFD_DRAW_TO_BITMAP) ? GLX_PIXMAP_BIT : 0));

        if (pfd.iPixelType == PFD_TYPE_COLORINDEX) {
            c->base.renderType = GLX_RGBA_BIT | GLX_COLOR_INDEX_BIT;
        }
        else {
            c->base.renderType = GLX_RGBA_BIT;
        }

        c->base.fbconfigID = -1;        // will be set by __glXScreenInit()

        /* SGIX_pbuffer / GLX 1.3 */
        // XXX: How can we find these values out ???
        c->base.maxPbufferWidth = -1;
        c->base.maxPbufferHeight = -1;
        c->base.maxPbufferPixels = -1;
        c->base.optimalPbufferWidth = 0;        // there is no optimal value
        c->base.optimalPbufferHeight = 0;

        /* SGIX_visual_select_group */
        // arrange for visuals with the best acceleration to be preferred in selection
        switch (pfd.dwFlags & (PFD_GENERIC_FORMAT | PFD_GENERIC_ACCELERATED)) {
        case 0:
            c->base.visualSelectGroup = 2;
            break;

        case PFD_GENERIC_ACCELERATED:
            c->base.visualSelectGroup = 1;
            break;

        case PFD_GENERIC_FORMAT:
            c->base.visualSelectGroup = 0;
            break;

        default:
            ;
            // "can't happen"
        }

        /* OML_swap_method */
        if (pfd.dwFlags & PFD_SWAP_EXCHANGE)
            c->base.swapMethod = GLX_SWAP_EXCHANGE_OML;
        else if (pfd.dwFlags & PFD_SWAP_COPY)
            c->base.swapMethod = GLX_SWAP_COPY_OML;
        else
            c->base.swapMethod = GLX_SWAP_UNDEFINED_OML;

        /* EXT_texture_from_pixmap */
        c->base.bindToTextureRgb = -1;
        c->base.bindToTextureRgba = -1;
        c->base.bindToMipmapTexture = -1;
        c->base.bindToTextureTargets = -1;
        c->base.yInverted = -1;
        c->base.sRGBCapable = 0;

        n++;

        // update previous config to point to this config
        if (prev)
            prev->base.next = &(c->base);

        prev = c;
    }

    GLWIN_DEBUG_MSG
        ("found %d pixelFormats suitable for conversion to fbConfigs", n);

    screen->base.numFBConfigs = n;
    screen->base.fbconfigs = &(result->base);
}

// helper function to access an attribute value from an attribute value array by attribute
static
    int
getAttrValue(const int attrs[], int values[], unsigned int num, int attr,
             int fallback)
{
    unsigned int i;

    for (i = 0; i < num; i++) {
        if (attrs[i] == attr) {
            GLWIN_TRACE_MSG("getAttrValue attr 0x%x, value %d", attr,
                            values[i]);
            return values[i];
        }
    }

    ErrorF("getAttrValue failed to find attr 0x%x, using default value %d\n",
           attr, fallback);
    return fallback;
}

//
// Create the GLXconfigs using wglGetPixelFormatAttribfvARB() extension
//
static void
glxWinCreateConfigsExt(HDC hdc, glxWinScreen * screen)
{
    GLXWinConfig *c, *result, *prev = NULL;
    int i = 0;
    int n = 0;

    const int attr = WGL_NUMBER_PIXEL_FORMATS_ARB;
    int numConfigs;

    int attrs[50];
    unsigned int num_attrs = 0;

    GLWIN_DEBUG_MSG("glxWinCreateConfigsExt");

    screen->base.numFBConfigs = 0;
    screen->base.fbconfigs = NULL;

    if (!wglGetPixelFormatAttribivARBWrapper(hdc, 0, 0, 1, &attr, &numConfigs)) {
        ErrorF
            ("wglGetPixelFormatAttribivARB failed for WGL_NUMBER_PIXEL_FORMATS_ARB: %s\n",
             glxWinErrorMessage());
        return;
    }

    LogMessage(X_INFO,
               "%d pixel formats reported by wglGetPixelFormatAttribivARB\n",
               numConfigs);

    /* alloc */
    result = malloc(sizeof(GLXWinConfig) * numConfigs);

    if (NULL == result) {
        return;
    }

    memset(result, 0, sizeof(GLXWinConfig) * numConfigs);
    n = 0;

#define ADD_ATTR(a) { attrs[num_attrs++] = a; assert(num_attrs < NUM_ELEMENTS(attrs)); }

    ADD_ATTR(WGL_DRAW_TO_WINDOW_ARB);
    ADD_ATTR(WGL_DRAW_TO_BITMAP_ARB);
    ADD_ATTR(WGL_ACCELERATION_ARB);
    ADD_ATTR(WGL_SWAP_LAYER_BUFFERS_ARB);
    ADD_ATTR(WGL_NUMBER_OVERLAYS_ARB);
    ADD_ATTR(WGL_NUMBER_UNDERLAYS_ARB);
    ADD_ATTR(WGL_TRANSPARENT_ARB);
    ADD_ATTR(WGL_TRANSPARENT_RED_VALUE_ARB);
    ADD_ATTR(WGL_TRANSPARENT_GREEN_VALUE_ARB);
    ADD_ATTR(WGL_TRANSPARENT_GREEN_VALUE_ARB);
    ADD_ATTR(WGL_TRANSPARENT_ALPHA_VALUE_ARB);
    ADD_ATTR(WGL_SUPPORT_OPENGL_ARB);
    ADD_ATTR(WGL_DOUBLE_BUFFER_ARB);
    ADD_ATTR(WGL_STEREO_ARB);
    ADD_ATTR(WGL_PIXEL_TYPE_ARB);
    ADD_ATTR(WGL_COLOR_BITS_ARB);
    ADD_ATTR(WGL_RED_BITS_ARB);
    ADD_ATTR(WGL_RED_SHIFT_ARB);
    ADD_ATTR(WGL_GREEN_BITS_ARB);
    ADD_ATTR(WGL_GREEN_SHIFT_ARB);
    ADD_ATTR(WGL_BLUE_BITS_ARB);
    ADD_ATTR(WGL_BLUE_SHIFT_ARB);
    ADD_ATTR(WGL_ALPHA_BITS_ARB);
    ADD_ATTR(WGL_ALPHA_SHIFT_ARB);
    ADD_ATTR(WGL_ACCUM_RED_BITS_ARB);
    ADD_ATTR(WGL_ACCUM_GREEN_BITS_ARB);
    ADD_ATTR(WGL_ACCUM_BLUE_BITS_ARB);
    ADD_ATTR(WGL_ACCUM_ALPHA_BITS_ARB);
    ADD_ATTR(WGL_DEPTH_BITS_ARB);
    ADD_ATTR(WGL_STENCIL_BITS_ARB);
    ADD_ATTR(WGL_AUX_BUFFERS_ARB);
    ADD_ATTR(WGL_SWAP_METHOD_ARB);

    if (screen->has_WGL_ARB_multisample) {
        // we may not query these attrs if WGL_ARB_multisample is not offered
        ADD_ATTR(WGL_SAMPLE_BUFFERS_ARB);
        ADD_ATTR(WGL_SAMPLES_ARB);
    }

    if (screen->has_WGL_ARB_render_texture) {
        // we may not query these attrs if WGL_ARB_render_texture is not offered
        ADD_ATTR(WGL_BIND_TO_TEXTURE_RGB_ARB);
        ADD_ATTR(WGL_BIND_TO_TEXTURE_RGBA_ARB);
    }

    if (screen->has_WGL_ARB_pbuffer) {
        // we may not query these attrs if WGL_ARB_pbuffer is not offered
        ADD_ATTR(WGL_DRAW_TO_PBUFFER_ARB);
        ADD_ATTR(WGL_MAX_PBUFFER_PIXELS_ARB);
        ADD_ATTR(WGL_MAX_PBUFFER_WIDTH_ARB);
        ADD_ATTR(WGL_MAX_PBUFFER_HEIGHT_ARB);
    }

    /* fill in configs */
    for (i = 0; i < numConfigs; i++) {
        int sizevalues=num_attrs*sizeof(int);
        int *values=(int*)_alloca(sizevalues);

        memset(values,0,sizevalues);

        c = &(result[i]);
        c->base.next = NULL;
        c->pixelFormatIndex = i + 1;

        if (!wglGetPixelFormatAttribivARBWrapper
            (hdc, i + 1, 0, num_attrs, attrs, values)) {
            ErrorF
                ("wglGetPixelFormatAttribivARB failed for index %d, error %s\n",
                 i + 1, glxWinErrorMessage());
            break;
        }

#define ATTR_VALUE(a, d) getAttrValue(attrs, values, num_attrs, (a), (d))

        if (!ATTR_VALUE(WGL_SUPPORT_OPENGL_ARB, 0)) {
            GLWIN_DEBUG_MSG
                ("pixelFormat %d isn't WGL_SUPPORT_OPENGL_ARB, skipping",
                 i + 1);
            continue;
        }

        c->base.doubleBufferMode =
            ATTR_VALUE(WGL_DOUBLE_BUFFER_ARB, 0) ? GL_TRUE : GL_FALSE;
        c->base.stereoMode = ATTR_VALUE(WGL_STEREO_ARB, 0) ? GL_TRUE : GL_FALSE;

        c->base.redBits = ATTR_VALUE(WGL_RED_BITS_ARB, 0);
        c->base.greenBits = ATTR_VALUE(WGL_GREEN_BITS_ARB, 0);
        c->base.blueBits = ATTR_VALUE(WGL_BLUE_BITS_ARB, 0);
        c->base.alphaBits = ATTR_VALUE(WGL_ALPHA_BITS_ARB, 0);

        c->base.redMask =
            BITS_AND_SHIFT_TO_MASK(c->base.redBits,
                                   ATTR_VALUE(WGL_RED_SHIFT_ARB, 0));
        c->base.greenMask =
            BITS_AND_SHIFT_TO_MASK(c->base.greenBits,
                                   ATTR_VALUE(WGL_GREEN_SHIFT_ARB, 0));
        c->base.blueMask =
            BITS_AND_SHIFT_TO_MASK(c->base.blueBits,
                                   ATTR_VALUE(WGL_BLUE_SHIFT_ARB, 0));
        c->base.alphaMask =
            BITS_AND_SHIFT_TO_MASK(c->base.alphaBits,
                                   ATTR_VALUE(WGL_ALPHA_SHIFT_ARB, 0));

        switch (ATTR_VALUE(WGL_PIXEL_TYPE_ARB, 0)) {
        case WGL_TYPE_COLORINDEX_ARB:
            c->base.indexBits = ATTR_VALUE(WGL_COLOR_BITS_ARB, 0);
            c->base.rgbBits = 0;
            c->base.visualType = GLX_STATIC_COLOR;
            break;

        case WGL_TYPE_RGBA_FLOAT_ARB:
            GLWIN_DEBUG_MSG
                ("pixelFormat %d is WGL_TYPE_RGBA_FLOAT_ARB, skipping", i + 1);
            continue;

        case WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT:
            GLWIN_DEBUG_MSG
                ("pixelFormat %d is WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT, skipping",
                 i + 1);
            continue;

        case WGL_TYPE_RGBA_ARB:
            c->base.indexBits = 0;
            c->base.rgbBits = ATTR_VALUE(WGL_COLOR_BITS_ARB, 0);
            c->base.visualType = GLX_TRUE_COLOR;
            break;

        default:
            ErrorF
                ("wglGetPixelFormatAttribivARB returned unknown value 0x%x for WGL_PIXEL_TYPE_ARB\n",
                 ATTR_VALUE(WGL_PIXEL_TYPE_ARB, 0));
            continue;
        }

        c->base.accumRedBits = ATTR_VALUE(WGL_ACCUM_RED_BITS_ARB, 0);
        c->base.accumGreenBits = ATTR_VALUE(WGL_ACCUM_GREEN_BITS_ARB, 0);
        c->base.accumBlueBits = ATTR_VALUE(WGL_ACCUM_BLUE_BITS_ARB, 0);
        c->base.accumAlphaBits = ATTR_VALUE(WGL_ACCUM_ALPHA_BITS_ARB, 0);

        c->base.depthBits = ATTR_VALUE(WGL_DEPTH_BITS_ARB, 0);
        c->base.stencilBits = ATTR_VALUE(WGL_STENCIL_BITS_ARB, 0);
        c->base.numAuxBuffers = ATTR_VALUE(WGL_AUX_BUFFERS_ARB, 0);

        {
            int layers =
                ATTR_VALUE(WGL_NUMBER_OVERLAYS_ARB,
                           0) + ATTR_VALUE(WGL_NUMBER_UNDERLAYS_ARB, 0);

            if (layers > 0) {
                ErrorF
                    ("pixelFormat %d: has %d overlay, %d underlays which aren't currently handled",
                     i, ATTR_VALUE(WGL_NUMBER_OVERLAYS_ARB, 0),
                     ATTR_VALUE(WGL_NUMBER_UNDERLAYS_ARB, 0));
                // XXX: need to iterate over layers?
            }
        }
        c->base.level = 0;

        c->base.visualID = -1;  // will be set by __glXScreenInit()

        /* EXT_visual_rating / GLX 1.2 */
        switch (ATTR_VALUE(WGL_ACCELERATION_ARB, 0)) {
        default:
            ErrorF
                ("wglGetPixelFormatAttribivARB returned unknown value 0x%x for WGL_ACCELERATION_ARB\n",
                 ATTR_VALUE(WGL_ACCELERATION_ARB, 0));

        case WGL_NO_ACCELERATION_ARB:
            c->base.visualRating = GLX_SLOW_VISUAL_EXT;
            break;

        case WGL_GENERIC_ACCELERATION_ARB:
        case WGL_FULL_ACCELERATION_ARB:
            c->base.visualRating = GLX_NONE_EXT;
            break;
        }

        /* EXT_visual_info / GLX 1.2 */
        // c->base.visualType is set above
        if (ATTR_VALUE(WGL_TRANSPARENT_ARB, 0)) {
            c->base.transparentPixel =
                (c->base.visualType ==
                 GLX_TRUE_COLOR) ? GLX_TRANSPARENT_RGB_EXT :
                GLX_TRANSPARENT_INDEX_EXT;
            c->base.transparentRed =
                ATTR_VALUE(WGL_TRANSPARENT_RED_VALUE_ARB, 0);
            c->base.transparentGreen =
                ATTR_VALUE(WGL_TRANSPARENT_GREEN_VALUE_ARB, 0);
            c->base.transparentBlue =
                ATTR_VALUE(WGL_TRANSPARENT_BLUE_VALUE_ARB, 0);
            c->base.transparentAlpha =
                ATTR_VALUE(WGL_TRANSPARENT_ALPHA_VALUE_ARB, 0);
            c->base.transparentIndex =
                ATTR_VALUE(WGL_TRANSPARENT_INDEX_VALUE_ARB, 0);
        }
        else {
            c->base.transparentPixel = GLX_NONE_EXT;
            c->base.transparentRed = GLX_NONE;
            c->base.transparentGreen = GLX_NONE;
            c->base.transparentBlue = GLX_NONE;
            c->base.transparentAlpha = GLX_NONE;
            c->base.transparentIndex = GLX_NONE;
        }

        /* ARB_multisample / SGIS_multisample */
        if (screen->has_WGL_ARB_multisample) {
            c->base.sampleBuffers = ATTR_VALUE(WGL_SAMPLE_BUFFERS_ARB, 0);
            c->base.samples = ATTR_VALUE(WGL_SAMPLES_ARB, 0);
        }
        else {
            c->base.sampleBuffers = 0;
            c->base.samples = 0;
        }

        /* SGIX_fbconfig / GLX 1.3 */
        c->base.drawableType =
            ((ATTR_VALUE(WGL_DRAW_TO_WINDOW_ARB, 0) ? GLX_WINDOW_BIT : 0)
             | (ATTR_VALUE(WGL_DRAW_TO_BITMAP_ARB, 0) ? GLX_PIXMAP_BIT : 0)
             | (ATTR_VALUE(WGL_DRAW_TO_PBUFFER_ARB, 0) ? GLX_PBUFFER_BIT : 0));

        /*
           Assume OpenGL RGBA rendering is available on all visuals
           (it is specified to render to red component in single-channel visuals,
           if supported, but there doesn't seem to be any mechanism to check if it
           is supported)

           Color index rendering is only supported on single-channel visuals
         */
        if (c->base.visualType == GLX_STATIC_COLOR) {
            c->base.renderType = GLX_RGBA_BIT | GLX_COLOR_INDEX_BIT;
        }
        else {
            c->base.renderType = GLX_RGBA_BIT;
        }

        c->base.fbconfigID = -1;        // will be set by __glXScreenInit()

        /* SGIX_pbuffer / GLX 1.3 */
        if (screen->has_WGL_ARB_pbuffer) {
            c->base.maxPbufferWidth = ATTR_VALUE(WGL_MAX_PBUFFER_WIDTH_ARB, -1);
            c->base.maxPbufferHeight =
                ATTR_VALUE(WGL_MAX_PBUFFER_HEIGHT_ARB, -1);
            c->base.maxPbufferPixels =
                ATTR_VALUE(WGL_MAX_PBUFFER_PIXELS_ARB, -1);
        }
        else {
            c->base.maxPbufferWidth = -1;
            c->base.maxPbufferHeight = -1;
            c->base.maxPbufferPixels = -1;
        }
        c->base.optimalPbufferWidth = 0;        // there is no optimal value
        c->base.optimalPbufferHeight = 0;

        /* SGIX_visual_select_group */
        // arrange for visuals with the best acceleration to be preferred in selection
        switch (ATTR_VALUE(WGL_ACCELERATION_ARB, 0)) {
        case WGL_FULL_ACCELERATION_ARB:
            c->base.visualSelectGroup = 2;
            break;

        case WGL_GENERIC_ACCELERATION_ARB:
            c->base.visualSelectGroup = 1;
            break;

        default:
        case WGL_NO_ACCELERATION_ARB:
            c->base.visualSelectGroup = 0;
            break;
        }

        /* OML_swap_method */
        switch (ATTR_VALUE(WGL_SWAP_METHOD_ARB, 0)) {
        case WGL_SWAP_EXCHANGE_ARB:
            c->base.swapMethod = GLX_SWAP_EXCHANGE_OML;
            break;

        case WGL_SWAP_COPY_ARB:
            c->base.swapMethod = GLX_SWAP_COPY_OML;
            break;

        default:
            ErrorF
                ("wglGetPixelFormatAttribivARB returned unknown value 0x%x for WGL_SWAP_METHOD_ARB\n",
                 ATTR_VALUE(WGL_SWAP_METHOD_ARB, 0));

        case WGL_SWAP_UNDEFINED_ARB:
            c->base.swapMethod = GLX_SWAP_UNDEFINED_OML;
        }

        /* EXT_texture_from_pixmap */
        /*
           Mesa's DRI configs always have bindToTextureRgb/Rgba TRUE (see driCreateConfigs(), so setting
           bindToTextureRgb/bindToTextureRgba to FALSE means that swrast can't find any fbConfigs to use,
           so setting these to 0, even if we know bindToTexture isn't available, isn't a good idea...
         */
        if (screen->has_WGL_ARB_render_texture) {
            c->base.bindToTextureRgb =
                ATTR_VALUE(WGL_BIND_TO_TEXTURE_RGB_ARB, -1);
            c->base.bindToTextureRgba =
                ATTR_VALUE(WGL_BIND_TO_TEXTURE_RGBA_ARB, -1);
        }
        else {
            c->base.bindToTextureRgb = -1;
            c->base.bindToTextureRgba = -1;
        }
        c->base.bindToMipmapTexture = -1;
        c->base.bindToTextureTargets =
            GLX_TEXTURE_1D_BIT_EXT | GLX_TEXTURE_2D_BIT_EXT |
            GLX_TEXTURE_RECTANGLE_BIT_EXT;
        c->base.yInverted = -1;
        c->base.sRGBCapable = 0;

        n++;

        // update previous config to point to this config
        if (prev)
            prev->base.next = &(c->base);

        prev = c;
    }

    screen->base.numFBConfigs = n;
    screen->base.fbconfigs = &(result->base);
}