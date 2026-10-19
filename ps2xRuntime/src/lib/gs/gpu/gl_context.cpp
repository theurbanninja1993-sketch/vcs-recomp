#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // before gl_context.h, which otherwise defines APIENTRY itself
#endif

#include "runtime/gs/gpu/gl_context.h"

#include <cstdio>

namespace gsgpu
{
    std::atomic<uintptr_t> g_hostSharedGlContext{0u};
    std::atomic<uintptr_t> g_hostSharedGlDisplay{0u};
}

#if defined(_WIN32)

namespace gsgpu
{
    namespace
    {
        // WGL_ARB_create_context / _profile
        constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
        constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
        constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
        constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
        using CreateContextAttribsFn = HGLRC(WINAPI *)(HDC, HGLRC, const int *);

        const wchar_t *kWindowClass = L"ps2x-gsgpu";

        // Extension functions come from wglGetProcAddress; the GL 1.1 core
        // ones only from opengl32.dll itself.
        void *glProc(const char *name)
        {
            PROC proc = wglGetProcAddress(name);
            const auto value = reinterpret_cast<intptr_t>(proc);
            if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
            {
                static HMODULE s_opengl = LoadLibraryW(L"opengl32.dll");
                proc = s_opengl ? GetProcAddress(s_opengl, name) : nullptr;
            }
            return reinterpret_cast<void *>(proc);
        }
    }

    GLContext::~GLContext()
    {
        destroy();
    }

    bool GLContext::create(std::string &error)
    {
        // A hidden window only provides a pixel format; rendering goes to
        // framebuffer objects.
        HINSTANCE instance = GetModuleHandleW(nullptr);
        static const bool s_registered = [instance]()
        {
            WNDCLASSW wc{};
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = kWindowClass;
            return RegisterClassW(&wc) != 0;
        }();
        (void)s_registered;
        // Prefer the host window's DC: creating the context on the same DC as
        // raylib's means the same pixel format, which is what WGL needs to
        // share objects reliably. A private window picks its own format, and
        // sharing then produced a corrupt display texture - which is why the
        // renderer was left on the readback path (correct but a full frame
        // round-trip through the CPU every frame, scaling with resolution).
        HWND window = nullptr;
        HDC dc = reinterpret_cast<HDC>(g_hostSharedGlDisplay.load(std::memory_order_relaxed));
        const bool ownDc = dc == nullptr;
        if (ownDc)
        {
            window = CreateWindowExW(0, kWindowClass, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
            if (!window)
            {
                error = "CreateWindowEx failed";
                return false;
            }
            dc = GetDC(window);
        }
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        if (ownDc)
        {
            // The host's DC already carries raylib's pixel format; setting one
            // on it again would fail.
            const int format = ChoosePixelFormat(dc, &pfd);
            if (format == 0 || !SetPixelFormat(dc, format, &pfd))
            {
                ReleaseDC(window, dc);
                DestroyWindow(window);
                error = "no OpenGL pixel format";
                return false;
            }
        }
        HGLRC legacy = wglCreateContext(dc);
        if (!legacy || !wglMakeCurrent(dc, legacy))
        {
            if (legacy)
                wglDeleteContext(legacy);
            if (ownDc)
            {
                ReleaseDC(window, dc);
                DestroyWindow(window);
            }
            error = "wglCreateContext failed";
            return false;
        }
        auto createContextAttribs = reinterpret_cast<CreateContextAttribsFn>(wglGetProcAddress("wglCreateContextAttribsARB"));
        // The renderer only needs core 4.5 (direct state access); ask for 4.6
        // first and take 4.5 if the driver caps there, so machines with older
        // drivers still get the GPU renderer instead of the CPU fallback.
        auto createCoreContext = [&](const int major, const int minor) -> HGLRC
        {
            const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, major,
                                      WGL_CONTEXT_MINOR_VERSION_ARB, minor,
                                      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
            return createContextAttribs ? createContextAttribs(dc, nullptr, attributes) : nullptr;
        };
        // Share with the host window's context when it published one (the
        // game): the composed display texture can then be drawn by the host
        // with no readback or re-upload.
        auto shareContext = reinterpret_cast<HGLRC>(g_hostSharedGlContext.load(std::memory_order_relaxed));
        const int requestedMinor = []()
        {
            const char *env = std::getenv("PS2X_GL_VERSION");
            return env != nullptr ? std::atoi(env) : 6;
        }();
        // Each version is tried shared first, then unshared on the same DC. Sharing
        // is what lets the composed picture be handed to the host with no readback,
        // so it is worth knowing precisely which part a driver objects to: the share
        // (fixable by matching the two contexts) or the DC itself (not). The Windows
        // error code is the only thing that says which, and nothing was logging it.
        HGLRC context = nullptr;
        // Whether the context that was actually made is sharing. Not the same as
        // "a share context existed": the fallback below creates one that does not
        // share, and the present path must not be told otherwise or it will hand the
        // host a texture from a context it cannot see.
        bool usedShare = false;
        for (int minor = requestedMinor; minor >= 5 && context == nullptr; --minor)
        {
            if (createContextAttribs == nullptr)
                break;
            const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, minor,
                                      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
            context = createContextAttribs(dc, shareContext, attributes);
            if (context != nullptr)
            {
                usedShare = shareContext != nullptr;
                break;
            }
            if (shareContext != nullptr)
            {
                std::fprintf(stderr, "[gs:gpu] shared 4.%d core refused (Windows error %lu)\n", minor,
                             static_cast<unsigned long>(GetLastError()));
                context = createContextAttribs(dc, nullptr, attributes);
                if (context != nullptr)
                    std::fprintf(stderr, "[gs:gpu] 4.%d core works on this DC when unshared: it is the share it "
                                         "objects to\n", minor);
                else
                    std::fprintf(stderr, "[gs:gpu] 4.%d core refused on this DC as well (Windows error %lu)\n", minor,
                                 static_cast<unsigned long>(GetLastError()));
            }
        }
        m_shared = context != nullptr && usedShare;
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (!context || !wglMakeCurrent(dc, context))
        {
            if (context)
                wglDeleteContext(context);
            if (ownDc)
            {
                ReleaseDC(window, dc);
                DestroyWindow(window);
            }
            // Second attempt, on a private window with nothing shared. A driver may
            // refuse a core context on the host's DC, or refuse to share across the
            // two context versions, and this is the arrangement that works when it
            // does: the composed picture comes back through a framebuffer readback,
            // which costs a round-trip per frame and is still a great deal better
            // than the CPU rasterizer. What was happening instead - on a Windows
            // laptop, with the host's DC and its context shared - was falling all
            // the way to the rasterizer because this was the only attempt made.
            //
            // Once only, or this would recurse: the retry suppresses both published
            // values, so the recursive call takes the private-window path.
            static bool s_triedPrivate = false;
            if (!s_triedPrivate && !ownDc)
            {
                s_triedPrivate = true;
                const uintptr_t dcSaved = g_hostSharedGlDisplay.exchange(0u, std::memory_order_relaxed);
                const uintptr_t ctxSaved = g_hostSharedGlContext.exchange(0u, std::memory_order_relaxed);
                std::fprintf(stderr, "[gs:gpu] shared core context on the host's DC failed; retrying unshared, "
                                     "on a private window\n");
                const bool ok = create(error);
                g_hostSharedGlDisplay.store(dcSaved, std::memory_order_relaxed);
                g_hostSharedGlContext.store(ctxSaved, std::memory_order_relaxed);
                if (ok)
                {
                    std::fprintf(stderr, "[gs:gpu] running unshared: the display will go through a readback\n");
                    return true;
                }
            }
            error = "wglCreateContextAttribsARB (GL 4.6 core) failed";
            return false;
        }
        m_ownsDc = ownDc;
        m_display = window; // null when the DC belongs to the host
        m_dc = dc;
        m_context = context;

#define GSGPU_LOAD(type, name)                                                            \
    gl.name = reinterpret_cast<type>(glProc(#name));                                      \
    if (!gl.name)                                                                         \
    {                                                                                     \
        error = "missing GL function " #name;                                             \
        destroy();                                                                        \
        return false;                                                                     \
    }
        GSGPU_GL_FUNCTIONS(GSGPU_LOAD)
#undef GSGPU_LOAD
        return true;
    }

    void GLContext::destroy()
    {
        if (!m_context && !m_dc)
            return;
        wglMakeCurrent(nullptr, nullptr);
        if (m_context)
            wglDeleteContext(static_cast<HGLRC>(m_context));
        // Only release what we created: the DC may be the host window's.
        if (m_ownsDc && m_dc)
        {
            ReleaseDC(static_cast<HWND>(m_display), static_cast<HDC>(m_dc));
            DestroyWindow(static_cast<HWND>(m_display));
        }
        m_context = nullptr;
        m_dc = nullptr;
        m_display = nullptr;
    }
}

#else

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

namespace gsgpu
{
    namespace
    {
        EGLDisplay surfacelessDisplay()
        {
            auto getPlatformDisplay =
                reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (getPlatformDisplay)
            {
                EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
                if (display != EGL_NO_DISPLAY)
                    return display;
            }
            return eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
    }

    GLContext::~GLContext()
    {
        destroy();
    }

    // A GLX context sharing the host window's (raylib's, X11/GLX): the
    // renderer's composed display texture can then be drawn by the host with
    // no readback or re-upload. Rendering goes to FBOs; the tiny override-
    // redirect X window below only provides the drawable GL requires.
    bool GLContext::createGlxShared(std::string &error)
    {
        const auto hostContext = reinterpret_cast<GLXContext>(g_hostSharedGlContext.load(std::memory_order_relaxed));
        if (hostContext == nullptr)
        {
            error = "no host GL context";
            return false;
        }
        Display *display = XOpenDisplay(nullptr); // our own connection: own thread
        if (display == nullptr)
        {
            error = "XOpenDisplay failed";
            return false;
        }
        static int fbAttribs[] = {GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_DOUBLEBUFFER, True,
                                  GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None};
        int fbCount = 0;
        GLXFBConfig *configs = glXChooseFBConfig(display, DefaultScreen(display), fbAttribs, &fbCount);
        if (configs == nullptr || fbCount == 0)
        {
            XCloseDisplay(display);
            error = "no GLX FBConfig";
            return false;
        }
        auto createAttribs =
            reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glXCreateContextAttribsARB")));
        if (createAttribs == nullptr)
        {
            XCloseDisplay(display);
            error = "no glXCreateContextAttribsARB";
            return false;
        }
        const int requestedMinor = []()
        {
            const char *env = std::getenv("PS2X_GL_VERSION");
            return env != nullptr ? std::atoi(env) : 6;
        }();
        GLXContext context = nullptr;
        for (int minor = requestedMinor; minor >= 5 && context == nullptr; --minor)
        {
            const int attributes[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, minor,
                                      GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
            context = createAttribs(display, configs[0], hostContext, True, attributes);
        }
        if (context == nullptr)
        {
            XCloseDisplay(display);
            error = "glXCreateContextAttribsARB (shared, GL 4.5 core) failed";
            return false;
        }
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        Window window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, 16, 16, 0, CopyFromParent, InputOutput,
                                      CopyFromParent, CWOverrideRedirect, &attributes);
        if (window == 0 || !glXMakeContextCurrent(display, window, window, context))
        {
            if (window != 0)
                XDestroyWindow(display, window);
            glXDestroyContext(display, context);
            XCloseDisplay(display);
            error = "glXMakeContextCurrent failed";
            return false;
        }
        m_glxOwnDisplay = display;
        m_window = reinterpret_cast<void *>(static_cast<uintptr_t>(window));
        m_display = display;
        m_context = context;
        m_shared = true;

#define GSGPU_LOAD(type, name)                                                       \
    gl.name = reinterpret_cast<type>(glXGetProcAddressARB(reinterpret_cast<const GLubyte *>(#name))); \
    if (!gl.name)                                                                    \
    {                                                                                \
        error = "missing GL function " #name;                                        \
        destroy();                                                                   \
        return false;                                                                \
    }
        GSGPU_GL_FUNCTIONS(GSGPU_LOAD)
#undef GSGPU_LOAD
        return true;
    }

    bool GLContext::create(std::string &error)
    {
        // Sharing the window's context lets the renderer hand the composed
        // display over as a texture: no readback, no upload, no CPU copies.
        // PS2X_GS_PRESENT_SHARE=0 keeps the renderer on its own surfaceless
        // context (with the readback path) - the fallback if a driver shares
        // badly, and the only option in tools (gs_replay has no window).
        static const bool s_allowShare = []()
        {
            const char *env = std::getenv("PS2X_GS_PRESENT_SHARE");
            return env == nullptr || std::atoi(env) != 0;
        }();
        if (s_allowShare && g_hostSharedGlContext.load(std::memory_order_relaxed) != 0u)
        {
            if (createGlxShared(error))
                return true;
            std::fprintf(stderr, "[gs:gpu] GL sharing unavailable (%s); using a surfaceless context\n", error.c_str());
            error.clear();
        }
        EGLDisplay display = surfacelessDisplay();
        if (display == EGL_NO_DISPLAY)
        {
            error = "no EGL display";
            return false;
        }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(display, &major, &minor))
        {
            error = "eglInitialize failed";
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_API))
        {
            error = "eglBindAPI(OpenGL) failed";
            return false;
        }
        // Core 4.5 is all the renderer uses (direct state access); ask for 4.6
        // first and fall back to 4.5 so drivers that cap there still get the
        // GPU renderer instead of the CPU rasterizer.
        const int requestedMinor = []()
        {
            const char *env = std::getenv("PS2X_GL_VERSION");
            return env != nullptr ? std::atoi(env) : 6;
        }();
        EGLContext context = EGL_NO_CONTEXT;
        for (int minor = requestedMinor; minor >= 5 && context == EGL_NO_CONTEXT; --minor)
        {
            const EGLint contextAttributes[] = {
                EGL_CONTEXT_MAJOR_VERSION, 4,
                EGL_CONTEXT_MINOR_VERSION, minor,
                EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                EGL_NONE};
            // Surfaceless: no config, no surface (EGL_KHR_no_config_context +
            // EGL_KHR_surfaceless_context); rendering goes to framebuffer objects.
            context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttributes);
        }
        if (context == EGL_NO_CONTEXT)
        {
            error = "eglCreateContext (GL 4.6 core) failed";
            return false;
        }
        if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        {
            eglDestroyContext(display, context);
            error = "eglMakeCurrent failed";
            return false;
        }
        m_display = display;
        m_context = context;

#define GSGPU_LOAD(type, name)                                                            \
    gl.name = reinterpret_cast<type>(eglGetProcAddress(#name));                           \
    if (!gl.name)                                                                         \
    {                                                                                     \
        error = "missing GL function " #name;                                             \
        destroy();                                                                        \
        return false;                                                                     \
    }
        GSGPU_GL_FUNCTIONS(GSGPU_LOAD)
#undef GSGPU_LOAD
        return true;
    }

    void GLContext::destroy()
    {
        if (m_glxOwnDisplay != nullptr)
        {
            auto *display = static_cast<Display *>(m_glxOwnDisplay);
            glXMakeContextCurrent(display, None, None, nullptr);
            if (m_context != nullptr)
                glXDestroyContext(display, static_cast<GLXContext>(m_context));
            if (m_window != nullptr)
                XDestroyWindow(display, static_cast<Window>(reinterpret_cast<uintptr_t>(m_window)));
            XCloseDisplay(display);
            m_glxOwnDisplay = nullptr;
            m_window = nullptr;
            m_context = nullptr;
            m_display = nullptr;
            m_shared = false;
            return;
        }
        if (!m_display)
            return;
        auto display = static_cast<EGLDisplay>(m_display);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context)
            eglDestroyContext(display, static_cast<EGLContext>(m_context));
        m_context = nullptr;
        m_display = nullptr;
    }
}

#endif
