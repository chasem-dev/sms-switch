// Checks the OpenGL features sms.nro's renderer relies on, one tile each,
// through the same stack (SDL2 window, OpenGL 3.3 core on Mesa/nouveau), and
// logs a PASS/FAIL line per feature to glprobe.log beside the NRO. A tile
// draws green when its feature works. Useful on an emulator or a console
// before blaming the port's renderer.
#include <switch.h>
#include <SDL.h>
#include <glad/glad.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static FILE* g_log;
#define LOG(...) (fprintf(g_log, __VA_ARGS__), fflush(g_log))

static GLuint shader(GLenum type, const char* src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char text[2048];
		glGetShaderInfoLog(s, sizeof text, NULL, text);
		LOG("shader: %s\n", text);
	}
	return s;
}

static GLuint program(const char* vs, const char* fs)
{
	GLuint p = glCreateProgram();
	glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_i");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char text[2048];
		glGetProgramInfoLog(p, sizeof text, NULL, text);
		LOG("link: %s\n", text);
	}
	return p;
}

static std::string read_file(const char* path)
{
	std::string out;
	if (FILE* f = fopen(path, "rb")) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	return out;
}

static const char* kGreen = "#version 330 core\nout vec4 o; void main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n";
static const char* kVaryingFs = "#version 330 core\nin vec4 v_c; out vec4 o; void main() { o = v_c; }\n";

struct Tile {
	const char* name;
	int x, y;
};
static std::vector<Tile> g_tiles;
static int g_next;

// Sets the viewport to the next 128x128 tile and records its name.
static void tile(const char* name)
{
	const int x = 16 + (g_next % 8) * 144, y = 16 + (g_next / 8) * 144;
	g_next++;
	glViewport(x, y, 128, 128);
	g_tiles.push_back({name, x + 64, y + 64});
}

int main(int, char**)
{
	g_log = fopen("glprobe.log", "w");
	if (!g_log)
		g_log = stderr;
	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_Window* w = SDL_CreateWindow("glprobe", 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
	SDL_GLContext c = w ? SDL_GL_CreateContext(w) : NULL;
	if (!c || !gladLoadGLLoader(SDL_GL_GetProcAddress)) {
		LOG("no GL context: %s\n", SDL_GetError());
		return 1;
	}
	LOG("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

	// A full-tile quad as two triangles, positions in a VBO.
	const float quad[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
	const GLuint quad_u[] = {0, 1, 2, 3};
	const GLuint idx[] = {0, 1, 2, 2, 1, 3};
	GLuint vao, vbo, vbo_i, ebo, ubo, stream;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	glGenBuffers(1, &vbo_i);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_i);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad_u, quad_u, GL_STATIC_DRAW);
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);

	// Uniform block contents: green as floats, and as uint bits at index 5.
	GLuint words[184 * 4] = {};
	const float green[4] = {0, 1, 0, 1};
	memcpy(&words[0], green, 16);
	memcpy(&words[5 * 4], green, 16);
	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, sizeof words, words, GL_STATIC_DRAW);

	// One streaming buffer holding vertices and, at an aligned offset, the
	// uniform block (as the port's renderer does).
	GLint align = 256;
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
	const GLintptr ubo_off = ((sizeof quad + align - 1) / align) * align;
	glGenBuffers(1, &stream);
	glBindBuffer(GL_ARRAY_BUFFER, stream);
	glBufferData(GL_ARRAY_BUFFER, ubo_off + sizeof words, NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof quad, quad);
	glBufferSubData(GL_ARRAY_BUFFER, ubo_off, sizeof words, words);

	const char* vs_pos = "#version 330 core\nin vec3 a_pos; void main() { gl_Position = vec4(a_pos, 1.0); }\n";
	GLuint p_plain = program(vs_pos, kGreen);
	GLuint p_uniform = program(vs_pos, "#version 330 core\nuniform vec4 u_c; out vec4 o; void main() { o = u_c; }\n");
	const char* ubo_fs_const = "#version 330 core\nlayout(std140) uniform B { vec4 c[184]; }; out vec4 o; void main() { o = c[0]; }\n";
	GLuint p_ubo_const = program(vs_pos, ubo_fs_const);
	// As the GX vertex shader reads its matrices: uvec4 words, dynamic index.
	const char* vs_ubo_dyn = "#version 330 core\nlayout(std140) uniform B { uvec4 xf[184]; };\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"float X(int i) { return uintBitsToFloat(xf[i >> 2][i & 3]); }\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = vec4(X(u_i), X(u_i + 1), X(u_i + 2), X(u_i + 3)); }\n";
	GLuint p_ubo_dyn = program(vs_ubo_dyn, kVaryingFs);
	// The parts of that pattern, one at a time.
	const char* vs_vec4_dyn = "#version 330 core\nlayout(std140) uniform B { vec4 c[184]; };\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = c[u_i >> 2]; }\n";
	GLuint p_vec4_dyn = program(vs_vec4_dyn, kVaryingFs);
	const char* vs_uvec4_dyn = "#version 330 core\nlayout(std140) uniform B { uvec4 xf[184]; };\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = uintBitsToFloat(xf[u_i >> 2]); }\n";
	GLuint p_uvec4_dyn = program(vs_uvec4_dyn, kVaryingFs);
	const char* vs_comp_dyn = "#version 330 core\nlayout(std140) uniform B { uvec4 xf[184]; };\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"float X(int i) { uvec4 v = xf[5]; return uintBitsToFloat(v[i & 3]); }\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = vec4(X(u_i), X(u_i + 1), X(u_i + 2), X(u_i + 3)); }\n";
	GLuint p_comp_dyn = program(vs_comp_dyn, kVaryingFs);
	// The workaround: load the uvec4 whole, then select the component.
	const char* vs_select = "#version 330 core\nlayout(std140) uniform B { uvec4 xf[184]; };\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"uint W(int i) { uvec4 v = xf[i >> 2]; int c = i & 3; return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w; }\n"
		"float X(int i) { return uintBitsToFloat(W(i)); }\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = vec4(X(u_i), X(u_i + 1), X(u_i + 2), X(u_i + 3)); }\n";
	GLuint p_select = program(vs_select, kVaryingFs);
	// Alternatives: a texture buffer, a storage buffer.
	const char* vs_tbo = "#version 330 core\nuniform usamplerBuffer u_xf;\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"float X(int i) { return uintBitsToFloat(texelFetch(u_xf, i).x); }\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = vec4(X(u_i), X(u_i + 1), X(u_i + 2), X(u_i + 3)); }\n";
	GLuint p_tbo = program(vs_tbo, kVaryingFs);
	const char* vs_ssbo = "#version 430 core\nlayout(std430, binding = 0) readonly buffer S { uint xf[]; };\n"
		"uniform int u_i; in vec3 a_pos; out vec4 v_c;\n"
		"float X(int i) { return uintBitsToFloat(xf[i]); }\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = vec4(X(u_i), X(u_i + 1), X(u_i + 2), X(u_i + 3)); }\n";
	GLuint p_ssbo = program(vs_ssbo, kVaryingFs);
	GLuint tbo_tex;
	glGenTextures(1, &tbo_tex);
	glBindTexture(GL_TEXTURE_BUFFER, tbo_tex);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, ubo);
	const char* vs_int = "#version 330 core\nin vec3 a_pos; in uint a_i; out vec4 v_c;\n"
		"void main() { gl_Position = vec4(a_pos, 1.0); v_c = a_i < 4u ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }\n";
	GLuint p_int = program(vs_int, kVaryingFs);

	// probe.vs + probe.fs beside the NRO: compile, link and draw them once
	// (an emulator translates a shader when it is first drawn with).
	const std::string pvs = read_file("probe.vs"), pfs = read_file("probe.fs");
	GLuint p_file = 0;
	if (!pvs.empty() && !pfs.empty()) {
		p_file = program(pvs.c_str(), pfs.c_str());
		LOG("probe.vs/probe.fs: program %u\n", p_file);
	}

	for (int frame = 0; frame < 120 && appletMainLoop(); frame++) {
		g_next = 0;
		g_tiles.clear();
		glDisable(GL_SCISSOR_TEST);
		glViewport(0, 0, 1280, 720);
		glClearColor(1.0f, 0.0f, 0.0f, 1.0f); // red: a tile that draws nothing
		glClear(GL_COLOR_BUFFER_BIT);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
		glDisableVertexAttribArray(1);

		tile("plain draw (VBO positions)");
		glUseProgram(p_plain);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		tile("plain uniform vec4");
		glUseProgram(p_uniform);
		glUniform4f(glGetUniformLocation(p_uniform, "u_c"), 0, 1, 0, 1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		tile("indexed draw (glDrawElements)");
		glUseProgram(p_plain);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

		tile("glDrawElementsBaseVertex");
		glDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0, 0);

		tile("uniform block, constant index");
		glUseProgram(p_ubo_const);
		glUniformBlockBinding(p_ubo_const, glGetUniformBlockIndex(p_ubo_const, "B"), 0);
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		tile("uniform block uvec4, dynamic index (GX)");
		glUseProgram(p_ubo_dyn);
		glUniformBlockBinding(p_ubo_dyn, glGetUniformBlockIndex(p_ubo_dyn, "B"), 0);
		glUniform1i(glGetUniformLocation(p_ubo_dyn, "u_i"), 20);
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		tile("uniform block from the vertex buffer (GX)");
		glBindBuffer(GL_ARRAY_BUFFER, stream);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
		glBindBufferRange(GL_UNIFORM_BUFFER, 0, stream, ubo_off, sizeof words);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
		const GLuint split[] = {p_vec4_dyn, p_uvec4_dyn, p_comp_dyn, p_select};
		const char* split_names[] = {"uniform block vec4, dynamic array index", "uniform block uvec4, dynamic array index",
		                             "uniform block uvec4, dynamic component", "uniform block, whole uvec4 + select (fix)"};
		for (int i = 0; i < 4; i++) {
			tile(split_names[i]);
			glUseProgram(split[i]);
			glUniformBlockBinding(split[i], glGetUniformBlockIndex(split[i], "B"), 0);
			glUniform1i(glGetUniformLocation(split[i], "u_i"), 20);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}

		tile("texture buffer, dynamic index");
		glUseProgram(p_tbo);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_BUFFER, tbo_tex);
		glUniform1i(glGetUniformLocation(p_tbo, "u_xf"), 0);
		glUniform1i(glGetUniformLocation(p_tbo, "u_i"), 20);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		tile("storage buffer, dynamic index");
		glUseProgram(p_ssbo);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ubo);
		glUniform1i(glGetUniformLocation(p_ssbo, "u_i"), 20);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		tile("integer vertex attribute (glVertexAttribIPointer)");
		glUseProgram(p_int);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_i);
		glEnableVertexAttribArray(1);
		glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 0, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray(1);

		if (p_file && frame == 5) {
			tile("probe.vs + probe.fs drawn (any colour)");
			glUseProgram(p_file);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glFinish();
			LOG("probe.vs/probe.fs drawn\n");
		}

		if (frame == 30) {
			glFinish();
			int pass = 0;
			for (const Tile& t : g_tiles) {
				unsigned char px[4] = {0};
				glReadPixels(t.x, t.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
				const bool ok = px[0] < 32 && px[1] > 224 && px[2] < 32;
				pass += ok;
				LOG("%s %-48s got %3u %3u %3u\n", ok ? "PASS" : "FAIL", t.name, px[0], px[1], px[2]);
			}
			LOG("%d of %d passed, gl error 0x%x\n", pass, (int)g_tiles.size(), glGetError());
		}
		SDL_GL_SwapWindow(w);
	}
	SDL_Quit();
	return 0;
}
