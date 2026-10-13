// Native GS renderer (work in progress): translates GS draw state into Metal draw calls, so that triangles
// go through the GPU's hardware rasterizer instead of being rasterized by compute shaders. Same module ABI
// as the paraLLEl-GS bridge (runtime/gs/gs_parallel_api.h), so the runtime and gs-replay load either one.
//
// Blending, write masks and the alpha test are computed in the fragment shader from the destination colour
// (framebuffer fetch), which is what lets the GS equations be reproduced without approximating them with the
// fixed blend factors of the API.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define XXH_INLINE_ALL
#include "third_party/xxhash.h"
#include "texture_replace.h"
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include "gs_decode.h"
#include "runtime/gs/gs_parallel_api.h"

#include <atomic>
#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Sits above the window's own content and lets every event through to it.
@interface BlackNativeGSView : NSView
@end
@implementation BlackNativeGSView
- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}
@end

struct PassTime { double seconds = 0.0; uint64_t passes = 0, draws = 0; };
static std::mutex g_passTimeMutex;
static std::map<uint64_t, PassTime> g_passTime;
static std::atomic<uint64_t> g_gpuNanoseconds{0};
// When frames really reached the screen (the drawable's presented time): how many came more than a vertical blank and
// a half after the one before, and the longest gap, since the last report.
static std::atomic<uint64_t> g_lastPresented{0}, g_presentLate{0}, g_presentWorst{0}, g_presented{0};

namespace
{
    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kPages = kVramBytes / 8192u;
    constexpr uint32_t kTargetHeight = 512u;
    constexpr uint32_t kChunkVertices = 512u * 1024u;

    struct GpuVertex
    {
        float x, y, z;
        uint32_t rgba;
        float s, t, q;
        float fog;
    };

    // Mirrors `Uniforms` in the shader source below: 4-byte fields only.
    struct Uniforms
    {
        float targetWidth, targetHeight;
        uint32_t tme, tfx, tcc, fge;
        uint32_t abe, blendA, blendB, blendC, blendD, fix;
        uint32_t atst, aref, alphaMode; // alphaMode: 0 no test, 1 drop what fails, 2 drop what passes
        uint32_t writeMode;             // 0 colour and alpha, 1 nothing (depth only), 2 colour without alpha
        uint32_t fbmask, fba, colclamp, destinationHasAlpha;
        float fogR, fogG, fogB;
        float texScaleU, texScaleV; // normalized texture coordinates to those of the bound texture (a render target is larger)
        uint32_t texAlphaFixed, ta0; // 24-bit read of a 32-bit target: alpha comes from TEXA
        uint32_t texIndexed, texLinear; // the alpha of a render target read as an 8-bit index into the palette texture
        float texWidth, texHeight;     // the size TEX0 gives, in texels
        uint32_t wrapU, wrapV;          // 2 region clamp, 3 region repeat
        float minU, maxU, minV, maxV;
        uint32_t maxLevel, lodFixed;    // mip levels below the base; LCM
        float lodScale, lodK;           // 1 << L and K
        float pointSize;
        uint32_t paletteScale; // texels of the palette texture per palette entry (a palette copied from an upscaled target)
        uint32_t texBox;       // texels of the bound texture per texel the game sees, when it filters a larger render target
        uint32_t texSelf;      // the texel is the very pixel being drawn (the batch re-reads its own target in place)
        uint32_t texReplaced;  // the bound texture is a replacement pack texture: full-range alpha, hardware mip level
    };

    struct ShuffleUniforms
    {
        uint32_t sourceHalf, destinationHalf, mask16, sourceIsDepth;
    };

    const char *const kShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float targetWidth, targetHeight;
    uint tme, tfx, tcc, fge;
    uint abe, blendA, blendB, blendC, blendD, fix;
    uint atst, aref, alphaMode;
    uint writeMode;
    uint fbmask, fba, colclamp, destinationHasAlpha;
    float fogR, fogG, fogB;
    float texScaleU, texScaleV;
    uint texAlphaFixed, ta0;
    uint texIndexed, texLinear;
    float texWidth, texHeight;
    uint wrapU, wrapV;
    float minU, maxU, minV, maxV;
    uint maxLevel, lodFixed;
    float lodScale, lodK;
    float pointSize;
    uint paletteScale;
    uint texBox;
    uint texSelf;
    uint texReplaced;
};

// GS_SPECIALIZED: the fields that pick a code path come from function constants, one compiled variant per
// combination in use (the generic build reads them from the uniforms and decides per pixel).
#if defined(GS_SPECIALIZED)
constant uint k_tme [[function_constant(0)]];
constant uint k_tfx [[function_constant(1)]];
constant uint k_tcc [[function_constant(2)]];
constant uint k_fge [[function_constant(3)]];
constant uint k_abe [[function_constant(4)]];
constant uint k_blendA [[function_constant(5)]];
constant uint k_blendB [[function_constant(6)]];
constant uint k_blendC [[function_constant(7)]];
constant uint k_blendD [[function_constant(8)]];
constant uint k_atst [[function_constant(9)]];
constant uint k_alphaMode [[function_constant(10)]];
constant uint k_writeMode [[function_constant(11)]];
constant uint k_fba [[function_constant(12)]];
constant uint k_colclamp [[function_constant(13)]];
constant uint k_destinationHasAlpha [[function_constant(14)]];
constant uint k_texAlphaFixed [[function_constant(15)]];
constant uint k_texIndexed [[function_constant(16)]];
constant uint k_texLinear [[function_constant(17)]];
constant uint k_wrapU [[function_constant(18)]];
constant uint k_wrapV [[function_constant(19)]];
constant uint k_texBox [[function_constant(20)]];
constant uint k_texSelf [[function_constant(21)]];
constant uint k_hasLevels [[function_constant(22)]];
constant uint k_lodFixed [[function_constant(23)]];
constant uint k_hasMask [[function_constant(24)]];
#define S(field) k_##field
#define HAS_LEVELS (k_hasLevels != 0)
#define LOD_FIXED (k_lodFixed != 0)
#define HAS_MASK (k_hasMask != 0)
#else
#define S(field) u.field
#define HAS_LEVELS (u.maxLevel != 0)
#define LOD_FIXED (u.lodFixed != 0)
#define HAS_MASK (u.fbmask != 0)
#endif

struct ShuffleUniforms
{
    uint sourceHalf, destinationHalf, mask16, sourceIsDepth;
};

struct VertexIn
{
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float3 stq [[attribute(2)]];
    float fog [[attribute(3)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 color [[center_no_perspective]];
    float3 stq [[center_no_perspective]];
    float fog [[center_no_perspective]];
    float pointSize [[point_size]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]], constant Uniforms &u [[buffer(1)]])
{
    VertexOut out;
    // The GS samples a pixel at its integer coordinate, the GPU at the centre: half a pixel of shift makes
    // both cover the same pixels.
    out.position = float4((in.position.x + 0.5) / u.targetWidth * 2.0 - 1.0, 1.0 - (in.position.y + 0.5) / u.targetHeight * 2.0, in.position.z, 1.0);
    out.color = in.color;
    out.stq = in.stq;
    out.fog = in.fog;
    out.pointSize = u.pointSize;
    return out;
}

// A render target's alpha channel used as an 8-bit texture: palette lookup per texel, filtered afterwards as the GS does.
static float4 indexedTexel(texture2d<float> tex, texture2d<float> palette, float2 at, float2 size, uint paletteScale)
{
    uint2 where = uint2(clamp(at, float2(0.0), size - 1.0));
    uint index = uint(round(tex.read(where).a * 255.0));
    // The palette texture keeps the layout it has in VRAM: 16x16, with the two middle index bits swapped.
    uint entry = (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
    return round(palette.read(uint2(entry & 15u, entry >> 4) * paletteScale + paletteScale / 2) * 255.0);
}

struct QuadOut
{
    float4 position [[position]];
};

vertex QuadOut quadVertex(uint id [[vertex_id]])
{
    QuadOut out;
    out.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
    return out;
}

// The target seen as 16-bit pixels: each 32-bit pixel is two of them (red+green, blue+alpha). A full-screen copy
// in that view moves one half of the source word into one half of the destination word, under a write mask.
fragment float4 shuffleFragment(QuadOut in [[stage_in]], float4 destination [[color(0)]], depth2d<float> depth [[texture(0)]], constant ShuffleUniforms &u [[buffer(1)]])
{
    uint4 d = uint4(round(destination * 255.0));
    uint word = d.r | (d.g << 8) | (d.b << 16) | (d.a << 24);
    uint source = word;
    if (u.sourceIsDepth != 0) source = uint(depth.read(uint2(in.position.xy)) * 16777216.0 + 0.5);
    uint from = (source >> (16 * u.sourceHalf)) & 0xFFFFu;
    uint shift = 16 * u.destinationHalf;
    uint old = (word >> shift) & 0xFFFFu;
    uint merged = (old & u.mask16) | (from & ~u.mask16 & 0xFFFFu);
    word = (word & ~(0xFFFFu << shift)) | (merged << shift);
    return float4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;
}

struct PresentUniforms
{
    float originX, originY, scaleX, scaleY;
};

// The displayed part of a target, stretched over the window's drawable.
fragment float4 presentFragment(QuadOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], constant PresentUniforms &u [[buffer(1)]])
{
    return float4(tex.sample(smp, float2(u.originX, u.originY) + in.position.xy * float2(u.scaleX, u.scaleY)).rgb, 1.0);
}

struct DepthOut
{
    float depth [[depth(any)]];
};

// A depth buffer drawn to as if it were colour, textured with another depth buffer: a depth-to-depth copy.
fragment DepthOut depthCopyFragment(VertexOut in [[stage_in]], depth2d<float> source [[texture(0)]], sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    DepthOut out;
    out.depth = source.sample(smp, in.stq.xy / in.stq.z * float2(u.texScaleU, u.texScaleV));
    return out;
}

struct DepthAndColorOut
{
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

// The same copy while ZBUF points at a colour target: the sprite's own Z lands there as pixel bytes (a clear in disguise).
fragment DepthAndColorOut depthCopyClearFragment(VertexOut in [[stage_in]], depth2d<float> source [[texture(0)]], sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    DepthAndColorOut out;
    out.depth = source.sample(smp, in.stq.xy / in.stq.z * float2(u.texScaleU, u.texScaleV));
    uint word = uint(in.position.z * 16777216.0 + 0.5);
    out.color = float4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;
    return out;
}

static float4 shade(VertexOut in, float4 destination, texture2d<float> tex, texture2d<float> palette, sampler smp, constant Uniforms &u)
{
#if defined(GS_TRIVIAL)
    return float4(in.color.rgb / 255.0, 1.0); // PS2X_GS_NATIVE_TRIVIAL=1: what rasterizing alone costs (diagnostic)
#endif
    // Everything below is in GS units: 0..255 per channel, alpha 128 meaning 1.0.
    float4 c = floor(in.color);
    if (S(tme) != 0)
    {
        float2 coordinate = in.stq.xy / in.stq.z;
        if (S(wrapU) >= 2 || S(wrapV) >= 2)
        {
            // Region modes work on texel coordinates: clamp to [MIN, MAX], or (texel & MIN) | MAX (unfiltered).
            float2 texel = coordinate * float2(u.texWidth, u.texHeight);
            if (S(wrapU) == 2) texel.x = clamp(texel.x, u.minU + 0.5, u.maxU + 0.5);
            else if (S(wrapU) == 3) texel.x = float((int(floor(texel.x)) & int(u.minU)) | int(u.maxU)) + 0.5;
            else if (S(wrapU) == 4) texel.x = fract(coordinate.x) * u.texWidth;
            if (S(wrapV) == 2) texel.y = clamp(texel.y, u.minV + 0.5, u.maxV + 0.5);
            else if (S(wrapV) == 3) texel.y = float((int(floor(texel.y)) & int(u.minV)) | int(u.maxV)) + 0.5;
            else if (S(wrapV) == 4) texel.y = fract(coordinate.y) * u.texHeight;
            coordinate = texel / float2(u.texWidth, u.texHeight);
        }
        coordinate *= float2(u.texScaleU, u.texScaleV);
        float4 t;
        if (S(texSelf) != 0)
        {
            // The game re-reads the frame in place (each pixel reads itself): that is the destination pixel, with no
            // copy of the target and no break in the pass.
            if (S(texIndexed) != 0)
            {
                uint index = uint(round(destination.a * 255.0));
                uint entry = (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
                t = round(palette.read(uint2(entry & 15u, entry >> 4) * u.paletteScale + u.paletteScale / 2) * 255.0);
            }
            else
                t = round(destination * 255.0);
        }
        else if (S(texIndexed) != 0)
        {
            float2 size = float2(tex.get_width(), tex.get_height());
            float2 at = coordinate * size;
            if (S(texLinear) != 0)
            {
                at -= 0.5;
                float2 base = floor(at), f = at - base;
                float4 top = mix(indexedTexel(tex, palette, base, size, u.paletteScale), indexedTexel(tex, palette, base + float2(1.0, 0.0), size, u.paletteScale), f.x);
                float4 bottom = mix(indexedTexel(tex, palette, base + float2(0.0, 1.0), size, u.paletteScale), indexedTexel(tex, palette, base + float2(1.0, 1.0), size, u.paletteScale), f.x);
                t = floor(mix(top, bottom, f.y));
            }
            else
                t = indexedTexel(tex, palette, floor(at), size, u.paletteScale);
        }
        else
        {
            // The GS picks the mip level from Q, not from screen-space derivatives: (log2(1/Q) << L) + K, rounded.
            float lod = 0.0;
            if (HAS_LEVELS)
            {
                lod = LOD_FIXED ? u.lodK : log2(1.0 / abs(in.stq.z)) * u.lodScale + u.lodK;
                lod = clamp(floor(lod + 0.5), 0.0, float(u.maxLevel));
            }
            if (u.texReplaced != 0)
            {
                // A replacement from a texture pack: its own alpha is 0..255 (opaque = 255), the GS works in 0..128.
                t = round(tex.sample(smp, coordinate) * 255.0);
                t.a = floor(t.a * 128.0 / 255.0 + 0.5);
            }
            else if (S(texBox) > 1)
            {
                // A render target drawn larger, read with the GS's bilinear filter: the game counts on that filter
                // mixing neighbouring texels of its own size (it blurs by reading half a texel off). One of its
                // texels is texBox of ours across, so the same mix is the mean over that many of our samples.
                // Four filtered samples, a quarter of the game's texel away from the centre on each axis: together they
                // weigh the texBox x texBox texels of ours about evenly, at any scale (texBox squared samples would be
                // nine at 3x and sixteen at 4x, on passes that cover the whole screen).
                float2 reach = float(S(texBox)) * 0.25 / float2(tex.get_width(), tex.get_height());
                float4 sum = tex.sample(smp, coordinate + float2(-reach.x, -reach.y), level(0.0)) + tex.sample(smp, coordinate + float2(reach.x, -reach.y), level(0.0)) +
                             tex.sample(smp, coordinate + float2(-reach.x, reach.y), level(0.0)) + tex.sample(smp, coordinate + float2(reach.x, reach.y), level(0.0));
                t = round(sum * 0.25 * 255.0);
            }
            else
                t = round(tex.sample(smp, coordinate, level(lod)) * 255.0);
        }
        if (S(texAlphaFixed) != 0) t.a = float(u.ta0);
        float3 modulated = min(floor(t.rgb * c.rgb / 128.0), 255.0);
        if (S(tfx) == 0)
        {
            c.rgb = modulated;
            if (S(tcc) != 0) c.a = min(floor(t.a * c.a / 128.0), 255.0);
        }
        else if (S(tfx) == 1)
        {
            c.rgb = t.rgb;
            if (S(tcc) != 0) c.a = t.a;
        }
        else
        {
            c.rgb = min(modulated + c.a, 255.0);
            if (S(tcc) != 0) c.a = S(tfx) == 2 ? min(t.a + c.a, 255.0) : t.a;
        }
    }
    if (S(fge) != 0)
    {
        float f = floor(in.fog);
        c.rgb = floor((c.rgb * f + float3(u.fogR, u.fogG, u.fogB) * (256.0 - f)) / 256.0);
    }
    if (S(alphaMode) != 0)
    {
        float a = c.a, r = float(u.aref);
        bool pass = S(atst) == 0 ? false : S(atst) == 1 ? true : S(atst) == 2 ? a < r : S(atst) == 3 ? a <= r : S(atst) == 4 ? a == r : S(atst) == 5 ? a >= r : S(atst) == 6 ? a > r : a != r;
        if (pass != (S(alphaMode) == 1)) discard_fragment();
    }
    float4 d = round(destination * 255.0);
    if (S(writeMode) == 1) return destination;
    float3 rgb = c.rgb;
    if (S(abe) != 0)
    {
        float3 A = S(blendA) == 0 ? c.rgb : S(blendA) == 1 ? d.rgb : float3(0.0);
        float3 B = S(blendB) == 0 ? c.rgb : S(blendB) == 1 ? d.rgb : float3(0.0);
        float C = S(blendC) == 0 ? c.a : S(blendC) == 1 ? (S(destinationHasAlpha) != 0 ? d.a : 128.0) : float(u.fix);
        float3 D = S(blendD) == 0 ? c.rgb : S(blendD) == 1 ? d.rgb : float3(0.0);
        rgb = floor((A - B) * C / 128.0) + D;
    }
    rgb = S(colclamp) != 0 ? clamp(rgb, 0.0, 255.0) : rgb - 256.0 * floor(rgb / 256.0);
    float alpha = S(fba) != 0 && c.a < 128.0 ? c.a + 128.0 : c.a;
    if (S(writeMode) == 2) alpha = d.a;
    uint4 o = uint4(float4(rgb, clamp(alpha, 0.0, 255.0)));
    if (HAS_MASK)
    {
        uint packed = o.r | (o.g << 8) | (o.b << 16) | (o.a << 24);
        uint4 di = uint4(d);
        uint old = di.r | (di.g << 8) | (di.b << 16) | (di.a << 24);
        packed = (packed & ~u.fbmask) | (old & u.fbmask);
        o = uint4(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255, packed >> 24);
    }
    return float4(o) / 255.0;
}

// Reads the pixel it writes to (programmable blending): for everything the GS mixes with or masks against the frame.
fragment float4 fragmentMain(VertexOut in [[stage_in]], float4 destination [[color(0)]], texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]],
                             sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    return shade(in, destination, tex, palette, smp, u);
}

// The same without that read, for draws that just replace the pixel (no blending, no write mask, not the
// second pass of an alpha test): the GPU can then drop hidden fragments instead of shading every layer in order.
fragment float4 fragmentOpaque(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]],
                               sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    return shade(in, float4(0.0), tex, palette, smp, u);
}
)MSL";

    bool isPaletted(uint32_t psm) { return psm == 0x13u || psm == 0x14u || psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu; }
    bool isDepthFormat(uint32_t psm) { return (psm & 0x30u) == 0x30u; }

    class Renderer final : public gsn::Sink
    {
    public:
        gsn::Frontend frontend;
        std::vector<uint8_t> vram;

        bool initialize(const uint8_t *initial)
        {
            device = MTLCreateSystemDefaultDevice();
            if (!device)
                return false;
            queue = [device newCommandQueue];
            NSError *error = nil;
            MTLCompileOptions *compileOptions = [MTLCompileOptions new];
            if (std::getenv("PS2X_GS_NATIVE_TRIVIAL"))
                compileOptions.preprocessorMacros = @{@"GS_TRIVIAL" : @1};
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:compileOptions error:&error];
            if (!library)
            {
                std::fprintf(stderr, "[native-gs] shader: %s\n", error.localizedDescription.UTF8String);
                return false;
            }
            if (!std::getenv("PS2X_GS_NATIVE_GENERIC_SHADER") && !std::getenv("PS2X_GS_NATIVE_TRIVIAL"))
            {
                MTLCompileOptions *specializedOptions = [MTLCompileOptions new];
                specializedOptions.preprocessorMacros = @{@"GS_SPECIALIZED" : @1};
                specializedLibrary = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:specializedOptions error:&error];
                if (!specializedLibrary)
                    std::fprintf(stderr, "[native-gs] specialized shader: %s\n", error.localizedDescription.UTF8String);
            }
            MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:@"vertexMain"];
            descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
            layout.attributes[0].format = MTLVertexFormatFloat3;
            layout.attributes[0].offset = offsetof(GpuVertex, x);
            layout.attributes[1].format = MTLVertexFormatUChar4;
            layout.attributes[1].offset = offsetof(GpuVertex, rgba);
            layout.attributes[2].format = MTLVertexFormatFloat3;
            layout.attributes[2].offset = offsetof(GpuVertex, s);
            layout.attributes[3].format = MTLVertexFormatFloat;
            layout.attributes[3].offset = offsetof(GpuVertex, fog);
            layout.layouts[0].stride = sizeof(GpuVertex);
            descriptor.vertexDescriptor = layout;
            baseDescriptor = [descriptor copy];
            pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentOpaque"];
            opaquePipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
            if (!pipeline)
            {
                std::fprintf(stderr, "[native-gs] pipeline: %s\n", error.localizedDescription.UTF8String);
                return false;
            }
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
            descriptor.fragmentFunction = [library newFunctionWithName:@"depthCopyFragment"];
            depthCopyPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            descriptor.fragmentFunction = [library newFunctionWithName:@"depthCopyClearFragment"];
            depthCopyClearPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            MTLRenderPipelineDescriptor *shuffle = [MTLRenderPipelineDescriptor new];
            shuffle.vertexFunction = [library newFunctionWithName:@"quadVertex"];
            shuffle.fragmentFunction = [library newFunctionWithName:@"shuffleFragment"];
            shuffle.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            shufflePipeline = [device newRenderPipelineStateWithDescriptor:shuffle error:&error];
            shuffle.fragmentFunction = [library newFunctionWithName:@"presentFragment"];
            shuffle.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            presentPipeline = [device newRenderPipelineStateWithDescriptor:shuffle error:&error];
            shuffle.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            resolvePipeline = [device newRenderPipelineStateWithDescriptor:shuffle error:&error];
            if (!depthCopyPipeline || !depthCopyClearPipeline || !shufflePipeline || !presentPipeline)
            {
                std::fprintf(stderr, "[native-gs] pipeline: %s\n", error.localizedDescription.UTF8String);
                return false;
            }
            MTLTextureDescriptor *white = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
            fallbackTexture = [device newTextureWithDescriptor:white];
            const uint32_t pixel = 0xFFFFFFFFu;
            [fallbackTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&pixel bytesPerRow:4];

            GSMem::InitLookupTables();
            vram.assign(initial, initial + kVramBytes);
            frontend.vram = vram.data();
            frontend.sink = this;
            stats = std::getenv("PS2X_GS_NATIVE_STATS") != nullptr;
            // PS2X_GS_NATIVE_SCALE=<1..8>: internal resolution, as a multiple of the game's own.
            if (const char *wanted = std::getenv("PS2X_GS_NATIVE_SCALE"))
                scale = uint32_t(std::clamp(std::atoi(wanted), 1, 8));
            if (const char *narrow = std::getenv("PS2X_GS_NATIVE_SCALE_NARROW"); narrow && narrow[0] == '0')
                scaleMinWidth = 8;
            std::fprintf(stderr, "[native-gs] Metal renderer on %s\n", device.name.UTF8String);
            return true;
        }

        // gsn::Sink
        void stateChanging() override { closeBatch(); }

        void triangle(const gsn::Vertex &a, const gsn::Vertex &b, const gsn::Vertex &c) override
        {
            if (!batchOpen)
                openBatch();
            if (logging() && batch.logged < 2u && (uint32_t(batch.ctx.frame >> 32) != 0u || (uint32_t(batch.ctx.tex0 >> 20) & 0x3Fu) == 0x1Bu))
            {
                ++batch.logged;
                for (const gsn::Vertex *v : {&a, &b, &c})
                    std::fprintf(stderr, "[tri] xy %.2f,%.2f st %.4f,%.4f q %.4f (uv %.2f,%.2f) z %x rgba %08x\n", v->x / 16.0 - batch.offsetX, v->y / 16.0 - batch.offsetY, v->s, v->t, v->q,
                                 v->s / v->q * float(1u << (uint32_t(batch.ctx.tex0 >> 26) & 15u)), v->t / v->q * float(1u << (uint32_t(batch.ctx.tex0 >> 30) & 15u)), v->z, v->rgba);
            }
            if (batch.skip)
            {
                ++skipped[batch.skipReason];
                return;
            }
            GpuVertex *out = reserve(3);
            const uint32_t flat = c.rgba;
            put(out[0], a, batch.gouraud ? a.rgba : flat, a.z);
            put(out[1], b, batch.gouraud ? b.rgba : flat, b.z);
            put(out[2], c, flat, c.z);
            ++drawn;
        }

        void sprite(const gsn::Vertex &a, const gsn::Vertex &b) override
        {
            if (!batchOpen)
                openBatch();
            if (batch.shuffle)
            {
                // Which half of the 32-bit word each side is: the 16-bit view lays the halves out 8 pixels apart.
                batch.shuffleDestination = (uint32_t(std::min(a.x, b.x) / 16 - int32_t(batch.offsetX)) & 8u) != 0u ? 1u : 0u;
                batch.shuffleSource = (uint32_t(std::min(a.u, b.u) / 16u) & 8u) != 0u ? 1u : 0u;
                ++batch.primitives;
                ++drawn;
                return;
            }
            if (logging() && batch.logged < 3u)
            {
                ++batch.logged;
                std::fprintf(stderr, "[sprite] xy %.2f,%.2f - %.2f,%.2f  uv %.2f,%.2f - %.2f,%.2f  st %.4f,%.4f - %.4f,%.4f q %.3f z %x rgba %08x\n", a.x / 16.0 - batch.offsetX, a.y / 16.0 - batch.offsetY,
                             b.x / 16.0 - batch.offsetX, b.y / 16.0 - batch.offsetY, a.u / 16.0, a.v / 16.0, b.u / 16.0, b.v / 16.0, a.s, a.t, b.s, b.t, b.q, b.z, b.rgba);
            }
            if (batch.skip)
            {
                ++skipped[batch.skipReason];
                return;
            }
            // Flat rectangle: colour, depth, fog and Q come from the second vertex.
            GpuVertex first, second;
            gsn::Vertex a1 = a;
            a1.q = b.q;
            a1.fog = b.fog;
            put(first, a1, b.rgba, b.z);
            put(second, b, b.rgba, b.z);
            if (!batch.fst)
            {
                first.s /= first.q;
                first.t /= first.q;
                second.s /= second.q;
                second.t /= second.q;
                first.q = second.q = 1.0f;
            }
            if (first.x > second.x)
            {
                std::swap(first.x, second.x);
                std::swap(first.s, second.s);
            }
            if (first.y > second.y)
            {
                std::swap(first.y, second.y);
                std::swap(first.t, second.t);
            }
            if (scaleOf(uint32_t(batch.ctx.frame >> 16) & 0x3Fu) > 1u && second.x > first.x && second.y > first.y)
            {
                // The GS samples pixels at integer coordinates, so a sprite from x0 to x1 fills the pixels x0..x1-1: the
                // area half a pixel up and to the left of its outline. At the game's resolution both readings pick the same
                // pixels; at a higher one only the area reading fills a screen edge to edge. The texture mapping stays put.
                const float ds = (second.s - first.s) / (second.x - first.x) * 0.5f, dt = (second.t - first.t) / (second.y - first.y) * 0.5f;
                first.x -= 0.5f; second.x -= 0.5f; first.y -= 0.5f; second.y -= 0.5f;
                first.s -= ds; second.s -= ds; first.t -= dt; second.t -= dt;
            }
            GpuVertex *out = reserve(6);
            GpuVertex topRight = first, bottomLeft = first;
            topRight.x = second.x;
            topRight.s = second.s;
            bottomLeft.y = second.y;
            bottomLeft.t = second.t;
            out[0] = first;
            out[1] = topRight;
            out[2] = bottomLeft;
            out[3] = bottomLeft;
            out[4] = topRight;
            out[5] = second;
            ++drawn;
        }

        // Lines and points go through the GPU's own line and point rasterization (one pixel wide, like the GS's).
        void line(const gsn::Vertex &a, const gsn::Vertex &b) override
        {
            if (!batchOpen)
                openBatch();
            if (batch.skip || batch.shuffle || batch.depthCopy)
            {
                ++skipped[SkipLinesPoints];
                return;
            }
            GpuVertex *out = reserve(2);
            put(out[0], a, batch.gouraud ? a.rgba : b.rgba, a.z);
            put(out[1], b, b.rgba, b.z);
            ++drawn;
        }

        void point(const gsn::Vertex &a) override
        {
            if (!batchOpen)
                openBatch();
            if (batch.skip || batch.shuffle || batch.depthCopy)
            {
                ++skipped[SkipLinesPoints];
                return;
            }
            put(*reserve(1), a, a.rgba, a.z);
            ++drawn;
        }

        void vramWritten(const std::vector<uint16_t> &blocks) override
        {
            if (logging())
                std::fprintf(stderr, "[upload] %zu blocks changed from %u\n", blocks.size(), unsigned(blocks.front()));
            ++vramGeneration;
            std::bitset<kPages> pages;
            uint32_t firstPage = kPages, lastPage = 0;
            for (const uint16_t block : blocks)
            {
                ++blockGeneration[block];
                for (TextureEntry *user : blockUsers[block])
                    user->valid = false;
                pages.set(block >> 5);
                firstPage = std::min<uint32_t>(firstPage, block >> 5);
                lastPage = std::max<uint32_t>(lastPage, block >> 5);
            }
            for (auto &entry : targets)
            {
                Target &target = entry.second;
                const uint32_t count = target.pageCount();
                if (firstPage >= target.fbp + count || lastPage < target.fbp)
                    continue;
                for (uint32_t page = std::max(firstPage, target.fbp) - target.fbp; page < count && target.fbp + page <= lastPage; ++page)
                    if (pages.test(target.fbp + page))
                        target.stale.set(page);
            }
            releaseBlocks(blocks, false);
        }

        bool uploadUnchanged(uint32_t dbp, bool atOrigin, uint64_t hash, uint64_t rectKey) override
        {
            const auto found = rectangles.find(rectKey);
            if (found == rectangles.end() || found->second.hash != hash || generationSum(found->second.blocks) != found->second.blockSum)
                return false;
            for (const uint16_t block : found->second.blocks)
                if (gpuDirtyBlocks.test(block))
                    return false;
            // The content record evolves exactly as if the transfer had been written.
            UploadRecord &record = uploads[dbp];
            record.hash = atOrigin ? hash : record.hash * 0x100000001b3ull ^ hash;
            ++uploadsSkipped;
            return true;
        }

        void uploadFinished(uint32_t dbp, bool atOrigin, uint64_t hash, uint64_t rectKey, std::vector<uint16_t> &blocks) override
        {
            {
                UploadRecord &rectangle = rectangles[rectKey];
                rectangle.hash = hash;
                rectangle.blocks = blocks;
                rectangle.blockSum = generationSum(blocks);
            }
            // Several rectangles sent to one base (mip levels packed in a page) add up to one piece of content.
            UploadRecord &record = uploads[dbp];
            if (atOrigin)
            {
                record.hash = hash;
                record.blocks = blocks;
            }
            else
            {
                record.hash = record.hash * 0x100000001b3ull ^ hash;
                record.blocks.insert(record.blocks.end(), blocks.begin(), blocks.end());
                std::sort(record.blocks.begin(), record.blocks.end());
                record.blocks.erase(std::unique(record.blocks.begin(), record.blocks.end()), record.blocks.end());
            }
            record.blockSum = generationSum(record.blocks);
            for (const uint16_t block : blocks)
                blockOwner[block] = uint16_t(dbp + 1u);
            // A transfer owns what it covers even when it changed no byte of the CPU copy: whatever a target drew
            // there is superseded (the texture is refreshed from VRAM before its next use) and must not be read back.
            releaseBlocks(blocks, true);
        }

        // These blocks now belong to the CPU copy: forget that targets drew there.
        void releaseBlocks(const std::vector<uint16_t> &blocks, bool refreshTargets)
        {
            bool any = false;
            for (const uint16_t block : blocks)
                any = any || gpuDirtyBlocks.test(block);
            if (!any)
                return;
            for (auto &entry : targets)
            {
                Target &target = entry.second;
                for (const uint16_t block : blocks)
                    if (target.gpuDirty.test(block))
                    {
                        target.gpuDirty.reset(block);
                        target.markedRect = ~0ull;
                        if (refreshTargets && (block >> 5) >= target.fbp)
                            target.stale.set((block >> 5) - target.fbp);
                    }
            }
            for (const uint16_t block : blocks)
                gpuDirtyBlocks.reset(block);
        }

        uint64_t generationSum(const std::vector<uint16_t> &blocks) const
        {
            uint64_t sum = 0;
            for (const uint16_t block : blocks)
                sum += blockGeneration[block];
            return sum;
        }

        uint64_t scanoutCount() const { return scanouts; }
        void flushRequested()
        {
            ++flushRequests;
            static const bool eager = std::getenv("PS2X_GS_NATIVE_EAGER_FLUSH") != nullptr;
            if (eager || drawCalls - drawsAtCommit >= 400u)
            {
                drawsAtCommit = drawCalls;
                flush(false);
            }
        }

        void flush(bool wait)
        {
            closeBatch();
            endPass();
            if (commandBuffer)
            {
                [commandBuffer commit];
                if (wait)
                    [commandBuffer waitUntilCompleted];
                lastCommitted = commandBuffer;
                commandBuffer = nil;
            }
            else if (wait && lastCommitted)
                [lastCommitted waitUntilCompleted];
        }

        // Direct presentation: a Metal layer over the window, fed from the displayed target without a trip through the CPU.
        bool attachWindow(void *window)
        {
            NSWindow *nsWindow = (__bridge NSWindow *)window;
            NSView *content = nsWindow.contentView;
            if (!content)
                return false;
            presentView = [[BlackNativeGSView alloc] initWithFrame:content.bounds];
            presentView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            // The Metal layer is a sublayer of the view's own layer: there its gravity decides how the picture fills
            // the window (stretched to the window's shape: 4:3, or 16:9 in the wide mode).
            presentView.layer = [CALayer layer];
            presentView.wantsLayer = YES;
            presentLayer = [CAMetalLayer layer];
            presentLayer.device = device;
            presentLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            presentLayer.frame = presentView.layer.bounds;
            presentLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
            presentLayer.contentsScale = nsWindow.backingScaleFactor;
            presentLayer.framebufferOnly = YES;
            presentLayer.contentsGravity = kCAGravityResize;
            [presentView.layer addSublayer:presentLayer];
            [content addSubview:presentView positioned:NSWindowAbove relativeTo:nil];
            return true;
        }

        int present(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t dbx, uint32_t dby, uint32_t width, uint32_t height, GSParallelScanout &request)
        {
            closeBatch();
            // A buffer nothing was drawn to (a video frame sent by the CPU) becomes a target fed from VRAM.
            Target &shown = target(fbp, fbw, psm);
            const uint32_t scale = scaleOf(fbw);
            shown.usedRows = std::max(shown.usedRows, std::min(dby + height, kTargetHeight));
            refreshTarget(shown, shown.psm);
            width = std::min(width, fbw * 64u - std::min(dbx, fbw * 64u));
            height = std::min(height, kTargetHeight - std::min(dby, kTargetHeight));
            if (width == 0u || height == 0u)
                return 0;
            presentLayer.drawableSize = CGSizeMake(width * scale, height * scale);
            id<CAMetalDrawable> drawable = [presentLayer nextDrawable];
            if (!drawable)
                return 0;
            endPass();
            ensureCommandBuffer();
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> quad = [commandBuffer renderCommandEncoderWithDescriptor:pass];
            const float uniforms[4] = {float(dbx) / float(fbw * 64u), float(dby) / float(kTargetHeight),
                                       float(width) / float(fbw * 64u) / float(drawable.texture.width), float(height) / float(kTargetHeight) / float(drawable.texture.height)};
            [quad setRenderPipelineState:presentPipeline];
            [quad setFragmentTexture:shown.color atIndex:0];
            [quad setFragmentSamplerState:sampler(true, 1, 1) atIndex:0];
            [quad setFragmentBytes:uniforms length:sizeof(uniforms) atIndex:1];
            [quad drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [quad endEncoding];
            [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
              const uint64_t at = uint64_t(shown.presentedTime * 1e9);
              if (at == 0u)
                  return; // never shown
              const uint64_t before = g_lastPresented.exchange(at, std::memory_order_relaxed);
              if (before != 0u && at > before)
              {
                  const uint64_t gap = at - before;
                  g_presented.fetch_add(1u, std::memory_order_relaxed);
                  if (gap > 26000000u) // more than three refreshes of a 120 Hz screen: a frame's worth of delay at 60
                      g_presentLate.fetch_add(1u, std::memory_order_relaxed);
                  uint64_t worst = g_presentWorst.load(std::memory_order_relaxed);
                  while (gap > worst && !g_presentWorst.compare_exchange_weak(worst, gap, std::memory_order_relaxed)) {}
              }
            }];
            [commandBuffer presentDrawable:drawable];
            flush(false);
            request.width = width;
            request.height = height;
            ++scanouts;
            noteScanout();
            return 1;
        }

        int scanout(GSParallelScanout &request)
        {
            // Circuit 1 when it is enabled, else circuit 2.
            const bool first = (request.pmode & 1u) != 0u;
            if (!first && (request.pmode & 2u) == 0u)
                return 0;
            const uint64_t dispfb = first ? request.dispfb1 : request.dispfb2, display = first ? request.display1 : request.display2;
            uint32_t fbp = uint32_t(dispfb) & 0x1FFu, fbw = uint32_t(dispfb >> 9) & 0x3Fu, psm = uint32_t(dispfb >> 15) & 0x1Fu;
            // PS2X_GS_NATIVE_SHOW=<fbp>,<fbw>,<psm> (diagnostic): scan out that target instead of the displayed one.
            if (const char *show = std::getenv("PS2X_GS_NATIVE_SHOW"))
                std::sscanf(show, "%u,%u,%u", &fbp, &fbw, &psm);
            const uint32_t dbx = uint32_t(dispfb >> 32) & 0x7FFu, dby = uint32_t(dispfb >> 43) & 0x7FFu;
            const uint32_t magh = uint32_t(display >> 23) & 0xFu;
            uint32_t width = ((uint32_t(display >> 32) & 0xFFFu) + 1u) / (magh + 1u), height = (uint32_t(display >> 44) & 0x7FFu) + 1u;
            if (fbw == 0u)
                return 0;
            if (presentLayer && !request.rgba)
                return present(fbp, fbw, psm, dbx, dby, width, height, request);
            if (!request.rgba)
                return 0;
            width = std::min({width, fbw * 64u, request.stride / 4u});
            height = std::min(height, request.maxHeight);
            flush(true);
            const auto found = targets.find(targetKey(fbp, fbw));
            if (found != targets.end())
            {
                Target &target = found->second;
                refreshTarget(target, psm);
                flush(true);
                width = std::min(width, fbw * 64u - std::min(dbx, fbw * 64u));
                height = std::min(height, kTargetHeight - std::min(dby, kTargetHeight));
                if (width == 0u || height == 0u)
                    return 0;
                const uint32_t scale = scaleOf(fbw);
                if (scale == 1u)
                    [target.color getBytes:request.rgba bytesPerRow:width * 4u fromRegion:MTLRegionMake2D(dbx, dby, width, height) mipmapLevel:0];
                else if (width * scale * 4u <= request.stride && height * scale <= request.maxHeight)
                {
                    // Room for the whole image: rows packed at its own width, as the caller expects from a larger scanout.
                    [target.color getBytes:request.rgba bytesPerRow:width * scale * 4u fromRegion:MTLRegionMake2D(dbx * scale, dby * scale, width * scale, height * scale) mipmapLevel:0];
                    width *= scale;
                    height *= scale;
                }
                else
                {
                    // The caller's buffer is at the game's resolution: filter the larger image down to it.
                    if (!resolveTexture || resolveTexture.width != width || resolveTexture.height != height)
                    {
                        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
                        descriptor.usage = MTLTextureUsageRenderTarget;
                        descriptor.storageMode = MTLStorageModeShared;
                        resolveTexture = [device newTextureWithDescriptor:descriptor];
                    }
                    ensureCommandBuffer();
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = resolveTexture;
                    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> quad = [commandBuffer renderCommandEncoderWithDescriptor:pass];
                    const float uniforms[4] = {float(dbx) / float(fbw * 64u), float(dby) / float(kTargetHeight), 1.0f / float(fbw * 64u), 1.0f / float(kTargetHeight)};
                    [quad setRenderPipelineState:resolvePipeline];
                    [quad setFragmentTexture:target.color atIndex:0];
                    [quad setFragmentSamplerState:sampler(true, 1, 1) atIndex:0];
                    [quad setFragmentBytes:uniforms length:sizeof(uniforms) atIndex:1];
                    [quad drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [quad endEncoding];
                    flush(true);
                    [resolveTexture getBytes:request.rgba bytesPerRow:width * 4u fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
                }
            }
            else
            {
                GSMem::TexturePageCache cache;
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const uint32_t pixel = expand(GSMem::ReadTexture(cache, vram.data(), psm, fbp * 32u, fbw, x + dbx, y + dby), psm, 0x80u, 0x80u, false);
                        std::memcpy(request.rgba + (size_t(y) * width + x) * 4u, &pixel, 4);
                    }
            }
            for (size_t i = 0, n = size_t(width) * height; i < n; ++i)
                request.rgba[i * 4u + 3u] = 255u;
            request.width = width;
            request.height = height;
            ++scanouts;
            noteScanout();
            if (stats && scanouts % 60u == 0u)
            {
                std::fprintf(stderr, "[native-gs] gpu=%.2f ms/scanout passes=%.1f/scanout\n", double(g_gpuNanoseconds.load()) / 1e6 / double(std::max<uint64_t>(scanouts, 1)), double(passes) / double(std::max<uint64_t>(scanouts, 1)));
                if (replPack.size() != 0)
                    std::fprintf(stderr, "[texrepl] packs=%zu lookups=%llu hits=%llu (+mips %llu) loaded=%zu (%.0f MB) failures=%llu\n", replPack.size(), (unsigned long long)replLookups, (unsigned long long)replHitsBase, (unsigned long long)replHitsMips, replCache.size(), double(replBytes) / 1048576.0, (unsigned long long)replLoadFailures);
                std::fprintf(stderr, "[native-gs] drawn=%llu skipped: depth-format target=%llu aliased target=%llu lines/points=%llu ztest never=%llu | draws=%llu readbacks=%llu palette copies=%llu uploads skipped=%llu textures=%zu decoded=%llu (no upload %llu, changed since %llu, mips elsewhere %llu) reused=%llu targets=%zu target-as-texture=%llu (own target %llu)\n",
                             (unsigned long long)drawn, (unsigned long long)skipped[SkipDepthTarget], (unsigned long long)skipped[SkipAliasedTarget], (unsigned long long)skipped[SkipLinesPoints],
                             (unsigned long long)skipped[SkipNever], (unsigned long long)drawCalls, (unsigned long long)readbacks, (unsigned long long)drawnPaletteCopies, (unsigned long long)uploadsSkipped, textures.size(), (unsigned long long)decodes, (unsigned long long)unknownNoUpload, (unsigned long long)unknownChanged, (unsigned long long)unknownMips, (unsigned long long)contentHits, targets.size(), (unsigned long long)targetSamples, (unsigned long long)selfSamples);
            }
            return 1;
        }

    private:
        enum SkipReason { SkipNone, SkipDepthTarget, SkipAliasedTarget, SkipLinesPoints, SkipNever, SkipCount };

        struct Target
        {
            id<MTLTexture> color = nil;
            uint32_t fbp = 0, fbw = 0, psm = 0;
            id<MTLTexture> copy = nil; // for draws that sample the target they draw to
            bool drawn = false;
            std::bitset<kPages> stale; // pages (relative to fbp) whose CPU copy is newer than the texture
            std::bitset<kVramBytes / 256u> gpuDirty; // blocks (absolute) drawn to since VRAM last received them
            uint64_t markedRect = ~0ull;     // the rectangle gpuDirty already covers (most draws repeat it)
            uint32_t usedRows = 0; // how far down anything was drawn: the memory below belongs to something else
            uint32_t pageCount() const
            {
                const uint32_t pageHeight = psm == 2u || psm == 0xAu ? 64u : 32u;
                return std::min(kPages - fbp, fbw * ((usedRows + pageHeight - 1u) / pageHeight));
            }
        };

        struct DepthTarget
        {
            id<MTLTexture> texture = nil;
            bool fresh = false; // not drawn to yet: its first pass starts from depth 0
        };

        struct TextureEntry
        {
            id<MTLTexture> texture = nil;
            std::vector<uint16_t> pages;
            uint64_t hash = 0;
            bool valid = false, drawnOver = false, registered = false;
            std::vector<uint16_t> mipBlocks; // blocks of the levels below the base, level after level
            uint32_t mipStart[9] = {};
        };

        struct Batch
        {
            gsn::Context ctx;
            uint32_t mode = 0;
            uint64_t texa = 0, fogcol = 0, colclamp = 0, texclut = 0;
            float offsetX = 0.0f, offsetY = 0.0f, texelU = 1.0f, texelV = 1.0f, maxY = 0.0f, maxX = 0.0f;
            bool gouraud = false, fst = false, textured = false, skip = false, depthCopy = false, shuffle = false, selfFetch = false;
            uint32_t shuffleSource = 0, shuffleDestination = 0;
            SkipReason skipReason = SkipNone;
            uint32_t firstVertex = 0, vertexCount = 0, logged = 0, primitives = 0;
        };

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<MTLRenderPipelineState> opaquePipeline = nil, boundPipeline = nil;
        // One pipeline per combination of the path-selecting state (see GS_SPECIALIZED in the shader): compiled in the
        // background the first time the combination is drawn, the generic pipeline standing in until it is ready.
        id<MTLLibrary> specializedLibrary = nil;
        MTLRenderPipelineDescriptor *baseDescriptor = nil;
        struct Specialized { id<MTLRenderPipelineState> state = nil; bool requested = false; };
        std::unordered_map<uint64_t, Specialized> specialized;
        std::mutex specializedMutex;
        uint64_t specializedDraws = 0, genericDraws = 0;
        id<MTLRenderPipelineState> specializedPipeline(const Uniforms &u, bool opaque)
        {
            if (!specializedLibrary)
                return nil;
            const uint32_t values[25] = {u.tme, u.tfx, u.tcc, u.fge, u.abe, u.blendA, u.blendB, u.blendC, u.blendD, u.atst, u.alphaMode, u.writeMode, u.fba, u.colclamp,
                                         u.destinationHasAlpha, u.texAlphaFixed, u.texIndexed, u.texLinear, u.wrapU, u.wrapV, u.texBox, u.texSelf,
                                         u.maxLevel != 0u ? 1u : 0u, u.lodFixed != 0u ? 1u : 0u, u.fbmask != 0u ? 1u : 0u};
            uint64_t key = opaque ? 1u : 0u;
            for (uint32_t value : values)
                key = key * 11u + std::min(value, 10u);
            std::lock_guard<std::mutex> lock(specializedMutex);
            Specialized &entry = specialized[key];
            if (entry.state || entry.requested)
                return entry.state;
            entry.requested = true;
            MTLFunctionConstantValues *constants = [MTLFunctionConstantValues new];
            [constants setConstantValues:values type:MTLDataTypeUInt withRange:NSMakeRange(0, 25)];
            NSError *error = nil;
            MTLRenderPipelineDescriptor *descriptor = [baseDescriptor copy];
            descriptor.fragmentFunction = [specializedLibrary newFunctionWithName:opaque ? @"fragmentOpaque" : @"fragmentMain" constantValues:constants error:&error];
            if (!descriptor.fragmentFunction)
                return nil;
            Renderer *self = this;
            [device newRenderPipelineStateWithDescriptor:descriptor completionHandler:^(id<MTLRenderPipelineState> state, NSError *) {
              std::lock_guard<std::mutex> ready(self->specializedMutex);
              self->specialized[key].state = state;
            }];
            return nil;
        }
        id<MTLRenderPipelineState> pipeline = nil, depthCopyPipeline = nil, depthCopyClearPipeline = nil, shufflePipeline = nil, presentPipeline = nil, resolvePipeline = nil;
        BlackNativeGSView *presentView = nil;
        CAMetalLayer *presentLayer = nil;
        std::map<uint64_t, std::pair<id<MTLTexture>, uint64_t>> palettes; // texture and the hash of its entries
        std::map<uint32_t, id<MTLTexture>> drawnPalettes;                // by CBP: copies of palettes that live in a target
        uint64_t drawnPaletteCopies = 0;
        id<MTLTexture> fallbackTexture = nil;
        id<MTLCommandBuffer> commandBuffer = nil, lastCommitted = nil;
        id<MTLRenderCommandEncoder> encoder = nil;
        id<MTLBuffer> chunk = nil;
        uint32_t chunkUsed = 0;
        std::vector<id<MTLBuffer>> chunkPool;
        std::mutex chunkMutex;
        uint64_t passTarget = ~0ull, passDepth = ~0ull;
        std::map<uint64_t, Target> targets;
        std::map<uint64_t, DepthTarget> depths;
        std::map<uint32_t, id<MTLDepthStencilState>> depthStates;
        std::map<uint32_t, id<MTLSamplerState>> samplers;
        std::unordered_map<uint64_t, TextureEntry> textures;
        // What is known about the bytes at a transfer destination: used to recognize the same texture when the game
        // sends it again, possibly somewhere else (it refills its texture memory several times per frame).
        struct UploadRecord
        {
            uint64_t hash = 0, blockSum = 0;
            std::vector<uint16_t> blocks;
        };
        std::unordered_map<uint32_t, UploadRecord> uploads;
        std::unordered_map<uint64_t, UploadRecord> rectangles; // by destination rectangle: the last transfer written there
        uint64_t uploadsSkipped = 0;
        std::unordered_map<uint64_t, id<MTLTexture>> contentCache;
        uint64_t blockGeneration[kVramBytes / 256u] = {};
        std::bitset<kVramBytes / 256u> gpuDirtyBlocks; // union of every target's gpuDirty
        uint64_t syncReason = 0;
        std::vector<TextureEntry *> blockUsers[kVramBytes / 256u]; // decoded textures that read each block
        uint64_t readbacks = 0;
        uint16_t blockOwner[kVramBytes / 256u] = {}; // base of the last upload that covered the block, plus one
        uint64_t vramGeneration = 1;
        Batch batch;
        bool batchOpen = false, stats = false;
        uint32_t scale = 1;
        // Every buffer is drawn larger, and a larger buffer read back with the bilinear filter is averaged over the
        // game's own texel size (texBox in the shader), which keeps the blur the game makes by reading half a texel
        // off (light shafts, glow, smoke: 160 wide buffers). PS2X_GS_NATIVE_SCALE_NARROW=0 instead keeps buffers
        // narrower than a screen (fbw < 8) at the game's size: exact, but blocky next to the enlarged scene.
        uint32_t scaleMinWidth = 0;
        uint32_t scaleOf(uint32_t fbw) const { return fbw >= scaleMinWidth ? scale : 1u; }
        id<MTLTexture> resolveTexture = nil;
        uint64_t passes = 0, passDrawsStart = 0;
        bool passTimes = std::getenv("PS2X_GS_NATIVE_PASS_TIMES") != nullptr;
        uint64_t flushRequests = 0, drawsAtCommit = 0, windowFlushes = 0;
        bool noOpaque = std::getenv("PS2X_GS_NATIVE_NO_OPAQUE") != nullptr; // every draw through the destination-reading shader
        bool noSelfFetch = std::getenv("PS2X_GS_NATIVE_NO_SELF_FETCH") != nullptr; // in-place reads through a copy, as before
        bool splitting = false; // closeBatch() is drawing the primitives of a self-sampling batch one by one
        bool statsEnabled = std::getenv("PS2X_GS_NATIVE_STATS") != nullptr;
        uint64_t drawn = 0, skipped[SkipCount] = {}, drawCalls = 0, decodes = 0, scanouts = 0, targetSamples = 0, selfSamples = 0, contentHits = 0, unknownNoUpload = 0, unknownChanged = 0, unknownMips = 0;

        bool logging() const
        {
            static const char *const logAt = std::getenv("PS2X_GS_NATIVE_LOG");
            static const uint64_t at = logAt ? std::strtoull(logAt, nullptr, 0) : ~0ull;
            return scanouts == at || at == 0xFFFFFFFFull; // 0xFFFFFFFF: every scanout
        }

        static uint64_t targetKey(uint32_t fbp, uint32_t fbw) { return uint64_t(fbp) | (uint64_t(fbw) << 16); }

        void put(GpuVertex &out, const gsn::Vertex &in, uint32_t rgba, uint32_t z)
        {
            out.x = float(in.x) * (1.0f / 16.0f) - batch.offsetX;
            out.y = float(in.y) * (1.0f / 16.0f) - batch.offsetY;
            batch.maxY = std::max(batch.maxY, out.y);
            batch.maxX = std::max(batch.maxX, out.x);
            out.z = float(z & 0xFFFFFFu) * (1.0f / 16777216.0f);
            out.rgba = rgba;
            if (batch.fst)
            {
                out.s = float(in.u) * batch.texelU;
                out.t = float(in.v) * batch.texelV;
                out.q = 1.0f;
            }
            else
            {
                out.s = in.s;
                out.t = in.t;
                out.q = in.q;
            }
            out.fog = float(in.fog);
        }

        void openBatch()
        {
            const gsn::Registers &regs = frontend.regs;
            batch = Batch{};
            batch.mode = regs.mode();
            batch.ctx = regs.ctx[(batch.mode >> 9) & 1u];
            batch.texa = regs.texa;
            batch.fogcol = regs.fogcol;
            batch.colclamp = regs.colclamp;
            batch.texclut = regs.texclut;
            batch.gouraud = (batch.mode & 0x008u) != 0u;
            batch.textured = (batch.mode & 0x010u) != 0u;
            batch.fst = (batch.mode & 0x100u) != 0u;
            batch.offsetX = float(batch.ctx.xyoffset & 0xFFFFu) * (1.0f / 16.0f);
            batch.offsetY = float((batch.ctx.xyoffset >> 32) & 0xFFFFu) * (1.0f / 16.0f);
            batch.texelU = 1.0f / (16.0f * float(1u << (uint32_t(batch.ctx.tex0 >> 26) & 15u)));
            batch.texelV = 1.0f / (16.0f * float(1u << (uint32_t(batch.ctx.tex0 >> 30) & 15u)));
            const uint32_t fbp = uint32_t(batch.ctx.frame) & 0x1FFu, fbw = uint32_t(batch.ctx.frame >> 16) & 0x3Fu, psm = uint32_t(batch.ctx.frame >> 24) & 0x3Fu;
            const uint32_t test = uint32_t(batch.ctx.test);
            const uint32_t tpsm = uint32_t(batch.ctx.tex0 >> 20) & 0x3Fu, tbp = uint32_t(batch.ctx.tex0) & 0x3FFFu, tbw = uint32_t(batch.ctx.tex0 >> 14) & 0x3Fu;
            const bool flatSprite = (batch.mode & 7u) == 6u && batch.textured && batch.fst;
            if (fbw != 0u && isDepthFormat(psm) && flatSprite && isDepthFormat(tpsm) && (tbp & 31u) == 0u && tbw != 0u)
                batch.depthCopy = true;
            else if (isDepthFormat(psm) || fbw == 0u)
                batch.skip = true, batch.skipReason = SkipDepthTarget;
            else if ((test & 0x10000u) != 0u && ((test >> 17) & 3u) == 0u)
                batch.skip = true, batch.skipReason = SkipNever;
            else
            {
                // The same memory drawn to with another pixel size (32 bits against 16): not translated yet.
                const auto found = targets.find(targetKey(fbp, fbw));
                if (found != targets.end() && (found->second.psm == 2u || found->second.psm == 0xAu) != (psm == 2u || psm == 0xAu))
                {
                    // A 32-bit target drawn to as 16-bit pixels, textured with itself or with a depth buffer in the same
                    // 16-bit view: a channel shuffle, done in one full-screen pass. Anything else is not translated.
                    const bool fromSelf = (tpsm == 2u || tpsm == 0xAu) && tbp == fbp * 32u && tbw == fbw;
                    const bool fromDepth = (tpsm == 0x32u || tpsm == 0x3Au) && (tbp & 31u) == 0u && tbw == fbw;
                    if ((psm == 2u || psm == 0xAu) && flatSprite && (fromSelf || fromDepth) && (uint32_t(batch.ctx.tex0 >> 35) & 3u) == 1u && (batch.mode & 0x40u) != 0u
                        && (uint32_t(batch.ctx.alpha) & 0xFFu) == 0x2Au)
                        batch.shuffle = true;
                    else
                        batch.skip = true, batch.skipReason = SkipAliasedTarget;
                }
            }
            batch.firstVertex = chunkUsed;
            batchOpen = true;
        }

        GpuVertex *reserve(uint32_t count)
        {
            if (!chunk || chunkUsed + count > kChunkVertices)
            {
                // Out of room: what is queued goes out with the old buffer, the batch carries on in a new one.
                const Batch saved = batch;
                closeBatch();
                if (chunk)
                {
                    // Command buffers finish in order: when the last one that may use this buffer is done, it is free.
                    ensureCommandBuffer();
                    id<MTLBuffer> used = chunk;
                    Renderer *self = this;
                    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                      std::lock_guard<std::mutex> lock(self->chunkMutex);
                      self->chunkPool.push_back(used);
                    }];
                    flush(false);
                    chunk = nil;
                }
                {
                    std::lock_guard<std::mutex> lock(chunkMutex);
                    if (!chunkPool.empty())
                    {
                        chunk = chunkPool.back();
                        chunkPool.pop_back();
                    }
                }
                if (!chunk)
                    chunk = [device newBufferWithLength:kChunkVertices * sizeof(GpuVertex) options:MTLResourceStorageModeShared];
                chunkUsed = 0;
                batch = saved;
                batch.firstVertex = 0;
                batch.vertexCount = 0;
                batchOpen = true;
            }
            GpuVertex *out = static_cast<GpuVertex *>(chunk.contents) + chunkUsed;
            chunkUsed += count;
            batch.vertexCount += count;
            return out;
        }

        // GPU time per scanout, watched in windows of 120: when the GPU needed more than a vertical blank for some of
        // them, a line goes to stderr with the "[black-fps]" tag the launcher keeps in recomp/diagnostics/hitches.log
        // (the game's own counters do not see a late GPU: it hands every frame over on time).
        uint64_t windowGpu = 0, windowWorst = 0, windowLate = 0, windowCount = 0, lastGpu = 0, windowPasses = 0, windowDraws = 0, windowPrimitives = 0, windowCopies = 0;
        void noteScanout()
        {
            const uint64_t total = g_gpuNanoseconds.load(std::memory_order_relaxed), spent = total - lastGpu;
            lastGpu = total;
            windowGpu += spent;
            windowWorst = std::max(windowWorst, spent);
            windowLate += spent > 16000000u ? 1u : 0u;
            if (++windowCount < 120u)
                return;
            // On screen: frames that came more than 25 ms after the one before them are the ones the eye sees.
            const uint64_t shown = g_presented.exchange(0u), late = g_presentLate.exchange(0u), gap = g_presentWorst.exchange(0u);
            if (late != 0u || statsEnabled)
                std::fprintf(stderr, "[black-fps] gpu: %llu of %llu frames reached the screen late (longest gap %.1f ms) | GPU mean %.1f ms, %llu of 120 scanouts over 16 ms | per scanout: %.0f passes, %.0f flush requests, %.0f draws, %.0f primitives, %.1f copies of a target | scale %u\n",
                             (unsigned long long)late, (unsigned long long)shown, double(gap) / 1e6, double(windowGpu) / 120e6, (unsigned long long)windowLate, double(passes - windowPasses) / 120.0, double(flushRequests - windowFlushes) / 120.0,
                             double(drawCalls - windowDraws) / 120.0, double(drawn - windowPrimitives) / 120.0, double(selfSamples - windowCopies) / 120.0, scale);
            windowGpu = windowWorst = windowLate = windowCount = 0;
            windowPasses = passes;
            windowFlushes = flushRequests;
            windowDraws = drawCalls;
            windowPrimitives = drawn;
            windowCopies = selfSamples;
        }

        void ensureCommandBuffer()
        {
            if (commandBuffer)
                return;
            commandBuffer = [queue commandBuffer];
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    // Command buffers overlap on the GPU (the next one starts while this one is still finishing): the
                    // time is counted once, as the union of their intervals, or the sum would say the GPU is busier than
                    // it is.
                    static std::mutex busyMutex;
                    static double busyUntil = 0.0;
                    std::lock_guard<std::mutex> lock(busyMutex);
                    const double begin = std::max(done.GPUStartTime, busyUntil), end = done.GPUEndTime;
                    if (end > begin)
                        g_gpuNanoseconds.fetch_add(uint64_t((end - begin) * 1e9), std::memory_order_relaxed);
                    busyUntil = std::max(busyUntil, end);
                }];
        }

        void endPass()
        {
            if (encoder)
            {
                [encoder endEncoding];
                encoder = nil;
                // PS2X_GS_NATIVE_PASS_TIMES=1 (diagnostic, slow): every pass in a command buffer of its own, so the GPU
                // time of each can be read; the totals per target are printed at the end.
                if (passTimes && commandBuffer)
                {
                    const uint64_t key = passTarget;
                    const uint64_t draws = drawCalls - passDrawsStart;
                    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
                      std::lock_guard<std::mutex> lock(g_passTimeMutex);
                      PassTime &entry = g_passTime[key];
                      entry.seconds += done.GPUEndTime - done.GPUStartTime;
                      entry.passes += 1;
                      entry.draws += draws;
                    }];
                    [commandBuffer commit];
                    lastCommitted = commandBuffer;
                    commandBuffer = nil;
                }
            }
            passTarget = passDepth = ~0ull;
        }

        static uint32_t expand(uint32_t value, uint32_t psm, uint32_t ta0, uint32_t ta1, bool aem)
        {
            switch (psm)
            {
            case 0x00: return value;
            case 0x01: return (value & 0xFFFFFFu) | ((aem && (value & 0xFFFFFFu) == 0u ? 0u : ta0) << 24);
            case 0x02: case 0x0A:
            {
                const uint32_t rgb = ((value & 0x1Fu) << 3) | ((value & 0x3E0u) << 6) | ((value & 0x7C00u) << 9);
                const uint32_t alpha = (value & 0x8000u) != 0u ? ta1 : (aem && (value & 0x7FFFu) == 0u ? 0u : ta0);
                return rgb | (alpha << 24);
            }
            default: return value;
            }
        }

        Target &target(uint32_t fbp, uint32_t fbw, uint32_t psm)
        {
            Target &entry = targets[targetKey(fbp, fbw)];
            if (!entry.color)
            {
                const uint32_t scale = scaleOf(fbw);
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:fbw * 64u * scale height:kTargetHeight * scale mipmapped:NO];
                descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                descriptor.storageMode = MTLStorageModeShared;
                entry.color = [device newTextureWithDescriptor:descriptor];
                entry.fbp = fbp;
                entry.fbw = fbw;
                entry.psm = psm;
                entry.stale.set();
            }
            return entry;
        }

        // Brings the pages a host transfer wrote since the last use from the CPU copy of VRAM into the texture.
        void refreshTarget(Target &target, uint32_t psm)
        {
            if (target.stale.none())
                return;
            const bool narrow = psm == 2u || psm == 0xAu; // 16-bit pages hold 64x64 pixels
            const uint32_t pageHeight = narrow ? 64u : 32u, pages = target.pageCount();
            // The pages go through a buffer and a blit in the command stream, in order with the draws around them: writing
            // the texture from the CPU would mean waiting for the GPU to finish everything queued so far.
            uint32_t count = 0;
            for (uint32_t page = 0; page < pages; ++page)
                if (target.stale.test(page) && (page / target.fbw + 1u) * pageHeight <= kTargetHeight)
                    ++count;
            const uint32_t scale = scaleOf(target.fbw);
            const uint32_t stride = 64u * scale, rows = pageHeight * scale;
            const size_t pageBytes = size_t(stride) * rows * 4u;
            id<MTLBuffer> staging = count != 0u ? [device newBufferWithLength:pageBytes * count options:MTLResourceStorageModeShared] : nil;
            id<MTLBlitCommandEncoder> blit = nil;
            const gsn::Swizzle &layout = gsn::Swizzle::of(psm);
            uint32_t slot = 0;
            for (uint32_t page = 0; page < pages; ++page)
            {
                if (!target.stale.test(page))
                    continue;
                target.stale.reset(page);
                const uint32_t x0 = (page % target.fbw) * 64u, y0 = (page / target.fbw) * pageHeight;
                if (y0 + pageHeight > kTargetHeight)
                    continue;
                if (!blit)
                {
                    closeBatch();
                    endPass();
                    ensureCommandBuffer();
                    blit = [commandBuffer blitCommandEncoder];
                }
                uint32_t *pixels = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(staging.contents) + pageBytes * slot);
                for (uint32_t y = 0; y < rows; ++y)
                    for (uint32_t x = 0; x < stride; ++x)
                        pixels[size_t(y) * stride + x] = (y % scale) != 0u ? pixels[size_t(y - 1u) * stride + x] : (x % scale) != 0u ? pixels[size_t(y) * stride + x - 1u]
                            : expand(layout.read(vram.data(), target.fbp * 32u, target.fbw, x0 + x / scale, y0 + y / scale), psm, 0x80u, 0x80u, false);
                [blit copyFromBuffer:staging sourceOffset:pageBytes * slot sourceBytesPerRow:stride * 4u sourceBytesPerImage:pageBytes sourceSize:MTLSizeMake(stride, rows, 1)
                           toTexture:target.color destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(x0 * scale, y0 * scale, 0)];
                ++slot;
            }
            [blit endEncoding];
        }

        DepthTarget &depth(uint32_t zbp, uint32_t fbw)
        {
            DepthTarget &entry = depths[targetKey(zbp, fbw)];
            id<MTLTexture> __strong &texture = entry.texture;
            if (!texture)
            {
                const uint32_t scale = scaleOf(fbw);
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:fbw * 64u * scale height:kTargetHeight * scale mipmapped:NO];
                descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                descriptor.storageMode = MTLStorageModePrivate;
                texture = [device newTextureWithDescriptor:descriptor];
                // Starts as what VRAM holds there (24-bit Z), e.g. a buffer the game filled before it was first used here.
                const uint32_t width = fbw * 64u * scale, rows = kTargetHeight * scale;
                id<MTLBuffer> staging = [device newBufferWithLength:size_t(width) * rows * 4u options:MTLResourceStorageModeShared];
                float *values = static_cast<float *>(staging.contents);
                const gsn::Swizzle &layout = gsn::Swizzle::of(0x31u);
                for (uint32_t y = 0; y < rows; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                        values[size_t(y) * width + x] = float(layout.read(vram.data(), zbp * 32u, fbw, x / scale, y / scale)) * (1.0f / 16777216.0f);
                endPass();
                ensureCommandBuffer();
                id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:width * 4u sourceBytesPerImage:size_t(width) * rows * 4u sourceSize:MTLSizeMake(width, rows, 1)
                           toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
            }
            return entry;
        }

        id<MTLDepthStencilState> depthState(bool test, bool greaterOnly, bool write)
        {
            const uint32_t key = (test ? 1u : 0u) | (greaterOnly ? 2u : 0u) | (write ? 4u : 0u);
            id<MTLDepthStencilState> __strong &state = depthStates[key];
            if (!state)
            {
                MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
                descriptor.depthCompareFunction = !test ? MTLCompareFunctionAlways : greaterOnly ? MTLCompareFunctionGreater : MTLCompareFunctionGreaterEqual;
                descriptor.depthWriteEnabled = write;
                state = [device newDepthStencilStateWithDescriptor:descriptor];
            }
            return state;
        }

        id<MTLSamplerState> sampler(bool linear, uint32_t wrapU, uint32_t wrapV)
        {
            const uint32_t key = (linear ? 1u : 0u) | (wrapU << 1) | (wrapV << 3);
            id<MTLSamplerState> __strong &state = samplers[key];
            if (!state)
            {
                MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
                descriptor.minFilter = descriptor.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                descriptor.mipFilter = MTLSamplerMipFilterLinear;
                if (linear)
                    descriptor.maxAnisotropy = 8;
                // The region modes resolve the coordinate in the shader; the sampler only has to stay out of the way.
                descriptor.sAddressMode = wrapU != 0u ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                descriptor.tAddressMode = wrapV != 0u ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                state = [device newSamplerStateWithDescriptor:descriptor];
            }
            return state;
        }

        // ---- texture replacement packs (PS2X_TEXTURE_PACKS), same naming and hashes as PCSX2 ----
        texrepl::Pack replPack;
        struct ReplEntry
        {
            id<MTLTexture> texture = nil; // nil: the file could not be used, do not try again
            size_t bytes = 0;
            uint64_t lastUse = 0;
        };
        std::unordered_map<texrepl::Key, ReplEntry, texrepl::KeyHash> replCache;
        std::unordered_set<const void *> replacedTextures;
        std::unordered_set<texrepl::Key, texrepl::KeyHash> replLogged;
        size_t replBytes = 0, replBudget = 2048ull << 20;
        uint64_t replClock = 0, replLookups = 0, replHitsBase = 0, replHitsMips = 0, replLoadFailures = 0;
        bool replLog = false, replReady = false;

        void initReplacements()
        {
            if (replReady)
                return;
            replReady = true;
            if (const char *dirs = std::getenv("PS2X_TEXTURE_PACKS"))
                replPack.scan(dirs);
            if (const char *mb = std::getenv("PS2X_TEXTURE_CACHE_MB"))
                replBudget = size_t(std::max(64, std::atoi(mb))) << 20;
            replLog = std::getenv("PS2X_TEXTURE_LOG") != nullptr;
            if (replPack.size() != 0 && ![device supportsBCTextureCompression])
            {
                std::fprintf(stderr, "[texrepl] this GPU has no BC texture compression: replacements disabled\n");
                replPack = texrepl::Pack();
            }
        }

        bool loadPng(const std::string &path, texrepl::Image &out)
        {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
            CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
            if (!source)
                return false;
            CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
            CFRelease(source);
            if (!image)
                return false;
            const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
            if (w == 0 || h == 0 || w > 8192 || h > 8192)
            {
                CGImageRelease(image);
                return false;
            }
            std::vector<uint8_t> rgba(w * h * 4);
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            CGContextRef context = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(space);
            if (!context)
            {
                CGImageRelease(image);
                return false;
            }
            CGContextDrawImage(context, CGRectMake(0, 0, double(w), double(h)), image);
            CGContextRelease(context);
            CGImageRelease(image);
            for (size_t i = 0; i < w * h; ++i) // back to straight alpha
            {
                uint8_t *p = &rgba[i * 4];
                if (p[3] != 0 && p[3] != 255)
                    for (int c = 0; c < 3; ++c)
                        p[c] = uint8_t(std::min(255u, (uint32_t(p[c]) * 255u + p[3] / 2u) / p[3]));
            }
            texrepl::buildMips(out, uint32_t(w), uint32_t(h), std::move(rgba));
            return true;
        }

        id<MTLTexture> loadReplacement(const texrepl::Key &key, const std::string &path)
        {
            ReplEntry &entry = replCache[key];
            entry.lastUse = ++replClock;
            if (entry.texture)
                return entry.texture;
            if (entry.bytes == ~size_t(0)) // failed before
                return nil;
            texrepl::Image image;
            const bool dds = path.size() > 4 && (path.compare(path.size() - 4, 4, ".dds") == 0 || path.compare(path.size() - 4, 4, ".DDS") == 0);
            if (!(dds ? texrepl::loadDds(path, image) : loadPng(path, image)))
            {
                entry.bytes = ~size_t(0);
                ++replLoadFailures;
                std::fprintf(stderr, "[texrepl] cannot load %s\n", path.c_str());
                return nil;
            }
            const MTLPixelFormat format = image.format == texrepl::Image::BC1 ? MTLPixelFormatBC1_RGBA : image.format == texrepl::Image::BC3 ? MTLPixelFormatBC3_RGBA : MTLPixelFormatRGBA8Unorm;
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:image.levels[0].width height:image.levels[0].height mipmapped:NO];
            descriptor.mipmapLevelCount = image.levels.size();
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
            if (!texture)
            {
                entry.bytes = ~size_t(0);
                return nil;
            }
            for (size_t level = 0; level < image.levels.size(); ++level)
            {
                const texrepl::Level &lv = image.levels[level];
                [texture replaceRegion:MTLRegionMake2D(0, 0, lv.width, lv.height) mipmapLevel:level slice:0 withBytes:image.data.data() + lv.offset bytesPerRow:lv.rowBytes bytesPerImage:0];
            }
            entry.texture = texture;
            entry.bytes = image.data.size();
            replBytes += entry.bytes;
            replacedTextures.insert((__bridge const void *)texture);
            // Over budget: drop the least recently used ones. Whoever still points at an evicted texture (a decoded entry, the
            // content cache) forgets it too, so the "replaced" flag can never outlive the texture it belongs to.
            std::vector<id<MTLTexture>> evicted;
            while (replBytes > replBudget)
            {
                auto oldest = replCache.end();
                for (auto it = replCache.begin(); it != replCache.end(); ++it)
                    if (it->second.texture && it->second.lastUse != replClock && (oldest == replCache.end() || it->second.lastUse < oldest->second.lastUse))
                        oldest = it;
                if (oldest == replCache.end())
                    break;
                replBytes -= oldest->second.bytes;
                evicted.push_back(oldest->second.texture);
                replCache.erase(oldest);
            }
            if (!evicted.empty())
            {
                for (auto &kv : textures)
                    for (id<MTLTexture> gone : evicted)
                        if (kv.second.texture == gone)
                        {
                            kv.second.texture = nil;
                            kv.second.valid = false;
                            kv.second.hash = 0;
                        }
                for (auto it = contentCache.begin(); it != contentCache.end();)
                {
                    bool match = false;
                    for (id<MTLTexture> gone : evicted)
                        match = match || it->second == gone;
                    it = match ? contentCache.erase(it) : std::next(it);
                }
                for (id<MTLTexture> gone : evicted)
                    replacedTextures.erase((__bridge const void *)gone);
            }
            return texture;
        }

        // The replacement for an indexed texture, or nil. `indices` holds one byte per texel for each mip level.
        id<MTLTexture> findReplacement(const std::vector<uint8_t> *indices, uint32_t levelCount, const uint32_t *palette, uint32_t entries, uint64_t tex0)
        {
            ++replLookups;
            texrepl::Key key;
            key.bits = (uint32_t(tex0 >> 20) & 0x3Fu) | ((uint32_t(tex0 >> 26) & 15u) << 6) | ((uint32_t(tex0 >> 30) & 15u) << 10);
            key.clut = XXH3_64bits(palette, size_t(entries) * 4u);
            // PCSX2 hashes the base level and then as many mip levels as the draw it saw used: try every prefix.
            const std::string *path = nullptr;
            uint32_t usedLevels = 0;
            {
                XXH3_state_t state;
                XXH3_64bits_reset(&state);
                for (uint32_t level = 0; level < levelCount && !path; ++level)
                {
                    XXH3_64bits_update(&state, indices[level].data(), indices[level].size());
                    key.tex = XXH3_64bits_digest(&state);
                    path = replPack.find(key);
                    usedLevels = level + 1u;
                }
            }
            const bool withMips = path && usedLevels > 1u;
            if (replLog && replLogged.insert(key).second)
                std::fprintf(stderr, "[texrepl] %s %016llx-%016llx-%08x levels=%u used=%u\n", path ? (withMips ? "hit+mips" : "hit") : "miss", (unsigned long long)key.tex, (unsigned long long)key.clut, key.bits, levelCount, usedLevels);
            if (!path)
            {
                // Diagnostic: PS2X_TEXTURE_DUMP=<dir> writes the indices and palette of what found no replacement.
                if (const char *dump = std::getenv("PS2X_TEXTURE_DUMP"))
                {
                    char name[512];
                    for (uint32_t level = 0; level < levelCount; ++level)
                    {
                        std::snprintf(name, sizeof name, "%s/%016llx-%08x-L%u.bin", dump, (unsigned long long)key.clut, key.bits, level);
                        if (FILE *f = std::fopen(name, "wb"))
                        {
                            std::fwrite(indices[level].data(), 1, indices[level].size(), f);
                            std::fclose(f);
                        }
                    }
                    std::snprintf(name, sizeof name, "%s/%016llx-%08x-palette.bin", dump, (unsigned long long)key.clut, key.bits);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(palette, 4, entries, f);
                        std::fclose(f);
                    }
                }
                return nil;
            }
            id<MTLTexture> texture = loadReplacement(key, *path);
            if (texture)
                ++(withMips ? replHitsMips : replHitsBase);
            return texture;
        }

        // The texture TEX0 names, decoded from the CPU copy of VRAM into RGBA (palette applied).
        id<MTLTexture> texture(const gsn::Context &ctx, uint64_t texa)
        {
            const uint64_t tex0 = ctx.tex0;
            // Mip levels live at their own addresses (MIPTBP1/2), each half the size of the one above.
            struct Level { uint32_t tbp, tbw, width, height; };
            Level levels[7];
            uint32_t levelCount = 1;
            {
                const uint32_t wanted = std::min<uint32_t>(uint32_t(ctx.tex1 >> 2) & 7u, 6u);
                const uint32_t w0 = 1u << std::min<uint32_t>(uint32_t(tex0 >> 26) & 15u, 10u), h0 = 1u << std::min<uint32_t>(uint32_t(tex0 >> 30) & 15u, 10u);
                for (uint32_t level = 1; level <= wanted && (w0 >> level) != 0u && (h0 >> level) != 0u; ++level)
                {
                    const uint64_t reg = level <= 3u ? ctx.miptbp1 : ctx.miptbp2;
                    const uint32_t shift = ((level - 1u) % 3u) * 20u;
                    levels[level] = {uint32_t(reg >> shift) & 0x3FFFu, std::max<uint32_t>(uint32_t(reg >> (shift + 14u)) & 0x3Fu, 1u), w0 >> level, h0 >> level};
                    levelCount = level + 1u;
                }
            }
            const uint32_t tbp = uint32_t(tex0) & 0x3FFFu, tbw = std::max<uint32_t>(uint32_t(tex0 >> 14) & 0x3Fu, 1u), psm = uint32_t(tex0 >> 20) & 0x3Fu;
            const uint32_t width = 1u << std::min<uint32_t>(uint32_t(tex0 >> 26) & 15u, 10u), height = 1u << std::min<uint32_t>(uint32_t(tex0 >> 30) & 15u, 10u);
            const uint32_t cbp = uint32_t(tex0 >> 37) & 0x3FFFu, cpsm = uint32_t(tex0 >> 51) & 15u;
            const bool paletted = isPaletted(psm);
            if (isDepthFormat(psm))
                return fallbackTexture;
            // Everything that decides the pixels, except their bytes: address, format, size, palette, TEXA.
            uint64_t key = ((tex0 & 0x3FFFFFFFFull) | (paletted ? tex0 & (0x1FFFFFFull << 37) : 0u)) ^ (texa * 0x9E3779B97F4A7C15ull);
            for (uint32_t level = 1; level < levelCount; ++level)
                key = (key ^ (uint64_t(levels[level].tbp) | (uint64_t(levels[level].tbw) << 14) | (uint64_t(level) << 20))) * 0x100000001b3ull;
            levels[0] = {tbp, tbw, width, height};
            TextureEntry &entry = textures[key];
            // Valid until a transfer changes one of its blocks or a target draws over one: both are told to the entry
            // through the per-block user lists, so this test costs nothing per lookup.
            if (entry.texture && entry.valid && !entry.drawnOver)
                return entry.texture;
            if (entry.drawnOver || !entry.registered)
            {
                // What a target drew over this texture's memory has to come back to VRAM first.
                std::bitset<kVramBytes / 256u> needed;
                if (entry.registered)
                    for (const uint16_t block : entry.pages)
                        needed.set(block);
                else
                {
                    if (paletted)
                        for (uint32_t block = cbp; block < cbp + (psm == 0x13u || psm == 0x1Bu ? 4u : 1u) && block < kVramBytes / 256u; ++block)
                            needed.set(block);
                    const gsn::Swizzle &where = gsn::Swizzle::of(psm);
                    for (uint32_t level = 0; level < levelCount; ++level)
                        for (uint32_t y = 0; y < levels[level].height; y += 8u)
                            for (uint32_t x = 0; x < levels[level].width; x += 8u)
                                needed.set(where.bitAddress(levels[level].tbp, levels[level].tbw, x, y) >> 11);
                }
                syncReason = tex0;
                syncFromGpu(needed);
                entry.drawnOver = false;
                if (entry.texture && entry.valid)
                    return entry.texture;
            }
            const uint32_t ta0 = uint32_t(texa) & 0xFFu, ta1 = uint32_t(texa >> 32) & 0xFFu;
            const bool aem = ((texa >> 15) & 1u) != 0u;
            uint32_t palette[256];
            uint64_t paletteHash = 0xcbf29ce484222325ull;
            const gsn::Swizzle &layout = gsn::Swizzle::of(psm);
            if (paletted)
            {
                const gsn::Swizzle &clut = gsn::Swizzle::of(cpsm);
                const uint32_t entries = psm == 0x13u || psm == 0x1Bu ? 256u : 16u;
                for (uint32_t i = 0; i < entries; ++i)
                {
                    // CSM1 layout: 16x16 with the middle index bits swapped, or 8x2.
                    const uint32_t at = entries == 256u ? (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1) : i;
                    const uint32_t x = entries == 256u ? at & 15u : at & 7u, y = entries == 256u ? at >> 4 : at >> 3;
                    palette[i] = expand(clut.read(vram.data(), cbp, 1u, x, y), cpsm, ta0, ta1, aem);
                    paletteHash = (paletteHash ^ palette[i]) * 0x100000001b3ull;
                }
            }
            if (!entry.registered)
            {
                // The key fixes the address, format and size, so the blocks of an entry never change: listed once,
                // level by level (the first level also carries the palette's blocks).
                std::bitset<kVramBytes / 256u> used;
                if (paletted)
                {
                    const gsn::Swizzle &clut = gsn::Swizzle::of(cpsm);
                    for (uint32_t y = 0; y < (psm == 0x13u || psm == 0x1Bu ? 16u : 2u); y += 8u)
                        for (uint32_t x = 0; x < (psm == 0x13u || psm == 0x1Bu ? 16u : 8u); x += 8u)
                            used.set(clut.bitAddress(cbp, 1u, x, y) >> 11);
                }
                std::bitset<kVramBytes / 256u> all = used;
                entry.pages.clear();
                entry.mipBlocks.clear();
                for (uint32_t level = 0; level < levelCount; ++level)
                {
                    std::bitset<kVramBytes / 256u> mine;
                    for (uint32_t y = 0; y < levels[level].height; y += 8u)
                        for (uint32_t x = 0; x < levels[level].width; x += 8u)
                            mine.set(layout.bitAddress(levels[level].tbp, levels[level].tbw, x, y) >> 11);
                    all |= mine;
                    entry.mipStart[level] = uint32_t(entry.mipBlocks.size());
                    if (level != 0u)
                        for (uint32_t block = 0; block < kVramBytes / 256u; ++block)
                            if (mine.test(block))
                                entry.mipBlocks.push_back(uint16_t(block));
                }
                entry.mipStart[levelCount] = uint32_t(entry.mipBlocks.size());
                for (uint32_t block = 0; block < kVramBytes / 256u; ++block)
                    if (all.test(block))
                    {
                        entry.pages.push_back(uint16_t(block));
                        blockUsers[block].push_back(&entry);
                    }
                entry.registered = true;
            }
            entry.valid = true;

            // The same bytes as an earlier upload, in the same format: the texture decoded then.
            const auto record = uploads.find(tbp);
            bool known = record != uploads.end() && generationSum(record->second.blocks) == record->second.blockSum;
            if (record == uploads.end())
                ++unknownNoUpload;
            else if (!known)
                ++unknownChanged;
            const bool knownBase = known;
            // Mip levels may have been sent on their own: each one is identified by the upload that covers it.
            uint64_t mipHash = 0;
            for (uint32_t level = 1; level < levelCount && known; ++level)
            {
                const uint32_t first = entry.mipStart[level], last = entry.mipStart[level + 1u];
                const uint32_t owner = first != last ? blockOwner[entry.mipBlocks[first]] : 0u;
                const auto from = owner != 0u ? uploads.find(owner - 1u) : uploads.end();
                known = from != uploads.end() && generationSum(from->second.blocks) == from->second.blockSum;
                for (uint32_t i = first; i < last && known; ++i)
                    known = blockOwner[entry.mipBlocks[i]] == owner;
                if (known)
                    mipHash = (mipHash ^ from->second.hash ^ uint64_t(levels[level].tbp - (owner - 1u))) * 0x100000001b3ull;
            }
            if (knownBase && !known)
                ++unknownMips;
            const uint64_t contentKey = known ? (record->second.hash ^ (paletteHash * 0x9E3779B97F4A7C15ull) ^ ((tex0 >> 14) & 0xFFFFFull) * 0xD6E8FEB86659FD93ull ^ (texa << 1) ^ (uint64_t(levelCount) << 60) ^ mipHash) : 0u;
            if (known)
            {
                const auto hit = contentCache.find(contentKey);
                if (hit != contentCache.end())
                {
                    entry.texture = hit->second;
                    entry.hash = ~0ull;
                    ++contentHits;
                    return entry.texture;
                }
            }

            initReplacements();
            const bool wantIndices = paletted && replPack.size() != 0;
            std::vector<uint8_t> indices[7];
            const uint32_t indexMask = psm == 0x13u || psm == 0x1Bu ? 0xFFu : 0x0Fu;
            std::vector<uint32_t> pixels[7];
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint32_t level = 0; level < levelCount; ++level)
            {
                const Level &from = levels[level];
                pixels[level].resize(size_t(from.width) * from.height);
                if (wantIndices)
                    indices[level].resize(size_t(from.width) * from.height);
                for (uint32_t y = 0; y < from.height; ++y)
                    for (uint32_t x = 0; x < from.width; ++x)
                    {
                        const uint32_t value = layout.read(vram.data(), from.tbp, from.tbw, x, y);
                        const uint32_t pixel = paletted ? palette[value & 0xFFu] : expand(value, psm, ta0, ta1, aem);
                        pixels[level][size_t(y) * from.width + x] = pixel;
                        if (wantIndices)
                            indices[level][size_t(y) * from.width + x] = uint8_t(value & indexMask);
                        hash = (hash ^ pixel) * 0x100000001b3ull;
                    }
            }
            ++decodes;
            if (wantIndices && (psm == 0x13u || psm == 0x14u))
            {
                // PCSX2 hashes plain 8- and 4-bit textures as the raw 256-byte GS blocks, row of blocks by row of blocks (a level
                // smaller than a block is hashed as expanded indices instead): the bytes a decoder never sees unswizzled.
                const uint32_t blockW = psm == 0x13u ? 16u : 32u, blockH = 16u;
                for (uint32_t level = 0; level < levelCount; ++level)
                {
                    const Level &from = levels[level];
                    if (from.width < blockW || from.height < blockH)
                        continue;
                    std::vector<uint8_t> &raw = indices[level];
                    raw.clear();
                    raw.reserve(size_t(from.width / blockW) * (from.height / blockH) * 256u);
                    for (uint32_t by = 0; by < from.height / blockH; ++by)
                        for (uint32_t bx = 0; bx < from.width / blockW; ++bx)
                        {
                            const uint8_t *block = vram.data() + (layout.bitAddress(from.tbp, from.tbw, bx * blockW, by * blockH) >> 3);
                            raw.insert(raw.end(), block, block + 256);
                        }
                }
            }
            if (wantIndices)
            {
                if (id<MTLTexture> replacement = findReplacement(indices, levelCount, palette, psm == 0x13u || psm == 0x1Bu ? 256u : 16u, tex0))
                {
                    entry.texture = replacement;
                    entry.hash = ~0ull;
                    if (known)
                        contentCache[contentKey] = replacement;
                    return replacement;
                }
            }
            if (!entry.texture || entry.hash != hash)
            {
                // A texture a queued draw may still sample is never rewritten: a new one takes its place.
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
                descriptor.mipmapLevelCount = levelCount;
                entry.texture = [device newTextureWithDescriptor:descriptor];
                for (uint32_t level = 0; level < levelCount; ++level)
                    [entry.texture replaceRegion:MTLRegionMake2D(0, 0, levels[level].width, levels[level].height) mipmapLevel:level withBytes:pixels[level].data() bytesPerRow:levels[level].width * 4u];
                entry.hash = hash;
            }
            if (known)
                contentCache[contentKey] = entry.texture;
            return entry.texture;
        }

        // The 256 palette entries TEX0 names (CSM1), as a 256x1 texture.
        // The 256 palette entries TEX0 names (CSM1), as a texture in the layout they have in VRAM (16x16). A palette the
        // game drew on the GPU is copied from the target inside the command stream, with no trip through the CPU.
        id<MTLTexture> palette(uint64_t tex0, uint64_t texa, uint32_t &texelsPerEntry)
        {
            const uint32_t cbp = uint32_t(tex0 >> 37) & 0x3FFFu, cpsm = uint32_t(tex0 >> 51) & 15u;
            const uint32_t ta0 = uint32_t(texa) & 0xFFu, ta1 = uint32_t(texa >> 32) & 0xFFu;
            const bool aem = ((texa >> 15) & 1u) != 0u;
            texelsPerEntry = 1;
            if (cbp < kVramBytes / 256u && gpuDirtyBlocks.test(cbp))
                for (auto &entry : targets)
                {
                    Target &target = entry.second;
                    if (!target.gpuDirty.test(cbp) || (target.psm != 0u && target.psm != 1u))
                        continue;
                    const gsn::Swizzle &layout = gsn::Swizzle::of(target.psm);
                    for (uint32_t y = 0; y + 16u <= kTargetHeight; y += 8u)
                        for (uint32_t x = 0; x + 16u <= target.fbw * 64u; x += 8u)
                        {
                            if ((layout.bitAddress(target.fbp * 32u, target.fbw, x, y) >> 11) != cbp)
                                continue;
                            const uint32_t scale = scaleOf(target.fbw);
                            id<MTLTexture> __strong &copy = drawnPalettes[cbp];
                            if (!copy || copy.width != 16u * scale)
                            {
                                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16u * scale height:16u * scale mipmapped:NO];
                                descriptor.storageMode = MTLStorageModePrivate;
                                copy = [device newTextureWithDescriptor:descriptor];
                            }
                            endPass();
                            ensureCommandBuffer();
                            id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                            [blit copyFromTexture:target.color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x * scale, y * scale, 0) sourceSize:MTLSizeMake(16u * scale, 16u * scale, 1)
                                        toTexture:copy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                            [blit endEncoding];
                            texelsPerEntry = scale;
                            ++drawnPaletteCopies;
                            return copy;
                        }
                }
            uint32_t entries[256];
            uint64_t hash = 0xcbf29ce484222325ull;
            const gsn::Swizzle &layout = gsn::Swizzle::of(cpsm);
            for (uint32_t i = 0; i < 256u; ++i)
            {
                entries[i] = expand(layout.read(vram.data(), cbp, 1u, i & 15u, i >> 4), cpsm, ta0, ta1, aem);
                hash = (hash ^ entries[i]) * 0x100000001b3ull;
            }
            auto &entry = palettes[uint64_t(cbp) | (uint64_t(cpsm) << 16) | ((texa * 0x9E3779B97F4A7C15ull) << 20)];
            if (!entry.first || entry.second != hash)
            {
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
                entry.first = [device newTextureWithDescriptor:descriptor];
                [entry.first replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:entries bytesPerRow:64];
                entry.second = hash;
            }
            return entry.first;
        }

        void shuffle()
        {
            const gsn::Context &ctx = batch.ctx;
            const uint32_t fbp = uint32_t(ctx.frame) & 0x1FFu, fbw = uint32_t(ctx.frame >> 16) & 0x3Fu, fbmask = uint32_t(ctx.frame >> 32);
            const uint32_t tbp = uint32_t(ctx.tex0) & 0x3FFFu, tpsm = uint32_t(ctx.tex0 >> 20) & 0x3Fu;
            if (logging())
                std::fprintf(stderr, "[shuffle] source half %u -> destination half %u, texa=%016llx fba=%llu test=%05x tex0=%016llx\n", batch.shuffleSource, batch.shuffleDestination,
                             (unsigned long long)batch.texa, (unsigned long long)(ctx.fba & 1u), uint32_t(ctx.test) & 0x7FFFFu, (unsigned long long)ctx.tex0);
            Target &destination = targets[targetKey(fbp, fbw)];
            refreshTarget(destination, destination.psm);
            id<MTLTexture> shuffleDepth = isDepthFormat(tpsm) ? depth(tbp / 32u, fbw).texture : nil;
            endPass();
            ensureCommandBuffer();
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = destination.color;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> quad = [commandBuffer renderCommandEncoderWithDescriptor:pass];
            ShuffleUniforms u{};
            u.sourceHalf = batch.shuffleSource;
            u.destinationHalf = batch.shuffleDestination;
            // The mask is given for 8-bit channels; a 16-bit pixel keeps the top 5 bits of each and the top bit of alpha.
            u.mask16 = ((fbmask >> 3) & 0x1Fu) | (((fbmask >> 11) & 0x1Fu) << 5) | (((fbmask >> 19) & 0x1Fu) << 10) | (((fbmask >> 31) & 1u) << 15);
            u.sourceIsDepth = isDepthFormat(tpsm) ? 1u : 0u;
            // Two rows of the 16-bit view are one row of the target.
            const uint32_t sx0 = uint32_t(ctx.scissor) & 0x7FFu, sx1 = uint32_t(ctx.scissor >> 16) & 0x7FFu, sy0 = uint32_t(ctx.scissor >> 32) & 0x7FFu, sy1 = uint32_t(ctx.scissor >> 48) & 0x7FFu;
            const uint32_t x0 = std::min(sx0, fbw * 64u), x1 = std::min(sx1 + 1u, fbw * 64u), y0 = std::min(sy0 / 2u, kTargetHeight), y1 = std::min((sy1 + 1u) / 2u, kTargetHeight);
            if (x1 > x0 && y1 > y0)
            {
                [quad setRenderPipelineState:shufflePipeline];
                const uint32_t scale = scaleOf(fbw);
                [quad setScissorRect:(MTLScissorRect){x0 * scale, y0 * scale, (x1 - x0) * scale, (y1 - y0) * scale}];
                [quad setFragmentTexture:shuffleDepth atIndex:0];
                [quad setFragmentBytes:&u length:sizeof(u) atIndex:1];
                [quad drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                ++drawCalls;
            }
            [quad endEncoding];
        }

        void depthCopy()
        {
            const gsn::Context &ctx = batch.ctx;
            const uint32_t fbp = uint32_t(ctx.frame) & 0x1FFu, fbw = uint32_t(ctx.frame >> 16) & 0x3Fu;
            const uint32_t tbp = uint32_t(ctx.tex0) & 0x3FFFu, tbw = uint32_t(ctx.tex0 >> 14) & 0x3Fu;
            DepthTarget &source = depth(tbp / 32u, tbw);
            DepthTarget &destination = depth(fbp, fbw);
            if (source.texture == destination.texture)
                return;
            // ZBUF is written too (always passing, not masked): whatever colour target lives there receives the sprite's Z.
            const uint32_t zbp = uint32_t(ctx.zbuf) & 0x1FFu;
            Target *cleared = ((ctx.zbuf >> 32) & 1u) == 0u && zbp != fbp ? &target(zbp, fbw, 0u) : nullptr;
            if (cleared)
            {
                // The rows about to be written must hold their VRAM contents first, or a later refresh would bring
                // the stale CPU copy back over what is drawn now.
                cleared->usedRows = std::max(cleared->usedRows, std::min(uint32_t(std::max(batch.maxY, 0.0f)) + 1u, kTargetHeight));
                refreshTarget(*cleared, cleared->psm);
                cleared->drawn = true;
                // Only what the sprites cover: the game keeps palettes in the unused strip beside these small targets.
                markDrawn(*cleared, 0u, 0u, std::min(uint32_t(std::max(batch.maxX, 0.0f)), fbw * 64u), std::min(uint32_t(std::max(batch.maxY, 0.0f)), kTargetHeight));
            }
            endPass();
            ensureCommandBuffer();
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            if (cleared)
            {
                pass.colorAttachments[0].texture = cleared->color;
                pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            }
            pass.depthAttachment.texture = destination.texture;
            pass.depthAttachment.loadAction = destination.fresh ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.depthAttachment.clearDepth = 0.0;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            destination.fresh = false;
            id<MTLRenderCommandEncoder> copy = [commandBuffer renderCommandEncoderWithDescriptor:pass];
            Uniforms u{};
            u.targetWidth = float(fbw * 64u);
            u.targetHeight = float(kTargetHeight);
            u.texScaleU = float(1u << (uint32_t(ctx.tex0 >> 26) & 15u)) / float(tbw * 64u);
            u.texScaleV = float(1u << (uint32_t(ctx.tex0 >> 30) & 15u)) / float(kTargetHeight);
            [copy setRenderPipelineState:cleared ? depthCopyClearPipeline : depthCopyPipeline];
            [copy setDepthStencilState:depthState(false, false, true)];
            [copy setVertexBuffer:chunk offset:0 atIndex:0];
            [copy setVertexBytes:&u length:sizeof(u) atIndex:1];
            [copy setFragmentBytes:&u length:sizeof(u) atIndex:1];
            [copy setFragmentTexture:source.texture atIndex:0];
            [copy setFragmentSamplerState:sampler(false, 1, 1) atIndex:0];
            [copy drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:batch.firstVertex vertexCount:batch.vertexCount];
            [copy endEncoding];
            ++drawCalls;
        }

        // Something was drawn to these pixels of a target: those blocks are now newer on the GPU than in VRAM.
        void markDrawn(Target &target, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
        {
            if (x1 <= x0 || y1 <= y0)
                return;
            const uint64_t rect = uint64_t(x0) | (uint64_t(y0) << 12) | (uint64_t(x1) << 24) | (uint64_t(y1) << 36);
            if (rect == target.markedRect)
                return;
            target.markedRect = rect;
            const gsn::Swizzle &layout = gsn::Swizzle::of(target.psm);
            for (uint32_t y = y0 & ~7u; y < y1; y += 8u)
                for (uint32_t x = x0 & ~7u; x < x1; x += 8u)
                {
                    const uint32_t block = layout.bitAddress(target.fbp * 32u, target.fbw, x, y) >> 11;
                    if (!gpuDirtyBlocks.test(block))
                        for (TextureEntry *user : blockUsers[block])
                            user->drawnOver = true;
                    target.gpuDirty.set(block);
                    gpuDirtyBlocks.set(block);
                }
        }

        // The game also draws things it later reads as plain memory (palettes drawn with triangles, hidden in the unused
        // strip beside a small target, for one). Before VRAM is decoded, the blocks a target drew to come back from the GPU.
        // It waits for the GPU, so it only happens for blocks that are both drawn to and read as memory.
        void syncFromGpu(const std::bitset<kVramBytes / 256u> &needed)
        {
            if ((needed & gpuDirtyBlocks).none())
                return;
            flush(true);
            std::vector<uint16_t> changed;
            std::vector<uint32_t> pixels;
            for (auto &entry : targets)
            {
                Target &target = entry.second;
                if ((target.gpuDirty & needed).none())
                    continue;
                target.markedRect = ~0ull;
                const bool narrow = target.psm == 2u || target.psm == 0xAu;
                const uint32_t scale = scaleOf(target.fbw);
                const uint32_t pageHeight = narrow ? 64u : 32u, stride = 64u * scale, rows = pageHeight * scale;
                const gsn::Swizzle &layout = gsn::Swizzle::of(target.psm);
                for (uint32_t page = 0; page < target.fbw * (kTargetHeight / pageHeight) && target.fbp + page < kPages; ++page)
                {
                    const uint32_t x0 = (page % target.fbw) * 64u, y0 = (page / target.fbw) * pageHeight;
                    bool fetched = false;
                    for (uint32_t y = 0; y < pageHeight; y += 8u)
                        for (uint32_t x = 0; x < 64u; x += 8u)
                        {
                            const uint32_t block = layout.bitAddress(target.fbp * 32u, target.fbw, x0 + x, y0 + y) >> 11;
                            if (!target.gpuDirty.test(block) || !needed.test(block))
                                continue;
                            if (!fetched)
                            {
                                pixels.resize(size_t(stride) * rows);
                                [target.color getBytes:pixels.data() bytesPerRow:stride * 4u fromRegion:MTLRegionMake2D(x0 * scale, y0 * scale, stride, rows) mipmapLevel:0];
                                fetched = true;
                                ++readbacks;
                                if (std::getenv("PS2X_GS_NATIVE_READBACK_LOG"))
                                    std::fprintf(stderr, "[readback] scanout %llu target fbp=%u fbw=%u page %u for tex0=%016llx\n", (unsigned long long)scanouts, target.fbp, target.fbw, target.fbp + page, (unsigned long long)syncReason);
                            }
                            bool different = false;
                            for (uint32_t v = y; v < y + 8u; ++v)
                                for (uint32_t h = x; h < x + 8u; ++h)
                                {
                                    uint32_t value = pixels[size_t(v * scale) * stride + h * scale];
                                    if (narrow)
                                        value = ((value >> 3) & 0x1Fu) | (((value >> 11) & 0x1Fu) << 5) | (((value >> 19) & 0x1Fu) << 10) | ((value >> 31) << 15);
                                    different = layout.write(vram.data(), target.fbp * 32u, target.fbw, x0 + h, y0 + v, value) || different;
                                }
                            if (different && (changed.empty() || changed.back() != block))
                                changed.push_back(uint16_t(block));
                        }
                }
                target.gpuDirty &= ~needed;
            }
            gpuDirtyBlocks.reset();
            for (auto &entry : targets)
                gpuDirtyBlocks |= entry.second.gpuDirty;
            if (!changed.empty())
            {
                // Decoded textures and palettes over these blocks are out of date; the targets themselves are not.
                ++vramGeneration;
                for (const uint16_t block : changed)
                {
                    ++blockGeneration[block];
                    for (TextureEntry *user : blockUsers[block])
                        user->valid = false;
                }
            }
        }

        void closeBatch()
        {
            if (!batchOpen)
                return;
            batchOpen = false;
            // PS2X_GS_NATIVE_LOG=<scanout>: one line per batch drawn while that scanout is being built.
            if (logging() && !splitting && (batch.vertexCount != 0u || batch.skip))
            {
                const gsn::Context &c = batch.ctx;
                std::fprintf(stderr, "[draw] %s fb=%u/%u/%02x msk=%08x z=%u/%02x%s test=%05x sc=%u-%u,%u-%u %s%s", batch.skip ? "SKIP" : "    ", uint32_t(c.frame) & 0x1FFu, uint32_t(c.frame >> 16) & 0x3Fu,
                             uint32_t(c.frame >> 24) & 0x3Fu, uint32_t(c.frame >> 32), uint32_t(c.zbuf) & 0x1FFu, uint32_t(c.zbuf >> 24) & 0xFu, ((c.zbuf >> 32) & 1u) ? "m" : "", uint32_t(c.test) & 0x7FFFFu,
                             uint32_t(c.scissor) & 0x7FFu, uint32_t(c.scissor >> 16) & 0x7FFu, uint32_t(c.scissor >> 32) & 0x7FFu, uint32_t(c.scissor >> 48) & 0x7FFu,
                             (batch.mode & 7u) == 6u ? "sprite" : "tri", (batch.mode & 0x100u) ? " fst" : "");
                if (batch.mode & 0x40u)
                    std::fprintf(stderr, " blend=%u%u%u%u/%02x", uint32_t(c.alpha) & 3u, uint32_t(c.alpha >> 2) & 3u, uint32_t(c.alpha >> 4) & 3u, uint32_t(c.alpha >> 6) & 3u, uint32_t(c.alpha >> 32) & 0xFFu);
                if (batch.textured)
                    std::fprintf(stderr, " tex=%u/%u/%02x %ux%u tfx=%u tcc=%u cbp=%u clamp=%x tex1=%x", uint32_t(c.tex0) & 0x3FFFu, uint32_t(c.tex0 >> 14) & 0x3Fu, uint32_t(c.tex0 >> 20) & 0x3Fu, 1u << (uint32_t(c.tex0 >> 26) & 15u),
                                 1u << (uint32_t(c.tex0 >> 30) & 15u), uint32_t(c.tex0 >> 35) & 3u, uint32_t(c.tex0 >> 34) & 1u, uint32_t(c.tex0 >> 37) & 0x3FFFu, uint32_t(c.clamp) & 15u, uint32_t(c.tex1) & 0x1FFu);
                // Vertex colour range (RGBA bytes; 0x80 is 1.0 when modulating a texture) and the screen box, to find the
                // primitives behind a visible defect.
                if (chunk && batch.vertexCount)
                {
                    const GpuVertex *v = static_cast<const GpuVertex *>(chunk.contents) + batch.firstVertex;
                    uint32_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    for (uint32_t i = 0; i < batch.vertexCount; ++i)
                    {
                        for (uint32_t c = 0; c < 4u; ++c)
                        {
                            const uint32_t b = (v[i].rgba >> (c * 8u)) & 0xFFu;
                            lo[c] = std::min(lo[c], b);
                            hi[c] = std::max(hi[c], b);
                        }
                        x0 = std::min(x0, v[i].x); y0 = std::min(y0, v[i].y); x1 = std::max(x1, v[i].x); y1 = std::max(y1, v[i].y);
                    }
                    std::fprintf(stderr, " rgba=%02x%02x%02x%02x-%02x%02x%02x%02x box=%.0f,%.0f-%.0f,%.0f", lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3], x0, y0, x1, y1);
                }
                std::fprintf(stderr, " verts=%u\n", batch.vertexCount);
            }
            if (batch.shuffle && batch.primitives != 0u)
            {
                shuffle();
                return;
            }
            if (batch.skip || batch.vertexCount == 0u)
                return;
            if (batch.depthCopy)
            {
                depthCopy();
                return;
            }
            const gsn::Context &ctx = batch.ctx;
            const uint32_t fbp = uint32_t(ctx.frame) & 0x1FFu, fbw = uint32_t(ctx.frame >> 16) & 0x3Fu, psm = uint32_t(ctx.frame >> 24) & 0x3Fu;
            const uint32_t zbp = uint32_t(ctx.zbuf) & 0x1FFu;
            const bool zmask = ((ctx.zbuf >> 32) & 1u) != 0u;
            const uint32_t test = uint32_t(ctx.test);

            // A batch that reads the target it draws to. On the GS every primitive sees what the ones before it
            // drew, and Black counts on it: it blurs its glow buffer with twenty sprites in a row, each reading what
            // the one before left. With one copy of the target for the whole batch nothing accumulated (the glow
            // came out sharp, with streaks). Such a batch goes one primitive at a time, each with a fresh copy.
            if (batch.textured && !splitting)
            {
                const uint32_t tbp = uint32_t(ctx.tex0) & 0x3FFFu, tbw = uint32_t(ctx.tex0 >> 14) & 0x3Fu, tpsm = uint32_t(ctx.tex0 >> 20) & 0x3Fu;
                const uint32_t kind = batch.mode & 7u, step = kind == 6u ? 6u : kind == 0u ? 1u : kind <= 2u ? 2u : 3u;
                if ((tbp & 31u) == 0u && tbp / 32u == fbp && tbw == fbw && (tpsm == 0u || tpsm == 1u || tpsm == 0x1Bu) && batch.vertexCount > step && chunk)
                {
                    // Only where it matters: a primitive starts a new group (and a new copy) when the texels it reads
                    // include pixels that an earlier primitive of the group has drawn, judged as the GS would see it
                    // (at the game's own resolution). The blur's sprites each read their neighbour's output, so they
                    // go one by one; the twenty tiles that re-read the main frame in place each read only their own
                    // pixels, and stay one group with one copy (forty copies of the whole frame per scanout otherwise).
                    const GpuVertex *vertices = static_cast<const GpuVertex *>(chunk.contents);
                    const float tw = float(1u << (uint32_t(ctx.tex0 >> 26) & 15u)), th = float(1u << (uint32_t(ctx.tex0 >> 30) & 15u));
                    const bool linear = ((ctx.tex1 >> 5) & 1u) != 0u;
                    const float shift = kind == 6u && scaleOf(fbw) > 1u ? 0.5f : 0.0f; // sprite() moved it half a pixel
                    struct Box { int x0, y0, x1, y1; };
                    std::vector<Box> drawn;
                    bool identity = kind == 6u && !noSelfFetch; // every sprite maps each pixel onto itself
                    bool fetch = false;
                    const Batch whole = batch;
                    const uint32_t end = whole.firstVertex + whole.vertexCount;
                    uint32_t groupFirst = whole.firstVertex;
                    splitting = true;
                    const auto flushGroup = [&](uint32_t until)
                    {
                        if (until == groupFirst)
                            return;
                        batch = whole;
                        batch.firstVertex = groupFirst;
                        batch.vertexCount = until - groupFirst;
                        batch.selfFetch = fetch;
                        batchOpen = true;
                        closeBatch();
                        groupFirst = until;
                        drawn.clear();
                    };
                    for (uint32_t first = whole.firstVertex; first + step <= end; first += step)
                    {
                        Box target{}, source{};
                        if (kind == 6u)
                        {
                            const GpuVertex &a = vertices[first], &b = vertices[first + 5u];
                            const float x0 = a.x + shift, x1 = b.x + shift, y0 = a.y + shift, y1 = b.y + shift;
                            target = {int(std::ceil(x0)), int(std::ceil(y0)), int(std::ceil(x1)) - 1, int(std::ceil(y1)) - 1};
                            // Texel coordinates at the first and last pixel the GS samples.
                            const float u0 = a.s / a.q * tw, u1 = b.s / b.q * tw, v0 = a.t / a.q * th, v1 = b.t / b.q * th;
                            const float du = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0.0f, dv = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0.0f;
                            // (u0, v0) belong to the vertex as it was emitted, half a pixel off when the target is enlarged.
                            float ua = u0 + (float(target.x0) - a.x) * du, ub = u0 + (float(target.x1) - a.x) * du;
                            float va = v0 + (float(target.y0) - a.y) * dv, vb = v0 + (float(target.y1) - a.y) * dv;
                            if (ua > ub) std::swap(ua, ub);
                            if (va > vb) std::swap(va, vb);
                            identity = identity && std::fabs(du - 1.0f) < 1e-4f && std::fabs(dv - 1.0f) < 1e-4f &&
                                       std::fabs(ua - (float(target.x0) + 0.5f)) < 1e-3f && std::fabs(va - (float(target.y0) + 0.5f)) < 1e-3f;
                            if (linear)
                                source = {int(std::floor(ua - 0.5f)), int(std::floor(va - 0.5f)), int(std::ceil(ub - 0.5f)), int(std::ceil(vb - 0.5f))};
                            else
                                source = {int(std::floor(ua)), int(std::floor(va)), int(std::floor(ub)), int(std::floor(vb))};
                        }
                        else
                        {
                            identity = false;
                            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                            for (uint32_t k = 0; k < step; ++k)
                            {
                                const GpuVertex &v = vertices[first + k];
                                x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
                                const float u = v.s / v.q * tw, w = v.t / v.q * th;
                                u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, w); v1 = std::max(v1, w);
                            }
                            target = {int(std::floor(x0)) - 1, int(std::floor(y0)) - 1, int(std::ceil(x1)) + 1, int(std::ceil(y1)) + 1};
                            source = {int(std::floor(u0)) - 1, int(std::floor(v0)) - 1, int(std::ceil(u1)) + 1, int(std::ceil(v1)) + 1};
                        }
                        bool reads = false;
                        for (const Box &box : drawn)
                            reads = reads || (source.x0 <= box.x1 && source.x1 >= box.x0 && source.y0 <= box.y1 && source.y1 >= box.y0);
                        if (reads)
                            flushGroup(first);
                        drawn.push_back(target);
                    }
                    fetch = identity && groupFirst == whole.firstVertex; // one group of in-place reads: no copy at all
                    flushGroup(end);
                    splitting = false;
                    batch = whole;
                    return;
                }
            }

            // Textures and stale target pages first: both may have to end the current pass.
            Target &destination = target(fbp, fbw, psm);
            {
                const uint32_t bottom = std::min({(uint32_t(ctx.scissor >> 48) & 0x7FFu) + 1u, uint32_t(std::max(batch.maxY, 0.0f)) + 1u, kTargetHeight});
                destination.usedRows = std::max(destination.usedRows, bottom);
            }
            refreshTarget(destination, psm);
            id<MTLTexture> source = fallbackTexture;
            float texScaleU = 1.0f, texScaleV = 1.0f;
            bool texAlphaFixed = false;
            id<MTLTexture> indexPalette = nil;
            uint32_t paletteScale = 1, texBox = 1;
            bool targetSource = false;
            if (batch.textured)
            {
                const uint32_t tbp = uint32_t(ctx.tex0) & 0x3FFFu, tbw = uint32_t(ctx.tex0 >> 14) & 0x3Fu, tpsm = uint32_t(ctx.tex0 >> 20) & 0x3Fu;
                const auto found = (tbp & 31u) == 0u && (tpsm == 0u || tpsm == 1u || tpsm == 0x1Bu) ? targets.find(targetKey(tbp / 32u, tbw)) : targets.end();
                if (found != targets.end() && found->second.drawn && (found->second.psm == 0u || found->second.psm == 1u))
                {
                    // The texture is something that was drawn: sample the target itself.
                    Target &from = found->second;
                    refreshTarget(from, from.psm);
                    source = from.color;
                    if (&from == &destination && batch.selfFetch)
                        source = fallbackTexture; // the shader takes the texel from the pixel it is drawing
                    else if (&from == &destination)
                    {
                        // A pass cannot sample what it draws to: a copy of the target as it is now.
                        endPass();
                        ensureCommandBuffer();
                        if (!from.copy)
                        {
                            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:from.color.width height:from.color.height mipmapped:NO];
                            descriptor.storageMode = MTLStorageModePrivate;
                            from.copy = [device newTextureWithDescriptor:descriptor];
                        }
                        // Only the rows in use: this now happens once per primitive of such a batch.
                        const NSUInteger rows = std::min<NSUInteger>(from.color.height, NSUInteger(std::max(from.usedRows, 1u)) * scaleOf(from.fbw));
                        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                        [blit copyFromTexture:from.color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(from.color.width, rows, 1)
                                    toTexture:from.copy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [blit endEncoding];
                        source = from.copy;
                        ++selfSamples;
                    }
                    texScaleU = float(1u << (uint32_t(ctx.tex0 >> 26) & 15u)) / float(from.fbw * 64u);
                    texScaleV = float(1u << (uint32_t(ctx.tex0 >> 30) & 15u)) / float(kTargetHeight);
                    texAlphaFixed = tpsm == 1u;
                    texBox = scaleOf(from.fbw);
                    targetSource = true;
                    if (tpsm == 0x1Bu)
                        indexPalette = palette(ctx.tex0, batch.texa, paletteScale);
                    ++targetSamples;
                }
                else
                    source = texture(ctx, batch.texa);
            }
            destination.drawn = true;
            {
                const uint32_t sx0 = uint32_t(ctx.scissor) & 0x7FFu, sx1 = uint32_t(ctx.scissor >> 16) & 0x7FFu, sy0 = uint32_t(ctx.scissor >> 32) & 0x7FFu, sy1 = uint32_t(ctx.scissor >> 48) & 0x7FFu;
                markDrawn(destination, std::min(sx0, fbw * 64u), std::min(sy0, destination.usedRows), std::min(sx1 + 1u, fbw * 64u), std::min(sy1 + 1u, destination.usedRows));
            }
            DepthTarget &depthTarget = depth(zbp, fbw);

            ensureCommandBuffer();
            const uint64_t wantedTarget = targetKey(fbp, fbw), wantedDepth = targetKey(zbp, fbw);
            if (!encoder || passTarget != wantedTarget || passDepth != wantedDepth)
            {
                endPass();
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = destination.color;
                pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.depthAttachment.texture = depthTarget.texture;
                pass.depthAttachment.loadAction = depthTarget.fresh ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.depthAttachment.clearDepth = 0.0;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
                depthTarget.fresh = false;
                encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
                ++passes;
                [encoder setRenderPipelineState:pipeline];
                boundPipeline = pipeline;
                [encoder setCullMode:MTLCullModeNone];
                passTarget = wantedTarget;
                passDrawsStart = drawCalls;
                passDepth = wantedDepth;
                boundChunk = nil;
            }
            if (boundChunk != chunk)
            {
                [encoder setVertexBuffer:chunk offset:0 atIndex:0];
                boundChunk = chunk;
            }

            const uint32_t sx0 = uint32_t(ctx.scissor) & 0x7FFu, sx1 = uint32_t(ctx.scissor >> 16) & 0x7FFu, sy0 = uint32_t(ctx.scissor >> 32) & 0x7FFu, sy1 = uint32_t(ctx.scissor >> 48) & 0x7FFu;
            const uint32_t targetWidth = fbw * 64u;
            const uint32_t x0 = std::min(sx0, targetWidth), x1 = std::min(sx1 + 1u, targetWidth), y0 = std::min(sy0, kTargetHeight), y1 = std::min(sy1 + 1u, kTargetHeight);
            if (x1 <= x0 || y1 <= y0)
                return;
            const uint32_t scale = scaleOf(fbw);
            [encoder setScissorRect:(MTLScissorRect){x0 * scale, y0 * scale, (x1 - x0) * scale, (y1 - y0) * scale}];

            Uniforms u{};
            u.pointSize = float(scale);
            u.paletteScale = paletteScale;
            u.targetWidth = float(targetWidth);
            u.targetHeight = float(kTargetHeight);
            u.tme = batch.textured ? 1u : 0u;
            u.tfx = uint32_t(ctx.tex0 >> 35) & 3u;
            u.tcc = uint32_t(ctx.tex0 >> 34) & 1u;
            u.fge = (batch.mode >> 5) & 1u;
            u.abe = (batch.mode >> 6) & 1u;
            u.blendA = uint32_t(ctx.alpha) & 3u;
            u.blendB = uint32_t(ctx.alpha >> 2) & 3u;
            u.blendC = uint32_t(ctx.alpha >> 4) & 3u;
            u.blendD = uint32_t(ctx.alpha >> 6) & 3u;
            u.fix = uint32_t(ctx.alpha >> 32) & 0xFFu;
            u.atst = (test >> 1) & 7u;
            u.aref = (test >> 4) & 0xFFu;
            u.fbmask = uint32_t(ctx.frame >> 32) | (psm == 1u ? 0xFF000000u : 0u);
            u.fba = uint32_t(ctx.fba) & 1u;
            u.colclamp = uint32_t(batch.colclamp) & 1u;
            u.destinationHasAlpha = psm == 1u ? 0u : 1u;
            u.fogR = float(batch.fogcol & 0xFFu);
            u.fogG = float((batch.fogcol >> 8) & 0xFFu);
            u.fogB = float((batch.fogcol >> 16) & 0xFFu);
            u.texScaleU = texScaleU;
            u.texScaleV = texScaleV;
            u.texAlphaFixed = texAlphaFixed ? 1u : 0u;
            u.ta0 = uint32_t(batch.texa) & 0xFFu;
            u.texWidth = float(1u << (uint32_t(ctx.tex0 >> 26) & 15u));
            u.texHeight = float(1u << (uint32_t(ctx.tex0 >> 30) & 15u));
            u.wrapU = uint32_t(ctx.clamp) & 3u;
            u.wrapV = uint32_t(ctx.clamp >> 2) & 3u;
            u.minU = float(uint32_t(ctx.clamp >> 4) & 0x3FFu);
            u.maxU = float(uint32_t(ctx.clamp >> 14) & 0x3FFu);
            u.minV = float(uint32_t(ctx.clamp >> 24) & 0x3FFu);
            u.maxV = float(uint32_t(ctx.clamp >> 34) & 0x3FFu);
            if (texBox != 0u && targetSource)
            {
                // The texture is a piece of a render target: the sampler's own clamping and wrapping act on the
                // whole target, so past the edge of the texture the filter would pick up whatever was drawn next to
                // it. Clamp and repeat are done on the texture's own size instead (a line of wrong pixels along the
                // bottom and right edges of the glow rectangles otherwise).
                if (u.wrapU == 1u) { u.wrapU = 2u; u.minU = 0.0f; u.maxU = u.texWidth - 1.0f; }
                else if (u.wrapU == 0u) u.wrapU = 4u;
                if (u.wrapV == 1u) { u.wrapV = 2u; u.minV = 0.0f; u.maxV = u.texHeight - 1.0f; }
                else if (u.wrapV == 0u) u.wrapV = 4u;
            }
            u.maxLevel = batch.textured && source != fallbackTexture ? uint32_t(source.mipmapLevelCount) - 1u : 0u;
            u.lodFixed = uint32_t(ctx.tex1) & 1u;
            u.lodScale = float(1u << (uint32_t(ctx.tex1 >> 19) & 3u));
            u.lodK = float(int32_t(uint32_t(ctx.tex1 >> 32) << 20) >> 20) * (1.0f / 16.0f);
            u.texIndexed = indexPalette ? 1u : 0u;
            u.texLinear = ((ctx.tex1 >> 5) & 1u) != 0u ? 1u : 0u;
            u.texBox = u.texLinear != 0u ? texBox : 1u;
            u.texSelf = batch.selfFetch && targetSource ? 1u : 0u;
            u.texReplaced = batch.textured && source && replacedTextures.count((__bridge const void *)source) != 0 ? 1u : 0u;
            if (u.texSelf != 0u)
                u.texBox = 1u;
            [encoder setFragmentTexture:indexPalette ? indexPalette : fallbackTexture atIndex:1];

            if (batch.textured)
            {
                const bool linear = ((ctx.tex1 >> 5) & 1u) != 0u || u.texReplaced != 0u;
                [encoder setFragmentTexture:source atIndex:0];
                [encoder setFragmentSamplerState:sampler(linear, uint32_t(ctx.clamp) & 3u, uint32_t(ctx.clamp >> 2) & 3u) atIndex:0];
            }
            else
            {
                [encoder setFragmentTexture:fallbackTexture atIndex:0];
                [encoder setFragmentSamplerState:sampler(false, 0, 0) atIndex:0];
            }

            const bool depthTest = (test & 0x10000u) != 0u && ((test >> 17) & 3u) != 1u, greaterOnly = ((test >> 17) & 3u) == 3u;
            const bool alphaTest = (test & 1u) != 0u && u.atst != 1u;
            const uint32_t afail = (test >> 12) & 3u;
            const auto draw = [&](uint32_t alphaMode, uint32_t writeMode, bool depthWrite)
            {
                u.alphaMode = alphaMode;
                u.writeMode = writeMode;
                const bool opaque = opaquePipeline && !noOpaque && u.abe == 0u && u.fbmask == 0u && writeMode == 0u && u.texSelf == 0u;
                id<MTLRenderPipelineState> wanted = specializedPipeline(u, opaque);
                if (wanted)
                    ++specializedDraws;
                else
                {
                    ++genericDraws;
                    wanted = opaque ? opaquePipeline : pipeline;
                }
                if (wanted != boundPipeline)
                {
                    [encoder setRenderPipelineState:wanted];
                    boundPipeline = wanted;
                }
                [encoder setDepthStencilState:depthState(depthTest, greaterOnly, depthWrite)];
                [encoder setVertexBytes:&u length:sizeof(u) atIndex:1];
                [encoder setFragmentBytes:&u length:sizeof(u) atIndex:1];
                const uint32_t type = batch.mode & 7u;
                [encoder drawPrimitives:type == 0u ? MTLPrimitiveTypePoint : type <= 2u ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle vertexStart:batch.firstVertex vertexCount:batch.vertexCount];
                ++drawCalls;
            };
            // What a failed alpha test still writes: 1 the frame buffer, 2 the depth buffer, 3 colour without alpha.
            const uint32_t failWrite = afail == 1u ? 0u : afail == 2u ? 1u : 2u;
            const bool failDepth = afail == 2u && !zmask;
            if (!alphaTest)
                draw(0u, 0u, !zmask);
            else if (u.atst == 0u)
            {
                if (afail != 0u)
                    draw(0u, failWrite, failDepth);
            }
            else
            {
                draw(1u, 0u, !zmask);
                if (afail != 0u)
                    draw(2u, failWrite, failDepth);
            }
        }
        id<MTLBuffer> boundChunk = nil;
    };

    Renderer &renderer(void *handle) { return *static_cast<Renderer *>(handle); }

    void *create(const char *, const uint8_t *initial)
    {
        @autoreleasepool
        {
            Renderer *created = new Renderer();
            if (!created->initialize(initial))
            {
                delete created;
                return nullptr;
            }
            return created;
        }
    }
    void destroy(void *handle)
    {
        @autoreleasepool
        {
            renderer(handle).flush(true);
            std::lock_guard<std::mutex> lock(g_passTimeMutex);
            const uint64_t scanouts = std::max<uint64_t>(renderer(handle).scanoutCount(), 1u);
            for (const auto &[key, entry] : g_passTime)
                std::fprintf(stderr, "[native-gs] target fbp=%llu fbw=%llu: %.2f ms/scanout in %.1f passes/scanout, %.0f draws/scanout\n", (unsigned long long)(key & 0xFFFFu),
                             (unsigned long long)(key >> 16), entry.seconds * 1e3 / double(scanouts), double(entry.passes) / double(scanouts), double(entry.draws) / double(scanouts));
        }
        @autoreleasepool
        {
            renderer(handle).flush(true);
            delete &renderer(handle);
        }
    }
    void reset(void *handle)
    {
        @autoreleasepool
        {
            renderer(handle).stateChanging();
            renderer(handle).frontend.reset();
        }
    }
    void gif(void *handle, uint32_t path, const uint8_t *data, uint32_t size)
    {
        @autoreleasepool { renderer(handle).frontend.gif(path, data, size); }
    }
    void reg(void *handle, uint8_t address, uint64_t value)
    {
        @autoreleasepool { renderer(handle).frontend.writeRegister(address, value); }
    }
    void image(void *handle, const uint8_t *data, uint32_t size)
    {
        @autoreleasepool { renderer(handle).frontend.transferData(data, size); }
    }
    // The runtime asks for a flush every time it has handed over a batch of GIF data, many times per frame. Committing
    // each time ends the render pass, and the next draw starts another one that reloads and later stores the whole
    // colour and depth target: at 3x that is most of the GPU time of a frame. The work is committed when it is
    // needed instead (a scanout, a wait, a read back, a full vertex buffer), and here only when a good amount has
    // piled up, so the GPU still starts on the frame before the scanout. PS2X_GS_NATIVE_EAGER_FLUSH=1: as before.
    void flush(void *handle)
    {
        @autoreleasepool
        {
            renderer(handle).flushRequested();
        }
    }
    void wait(void *handle)
    {
        @autoreleasepool { renderer(handle).flush(true); }
    }
    // The CPU copy only: what was drawn on the GPU is not brought back yet.
    void read(void *handle, uint32_t offset, uint8_t *out, uint32_t size)
    {
        if (offset < kVramBytes)
            std::memcpy(out, renderer(handle).vram.data() + offset, std::min(size, kVramBytes - offset));
    }
    void write(void *handle, uint32_t offset, const uint8_t *in, uint32_t size)
    {
        @autoreleasepool
        {
            if (offset >= kVramBytes || size == 0u)
                return;
            size = std::min(size, kVramBytes - offset);
            renderer(handle).stateChanging();
            std::memcpy(renderer(handle).vram.data() + offset, in, size);
            std::vector<uint16_t> blocks;
            for (uint32_t block = offset >> 8; block <= (offset + size - 1u) >> 8; ++block)
                blocks.push_back(uint16_t(block));
            renderer(handle).vramWritten(blocks);
        }
    }
    void fifo(void *, uint8_t *out, uint32_t size) { std::memset(out, 0, size); }
    void clear(void *handle, uint32_t fbp, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t rgba, uint32_t fbmsk, uint32_t fba)
    {
        @autoreleasepool
        {
            // A flat sprite through the normal path, with the register state put back afterwards.
            gsn::Frontend &frontend = renderer(handle).frontend;
            renderer(handle).stateChanging();
            const gsn::Registers saved = frontend.regs;
            frontend.writeRegister(0x1A, 1);
            frontend.writeRegister(0x00, 6);
            frontend.writeRegister(0x4C, uint64_t(fbp) | (uint64_t(bw) << 16) | (uint64_t(fbmsk) << 32));
            frontend.writeRegister(0x4E, uint64_t(1) << 32);
            frontend.writeRegister(0x18, 0);
            frontend.writeRegister(0x40, uint64_t(x0) | (uint64_t(x1) << 16) | (uint64_t(y0) << 32) | (uint64_t(y1) << 48));
            frontend.writeRegister(0x47, 0);
            frontend.writeRegister(0x4A, fba & 1u);
            frontend.writeRegister(0x01, uint64_t(rgba) | (uint64_t(0x3F800000u) << 32));
            frontend.writeRegister(0x05, uint64_t(x0 * 16u) | (uint64_t(y0 * 16u) << 16));
            frontend.writeRegister(0x05, uint64_t((x1 + 1u) * 16u) | (uint64_t((y1 + 1u) * 16u) << 16));
            renderer(handle).stateChanging();
            frontend.regs = saved;
        }
    }
    int scanout(void *handle, GSParallelScanout *request)
    {
        @autoreleasepool { return request ? renderer(handle).scanout(*request) : 0; }
    }
    int attachWindow(void *handle, void *window)
    {
        @autoreleasepool { return window && renderer(handle).attachWindow(window) ? 1 : 0; }
    }
}

extern "C" __attribute__((visibility("default"))) const GSParallelAPI *black_parallel_gs_api()
{
    static const GSParallelAPI api{6, create, destroy, reset, gif, reg, image, flush, wait, read, write, fifo, clear, scanout, attachWindow};
    return &api;
}
