#include "core/render/GlLines.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace bedrocktoolsplus::core::gllines {
namespace {

// GLES3 constant and entry point, resolved at runtime so that a device
// without them only loses the extra safety and not the library load.
constexpr int kVertexArrayBinding = 0x85B5;
constexpr int kAttribPosition = 0;

const char* kVertexSource =
    "attribute vec3 aPosition;"
    "uniform mat4 uMatrix;"
    "void main() { gl_Position = uMatrix * vec4(aPosition, 1.0); }";

const char* kFragmentSource =
    "precision mediump float;"
    "uniform vec4 uColor;"
    "void main() { gl_FragColor = uColor; }";

GLuint s_program = 0;
GLuint s_buffer = 0;
GLint s_matrixLocation = -1;
GLint s_colorLocation = -1;
bool s_prepared = false;
bool s_ready = false;

using BindVertexArrayFn = void (*)(unsigned int);
BindVertexArrayFn s_bindVertexArray = nullptr;

GLuint compileShader(unsigned int type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

void prepare() {
    if (s_prepared) return;
    s_prepared = true;

    s_bindVertexArray = reinterpret_cast<BindVertexArrayFn>(dlsym(RTLD_DEFAULT, "glBindVertexArray"));

    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexSource);
    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentSource);
    if (!vertexShader || !fragmentShader) {
        if (vertexShader) glDeleteShader(vertexShader);
        if (fragmentShader) glDeleteShader(fragmentShader);
        return;
    }

    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return;
    }

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glBindAttribLocation(program, kAttribPosition, "aPosition");
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(program);
        return;
    }

    s_program = program;
    s_matrixLocation = glGetUniformLocation(program, "uMatrix");
    s_colorLocation = glGetUniformLocation(program, "uColor");
    glGenBuffers(1, &s_buffer);

    if (s_matrixLocation < 0 || s_colorLocation < 0 || s_buffer == 0) {
        if (s_buffer) glDeleteBuffers(1, &s_buffer);
        glDeleteProgram(s_program);
        s_program = 0;
        s_buffer = 0;
        return;
    }

    s_ready = true;
}

void transform(const float m[16], float x, float y, float z, float& outX, float& outY, float& outZ, float& outW) {
    outX = m[0] * x + m[4] * y + m[8] * z + m[12];
    outY = m[1] * x + m[5] * y + m[9] * z + m[13];
    outZ = m[2] * x + m[6] * y + m[10] * z + m[14];
    outW = m[3] * x + m[7] * y + m[11] * z + m[15];
}

// A matrix is accepted as the view projection when it puts a point five blocks
// ahead of the camera near the middle of the screen, and does not put a point
// behind the camera inside the frustum. Both checks together reject object
// matrices and degenerate reads without knowing any shader names.
bool looksLikeViewProjection(const float m[16], const Camera& camera) {
    glm::vec3 forward(camera.forward[0], camera.forward[1], camera.forward[2]);
    if (!(glm::dot(forward, forward) > 0.0f)) return false;
    forward = glm::normalize(forward);

    const glm::vec3 eye(camera.position[0], camera.position[1], camera.position[2]);
    const glm::vec3 ahead = eye + forward * 5.0f;
    const glm::vec3 behind = eye - forward * 3.0f;

    float ax = 0.0f, ay = 0.0f, az = 0.0f, aw = 0.0f;
    transform(m, ahead.x, ahead.y, ahead.z, ax, ay, az, aw);
    if (!(aw > 0.0f)) return false;

    const float ndcX = ax / aw;
    const float ndcY = ay / aw;
    const float ndcZ = az / aw;
    if (std::fabs(ndcX) > 0.6f || std::fabs(ndcY) > 0.6f) return false;
    if (!(ndcZ > -1.0f && ndcZ < 1.0f)) return false;

    float bx = 0.0f, by = 0.0f, bz = 0.0f, bw = 0.0f;
    transform(m, behind.x, behind.y, behind.z, bx, by, bz, bw);
    if (bw > 0.0f) {
        const float behindX = bx / bw;
        const float behindY = by / bw;
        const float behindZ = bz / bw;
        if (std::fabs(behindX) < 1.0f && std::fabs(behindY) < 1.0f && behindZ > -1.0f && behindZ < 1.0f) return false;
    }

    return true;
}

bool matrixFromCurrentProgram(float out[16], const Camera& camera) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program == 0) return false;

    GLint uniformCount = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    if (uniformCount <= 0 || uniformCount > 256) return false;

    GLint maxNameLength = 64;
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLength);
    if (maxNameLength < 16) maxNameLength = 16;
    if (maxNameLength > 256) maxNameLength = 256;

    std::vector<char> name(static_cast<std::size_t>(maxNameLength) + 1, '\0');

    for (GLint index = 0; index < uniformCount; ++index) {
        GLsizei written = 0;
        GLint size = 0;
        GLenum type = 0;
        name.assign(name.size(), '\0');
        glGetActiveUniform(program, static_cast<unsigned int>(index), maxNameLength, &written, &size, &type, name.data());
        if (type != GL_FLOAT_MAT4) continue;

        const GLint location = glGetUniformLocation(program, name.data());
        if (location < 0) continue;

        float candidate[16] = {};
        glGetUniformfv(program, location, candidate);

        bool sane = true;
        for (float value : candidate) {
            if (!std::isfinite(value)) {
                sane = false;
                break;
            }
        }
        if (!sane) continue;

        if (looksLikeViewProjection(candidate, camera)) {
            std::memcpy(out, candidate, sizeof(candidate));
            return true;
        }
    }

    return false;
}

void matrixFromCamera(float out[16], const Camera& camera) {
    glm::vec3 forward(camera.forward[0], camera.forward[1], camera.forward[2]);
    if (!(glm::dot(forward, forward) > 0.0f)) forward = glm::vec3(0.0f, 0.0f, -1.0f);
    forward = glm::normalize(forward);

    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::fabs(glm::dot(forward, up)) > 0.999f) up = glm::vec3(0.0f, 0.0f, 1.0f);

    const glm::vec3 eye(camera.position[0], camera.position[1], camera.position[2]);
    const glm::mat4 view = glm::lookAt(eye, eye + forward, up);

    float fov = camera.fovDegrees;
    if (!(fov > 10.0f && fov < 140.0f)) fov = 70.0f;

    GLint viewport[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, viewport);
    float aspect = viewport[3] > 0 ? static_cast<float>(viewport[2]) / static_cast<float>(viewport[3]) : 1.0f;
    if (!(aspect > 0.05f && aspect < 20.0f)) aspect = 1.0f;

    const glm::mat4 projection = glm::perspective(glm::radians(fov), aspect, 0.05f, 1000.0f);
    const glm::mat4 viewProjection = projection * view;
    std::memcpy(out, glm::value_ptr(viewProjection), 16 * sizeof(float));
}

struct State {
    GLint program = 0;
    GLint arrayBuffer = 0;
    GLint elementBuffer = 0;
    GLint vertexArray = 0;
    GLboolean depthTest = 0;
    GLboolean blend = 0;
    GLboolean cullFace = 0;
    GLboolean depthMask = 0;
    GLint blendSrc = 0;
    GLint blendDst = 0;
};

void saveState(State& state) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.arrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state.elementBuffer);
    glGetBooleanv(GL_DEPTH_TEST, &state.depthTest);
    glGetBooleanv(GL_BLEND, &state.blend);
    glGetBooleanv(GL_CULL_FACE, &state.cullFace);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depthMask);
    glGetIntegerv(GL_BLEND_SRC_RGB, &state.blendSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &state.blendDst);

    state.vertexArray = 0;
    if (s_bindVertexArray) {
        glGetIntegerv(kVertexArrayBinding, &state.vertexArray);
        // Draw with the default array object so the game's own vertex setup is
        // never overwritten.
        s_bindVertexArray(0);
    }
}

void restoreState(const State& state) {
    glDisableVertexAttribArray(kAttribPosition);
    glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.elementBuffer);
    glUseProgram(state.program);

    if (state.depthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);

    if (state.blend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);

    if (state.cullFace) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);

    glDepthMask(state.depthMask);
    glBlendFunc(state.blendSrc, state.blendDst);

    if (s_bindVertexArray) s_bindVertexArray(state.vertexArray);
}

} // namespace

bool drawSegments(const Camera& camera, const float* vertices, std::size_t vertexCount, std::uint32_t color) {
    if (!vertices || vertexCount < 2 || (vertexCount % 2) != 0) return false;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return false;

    prepare();
    if (!s_ready) return false;

    float matrix[16] = {};
    if (!matrixFromCurrentProgram(matrix, camera)) matrixFromCamera(matrix, camera);

    State state{};
    saveState(state);

    glUseProgram(s_program);
    glUniformMatrix4fv(s_matrixLocation, 1, GL_FALSE, matrix);
    glUniform4f(s_colorLocation,
                ((color >> 16) & 0xFF) / 255.0f,
                ((color >> 8) & 0xFF) / 255.0f,
                (color & 0xFF) / 255.0f,
                ((color >> 24) & 0xFF) / 255.0f);

    glBindBuffer(GL_ARRAY_BUFFER, s_buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<long>(vertexCount * 3 * sizeof(float)), vertices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(kAttribPosition);
    glVertexAttribPointer(kAttribPosition, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Draw over the world without disturbing it: no culling, no depth writes,
    // and blending left as the game had it.
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    glDrawArrays(GL_LINES, 0, static_cast<int>(vertexCount));

    restoreState(state);
    return true;
}

} // namespace bedrocktoolsplus::core::gllines
