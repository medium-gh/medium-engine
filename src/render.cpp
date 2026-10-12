#include "medium/render.hpp"
#include <chrono>
#include <cstring>
#include <numbers>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_truetype.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
namespace med {
namespace {
template <class T, auto Release> struct GPUDelete {
    SDL_GPUDevice *device{};
    void operator()(T *p) const {
        if (p)
            Release(device, p);
    }
};
using TexturePtr = std::unique_ptr<SDL_GPUTexture, GPUDelete<SDL_GPUTexture, SDL_ReleaseGPUTexture>>;
using BufferPtr = std::unique_ptr<SDL_GPUBuffer, GPUDelete<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>>;
using TransferPtr =
    std::unique_ptr<SDL_GPUTransferBuffer, GPUDelete<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer>>;
using PipelinePtr = std::unique_ptr<SDL_GPUGraphicsPipeline,
                                    GPUDelete<SDL_GPUGraphicsPipeline, SDL_ReleaseGPUGraphicsPipeline>>;
using SamplerPtr = std::unique_ptr<SDL_GPUSampler, GPUDelete<SDL_GPUSampler, SDL_ReleaseGPUSampler>>;
struct Texture {
    TexturePtr gpu;
    size_t bytes{};
    // Kept per texture rather than built per draw, because its *address* is what an immediate
    // mode UI stores as a texture id: a temporary would dangle the moment the frame ended.
    SDL_GPUTextureSamplerBinding binding{};
    int width{}, height{};
    // A texture rewritten every frame (`create_texture`) keeps its staging buffer rather than making
    // one per update.
    TransferPtr staging{nullptr, {nullptr}};
    // Pixel art: drawn with crisp texels (`set_pixel_art`).
    bool crisp{};
    // Mip levels, itself included (`create_texture`): above 1, made on the GPU after every upload and
    // sampled through the renderer's mip sampler.
    int levels = 1;
};
struct Vertex {
    float x, y, u, v, r, g, b, a;
};
void require(bool ok, const char *operation) {
    if (!ok)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
} // namespace
struct Renderer::Impl {
    struct Platform {
        Platform() { require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "SDL_Init"); }
        ~Platform() { SDL_Quit(); }
    } platform;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{nullptr, SDL_DestroyWindow};
    std::unique_ptr<SDL_GPUDevice, decltype(&SDL_DestroyGPUDevice)> gpu{nullptr, SDL_DestroyGPUDevice};
    // Every image the game or the editor holds at once: composed characters alone are a page per 2048 x 2048
    // of their frames (five for a human with weapons), for the player and each NPC outfit. At
    // 64 the editor ran out opening one more actor sheet ("entity capacity exceeded").
    Pool<Texture, 512> textures;
    TexturePtr framebuffer;
    struct ViewTarget {
        TexturePtr gpu;
        SDL_GPUTextureSamplerBinding binding{};
        uint32_t width = 0, height = 0;
        bool active = false;
    };
    std::array<ViewTarget, 3> targets;
    bool sampled_views = false;
    size_t target_allocations = 0;
    fs::path capture_path;
    std::optional<Rect> viewport_area;
    // Fixed slots, not a list. A rectangle with a non-positive extent means the slot is not
    // shown this frame; the slot itself, and the sampled target and binding that belong to it,
    // keep their identity. A variable-length list made slot 0 mean the Scene in one mode and the
    // actor preview in another, which is how one target came to serve two different views.
    std::array<Rect, Renderer::max_views> views{};
    // Per view, because a view's rectangle and the extent it shows are different questions: the
    // Game view keeps the fixed virtual resolution whatever shape its panel is, while an
    // authoring view takes the panel's shape so none of it is wasted on bars.
    std::array<Vec2, Renderer::max_views> canvases{Vec2{1280, 720}, Vec2{1280, 720}, Vec2{1280, 720}};
    int current_view = 0, pointer_view = 0;
    static bool shown(const Rect &r) { return r.w >= 1 && r.h >= 1; }
    bool any_view_shown() const {
        for (const auto &v : views)
            if (shown(v))
                return true;
        return false;
    }
    uint32_t framebuffer_w{}, framebuffer_h{};
    Handle white{}, font{};
    // Glyphs: printable ASCII, Latin-1 and Latin Extended-A, packed into one atlas - the Latin
    // scripts of Western and Central Europe, and the pseudo-locale's accents. Anything else draws as
    // '?'. The font's own data is kept for its kerning.
    struct GlyphRange {
        char32_t first;
        int count;
        size_t offset;
    };
    static constexpr std::array<GlyphRange, 3> glyph_ranges{{{32, 95, 0}, {160, 96, 95}, {256, 128, 191}}};
    // Two faces (`TextFace`): the body face rasterised at 28 px, the display face - titles, shown larger -
    // at twice that so it stays crisp, each in its own atlas. Scale 1 is 28 px of line in either.
    struct Face {
        int atlas_w = 1024, atlas_h = 512;
        float pixels = 28; // the size the glyphs were rasterised at
        std::array<stbtt_packedchar, 319> glyphs{};
        std::vector<uint8_t> data;
        stbtt_fontinfo info{};
        float scale = 0; // font units to raster pixels
        Handle texture{};
        bool loaded = false;
        // Raster pixels to layout units: 1 for the body face, 28 / pixels for a finer one.
        float unit() const { return 28 / pixels; }
    };
    std::array<Face, 2> faces{};
    const Face &face(TextFace which) const {
        const auto &chosen = faces[static_cast<size_t>(which)];
        return chosen.loaded ? chosen : faces[0];
    }
    int glyph_index(char32_t c) const {
        for (const auto &range : glyph_ranges)
            if (c >= range.first && c < range.first + static_cast<char32_t>(range.count))
                return static_cast<int>(range.offset + (c - range.first));
        return -1;
    }
    BufferPtr vertices;
    // Six indices a sprite into four vertices each, written once: a quad's two triangles share
    // two corners, so a sprite uploads four vertices rather than six.
    BufferPtr indices;
    TransferPtr transfer;
    // `sampler` reads the one level every texture but a mipmapped one has; `mip_sampler` blends between the
    // levels of one that has more (`create_texture`), so minified art is filtered rather than aliased.
    SamplerPtr sampler, mip_sampler;
    PipelinePtr alpha, additive, multiply;
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)> audio{nullptr,
                                                                              SDL_DestroyAudioStream};
    Mixer mixer{48000};
    // One buffer, reused. Sized for a frame at 60 Hz with headroom, and allocated once here
    // rather than per pump, so the mix's own no-allocation guarantee is not undone by its caller.
    std::vector<float> audio_buffer = std::vector<float>(1600 * 2);
    std::vector<Sprite> queue;
    size_t draw_count{}, sprite_count{}, memory{};
    double display_wait_ms = 0; // the last present's time in the driver's hands: acquire and submit
    int pixel_w = 1280, pixel_h = 720;
    bool claimed = false;
    static constexpr uint32_t capacity = 16384;
    ~Impl() {
        if (gpu) {
            SDL_WaitForGPUIdle(gpu.get());
            textures.each([&](Handle, Texture &t) { t.gpu.reset(); });
            if (claimed)
                SDL_ReleaseWindowFromGPUDevice(gpu.get(), window.get());
        }
    }
    // The mip levels a w x h texture can take of those wanted: while every level halves both sides exactly
    // (so a level's texel is exactly 2^level pixels of the first, which is what packing to a grid relies on),
    // and only when the device can render into the format, which generating them needs.
    int mip_levels(int w, int h, int wanted) const {
        if (wanted <= 1 || !SDL_GPUTextureSupportsFormat(
                               gpu.get(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                               SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET))
            return 1;
        int levels = 1;
        while (levels < std::min(wanted, 16) && w % (1 << levels) == 0 && h % (1 << levels) == 0)
            ++levels;
        return levels;
    }
    Handle upload(const uint8_t *pixels, int w, int h, int wanted_levels = 1) {
        if (w < 1 || h < 1 || w > 8192 || h > 8192)
            throw std::runtime_error("Texture dimensions exceed limits");
        const int levels = mip_levels(w, h, wanted_levels);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        // Generating mips renders each level from the one before, so a mipmapped texture is a target too.
        info.usage = levels > 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                                : SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<uint32_t>(w);
        info.height = static_cast<uint32_t>(h);
        info.layer_count_or_depth = 1;
        info.num_levels = static_cast<uint32_t>(levels);
        TexturePtr texture{SDL_CreateGPUTexture(gpu.get(), &info), {gpu.get()}};
        require(bool(texture), "create texture");
        uint32_t bytes = static_cast<uint32_t>(w * h * 4);
        size_t resident = 0; // every level's pixels, for texture_bytes
        for (int level = 0; level < levels; ++level)
            resident += static_cast<size_t>(w >> level) * static_cast<size_t>(h >> level) * 4;
        SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
        TransferPtr staging{SDL_CreateGPUTransferBuffer(gpu.get(), &transfer_info), {gpu.get()}};
        require(bool(staging), "create staging buffer");
        auto *dst = SDL_MapGPUTransferBuffer(gpu.get(), staging.get(), false);
        require(dst != nullptr, "map staging");
        std::memcpy(dst, pixels, bytes);
        SDL_UnmapGPUTransferBuffer(gpu.get(), staging.get());
        auto *cmd = SDL_AcquireGPUCommandBuffer(gpu.get());
        require(cmd != nullptr, "acquire upload command");
        auto *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = staging.get();
        source.pixels_per_row = static_cast<uint32_t>(w);
        source.rows_per_layer = static_cast<uint32_t>(h);
        SDL_GPUTextureRegion region{};
        region.texture = texture.get();
        region.w = static_cast<uint32_t>(w);
        region.h = static_cast<uint32_t>(h);
        region.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &region, false);
        SDL_EndGPUCopyPass(copy);
        // Outside any pass, after the first level is written: each level blitted from the one before.
        if (levels > 1)
            SDL_GenerateMipmapsForGPUTexture(cmd, texture.get());
        require(SDL_SubmitGPUCommandBuffer(cmd), "submit upload");
        auto handle = textures.create(Texture{std::move(texture), resident, {}, w, h});
        // The binding has to be filled in after the move, because it points at the texture the
        // pool now owns rather than at the local that was moved from.
        auto *stored = textures.get(handle);
        stored->levels = levels;
        stored->binding = {stored->gpu.get(), levels > 1 ? mip_sampler.get() : sampler.get()};
        memory += resident;
        return handle;
    }
    void load_face(Face &f, std::vector<uint8_t> bytes) {
        f.data = std::move(bytes);
        if (!stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0)))
            throw std::runtime_error("The font will not load");
        f.scale = stbtt_ScaleForPixelHeight(&f.info, f.pixels);
        std::vector<uint8_t> bitmap(static_cast<size_t>(f.atlas_w) * static_cast<size_t>(f.atlas_h));
        stbtt_pack_context pack{};
        if (!stbtt_PackBegin(&pack, bitmap.data(), f.atlas_w, f.atlas_h, 0, 1, nullptr))
            throw std::runtime_error("Font atlas unavailable");
        std::array<stbtt_pack_range, glyph_ranges.size()> ranges{};
        for (size_t i = 0; i < glyph_ranges.size(); ++i) {
            ranges[i].font_size = f.pixels;
            ranges[i].first_unicode_codepoint_in_range = static_cast<int>(glyph_ranges[i].first);
            ranges[i].num_chars = glyph_ranges[i].count;
            ranges[i].chardata_for_range = f.glyphs.data() + glyph_ranges[i].offset;
        }
        const bool packed =
            stbtt_PackFontRanges(&pack, f.data.data(), 0, ranges.data(), static_cast<int>(ranges.size()));
        stbtt_PackEnd(&pack);
        if (!packed)
            throw std::runtime_error("Font atlas overflow");
        std::vector<uint8_t> rgba(bitmap.size() * 4, 255);
        for (size_t i = 0; i < bitmap.size(); ++i)
            rgba[i * 4 + 3] = bitmap[i];
        f.texture = upload(rgba.data(), f.atlas_w, f.atlas_h);
        f.loaded = true;
    }
    void init(const fs::path &assets, const std::string &title) {
        window.reset(
            SDL_CreateWindow(title.c_str(), 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
        require(bool(window), "create window");
        // The display scale is only knowable once there is a window on a display, so the
        // window is created at the canvas size and then resized. On a display at 100% this
        // resolves to the same 1280x720 and changes nothing; at 125% it asks for 1600x900,
        // which is the same physical size as a 1280x720 window on an unscaled display.
        //
        // The canvas stays 1280x720 either way. CanvasView letterboxes it into whatever the
        // window turns out to be, so nothing downstream has to know this happened.
        if (const float display_scale = SDL_GetWindowDisplayScale(window.get()); display_scale > 1.0f) {
            const auto wanted = scaled_window_size({1280, 720}, display_scale);
            SDL_SetWindowSize(window.get(), static_cast<int>(wanted.x), static_cast<int>(wanted.y));
            log(LogLevel::Info, "Display scale " + std::to_string(display_scale) + "; window " +
                                    std::to_string(static_cast<int>(wanted.x)) + "x" +
                                    std::to_string(static_cast<int>(wanted.y)));
        }
        gpu.reset(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan"));
        require(bool(gpu), "SDL_GPU Vulkan initialization; check driver support");
        require(SDL_ClaimWindowForGPUDevice(gpu.get(), window.get()), "claim window");
        claimed = true;
        require(SDL_SetGPUSwapchainParameters(gpu.get(), window.get(), SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                              SDL_GPU_PRESENTMODE_VSYNC),
                "configure swapchain");
        log(LogLevel::Info, std::string("SDL_GPU backend: ") + SDL_GetGPUDeviceDriver(gpu.get()));
        auto shader = [&](const char *file, SDL_GPUShaderStage stage) {
            auto bytes = read_bytes(assets / "shaders" / file);
            SDL_GPUShaderCreateInfo ci{};
            ci.code_size = bytes.size();
            ci.code = bytes.data();
            ci.entrypoint = "main";
            ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
            ci.stage = stage;
            ci.num_samplers = stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? 1 : 0;
            ci.num_uniform_buffers = stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? 1 : 0;
            auto *s = SDL_CreateGPUShader(gpu.get(), &ci);
            require(s != nullptr, "create shader");
            return std::unique_ptr<SDL_GPUShader, GPUDelete<SDL_GPUShader, SDL_ReleaseGPUShader>>(
                s, {gpu.get()});
        };
        auto vs = shader("sprite.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX);
        auto fs = shader("sprite.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT);
        SDL_GPUVertexBufferDescription desc{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        std::array<SDL_GPUVertexAttribute, 3> attrs{{{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
                                                     {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8},
                                                     {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16}}};
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GetGPUSwapchainTextureFormat(gpu.get(), window.get());
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs.get();
        pi.fragment_shader = fs.get();
        pi.vertex_input_state = {&desc, 1, attrs.data(), 3};
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pi.target_info.color_target_descriptions = &target;
        pi.target_info.num_color_targets = 1;
        alpha = PipelinePtr(SDL_CreateGPUGraphicsPipeline(gpu.get(), &pi), {gpu.get()});
        require(bool(alpha), "create alpha pipeline");
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        additive = PipelinePtr(SDL_CreateGPUGraphicsPipeline(gpu.get(), &pi), {gpu.get()});
        require(bool(additive), "create additive pipeline");
        // The destination times the source colour, and its alpha left as it was.
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        multiply = PipelinePtr(SDL_CreateGPUGraphicsPipeline(gpu.get(), &pi), {gpu.get()});
        require(bool(multiply), "create multiply pipeline");
        SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, capacity * 4 * sizeof(Vertex), 0};
        vertices = BufferPtr(SDL_CreateGPUBuffer(gpu.get(), &bi), {gpu.get()});
        require(bool(vertices), "create vertex buffer");
        SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bi.size, 0};
        transfer = TransferPtr(SDL_CreateGPUTransferBuffer(gpu.get(), &ti), {gpu.get()});
        require(bool(transfer), "create frame staging");
        {
            // Sixteen-bit indices reach the last of capacity * 4 vertices exactly (65,535).
            static_assert(capacity * 4 - 1 <= 0xffff, "sprite indices must fit sixteen bits");
            SDL_GPUBufferCreateInfo ii{SDL_GPU_BUFFERUSAGE_INDEX, capacity * 6 * sizeof(uint16_t), 0};
            indices = BufferPtr(SDL_CreateGPUBuffer(gpu.get(), &ii), {gpu.get()});
            require(bool(indices), "create index buffer");
            SDL_GPUTransferBufferCreateInfo it{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, ii.size, 0};
            TransferPtr staging{SDL_CreateGPUTransferBuffer(gpu.get(), &it), {gpu.get()}};
            require(bool(staging), "create index staging");
            auto *out = static_cast<uint16_t *>(SDL_MapGPUTransferBuffer(gpu.get(), staging.get(), false));
            require(out != nullptr, "map index staging");
            for (uint32_t quad = 0; quad < capacity; ++quad)
                for (const uint32_t corner : {0u, 1u, 2u, 0u, 2u, 3u})
                    *out++ = static_cast<uint16_t>(quad * 4 + corner);
            SDL_UnmapGPUTransferBuffer(gpu.get(), staging.get());
            auto *cmd = SDL_AcquireGPUCommandBuffer(gpu.get());
            require(cmd != nullptr, "acquire index upload");
            auto *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{staging.get(), 0};
            SDL_GPUBufferRegion dest{indices.get(), 0, ii.size};
            SDL_UploadToGPUBuffer(copy, &src, &dest, false);
            SDL_EndGPUCopyPass(copy);
            require(SDL_SubmitGPUCommandBuffer(cmd), "submit index upload");
        }
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = SamplerPtr(SDL_CreateGPUSampler(gpu.get(), &si), {gpu.get()});
        require(bool(sampler), "create sampler");
        // Trilinear: between the two levels nearest the minification. The LOD range must be opened
        // explicitly - zeroed, as `si` leaves it, it pins every lookup to the first level - and the
        // texture's own level count caps it (1000 is Vulkan's "no clamp").
        SDL_GPUSamplerCreateInfo mi = si;
        mi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        mi.min_lod = 0;
        mi.max_lod = 1000;
        mip_sampler = SamplerPtr(SDL_CreateGPUSampler(gpu.get(), &mi), {gpu.get()});
        require(bool(mip_sampler), "create mip sampler");
        std::array<uint8_t, 4> pixel{255, 255, 255, 255};
        white = upload(pixel.data(), 1, 1);
        load_face(faces[0], read_bytes(assets / "font.ttf"));
        font = faces[0].texture;
        // The display face is optional: a project without one sets its titles in the body face.
        if (std::error_code missing; fs::exists(assets / "font_display.ttf", missing)) {
            try {
                faces[1].atlas_w = 2048;
                faces[1].atlas_h = 1024;
                faces[1].pixels = 56;
                load_face(faces[1], read_bytes(assets / "font_display.ttf"));
            } catch (const std::exception &e) {
                faces[1] = {};
                log(LogLevel::Warning, std::string("Display face unavailable: ") + e.what());
            }
        }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            // Stereo now: the mixer pans, and a mono device would throw that away.
            SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
            audio.reset(
                SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr));
            if (audio)
                SDL_ResumeAudioStreamDevice(audio.get());
            else
                log(LogLevel::Warning, std::string("Audio unavailable: ") + SDL_GetError());
        } else
            log(LogLevel::Warning, std::string("Audio subsystem unavailable: ") + SDL_GetError());
    }
};
Renderer::Renderer(const fs::path &assets, const std::string &title) : impl_(std::make_unique<Impl>()) {
    impl_->init(assets, title);
}
Renderer::~Renderer() = default;
SDL_Window *Renderer::window() const { return impl_->window.get(); }
SDL_GPUDevice *Renderer::device() const { return impl_->gpu.get(); }
Handle Renderer::load_texture(const fs::path &path) {
    if (!fs::exists(path)) {
        log(LogLevel::Warning, "Texture missing: " + path.string());
        return impl_->white;
    }
    auto bytes = read_bytes(path);
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) || w < 1 ||
        h < 1 || w > 8192 || h > 8192)
        throw std::runtime_error("Invalid texture header/dimensions: " + path.string());
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4),
        stbi_image_free};
    if (!pixels) {
        throw std::runtime_error("Invalid PNG payload: " + path.string());
    }
    return impl_->upload(pixels.get(), w, h);
}
std::vector<uint8_t> Renderer::read_pixels(const fs::path &path, int &width, int &height) {
    auto bytes = read_bytes(path);
    int channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4),
        stbi_image_free};
    if (!pixels || width < 1 || height < 1 || width > 8192 || height > 8192)
        throw std::runtime_error("Not a readable image: " + path.string());
    return {pixels.get(), pixels.get() + static_cast<size_t>(width) * static_cast<size_t>(height) * 4};
}
TextureAtlas Renderer::load_atlas(std::span<const fs::path> paths) {
    if (paths.size() > 64)
        throw std::runtime_error("An interface atlas holds at most 64 images");
    constexpr int side = 2048, gutter = 2;
    std::vector<uint8_t> packed(static_cast<size_t>(side) * side * 4, 0);
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    Surface target(SDL_CreateSurfaceFrom(side, side, SDL_PIXELFORMAT_RGBA32, packed.data(), side * 4),
                   SDL_DestroySurface);
    require(bool(target), "create atlas surface");
    // Every image read and trimmed first, then placed tallest first on shelves: in list order a tall
    // image after a row of short ones wasted most of a shelf, and the skin outgrew the page.
    struct Source {
        size_t index;
        std::vector<uint8_t> pixels;
        int w = 0, h = 0, left = 0, top = 0, right = 0, bottom = 0, dw = 0, dh = 0;
    };
    std::vector<Source> sources;
    TextureAtlas atlas;
    atlas.regions.resize(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!fs::exists(paths[i]))
            continue;
        Source source{i, {}, 0, 0};
        source.pixels = read_pixels(paths[i], source.w, source.h);
        int left = source.w, top = source.h, right = 0, bottom = 0;
        for (int py = 0; py < source.h; ++py)
            for (int px = 0; px < source.w; ++px)
                if (source.pixels[static_cast<size_t>((py * source.w + px) * 4 + 3)] > 8) {
                    left = std::min(left, px);
                    top = std::min(top, py);
                    right = std::max(right, px + 1);
                    bottom = std::max(bottom, py + 1);
                }
        if (left >= right || top >= bottom)
            continue;
        const float factor = std::min(
            {1.0f, 768.0f / static_cast<float>(right - left), 576.0f / static_cast<float>(bottom - top)});
        source.left = left;
        source.top = top;
        source.right = right;
        source.bottom = bottom;
        source.dw = std::max(1, static_cast<int>(std::lround(static_cast<float>(right - left) * factor)));
        source.dh = std::max(1, static_cast<int>(std::lround(static_cast<float>(bottom - top) * factor)));
        sources.push_back(std::move(source));
    }
    std::stable_sort(sources.begin(), sources.end(),
                     [](const Source &a, const Source &b) { return a.dh > b.dh; });
    int x = gutter, y = gutter, row_height = 0;
    for (auto &source : sources) {
        if (x + source.dw + gutter > side) {
            x = gutter;
            y += row_height + gutter * 2;
            row_height = 0;
        }
        if (y + source.dh + gutter > side)
            throw std::runtime_error("Interface atlas exceeds 2048 pixels");
        Surface image(SDL_CreateSurfaceFrom(source.w, source.h, SDL_PIXELFORMAT_RGBA32, source.pixels.data(),
                                            source.w * 4),
                      SDL_DestroySurface);
        require(bool(image), "create source atlas surface");
        require(SDL_SetSurfaceBlendMode(image.get(), SDL_BLENDMODE_NONE), "copy atlas alpha");
        const SDL_Rect src{source.left, source.top, source.right - source.left, source.bottom - source.top},
            dest{x, y, source.dw, source.dh};
        require(SDL_BlitSurfaceScaled(image.get(), &src, target.get(), &dest, SDL_SCALEMODE_LINEAR),
                "pack interface atlas");
        atlas.regions[source.index] = {static_cast<float>(x) / side, static_cast<float>(y) / side,
                                       static_cast<float>(source.dw) / side,
                                       static_cast<float>(source.dh) / side};
        x += source.dw + gutter * 2;
        row_height = std::max(row_height, source.dh);
    }
    atlas.texture = impl_->upload(packed.data(), side, side);
    return atlas;
}
Handle Renderer::load_pixels(int width, int height, std::span<const uint8_t> rgba, int levels) {
    if (width < 1 || height < 1 || width > 8192 || height > 8192 || levels < 1 ||
        rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4)
        throw std::runtime_error("A texture image is 1..8192 pixels a side, RGBA8, with at least one level");
    return impl_->upload(rgba.data(), width, height, levels);
}
Handle Renderer::create_texture(int width, int height, int levels) {
    if (width < 1 || height < 1 || width > 2048 || height > 2048)
        throw std::runtime_error("A written texture is 1..2048 pixels a side");
    if (levels < 1)
        throw std::runtime_error("A texture has at least one level");
    std::vector<uint8_t> blank(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 255);
    const auto handle = impl_->upload(blank.data(), width, height, levels);
    auto *texture = impl_->textures.get(handle);
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                         static_cast<uint32_t>(blank.size()), 0};
    texture->staging = TransferPtr(SDL_CreateGPUTransferBuffer(impl_->gpu.get(), &info), {impl_->gpu.get()});
    require(bool(texture->staging), "create texture staging buffer");
    return handle;
}
void Renderer::update_texture(Handle handle, std::span<const uint8_t> rgba) {
    auto *texture = impl_->textures.get(handle);
    if (!texture || !texture->staging)
        throw std::runtime_error("Only a texture made by create_texture can be written");
    const auto bytes = static_cast<size_t>(texture->width) * static_cast<size_t>(texture->height) * 4;
    if (rgba.size() != bytes)
        throw std::runtime_error("A texture update is width * height * 4 bytes");
    // Cycled, so writing this frame's pixels never waits for the GPU to finish reading last frame's.
    auto *mapped = SDL_MapGPUTransferBuffer(impl_->gpu.get(), texture->staging.get(), true);
    require(mapped != nullptr, "map texture staging");
    std::memcpy(mapped, rgba.data(), bytes);
    SDL_UnmapGPUTransferBuffer(impl_->gpu.get(), texture->staging.get());
    auto *cmd = SDL_AcquireGPUCommandBuffer(impl_->gpu.get());
    require(cmd != nullptr, "acquire texture update command");
    auto *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = texture->staging.get();
    source.pixels_per_row = static_cast<uint32_t>(texture->width);
    source.rows_per_layer = static_cast<uint32_t>(texture->height);
    SDL_GPUTextureRegion region{};
    region.texture = texture->gpu.get();
    region.w = static_cast<uint32_t>(texture->width);
    region.h = static_cast<uint32_t>(texture->height);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, true);
    SDL_EndGPUCopyPass(copy);
    // The smaller levels made again from the new pixels, in the same command buffer, so no frame samples a
    // chain half old and half new. (A cycled upload may have moved the texture to fresh memory; the levels
    // made here are that memory's.)
    if (texture->levels > 1)
        SDL_GenerateMipmapsForGPUTexture(cmd, texture->gpu.get());
    require(SDL_SubmitGPUCommandBuffer(cmd), "submit texture update");
}
int Renderer::texture_levels(Handle handle) const {
    const auto *texture = impl_->textures.get(handle);
    return texture ? texture->levels : 0;
}
Handle Renderer::white_texture() const { return impl_->white; }
void Renderer::release(Handle h) {
    if (h == impl_->white || h == impl_->font || (impl_->faces[1].loaded && h == impl_->faces[1].texture))
        throw std::runtime_error("Cannot release built-in textures");
    if (std::any_of(impl_->queue.begin(), impl_->queue.end(),
                    [&](const Sprite &s) { return s.texture == h; }))
        throw std::runtime_error("Cannot release a queued texture");
    auto *t = impl_->textures.get(h);
    if (!t)
        throw std::runtime_error("Stale texture handle");
    impl_->memory -= t->bytes;
    impl_->textures.destroy(h);
}
void Renderer::add(Sprite s) {
    if (s.mask > SpriteMask::Ribbon)
        throw std::runtime_error("Unknown sprite coverage mask");
    if (!impl_->textures.get(s.texture))
        throw std::runtime_error("Stale texture handle in sprite");
    if (impl_->queue.size() >= Impl::capacity)
        throw std::runtime_error("Sprite budget exceeded");
    if (!finite({s.bounds.x, s.bounds.y}) || !finite({s.bounds.w, s.bounds.h}) || !std::isfinite(s.angle) ||
        !std::isfinite(s.depth) || !std::isfinite(s.blur) || s.blur < 0 || s.blur > 4 || s.bounds.w < 0 ||
        s.bounds.h < 0)
        throw std::runtime_error("Invalid sprite transform");
    // The rotated box's half extents, for culling; an unrotated sprite - nearly all of them - needs no
    // trigonometry at all.
    const float cos_a = s.angle == 0 ? 1.0f : std::abs(std::cos(s.angle));
    const float sin_a = s.angle == 0 ? 0.0f : std::abs(std::sin(s.angle));
    float extent_x = (cos_a * s.bounds.w + sin_a * s.bounds.h) / 2;
    float extent_y = (sin_a * s.bounds.w + cos_a * s.bounds.h) / 2;
    float center_x = s.bounds.x + s.bounds.w / 2, center_y = s.bounds.y + s.bounds.h / 2;
    const auto canvas = view_canvas(impl_->current_view);
    if (center_x + extent_x < 0 || center_y + extent_y < 0 || center_x - extent_x > canvas.x ||
        center_y - extent_y > canvas.y)
        return;
    // Stamped here rather than left to the caller, so a sprite can never end up in a view its
    // pass did not intend - a stray in the wrong view is very hard to see and easy to produce.
    s.view = impl_->current_view;
    impl_->queue.push_back(s);
}
void Renderer::rect(Rect b, Color c, int layer, float depth) {
    add({impl_->white, b, {0, 0, 1, 1}, c, layer, depth});
}
void Renderer::text(Vec2 p, const std::string &message, Color color, float scale, int layer, float wrap,
                    float fit, TextFace which) {
    // Shrunk to fit a width, when given one, then wrapped at a width.
    if (fit > 0) {
        const float width = text_width(message, scale, which);
        if (width > fit)
            scale *= fit / width;
    }
    if (wrap > 0) {
        std::string joined;
        for (const auto &line :
             wrap_text(message, wrap, [&](std::string_view s) { return text_width(s, scale, which); })) {
            if (!joined.empty())
                joined += '\n';
            joined += line;
        }
        text(p, joined, color, scale, layer, 0, 0, which);
        return;
    }
    auto &d = *impl_;
    const auto &f = d.face(which);
    // The glyphs' own pixels to the layout's: a finer raster is drawn at the same size.
    const float drawn = scale * f.unit();
    float x = 0, y = 0;
    char32_t previous = 0;
    for (size_t at = 0; at < message.size();) {
        const char32_t c = next_codepoint(message, at);
        if (c == '\n') {
            x = 0;
            y += 25 / f.unit();
            previous = 0;
            continue;
        }
        int index = d.glyph_index(c);
        const char32_t shown = index < 0 ? U'?' : c;
        if (index < 0)
            index = d.glyph_index(U'?');
        if (previous)
            x += static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(previous),
                                                                  static_cast<int>(shown))) *
                 f.scale;
        stbtt_aligned_quad q{};
        stbtt_GetPackedQuad(f.glyphs.data(), f.atlas_w, f.atlas_h, index, &x, &y, &q, 1);
        previous = shown;
        if (q.x1 <= q.x0 || q.y1 <= q.y0)
            continue; // a space
        add({f.texture,
             {p.x + q.x0 * drawn, p.y + q.y0 * drawn, (q.x1 - q.x0) * drawn, (q.y1 - q.y0) * drawn},
             {q.s0, q.t0, q.s1 - q.s0, q.t1 - q.t0},
             color,
             layer});
    }
}
float Renderer::text_width(std::string_view message, float scale, TextFace which) const {
    const auto &d = *impl_;
    const auto &f = d.face(which);
    float widest = 0, x = 0;
    char32_t previous = 0;
    for (size_t at = 0; at < message.size();) {
        const char32_t c = next_codepoint(message, at);
        if (c == '\n') {
            widest = std::max(widest, x);
            x = 0;
            previous = 0;
            continue;
        }
        const int found = d.glyph_index(c);
        const char32_t shown = found < 0 ? U'?' : c;
        if (previous)
            x += static_cast<float>(stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(previous),
                                                                  static_cast<int>(shown))) *
                 f.scale;
        x += f.glyphs[static_cast<size_t>(found < 0 ? d.glyph_index(U'?') : found)].xadvance;
        previous = shown;
    }
    return std::max(widest, x) * scale * f.unit();
}
bool Renderer::has_display_face() const { return impl_->faces[1].loaded; }
bool Renderer::has_glyph(char32_t codepoint) const { return impl_->glyph_index(codepoint) >= 0; }
void Renderer::set_viewport(std::optional<Rect> area) {
    if (area)
        CanvasView{*area}.fitted();
    impl_->viewport_area = area;
}
void Renderer::set_view(int view) { impl_->current_view = std::clamp(view, 0, max_views - 1); }
int Renderer::view() const { return impl_->current_view; }
void Renderer::set_pointer_view(int view) { impl_->pointer_view = std::clamp(view, 0, max_views - 1); }
int Renderer::pointer_view() const { return impl_->pointer_view; }
void Renderer::set_view_canvas(int view, Vec2 canvas) {
    if (view < 0 || view >= max_views)
        return;
    // Bounded rather than trusted: a zero or non-finite extent divides by zero in the vertex
    // transform, and an enormous one makes every sprite a sub-pixel speck.
    if (!finite(canvas) || canvas.x < 64 || canvas.y < 64 || canvas.x > 8192 || canvas.y > 8192)
        throw std::runtime_error("Invalid view canvas extent");
    impl_->canvases[static_cast<size_t>(view)] = canvas;
}
Vec2 Renderer::view_canvas(int view) const {
    return view >= 0 && view < max_views ? impl_->canvases[static_cast<size_t>(view)] : Vec2{1280, 720};
}
bool Renderer::view_active(int view) const {
    return view >= 0 && view < max_views && Impl::shown(impl_->views[static_cast<size_t>(view)]);
}
void Renderer::set_views(std::span<const Rect> views) {
    if (views.size() > static_cast<size_t>(max_views))
        throw std::runtime_error("At most three views are supported");
    impl_->views.fill({});
    for (size_t i = 0; i < views.size(); ++i) {
        // A slot the caller is not showing this frame is left zeroed rather than validated:
        // "not shown" is an ordinary state, and a caller should not have to invent a rectangle
        // for a view it is deliberately hiding.
        if (!Impl::shown(views[i]))
            continue;
        // Validated here rather than at draw time: a degenerate view would otherwise surface as
        // an inverted GPU viewport mid-frame, which is far harder to attribute.
        CanvasView{views[i]}.fitted();
        impl_->views[i] = views[i];
    }
}
void Renderer::prepare_view_targets(bool enabled) {
    auto &d = *impl_;
    if (!d.queue.empty())
        throw std::runtime_error("Prepare view targets before queuing sprites");
    d.sampled_views = enabled;
    const float density = SDL_GetWindowPixelDensity(window());
    const bool minimized = (SDL_GetWindowFlags(window()) & SDL_WINDOW_MINIMIZED) != 0;
    // With no slot shown, slot 0 stands for the whole viewport - the single-view case a caller
    // that never calls set_views gets, and the one the standalone runtime uses.
    const bool implicit = !d.any_view_shown();
    for (size_t i = 0; i < d.targets.size(); ++i) {
        auto &target = d.targets[i];
        const bool wanted = implicit ? i == 0 : Impl::shown(d.views[i]);
        target.active = enabled && wanted && !minimized;
        if (!target.active) {
            // SDL release defers destruction until GPU use completes. The binding is not
            // referenced by this frame's UI; its address remains stable for later reuse.
            target.gpu.reset();
            target.binding = {};
            target.width = target.height = 0;
            continue;
        }
        const auto fitted = canvas_view(static_cast<int>(i)).fitted();
        auto dimension = [density](float size) {
            return static_cast<uint32_t>(std::clamp(std::ceil(size * density / 64) * 64, 64.0f, 2048.0f));
        };
        const auto width = dimension(fitted.w), height = dimension(fitted.h);
        if (width == target.width && height == target.height)
            continue;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GetGPUSwapchainTextureFormat(device(), window());
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = info.num_levels = 1;
        TexturePtr candidate{SDL_CreateGPUTexture(device(), &info), {device()}};
        require(bool(candidate), "create sampled view target");
        target.gpu = std::move(candidate);
        target.width = width;
        target.height = height;
        target.binding = {target.gpu.get(), d.sampler.get()};
        ++d.target_allocations;
    }
}
uint64_t Renderer::view_texture_id(int view) const {
    if (view < 0 || static_cast<size_t>(view) >= impl_->targets.size())
        return 0;
    const auto &target = impl_->targets[static_cast<size_t>(view)];
    return target.active ? reinterpret_cast<uint64_t>(&target.binding) : 0;
}
size_t Renderer::view_target_bytes() const {
    size_t bytes = 0;
    for (const auto &target : impl_->targets)
        bytes += static_cast<size_t>(target.width) * target.height * 4;
    return bytes;
}
size_t Renderer::view_target_allocations() const { return impl_->target_allocations; }
CanvasView Renderer::canvas_view() const { return canvas_view(impl_->pointer_view); }
CanvasView Renderer::canvas_view(int view) const {
    if (view_active(view))
        return {impl_->views[static_cast<size_t>(view)], view_canvas(view)};
    // An unshown slot has no rectangle of its own. Slot 0 answers with the viewport, which is
    // the single-view case; any other slot would be answering about something not on screen, so
    // it answers with the viewport too rather than throwing at a caller that merely asked.
    int w = 0, h = 0;
    SDL_GetWindowSize(window(), &w, &h);
    return {impl_->viewport_area.value_or(
        Rect{0, 0, static_cast<float>(std::max(w, 1)), static_cast<float>(std::max(h, 1))})};
}
int Renderer::view_at(Vec2 screen) const {
    // Reverse order, so a view drawn over another claims the point - the same rule the draw
    // order already implies. Unshown slots cannot claim anything.
    for (size_t i = impl_->views.size(); i-- > 0;)
        // Through `canvas_view`, so this asks about the extent the view actually shows. Testing
        // the default canvas instead letterboxed a 16:9 slice into a panel that is not 16:9, and
        // a click in the part of the panel outside that slice belonged to no view at all.
        if (Impl::shown(impl_->views[i]) && canvas_view(static_cast<int>(i)).contains(screen))
            return static_cast<int>(i);
    // No slot shown at all is the single-view case, where slot 0 is the whole viewport.
    if (!impl_->any_view_shown() && canvas_view(0).contains(screen))
        return 0;
    return -1;
}
Vec2 Renderer::screen_to_canvas(Vec2 p) const { return canvas_view().to_canvas(p); }
Vec2 Renderer::canvas_to_screen(Vec2 p) const { return canvas_view().to_screen(p); }
uint64_t Renderer::texture_id(Handle handle) const {
    const auto *texture = impl_->textures.get(handle);
    return texture ? reinterpret_cast<uint64_t>(&texture->binding) : 0;
}
void Renderer::set_pixel_art(Handle handle, bool crisp) {
    if (auto *texture = impl_->textures.get(handle))
        texture->crisp = crisp;
}
Vec2 Renderer::texture_size(Handle handle) const {
    const auto *texture = impl_->textures.get(handle);
    return texture ? Vec2{static_cast<float>(texture->width), static_cast<float>(texture->height)} : Vec2{};
}
size_t Renderer::draws() const { return impl_->draw_count; }
size_t Renderer::sprites() const { return impl_->sprite_count; }
double Renderer::display_wait_ms() const { return impl_->display_wait_ms; }
size_t Renderer::texture_bytes() const {
    return impl_->memory + view_target_bytes() +
           static_cast<size_t>(impl_->framebuffer_w) * impl_->framebuffer_h * 4;
}
size_t Renderer::texture_staging_bytes() const {
    size_t bytes = 0;
    impl_->textures.each([&](Handle, const Texture &texture) {
        if (texture.staging)
            bytes += static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 4;
    });
    return bytes;
}
void Renderer::capture(const fs::path &path) { impl_->capture_path = path; }
Mixer &Renderer::mixer() { return impl_->mixer; }
void Renderer::tone(float f, float volume) {
    // A voice rather than a queued buffer. The behaviour a caller sees is the same - a short
    // decaying tone - but it now goes through the mixer, so it obeys the bus gains, counts against
    // the voice limit, and can be placed in the world by a caller that knows where it happened.
    Sound sound;
    sound.frequency = std::clamp(f, 1.0f, 20000.0f);
    sound.gain = std::clamp(volume, 0.0f, 0.5f);
    sound.seconds = 0.1f;
    sound.bus = Bus::Effects;
    try {
        impl_->mixer.play(sound);
    } catch (const std::exception &e) {
        // A refused sound is never worth interrupting a frame for.
        log(LogLevel::Warning, std::string("Sound refused: ") + e.what());
    }
}
void Renderer::pump_audio() {
    // Music is decoded here, on the game thread, ahead of the mix - never inside `render`.
    impl_->mixer.stream_music();
    if (!impl_->audio)
        return;
    // Keep roughly a fifth of a second queued: enough that a long frame does not starve the
    // device, short enough that a gain change is heard promptly rather than after the backlog.
    constexpr int queued_target = 48000 / 5 * static_cast<int>(sizeof(float)) * 2;
    if (SDL_GetAudioStreamQueued(impl_->audio.get()) >= queued_target)
        return;
    impl_->mixer.render(impl_->audio_buffer);
    if (!SDL_PutAudioStreamData(impl_->audio.get(), impl_->audio_buffer.data(),
                                static_cast<int>(impl_->audio_buffer.size() * sizeof(float))))
        log(LogLevel::Warning, SDL_GetError());
}
bool Renderer::present(const std::function<void(SDL_GPUCommandBuffer *)> &prepare,
                       const std::function<void(SDL_GPUCommandBuffer *, SDL_GPURenderPass *)> &overlay) {
    auto &d = *impl_;
    // View first: each view is a separate viewport, so interleaving them would mean resetting
    // the GPU viewport per sprite. Layer and depth order within a view is unchanged.
    std::stable_sort(d.queue.begin(), d.queue.end(), [](const Sprite &a, const Sprite &b) {
        if (a.view != b.view)
            return a.view < b.view;
        return a.layer != b.layer ? a.layer < b.layer : a.depth < b.depth;
    });
    d.sprite_count = d.queue.size();
    d.draw_count = 0;
    auto *cmd = SDL_AcquireGPUCommandBuffer(device());
    require(cmd != nullptr, "acquire frame");
    SDL_GPUTexture *swap = nullptr;
    uint32_t w = 0, h = 0;
    // Timed on their own, here and at the submit below: these are where a frame waits on the
    // display - for a free swapchain image, and inside the submit that queues it for presentation,
    // which is where a vsync'd driver blocks - and that is idleness rather than cost. A caller
    // budgeting the rest of `present` as submission must be able to take them out.
    auto waiting = std::chrono::steady_clock::now();
    const bool acquired = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window(), &swap, &w, &h);
    d.display_wait_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count();
    if (!acquired) {
        SDL_CancelGPUCommandBuffer(cmd);
        throw std::runtime_error(SDL_GetError());
    }
    if (!swap) {
        SDL_CancelGPUCommandBuffer(cmd);
        d.queue.clear();
        return false;
    }
    auto *vertices = static_cast<Vertex *>(SDL_MapGPUTransferBuffer(device(), d.transfer.get(), true));
    if (!vertices) {
        SDL_CancelGPUCommandBuffer(cmd);
        throw std::runtime_error(SDL_GetError());
    }
    size_t count = 0;
    for (auto &s : d.queue) {
        // The sprite's own view's canvas, not a global one: two views on screen at once can be
        // showing different extents, and a sprite belongs to exactly one of them.
        const auto canvas = view_canvas(s.view);
        constexpr std::array<Vec2, 4> corners{{{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}}};
        // Once a sprite, and not at all when it is not rotated (R1: this was two sines and two
        // cosines a vertex, six vertices a sprite).
        const float cos_a = s.angle == 0 ? 1.0f : std::cos(s.angle);
        const float sin_a = s.angle == 0 ? 0.0f : std::sin(s.angle);
        const float centre_x = s.bounds.x + s.bounds.w / 2, centre_y = s.bounds.y + s.bounds.h / 2;
        for (const auto &q : corners) {
            const float x = q.x * s.bounds.w, y = q.y * s.bounds.h;
            const float rx = x * cos_a - y * sin_a + centre_x;
            const float ry = x * sin_a + y * cos_a + centre_y;
            vertices[count++] = {rx * 2 / canvas.x - 1,
                                 1 - ry * 2 / canvas.y,
                                 s.uv.x + (q.x + 0.5f) * s.uv.w,
                                 s.uv.y + (q.y + 0.5f) * s.uv.h,
                                 s.color.r,
                                 s.color.g,
                                 s.color.b,
                                 s.color.a};
        }
    }
    SDL_UnmapGPUTransferBuffer(device(), d.transfer.get());
    if (count) {
        auto *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{d.transfer.get(), 0};
        SDL_GPUBufferRegion dest{d.vertices.get(), 0, static_cast<uint32_t>(count * sizeof(Vertex))};
        SDL_UploadToGPUBuffer(copy, &src, &dest, true);
        SDL_EndGPUCopyPass(copy);
    }
    if (prepare)
        prepare(cmd);
    SDL_GPUColorTargetInfo target{};
    if (!d.framebuffer || d.framebuffer_w != w || d.framebuffer_h != h) {
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GetGPUSwapchainTextureFormat(device(), window());
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = w;
        ci.height = h;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        d.framebuffer = TexturePtr(SDL_CreateGPUTexture(device(), &ci), {device()});
        require(bool(d.framebuffer), "create offscreen framebuffer");
        d.framebuffer_w = w;
        d.framebuffer_h = h;
    }
    target.texture = d.framebuffer.get();
    target.clear_color = {0.055f, 0.055f, 0.055f, 1};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    int logical_w = 0, logical_h = 0;
    SDL_GetWindowSize(window(), &logical_w, &logical_h);
    const float ratio_x = static_cast<float>(w) / static_cast<float>(std::max(logical_w, 1)),
                ratio_y = static_cast<float>(h) / static_cast<float>(std::max(logical_h, 1));
    auto draw_view = [&](SDL_GPURenderPass *pass, int view, Rect fitted) {
        SDL_GPUViewport viewport{fitted.x, fitted.y, fitted.w, fitted.h, 0, 1};
        SDL_SetGPUViewport(pass, &viewport);
        SDL_GPUBufferBinding binding{d.vertices.get(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_GPUBufferBinding index_binding{d.indices.get(), 0};
        SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
        const auto canvas = view_canvas(view);
        const float scale_x = fitted.w / canvas.x, scale_y = fitted.h / canvas.y;
        size_t i = 0;
        while (i < d.queue.size()) {
            auto &s = d.queue[i];
            if (s.view != view) {
                ++i;
                continue;
            }
            size_t end = i + 1;
            while (end < d.queue.size()) {
                const auto &n = d.queue[end];
                if (n.view != s.view || n.texture != s.texture || n.additive != s.additive ||
                    n.multiply != s.multiply || n.clip != s.clip || n.blur != s.blur || n.mask != s.mask)
                    break;
                ++end;
            }
            SDL_BindGPUGraphicsPipeline(pass, s.multiply   ? d.multiply.get()
                                              : s.additive ? d.additive.get()
                                                           : d.alpha.get());
            const auto *texture = d.textures.get(s.texture);
            SDL_GPUTextureSamplerBinding tex{texture->gpu.get(),
                                             texture->levels > 1 ? d.mip_sampler.get() : d.sampler.get()};
            SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
            const std::array<float, 4> params{
                texture->crisp ? 1.0f : 0.0f, std::clamp(s.blur, 0.0f, 4.0f) * scale_x,
                std::clamp(s.blur, 0.0f, 4.0f) * scale_y, static_cast<float>(s.mask)};
            SDL_PushGPUFragmentUniformData(cmd, 0, params.data(), sizeof(params));
            const Rect region = s.clip.value_or(Rect{0, 0, canvas.x, canvas.y});
            SDL_Rect clip{static_cast<int>(fitted.x + region.x * scale_x),
                          static_cast<int>(fitted.y + region.y * scale_y),
                          static_cast<int>(region.w * scale_x), static_cast<int>(region.h * scale_y)};
            SDL_SetGPUScissor(pass, &clip);
            SDL_DrawGPUIndexedPrimitives(pass, static_cast<uint32_t>((end - i) * 6), 1,
                                         static_cast<uint32_t>(i * 6), 0, 0);
            ++d.draw_count;
            i = end;
        }
    };
    if (d.sampled_views) {
        for (size_t i = 0; i < d.targets.size(); ++i) {
            const auto &view = d.targets[i];
            if (!view.active)
                continue;
            SDL_GPUColorTargetInfo destination{};
            destination.texture = view.gpu.get();
            destination.clear_color = {0.078f, 0.078f, 0.078f, 1}; // the studio's stage (#141414)
            destination.load_op = SDL_GPU_LOADOP_CLEAR;
            destination.store_op = SDL_GPU_STOREOP_STORE;
            auto *view_pass = SDL_BeginGPURenderPass(cmd, &destination, 1, nullptr);
            draw_view(view_pass, static_cast<int>(i),
                      {0, 0, static_cast<float>(view.width), static_cast<float>(view.height)});
            SDL_EndGPURenderPass(view_pass);
        }
    }
    auto *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (!d.sampled_views) {
        const bool implicit = !d.any_view_shown();
        for (size_t i = 0; i < d.views.size(); ++i) {
            if (implicit ? i != 0 : !Impl::shown(d.views[i]))
                continue;
            const auto fitted = canvas_view(static_cast<int>(i)).fitted();
            draw_view(pass, static_cast<int>(i),
                      {fitted.x * ratio_x, fitted.y * ratio_y, fitted.w * ratio_x, fitted.h * ratio_y});
        }
    }
    if (overlay)
        overlay(cmd, pass);
    SDL_EndGPURenderPass(pass);
    d.queue.clear();
    SDL_GPUBlitInfo blit{};
    blit.source = {d.framebuffer.get(), 0, 0, 0, 0, w, h};
    blit.destination = {swap, 0, 0, 0, 0, w, h};
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);
    if (!d.capture_path.empty()) {
        uint32_t pitch = (w + 63U) & ~63U;
        SDL_GPUTransferBufferCreateInfo ci{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, pitch * h * 4, 0};
        TransferPtr download{SDL_CreateGPUTransferBuffer(device(), &ci), {device()}};
        require(bool(download), "create capture transfer");
        auto *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = d.framebuffer.get();
        src.w = w;
        src.h = h;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = download.get();
        dst.pixels_per_row = pitch;
        dst.rows_per_layer = h;
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        waiting = std::chrono::steady_clock::now();
        auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        require(fence != nullptr, "submit capture");
        bool waited = SDL_WaitForGPUFences(device(), true, &fence, 1);
        SDL_ReleaseGPUFence(device(), fence);
        require(waited, "wait for capture");
        d.display_wait_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count();
        auto *raw = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device(), download.get(), false));
        require(raw != nullptr, "map capture");
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        bool bgra =
            SDL_GetGPUSwapchainTextureFormat(device(), window()) == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                size_t source = (static_cast<size_t>(y) * pitch + x) * 4,
                       dest = (static_cast<size_t>(y) * w + x) * 4;
                rgba[dest] = raw[source + (bgra ? 2 : 0)];
                rgba[dest + 1] = raw[source + 1];
                rgba[dest + 2] = raw[source + (bgra ? 0 : 2)];
                rgba[dest + 3] = 255;
            }
        SDL_UnmapGPUTransferBuffer(device(), download.get());
        if (!d.capture_path.parent_path().empty())
            fs::create_directories(d.capture_path.parent_path());
        if (!stbi_write_png(d.capture_path.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                            rgba.data(), static_cast<int>(w * 4)))
            throw std::runtime_error("Cannot write capture");
        d.capture_path.clear();
    } else {
        waiting = std::chrono::steady_clock::now();
        require(SDL_SubmitGPUCommandBuffer(cmd), "submit frame");
        d.display_wait_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count();
    }
    return true;
}
} // namespace med
