// OpenGL on the RTX through AppleMetalOpenGLRenderer: an accelerated CGL
// context (legacy 2.1 and core 4.1), renderer string, clear + triangle into an
// FBO, read back.
//   cc gl_test.c -framework OpenGL -o gl_test
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <string.h>

static int run(int core) {
    CGLPixelFormatAttribute a[] = {kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAAllowOfflineRenderers,
                                   kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)(core ? kCGLOGLPVersion_GL4_Core
                                                                                        : kCGLOGLPVersion_Legacy), 0};
    CGLPixelFormatObj pf;
    GLint n = 0;
    if (CGLChoosePixelFormat(a, &pf, &n) != kCGLNoError || !pf) { printf("%s: no accelerated pixel format\n", core ? "core" : "legacy"); return 1; }
    CGLContextObj ctx;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError) { printf("no context\n"); return 1; }
    CGLSetCurrentContext(ctx);
    printf("%s: %s | %s | %s\n", core ? "core" : "legacy", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
           glGetString(GL_VERSION));
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, 16, 16);
    glClearColor(0.0f, 0.5f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[16 * 16 * 4];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int bad = 0;
    for (int i = 0; i < 256; i++)
        if (px[i * 4] > 2 || px[i * 4 + 1] < 126 || px[i * 4 + 1] > 129 || px[i * 4 + 2] < 253) bad++;
    printf("%s clear readback: %d wrong of 256 (px0 %d %d %d %d), GL error 0x%x\n", core ? "core" : "legacy", bad,
           px[0], px[1], px[2], px[3], glGetError());
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    CGLDestroyPixelFormat(pf);
    return bad != 0;
}

int main(void) {
    int r = run(0) | run(1);
    printf("gl_test: %s\n", r ? "FAIL" : "PASS");
    return r;
}
