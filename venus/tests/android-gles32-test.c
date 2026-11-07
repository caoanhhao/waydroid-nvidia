/* Does ES 3.2 really work, not just get reported? Draws through the stages ES 3.2 adds over
 * 3.1 and reads the pixels back:
 *   1. tessellation: one patch, the evaluation shader places a full-screen quad
 *   2. geometry: a point, the geometry shader expands it into a full-screen quad
 *   3. texture buffer: the fragment shader reads its colour from a samplerBuffer
 * Built for every ABI by run-gpuinfo.sh. Exit code = number of failed checks. */
#include <stdio.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok;
    char log[512];
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("  compile 0x%x failed: %s\n", type, log);
    }
    return s;
}

static GLuint program(const char *vs, const char *tcs, const char *tes, const char *gs,
                      const char *fs)
{
    GLuint p = glCreateProgram();
    GLint ok;
    char log[512];
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    if (tcs)
        glAttachShader(p, shader(GL_TESS_CONTROL_SHADER, tcs));
    if (tes)
        glAttachShader(p, shader(GL_TESS_EVALUATION_SHADER, tes));
    if (gs)
        glAttachShader(p, shader(GL_GEOMETRY_SHADER, gs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("  link failed: %s\n", log);
        return 0;
    }
    return p;
}

/* clear to black, draw, expect the centre pixel to be `want` (RGBA8) */
static int check(const char *name, GLuint prog, GLenum mode, unsigned want)
{
    unsigned char px[4] = { 0 };
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (prog) {
        glUseProgram(prog);
        glDrawArrays(mode, 0, mode == GL_PATCHES ? 4 : 1);
    }
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned got = px[0] << 24 | px[1] << 16 | px[2] << 8 | px[3];
    int ok = prog && got == want && glGetError() == GL_NO_ERROR;
    printf("%-15s %s (pixel %08x, want %08x)\n", name, ok ? "PASS" : "FAIL", got, want);
    return !ok;
}

#define HDR "#version 320 es\nprecision highp float;\n"

int main(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg;
    EGLint n;
    eglInitialize(d, NULL, NULL);
    const EGLint ca[] = { EGL_RENDERABLE_TYPE, 0x40, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    const EGLint xa[] = { 0x3098, 3, 0x30FB, 2, EGL_NONE };
    const EGLint sa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    eglChooseConfig(d, ca, &cfg, 1, &n);
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, c)) {
        printf("no ES 3.2 context (0x%x)\n", eglGetError());
        return 1;
    }
    printf("%s\n", glGetString(GL_VERSION));

    /* render into an RGBA8 texture: the pbuffer config may have no alpha */
    GLuint tex, fbo, vao;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 64, 64);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    const char *vs = HDR "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    const char *red = HDR "out vec4 o; void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    const char *green = HDR "out vec4 o; void main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    int fails = 0;

    /* 1. tessellation: a quad patch whose corners the evaluation shader puts at ±1 */
    const char *tcs = HDR "layout(vertices = 4) out;\n"
        "void main() {\n"
        "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
        "  gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = gl_TessLevelOuter[3] = 1.0;\n"
        "  gl_TessLevelInner[0] = gl_TessLevelInner[1] = 1.0;\n"
        "}\n";
    const char *tes = HDR "layout(quads) in;\n"
        "void main() { gl_Position = vec4(gl_TessCoord.xy * 2.0 - 1.0, 0.0, 1.0); }\n";
    glPatchParameteri(GL_PATCH_VERTICES, 4);
    fails += check("tessellation", program(vs, tcs, tes, NULL, red), GL_PATCHES, 0xff0000ff);

    /* 2. geometry: one point in, a full-screen triangle strip out */
    const char *gs = HDR "layout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
        "void main() {\n"
        "  gl_Position = vec4(-1, -1, 0, 1); EmitVertex();\n"
        "  gl_Position = vec4( 1, -1, 0, 1); EmitVertex();\n"
        "  gl_Position = vec4(-1,  1, 0, 1); EmitVertex();\n"
        "  gl_Position = vec4( 1,  1, 0, 1); EmitVertex();\n"
        "  EndPrimitive();\n"
        "}\n";
    fails += check("geometry", program(vs, NULL, NULL, gs, green), GL_POINTS, 0x00ff00ff);

    /* 3. texture buffer: blue read from a samplerBuffer, drawn through the geometry shader */
    static const unsigned char blue[4] = { 0, 0, 255, 255 };
    GLuint buf, tbo;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_TEXTURE_BUFFER, buf);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(blue), blue, GL_STATIC_DRAW);
    glGenTextures(1, &tbo);
    glBindTexture(GL_TEXTURE_BUFFER, tbo);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, buf);
    const char *tbfs = HDR "uniform highp samplerBuffer t; out vec4 o;\n"
        "void main() { o = texelFetch(t, 0); }\n";
    fails += check("texture buffer", program(vs, NULL, NULL, gs, tbfs), GL_POINTS, 0x0000ffff);

    printf("%d failed\n", fails);
    return fails;
}
