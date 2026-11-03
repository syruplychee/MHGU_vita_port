// Replay private vertex-pass captures on desktop OpenGL. No game data is built in.
// Input: raster-cases.txt, and per-case rastermeta.txt (vertex/index counts),
// gxp16.bin (prepass float4 clip positions), clip-main.bin, indices.bin (uint16).
// Requires external GLAD, Windows OpenGL, and a GL 4.3-capable driver.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<char> read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    return shader;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
        std::string root = argv[1];
        WNDCLASSA window_class{};
        window_class.lpfnWndProc = DefWindowProcA;
        window_class.hInstance = GetModuleHandleA(nullptr);
        window_class.lpszClassName = "VPVertexDepthReplay";
        window_class.style = CS_OWNDC;
        RegisterClassA(&window_class);
        HWND window = CreateWindowA(window_class.lpszClassName, "replay", WS_POPUP,
            0, 0, 16, 16, nullptr, nullptr, window_class.hInstance, nullptr);
        HDC dc = GetDC(window);
        PIXELFORMATDESCRIPTOR format{};
        format.nSize = sizeof(format);
        format.nVersion = 1;
        format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
        format.iPixelType = PFD_TYPE_RGBA;
        format.cColorBits = 32;
        if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &format), &format) ||
            !wglMakeCurrent(dc, wglCreateContext(dc)) || !gladLoadGL())
            throw std::runtime_error("Cannot create OpenGL context");

        GLuint program = glCreateProgram();
        glAttachShader(program, compile_shader(GL_VERTEX_SHADER,
            "#version 430\nlayout(location=0)in vec4 clip;"
            "void main(){gl_Position=clip;}"));
        glAttachShader(program, compile_shader(GL_FRAGMENT_SHADER,
            "#version 430\nout vec4 color;void main(){color=vec4(1);}"));
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) throw std::runtime_error("Cannot link replay program");
        glUseProgram(program);

        GLuint vao, vertices, indices, framebuffer, color, depth;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vertices);
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
        glGenBuffers(1, &indices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 960, 540, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, color, 0);
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 960, 540);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
            GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Incomplete replay framebuffer");
        glViewport(0, 0, 960, 540);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);

        std::ifstream list(root + "/raster-cases.txt");
        if (!list) throw std::runtime_error("Missing case list");
        std::string name;
        unsigned cases = 0, total_lost = 0;
        while (list >> name) {
            std::string directory = root + "/" + name;
            unsigned vertex_count = 0, index_count = 0;
            std::ifstream metadata(directory + "/rastermeta.txt");
            if (!(metadata >> vertex_count >> index_count))
                throw std::runtime_error("Invalid metadata: " + name);
            auto pre = read_file(directory + "/gxp16.bin");
            auto main = read_file(directory + "/clip-main.bin");
            auto index_data = read_file(directory + "/indices.bin");
            if (vertex_count < 3 || index_count < 3 || pre.size() != vertex_count * 16ull ||
                main.size() != pre.size() || index_data.size() != index_count * 2ull)
                throw std::runtime_error("Capture size mismatch: " + name);
            for (std::size_t offset = 0; offset < index_data.size(); offset += 2) {
                uint16_t index;
                std::memcpy(&index, index_data.data() + offset, sizeof(index));
                if (index != UINT16_MAX && index >= vertex_count)
                    throw std::runtime_error("Vertex index outside capture: " + name);
            }
            for (std::size_t offset = 0; offset < pre.size(); offset += 4) {
                float first, second;
                std::memcpy(&first, pre.data() + offset, sizeof(first));
                std::memcpy(&second, main.data() + offset, sizeof(second));
                if (!std::isfinite(first) || !std::isfinite(second))
                    throw std::runtime_error("Nonfinite clip position: " + name);
            }
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_data.size(), index_data.data(), GL_STATIC_DRAW);
            std::vector<unsigned char> images[2];
            for (unsigned mode = 0; mode < 2; ++mode) {
                glColorMask(1, 1, 1, 1);
                glDepthMask(1);
                glClearColor(0, 0, 0, 0);
                glClearDepth(1);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glBufferData(GL_ARRAY_BUFFER, pre.size(), pre.data(), GL_STATIC_DRAW);
                glColorMask(0, 0, 0, 0);
                glDrawElements(GL_TRIANGLE_STRIP, index_count, GL_UNSIGNED_SHORT, nullptr);
                glColorMask(1, 1, 1, 1);
                glDepthMask(0);
                const auto &positions = mode ? main : pre;
                glBufferData(GL_ARRAY_BUFFER, positions.size(), positions.data(), GL_STATIC_DRAW);
                glDrawElements(GL_TRIANGLE_STRIP, index_count, GL_UNSIGNED_SHORT, nullptr);
                images[mode].resize(960 * 540 * 4);
                glReadPixels(0, 0, 960, 540, GL_RGBA, GL_UNSIGNED_BYTE, images[mode].data());
            }
            unsigned covered = 0, lost = 0, gained = 0;
            for (unsigned pixel = 0; pixel < 960 * 540; ++pixel) {
                bool reference = images[0][pixel * 4] != 0;
                bool candidate = images[1][pixel * 4] != 0;
                covered += reference;
                lost += reference && !candidate;
                gained += !reference && candidate;
            }
            if (glGetError() != GL_NO_ERROR) throw std::runtime_error("GL error: " + name);
            if (!covered) throw std::runtime_error("No reference pixels covered: " + name);
            std::printf("%s covered=%u lost=%u gained=%u\n", name.c_str(), covered, lost, gained);
            total_lost += lost;
            ++cases;
        }
        if (!cases) throw std::runtime_error("No cases supplied");
        std::printf("SUMMARY cases=%u lost=%u\n", cases, total_lost);
        return total_lost ? 1 : 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
