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

#include "gs_decode.h"
#include "runtime/gs/gs_parallel_api.h"

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
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
};

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

fragment float4 fragmentMain(VertexOut in [[stage_in]], float4 destination [[color(0)]], texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]],
                             sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    // Everything below is in GS units: 0..255 per channel, alpha 128 meaning 1.0.
    float4 c = floor(in.color);
    if (u.tme != 0)
    {
        float2 coordinate = in.stq.xy / in.stq.z;
        if (u.wrapU >= 2 || u.wrapV >= 2)
        {
            // Region modes work on texel coordinates: clamp to [MIN, MAX], or (texel & MIN) | MAX (unfiltered).
            float2 texel = coordinate * float2(u.texWidth, u.texHeight);
            if (u.wrapU == 2) texel.x = clamp(texel.x, u.minU + 0.5, u.maxU + 0.5);
            else if (u.wrapU == 3) texel.x = float((int(floor(texel.x)) & int(u.minU)) | int(u.maxU)) + 0.5;
            if (u.wrapV == 2) texel.y = clamp(texel.y, u.minV + 0.5, u.maxV + 0.5);
            else if (u.wrapV == 3) texel.y = float((int(floor(texel.y)) & int(u.minV)) | int(u.maxV)) + 0.5;
            coordinate = texel / float2(u.texWidth, u.texHeight);
        }
        coordinate *= float2(u.texScaleU, u.texScaleV);
        float4 t;
        if (u.texIndexed != 0)
        {
            float2 size = float2(tex.get_width(), tex.get_height());
            float2 at = coordinate * size;
            if (u.texLinear != 0)
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
            if (u.maxLevel != 0)
            {
                lod = u.lodFixed != 0 ? u.lodK : log2(1.0 / abs(in.stq.z)) * u.lodScale + u.lodK;
                lod = clamp(floor(lod + 0.5), 0.0, float(u.maxLevel));
            }
            t = round(tex.sample(smp, coordinate, level(lod)) * 255.0);
        }
        if (u.texAlphaFixed != 0) t.a = float(u.ta0);
        float3 modulated = min(floor(t.rgb * c.rgb / 128.0), 255.0);
        if (u.tfx == 0)
        {
            c.rgb = modulated;
            if (u.tcc != 0) c.a = min(floor(t.a * c.a / 128.0), 255.0);
        }
        else if (u.tfx == 1)
        {
            c.rgb = t.rgb;
            if (u.tcc != 0) c.a = t.a;
        }
        else
        {
            c.rgb = min(modulated + c.a, 255.0);
            if (u.tcc != 0) c.a = u.tfx == 2 ? min(t.a + c.a, 255.0) : t.a;
        }
    }
    if (u.fge != 0)
    {
        float f = floor(in.fog);
        c.rgb = floor((c.rgb * f + float3(u.fogR, u.fogG, u.fogB) * (256.0 - f)) / 256.0);
    }
    if (u.alphaMode != 0)
    {
        float a = c.a, r = float(u.aref);
        bool pass = u.atst == 0 ? false : u.atst == 1 ? true : u.atst == 2 ? a < r : u.atst == 3 ? a <= r : u.atst == 4 ? a == r : u.atst == 5 ? a >= r : u.atst == 6 ? a > r : a != r;
        if (pass != (u.alphaMode == 1)) discard_fragment();
    }
    float4 d = round(destination * 255.0);
    if (u.writeMode == 1) return destination;
    float3 rgb = c.rgb;
    if (u.abe != 0)
    {
        float3 A = u.blendA == 0 ? c.rgb : u.blendA == 1 ? d.rgb : float3(0.0);
        float3 B = u.blendB == 0 ? c.rgb : u.blendB == 1 ? d.rgb : float3(0.0);
        float C = u.blendC == 0 ? c.a : u.blendC == 1 ? (u.destinationHasAlpha != 0 ? d.a : 128.0) : float(u.fix);
        float3 D = u.blendD == 0 ? c.rgb : u.blendD == 1 ? d.rgb : float3(0.0);
        rgb = floor((A - B) * C / 128.0) + D;
    }
    rgb = u.colclamp != 0 ? clamp(rgb, 0.0, 255.0) : rgb - 256.0 * floor(rgb / 256.0);
    float alpha = u.fba != 0 && c.a < 128.0 ? c.a + 128.0 : c.a;
    if (u.writeMode == 2) alpha = d.a;
    uint4 o = uint4(float4(rgb, clamp(alpha, 0.0, 255.0)));
    if (u.fbmask != 0)
    {
        uint packed = o.r | (o.g << 8) | (o.b << 16) | (o.a << 24);
        uint4 di = uint4(d);
        uint old = di.r | (di.g << 8) | (di.b << 16) | (di.a << 24);
        packed = (packed & ~u.fbmask) | (old & u.fbmask);
        o = uint4(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255, packed >> 24);
    }
    return float4(o) / 255.0;
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
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:nil error:&error];
            if (!library)
            {
                std::fprintf(stderr, "[native-gs] shader: %s\n", error.localizedDescription.UTF8String);
                return false;
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
            pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
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
            if (scale > 1u && second.x > first.x && second.y > first.y)
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
            for (const uint16_t block : blocks)
            {
                ++blockGeneration[block];
                pages.set(block >> 5);
            }
            for (auto &entry : targets)
            {
                Target &target = entry.second;
                for (uint32_t page = 0, count = target.pageCount(); page < count; ++page)
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
            [commandBuffer presentDrawable:drawable];
            flush(false);
            request.width = width;
            request.height = height;
            ++scanouts;
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
            if (stats && scanouts % 60u == 0u)
            {
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
            uint64_t checkedGeneration = 0, pageSum = 0, hash = 0;
            uint64_t dirtyEpoch = ~0ull; // gpuDirtyEpoch when the blocks were last checked against the targets
            bool drawnOver = false;
        };

        struct Batch
        {
            gsn::Context ctx;
            uint32_t mode = 0;
            uint64_t texa = 0, fogcol = 0, colclamp = 0, texclut = 0;
            float offsetX = 0.0f, offsetY = 0.0f, texelU = 1.0f, texelV = 1.0f, maxY = 0.0f, maxX = 0.0f;
            bool gouraud = false, fst = false, textured = false, skip = false, depthCopy = false, shuffle = false;
            uint32_t shuffleSource = 0, shuffleDestination = 0;
            SkipReason skipReason = SkipNone;
            uint32_t firstVertex = 0, vertexCount = 0, logged = 0, primitives = 0;
        };

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
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
        uint64_t gpuDirtyEpoch = 0;           // bumped whenever a block becomes dirty
        uint64_t readbacks = 0;
        uint16_t blockOwner[kVramBytes / 256u] = {}; // base of the last upload that covered the block, plus one
        uint64_t vramGeneration = 1;
        Batch batch;
        bool batchOpen = false, stats = false;
        uint32_t scale = 1;
        id<MTLTexture> resolveTexture = nil;
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

        void ensureCommandBuffer()
        {
            if (commandBuffer)
                return;
            commandBuffer = [queue commandBuffer];
        }

        void endPass()
        {
            if (encoder)
            {
                [encoder endEncoding];
                encoder = nil;
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
                descriptor.mipFilter = MTLSamplerMipFilterNearest;
                // The region modes resolve the coordinate in the shader; the sampler only has to stay out of the way.
                descriptor.sAddressMode = wrapU != 0u ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                descriptor.tAddressMode = wrapV != 0u ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                state = [device newSamplerStateWithDescriptor:descriptor];
            }
            return state;
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
            // Not while a target has drawn over this texture's memory since: that has to come back to VRAM first.
            if (entry.dirtyEpoch != gpuDirtyEpoch)
            {
                entry.dirtyEpoch = gpuDirtyEpoch;
                entry.drawnOver = false;
                for (const uint16_t block : entry.pages)
                    entry.drawnOver = entry.drawnOver || gpuDirtyBlocks.test(block);
            }
            const bool drawnOver = entry.drawnOver;
            if (entry.texture && !drawnOver && entry.checkedGeneration == vramGeneration)
                return entry.texture;
            if (entry.texture && !drawnOver && generationSum(entry.pages) == entry.pageSum)
            {
                entry.checkedGeneration = vramGeneration;
                return entry.texture;
            }

            if (gpuDirtyBlocks.any())
            {
                std::bitset<kVramBytes / 256u> needed;
                if (paletted)
                    for (uint32_t block = cbp; block < cbp + (psm == 0x13u || psm == 0x1Bu ? 4u : 1u) && block < kVramBytes / 256u; ++block)
                        needed.set(block);
                const gsn::Swizzle &where = gsn::Swizzle::of(psm);
                for (uint32_t level = 0; level < levelCount; ++level)
                    for (uint32_t y = 0; y < levels[level].height; y += 8u)
                        for (uint32_t x = 0; x < levels[level].width; x += 8u)
                            needed.set(where.bitAddress(levels[level].tbp, levels[level].tbw, x, y) >> 11);
                syncReason = tex0;
                syncFromGpu(needed);
                entry.drawnOver = false;
                entry.dirtyEpoch = gpuDirtyEpoch;
            }
            const uint32_t ta0 = uint32_t(texa) & 0xFFu, ta1 = uint32_t(texa >> 32) & 0xFFu;
            const bool aem = ((texa >> 15) & 1u) != 0u;
            GSMem::TexturePageCache cache;
            std::bitset<kVramBytes / 256u> used;
            uint32_t palette[256];
            uint64_t paletteHash = 0xcbf29ce484222325ull;
            if (paletted)
            {
                const uint32_t entries = psm == 0x13u || psm == 0x1Bu ? 256u : 16u;
                for (uint32_t i = 0; i < entries; ++i)
                {
                    // CSM1 layout: 16x16 with the middle index bits swapped, or 8x2.
                    const uint32_t at = entries == 256u ? (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1) : i;
                    const uint32_t x = entries == 256u ? at & 15u : at & 7u, y = entries == 256u ? at >> 4 : at >> 3;
                    palette[i] = expand(GSMem::ReadTexture(cache, vram.data(), cpsm, cbp, 1u, x, y), cpsm, ta0, ta1, aem);
                    paletteHash = (paletteHash ^ palette[i]) * 0x100000001b3ull;
                    if ((x & 7u) == 0u && (y & 7u) == 0u)
                        used.set(((GSMem::PixelBitAddress(cpsm, cbp, 1u, x, y) >> 3) & (kVramBytes - 1u)) >> 8);
                }
            }
            for (uint32_t level = 0; level < levelCount; ++level)
                for (uint32_t y = 0; y < levels[level].height; y += 8u)
                    for (uint32_t x = 0; x < levels[level].width; x += 8u)
                        used.set(gsn::Swizzle::of(psm).bitAddress(levels[level].tbp, levels[level].tbw, x, y) >> 11);
            entry.pages.clear();
            for (uint32_t block = 0; block < kVramBytes / 256u; ++block)
                if (used.test(block))
                {
                    entry.pages.push_back(uint16_t(block));
                }
            entry.pageSum = generationSum(entry.pages);
            entry.checkedGeneration = vramGeneration;

            const gsn::Swizzle &layout = gsn::Swizzle::of(psm);
            // The same bytes as an earlier upload, in the same format: the texture decoded then.
            const auto record = uploads.find(tbp);
            bool known = record != uploads.end() && generationSum(record->second.blocks) == record->second.blockSum;
            if (record == uploads.end())
                ++unknownNoUpload;
            else if (!known)
                ++unknownChanged;
            const bool knownBase = known;
            // With mip levels the upload has to account for every block the texture reads (levels sent under the same base).
            // Mip levels may have been sent on their own: each one is identified by the upload that covers it.
            uint64_t mipHash = 0;
            for (uint32_t level = 1; level < levelCount && known; ++level)
            {
                const uint32_t owner = blockOwner[layout.bitAddress(levels[level].tbp, levels[level].tbw, 0u, 0u) >> 11];
                const auto from = owner != 0u ? uploads.find(owner - 1u) : uploads.end();
                known = from != uploads.end() && generationSum(from->second.blocks) == from->second.blockSum;
                for (uint32_t y = 0; y < levels[level].height && known; y += 8u)
                    for (uint32_t x = 0; x < levels[level].width && known; x += 8u)
                        known = blockOwner[layout.bitAddress(levels[level].tbp, levels[level].tbw, x, y) >> 11] == owner;
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

            std::vector<uint32_t> pixels[7];
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint32_t level = 0; level < levelCount; ++level)
            {
                const Level &from = levels[level];
                pixels[level].resize(size_t(from.width) * from.height);
                for (uint32_t y = 0; y < from.height; ++y)
                    for (uint32_t x = 0; x < from.width; ++x)
                    {
                        const uint32_t value = layout.read(vram.data(), from.tbp, from.tbw, x, y);
                        const uint32_t pixel = paletted ? palette[value & 0xFFu] : expand(value, psm, ta0, ta1, aem);
                        pixels[level][size_t(y) * from.width + x] = pixel;
                        hash = (hash ^ pixel) * 0x100000001b3ull;
                    }
            }
            ++decodes;
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
            bool fresh = false;
            for (uint32_t y = y0 & ~7u; y < y1; y += 8u)
                for (uint32_t x = x0 & ~7u; x < x1; x += 8u)
                {
                    const uint32_t block = layout.bitAddress(target.fbp * 32u, target.fbw, x, y) >> 11;
                    fresh = fresh || !gpuDirtyBlocks.test(block);
                    target.gpuDirty.set(block);
                    gpuDirtyBlocks.set(block);
                }
            if (fresh)
                ++gpuDirtyEpoch;
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
                    ++blockGeneration[block];
            }
        }

        void closeBatch()
        {
            if (!batchOpen)
                return;
            batchOpen = false;
            // PS2X_GS_NATIVE_LOG=<scanout>: one line per batch drawn while that scanout is being built.
            if (logging() && (batch.vertexCount != 0u || batch.skip))
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
            uint32_t paletteScale = 1;
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
                    if (&from == &destination)
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
                        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                        [blit copyFromTexture:from.color toTexture:from.copy];
                        [blit endEncoding];
                        source = from.copy;
                        ++selfSamples;
                    }
                    texScaleU = float(1u << (uint32_t(ctx.tex0 >> 26) & 15u)) / float(from.fbw * 64u);
                    texScaleV = float(1u << (uint32_t(ctx.tex0 >> 30) & 15u)) / float(kTargetHeight);
                    texAlphaFixed = tpsm == 1u;
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
                [encoder setRenderPipelineState:pipeline];
                [encoder setCullMode:MTLCullModeNone];
                passTarget = wantedTarget;
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
            u.maxLevel = batch.textured && source != fallbackTexture ? uint32_t(source.mipmapLevelCount) - 1u : 0u;
            u.lodFixed = uint32_t(ctx.tex1) & 1u;
            u.lodScale = float(1u << (uint32_t(ctx.tex1 >> 19) & 3u));
            u.lodK = float(int32_t(uint32_t(ctx.tex1 >> 32) << 20) >> 20) * (1.0f / 16.0f);
            u.texIndexed = indexPalette ? 1u : 0u;
            u.texLinear = ((ctx.tex1 >> 5) & 1u) != 0u ? 1u : 0u;
            [encoder setFragmentTexture:indexPalette ? indexPalette : fallbackTexture atIndex:1];

            if (batch.textured)
            {
                const bool linear = ((ctx.tex1 >> 5) & 1u) != 0u;
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
    void flush(void *handle)
    {
        @autoreleasepool { renderer(handle).flush(false); }
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
