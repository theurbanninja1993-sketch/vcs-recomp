#pragma once

// Headless OpenGL 4.6 core context for the GPU GS renderer (EGL surfaceless
// on Linux, a hidden window's WGL context on Windows), plus the GL entry
// points it uses.

#if defined(_WIN32) && !defined(APIENTRY)
// glcorearb.h would pull in <windows.h> for these, which clashes with raylib.
#define APIENTRY __stdcall
#define GSGPU_DEFINED_APIENTRY
#endif
#include <GL/glcorearb.h>
#include <atomic>

#include <string>

namespace gsgpu
{
    // X(type, name) for every GL function the renderer calls.
#define GSGPU_GL_FUNCTIONS(X)                                              \
    X(PFNGLGETSTRINGPROC, glGetString)                                     \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                 \
    X(PFNGLGETERRORPROC, glGetError)                                       \
    X(PFNGLENABLEPROC, glEnable)                                           \
    X(PFNGLDISABLEPROC, glDisable)                                         \
    X(PFNGLVIEWPORTPROC, glViewport)                                       \
    X(PFNGLSCISSORPROC, glScissor)                                         \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                   \
    X(PFNGLCLEARPROC, glClear)                                             \
    X(PFNGLFINISHPROC, glFinish)                                           \
    X(PFNGLFLUSHPROC, glFlush)                                             \
    X(PFNGLCOLORMASKPROC, glColorMask)                                     \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                     \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                     \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)             \
    X(PFNGLBLENDCOLORPROC, glBlendColor)                                   \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                 \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                   \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                   \
    X(PFNGLTEXTUREBARRIERPROC, glTextureBarrier)                           \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)               \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                           \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                           \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                       \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                     \
    X(PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC, glCompressedTextureSubImage2D) \
    X(PFNGLTEXTUREPARAMETERFPROC, glTextureParameterf)                     \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)                     \
    X(PFNGLGETTEXTURESUBIMAGEPROC, glGetTextureSubImage)                   \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)                         \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData)                       \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                   \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                   \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)         \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                         \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC, glNamedFramebufferDrawBuffer)   \
    X(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer)               \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers)                             \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                             \
    X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage)                   \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)                   \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                           \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays)                   \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                   \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                         \
    X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer)         \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib)         \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat)         \
    X(PFNGLVERTEXARRAYATTRIBIFORMATPROC, glVertexArrayAttribIFormat)       \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding)       \
    X(PFNGLCREATESHADERPROC, glCreateShader)                               \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                               \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                             \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                 \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                       \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                               \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                             \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                               \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                 \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                               \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                     \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                             \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                   \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                   \
    X(PFNGLPROGRAMUNIFORM1IPROC, glProgramUniform1i)                       \
    X(PFNGLPROGRAMUNIFORM1UIPROC, glProgramUniform1ui)                     \
    X(PFNGLPROGRAMUNIFORM2FPROC, glProgramUniform2f)                       \
    X(PFNGLPROGRAMUNIFORM4FPROC, glProgramUniform4f)                       \
    X(PFNGLPROGRAMUNIFORM4UIPROC, glProgramUniform4ui)                     \
    X(PFNGLPROGRAMUNIFORM3FPROC, glProgramUniform3f)                       \
    X(PFNGLPROGRAMUNIFORM1FPROC, glProgramUniform1f)                       \
    X(PFNGLCLIPCONTROLPROC, glClipControl)                                 \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                             \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv)        \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute)                         \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier)                             \
    X(PFNGLGETNAMEDBUFFERSUBDATAPROC, glGetNamedBufferSubData)             \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                   \
    X(PFNGLMAPNAMEDBUFFERRANGEPROC, glMapNamedBufferRange)                 \
    X(PFNGLUNMAPNAMEDBUFFERPROC, glUnmapNamedBuffer)                       \
    X(PFNGLFENCESYNCPROC, glFenceSync)                                     \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)                           \
    X(PFNGLDELETESYNCPROC, glDeleteSync)                                   \
    X(PFNGLGENQUERIESPROC, glGenQueries)                                   \
    X(PFNGLBEGINQUERYPROC, glBeginQuery)                                   \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                         \
    X(PFNGLCREATESAMPLERSPROC, glCreateSamplers)                           \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                           \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                     \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                 \
    X(PFNGLENDQUERYPROC, glEndQuery)                                       \
    X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv)                       \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)                 \
    X(PFNGLPROGRAMUNIFORM2UIPROC, glProgramUniform2ui)                     \
    X(PFNGLPROGRAMUNIFORM2IPROC, glProgramUniform2i)                       \
    X(PFNGLPROGRAMUNIFORM3UIPROC, glProgramUniform3ui)                     \
    X(PFNGLPROGRAMUNIFORM4IPROC, glProgramUniform4i)

    struct GL
    {
#define GSGPU_DECLARE(type, name) type name = nullptr;
        GSGPU_GL_FUNCTIONS(GSGPU_DECLARE)
#undef GSGPU_DECLARE
    };

    // The host window's native GL context (raylib's), published by the main
    // thread before the renderer starts. When set, the renderer creates its
    // context in that context's share group, so the composed display texture
    // can be handed to the host and drawn directly - no readback, no upload.
    // Linux: GLXContext + Display*, Windows: HGLRC + HDC.
    extern std::atomic<uintptr_t> g_hostSharedGlContext;
    extern std::atomic<uintptr_t> g_hostSharedGlDisplay;

    // Owns the context. create() makes it current on the calling thread and
    // loads `gl`; all GL use must stay on that thread.
    class GLContext
    {
    public:
        GLContext() = default;
        ~GLContext();
        GLContext(const GLContext &) = delete;
        GLContext &operator=(const GLContext &) = delete;

        bool create(std::string &error);
        void destroy();
        [[nodiscard]] bool valid() const { return m_context != nullptr; }

        GL gl;

        // True when the context shares the host window's share group, so the
        // renderer's textures can be drawn by the host.
        [[nodiscard]] bool sharedWithHost() const { return m_shared; }

    private:
        bool createGlxShared(std::string &error);
        void *m_display = nullptr; // EGLDisplay (Windows: the hidden HWND)
        void *m_context = nullptr; // EGLContext (Windows: HGLRC)
        void *m_dc = nullptr;      // Windows: the window's HDC
        bool m_ownsDc = true;      // false when the DC is the host window's
        void *m_window = nullptr;  // GLX: the hidden X window (the drawable)
        void *m_glxOwnDisplay = nullptr; // GLX: our own X connection
        bool m_shared = false;
    };
}
