#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

@interface BlackPassthroughView : NSView
@end
@implementation BlackPassthroughView
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
@end

struct BlackMetalPresenter {
    __strong BlackPassthroughView *view;
    __strong CAMetalLayer *layer;
    __strong id<MTLDevice> device;
    __strong id<MTLCommandQueue> queue;
};

extern "C" void *black_metal_presenter_create(void *window, VkDevice vkDevice,
                                                VkQueue vkQueue,
                                                PFN_vkExportMetalObjectsEXT exportObjects) {
    if (!window || !vkDevice || !vkQueue || !exportObjects) return nullptr;

    VkExportMetalDeviceInfoEXT deviceInfo{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
    VkExportMetalCommandQueueInfoEXT queueInfo{VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
    queueInfo.queue = vkQueue;
    VkExportMetalObjectsInfoEXT exportInfo{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
    exportInfo.pNext = &deviceInfo;
    deviceInfo.pNext = &queueInfo;
    exportObjects(vkDevice, &exportInfo);
    if (!deviceInfo.mtlDevice || !queueInfo.mtlCommandQueue) return nullptr;

    NSWindow *nsWindow = (__bridge NSWindow *)window;
    NSView *content = nsWindow.contentView;
    if (!content) return nullptr;
    auto *presenter = new BlackMetalPresenter{};
    presenter->device = (__bridge id<MTLDevice>)deviceInfo.mtlDevice;
    presenter->queue = (__bridge id<MTLCommandQueue>)queueInfo.mtlCommandQueue;
    presenter->view = [[BlackPassthroughView alloc] initWithFrame:content.bounds];
    presenter->view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    presenter->view.wantsLayer = YES;
    presenter->layer = [CAMetalLayer layer];
    presenter->layer.device = presenter->device;
    presenter->layer.frame = presenter->view.bounds;
    presenter->layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    presenter->layer.contentsScale = nsWindow.backingScaleFactor;
    presenter->layer.framebufferOnly = YES;
    presenter->layer.contentsGravity = kCAGravityResizeAspect;
    presenter->view.layer = presenter->layer;
    [content addSubview:presenter->view positioned:NSWindowAbove relativeTo:nil];
    return presenter;
}

extern "C" int black_metal_presenter_present(void *opaque, VkDevice vkDevice, VkImage image,
                                               VkFormat format,
                                               uint32_t width, uint32_t height,
                                               PFN_vkExportMetalObjectsEXT exportObjects) {
    auto *presenter = static_cast<BlackMetalPresenter *>(opaque);
    if (!presenter || !image || !exportObjects || !width || !height) return 0;
    VkExportMetalTextureInfoEXT textureInfo{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
    textureInfo.image = image;
    textureInfo.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalObjectsInfoEXT exportInfo{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
    exportInfo.pNext = &textureInfo;
    exportObjects(vkDevice, &exportInfo);
    id<MTLTexture> source = (__bridge id<MTLTexture>)textureInfo.mtlTexture;
    if (!source) return 0;

    presenter->layer.pixelFormat = source.pixelFormat;
    presenter->layer.drawableSize = CGSizeMake(width, height);
    id<CAMetalDrawable> drawable = [presenter->layer nextDrawable];
    if (!drawable || drawable.texture.width != width || drawable.texture.height != height) return 0;
    id<MTLCommandBuffer> command = [presenter->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1) toTexture:drawable.texture
          destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [command presentDrawable:drawable];
    [command commit];
    (void)format;
    return 1;
}

extern "C" void black_metal_presenter_destroy(void *opaque) {
    auto *presenter = static_cast<BlackMetalPresenter *>(opaque);
    if (!presenter) return;
    [presenter->view removeFromSuperview];
    delete presenter;
}
