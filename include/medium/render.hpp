#pragma once
#include "medium/audio.hpp"
#include "medium/core.hpp"
#include <SDL3/SDL.h>
#include <memory>
namespace med {
// Color now lives in core.hpp: authored animation data carries a per-frame tint, and content
// types must not depend on the renderer. Everything that said `med::Color` still does.
struct Sprite {
    Handle texture;
    Rect bounds;
    Rect uv{0, 0, 1, 1};
    Color color;
    int layer{};
    float depth{}, angle{};
    bool additive{};
    // Multiplies what is already drawn by the sprite's colour: a light map over the world.
    // Wins over `additive` when both are set.
    bool multiply{};
    // Scissor rectangle in canvas coordinates. Empty means the whole view, which is what almost
    // every sprite wants. It used to default to a literal 1280x720 rectangle, which silently
    // became a real clip the moment a view showed a wider canvas than that: everything past
    // canvas x 1280 was scissored away and the view looked like it had stopped drawing.
    std::optional<Rect> clip{};
    // Which on-screen view this sprite belongs to. Bounds are always in the 1280x720 canvas,
    // so a view is only a different rectangle of the window to map that canvas onto - which is
    // what lets the editor draw the authoring view and the running game in the same frame with
    // two different cameras.
    int view{};
    // Soft focus in canvas pixels; used only by distant scenery. Zero keeps ordinary sprites sharp.
    float blur = 0;
    SpriteMask mask = SpriteMask::Quad;
};
struct TextureAtlas {
    Handle texture{};
    std::vector<Rect> regions; // normalized UVs, empty for an absent optional source
};
class Renderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    // Opens the window and the GPU device. `assets` is the directory it loads from:
    // `shaders/sprite.vert.spv`, `shaders/sprite.frag.spv`, `font.ttf` and, when there is one,
    // `font_display.ttf`.
    explicit Renderer(const fs::path &assets, const std::string &title = "Medium Engine");
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    SDL_Window *window() const;
    SDL_GPUDevice *device() const;
    Handle load_texture(const fs::path &path);
    // Upload a finished RGBA8 image once, without a persistent transfer buffer. Composed NPC
    // atlases are immutable while resident; only images rewritten by update_texture need staging.
    Handle load_pixels(int width, int height, std::span<const uint8_t> rgba, int levels = 1);
    // An image file's pixels as `load_texture` decodes them - RGBA8, rows top to bottom - for a caller
    // that packs several small images into one texture (the editor's icons) rather than spending a
    // texture slot on each. Throws for a file that is missing or is not an image.
    static std::vector<uint8_t> read_pixels(const fs::path &path, int &width, int &height);
    // Pack optional UI images in one texture. Trims transparent margins and scales each region
    // to at most 768x576; sources are untouched. Two transparent pixels separate adjacent regions.
    TextureAtlas load_atlas(std::span<const fs::path> paths);
    // A texture of `width` by `height` RGBA8 pixels to be written by `update_texture` - a light map
    // recomputed each frame. Linear filtered, like every texture.
    //
    // With `levels` above 1 it also carries a mip chain of up to that many levels (itself included), made
    // again from its pixels at every update and sampled between levels, for art drawn smaller than its
    // pixels - a composed character sheet (`composed_mip_levels`), which is packed so that no level mixes
    // two frames. Opt-in, because a texture packed tighter (glyphs, tiles, the interface atlas) would bleed
    // its neighbours into each other at the smaller levels. Fewer levels when the size does not halve
    // evenly that often, and none when the device cannot make them (`texture_levels` says which).
    Handle create_texture(int width, int height, int levels = 1);
    // Replaces every pixel of a texture made by `create_texture`; `rgba` is width * height * 4 bytes. A
    // texture with a mip chain has it made again from the new pixels.
    void update_texture(Handle handle, std::span<const uint8_t> rgba);
    // How many mip levels a texture has, itself included: 1 for every texture but one made with more; 0 for a
    // handle this renderer does not own.
    int texture_levels(Handle handle) const;
    // The 1x1 white texture a plain rectangle is drawn with, for a caller that needs a rectangle with
    // a blend of its own.
    Handle white_texture() const;
    // The texture as something an immediate-mode UI can draw, for the editor's sprite sheet and
    // asset previews. Returned as an opaque value rather than an ImGui type, because the
    // renderer must not depend on ImGui - what it actually is, is the address of a persistent
    // SDL_GPUTextureSamplerBinding owned by this renderer, which is what the SDL_GPU ImGui
    // backend takes as its ImTextureID. Zero for a handle this renderer does not own.
    uint64_t texture_id(Handle handle) const;
    // Pixel size, so a preview can lay a sprite sheet out without decoding the file again.
    Vec2 texture_size(Handle handle) const;
    // Draws the texture as pixel art: each texel hard-edged at whatever scale the view has, blended only
    // across the one screen pixel its edge falls in (a character baked from 3D to sprites).
    // Every texture starts filtered. A handle this renderer does not own is ignored.
    void set_pixel_art(Handle handle, bool crisp);
    void release(Handle handle);
    void add(Sprite sprite);
    void rect(Rect bounds, Color color, int layer = 100, float depth = 0);
    // Layer 110 by default: above world rects. A HUD draws its bands and their text above that
    // (`hud_layers`), so a world label cannot show through a HUD band.
    // UTF-8: printable ASCII, Latin-1 and Latin Extended-A, kerned; any other character draws as
    // '?'. With `wrap` above zero, broken into lines no wider than it, at spaces where it can. With
    // `fit` above zero, drawn smaller when its widest line would be wider than that: a label in a
    // fixed block stays in it in a longer language.
    // `face` picks the typeface (`TextFace`); both are sized so that scale 1 is the same line height.
    void text(Vec2 position, const std::string &text, Color color = {}, float scale = 1, int layer = 110,
              float wrap = 0, float fit = 0, TextFace face = TextFace::Body);
    // How wide `text` would be drawn at `scale`, its widest line when it has several.
    float text_width(std::string_view text, float scale = 1, TextFace face = TextFace::Body) const;
    // Whether `font_display.ttf` loaded; without it display text is set in the body face.
    bool has_display_face() const;
    bool has_glyph(char32_t codepoint) const;
    bool present(const std::function<void(SDL_GPUCommandBuffer *)> &prepare = {},
                 const std::function<void(SDL_GPUCommandBuffer *, SDL_GPURenderPass *)> &overlay = {});
    void set_viewport(std::optional<Rect> area);
    // How many independently composed views there are. The number is fixed, and so is what each
    // slot means: slot k is the same view for the life of the process. That is the whole point
    // of the limit - a sampled target, and the UI binding that samples it, can then survive a
    // mode change instead of being quietly handed to a different view with different contents.
    static constexpr int max_views = 3;
    // The on-screen rectangle each view maps its canvas onto, in window coordinates, in slot
    // order. A slot whose rectangle has a non-positive width or height is **not shown this
    // frame**: its target is released and its texture id is zero, while the slot keeps its
    // identity for when it comes back. Passing fewer rectangles than `max_views` leaves the
    // remaining slots unshown. Passing none leaves every slot unshown, in which case slot 0
    // falls back to the whole viewport - which is what a caller that never sets views gets, and
    // is exactly how this behaved before slots were fixed.
    void set_views(std::span<const Rect> views);
    // The canvas extent a view maps onto its rectangle. Every view starts at 1280x720, the fixed
    // virtual resolution the game composes in, and the Game view must stay there: how much world
    // a client can see is the game's decision and never a panel's. An editor view may take
    // its panel's own shape instead, so a tall panel shows more room rather than letterboxing a
    // 16:9 slice into it and wasting the rest. Sprite coordinates stay canvas coordinates either
    // way; only how many of them fit changes.
    void set_view_canvas(int view, Vec2 canvas);
    Vec2 view_canvas(int view) const;
    bool view_active(int view) const;
    // Which view the pointer transforms use: `canvas_view()`, `screen_to_canvas` and
    // `canvas_to_screen` without an explicit view. Separate from `set_view`, which chooses where
    // *drawing* goes, because the two genuinely differ - the HUD is drawn into the Game view in
    // the same frame that picking has to run against the authoring view.
    void set_pointer_view(int view);
    int pointer_view() const;
    // Which view subsequent add/rect/text calls belong to. Set once around a pass rather than
    // tagged per call, because a caller that forgets one sprite produces a stray in the wrong
    // view and that is very hard to see.
    void set_view(int view);
    // Configure sampled editor targets at the start of a frame, before queuing sprites or UI.
    // Slots have stable binding addresses; active dimensions are quantized and capped at 2048.
    // An empty/hidden view returns 0. present renders all targets before the overlay samples them.
    void prepare_view_targets(bool enabled);
    uint64_t view_texture_id(int view) const;
    size_t view_target_bytes() const;
    size_t view_target_allocations() const;
    int view() const;
    CanvasView canvas_view() const;
    CanvasView canvas_view(int view) const;
    Vec2 canvas_to_screen(Vec2 position) const;
    Vec2 screen_to_canvas(Vec2 position) const;
    // Which shown view contains a window point, or -1 when none does. Pointer transforms need
    // this before they can ask a view to unproject: clicking in the Game view must not move an
    // object using the Scene view's camera. Negative rather than 0 for "nowhere", because a
    // point outside every view and a point inside the first one are different answers.
    int view_at(Vec2 screen) const;
    size_t draws() const;
    size_t sprites() const;
    // How long the last `present` spent in the driver's hands: waiting for a swapchain image, and
    // inside the submit that queues the frame for display, where a vsync'd driver blocks. That is
    // the display's pace, not work; the rest of `present` - sorting, uploading and recording - is
    // the frame's submission.
    double display_wait_ms() const;
    size_t texture_bytes() const;
    // Persistent upload buffers for textures that can be rewritten; separate from sampled pixels.
    size_t texture_staging_bytes() const;
    void capture(const fs::path &path);
    void tone(float frequency = 440, float volume = 0.12f);
    // The mixer behind `tone`, for a caller that needs a bus, a priority or a position. `tone`
    // stays as the one-line way to make a noise; this is the same voices underneath it.
    Mixer &mixer();
    // Pumps the mixer into the audio device. Called once a frame from the thread that owns the
    // renderer - the mix itself allocates nothing, but queuing it touches SDL, so it happens here
    // rather than inside a driver callback.
    void pump_audio();
};
} // namespace med
