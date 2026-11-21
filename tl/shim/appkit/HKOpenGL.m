/* OpenGL for Mac apps without macOS: the OpenGL.framework functions a program imports (gl*, CGL*) and AppKit's
   NSOpenGLPixelFormat, NSOpenGLContext and NSOpenGLView, on whatever GL the host has.

   - iOS: an EAGLContext (OpenGL ES 3). Desktop calls ES lacks are translated (glClearDepth, glMapBuffer, the APPLE
     vertex array objects, legacy texture and renderbuffer formats, desktop GLSL without default precision) or kept
     as state only (glAlphaFunc and the fixed-function enables).
   - Mac (the test runner tlios): the Mac's own OpenGL (a legacy 2.1 CGL context), loaded at run time, so the program
     itself never links OpenGL.framework (its imports are refused as on iOS) but its calls take the same path.

   Either way the program's "default framebuffer" (framebuffer 0) is a framebuffer object of the context's
   (colour, depth 24, stencil 8) sized to the view it is attached to; binding 0 binds it and asking for the binding
   answers 0 while it is bound. flushBuffer (CGLFlushDrawable) reads it back and shows it as the contents of a layer
   at the bottom of the view's layer, on the main thread.

   Nothing here calls a gl* or CGL* function by name: those names are this file's own (defined for the program with
   asm labels, found by hk_lookup through the generated symbol table), so the host's functions are reached through
   a table filled with dlsym. Switches: HK_GL_SHADER_DUMP=DIR writes every shader the program compiles, as given and
   as translated for OpenGL ES, to DIR (Mac); HK_GL_LOG=1 logs contexts, framebuffer sizes and presents. */
#import "HKShim.h"
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#import <IOSurface/IOSurfaceRef.h>
#if !TARGET_OS_OSX
#import <OpenGLES/EAGL.h>
#endif

#define HK_GL_FN(ret, name, args) \
    ret hk_##name args __asm__("_" #name); \
    ret hk_##name args

typedef uint32_t GLenum;
typedef uint32_t GLbitfield;
typedef uint32_t GLuint;
typedef int32_t GLint;
typedef int32_t GLsizei;
typedef uint8_t GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef uint8_t GLubyte;

enum {
    GL_NO_ERROR = 0, GL_INVALID_ENUM = 0x0500,
    GL_POINT_SMOOTH = 0x0B10, GL_LINE_SMOOTH = 0x0B20, GL_LINE_STIPPLE = 0x0B24, GL_POLYGON_SMOOTH = 0x0B41,
    GL_LIGHTING = 0x0B50, GL_COLOR_MATERIAL = 0x0B57, GL_FOG = 0x0B60, GL_NORMALIZE = 0x0BA1, GL_ALPHA_TEST = 0x0BC0,
    GL_ALPHA_TEST_FUNC = 0x0BC1, GL_ALPHA_TEST_REF = 0x0BC2, GL_COLOR_LOGIC_OP = 0x0BF2, GL_TEXTURE_1D = 0x0DE0,
    GL_TEXTURE_2D = 0x0DE1, GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_ALIGNMENT = 0x0D05, GL_POLYGON_OFFSET_POINT = 0x2A01,
    GL_POLYGON_OFFSET_LINE = 0x2A02, GL_CLIP_PLANE0 = 0x3000, GL_CLIP_PLANE5 = 0x3005, GL_MULTISAMPLE = 0x809D,
    GL_TEXTURE_RECTANGLE = 0x84F5, GL_DEPTH_CLAMP = 0x864F, GL_VERTEX_PROGRAM_POINT_SIZE = 0x8642,
    GL_POINT_SPRITE = 0x8861, GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F, GL_FRAMEBUFFER_SRGB = 0x8DB9,
    GL_PRIMITIVE_RESTART = 0x8F9D,
    GL_ALWAYS = 0x0207, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_HALF_FLOAT_OES = 0x8D61,
    GL_STENCIL_INDEX = 0x1901, GL_DEPTH_COMPONENT = 0x1902, GL_RED = 0x1903, GL_ALPHA = 0x1906, GL_RGB = 0x1907,
    GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A, GL_BGRA = 0x80E1, GL_RG = 0x8227,
    GL_R8 = 0x8229, GL_RG8 = 0x822B, GL_ALPHA8 = 0x803C, GL_LUMINANCE8 = 0x8040, GL_LUMINANCE8_ALPHA8 = 0x8045,
    GL_RGB8 = 0x8051, GL_RGBA4 = 0x8056, GL_RGB5_A1 = 0x8057, GL_RGBA8 = 0x8058, GL_RGB565 = 0x8D62,
    GL_DEPTH_COMPONENT16 = 0x81A5, GL_DEPTH_COMPONENT24 = 0x81A6, GL_DEPTH_COMPONENT32 = 0x81A7,
    GL_DEPTH_STENCIL = 0x84F9, GL_DEPTH24_STENCIL8 = 0x88F0, GL_STENCIL_INDEX8 = 0x8D48,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C, GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31,
    GL_SHADER_TYPE = 0x8B4F, GL_BUFFER_SIZE = 0x8764, GL_READ_ONLY = 0x88B8, GL_WRITE_ONLY = 0x88B9,
    GL_READ_WRITE = 0x88BA, GL_MAP_READ_BIT = 1, GL_MAP_WRITE_BIT = 2, GL_PIXEL_PACK_BUFFER = 0x88EB,
    GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED, GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41,
    GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_RENDERBUFFER_BINDING = 0x8CA7, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_STENCIL_ATTACHMENT = 0x8D20,
    GL_VERTEX_ARRAY_BINDING = 0x85B5, GL_TEXTURE_BINDING_2D = 0x8069, GL_TEXTURE_BINDING_RECTANGLE = 0x84F6,
    GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_LINEAR = 0x2601,
    GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367,
};

static int g_log;
#define GLLOG(...) do { if (g_log) hk_log(__VA_ARGS__); } while (0)

/* ================================================================ the host's GL, through a table */

/* Functions with the same name and meaning on desktop GL and OpenGL ES 2/3. */
#define HK_GL_VOID(X) \
    X(ActiveTexture, (GLenum a), (a)) \
    X(AttachShader, (GLuint a, GLuint b), (a, b)) \
    X(BindAttribLocation, (GLuint a, GLuint b, const GLchar *c), (a, b, c)) \
    X(BindBuffer, (GLenum a, GLuint b), (a, b)) \
    X(BindRenderbuffer, (GLenum a, GLuint b), (a, b)) \
    X(BindTexture, (GLenum a, GLuint b), (a, b)) \
    X(BlendColor, (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
    X(BlendEquation, (GLenum a), (a)) \
    X(BlendEquationSeparate, (GLenum a, GLenum b), (a, b)) \
    X(BlendFunc, (GLenum a, GLenum b), (a, b)) \
    X(BlendFuncSeparate, (GLenum a, GLenum b, GLenum c, GLenum d), (a, b, c, d)) \
    X(BufferData, (GLenum a, GLsizeiptr b, const void *c, GLenum d), (a, b, c, d)) \
    X(BufferSubData, (GLenum a, GLintptr b, GLsizeiptr c, const void *d), (a, b, c, d)) \
    X(Clear, (GLbitfield a), (a)) \
    X(ClearColor, (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
    X(ClearStencil, (GLint a), (a)) \
    X(ColorMask, (GLboolean a, GLboolean b, GLboolean c, GLboolean d), (a, b, c, d)) \
    X(CompileShader, (GLuint a), (a)) \
    X(CompressedTexImage2D, (GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const void *h), (a, b, c, d, e, f, g, h)) \
    X(CompressedTexSubImage2D, (GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLsizei h, const void *i), (a, b, c, d, e, f, g, h, i)) \
    X(CopyTexImage2D, (GLenum a, GLint b, GLenum c, GLint d, GLint e, GLsizei f, GLsizei g, GLint h), (a, b, c, d, e, f, g, h)) \
    X(CopyTexSubImage2D, (GLenum a, GLint b, GLint c, GLint d, GLint e, GLint f, GLsizei g, GLsizei h), (a, b, c, d, e, f, g, h)) \
    X(CullFace, (GLenum a), (a)) \
    X(DeleteBuffers, (GLsizei a, const GLuint *b), (a, b)) \
    X(DeleteProgram, (GLuint a), (a)) \
    X(DeleteRenderbuffers, (GLsizei a, const GLuint *b), (a, b)) \
    X(DeleteShader, (GLuint a), (a)) \
    X(DeleteTextures, (GLsizei a, const GLuint *b), (a, b)) \
    X(DepthFunc, (GLenum a), (a)) \
    X(DepthMask, (GLboolean a), (a)) \
    X(DetachShader, (GLuint a, GLuint b), (a, b)) \
    X(DisableVertexAttribArray, (GLuint a), (a)) \
    X(DrawArrays, (GLenum a, GLint b, GLsizei c), (a, b, c)) \
    X(DrawElements, (GLenum a, GLsizei b, GLenum c, const void *d), (a, b, c, d)) \
    X(EnableVertexAttribArray, (GLuint a), (a)) \
    X(Finish, (void), ()) \
    X(Flush, (void), ()) \
    X(FramebufferRenderbuffer, (GLenum a, GLenum b, GLenum c, GLuint d), (a, b, c, d)) \
    X(FramebufferTexture2D, (GLenum a, GLenum b, GLenum c, GLuint d, GLint e), (a, b, c, d, e)) \
    X(FrontFace, (GLenum a), (a)) \
    X(GenBuffers, (GLsizei a, GLuint *b), (a, b)) \
    X(GenFramebuffers, (GLsizei a, GLuint *b), (a, b)) \
    X(GenRenderbuffers, (GLsizei a, GLuint *b), (a, b)) \
    X(GenTextures, (GLsizei a, GLuint *b), (a, b)) \
    X(GenerateMipmap, (GLenum a), (a)) \
    X(GetActiveAttrib, (GLuint a, GLuint b, GLsizei c, GLsizei *d, GLint *e, GLenum *f, GLchar *g), (a, b, c, d, e, f, g)) \
    X(GetActiveUniform, (GLuint a, GLuint b, GLsizei c, GLsizei *d, GLint *e, GLenum *f, GLchar *g), (a, b, c, d, e, f, g)) \
    X(GetAttachedShaders, (GLuint a, GLsizei b, GLsizei *c, GLuint *d), (a, b, c, d)) \
    X(GetBufferParameteriv, (GLenum a, GLenum b, GLint *c), (a, b, c)) \
    X(GetFramebufferAttachmentParameteriv, (GLenum a, GLenum b, GLenum c, GLint *d), (a, b, c, d)) \
    X(GetProgramInfoLog, (GLuint a, GLsizei b, GLsizei *c, GLchar *d), (a, b, c, d)) \
    X(GetProgramiv, (GLuint a, GLenum b, GLint *c), (a, b, c)) \
    X(GetRenderbufferParameteriv, (GLenum a, GLenum b, GLint *c), (a, b, c)) \
    X(GetShaderInfoLog, (GLuint a, GLsizei b, GLsizei *c, GLchar *d), (a, b, c, d)) \
    X(GetShaderSource, (GLuint a, GLsizei b, GLsizei *c, GLchar *d), (a, b, c, d)) \
    X(GetShaderiv, (GLuint a, GLenum b, GLint *c), (a, b, c)) \
    X(GetTexParameterfv, (GLenum a, GLenum b, GLfloat *c), (a, b, c)) \
    X(GetTexParameteriv, (GLenum a, GLenum b, GLint *c), (a, b, c)) \
    X(GetUniformfv, (GLuint a, GLint b, GLfloat *c), (a, b, c)) \
    X(GetUniformiv, (GLuint a, GLint b, GLint *c), (a, b, c)) \
    X(GetVertexAttribfv, (GLuint a, GLenum b, GLfloat *c), (a, b, c)) \
    X(GetVertexAttribiv, (GLuint a, GLenum b, GLint *c), (a, b, c)) \
    X(GetVertexAttribPointerv, (GLuint a, GLenum b, void **c), (a, b, c)) \
    X(Hint, (GLenum a, GLenum b), (a, b)) \
    X(LineWidth, (GLfloat a), (a)) \
    X(LinkProgram, (GLuint a), (a)) \
    X(PixelStorei, (GLenum a, GLint b), (a, b)) \
    X(PolygonOffset, (GLfloat a, GLfloat b), (a, b)) \
    X(ReadPixels, (GLint a, GLint b, GLsizei c, GLsizei d, GLenum e, GLenum f, void *g), (a, b, c, d, e, f, g)) \
    X(SampleCoverage, (GLfloat a, GLboolean b), (a, b)) \
    X(Scissor, (GLint a, GLint b, GLsizei c, GLsizei d), (a, b, c, d)) \
    X(StencilFunc, (GLenum a, GLint b, GLuint c), (a, b, c)) \
    X(StencilFuncSeparate, (GLenum a, GLenum b, GLint c, GLuint d), (a, b, c, d)) \
    X(StencilMask, (GLuint a), (a)) \
    X(StencilMaskSeparate, (GLenum a, GLuint b), (a, b)) \
    X(StencilOp, (GLenum a, GLenum b, GLenum c), (a, b, c)) \
    X(StencilOpSeparate, (GLenum a, GLenum b, GLenum c, GLenum d), (a, b, c, d)) \
    X(TexParameterf, (GLenum a, GLenum b, GLfloat c), (a, b, c)) \
    X(TexParameterfv, (GLenum a, GLenum b, const GLfloat *c), (a, b, c)) \
    X(TexParameteri, (GLenum a, GLenum b, GLint c), (a, b, c)) \
    X(TexParameteriv, (GLenum a, GLenum b, const GLint *c), (a, b, c)) \
    X(TexSubImage2D, (GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const void *i), (a, b, c, d, e, f, g, h, i)) \
    X(Uniform1f, (GLint a, GLfloat b), (a, b)) \
    X(Uniform1fv, (GLint a, GLsizei b, const GLfloat *c), (a, b, c)) \
    X(Uniform1i, (GLint a, GLint b), (a, b)) \
    X(Uniform1iv, (GLint a, GLsizei b, const GLint *c), (a, b, c)) \
    X(Uniform2f, (GLint a, GLfloat b, GLfloat c), (a, b, c)) \
    X(Uniform2fv, (GLint a, GLsizei b, const GLfloat *c), (a, b, c)) \
    X(Uniform2i, (GLint a, GLint b, GLint c), (a, b, c)) \
    X(Uniform2iv, (GLint a, GLsizei b, const GLint *c), (a, b, c)) \
    X(Uniform3f, (GLint a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
    X(Uniform3fv, (GLint a, GLsizei b, const GLfloat *c), (a, b, c)) \
    X(Uniform3i, (GLint a, GLint b, GLint c, GLint d), (a, b, c, d)) \
    X(Uniform3iv, (GLint a, GLsizei b, const GLint *c), (a, b, c)) \
    X(Uniform4f, (GLint a, GLfloat b, GLfloat c, GLfloat d, GLfloat e), (a, b, c, d, e)) \
    X(Uniform4fv, (GLint a, GLsizei b, const GLfloat *c), (a, b, c)) \
    X(Uniform4i, (GLint a, GLint b, GLint c, GLint d, GLint e), (a, b, c, d, e)) \
    X(Uniform4iv, (GLint a, GLsizei b, const GLint *c), (a, b, c)) \
    X(UniformMatrix2fv, (GLint a, GLsizei b, GLboolean c, const GLfloat *d), (a, b, c, d)) \
    X(UniformMatrix3fv, (GLint a, GLsizei b, GLboolean c, const GLfloat *d), (a, b, c, d)) \
    X(UniformMatrix4fv, (GLint a, GLsizei b, GLboolean c, const GLfloat *d), (a, b, c, d)) \
    X(UseProgram, (GLuint a), (a)) \
    X(ValidateProgram, (GLuint a), (a)) \
    X(VertexAttrib1f, (GLuint a, GLfloat b), (a, b)) \
    X(VertexAttrib1fv, (GLuint a, const GLfloat *b), (a, b)) \
    X(VertexAttrib2f, (GLuint a, GLfloat b, GLfloat c), (a, b, c)) \
    X(VertexAttrib2fv, (GLuint a, const GLfloat *b), (a, b)) \
    X(VertexAttrib3f, (GLuint a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
    X(VertexAttrib3fv, (GLuint a, const GLfloat *b), (a, b)) \
    X(VertexAttrib4f, (GLuint a, GLfloat b, GLfloat c, GLfloat d, GLfloat e), (a, b, c, d, e)) \
    X(VertexAttrib4fv, (GLuint a, const GLfloat *b), (a, b)) \
    X(Viewport, (GLint a, GLint b, GLsizei c, GLsizei d), (a, b, c, d))

#define HK_GL_RET(X) \
    X(GLenum, CheckFramebufferStatus, (GLenum a), (a)) \
    X(GLuint, CreateProgram, (void), ()) \
    X(GLuint, CreateShader, (GLenum a), (a)) \
    X(GLenum, GetError, (void), ()) \
    X(GLint, GetAttribLocation, (GLuint a, const GLchar *b), (a, b)) \
    X(GLint, GetUniformLocation, (GLuint a, const GLchar *b), (a, b)) \
    X(GLboolean, IsBuffer, (GLuint a), (a)) \
    X(GLboolean, IsFramebuffer, (GLuint a), (a)) \
    X(GLboolean, IsProgram, (GLuint a), (a)) \
    X(GLboolean, IsRenderbuffer, (GLuint a), (a)) \
    X(GLboolean, IsShader, (GLuint a), (a)) \
    X(GLboolean, IsTexture, (GLuint a), (a))

/* The rest of what this file calls on the host, by the host's own names. */
static struct {
#define X(n, p, a) void(*n) p;
    HK_GL_VOID(X)
#undef X
#define X(r, n, p, a) r(*n) p;
    HK_GL_RET(X)
#undef X
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*GetFloatv)(GLenum, GLfloat *);
    void (*GetBooleanv)(GLenum, GLboolean *);
    const GLubyte *(*GetString)(GLenum);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    GLboolean (*IsEnabled)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    GLboolean (*UnmapBuffer)(GLenum);
    void (*GenVertexArrays)(GLsizei, GLuint *); /* APPLE on the Mac's legacy GL, core on ES 3 */
    void (*BindVertexArray)(GLuint);
    void (*DeleteVertexArrays)(GLsizei, const GLuint *);
    GLboolean (*IsVertexArray)(GLuint);
#if TARGET_OS_OSX
    void (*AlphaFunc)(GLenum, GLfloat);
    void (*ClearDepth)(GLdouble);
    void (*DepthRange)(GLdouble, GLdouble);
    void *(*MapBuffer)(GLenum, GLenum);
    int (*CGLChoosePixelFormat)(const int *, void **, GLint *);
    int (*CGLDestroyPixelFormat)(void *);
    int (*CGLCreateContext)(void *, void *, void **);
    int (*CGLDestroyContext)(void *);
    int (*CGLSetCurrentContext)(void *);
    int (*CGLTexImageIOSurface2D)(void *, GLenum, GLenum, GLsizei, GLsizei, GLenum, GLenum, IOSurfaceRef, GLuint);
#else
    void (*ClearDepthf)(GLfloat);
    void (*DepthRangef)(GLfloat, GLfloat);
    void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
#endif
} R;

static void *g_lib;

static int gl_load(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_log = getenv("HK_GL_LOG") != NULL;
#if TARGET_OS_OSX
        g_lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
#else
        g_lib = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY | RTLD_LOCAL);
#endif
        if (!g_lib) {
            hk_log("OpenGL: the host's GL library did not load: %s", dlerror());
            return;
        }
#if TARGET_OS_OSX
        for (uint32_t i = 0; i < _dyld_image_count(); i++) /* the Mac's OpenGL must not bring AppKit in */
            if (strstr(_dyld_get_image_name(i), "/AppKit.framework/")) hk_log("OpenGL: THE REAL APPKIT IS LOADED: %s", _dyld_get_image_name(i));
#endif
#define S(field, name) R.field = (__typeof__(R.field))dlsym(g_lib, name)
#define X(n, p, a) S(n, "gl" #n);
        HK_GL_VOID(X)
#undef X
#define X(r, n, p, a) S(n, "gl" #n);
        HK_GL_RET(X)
#undef X
        S(VertexAttribPointer, "glVertexAttribPointer");
        S(BindFramebuffer, "glBindFramebuffer");
        S(DeleteFramebuffers, "glDeleteFramebuffers");
        S(GetIntegerv, "glGetIntegerv");
        S(GetFloatv, "glGetFloatv");
        S(GetBooleanv, "glGetBooleanv");
        S(GetString, "glGetString");
        S(Enable, "glEnable");
        S(Disable, "glDisable");
        S(IsEnabled, "glIsEnabled");
        S(ShaderSource, "glShaderSource");
        S(TexImage2D, "glTexImage2D");
        S(RenderbufferStorage, "glRenderbufferStorage");
        S(UnmapBuffer, "glUnmapBuffer");
#if TARGET_OS_OSX
        S(GenVertexArrays, "glGenVertexArraysAPPLE");
        S(BindVertexArray, "glBindVertexArrayAPPLE");
        S(DeleteVertexArrays, "glDeleteVertexArraysAPPLE");
        S(IsVertexArray, "glIsVertexArrayAPPLE");
        S(AlphaFunc, "glAlphaFunc");
        S(ClearDepth, "glClearDepth");
        S(DepthRange, "glDepthRange");
        S(MapBuffer, "glMapBuffer");
        S(CGLChoosePixelFormat, "CGLChoosePixelFormat");
        S(CGLDestroyPixelFormat, "CGLDestroyPixelFormat");
        S(CGLCreateContext, "CGLCreateContext");
        S(CGLDestroyContext, "CGLDestroyContext");
        S(CGLSetCurrentContext, "CGLSetCurrentContext");
        S(CGLTexImageIOSurface2D, "CGLTexImageIOSurface2D");
#else
        S(GenVertexArrays, "glGenVertexArrays");
        S(BindVertexArray, "glBindVertexArray");
        S(DeleteVertexArrays, "glDeleteVertexArrays");
        S(IsVertexArray, "glIsVertexArray");
        S(ClearDepthf, "glClearDepthf");
        S(DepthRangef, "glDepthRangef");
        S(MapBufferRange, "glMapBufferRange");
#endif
#undef S
    });
    return g_lib != NULL;
}

/* ================================================================ contexts and pixel formats */

#define HK_PF_MAGIC 0x484b5046u  /* 'HKPF' */
#define HK_CTX_MAGIC 0x484b474cu /* 'HKGL' */

/* What a CGLPixelFormatObj points to here. */
typedef struct HKGLPixelFormat {
    uint32_t magic;
    _Atomic int refs;
    int depth, stencil, samples, alpha, color, profile, double_buffer;
} HKGLPixelFormat;

/* What a CGLContextObj points to here. */
typedef struct HKGLContext {
    uint32_t magic;
    _Atomic int refs;
    void *native;          /* Mac: the host's CGLContextObj; iOS: an EAGLContext (retained) */
    pthread_mutex_t lock;  /* CGLLockContext; recursive */
    HKGLPixelFormat pf;
    void *owner;           /* the NSOpenGLContext wrapping it (not retained) */
    GLuint fbo, color_rb, depth_rb;
    GLint w, h;            /* the default framebuffer's size; 0 before it exists */
    /* Its colour: IOSurfaces (as textures) taken in turn, each shown as it is finished; or, when those cannot be
       made, a renderbuffer read back at each flush. */
    int surfaces;          /* 1: IOSurfaces; 0: read back; -1: not tried yet */
    IOSurfaceRef surf[3];
    GLuint surf_tex[3];
    int surf_cur;
    uint32_t frames;       /* HK_GL_LOG: frames shown since frames_since */
    double frames_since;
    GLint want_w, want_h;  /* the size its view asks for */
    GLint swap_interval;
    /* The fixed-function state OpenGL ES lacks, kept so the program reads back what it set. */
    GLboolean alpha_test;
    GLenum alpha_func;
    GLfloat alpha_ref;
    uint32_t legacy_caps;  /* bit per entry of legacy_cap_index() */
    GLuint vao;            /* the bound vertex array object (HK_GL_CHECK_ES) */
} HKGLContext;

static __thread HKGLContext *t_cur;

static int ctx_valid(const HKGLContext *c) { return c && c->magic == HK_CTX_MAGIC; }
static int pf_valid(const HKGLPixelFormat *p) { return p && p->magic == HK_PF_MAGIC; }

/* Attributes that carry a value (NSOpenGLPixelFormatAttribute and CGLPixelFormatAttribute share the numbers). */
static int attr_has_value(uint32_t a) {
    switch (a) {
    case 7: case 8: case 11: case 12: case 13: case 14: case 55: case 56: case 70: case 84: case 99: case 128: return 1;
    default: return 0;
    }
}

static void pf_parse(HKGLPixelFormat *pf, const uint32_t *attrs) {
    memset(pf, 0, sizeof *pf);
    pf->magic = HK_PF_MAGIC;
    atomic_init(&pf->refs, 1);
    pf->color = 32;
    pf->profile = 0x1000;
    for (const uint32_t *a = attrs; a && *a; a++) {
        uint32_t v = attr_has_value(*a) ? a[1] : 1;
        switch (*a) {
        case 5: case 3: pf->double_buffer = 1; break;
        case 8: pf->color = (int)v; break;
        case 11: pf->alpha = (int)v; break;
        case 12: pf->depth = (int)v; break;
        case 13: pf->stencil = (int)v; break;
        case 56: pf->samples = (int)v; break;
        case 99: pf->profile = (int)v; break;
        default: break;
        }
        if (attr_has_value(*a)) a++;
    }
}

static void native_make_current(HKGLContext *c) {
#if TARGET_OS_OSX
    if (R.CGLSetCurrentContext) R.CGLSetCurrentContext(c ? c->native : NULL);
#else
    Class eagl = NSClassFromString(@"EAGLContext");
    ((BOOL(*)(id, SEL, id))objc_msgSend)(eagl, @selector(setCurrentContext:), c ? (__bridge id)c->native : nil);
#endif
    t_cur = c;
}

static HKGLContext *ctx_create(const HKGLPixelFormat *pf, HKGLContext *share) {
    if (!gl_load()) return NULL;
    HKGLContext *c = calloc(1, sizeof *c);
    c->magic = HK_CTX_MAGIC;
    atomic_init(&c->refs, 1);
    if (pf) c->pf = *pf;
    else pf_parse(&c->pf, NULL);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->lock, &ma);
    pthread_mutexattr_destroy(&ma);
    c->alpha_func = GL_ALWAYS;
    c->swap_interval = 1;
    c->surfaces = getenv("HK_GL_READBACK") ? 0 : -1;
#if TARGET_OS_OSX
    /* A legacy (2.1) profile unless a core one is asked for; no drawable: the default framebuffer is ours. */
    int profile = c->pf.profile == 0x3200 || c->pf.profile == 0x4100 ? c->pf.profile : 0x1000;
    int attrs[] = {73 /* accelerated */, 96 /* allow offline renderers */, 99, profile, 0};
    void *pix = NULL;
    GLint n = 0;
    void *native = NULL;
    if (R.CGLChoosePixelFormat && R.CGLChoosePixelFormat(attrs, &pix, &n) == 0 && pix) {
        R.CGLCreateContext(pix, ctx_valid(share) ? share->native : NULL, &native);
        R.CGLDestroyPixelFormat(pix);
    }
    if (!native) {
        hk_log("OpenGL: the Mac's CGL made no context");
        free(c);
        return NULL;
    }
    c->native = native;
#else
    Class eagl = NSClassFromString(@"EAGLContext");
    id group = ctx_valid(share) ? [(__bridge id)share->native valueForKey:@"sharegroup"] : nil;
    id e = nil;
    for (NSUInteger api = kEAGLRenderingAPIOpenGLES3; api >= kEAGLRenderingAPIOpenGLES2 && !e; api--)
        e = ((id(*)(id, SEL, NSUInteger, id))objc_msgSend)([eagl alloc], @selector(initWithAPI:sharegroup:), api, group);
    if (!e) {
        hk_log("OpenGL: no EAGLContext");
        free(c);
        return NULL;
    }
    c->native = (__bridge_retained void *)e;
#endif
    GLLOG("OpenGL: context %p made (depth %d stencil %d profile 0x%x%s)", c, c->pf.depth, c->pf.stencil, c->pf.profile,
          share ? ", shared" : "");
    return c;
}

static HKGLContext *ctx_retain(HKGLContext *c) {
    if (ctx_valid(c)) atomic_fetch_add(&c->refs, 1);
    return c;
}

static void ctx_release(HKGLContext *c) {
    if (!ctx_valid(c) || atomic_fetch_sub(&c->refs, 1) != 1) return;
    if (t_cur == c) native_make_current(NULL);
    for (int i = 0; i < 3; i++)
        if (c->surf[i]) CFRelease(c->surf[i]);
#if TARGET_OS_OSX
    if (R.CGLDestroyContext) R.CGLDestroyContext(c->native);
#else
    id e = (__bridge_transfer id)c->native;
    (void)e; /* released here */
#endif
    c->magic = 0;
    pthread_mutex_destroy(&c->lock);
    free(c);
}

/* ---- the default framebuffer ---- */

#if TARGET_OS_OSX
#define HK_SURFACE_TARGET GL_TEXTURE_RECTANGLE /* what CGL binds an IOSurface to */
#define HK_SURFACE_BINDING GL_TEXTURE_BINDING_RECTANGLE
#else
#define HK_SURFACE_TARGET GL_TEXTURE_2D
#define HK_SURFACE_BINDING GL_TEXTURE_BINDING_2D
#endif

/* Three BGRA IOSurfaces of w x h, each the storage of a texture of the context's. The texture binding is kept. */
static int surfaces_make(HKGLContext *c, GLint w, GLint h) {
    GLint old_tex = 0;
    R.GetIntegerv(HK_SURFACE_BINDING, &old_tex);
    if (!c->surf_tex[0]) R.GenTextures(3, c->surf_tex);
    int ok = 1;
    for (int i = 0; i < 3 && ok; i++) {
        if (c->surf[i]) CFRelease(c->surf[i]);
        NSDictionary *props = @{(id)kIOSurfaceWidth : @(w), (id)kIOSurfaceHeight : @(h), (id)kIOSurfaceBytesPerElement : @4,
                                (id)kIOSurfacePixelFormat : @(0x42475241) /* 'BGRA' */};
        c->surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        if (!c->surf[i]) {
            ok = 0;
            break;
        }
        R.BindTexture(HK_SURFACE_TARGET, c->surf_tex[i]);
        R.TexParameteri(HK_SURFACE_TARGET, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        R.TexParameteri(HK_SURFACE_TARGET, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#if TARGET_OS_OSX
        ok = R.CGLTexImageIOSurface2D && R.CGLTexImageIOSurface2D(c->native, HK_SURFACE_TARGET, GL_RGBA, w, h, GL_BGRA,
                                                                  GL_UNSIGNED_INT_8_8_8_8_REV, c->surf[i], 0) == 0;
#else
        ok = ((BOOL(*)(id, SEL, IOSurfaceRef, NSUInteger, NSUInteger, uint32_t, uint32_t, NSUInteger, NSUInteger, uint32_t))objc_msgSend)(
            (__bridge id)c->native, @selector(texImageIOSurface:target:internalFormat:width:height:format:type:plane:), c->surf[i],
            HK_SURFACE_TARGET, GL_RGBA, (uint32_t)w, (uint32_t)h, GL_BGRA, GL_UNSIGNED_BYTE, 0);
#endif
    }
    R.BindTexture(HK_SURFACE_TARGET, (GLuint)old_tex);
    if (!ok) hk_log("OpenGL: no IOSurface colour buffers (%dx%d): reading frames back instead", w, h);
    return ok;
}

/* Makes (or resizes) the default framebuffer; the context is current on this thread. The program's bindings are
   kept, except that a new context starts with its default framebuffer bound and the viewport covering it, as a
   context attached to a drawable does. */
static void fbo_ensure(HKGLContext *c, GLint w, GLint h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (c->fbo && c->w == w && c->h == h) return;
    int fresh = !c->fbo;
    GLint old_fb = 0, old_rb = 0;
    R.GetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
    R.GetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    if (fresh) {
        R.GenFramebuffers(1, &c->fbo);
        R.GenRenderbuffers(1, &c->depth_rb);
    }
    if (c->surfaces != 0) c->surfaces = surfaces_make(c, w, h);
    if (!c->surfaces && !c->color_rb) R.GenRenderbuffers(1, &c->color_rb);
    if (!c->surfaces) {
        R.BindRenderbuffer(GL_RENDERBUFFER, c->color_rb);
        R.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    }
    R.BindRenderbuffer(GL_RENDERBUFFER, c->depth_rb);
    R.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    R.BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    c->surf_cur = 0;
    if (c->surfaces) R.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, HK_SURFACE_TARGET, c->surf_tex[0], 0);
    else R.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c->color_rb);
    R.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c->depth_rb);
    R.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->depth_rb);
    GLenum status = R.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) hk_log("OpenGL: the default framebuffer (%dx%d) is incomplete: 0x%x", w, h, status);
    c->w = w;
    c->h = h;
    if (fresh) {
        R.Viewport(0, 0, w, h);
        R.Scissor(0, 0, w, h);
    } else {
        R.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
    }
    R.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)old_rb);
    GLLOG("OpenGL: context %p default framebuffer %dx%d (fbo %u, %s)", c, w, h, c->fbo, c->surfaces ? "IOSurfaces" : "read back");
}

/* ---- iOS: no GL work while the app is in the background (iOS ends a process that submits GPU work then) ----
   When the app resigns active, threads other than the main one wait at their next CGLLockContext or
   makeCurrentContext (where a frame starts) until it is active again, and entering the background waits for frames
   in progress (each holds its context's lock) to end. */
#if !TARGET_OS_OSX
static pthread_mutex_t g_bg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_bg_cond = PTHREAD_COND_INITIALIZER;
static int g_background;
static NSHashTable *g_contexts; /* every NSOpenGLContext (weak), under g_bg_lock */
#endif

static void wait_foreground(void) {
#if !TARGET_OS_OSX
    if (!g_background || [NSThread isMainThread]) return;
    pthread_mutex_lock(&g_bg_lock);
    while (g_background) pthread_cond_wait(&g_bg_cond, &g_bg_lock);
    pthread_mutex_unlock(&g_bg_lock);
#endif
}

/* Runs block with c current on this thread (and locked), then puts back what was current. */
static void with_context(HKGLContext *c, void (^block)(void)) {
    pthread_mutex_lock(&c->lock);
    HKGLContext *prev = t_cur;
    if (prev != c) native_make_current(c);
    block();
    if (prev != c) native_make_current(prev);
    pthread_mutex_unlock(&c->lock);
}

/* ---- presenting: the default framebuffer read back as an image (rows turned over: GL's run bottom up) ---- */

static void free_pixels(void *info, const void *data, size_t size) { free((void *)data); }

static CGImageRef fbo_image(HKGLContext *c) {
    if (!c->fbo || !R.ReadPixels) return NULL;
    size_t w = (size_t)c->w, h = (size_t)c->h, stride = w * 4;
    GLint old_fb = 0, align = 4, row = 0, pack_buf = 0;
    R.GetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
    R.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_PACK_ROW_LENGTH, &row);
    R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
    while (R.GetError() != GL_NO_ERROR) {} /* the queries above may not exist on every GL: forget their errors */
    if (pack_buf) R.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    R.BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    R.PixelStorei(GL_PACK_ALIGNMENT, 4);
    if (row) R.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    uint8_t *px = malloc(stride * h), *up = malloc(stride * h);
    R.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    R.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
    R.PixelStorei(GL_PACK_ALIGNMENT, align);
    if (row) R.PixelStorei(GL_PACK_ROW_LENGTH, row);
    if (pack_buf) R.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
    for (size_t y = 0; y < h; y++) memcpy(up + y * stride, px + (h - 1 - y) * stride, stride);
    free(px);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, up, stride * h, free_pixels);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(w, h, 8, 32, stride, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL,
                                   false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(dp);
    return img;
}

/* The finished frame (an IOSurface, retained) and the next surface attached in its place; the program's binding
   is kept. glFlush hands the work to the GPU; whoever shows the surface waits for it (IOSurface usage is tracked). */
static IOSurfaceRef fbo_surface_frame(HKGLContext *c) {
    R.Flush();
    IOSurfaceRef done = c->surf[c->surf_cur];
    CFRetain(done);
    c->surf_cur = (c->surf_cur + 1) % 3;
    GLint old_fb = 0;
    R.GetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
    if ((GLuint)old_fb != c->fbo) R.BindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    R.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, HK_SURFACE_TARGET, c->surf_tex[c->surf_cur], 0);
    if ((GLuint)old_fb != c->fbo) R.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
    return done;
}

/* The frame to show: an IOSurface (rows bottom up: shown turned over) or a read-back image. +1. */
static CFTypeRef fbo_frame(HKGLContext *c) {
    if (!c->fbo) return NULL;
    return c->surfaces ? (CFTypeRef)fbo_surface_frame(c) : (CFTypeRef)fbo_image(c);
}

/* ================================================================ AppKit's classes */

@interface NSOpenGLPixelFormat : HKShimObject
- (instancetype)initWithAttributes:(const uint32_t *)attributes;
- (void *)CGLPixelFormatObj;
- (GLint)numberOfVirtualScreens;
- (void)getValues:(GLint *)vals forAttribute:(uint32_t)attrib forVirtualScreen:(GLint)screen;
@end

@interface NSOpenGLContext : HKShimObject
- (instancetype)initWithFormat:(NSOpenGLPixelFormat *)format shareContext:(NSOpenGLContext *)share;
- (instancetype)initWithCGLContextObj:(void *)context;
+ (NSOpenGLContext *)currentContext;
+ (void)clearCurrentContext;
@property(nonatomic, weak) NSView *view;
@property(nonatomic, readonly) NSOpenGLPixelFormat *pixelFormat;
- (void *)CGLContextObj;
- (void)makeCurrentContext;
- (void)flushBuffer;
- (void)update;
- (void)clearDrawable;
- (void)setValues:(const GLint *)vals forParameter:(NSInteger)param;
- (void)getValues:(GLint *)vals forParameter:(NSInteger)param;
- (void)hk_present;
@end

@interface NSOpenGLView : NSView
+ (NSOpenGLPixelFormat *)defaultPixelFormat;
- (instancetype)initWithFrame:(NSRect)frame pixelFormat:(NSOpenGLPixelFormat *)format;
@property(nonatomic, strong) NSOpenGLContext *openGLContext;
@property(nonatomic, strong) NSOpenGLPixelFormat *pixelFormat;
@property(nonatomic) BOOL wantsBestResolutionOpenGLSurface;
- (void)prepareOpenGL;
- (void)reshape;
- (void)update;
- (void)clearGLContext;
@end

/* HKCore.m's own (not in HKShim.h). */
@interface NSView (HKOpenGLCore)
- (void)hk_setWindow:(NSWindow *)window;
@end

static HKGLContext *ctx_of(NSOpenGLContext *ctx);

@implementation NSOpenGLPixelFormat {
  @public
    HKGLPixelFormat *_pf;
}
- (instancetype)initWithAttributes:(const uint32_t *)attributes {
    if ((self = [super init])) {
        _pf = calloc(1, sizeof *_pf);
        pf_parse(_pf, attributes);
    }
    return self;
}
- (instancetype)init { return [self initWithAttributes:NULL]; }
- (instancetype)initWithCGLPixelFormatObj:(void *)obj {
    if ((self = [super init])) {
        _pf = calloc(1, sizeof *_pf);
        if (pf_valid(obj)) *_pf = *(HKGLPixelFormat *)obj;
        else pf_parse(_pf, NULL);
        atomic_init(&_pf->refs, 1);
    }
    return self;
}
- (instancetype)initWithData:(NSData *)data { return [self initWithAttributes:NULL]; }
- (void)dealloc { free(_pf); }
- (void *)CGLPixelFormatObj { return _pf; }
- (GLint)numberOfVirtualScreens { return 1; }
- (void)getValues:(GLint *)vals forAttribute:(uint32_t)attrib forVirtualScreen:(GLint)screen {
    if (!vals) return;
    switch (attrib) {
    case 5: *vals = _pf->double_buffer; break;
    case 8: *vals = _pf->color; break;
    case 11: *vals = _pf->alpha; break;
    case 12: *vals = _pf->depth; break;
    case 13: *vals = _pf->stencil; break;
    case 56: *vals = _pf->samples; break;
    case 99: *vals = _pf->profile; break;
    case 73: *vals = 1; break;
    default: *vals = 0;
    }
}
@end

@implementation NSOpenGLContext {
    HKGLContext *_c;
    NSOpenGLPixelFormat *_format;
    __weak NSView *_view;
    id _pending;          /* the newest frame not yet shown */
    BOOL _presentQueued;
}

static HKGLContext *ctx_of(NSOpenGLContext *ctx) { return ctx ? ctx->_c : NULL; }

- (instancetype)initWithFormat:(NSOpenGLPixelFormat *)format shareContext:(NSOpenGLContext *)share {
    if ((self = [super init])) {
        _format = format ?: [[NSOpenGLPixelFormat alloc] initWithAttributes:NULL];
        _c = ctx_create(_format->_pf, share ? share->_c : NULL);
        if (!_c) return nil;
        _c->owner = (__bridge void *)self;
#if !TARGET_OS_OSX
        pthread_mutex_lock(&g_bg_lock);
        if (!g_contexts) g_contexts = [NSHashTable weakObjectsHashTable];
        [g_contexts addObject:self];
        pthread_mutex_unlock(&g_bg_lock);
#endif
    }
    return self;
}
- (instancetype)initWithFormat:(NSOpenGLPixelFormat *)format { return [self initWithFormat:format shareContext:nil]; }
- (instancetype)initWithCGLContextObj:(void *)context {
    if (!ctx_valid(context)) return nil;
    if ((self = [super init])) {
        _c = ctx_retain(context);
        if (!_c->owner) _c->owner = (__bridge void *)self;
        _format = [[NSOpenGLPixelFormat alloc] initWithCGLPixelFormatObj:&_c->pf];
    }
    return self;
}
- (void)dealloc {
    if (_c && _c->owner == (__bridge void *)self) _c->owner = NULL;
    ctx_release(_c);
}

+ (NSOpenGLContext *)currentContext { return t_cur ? (__bridge NSOpenGLContext *)t_cur->owner : nil; }
+ (void)clearCurrentContext { native_make_current(NULL); }
- (void *)CGLContextObj { return _c; }
- (NSOpenGLPixelFormat *)pixelFormat { return _format; }
- (NSView *)view { return _view; }

- (void)setView:(NSView *)view {
    _view = view;
    if (view) [self update];
}
- (void)clearDrawable { _view = nil; }
- (void)setFullScreen {}
- (void)setOffScreen:(void *)base width:(GLint)w height:(GLint)h rowbytes:(GLint)rb {}
- (GLint)currentVirtualScreen { return 0; }
- (void)setCurrentVirtualScreen:(GLint)screen {}
- (void *)CGLContextObjForShim { return _c; }

/* The size the view's drawable has: its bounds in pixels when it wants the best resolution, else in points. */
- (void)hk_measure {
    NSView *v = _view;
    if (!v) return;
    NSSize size = v.bounds.size;
    CGFloat scale = 1;
    if ([v respondsToSelector:@selector(wantsBestResolutionOpenGLSurface)] &&
        ((BOOL(*)(id, SEL))objc_msgSend)(v, @selector(wantsBestResolutionOpenGLSurface)))
        scale = v.window ? v.window.backingScaleFactor : hk_presenter()->scale;
    _c->want_w = (GLint)lround(size.width * scale);
    _c->want_h = (GLint)lround(size.height * scale);
}

- (void)update {
    if (!_view) return;
    if (![NSThread isMainThread]) { /* views are measured on the main thread */
        __weak NSOpenGLContext *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf update]; });
        return;
    }
    [self hk_measure];
    HKGLContext *c = _c;
    if (c->fbo && c->w == c->want_w && c->h == c->want_h) return;
    with_context(c, ^{ fbo_ensure(c, c->want_w, c->want_h); });
}

- (void)makeCurrentContext {
    wait_foreground();
    native_make_current(_c);
    if (!_c->fbo && _view) {
        if ([NSThread isMainThread]) [self hk_measure];
        if (_c->want_w > 0) fbo_ensure(_c, _c->want_w, _c->want_h);
    }
}

- (void)flushBuffer {
    HKGLContext *c = _c;
    if (!c->fbo) return;
    __block CFTypeRef img = NULL;
    if (t_cur == c) {
        pthread_mutex_lock(&c->lock);
        img = fbo_frame(c);
        pthread_mutex_unlock(&c->lock);
    } else {
        with_context(c, ^{ img = fbo_frame(c); });
    }
    if (!img) return;
    if (g_log) {
        double now = CACurrentMediaTime();
        if (!c->frames_since) c->frames_since = now;
        else if (++c->frames, now - c->frames_since >= 5) {
            hk_log("OpenGL: context %p: %.1f frames/s (%dx%d, %s)", c, c->frames / (now - c->frames_since), c->w, c->h,
                   c->surfaces ? "IOSurfaces" : "read back");
            c->frames = 0;
            c->frames_since = now;
        }
    }
    if (c->want_w > 0 && (c->w != c->want_w || c->h != c->want_h)) { /* the view changed size since: follow it */
        GLint w = c->want_w, h = c->want_h;
        if (t_cur == c) fbo_ensure(c, w, h);
    }
    BOOL queue;
    @synchronized(self) {
        _pending = (__bridge_transfer id)img;
        queue = !_presentQueued;
        _presentQueued = YES;
    }
    if ([NSThread isMainThread]) [self hk_present];
    else if (queue) {
        NSOpenGLContext *me = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [me hk_present]; });
    }
}

/* On the main thread: the newest frame becomes the contents of the layer under the view's own sublayers. */
- (void)hk_present {
    id frame;
    @synchronized(self) {
        frame = _pending;
        _pending = nil;
        _presentQueued = NO;
    }
    NSView *v = _view;
    if (!frame || !v) return;
    CALayer *host = v.layer;
    static char key;
    CALayer *surface = objc_getAssociatedObject(v, &key);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (!surface || surface.superlayer != host) {
        [surface removeFromSuperlayer];
        surface = [CALayer layer];
        surface.opaque = YES;
        surface.contentsGravity = kCAGravityResize;
        surface.name = @"HKOpenGLSurface";
        objc_setAssociatedObject(v, &key, surface, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [host insertSublayer:surface atIndex:0];
    }
    CGRect b = host.bounds;
    surface.bounds = (CGRect){CGPointZero, b.size};
    surface.position = CGPointMake(CGRectGetMidX(b), CGRectGetMidY(b));
    /* GL's rows run bottom up: an IOSurface written by GL is shown turned over (read-back images are turned already). */
    BOOL bottom_up = CFGetTypeID((__bridge CFTypeRef)frame) != CGImageGetTypeID();
    surface.transform = bottom_up ? CATransform3DMakeScale(1, -1, 1) : CATransform3DIdentity;
    surface.contents = frame;
    [CATransaction commit];
    static int said;
    if (g_log && said++ < 3) hk_log("OpenGL: presented %dx%d into %s", _c->w, _c->h, NSStringFromRect(v.bounds).UTF8String);
}

- (void)setValues:(const GLint *)vals forParameter:(NSInteger)param {
    if (vals && param == 222) _c->swap_interval = vals[0];
}
- (void)getValues:(GLint *)vals forParameter:(NSInteger)param {
    if (!vals) return;
    switch (param) {
    case 222: vals[0] = _c->swap_interval; break;
    case 236: vals[0] = 1; break;
    case 304: vals[0] = _c->w; vals[1] = _c->h; break;
    case 314: vals[0] = _view != nil; break;
    default: vals[0] = 0;
    }
}
@end

/* AppKit gives every view wantsBestResolutionOpenGLSurface (GLFW sets it on its own content view, not an
   NSOpenGLView): kept, so a context drawing into the view sizes its surface in pixels, as the app's framebuffer size
   (the view's backing size) expects. NSOpenGLView keeps its own below. */
static char g_best_key;
@implementation NSView (HKOpenGLResolution)
- (BOOL)wantsBestResolutionOpenGLSurface { return [objc_getAssociatedObject(self, &g_best_key) boolValue]; }
- (void)setWantsBestResolutionOpenGLSurface:(BOOL)flag {
    objc_setAssociatedObject(self, &g_best_key, flag ? @YES : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
@end

@implementation NSOpenGLView {
    NSOpenGLContext *_glContext;
    NSOpenGLPixelFormat *_glFormat;
    BOOL _prepared, _syncQueued, _wantsBest;
}

+ (NSOpenGLPixelFormat *)defaultPixelFormat {
    static const uint32_t attrs[] = {5, 12, 24, 0};
    return [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
}

- (instancetype)initWithFrame:(NSRect)frame { return [self initWithFrame:frame pixelFormat:[[self class] defaultPixelFormat]]; }
- (instancetype)initWithFrame:(NSRect)frame pixelFormat:(NSOpenGLPixelFormat *)format {
    if ((self = [super initWithFrame:frame])) _glFormat = format ?: [[self class] defaultPixelFormat];
    return self;
}
- (instancetype)initWithCoder:(NSCoder *)coder { return [self initWithFrame:NSZeroRect]; }

- (NSOpenGLContext *)openGLContext {
    if (!_glContext) {
        _glContext = [[NSOpenGLContext alloc] initWithFormat:_glFormat shareContext:nil];
        _glContext.view = self;
    }
    return _glContext;
}
- (void)setOpenGLContext:(NSOpenGLContext *)context {
    if (_glContext == context) return;
    if (_glContext.view == self) [_glContext clearDrawable];
    _glContext = context;
    context.view = self;
}
- (NSOpenGLPixelFormat *)pixelFormat { return _glFormat; }
- (void)setPixelFormat:(NSOpenGLPixelFormat *)format { _glFormat = format; }
- (void)clearGLContext {
    [_glContext clearDrawable];
    _glContext = nil;
    _prepared = NO;
}
- (BOOL)wantsBestResolutionOpenGLSurface { return _wantsBest; }
- (void)setWantsBestResolutionOpenGLSurface:(BOOL)flag {
    _wantsBest = flag;
    [self hk_glQueueSync];
}
- (void)prepareOpenGL {}
- (void)reshape {}
- (void)update { [_glContext update]; }
- (void)drawRect:(NSRect)rect {}
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)hk_setWindow:(NSWindow *)window {
    [super hk_setWindow:window];
    if (window) [self hk_glQueueSync];
}
- (void)setFrameSize:(NSSize)size {
    NSSize old = self.frame.size;
    [super setFrameSize:size];
    if (!CGSizeEqualToSize(old, size)) [self hk_glQueueSync];
}

/* As AppKit does on the display pass after the view enters a window or changes size: the context follows the view
   (update), prepareOpenGL once with the context current, then reshape. Later, on the main thread. */
- (void)hk_glQueueSync {
    if (_syncQueued) return;
    _syncQueued = YES;
    __weak NSOpenGLView *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf hk_glSync]; });
}

- (void)hk_glSync {
    _syncQueued = NO;
    if (!self.window) return;
    NSOpenGLContext *ctx = self.openGLContext;
    if (!ctx) return;
    if (ctx.view != self) ctx.view = self;
    [self update];
    if (!_prepared) {
        _prepared = YES;
        HKGLContext *c = ctx_of(ctx);
        pthread_mutex_lock(&c->lock);
        [ctx makeCurrentContext];
        [self prepareOpenGL];
        pthread_mutex_unlock(&c->lock);
    }
    [self reshape];
    if ([self hk_glDrawsItself]) [self setNeedsDisplay:YES];
}

/* A subclass that draws in drawRect: (the classic NSOpenGLView) is drawn with its context current; others draw on
   their own (a display link or a timer) and are never drawn into a bitmap. */
- (BOOL)hk_glDrawsItself {
    static IMP base;
    if (!base) base = class_getMethodImplementation([NSOpenGLView class], @selector(drawRect:));
    return class_getMethodImplementation([self class], @selector(drawRect:)) != base;
}

- (void)displayLayer:(CALayer *)layer {
    if (![self hk_glDrawsItself] || !self.window) return;
    NSOpenGLContext *ctx = self.openGLContext;
    if (!_prepared) [self hk_glSync];
    HKGLContext *c = ctx_of(ctx);
    pthread_mutex_lock(&c->lock);
    [ctx makeCurrentContext];
    [self drawRect:self.bounds];
    pthread_mutex_unlock(&c->lock);
}
@end

#if !TARGET_OS_OSX
@interface HKOpenGLLifecycle : NSObject
@end
@implementation HKOpenGLLifecycle
+ (void)load {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:@"UIApplicationWillResignActiveNotification" object:nil queue:nil usingBlock:^(NSNotification *n) {
        pthread_mutex_lock(&g_bg_lock);
        g_background = 1;
        pthread_mutex_unlock(&g_bg_lock);
    }];
    [nc addObserverForName:@"UIApplicationDidEnterBackgroundNotification" object:nil queue:nil usingBlock:^(NSNotification *n) {
        pthread_mutex_lock(&g_bg_lock);
        g_background = 1;
        NSArray *all = g_contexts.allObjects;
        pthread_mutex_unlock(&g_bg_lock);
        for (NSOpenGLContext *ctx in all) { /* frames in progress end; their work is handed to the GPU */
            HKGLContext *c = ctx_of(ctx);
            if (!c) continue;
            with_context(c, ^{ if (R.Finish) R.Finish(); });
        }
    }];
    [nc addObserverForName:@"UIApplicationDidBecomeActiveNotification" object:nil queue:nil usingBlock:^(NSNotification *n) {
        pthread_mutex_lock(&g_bg_lock);
        g_background = 0;
        pthread_cond_broadcast(&g_bg_cond);
        pthread_mutex_unlock(&g_bg_lock);
    }];
}
@end
#endif

/* ================================================================ CGL */

typedef int HKCGLError; /* kCGLNoError 0, kCGLBadAttribute 10000, kCGLBadContext 10004, kCGLBadPixelFormat 10005 */

HK_GL_FN(HKCGLError, CGLLockContext, (HKGLContext *c)) {
    if (!ctx_valid(c)) return 10004;
    wait_foreground();
    pthread_mutex_lock(&c->lock);
    return 0;
}
HK_GL_FN(HKCGLError, CGLUnlockContext, (HKGLContext *c)) {
    if (!ctx_valid(c)) return 10004;
    pthread_mutex_unlock(&c->lock);
    return 0;
}
HK_GL_FN(HKGLContext *, CGLGetCurrentContext, (void)) { return t_cur; }
HK_GL_FN(HKCGLError, CGLSetCurrentContext, (HKGLContext *c)) {
    if (c && !ctx_valid(c)) return 10004;
    NSOpenGLContext *owner = c ? (__bridge NSOpenGLContext *)c->owner : nil;
    if (owner) [owner makeCurrentContext];
    else native_make_current(c);
    return 0;
}
HK_GL_FN(HKCGLError, CGLFlushDrawable, (HKGLContext *c)) {
    if (!ctx_valid(c)) return 10004;
    NSOpenGLContext *owner = (__bridge NSOpenGLContext *)c->owner;
    [owner flushBuffer];
    return 0;
}
HK_GL_FN(HKCGLError, CGLChoosePixelFormat, (const uint32_t *attrs, HKGLPixelFormat **out, GLint *count)) {
    if (!out) return 10000;
    *out = calloc(1, sizeof **out);
    pf_parse(*out, attrs);
    if (count) *count = 1;
    return 0;
}
HK_GL_FN(HKCGLError, CGLDestroyPixelFormat, (HKGLPixelFormat *pf)) {
    if (pf_valid(pf) && atomic_fetch_sub(&pf->refs, 1) == 1) free(pf);
    return 0;
}
HK_GL_FN(HKGLPixelFormat *, CGLRetainPixelFormat, (HKGLPixelFormat *pf)) {
    if (pf_valid(pf)) atomic_fetch_add(&pf->refs, 1);
    return pf;
}
HK_GL_FN(void, CGLReleasePixelFormat, (HKGLPixelFormat *pf)) { hk_CGLDestroyPixelFormat(pf); }
HK_GL_FN(HKCGLError, CGLDescribePixelFormat, (HKGLPixelFormat *pf, GLint screen, uint32_t attrib, GLint *value)) {
    if (!pf_valid(pf) || !value) return 10005;
    NSOpenGLPixelFormat *f = [[NSOpenGLPixelFormat alloc] initWithCGLPixelFormatObj:pf];
    [f getValues:value forAttribute:attrib forVirtualScreen:screen];
    return 0;
}
HK_GL_FN(HKCGLError, CGLCreateContext, (HKGLPixelFormat *pf, HKGLContext *share, HKGLContext **out)) {
    if (!out) return 10004;
    NSOpenGLPixelFormat *f = [[NSOpenGLPixelFormat alloc] initWithCGLPixelFormatObj:pf];
    NSOpenGLContext *s = ctx_valid(share) ? (__bridge NSOpenGLContext *)share->owner : nil;
    NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:f shareContext:s];
    if (!ctx) return 10004;
    HKGLContext *c = ctx_of(ctx);
    c->owner = (__bridge_retained void *)ctx; /* a CGL context owns its NSOpenGLContext until destroyed */
    *out = c;
    return 0;
}
HK_GL_FN(HKCGLError, CGLDestroyContext, (HKGLContext *c)) {
    if (!ctx_valid(c)) return 10004;
    if (c->owner) {
        NSOpenGLContext *owner = (__bridge_transfer NSOpenGLContext *)c->owner;
        (void)owner; /* released here */
    }
    return 0;
}
HK_GL_FN(HKGLContext *, CGLRetainContext, (HKGLContext *c)) { return ctx_retain(c); }
HK_GL_FN(void, CGLReleaseContext, (HKGLContext *c)) { ctx_release(c); }
HK_GL_FN(GLuint, CGLGetContextRetainCount, (HKGLContext *c)) { return ctx_valid(c) ? (GLuint)atomic_load(&c->refs) : 0; }
HK_GL_FN(HKGLPixelFormat *, CGLGetPixelFormat, (HKGLContext *c)) { return ctx_valid(c) ? &c->pf : NULL; }
HK_GL_FN(void *, CGLGetShareGroup, (HKGLContext *c)) { return c; }
HK_GL_FN(HKCGLError, CGLSetParameter, (HKGLContext *c, int pname, const GLint *v)) {
    if (!ctx_valid(c)) return 10004;
    if (pname == 222 && v) c->swap_interval = *v;
    return 0;
}
HK_GL_FN(HKCGLError, CGLGetParameter, (HKGLContext *c, int pname, GLint *v)) {
    if (!ctx_valid(c)) return 10004;
    if (v) *v = pname == 222 ? c->swap_interval : 0;
    return 0;
}
HK_GL_FN(HKCGLError, CGLEnable, (HKGLContext *c, int pname)) { return 0; }
HK_GL_FN(HKCGLError, CGLDisable, (HKGLContext *c, int pname)) { return 0; }
HK_GL_FN(HKCGLError, CGLIsEnabled, (HKGLContext *c, int pname, GLint *enable)) {
    if (enable) *enable = 0;
    return 0;
}
HK_GL_FN(HKCGLError, CGLUpdateContext, (HKGLContext *c)) {
    if (ctx_valid(c) && c->owner) [(__bridge NSOpenGLContext *)c->owner update];
    return 0;
}
HK_GL_FN(HKCGLError, CGLClearDrawable, (HKGLContext *c)) { return 0; }
HK_GL_FN(const char *, CGLErrorString, (HKCGLError e)) { return e ? "CGL error" : "no error"; }
HK_GL_FN(void, CGLGetVersion, (GLint *major, GLint *minor)) {
    if (major) *major = 1;
    if (minor) *minor = 2;
}

/* CoreVideo's display link follows a context's display: there is one display here. */
HK_GL_FN(int32_t, CVDisplayLinkSetCurrentCGDisplayFromOpenGLContext, (void *link, void *ctx, void *pf)) { return 0; }

/* ================================================================ GL, as the program calls it */

#define X(n, p, a) \
    HK_GL_FN(void, gl##n, p) { \
        if (R.n) R.n a; \
    }
HK_GL_VOID(X)
#undef X
#define X(r, n, p, a) \
    HK_GL_FN(r, gl##n, p) { return R.n ? R.n a : (r)0; }
HK_GL_RET(X)
#undef X

/* ---- framebuffer 0 is the context's default framebuffer ---- */

HK_GL_FN(void, glBindFramebuffer, (GLenum target, GLuint fb)) {
    HKGLContext *c = t_cur;
    if (!fb && c && c->fbo) fb = c->fbo;
    if (R.BindFramebuffer) R.BindFramebuffer(target, fb);
}
HK_GL_FN(void, glDeleteFramebuffers, (GLsizei n, const GLuint *fbs)) {
    HKGLContext *c = t_cur;
    if (!R.DeleteFramebuffers || n <= 0 || !fbs) return;
    GLuint *keep = malloc(sizeof *keep * (size_t)n);
    GLsizei k = 0;
    for (GLsizei i = 0; i < n; i++)
        if (!c || fbs[i] != c->fbo) keep[k++] = fbs[i];
    R.DeleteFramebuffers(k, keep);
    free(keep);
}

/* ---- state ES lacks (iOS): the fixed-function enables, kept as state ---- */

static int legacy_cap_index(GLenum cap) {
#if TARGET_OS_OSX
    return -1;
#else
    static const GLenum caps[] = {GL_ALPHA_TEST, GL_TEXTURE_2D, GL_TEXTURE_1D, GL_LINE_SMOOTH, GL_POINT_SMOOTH,
                                  GL_POLYGON_SMOOTH, GL_MULTISAMPLE, GL_POINT_SPRITE, GL_VERTEX_PROGRAM_POINT_SIZE,
                                  GL_LIGHTING, GL_FOG, GL_COLOR_MATERIAL, GL_NORMALIZE, GL_TEXTURE_RECTANGLE,
                                  GL_COLOR_LOGIC_OP, GL_LINE_STIPPLE, GL_POLYGON_OFFSET_LINE, GL_POLYGON_OFFSET_POINT,
                                  GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_FRAMEBUFFER_SRGB, GL_PRIMITIVE_RESTART, GL_DEPTH_CLAMP};
    for (int i = 0; i < (int)(sizeof caps / sizeof *caps); i++)
        if (caps[i] == cap) return i;
    if (cap >= GL_CLIP_PLANE0 && cap <= GL_CLIP_PLANE5) return 24 + (int)(cap - GL_CLIP_PLANE0);
    return -1;
#endif
}

static int set_legacy_cap(GLenum cap, int on) {
    int i = legacy_cap_index(cap);
    HKGLContext *c = t_cur;
    if (i < 0) return 0;
    if (c) {
        if (on) c->legacy_caps |= 1u << i;
        else c->legacy_caps &= ~(1u << i);
        if (cap == GL_ALPHA_TEST) c->alpha_test = (GLboolean)on;
    }
    return 1;
}

HK_GL_FN(void, glEnable, (GLenum cap)) {
    if (!set_legacy_cap(cap, 1) && R.Enable) R.Enable(cap);
}
HK_GL_FN(void, glDisable, (GLenum cap)) {
    if (!set_legacy_cap(cap, 0) && R.Disable) R.Disable(cap);
}
HK_GL_FN(GLboolean, glIsEnabled, (GLenum cap)) {
    int i = legacy_cap_index(cap);
    if (i >= 0) return t_cur && (t_cur->legacy_caps >> i & 1);
    return R.IsEnabled ? R.IsEnabled(cap) : 0;
}
HK_GL_FN(void, glAlphaFunc, (GLenum func, GLfloat ref)) {
#if TARGET_OS_OSX
    if (R.AlphaFunc) R.AlphaFunc(func, ref);
#else
    static int said;
    if (!said++) hk_log("OpenGL: glAlphaFunc is kept as state only (OpenGL ES has no alpha test)");
#endif
    if (t_cur) {
        t_cur->alpha_func = func;
        t_cur->alpha_ref = ref;
    }
}

/* Answers about the default framebuffer say 0, as they would for a drawable's; ES-less state from the context. */
static int answer_int(GLenum pname, GLint *out) {
    HKGLContext *c = t_cur;
    if (!c) return 0;
#if !TARGET_OS_OSX
    int i = legacy_cap_index(pname);
    if (i >= 0) {
        *out = c->legacy_caps >> i & 1;
        return 1;
    }
    if (pname == GL_ALPHA_TEST_FUNC) {
        *out = (GLint)c->alpha_func;
        return 1;
    }
    if (pname == GL_ALPHA_TEST_REF) {
        *out = (GLint)(c->alpha_ref * 2147483647.0f);
        return 1;
    }
#endif
    return 0;
}

HK_GL_FN(void, glGetIntegerv, (GLenum pname, GLint *out)) {
    if (!out || answer_int(pname, out) || !R.GetIntegerv) return;
    R.GetIntegerv(pname, out);
    HKGLContext *c = t_cur;
    if (c && c->fbo && (pname == GL_FRAMEBUFFER_BINDING || pname == GL_READ_FRAMEBUFFER_BINDING) && (GLuint)*out == c->fbo) *out = 0;
}
HK_GL_FN(void, glGetBooleanv, (GLenum pname, GLboolean *out)) {
    GLint v = 0;
    if (!out) return;
    if (answer_int(pname, &v)) {
        *out = v != 0;
        return;
    }
    if (R.GetBooleanv) R.GetBooleanv(pname, out);
}
HK_GL_FN(void, glGetFloatv, (GLenum pname, GLfloat *out)) {
    GLint v = 0;
    if (!out) return;
#if !TARGET_OS_OSX
    if (pname == GL_ALPHA_TEST_REF && t_cur) {
        *out = t_cur->alpha_ref;
        return;
    }
#endif
    if (answer_int(pname, &v)) {
        *out = (GLfloat)v;
        return;
    }
    if (R.GetFloatv) R.GetFloatv(pname, out);
}

HK_GL_FN(const GLubyte *, glGetString, (GLenum name)) {
    if (!R.GetString) return NULL;
#if TARGET_OS_OSX
    return R.GetString(name);
#else
    static char *ext, *ver, *renderer;
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    if (!t_cur) return R.GetString(name);
    pthread_mutex_lock(&once);
    if (!ext) { /* the first time a context is current: the strings need one */
        const char *real = (const char *)R.GetString(GL_EXTENSIONS);
        /* What this file provides on top of ES, under the names desktop programs look for. */
        asprintf(&ext, "%s GL_APPLE_vertex_array_object GL_ARB_vertex_array_object GL_ARB_framebuffer_object "
                       "GL_EXT_framebuffer_object GL_ARB_texture_non_power_of_two GL_ARB_vertex_buffer_object "
                       "GL_ARB_shader_objects GL_ARB_fragment_shader GL_ARB_vertex_shader GL_EXT_bgra",
                 real ? real : "");
        const char *es = (const char *)R.GetString(GL_VERSION);
        asprintf(&ver, "2.1 Henge (%s)", es ? es : "OpenGL ES");
        const char *r = (const char *)R.GetString(GL_RENDERER);
        asprintf(&renderer, "%s", r ? r : "Apple GPU");
    }
    pthread_mutex_unlock(&once);
    switch (name) {
    case GL_EXTENSIONS: return (const GLubyte *)ext;
    case GL_VERSION: return (const GLubyte *)ver;
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"1.20";
    case GL_RENDERER: return (const GLubyte *)renderer;
    default: return R.GetString(name);
    }
#endif
}

/* ---- calls ES names or shapes differently ---- */

HK_GL_FN(void, glClearDepth, (GLdouble d)) {
#if TARGET_OS_OSX
    if (R.ClearDepth) R.ClearDepth(d);
#else
    if (R.ClearDepthf) R.ClearDepthf((GLfloat)d);
#endif
}
HK_GL_FN(void, glClearDepthf, (GLfloat d)) { hk_glClearDepth(d); }
HK_GL_FN(void, glDepthRange, (GLdouble n, GLdouble f)) {
#if TARGET_OS_OSX
    if (R.DepthRange) R.DepthRange(n, f);
#else
    if (R.DepthRangef) R.DepthRangef((GLfloat)n, (GLfloat)f);
#endif
}
HK_GL_FN(void, glDepthRangef, (GLfloat n, GLfloat f)) { hk_glDepthRange(n, f); }

HK_GL_FN(void *, glMapBuffer, (GLenum target, GLenum access)) {
#if TARGET_OS_OSX
    return R.MapBuffer ? R.MapBuffer(target, access) : NULL;
#else
    if (!R.MapBufferRange || !R.GetBufferParameteriv) return NULL;
    GLint size = 0;
    R.GetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (size <= 0) return NULL;
    GLbitfield bits = access == GL_READ_ONLY ? GL_MAP_READ_BIT : access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT : GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    return R.MapBufferRange(target, 0, size, bits);
#endif
}
HK_GL_FN(GLboolean, glUnmapBuffer, (GLenum target)) { return R.UnmapBuffer ? R.UnmapBuffer(target) : 0; }

/* HK_GL_CHECK_ES=1 (Mac): says once when the program does something desktop GL allows and OpenGL ES 3 does not. */
static int check_es(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_GL_CHECK_ES") != NULL;
    return on;
}

/* ES 3 takes vertex data from client memory only with the default vertex array object bound. */
HK_GL_FN(void, glVertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)) {
    if (check_es() && t_cur && t_cur->vao && p) {
        GLint buf = 0;
        R.GetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &buf);
        static int said;
        if (!buf && !said++) hk_log("OpenGL ES check: vertex data from client memory with vertex array object %u bound", t_cur->vao);
    }
    if (R.VertexAttribPointer) R.VertexAttribPointer(i, size, type, norm, stride, p);
}

HK_GL_FN(void, glGenVertexArraysAPPLE, (GLsizei n, GLuint *a)) {
    if (R.GenVertexArrays) R.GenVertexArrays(n, a);
}
HK_GL_FN(void, glBindVertexArrayAPPLE, (GLuint a)) {
    if (t_cur) t_cur->vao = a;
    if (R.BindVertexArray) R.BindVertexArray(a);
}
HK_GL_FN(void, glDeleteVertexArraysAPPLE, (GLsizei n, const GLuint *a)) {
    if (R.DeleteVertexArrays) R.DeleteVertexArrays(n, a);
}
HK_GL_FN(GLboolean, glIsVertexArrayAPPLE, (GLuint a)) { return R.IsVertexArray ? R.IsVertexArray(a) : 0; }

/* Desktop internal formats ES does not take: legacy component counts and sized formats ES 3 pairs differently. */
static GLint es_internal_format(GLint internal, GLenum format, GLenum type) {
#if TARGET_OS_OSX
    return internal;
#else
    switch (internal) {
    case 1: return GL_LUMINANCE;
    case 2: return GL_LUMINANCE_ALPHA;
    case 3: return GL_RGB;
    case 4: return GL_RGBA;
    default: break;
    }
    if (format == GL_BGRA) return GL_RGBA; /* APPLE_texture_format_BGRA8888 */
    switch (format) { /* the unsized formats: ES takes them with every classic type when the internal format matches */
    case GL_RGBA: case GL_RGB: case GL_ALPHA: case GL_LUMINANCE: case GL_LUMINANCE_ALPHA:
        if (type == GL_UNSIGNED_BYTE || type == GL_FLOAT || type == GL_HALF_FLOAT_OES || (type >= 0x8033 && type <= 0x8034) ||
            type == 0x8363 /* 5_6_5 */)
            return (GLint)format;
        break;
    default: break;
    }
    return internal;
#endif
}

HK_GL_FN(void, glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                              GLenum type, const void *pixels)) {
    if (R.TexImage2D) R.TexImage2D(target, level, es_internal_format(internal, format, type), w, h, border, format, type, pixels);
}

HK_GL_FN(void, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h)) {
#if !TARGET_OS_OSX
    switch (format) {
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT32: format = GL_DEPTH_COMPONENT24; break;
    case GL_DEPTH_STENCIL: format = GL_DEPTH24_STENCIL8; break;
    case GL_STENCIL_INDEX: format = GL_STENCIL_INDEX8; break;
    case GL_RGBA: format = GL_RGBA8; break;
    case GL_RGB: format = GL_RGB8; break;
    default: break;
    }
#endif
    if (R.RenderbufferStorage) R.RenderbufferStorage(target, format, w, h);
}

/* ---- shaders: desktop GLSL (1.10/1.20, no #version or a desktop one) as GLSL ES 1.00 ---- */

/* The source for OpenGL ES: a desktop #version line goes (ES 1.00 is the default), and a fragment shader gets a
   default float precision (desktop GLSL needs none) after any leading #extension lines. Shaders already for ES
   (#version 100 or 300 es) are left alone. malloc'd. */
static char *es_shader_source(const char *src, int fragment) {
    if (strstr(src, "#version 100") || strstr(src, " es\n") || strstr(src, " es\r")) return strdup(src);
    size_t len = strlen(src);
    char *out = malloc(len + 64), *o = out;
    const char *p = src;
    int head = 1, added = !fragment;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p + 1) : strlen(p);
        const char *t = p;
        while (t < p + n && (*t == ' ' || *t == '\t')) t++;
        int is_version = !strncmp(t, "#version", 8);
        int keeps_head = !strncmp(t, "#extension", 10) || *t == '\n' || *t == '\r' || (t[0] == '/' && t[1] == '/') || is_version;
        if (head && !keeps_head) {
            head = 0;
            if (!added) {
                o += sprintf(o, "precision highp float;\n");
                added = 1;
            }
        }
        if (is_version) *o++ = '\n'; /* keep the line count */
        else {
            memcpy(o, p, n);
            o += n;
        }
        p += n;
    }
    if (!added) o += sprintf(o, "\nprecision highp float;\n");
    *o = 0;
    return out;
}

static void dump_shader(GLuint shader, const char *src, int fragment) {
    static const char *dir;
    static _Atomic int n;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dir = getenv("HK_GL_SHADER_DUMP");
        if (dir) mkdir(dir, 0755);
    });
    if (!dir) return;
    int k = atomic_fetch_add(&n, 1);
    const char *ext = fragment ? "frag" : "vert";
    char path[1024];
    snprintf(path, sizeof path, "%s/%04d.%s", dir, k, ext);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(src, f);
        fclose(f);
    }
    char *es = es_shader_source(src, fragment);
    snprintf(path, sizeof path, "%s/%04d.es.%s", dir, k, ext);
    if ((f = fopen(path, "w"))) {
        if (!strstr(es, "#version")) fputs("#version 100\n", f);
        fputs(es, f);
        fclose(f);
    }
    free(es);
}

HK_GL_FN(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)) {
    if (!R.ShaderSource || count <= 0 || !strings) return;
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        if (strings[i]) total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *src = malloc(total + 1), *o = src;
    for (GLsizei i = 0; i < count; i++) {
        if (!strings[i]) continue;
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(o, strings[i], n);
        o += n;
    }
    *o = 0;
    GLint type = 0;
    if (R.GetShaderiv) R.GetShaderiv(shader, GL_SHADER_TYPE, &type);
    int fragment = type == GL_FRAGMENT_SHADER;
    dump_shader(shader, src, fragment);
#if TARGET_OS_OSX
    const GLchar *one = src;
#else
    char *es = es_shader_source(src, fragment);
    free(src);
    src = es;
    const GLchar *one = src;
#endif
    R.ShaderSource(shader, 1, &one, NULL);
    free(src);
}
