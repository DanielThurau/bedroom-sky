#include "renderer.h"
#include <iostream>
#include <cstring>

// --- Shader sources ---

static const char* sky_vs = R"(
#version 330 core
layout(location=0) in vec2 aPos;
out vec2 vNDC;
void main() {
    vNDC = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Sun-aware sky: reconstructs per-pixel view direction from inverse VP,
// blends day/twilight/night palettes by sun elevation, adds warm horizon
// bloom on the sun side and a tight bright disc where the sun is.
static const char* sky_fs = R"(
#version 330 core
in vec2 vNDC;
uniform mat4 uInvVP;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform float uSunEl;
uniform vec3 uMoonDir;
uniform float uMoonEl;
out vec4 FragColor;

void main() {
    // Per-pixel world-space view direction from NDC
    vec4 far_h = uInvVP * vec4(vNDC, 1.0, 1.0);
    vec3 far_p = far_h.xyz / far_h.w;
    vec3 dir = normalize(far_p - uCamPos);
    float up = dir.z;                        // ENU up component
    float t = clamp(up, 0.0, 1.0);           // 0 at horizon, 1 at zenith

    // How "night" it is. Full day above +6 deg, full night below -6 deg.
    float nightness = clamp(1.0 - (uSunEl + 6.0) / 12.0, 0.0, 1.0);
    // Twilight emphasis — peaks near the horizon, fades out past ±6 deg.
    float twilight = 1.0 - clamp(abs(uSunEl) / 6.0, 0.0, 1.0);

    // Palettes (top = zenith, bot = horizon)
    vec3 day_zenith   = vec3(0.28, 0.48, 0.88);
    vec3 day_horizon  = vec3(0.72, 0.83, 1.00);
    vec3 night_zenith = vec3(0.008, 0.010, 0.035);
    vec3 night_horizon= vec3(0.035, 0.045, 0.095);

    vec3 day_col   = mix(day_horizon, day_zenith, smoothstep(0.0, 0.65, t));
    vec3 night_col = mix(night_horizon, night_zenith, smoothstep(0.0, 0.65, t));
    vec3 base = mix(day_col, night_col, nightness);

    // Twilight horizon wash: warm on sun side, cool opposite side.
    vec2 hdir = normalize(dir.xy + 1e-6);
    vec2 sdir = normalize(uSunDir.xy + 1e-6);
    float sun_side = clamp(dot(hdir, sdir), -1.0, 1.0);
    float horizon_proximity = pow(1.0 - t, 3.0);     // strong near horizon
    vec3 warm = vec3(1.00, 0.52, 0.28);
    vec3 cool = vec3(0.22, 0.18, 0.42);
    vec3 twilight_add = mix(cool, warm, 0.5 + 0.5 * sun_side)
                      * horizon_proximity * twilight * 0.9;
    base += twilight_add;

    // Sun disc bloom — only visible when sun is up, only near the sun dir.
    float sun_vis = smoothstep(-2.0, 2.0, uSunEl);
    float sun_dot = max(0.0, dot(dir, uSunDir));
    float sun_halo = pow(sun_dot, 6.0) * 0.18;
    float sun_core = pow(sun_dot, 180.0) * 0.6;
    base += vec3(1.00, 0.90, 0.65) * (sun_halo + sun_core) * sun_vis;

    // Faint moon halo at night — subtle, keeps the sky feeling lit.
    float moon_vis = smoothstep(-2.0, 2.0, uMoonEl) * nightness;
    float moon_dot = max(0.0, dot(dir, uMoonDir));
    float moon_halo = pow(moon_dot, 16.0) * 0.06;
    base += vec3(0.75, 0.82, 1.0) * moon_halo * moon_vis;

    FragColor = vec4(base, 1.0);
}
)";

static const char* marker_vs = R"(
#version 330 core
layout(location=0) in vec2 aPos;
uniform mat4 uTransform;
void main() {
    gl_Position = uTransform * vec4(aPos, 0.0, 1.0);
    gl_PointSize = 8.0;
}
)";

static const char* marker_fs = R"(
#version 330 core
uniform vec4 uColor;
out vec4 FragColor;
void main() {
    FragColor = uColor;
}
)";

static const char* terrain_vs = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aColor;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* terrain_fs = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

static const char* text_vs = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uProj;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)";

static const char* text_fs = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uFont;
uniform vec3 uColor;
out vec4 FragColor;
void main() {
    float a = texture(uFont, vUV).r;
    if (a < 0.5) discard;
    FragColor = vec4(uColor, 1.0);
}
)";

// 3D ENU shader — vertices are in ENU coordinates, projected GPU-side
static const char* enu3d_vs = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aColor;
uniform mat4 uVP;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)";

static const char* enu3d_fs = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

// Celestial disc shader — quad with UV -1..1, rendered as soft-edged disc
static const char* celestial_vs = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uVP;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)";

static const char* celestial_fs = R"(
#version 330 core
in vec2 vUV;
uniform vec3 uCore;
uniform vec3 uHalo;
uniform float uIntensity;
out vec4 FragColor;
void main() {
    float r = length(vUV);
    if (r > 1.0) discard;
    float core = smoothstep(0.75, 0.55, r);
    float halo = smoothstep(1.0, 0.35, r);
    vec3 col = mix(uHalo, uCore, core);
    float a = halo * uIntensity;
    FragColor = vec4(col, a);
}
)";

// --- Helpers ---

static GLuint compile_shader(const char* src, GLenum type) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    int ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, nullptr, log);
        std::cerr << "Program link error: " << log << std::endl;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// --- Renderer init/shutdown ---

bool Renderer::init(int width, int height, const char* title) {
    width_ = width;
    height_ = height;

    if (!glfwInit()) {
        std::cerr << "Failed to init GLFW" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        std::cerr << "Failed to create window" << std::endl;
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GLAD" << std::endl;
        return false;
    }

    int fb_w, win_w;
    glfwGetFramebufferSize(window_, &fb_w, nullptr);
    glfwGetWindowSize(window_, &win_w, nullptr);
    dpi_scale_ = (win_w > 0) ? (float)fb_w / (float)win_w : 1.0f;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);

    init_shaders();
    init_geometry();
    init_font();

    return true;
}

void Renderer::shutdown() {
    if (sky_shader_) glDeleteProgram(sky_shader_);
    if (marker_shader_) glDeleteProgram(marker_shader_);
    if (text_shader_) glDeleteProgram(text_shader_);
    if (terrain_shader_) glDeleteProgram(terrain_shader_);
    if (enu3d_shader_) glDeleteProgram(enu3d_shader_);
    if (celestial_shader_) glDeleteProgram(celestial_shader_);
    if (celestial_vao_) glDeleteVertexArrays(1, &celestial_vao_);
    if (celestial_vbo_) glDeleteBuffers(1, &celestial_vbo_);
    if (quad_vao_) glDeleteVertexArrays(1, &quad_vao_);
    if (quad_vbo_) glDeleteBuffers(1, &quad_vbo_);
    if (marker_vao_) glDeleteVertexArrays(1, &marker_vao_);
    if (marker_vbo_) glDeleteBuffers(1, &marker_vbo_);
    if (terrain_vao_) glDeleteVertexArrays(1, &terrain_vao_);
    if (terrain_vbo_) glDeleteBuffers(1, &terrain_vbo_);
    if (buildings_vao_) glDeleteVertexArrays(1, &buildings_vao_);
    if (buildings_vbo_) glDeleteBuffers(1, &buildings_vbo_);
    if (buildings_edge_vao_) glDeleteVertexArrays(1, &buildings_edge_vao_);
    if (buildings_edge_vbo_) glDeleteBuffers(1, &buildings_edge_vbo_);
    if (streets_vao_) glDeleteVertexArrays(1, &streets_vao_);
    if (streets_vbo_) glDeleteBuffers(1, &streets_vbo_);
    if (minimap_vao_) glDeleteVertexArrays(1, &minimap_vao_);
    if (minimap_vbo_) glDeleteBuffers(1, &minimap_vbo_);
    if (batch2d_vao_) glDeleteVertexArrays(1, &batch2d_vao_);
    if (batch2d_vbo_) glDeleteBuffers(1, &batch2d_vbo_);
    if (font_vao_) glDeleteVertexArrays(1, &font_vao_);
    if (font_vbo_) glDeleteBuffers(1, &font_vbo_);
    if (font_texture_) glDeleteTextures(1, &font_texture_);
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

bool Renderer::should_close() const {
    return glfwWindowShouldClose(window_);
}

void Renderer::begin_frame() {
    glfwPollEvents();
    glfwGetFramebufferSize(window_, &width_, &height_);
    glViewport(0, 0, width_, height_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::end_frame() {
    glfwSwapBuffers(window_);
}

void Renderer::init_shaders() {
    sky_shader_ = link_program(
        compile_shader(sky_vs, GL_VERTEX_SHADER),
        compile_shader(sky_fs, GL_FRAGMENT_SHADER));
    marker_shader_ = link_program(
        compile_shader(marker_vs, GL_VERTEX_SHADER),
        compile_shader(marker_fs, GL_FRAGMENT_SHADER));
    text_shader_ = link_program(
        compile_shader(text_vs, GL_VERTEX_SHADER),
        compile_shader(text_fs, GL_FRAGMENT_SHADER));
    terrain_shader_ = link_program(
        compile_shader(terrain_vs, GL_VERTEX_SHADER),
        compile_shader(terrain_fs, GL_FRAGMENT_SHADER));
    enu3d_shader_ = link_program(
        compile_shader(enu3d_vs, GL_VERTEX_SHADER),
        compile_shader(enu3d_fs, GL_FRAGMENT_SHADER));
    celestial_shader_ = link_program(
        compile_shader(celestial_vs, GL_VERTEX_SHADER),
        compile_shader(celestial_fs, GL_FRAGMENT_SHADER));

    // Cache uniform locations
    marker_uTransform_ = glGetUniformLocation(marker_shader_, "uTransform");
    marker_uColor_ = glGetUniformLocation(marker_shader_, "uColor");
    enu3d_uVP_ = glGetUniformLocation(enu3d_shader_, "uVP");
    text_uFont_ = glGetUniformLocation(text_shader_, "uFont");
    text_uColor_ = glGetUniformLocation(text_shader_, "uColor");
    text_uProj_ = glGetUniformLocation(text_shader_, "uProj");
    celestial_uVP_ = glGetUniformLocation(celestial_shader_, "uVP");
    celestial_uCore_ = glGetUniformLocation(celestial_shader_, "uCore");
    celestial_uHalo_ = glGetUniformLocation(celestial_shader_, "uHalo");
    celestial_uIntensity_ = glGetUniformLocation(celestial_shader_, "uIntensity");
    sky_uInvVP_ = glGetUniformLocation(sky_shader_, "uInvVP");
    sky_uCamPos_ = glGetUniformLocation(sky_shader_, "uCamPos");
    sky_uSunDir_ = glGetUniformLocation(sky_shader_, "uSunDir");
    sky_uSunEl_ = glGetUniformLocation(sky_shader_, "uSunEl");
    sky_uMoonDir_ = glGetUniformLocation(sky_shader_, "uMoonDir");
    sky_uMoonEl_ = glGetUniformLocation(sky_shader_, "uMoonEl");
}

void Renderer::init_geometry() {
    // Fullscreen quad
    float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glGenVertexArrays(1, &quad_vao_);
    glGenBuffers(1, &quad_vbo_);
    glBindVertexArray(quad_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // Dynamic marker buffer
    glGenVertexArrays(1, &marker_vao_);
    glGenBuffers(1, &marker_vbo_);
    glBindVertexArray(marker_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 16384 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // Terrain batch buffer (interleaved pos3 + color4 = 7 floats, GPU-projected)
    glGenVertexArrays(1, &terrain_vao_);
    glGenBuffers(1, &terrain_vbo_);
    glBindVertexArray(terrain_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, terrain_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 70000 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // 3D ENU buffers for buildings and streets (interleaved pos3 + color4 = 7 floats)
    auto init_enu3d_vao = [](GLuint& vao, GLuint& vbo) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, 7 * sizeof(float), nullptr, GL_STREAM_DRAW);
        // pos3
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        // color4
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    };
    init_enu3d_vao(buildings_vao_, buildings_vbo_);
    init_enu3d_vao(buildings_edge_vao_, buildings_edge_vbo_);
    init_enu3d_vao(streets_vao_, streets_vbo_);

    // Minimap batch buffer (pos2 + color4 = 6 floats)
    glGenVertexArrays(1, &minimap_vao_);
    glGenBuffers(1, &minimap_vbo_);
    glBindVertexArray(minimap_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, minimap_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Generic 2D colored batch buffer (pos2 + color4 = 6 floats per vert)
    glGenVertexArrays(1, &batch2d_vao_);
    glGenBuffers(1, &batch2d_vbo_);
    glBindVertexArray(batch2d_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, batch2d_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Celestial billboard buffer (pos3 + uv2 = 5 floats per vert, 6 verts per body)
    glGenVertexArrays(1, &celestial_vao_);
    glGenBuffers(1, &celestial_vbo_);
    glBindVertexArray(celestial_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, celestial_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
}

void Renderer::init_font() {
    static const unsigned char font_data[96][8] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
        {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 '!'
        {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, // 34 '"'
        {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 35 '#'
        {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // 36 '$'
        {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // 37 '%'
        {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 38 '&'
        {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 39 '''
        {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 40 '('
        {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 41 ')'
        {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 '*'
        {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43 '+'
        {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 44 ','
        {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45 '-'
        {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 '.'
        {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // 47 '/'
        {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, // 48 '0'
        {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00}, // 49 '1'
        {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, // 50 '2'
        {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // 51 '3'
        {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // 52 '4'
        {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // 53 '5'
        {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // 54 '6'
        {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // 55 '7'
        {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // 56 '8'
        {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // 57 '9'
        {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // 58 ':'
        {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // 59 ';'
        {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // 60 '<'
        {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // 61 '='
        {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // 62 '>'
        {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // 63 '?'
        {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // 64 '@'
        {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00}, // 65 'A'
        {0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00}, // 66 'B'
        {0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00}, // 67 'C'
        {0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00}, // 68 'D'
        {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00}, // 69 'E'
        {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00}, // 70 'F'
        {0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7E,0x00}, // 71 'G'
        {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 72 'H'
        {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 73 'I'
        {0x1E,0x0C,0x0C,0x0C,0x0C,0xCC,0x78,0x00}, // 74 'J'
        {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00}, // 75 'K'
        {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00}, // 76 'L'
        {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 77 'M'
        {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 78 'N'
        {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 79 'O'
        {0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00}, // 80 'P'
        {0x7C,0xC6,0xC6,0xC6,0xD6,0xCC,0x76,0x00}, // 81 'Q'
        {0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00}, // 82 'R'
        {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, // 83 'S'
        {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 84 'T'
        {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 85 'U'
        {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 86 'V'
        {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 87 'W'
        {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 88 'X'
        {0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x00}, // 89 'Y'
        {0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00}, // 90 'Z'
        {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 91 '['
        {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // 92 '\'
        {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 93 ']'
        {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // 94 '^'
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE}, // 95 '_'
        {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 96 '`'
        {0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00}, // 97 'a'
        {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00}, // 98 'b'
        {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 99 'c'
        {0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00}, //100 'd'
        {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, //101 'e'
        {0x1C,0x36,0x30,0x7C,0x30,0x30,0x30,0x00}, //102 'f'
        {0x00,0x00,0x7E,0xC6,0x7E,0x06,0x7C,0x00}, //103 'g'
        {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, //104 'h'
        {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, //105 'i'
        {0x0C,0x00,0x1C,0x0C,0x0C,0xCC,0x78,0x00}, //106 'j'
        {0xC0,0xC0,0xCC,0xD8,0xF0,0xD8,0xCC,0x00}, //107 'k'
        {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, //108 'l'
        {0x00,0x00,0xCC,0xFE,0xD6,0xC6,0xC6,0x00}, //109 'm'
        {0x00,0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, //110 'n'
        {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, //111 'o'
        {0x00,0x00,0xFC,0xC6,0xFC,0xC0,0xC0,0x00}, //112 'p'
        {0x00,0x00,0x7E,0xC6,0x7E,0x06,0x06,0x00}, //113 'q'
        {0x00,0x00,0xDC,0xE6,0xC0,0xC0,0xC0,0x00}, //114 'r'
        {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, //115 's'
        {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00}, //116 't'
        {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x00}, //117 'u'
        {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, //118 'v'
        {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00}, //119 'w'
        {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, //120 'x'
        {0x00,0x00,0xC6,0xC6,0x7E,0x06,0x7C,0x00}, //121 'y'
        {0x00,0x00,0xFE,0x0C,0x38,0x60,0xFE,0x00}, //122 'z'
        {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, //123 '{'
        {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, //124 '|'
        {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, //125 '}'
        {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, //126 '~'
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, //127 DEL
    };

    const int char_w = 8, char_h = 8;
    const int num_chars = 96;
    const int tex_w = num_chars * char_w;
    const int tex_h = char_h;
    unsigned char* pixels = new unsigned char[tex_w * tex_h];
    memset(pixels, 0, tex_w * tex_h);

    for (int ch = 0; ch < num_chars; ch++) {
        for (int row = 0; row < 8; row++) {
            unsigned char bits = font_data[ch][row];
            int tex_y = (7 - row);
            for (int bit = 0; bit < 8; bit++) {
                if (bits & (0x80 >> bit)) {
                    pixels[tex_y * tex_w + ch * char_w + bit] = 255;
                }
            }
        }
    }

    glGenTextures(1, &font_texture_);
    glBindTexture(GL_TEXTURE_2D, font_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, tex_w, tex_h, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    delete[] pixels;

    glGenVertexArrays(1, &font_vao_);
    glGenBuffers(1, &font_vbo_);
    glBindVertexArray(font_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, font_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float) * 512, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
}
