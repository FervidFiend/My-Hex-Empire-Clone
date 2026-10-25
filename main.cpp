#include "Game.h"
#include <fstream>
#include <cstdlib>

// Deterministic framebuffer capture for comparisons with the Flash reference.
// Set HEX_EMPIRE_CAPTURE to an output .ppm path; no desktop chrome is captured.
static bool CaptureFrame(const char* path) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(extras::ActiveWindow, &width, &height);
    if (width <= 0 || height <= 0) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int row = height - 1; row >= 0; --row)
        output.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(row) * width * 3), width * 3);
    return output.good();
}

// ---------------- Main ----------------
int main() {
    char* capturePath = nullptr;
    size_t capturePathSize = 0;
    _dupenv_s(&capturePath, &capturePathSize, "HEX_EMPIRE_CAPTURE");
    if (capturePath) {
        InitLib();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    InitWindow(800, 600, "Hex Empire");
    InitGame();

    // ---------------- MAIN LOOP ----------------
    while (!WindowShouldClose()) {
        PollCustomEvents();
        float dt = GetFrameTime();

        UpdateGame(dt);

        BeginDrawing();
        DrawGame();

        if (capturePath) {
            const bool captured = CaptureFrame(capturePath);
            std::free(capturePath);
            ShutdownGame();
            CloseWindow();
            return captured ? 0 : 1;
        }

        EndDrawing();

    }

    ShutdownGame();
    CloseWindow();
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int) {
    return main();
}
