// Offscreen OpenGL 4.1 check for the displacement tessellation stages (Thema 207).
// Links the four GLSL 4.10 stages he_tests writes with HE_TESS_DUMP_DIR set
// (test_material_tessellation.cpp, "GL tessellation stages declare the fragment's
// HeLighting block") on the REAL driver, draws one triangle as a patch and counts
// what it covered. It proves link + draw, nothing about the look: that is the
// editor's headless dump.
//
// Build: clang -O1 -o tess_gl_harness scripts/tess_gl_harness.c -framework OpenGL -Wno-deprecated-declarations
// Run:   HE_TESS_DUMP_DIR=/tmp/tess he_tests --source-file='*test_material_tessellation.cpp'
//        ./tess_gl_harness /tmp/tess            (exit 0 = linked, drew, covered pixels)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef GL_PATCHES
#define GL_PATCHES 0x000E
#define GL_PATCH_VERTICES 0x8E72
#define GL_TESS_CONTROL_SHADER 0x8E88
#define GL_TESS_EVALUATION_SHADER 0x8E87
#endif
extern void glPatchParameteri(GLenum, GLint); // GL 4.0, not declared by gl3.h

static char* slurp(const char* dir, const char* name)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); return b;
}
static GLuint stage(GLenum t, const char* s, const char* what)
{
    GLuint h = glCreateShader(t); glShaderSource(h, 1, &s, 0); glCompileShader(h);
    GLint ok; glGetShaderiv(h, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[8192]; glGetShaderInfoLog(h, sizeof l, 0, l); fprintf(stderr, "%s compile: %s\n", what, l); exit(2); }
    return h;
}
static GLuint link(const char* dir, int tess)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, stage(GL_VERTEX_SHADER, slurp(dir, "material.vert"), "vertex"));
    if (tess)
    {
        glAttachShader(p, stage(GL_TESS_CONTROL_SHADER, slurp(dir, "material.tesc"), "control"));
        glAttachShader(p, stage(GL_TESS_EVALUATION_SHADER, slurp(dir, "material.tese"), "eval"));
    }
    glAttachShader(p, stage(GL_FRAGMENT_SHADER, slurp(dir, "material.frag"), "fragment"));
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char l[8192]; glGetProgramInfoLog(p, sizeof l, 0, l); fprintf(stderr, "link: %s\n", l); exit(3); }
    // Blocks by name (the renderer's setupProgram), every sampler on its own unit.
    const char* blocks[] = { "HeLighting", "U", "HeParams" };
    for (int i = 0; i < 3; ++i) { GLuint b = glGetUniformBlockIndex(p, blocks[i]); if (b != GL_INVALID_INDEX) glUniformBlockBinding(p, b, i); }
    glUseProgram(p);
    GLint n = 0; glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n); int unit = 0;
    for (GLint i = 0; i < n; ++i)
    {
        char nm[256]; GLint sz; GLenum ty; glGetActiveUniform(p, i, sizeof nm, 0, &sz, &ty, nm);
        if (ty == GL_SAMPLER_2D || ty == GL_SAMPLER_2D_ARRAY || ty == GL_SAMPLER_CUBE)
            glUniform1i(glGetUniformLocation(p, nm), unit++);
    }
    return p;
}
static int draw(GLuint p, int tess, GLuint vao, int W, int H)
{
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(p); glBindVertexArray(vao);
    glDrawElements(tess ? GL_PATCHES : GL_TRIANGLES, 3, GL_UNSIGNED_INT, 0);
    GLenum e = glGetError(); if (e) { fprintf(stderr, "draw error 0x%x\n", e); exit(4); }
    unsigned char* b = malloc(W * H * 4); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, b);
    int covered = 0; for (int i = 0; i < W * H; ++i) if (b[i * 4 + 3] > 0) ++covered;
    free(b); return covered;
}
int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dump dir>\n", argv[0]); return 1; }
    CGLPixelFormatAttribute at[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFAAccelerated, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint np; CGLChoosePixelFormat(at, &pf, &np); CGLContextObj cx; CGLCreateContext(pf, 0, &cx); CGLSetCurrentContext(cx);
    printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    const int W = 128, H = 128;
    GLuint fbo, tex; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, W, H); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    // One CCW triangle facing the camera (+Z normal), identity transforms.
    const float v[] = { -0.8f,-0.8f,0, 0,0,1, 0,0,   0.8f,-0.8f,0, 0,0,1, 1,0,   -0.8f,0.8f,0, 0,0,1, 0,1 };
    const unsigned idx[] = { 0, 1, 2 };
    GLuint vao, vbo, ebo; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    for (int a = 0; a < 3; ++a) glEnableVertexAttribArray(a);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    // UBOs: U = mvp, model identity, colour; HeLighting zero except the camera
    // right at the triangle (full tessellation factor); HeParams zero.
    float u[44] = { 0 }; for (int i = 0; i < 4; ++i) { u[i * 5] = 1; u[16 + i * 5] = 1; } u[32] = u[33] = u[34] = 0.8f;
    static float light[16384]; memset(light, 0, sizeof light); light[12] = 0; light[13] = 0; light[14] = 0.5f;
    static float params[64];
    GLuint ubo[3]; glGenBuffers(3, ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]); glBufferData(GL_UNIFORM_BUFFER, sizeof light, light, GL_STATIC_DRAW); glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo[0]);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]); glBufferData(GL_UNIFORM_BUFFER, sizeof u, u, GL_STATIC_DRAW); glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo[1]);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo[2]); glBufferData(GL_UNIFORM_BUFFER, sizeof params, params, GL_STATIC_DRAW); glBindBufferBase(GL_UNIFORM_BUFFER, 2, ubo[2]);
    glPatchParameteri(GL_PATCH_VERTICES, 3);

    const int plain = draw(link(argv[1], 0), 0, vao, W, H);
    const int tess  = draw(link(argv[1], 1), 1, vao, W, H);
    printf("covered: two stages %d, four stages %d (of %d)\n", plain, tess, W * H);
    return (plain > 0 && tess > 0) ? 0 : 5;
}
