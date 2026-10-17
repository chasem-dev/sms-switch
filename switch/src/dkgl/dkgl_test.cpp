// dkgl's own pixel checks (gx_selftest.nro, before the GX ones): the paths
// sms_gx's draws take, one at a time, through dkgl's GL calls, so a wrong
// result names the path.
#include "dkgl.h"
#include "dkgl_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

namespace dkgl {

namespace {

#define F(name, type) static auto name = reinterpret_cast<type>(getProc(#name));
#define GLF(X) \
    X(glCreateShader, PFNGLCREATESHADERPROC) \
    X(glShaderSource, PFNGLSHADERSOURCEPROC) \
    X(glCreateProgram, PFNGLCREATEPROGRAMPROC) \
    X(glAttachShader, PFNGLATTACHSHADERPROC) \
    X(glLinkProgram, PFNGLLINKPROGRAMPROC) \
    X(glGetProgramiv, PFNGLGETPROGRAMIVPROC) \
    X(glUseProgram, PFNGLUSEPROGRAMPROC) \
    X(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
    X(glUniform4fv, PFNGLUNIFORM4FVPROC) \
    X(glUniform1i, PFNGLUNIFORM1IPROC) \
    X(glGenBuffers, PFNGLGENBUFFERSPROC) \
    X(glBindBuffer, PFNGLBINDBUFFERPROC) \
    X(glBufferData, PFNGLBUFFERDATAPROC) \
    X(glBindBufferRange, PFNGLBINDBUFFERRANGEPROC) \
    X(glGetUniformBlockIndex, PFNGLGETUNIFORMBLOCKINDEXPROC) \
    X(glUniformBlockBinding, PFNGLUNIFORMBLOCKBINDINGPROC) \
    X(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC) \
    X(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC) \
    X(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC) \
    X(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
    X(glVertexAttrib4f, PFNGLVERTEXATTRIB4FPROC) \
    X(glVertexAttribI4ui, PFNGLVERTEXATTRIBI4UIPROC) \
    X(glDrawArrays, PFNGLDRAWARRAYSPROC) \
    X(glGenTextures, PFNGLGENTEXTURESPROC) \
    X(glBindTexture, PFNGLBINDTEXTUREPROC) \
    X(glTexImage2D, PFNGLTEXIMAGE2DPROC) \
    X(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
    X(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
    X(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
    X(glViewport, PFNGLVIEWPORTPROC) \
    X(glReadPixels, PFNGLREADPIXELSPROC) \
    X(glDisable, PFNGLDISABLEPROC) \
    X(glClearColor, PFNGLCLEARCOLORPROC) \
    X(glClear, PFNGLCLEARPROC) \
    X(glBindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC) \
    X(glBindFragDataLocation, PFNGLBINDFRAGDATALOCATIONPROC) \
    X(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC) \
    X(glEnable, PFNGLENABLEPROC) \
    X(glColorMask, PFNGLCOLORMASKPROC) \
    X(glBlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC) \
    X(glBlendEquationSeparate, PFNGLBLENDEQUATIONSEPARATEPROC) \
    X(glBlendColor, PFNGLBLENDCOLORPROC) \
    X(glDepthFunc, PFNGLDEPTHFUNCPROC) \
    X(glDepthMask, PFNGLDEPTHMASKPROC) \
    X(glCullFace, PFNGLCULLFACEPROC) \
    X(glFrontFace, PFNGLFRONTFACEPROC) \
    X(glClearDepth, PFNGLCLEARDEPTHPROC)

int s_fails = 0;

GLuint program(const char* vs, const char* fs) {
    GLF(F)
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glShaderSource(f, 1, &fs, nullptr);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 4, "a_clr");
    glBindAttribLocation(p, 14, "a_mtx");
    glBindFragDataLocation(p, 0, "o_color");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) printf("dkgl check: a program did not link\n");
    return p;
}

// Draws a full-target triangle strip and checks the pixel in the middle.
void expect(const char* what, const uint8_t want[4]) {
    GLF(F)
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint8_t got[4] = {0, 0, 0, 0};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    bool ok = true;
    for (int i = 0; i < 4; i++) ok = ok && abs(int(got[i]) - int(want[i])) <= 2;
    s_fails += !ok;
    printf("%s dkgl %-44s got %3d %3d %3d %3d want %3d %3d %3d %3d\n", ok ? "PASS" : "FAIL", what, got[0], got[1],
           got[2], got[3], want[0], want[1], want[2], want[3]);
}

}  // namespace

int runChecks() {
    GLF(F)
    s_fails = 0;
    // a 16x16 target
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 16, 16);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // a quad from a vertex buffer: positions, and bytes of colour
    struct V {
        float x, y, z;
        uint8_t c[4];
    } verts[4] = {{-1, -1, 0, {10, 20, 30, 255}}, {1, -1, 0, {10, 20, 30, 255}}, {-1, 1, 0, {10, 20, 30, 255}},
                  {1, 1, 0, {10, 20, 30, 255}}};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V), reinterpret_cast<const void*>(12));
    glEnableVertexAttribArray(4);

    static const char* kVsHead = "#version 330 core\nin vec3 a_pos; in vec4 a_clr; in uvec3 a_mtx;\nout vec4 v_c;\n";
    static const char* kFsCopy = "#version 330 core\nin vec4 v_c; out vec4 o_color;\nvoid main() { o_color = v_c; }\n";
    char vs[2048];

    // 1. loose uniforms in the vertex shader, as GX's u_chan (vec4[4] after
    // a vec2), and its first word of each element
    snprintf(vs, sizeof vs,
             "%suniform vec4 u_proj[2];\nuniform vec2 u_efb;\nuniform vec4 u_chan[4];\n"
             "void main() { gl_Position = vec4(a_pos, 1.0) + u_proj[0] * 0.0 + vec4(u_efb, 0, 0) * 0.0; v_c = u_chan[%%d]; }\n",
             kVsHead);
    for (int el : {0, 2, 3}) {
        char src[2048];
        snprintf(src, sizeof src, vs, el);
        GLuint p = program(src, kFsCopy);
        glUseProgram(p);
        const float proj[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        glUniform4fv(glGetUniformLocation(p, "u_proj"), 2, proj);
        const float efb[4] = {640, 528, 0, 0};
        auto u2 = reinterpret_cast<void(APIENTRY*)(GLint, GLsizei, const GLfloat*)>(getProc("glUniform2fv"));
        u2(glGetUniformLocation(p, "u_efb"), 1, efb);
        const float chan[16] = {0.2f, 0.4f, 0.6f, 1.0f, 0.1f, 0.1f, 0.1f, 1.0f,
                                0.8f, 0.5f, 0.3f, 1.0f, 0.9f, 0.7f, 0.5f, 1.0f};
        glUniform4fv(glGetUniformLocation(p, "u_chan"), 4, chan);
        uint8_t want[4];
        for (int i = 0; i < 4; i++) want[i] = uint8_t(lroundf(chan[el * 4 + i] * 255.0f));
        char what[64];
        snprintf(what, sizeof what, "vertex shader u_chan[%d]", el);
        expect(what, want);
    }

    // 2. the same uniform read in the fragment shader
    {
        GLuint p = program("#version 330 core\nin vec3 a_pos;\nvoid main() { gl_Position = vec4(a_pos, 1.0); }\n",
                           "#version 330 core\nuniform vec2 u_x;\nuniform vec4 u_chan[4];\nout vec4 o_color;\n"
                           "void main() { o_color = u_chan[2] + vec4(u_x, 0, 0) * 0.0; }\n");
        glUseProgram(p);
        const float chan[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0.8f, 0.5f, 0.3f, 1.0f, 0, 0, 0, 0};
        glUniform4fv(glGetUniformLocation(p, "u_chan"), 4, chan);
        const uint8_t want[4] = {204, 128, 77, 255};
        expect("fragment shader u_chan[2]", want);
    }

    // 3. a uniform block read with a computed index, as GX's XF block
    {
        GLuint p = program(
            "#version 330 core\nin vec3 a_pos;\nlayout(std140) uniform XFBlock { uvec4 xf[184]; };\n"
            "uniform vec4 u_sel;\nout vec4 v_c;\n"
            "void main() { gl_Position = vec4(a_pos, 1.0); int i = int(u_sel.x); uvec4 w = xf[i >> 2];\n"
            "  uint u = (i & 3) == 0 ? w.x : (i & 3) == 1 ? w.y : (i & 3) == 2 ? w.z : w.w;\n"
            "  v_c = vec4(float((u >> 24) & 255u), float((u >> 16) & 255u), float((u >> 8) & 255u), float(u & 255u)) / 255.0; }\n",
            kFsCopy);
        glUseProgram(p);
        static uint32_t xf[736];
        for (int i = 0; i < 736; i++) xf[i] = 0x01020304u * uint32_t(i & 63);
        xf[611] = 0xC8643280u;  // a light colour's place
        GLuint ubo;
        glGenBuffers(1, &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof xf, xf, GL_STATIC_DRAW);
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, 0, sizeof xf);
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "XFBlock"), 0);
        const float sel[4] = {611, 0, 0, 0};
        glUniform4fv(glGetUniformLocation(p, "u_sel"), 1, sel);
        const uint8_t want[4] = {200, 100, 50, 128};
        expect("uniform block word 611 (computed index)", want);
    }

    // 4. vertex colours as bytes, and constant attributes
    {
        GLuint p = program((std::string(kVsHead) + "void main() { gl_Position = vec4(a_pos, 1.0); v_c = a_clr; }\n").c_str(),
                           kFsCopy);
        glUseProgram(p);
        const uint8_t want[4] = {10, 20, 30, 255};
        expect("byte colour attribute", want);
        glDisableVertexAttribArray(4);
        glVertexAttrib4f(4, 0.25f, 0.5f, 0.75f, 1.0f);
        const uint8_t want2[4] = {64, 128, 191, 255};
        expect("constant float attribute", want2);
        glEnableVertexAttribArray(4);
    }
    {
        GLuint p = program((std::string(kVsHead) +
                            "void main() { gl_Position = vec4(a_pos, 1.0); v_c = vec4(a_mtx, 255u) / 255.0; }\n")
                               .c_str(),
                           kFsCopy);
        glUseProgram(p);
        auto i4 = reinterpret_cast<void(APIENTRY*)(GLuint, GLuint, GLuint, GLuint, GLuint)>(getProc("glVertexAttribI4ui"));
        i4(14, 40, 80, 120, 0);
        const uint8_t want[4] = {40, 80, 120, 255};
        expect("constant integer attribute", want);
    }
    // 5. the game's shadow passes (ShadowUtil.cpp): alpha alone written,
    // then colour blended by that destination alpha while alpha comes from a
    // constant (GXSetDstAlpha)
    {
        GLuint p = program((std::string(kVsHead) + "void main() { gl_Position = vec4(a_pos, 1.0); v_c = a_clr; }\n").c_str(),
                           "#version 330 core\nin vec4 v_c; out vec4 o_color; uniform vec4 u_col;\n"
                           "void main() { o_color = u_col + v_c * 0.0; }\n");
        glUseProgram(p);
        const GLint col = glGetUniformLocation(p, "u_col");
        glDisable(GL_BLEND);
        glColorMask(1, 1, 1, 1);
        glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        const float quarter[4] = {1, 1, 1, 0.25f};
        glUniform4fv(col, 1, quarter);
        glColorMask(0, 0, 0, 1);
        const uint8_t want1[4] = {51, 102, 153, 64};
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glColorMask(1, 1, 1, 1);
        {
            uint8_t got[4] = {0, 0, 0, 0};
            glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
            bool ok = true;
            for (int i = 0; i < 4; i++) ok = ok && abs(int(got[i]) - int(want1[i])) <= 2;
            s_fails += !ok;
            printf("%s dkgl %-44s got %3d %3d %3d %3d want %3d %3d %3d %3d\n", ok ? "PASS" : "FAIL",
                   "alpha-only write", got[0], got[1], got[2], got[3], want1[0], want1[1], want1[2], want1[3]);
        }
        const float red[4] = {1, 0, 0, 1};
        glUniform4fv(col, 1, red);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_ALPHA, GL_ZERO);
        glBlendColor(0, 0, 0, 0);
        const uint8_t want2[4] = {102, 77, 115, 0};
        expect("blend by dst alpha, constant alpha", want2);
        glDisable(GL_BLEND);
    }
    // 6. the shadow volume passes with depth (ShadowUtil.cpp, as sms_gx
    // issues them): a floor, alpha cleared, front faces in front of the
    // floor write the shadow's alpha, back faces behind it blend the shadow
    // colour by that alpha
    {
        GLuint depthTex;
        glGenTextures(1, &depthTex);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 16, 16, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
        GLuint p = program((std::string(kVsHead) + "uniform vec4 u_z;\n"
                            "void main() { gl_Position = vec4(a_pos.xy, u_z.x, 1.0); v_c = a_clr; }\n").c_str(),
                           "#version 330 core\nin vec4 v_c; out vec4 o_color; uniform vec4 u_col;\n"
                           "void main() { o_color = u_col + v_c * 0.0; }\n");
        glUseProgram(p);
        const GLint col = glGetUniformLocation(p, "u_col"), z = glGetUniformLocation(p, "u_z");
        auto at = [&](float zz) { const float v[4] = {zz, 0, 0, 0}; glUniform4fv(z, 1, v); };
        auto colour = [&](float r, float g, float b, float a) { const float v[4] = {r, g, b, a}; glUniform4fv(col, 1, v); };
        glColorMask(1, 1, 1, 1);
        glDepthMask(1);
        glClearColor(0, 0, 0, 1);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // the floor
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        at(0.0f);
        colour(0.6f, 0.6f, 0.6f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // alpha cleared: no depth test, alpha only, constant alpha 0
        glDepthMask(0);
        glDepthFunc(GL_ALWAYS);
        glColorMask(0, 0, 0, 1);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_CONSTANT_ALPHA, GL_ZERO);
        glBlendColor(0, 0, 0, 0);
        colour(0.1176f, 0.196f, 0.45f, 0.706f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // front faces in front of the floor: the shadow's alpha
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        at(-0.5f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // back faces behind it: colour by destination alpha, alpha from the constant
        glDepthFunc(GL_GEQUAL);
        glCullFace(GL_FRONT);
        glFrontFace(GL_CW);  // the same quad, now a back face
        glColorMask(1, 1, 1, 1);
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_ALPHA, GL_ZERO);
        at(0.5f);
        const uint8_t want[4] = {66, 80, 126, 0};
        expect("shadow volume passes", want);
        glFrontFace(GL_CCW);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDepthMask(1);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("dkgl checks: %d failed\n", s_fails);
    return s_fails;
}

}  // namespace dkgl
