// The smallest program on the engine: a window, a few sprites that turn, a line of text.
//
//     medium_hello [ASSETS] [--frames N]
//
// ASSETS is the directory the renderer loads from: `shaders/sprite.vert.spv`, `shaders/sprite.frag.spv`
// and `font.ttf`. The build puts all three in <build>/assets, which is the default. `--frames N`
// draws N frames and exits, for a machine with nobody at it.
#include "medium/render.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

int main(int argc, char **argv) {
    try {
        med::fs::path assets = "assets";
        long frames = -1;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
                frames = std::strtol(argv[++i], nullptr, 10);
            else
                assets = argv[i];
        }
        med::Renderer renderer(assets, "medium-engine: hello");
        // Everything is drawn on a 1280x720 canvas, whatever size the window is.
        const med::Vec2 centre{640, 360};
        for (long frame = 0; frames < 0 || frame < frames; ++frame) {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_KEY_DOWN)
                    return 0;
            const float t = static_cast<float>(SDL_GetTicks()) / 1000.0f;
            renderer.rect({0, 0, 1280, 720}, {0.05f, 0.05f, 0.06f, 1}, 0);
            // Eight squares on a ring. A sprite is a textured rectangle with a colour, a layer to sort by
            // and an angle; the renderer's own white texture makes it a plain one.
            for (int i = 0; i < 8; ++i) {
                const float a = t * 0.6f + static_cast<float>(i) * 0.7853982f;
                med::Sprite sprite;
                sprite.texture = renderer.white_texture();
                sprite.bounds = {centre.x + std::cos(a) * 180 - 24, centre.y + std::sin(a) * 180 - 24, 48,
                                 48};
                sprite.color = {0.85f, 0.85f, 0.88f, 1};
                sprite.layer = 1;
                sprite.angle = a;
                renderer.add(sprite);
            }
            const std::string title = "medium-engine";
            renderer.text({centre.x - renderer.text_width(title, 2) / 2, centre.y - 16}, title,
                          {0.85f, 0.72f, 0.48f, 1}, 2);
            renderer.text({24, 680}, "any key to quit");
            renderer.present();
        }
    } catch (const std::exception &error) {
        std::cerr << "medium_hello: " << error.what() << '\n';
        return 1;
    }
}
