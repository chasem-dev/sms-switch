// The GLSL adapter of the deko3d GL driver (dkgl): turns a GL program's
// vertex, geometry and fragment shaders into what deko3d's compiler (uam) takes, and
// says where everything went.
//
// uam has no linking and no driver-held uniforms: every uniform block,
// sampler, input and output needs an explicit binding or location. So:
// - loose uniforms (the GL "default block") move into one std140 block at
//   kDefaultBlockBinding, the same in every stage, laid out here; the driver
//   keeps its bytes and binds it at each draw
// - each sampler (or sampler array) gets texture bindings from 0, in order
// - each named uniform block gets binding 0, 1, ... in order (the driver maps
//   them to the GL binding points glUniformBlockBinding chose)
// - vertex inputs get the locations glBindAttribLocation gave (the others the
//   next free ones), fragment outputs those of glBindFragDataLocation (else
//   0), and varyings matching locations by name across each stage boundary
// - #version becomes 460 (layout bindings need 420)
#ifndef SMS_SWITCH_DKGL_GLSL_H
#define SMS_SWITCH_DKGL_GLSL_H

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace dkgl {

enum { kDefaultBlockBinding = 15, kMaxSamplerBindings = 32 };
// stage bits (GlslUniform::stages, GlslProgram::blockStages)
enum { kStageVertex = 1, kStageFragment = 2, kStageGeometry = 4 };

// A loose uniform, now a member of the default block.
struct GlslUniform {
    std::string name;
    uint32_t type;    // GL type enum (GL_FLOAT_VEC4, GL_INT, GL_SAMPLER_2D, ...)
    int arraySize;    // 1 if not an array
    uint32_t offset;  // in the default block (samplers: unused)
    uint32_t stride;  // between array elements (std140: 16)
    int binding;      // samplers: the first texture binding; else -1
    uint32_t stages;  // samplers: the stages using it (kStage*)
};

struct GlslProgram {
    std::string vs, gs, fs;           // for uam (gs: empty without a geometry shader)
    std::vector<GlslUniform> uniforms;  // loose uniforms and samplers
    std::vector<std::string> blocks;  // named uniform blocks, by binding
    std::vector<uint32_t> blockStages;  // the stages using each (kStage*)
    uint32_t defaultBlockSize = 0;    // bytes, a multiple of 16 (0: none)
    std::map<std::string, int> attribs;  // vertex input locations
    std::string error;                // why adapting failed
};

// Adapts a program's stages (gs empty if it has none). attribBindings and
// fragBindings are what glBindAttribLocation and glBindFragDataLocation set.
// False (with out->error) for something the adapter cannot take.
bool adaptProgram(const std::string& vs, const std::string& gs, const std::string& fs,
                  const std::map<std::string, int>& attribBindings, const std::map<std::string, int>& fragBindings,
                  GlslProgram* out);

// The size in bytes of one element of a GL uniform type (0 if unknown).
uint32_t glslTypeSize(uint32_t type);

}  // namespace dkgl

#endif
