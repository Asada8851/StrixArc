// OpenGL on the Metal device (Apple's GL-over-Metal renderer, as Chess uses it): draws into a multisampled framebuffer
// object, resolves it with glBlitFramebuffer and reads it back with glReadPixels, then reads a single-sampled one
// directly. Chess died in RADV under glReadPixels -> gldBlitFramebufferData -> drawPrimitives (2026-10-21).
// "legacy" as the second argument takes a GL 2.1 context and also reads depth (picking reads the depth under the cursor).
// Usage: glreadprobe [samples] [legacy]
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int check(const char *what, int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const unsigned char *c = px + ((size_t)(h / 2) * w + w / 2) * 4;
    const int ok = c[0] > 200 && c[1] < 40 && c[2] < 40;
    printf("%-28s centre %3d %3d %3d %3d  glGetError 0x%x  %s\n", what, c[0], c[1], c[2], c[3], glGetError(), ok ? "ok" : "WRONG");
    free(px);
    return !ok;
}

int main(int argc, char **argv)
{
    const int samples = argc > 1 ? atoi(argv[1]) : 4, W = 64, H = 64, legacy = argc > 2 && !strcmp(argv[2], "legacy");
    CGLPixelFormatAttribute a[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)(legacy ? kCGLOGLPVersion_Legacy : kCGLOGLPVersion_3_2_Core),
                                    kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint n = 0; CGLContextObj ctx;
    if (CGLChoosePixelFormat(a, &pf, &n) || !pf || CGLCreateContext(pf, NULL, &ctx)) { printf("no accelerated GL context\n"); return 1; }
    CGLSetCurrentContext(ctx);
    printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    GLuint fb[2], rb[2], db[2];
    glGenFramebuffers(2, fb); glGenRenderbuffers(2, rb); glGenRenderbuffers(2, db);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]); glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, W, H);
    glBindRenderbuffer(GL_RENDERBUFFER, db[0]); glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glBindRenderbuffer(GL_RENDERBUFFER, db[1]); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[1]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db[1]);
    int bad = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]); glViewport(0, 0, W, H); glClearColor(1, 0, 0, 1); glClearDepth(0.25); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]); bad += check("MSAA resolve + read", W, H);
    { float z = -1; glReadPixels(W / 2, H / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z);
      printf("%-28s %.3f (want 0.250)  glGetError 0x%x  %s\n", "depth read after resolve", z, glGetError(), z > 0.24f && z < 0.26f ? "ok" : "WRONG");
      bad += !(z > 0.24f && z < 0.26f); }
    // a scaled blit (a draw rather than a resolve) and a read straight from the single-sampled target
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glBlitFramebuffer(0, 0, W / 2, H / 2, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fb[1]); bad += check("scaled blit + read", W, H);
    glFinish();
    CGLSetCurrentContext(NULL); CGLDestroyContext(ctx); CGLDestroyPixelFormat(pf);
    return bad ? 1 : 0;
}
