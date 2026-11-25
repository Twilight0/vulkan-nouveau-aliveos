#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

#include "shaders/tri.vert.spv.h"
#include "shaders/tri.frag.spv.h"
#include "shaders/tex.frag.spv.h"
#include "shaders/comp_ssbo.comp.spv.h"
#include "shaders/comp_tex_dyn.comp.spv.h"
#include "shaders/comp_img_store.comp.spv.h"
#include "shaders/comp_img_rw.comp.spv.h"
#include "shaders/indexed_ubo.vert.spv.h"
#include "shaders/indexed_ubo.frag.spv.h"

#define VK_CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "VK error %d at %s:%d (%s)\n", _r, __FILE__, __LINE__, #call); \
        return 1; \
    } \
} while (0)

struct vk_ctx {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice dev;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool cmd_pool;
};

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *props,
                              uint32_t type_bits, VkMemoryPropertyFlags req)
{
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props->memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int create_shader_module(VkDevice dev, const uint32_t *code, size_t size_bytes,
                                VkShaderModule *out)
{
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size_bytes,
        .pCode = code,
    };
    VK_CHECK(vkCreateShaderModule(dev, &ci, NULL, out));
    return 0;
}

static int create_buffer(struct vk_ctx *ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags mem_flags, VkBuffer *buf, VkDeviceMemory *mem)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(ctx->dev, &bci, NULL, buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx->dev, *buf, &req);

    uint32_t mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits, mem_flags);
    if (mt == UINT32_MAX) {
        fprintf(stderr, "No suitable memory type for buffer\n");
        return 1;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(ctx->dev, &mai, NULL, mem));
    VK_CHECK(vkBindBufferMemory(ctx->dev, *buf, *mem, 0));
    return 0;
}

static int submit_and_wait(struct vk_ctx *ctx, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkFence fence;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(ctx->dev, &fci, NULL, &fence));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(ctx->dev, 1, &fence, VK_TRUE, 3000000000ULL));

    vkDestroyFence(ctx->dev, fence, NULL);
    vkFreeCommandBuffers(ctx->dev, ctx->cmd_pool, 1, &cmd);
    return 0;
}

static int alloc_cmd(struct vk_ctx *ctx, VkCommandBuffer *cmd)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(ctx->dev, &ai, cmd));

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(*cmd, &bi));
    return 0;
}

static int test_graphics(struct vk_ctx *ctx, bool optimal, bool textured)
{
    const uint32_t W = 64, H = 64;
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = optimal ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(ctx->dev, &ici, NULL, &img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->dev, img, &req);
    uint32_t mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits,
                                optimal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                        : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    if (mt == UINT32_MAX) return 1;

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(ctx->dev, &mai, NULL, &img_mem));
    VK_CHECK(vkBindImageMemory(ctx->dev, img, img_mem, 0));

    VkImageView view;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_CHECK(vkCreateImageView(ctx->dev, &vci, NULL, &view));

    VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkRenderPass rp;
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
    };
    VK_CHECK(vkCreateRenderPass(ctx->dev, &rpci, NULL, &rp));

    VkFramebuffer fb;
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = W,
        .height = H,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(ctx->dev, &fci, NULL, &fb));

    /* Optional texture */
    VkImage tex_img = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer tex_stage = VK_NULL_HANDLE;
    VkDeviceMemory tex_stage_mem = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;

    if (textured) {
        uint8_t pixels[4 * 4] = {
            255,   0,   0, 255,   0, 255,   0, 255,
              0,   0, 255, 255, 255, 255,   0, 255,
        };
        if (create_buffer(ctx, sizeof(pixels), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &tex_stage, &tex_stage_mem)) return 1;
        void *p;
        VK_CHECK(vkMapMemory(ctx->dev, tex_stage_mem, 0, sizeof(pixels), 0, &p));
        memcpy(p, pixels, sizeof(pixels));
        vkUnmapMemory(ctx->dev, tex_stage_mem);

        VkImageCreateInfo tici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { 2, 2, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VK_CHECK(vkCreateImage(ctx->dev, &tici, NULL, &tex_img));
        vkGetImageMemoryRequirements(ctx->dev, tex_img, &req);
        mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo tmai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = mt,
        };
        VK_CHECK(vkAllocateMemory(ctx->dev, &tmai, NULL, &tex_mem));
        VK_CHECK(vkBindImageMemory(ctx->dev, tex_img, tex_mem, 0));

        VkCommandBuffer cmd;
        if (alloc_cmd(ctx, &cmd)) return 1;
        VkImageMemoryBarrier b0 = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = tex_img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
        VkBufferImageCopy bic = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { 2, 2, 1 },
        };
        vkCmdCopyBufferToImage(cmd, tex_stage, tex_img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        VkImageMemoryBarrier b1 = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = tex_img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        if (submit_and_wait(ctx, cmd)) return 1;

        VkImageViewCreateInfo tvci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = tex_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(ctx->dev, &tvci, NULL, &tex_view));

        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        VK_CHECK(vkCreateSampler(ctx->dev, &sci, NULL, &sampler));

        VkDescriptorSetLayoutBinding bind = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        VkDescriptorSetLayoutCreateInfo dslci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &bind,
        };
        VK_CHECK(vkCreateDescriptorSetLayout(ctx->dev, &dslci, NULL, &dsl));

        VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo dpci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &dps,
        };
        VK_CHECK(vkCreateDescriptorPool(ctx->dev, &dpci, NULL, &dp));
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dp,
            .descriptorSetCount = 1,
            .pSetLayouts = &dsl,
        };
        VK_CHECK(vkAllocateDescriptorSets(ctx->dev, &dsai, &ds));
        VkDescriptorImageInfo dii = {
            .sampler = sampler,
            .imageView = tex_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wds = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dii,
        };
        vkUpdateDescriptorSets(ctx->dev, 1, &wds, 0, NULL);
    }

    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = textured ? 1 : 0,
        .pSetLayouts = textured ? &dsl : NULL,
    };
    VK_CHECK(vkCreatePipelineLayout(ctx->dev, &plci, NULL, &pl));

    VkShaderModule vs, fs;
    if (create_shader_module(ctx->dev, tri_vert_spv, sizeof(tri_vert_spv), &vs)) return 1;
    if (textured) {
        if (create_shader_module(ctx->dev, tex_frag_spv, sizeof(tex_frag_spv), &fs)) return 1;
    } else {
        if (create_shader_module(ctx->dev, tri_frag_spv, sizeof(tri_frag_spv), &fs)) return 1;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    VkRect2D sc = { {0, 0}, {W, H} };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount = 1, .pScissors = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .layout = pl,
        .renderPass = rp,
    };
    VkPipeline pipe;
    VK_CHECK(vkCreateGraphicsPipelines(ctx->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    VkBuffer dst_buf;
    VkDeviceMemory dst_mem;
    if (create_buffer(ctx, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &dst_buf, &dst_mem)) return 1;

    VkCommandBuffer cmd;
    if (alloc_cmd(ctx, &cmd)) return 1;
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp,
        .framebuffer = fb,
        .renderArea = { {0, 0}, {W, H} },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (textured)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buf, 1, &bic);
    if (submit_and_wait(ctx, cmd)) return 1;

    uint8_t *out;
    VK_CHECK(vkMapMemory(ctx->dev, dst_mem, 0, W * H * 4, 0, (void **)&out));

    int err = 0;
    if (!textured) {
        /* Center pixel should be ~(64, 128, 191, 255) */
        uint8_t *px = &out[(32 * W + 32) * 4];
        if (abs(px[0] - 64) > 2 || abs(px[1] - 128) > 2 || abs(px[2] - 191) > 2 || px[3] != 255) {
            fprintf(stderr, "FAIL graphics(%s): center pixel=(%u,%u,%u,%u)\n",
                    optimal ? "optimal" : "linear", px[0], px[1], px[2], px[3]);
            err = 1;
        }
    } else {
        uint8_t *tl = &out[(8 * W + 8) * 4];
        uint8_t *tr = &out[(8 * W + 48) * 4];
        uint8_t *bl = &out[(48 * W + 8) * 4];
        uint8_t *br = &out[(48 * W + 48) * 4];
        if (tl[0] < 240 || tr[1] < 240 || bl[2] < 240 || br[0] < 240 || br[1] < 240) {
            fprintf(stderr, "FAIL graphics_textured: tl=(%u,%u,%u) tr=(%u,%u,%u) bl=(%u,%u,%u) br=(%u,%u,%u)\n",
                    tl[0], tl[1], tl[2], tr[0], tr[1], tr[2],
                    bl[0], bl[1], bl[2], br[0], br[1], br[2]);
            err = 1;
        }
    }

    vkUnmapMemory(ctx->dev, dst_mem);
    vkDestroyBuffer(ctx->dev, dst_buf, NULL);
    vkFreeMemory(ctx->dev, dst_mem, NULL);
    vkDestroyPipeline(ctx->dev, pipe, NULL);
    vkDestroyShaderModule(ctx->dev, vs, NULL);
    vkDestroyShaderModule(ctx->dev, fs, NULL);
    vkDestroyPipelineLayout(ctx->dev, pl, NULL);
    if (textured) {
        vkDestroyDescriptorPool(ctx->dev, dp, NULL);
        vkDestroyDescriptorSetLayout(ctx->dev, dsl, NULL);
        vkDestroySampler(ctx->dev, sampler, NULL);
        vkDestroyImageView(ctx->dev, tex_view, NULL);
        vkDestroyImage(ctx->dev, tex_img, NULL);
        vkFreeMemory(ctx->dev, tex_mem, NULL);
        vkDestroyBuffer(ctx->dev, tex_stage, NULL);
        vkFreeMemory(ctx->dev, tex_stage_mem, NULL);
    }
    vkDestroyFramebuffer(ctx->dev, fb, NULL);
    vkDestroyRenderPass(ctx->dev, rp, NULL);
    vkDestroyImageView(ctx->dev, view, NULL);
    vkDestroyImage(ctx->dev, img, NULL);
    vkFreeMemory(ctx->dev, img_mem, NULL);

    if (!err)
        printf("PASS %s\n", textured ? "graphics_textured" : (optimal ? "graphics_optimal_copy" : "graphics_linear"));
    return err;
}

static int test_compute_ssbo(struct vk_ctx *ctx)
{
    const uint32_t N = 64;
    const VkDeviceSize sz = N * sizeof(uint32_t);

    VkBuffer in_buf, out_buf;
    VkDeviceMemory in_mem, out_mem;
    if (create_buffer(ctx, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &in_buf, &in_mem)) return 1;
    if (create_buffer(ctx, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &out_buf, &out_mem)) return 1;

    uint32_t *in_ptr, *out_ptr;
    VK_CHECK(vkMapMemory(ctx->dev, in_mem, 0, sz, 0, (void **)&in_ptr));
    for (uint32_t i = 0; i < N; i++)
        in_ptr[i] = 100u + i;
    vkUnmapMemory(ctx->dev, in_mem);

    VK_CHECK(vkMapMemory(ctx->dev, out_mem, 0, sz, 0, (void **)&out_ptr));
    memset(out_ptr, 0xaa, sz);
    vkUnmapMemory(ctx->dev, out_mem);

    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = binds,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(ctx->dev, &dslci, NULL, &dsl));

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPool dp;
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps,
    };
    VK_CHECK(vkCreateDescriptorPool(ctx->dev, &dpci, NULL, &dp));

    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkAllocateDescriptorSets(ctx->dev, &dsai, &ds));

    VkDescriptorBufferInfo dbis[2] = {
        { in_buf, 0, sz },
        { out_buf, 0, sz },
    };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbis[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbis[1] },
    };
    vkUpdateDescriptorSets(ctx->dev, 2, writes, 0, NULL);

    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkCreatePipelineLayout(ctx->dev, &plci, NULL, &pl));

    VkShaderModule cs;
    if (create_shader_module(ctx->dev, comp_ssbo_comp_spv, sizeof(comp_ssbo_comp_spv), &cs)) return 1;

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = cs,
            .pName = "main",
        },
        .layout = pl,
    };
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(ctx->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkCommandBuffer cmd;
    if (alloc_cmd(ctx, &cmd)) return 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, N / 16, 1, 1);
    if (submit_and_wait(ctx, cmd)) return 1;

    VK_CHECK(vkMapMemory(ctx->dev, out_mem, 0, sz, 0, (void **)&out_ptr));
    int err = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t expected = (100u + i) * 2u + i + 7u;
        if (out_ptr[i] != expected) {
            fprintf(stderr, "FAIL compute_ssbo: idx=%u got=%u expected=%u\n",
                    i, out_ptr[i], expected);
            err = 1;
            break;
        }
    }
    vkUnmapMemory(ctx->dev, out_mem);

    vkDestroyPipeline(ctx->dev, pipe, NULL);
    vkDestroyShaderModule(ctx->dev, cs, NULL);
    vkDestroyPipelineLayout(ctx->dev, pl, NULL);
    vkDestroyDescriptorPool(ctx->dev, dp, NULL);
    vkDestroyDescriptorSetLayout(ctx->dev, dsl, NULL);
    vkDestroyBuffer(ctx->dev, in_buf, NULL);
    vkFreeMemory(ctx->dev, in_mem, NULL);
    vkDestroyBuffer(ctx->dev, out_buf, NULL);
    vkFreeMemory(ctx->dev, out_mem, NULL);

    if (!err)
        printf("PASS compute_ssbo (%u elements)\n", N);
    return err;
}

static int test_compute_tex_dyn_txs(struct vk_ctx *ctx)
{
    /* Create 2 textures of different sizes and colors:
     * tex[0]: 2x3, pixel (0,0) = (200, 10, 0, 255)
     * tex[1]: 4x5, pixel (0,0) = (10, 220, 0, 255)
     */
    const uint32_t widths[2] = { 2, 4 };
    const uint32_t heights[2] = { 3, 5 };
    const uint8_t colors[2][4] = {
        { 200, 10, 0, 255 },
        { 10, 220, 0, 255 },
    };

    VkImage imgs[2];
    VkDeviceMemory img_mems[2];
    VkImageView views[2];
    VkBuffer stages[2];
    VkDeviceMemory stage_mems[2];
    VkSampler sampler;

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
    };
    VK_CHECK(vkCreateSampler(ctx->dev, &sci, NULL, &sampler));

    for (int t = 0; t < 2; t++) {
        VkDeviceSize sz = widths[t] * heights[t] * 4;
        if (create_buffer(ctx, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stages[t], &stage_mems[t])) return 1;
        uint8_t *p;
        VK_CHECK(vkMapMemory(ctx->dev, stage_mems[t], 0, sz, 0, (void **)&p));
        for (uint32_t k = 0; k < widths[t] * heights[t]; k++)
            memcpy(p + k * 4, colors[t], 4);
        vkUnmapMemory(ctx->dev, stage_mems[t]);

        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { widths[t], heights[t], 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VK_CHECK(vkCreateImage(ctx->dev, &ici, NULL, &imgs[t]));

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(ctx->dev, imgs[t], &req);
        uint32_t mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = mt,
        };
        VK_CHECK(vkAllocateMemory(ctx->dev, &mai, NULL, &img_mems[t]));
        VK_CHECK(vkBindImageMemory(ctx->dev, imgs[t], img_mems[t], 0));

        VkCommandBuffer cmd;
        if (alloc_cmd(ctx, &cmd)) return 1;
        VkImageMemoryBarrier b0 = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = imgs[t],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
        VkBufferImageCopy bic = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { widths[t], heights[t], 1 },
        };
        vkCmdCopyBufferToImage(cmd, stages[t], imgs[t],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        VkImageMemoryBarrier b1 = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = imgs[t],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        if (submit_and_wait(ctx, cmd)) return 1;

        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imgs[t],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(ctx->dev, &vci, NULL, &views[t]));
    }

    const uint32_t N = 4;
    const VkDeviceSize out_sz = N * 4 * sizeof(uint32_t);
    VkBuffer out_buf;
    VkDeviceMemory out_mem;
    if (create_buffer(ctx, out_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &out_buf, &out_mem)) return 1;

    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = binds,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(ctx->dev, &dslci, NULL, &dsl));

    VkDescriptorPoolSize dps[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
    };
    VkDescriptorPool dp;
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = dps,
    };
    VK_CHECK(vkCreateDescriptorPool(ctx->dev, &dpci, NULL, &dp));

    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkAllocateDescriptorSets(ctx->dev, &dsai, &ds));

    VkDescriptorImageInfo diis[2] = {
        { sampler, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { sampler, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkDescriptorBufferInfo dbi = { out_buf, 0, out_sz };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
          .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = diis },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi },
    };
    vkUpdateDescriptorSets(ctx->dev, 2, writes, 0, NULL);

    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkCreatePipelineLayout(ctx->dev, &plci, NULL, &pl));

    VkShaderModule cs;
    if (create_shader_module(ctx->dev, comp_tex_dyn_comp_spv, sizeof(comp_tex_dyn_comp_spv), &cs)) return 1;

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = cs,
            .pName = "main",
        },
        .layout = pl,
    };
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(ctx->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkCommandBuffer cmd;
    if (alloc_cmd(ctx, &cmd)) return 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    if (submit_and_wait(ctx, cmd)) return 1;

    uint32_t *out_ptr;
    VK_CHECK(vkMapMemory(ctx->dev, out_mem, 0, out_sz, 0, (void **)&out_ptr));
    int err = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t idx = i & 1u;
        uint32_t w = out_ptr[i * 4 + 0];
        uint32_t h = out_ptr[i * 4 + 1];
        uint32_t r = out_ptr[i * 4 + 2];
        uint32_t g = out_ptr[i * 4 + 3];
        if (w != widths[idx] || h != heights[idx] ||
            r != colors[idx][0] || g != colors[idx][1]) {
            fprintf(stderr, "FAIL compute_tex_dyn_txs[%u]: got=(%u,%u,%u,%u) expected=(%u,%u,%u,%u)\n",
                    i, w, h, r, g, widths[idx], heights[idx], colors[idx][0], colors[idx][1]);
            err = 1;
            break;
        }
    }
    vkUnmapMemory(ctx->dev, out_mem);

    vkDestroyPipeline(ctx->dev, pipe, NULL);
    vkDestroyShaderModule(ctx->dev, cs, NULL);
    vkDestroyPipelineLayout(ctx->dev, pl, NULL);
    vkDestroyDescriptorPool(ctx->dev, dp, NULL);
    vkDestroyDescriptorSetLayout(ctx->dev, dsl, NULL);
    vkDestroyBuffer(ctx->dev, out_buf, NULL);
    vkFreeMemory(ctx->dev, out_mem, NULL);
    for (int t = 0; t < 2; t++) {
        vkDestroyImageView(ctx->dev, views[t], NULL);
        vkDestroyImage(ctx->dev, imgs[t], NULL);
        vkFreeMemory(ctx->dev, img_mems[t], NULL);
        vkDestroyBuffer(ctx->dev, stages[t], NULL);
        vkFreeMemory(ctx->dev, stage_mems[t], NULL);
    }
    vkDestroySampler(ctx->dev, sampler, NULL);

    if (!err)
        printf("PASS compute_tex_dyn_txs (dynamic array + textureSize)\n");
    return err;
}

static int test_compute_storage_image(struct vk_ctx *ctx)
{
    const uint32_t W = 4, H = 4;
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(ctx->dev, &ici, NULL, &img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->dev, img, &req);
    uint32_t mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(ctx->dev, &mai, NULL, &img_mem));
    VK_CHECK(vkBindImageMemory(ctx->dev, img, img_mem, 0));

    VkImageView view;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_CHECK(vkCreateImageView(ctx->dev, &vci, NULL, &view));

    VkDescriptorSetLayoutBinding bind = {
        0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL,
    };
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(ctx->dev, &dslci, NULL, &dsl));

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
    VkDescriptorPool dp;
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps,
    };
    VK_CHECK(vkCreateDescriptorPool(ctx->dev, &dpci, NULL, &dp));

    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkAllocateDescriptorSets(ctx->dev, &dsai, &ds));

    VkDescriptorImageInfo dii = {
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(ctx->dev, 1, &write, 0, NULL);

    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkCreatePipelineLayout(ctx->dev, &plci, NULL, &pl));

    VkShaderModule cs;
    if (create_shader_module(ctx->dev, comp_img_store_comp_spv,
                             sizeof(comp_img_store_comp_spv), &cs)) return 1;

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = cs,
            .pName = "main",
        },
        .layout = pl,
    };
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(ctx->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkBuffer dst_buf;
    VkDeviceMemory dst_mem;
    if (create_buffer(ctx, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &dst_buf, &dst_mem)) return 1;

    VkCommandBuffer cmd;
    if (alloc_cmd(ctx, &cmd)) return 1;
    VkImageMemoryBarrier b0 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);

    VkImageMemoryBarrier b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);

    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buf, 1, &bic);
    if (submit_and_wait(ctx, cmd)) return 1;

    uint8_t *out;
    VK_CHECK(vkMapMemory(ctx->dev, dst_mem, 0, W * H * 4, 0, (void **)&out));
    int err = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *px = &out[(y * W + x) * 4];
            uint8_t er = (uint8_t)((x * 255 + 1) / 3);
            uint8_t eg = (uint8_t)((y * 255 + 1) / 3);
            if (abs((int)px[0] - (int)er) > 2 || abs((int)px[1] - (int)eg) > 2 ||
                abs((int)px[2] - 128) > 2 || px[3] != 255) {
                fprintf(stderr, "FAIL compute_storage_image(%u,%u): got=(%u,%u,%u,%u) exp=(%u,%u,128,255)\n",
                        x, y, px[0], px[1], px[2], px[3], er, eg);
                err = 1;
                break;
            }
        }
        if (err) break;
    }
    vkUnmapMemory(ctx->dev, dst_mem);

    vkDestroyBuffer(ctx->dev, dst_buf, NULL);
    vkFreeMemory(ctx->dev, dst_mem, NULL);
    vkDestroyPipeline(ctx->dev, pipe, NULL);
    vkDestroyShaderModule(ctx->dev, cs, NULL);
    vkDestroyPipelineLayout(ctx->dev, pl, NULL);
    vkDestroyDescriptorPool(ctx->dev, dp, NULL);
    vkDestroyDescriptorSetLayout(ctx->dev, dsl, NULL);
    vkDestroyImageView(ctx->dev, view, NULL);
    vkDestroyImage(ctx->dev, img, NULL);
    vkFreeMemory(ctx->dev, img_mem, NULL);

    if (!err)
        printf("PASS compute_storage_image (4x4 imageStore)\n");
    return err;
}

static int test_graphics_indexed_ubo(struct vk_ctx *ctx)
{
    const uint32_t W = 32, H = 32;
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(ctx->dev, &ici, NULL, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->dev, img, &req);
    uint32_t mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(ctx->dev, &mai, NULL, &img_mem));
    VK_CHECK(vkBindImageMemory(ctx->dev, img, img_mem, 0));

    VkImageView view;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_CHECK(vkCreateImageView(ctx->dev, &vci, NULL, &view));

    VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
    };
    VkRenderPass rp;
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
    };
    VK_CHECK(vkCreateRenderPass(ctx->dev, &rpci, NULL, &rp));

    VkFramebuffer fb;
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
        .width = W, .height = H, .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(ctx->dev, &fci, NULL, &fb));

    struct vert { float x, y, r, g, b; };
    const struct vert verts[4] = {
        { -1.0f, -1.0f, 1.0f, 0.5f, 0.25f },
        {  1.0f, -1.0f, 1.0f, 0.5f, 0.25f },
        {  1.0f,  1.0f, 1.0f, 0.5f, 0.25f },
        { -1.0f,  1.0f, 1.0f, 0.5f, 0.25f },
    };
    const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    const float ubo_data[4] = { 0.5f, 1.0f, 1.0f, 1.0f };

    VkBuffer vb, ib, ub, dst_buf;
    VkDeviceMemory vb_mem, ib_mem, ub_mem, dst_mem;
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (create_buffer(ctx, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv, &vb, &vb_mem)) return 1;
    if (create_buffer(ctx, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hv, &ib, &ib_mem)) return 1;
    if (create_buffer(ctx, sizeof(ubo_data), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hv, &ub, &ub_mem)) return 1;
    if (create_buffer(ctx, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, &dst_buf, &dst_mem)) return 1;

    void *p;
    VK_CHECK(vkMapMemory(ctx->dev, vb_mem, 0, sizeof(verts), 0, &p)); memcpy(p, verts, sizeof(verts)); vkUnmapMemory(ctx->dev, vb_mem);
    VK_CHECK(vkMapMemory(ctx->dev, ib_mem, 0, sizeof(indices), 0, &p)); memcpy(p, indices, sizeof(indices)); vkUnmapMemory(ctx->dev, ib_mem);
    VK_CHECK(vkMapMemory(ctx->dev, ub_mem, 0, sizeof(ubo_data), 0, &p)); memcpy(p, ubo_data, sizeof(ubo_data)); vkUnmapMemory(ctx->dev, ub_mem);

    VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bind,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(ctx->dev, &dslci, NULL, &dsl));
    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
    VkDescriptorPool dp;
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps,
    };
    VK_CHECK(vkCreateDescriptorPool(ctx->dev, &dpci, NULL, &dp));
    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkAllocateDescriptorSets(ctx->dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = { ub, 0, sizeof(ubo_data) };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi,
    };
    vkUpdateDescriptorSets(ctx->dev, 1, &wds, 0, NULL);

    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkCreatePipelineLayout(ctx->dev, &plci, NULL, &pl));

    VkShaderModule vs, fs;
    if (create_shader_module(ctx->dev, indexed_ubo_vert_spv, sizeof(indexed_ubo_vert_spv), &vs)) return 1;
    if (create_shader_module(ctx->dev, indexed_ubo_frag_spv, sizeof(indexed_ubo_frag_spv), &fs)) return 1;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vib = { 0, sizeof(struct vert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via[2] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    VkRect2D sc = { {0, 0}, {W, H} };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp,
    };
    VkPipeline pipe;
    VK_CHECK(vkCreateGraphicsPipelines(ctx->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    VkCommandBuffer cmd;
    if (alloc_cmd(ctx, &cmd)) return 1;
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { {0, 0}, {W, H} }, .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy bic = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buf, 1, &bic);
    if (submit_and_wait(ctx, cmd)) return 1;

    uint8_t *out;
    VK_CHECK(vkMapMemory(ctx->dev, dst_mem, 0, W * H * 4, 0, (void **)&out));
    uint8_t *px = &out[(16 * W + 16) * 4];
    int err = 0;
    /* Expected: (1.0*0.5, 0.5*1.0, 0.25*1.0, 1.0) = (128, 128, 64, 255) */
    if (abs((int)px[0] - 128) > 2 || abs((int)px[1] - 128) > 2 || abs((int)px[2] - 64) > 2 || px[3] != 255) {
        fprintf(stderr, "FAIL graphics_indexed_ubo: got=(%u,%u,%u,%u) exp=(128,128,64,255)\n",
                px[0], px[1], px[2], px[3]);
        err = 1;
    }
    vkUnmapMemory(ctx->dev, dst_mem);

    vkDestroyPipeline(ctx->dev, pipe, NULL);
    vkDestroyShaderModule(ctx->dev, vs, NULL);
    vkDestroyShaderModule(ctx->dev, fs, NULL);
    vkDestroyPipelineLayout(ctx->dev, pl, NULL);
    vkDestroyDescriptorPool(ctx->dev, dp, NULL);
    vkDestroyDescriptorSetLayout(ctx->dev, dsl, NULL);
    vkDestroyBuffer(ctx->dev, vb, NULL); vkFreeMemory(ctx->dev, vb_mem, NULL);
    vkDestroyBuffer(ctx->dev, ib, NULL); vkFreeMemory(ctx->dev, ib_mem, NULL);
    vkDestroyBuffer(ctx->dev, ub, NULL); vkFreeMemory(ctx->dev, ub_mem, NULL);
    vkDestroyBuffer(ctx->dev, dst_buf, NULL); vkFreeMemory(ctx->dev, dst_mem, NULL);
    vkDestroyFramebuffer(ctx->dev, fb, NULL);
    vkDestroyRenderPass(ctx->dev, rp, NULL);
    vkDestroyImageView(ctx->dev, view, NULL);
    vkDestroyImage(ctx->dev, img, NULL);
    vkFreeMemory(ctx->dev, img_mem, NULL);

    if (!err)
        printf("PASS graphics_indexed_ubo\n");
    return err;
}

static int test_compute_image_rw(struct vk_ctx *ctx)
{
    const uint32_t W = 4, H = 4;
    VkImage imgs[2];
    VkDeviceMemory img_mems[2];
    VkImageView views[2];
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R32_UINT, .extent = { W, H, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VK_CHECK(vkCreateImage(ctx->dev, &ici, NULL, &imgs[i]));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(ctx->dev, imgs[i], &req);
        uint32_t mt = find_mem_type(&ctx->mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mt,
        };
        VK_CHECK(vkAllocateMemory(ctx->dev, &mai, NULL, &img_mems[i]));
        VK_CHECK(vkBindImageMemory(ctx->dev, imgs[i], img_mems[i], 0));
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(ctx->dev, &vci, NULL, &views[i]));
    }

    VkBuffer src_buf, dst_buf;
    VkDeviceMemory src_mem, dst_mem;
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (create_buffer(ctx, W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, &src_buf, &src_mem)) return 1;
    if (create_buffer(ctx, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, &dst_buf, &dst_mem)) return 1;

    uint32_t *in_p;
    VK_CHECK(vkMapMemory(ctx->dev, src_mem, 0, W * H * 4, 0, (void **)&in_p));
    for (uint32_t i = 0; i < W * H; i++) in_p[i] = 10u + i;
    vkUnmapMemory(ctx->dev, src_mem);

    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = binds,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(ctx->dev, &dslci, NULL, &dsl));
    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
    VkDescriptorPool dp;
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps,
    };
    VK_CHECK(vkCreateDescriptorPool(ctx->dev, &dpci, NULL, &dp));
    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkAllocateDescriptorSets(ctx->dev, &dsai, &ds));
    VkDescriptorImageInfo diis[2] = {
        { .imageView = views[0], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = views[1], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &diis[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &diis[1] },
    };
    vkUpdateDescriptorSets(ctx->dev, 2, writes, 0, NULL);

    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
    };
    VK_CHECK(vkCreatePipelineLayout(ctx->dev, &plci, NULL, &pl));
    VkShaderModule cs;
    if (create_shader_module(ctx->dev, comp_img_rw_comp_spv, sizeof(comp_img_rw_comp_spv), &cs)) return 1;
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" },
        .layout = pl,
    };
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(ctx->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkCommandBuffer cmd;
    if (alloc_cmd(ctx, &cmd)) return 1;
    VkImageMemoryBarrier b0[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .image = imgs[0], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .image = imgs[1], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 2, b0);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyBufferToImage(cmd, src_buf, imgs[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = imgs[0],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkImageMemoryBarrier b2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .image = imgs[1],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b2);
    vkCmdCopyImageToBuffer(cmd, imgs[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buf, 1, &bic);
    if (submit_and_wait(ctx, cmd)) return 1;

    uint32_t *out;
    VK_CHECK(vkMapMemory(ctx->dev, dst_mem, 0, W * H * 4, 0, (void **)&out));
    int err = 0;
    for (uint32_t i = 0; i < W * H; i++) {
        uint32_t exp = (10u + i) * 3u + 1u;
        if (out[i] != exp) {
            fprintf(stderr, "FAIL compute_image_rw[%u]: got=%u exp=%u\n", i, out[i], exp);
            err = 1;
            break;
        }
    }
    vkUnmapMemory(ctx->dev, dst_mem);

    vkDestroyPipeline(ctx->dev, pipe, NULL);
    vkDestroyShaderModule(ctx->dev, cs, NULL);
    vkDestroyPipelineLayout(ctx->dev, pl, NULL);
    vkDestroyDescriptorPool(ctx->dev, dp, NULL);
    vkDestroyDescriptorSetLayout(ctx->dev, dsl, NULL);
    vkDestroyBuffer(ctx->dev, src_buf, NULL); vkFreeMemory(ctx->dev, src_mem, NULL);
    vkDestroyBuffer(ctx->dev, dst_buf, NULL); vkFreeMemory(ctx->dev, dst_mem, NULL);
    for (int i = 0; i < 2; i++) {
        vkDestroyImageView(ctx->dev, views[i], NULL);
        vkDestroyImage(ctx->dev, imgs[i], NULL);
        vkFreeMemory(ctx->dev, img_mems[i], NULL);
    }

    if (!err)
        printf("PASS compute_image_rw (4x4 imageLoad+imageStore)\n");
    return err;
}

int main(void)
{
    struct vk_ctx ctx = {0};

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "nvk_fermi_smoke",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
    };
    VK_CHECK(vkCreateInstance(&ici, NULL, &ctx.instance));

    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &count, NULL));
    if (count == 0) {
        fprintf(stderr, "No Vulkan physical devices found\n");
        return 1;
    }
    VkPhysicalDevice *devs = calloc(count, sizeof(*devs));
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &count, devs));
    ctx.phys = devs[0];
    free(devs);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx.phys, &props);
    printf("Device: %s (driverVersion %u.%u.%u)\n", props.deviceName,
           VK_VERSION_MAJOR(props.driverVersion),
           VK_VERSION_MINOR(props.driverVersion),
           VK_VERSION_PATCH(props.driverVersion));
    vkGetPhysicalDeviceMemoryProperties(ctx.phys, &ctx.mem_props);

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phys, &qf_count, NULL);
    VkQueueFamilyProperties *qfs = calloc(qf_count, sizeof(*qfs));
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phys, &qf_count, qfs);
    ctx.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        if ((qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            ctx.queue_family = i;
            break;
        }
    }
    free(qfs);
    if (ctx.queue_family == UINT32_MAX) {
        fprintf(stderr, "No graphics+compute queue family\n");
        return 1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VK_CHECK(vkCreateDevice(ctx.phys, &dci, NULL, &ctx.dev));
    vkGetDeviceQueue(ctx.dev, ctx.queue_family, 0, &ctx.queue);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = ctx.queue_family,
    };
    VK_CHECK(vkCreateCommandPool(ctx.dev, &cpci, NULL, &ctx.cmd_pool));

    int fails = 0;
    fails += test_graphics(&ctx, false, false);
    fails += test_graphics(&ctx, true, false);
    fails += test_graphics(&ctx, true, true);
    fails += test_compute_ssbo(&ctx);
    fails += test_compute_tex_dyn_txs(&ctx);
    fails += test_compute_storage_image(&ctx);
    fails += test_graphics_indexed_ubo(&ctx);
    fails += test_compute_image_rw(&ctx);

    vkDestroyCommandPool(ctx.dev, ctx.cmd_pool, NULL);
    vkDestroyDevice(ctx.dev, NULL);
    vkDestroyInstance(ctx.instance, NULL);

    if (fails) {
        fprintf(stderr, "FAILED %d test(s)\n", fails);
        return 1;
    }
    printf("ALL SMOKE TESTS PASSED\n");
    return 0;
}
