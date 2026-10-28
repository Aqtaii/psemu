// Replay caller-supplied production opening-video SPIR-V with synthetic NV12.
// No proprietary shader bytes or game pixels are embedded in this test.
#define NOMINMAX
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(#x);                                            \
  } while (0)
#define VKOK(x) CHECK((x) == VK_SUCCESS)
std::vector<uint32_t> code(const char *path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  CHECK(f);
  size_t n = size_t(f.tellg());
  CHECK(n && n % 4 == 0);
  std::vector<uint32_t> r(n / 4);
  f.seekg(0);
  f.read(reinterpret_cast<char *>(r.data()), n);
  CHECK(f);
  return r;
}
int main(int argc, char **argv) {
  try {
    CHECK(argc >= 3 && argc <= 5);
    const bool native_vertex = argc >= 4;
    const bool video_quad = argc == 5;
    CHECK(!video_quad || std::strcmp(argv[4], "video-quad") == 0);
    CHECK(!native_vertex || std::strcmp(argv[3], "native-vertex") == 0);
    auto vs = code(argv[1]), ps = code(argv[2]);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    VKOK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 0;
    VKOK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    CHECK(n);
    std::vector<VkPhysicalDevice> pp(n);
    VKOK(vkEnumeratePhysicalDevices(instance, &n, pp.data()));
    auto physical = pp[0];
    VkPhysicalDeviceProperties prop{};
    vkGetPhysicalDeviceProperties(physical, &prop);
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> families(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nf, families.data());
    uint32_t family = 0;
    while (family < nf &&
           !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      ++family;
    CHECK(family < nf);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VkPhysicalDeviceFeatures supported{}, enabled{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    CHECK(supported.fragmentStoresAndAtomics);
    CHECK(!native_vertex || supported.vertexPipelineStoresAndAtomics);
    // Production buffer declarations are conservatively writable in SPIR-V.
    enabled.fragmentStoresAndAtomics = VK_TRUE;
    enabled.vertexPipelineStoresAndAtomics = native_vertex ? VK_TRUE : VK_FALSE;
    di.pEnabledFeatures = &enabled;
    VkDevice device;
    VKOK(vkCreateDevice(physical, &di, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    auto alloc = [&](VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
      uint32_t t = 0;
      while (t < mp.memoryTypeCount &&
             (!(req.memoryTypeBits & (1u << t)) ||
              (mp.memoryTypes[t].propertyFlags & flags) != flags))
        ++t;
      CHECK(t < mp.memoryTypeCount);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.memoryTypeIndex = t;
      ai.allocationSize = req.size;
      VkDeviceMemory m;
      VKOK(vkAllocateMemory(device, &ai, nullptr, &m));
      return m;
    };
    struct Buffer {
      VkBuffer h;
      VkDeviceMemory memory;
      void *mapped;
    };
    auto buffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage) {
      Buffer b{};
      VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      ci.size = bytes;
      ci.usage = usage;
      VKOK(vkCreateBuffer(device, &ci, nullptr, &b.h));
      VkMemoryRequirements req{};
      vkGetBufferMemoryRequirements(device, b.h, &req);
      b.memory = alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VKOK(vkBindBufferMemory(device, b.h, b.memory, 0));
      VKOK(vkMapMemory(device, b.memory, 0, bytes, 0, &b.mapped));
      return b;
    };
    constexpr uint32_t W = 64, H = 48, OW = 8, OH = 8;
    auto staging = buffer(W * H * 3 / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto output = buffer(OW * OH * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto constants = buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    float width = float(W);
    std::memcpy(constants.mapped, &width, 4);
    auto matrix = buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto vertices = buffer(144, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::array<float, 16> identity{};
    for (int i = 0; i < 4; ++i)
      identity[i * 5] = 1;
    if (video_quad)
      identity[10] = 0;
    std::memcpy(matrix.mapped, identity.data(), 64);
    const float tri[] = {-1, -1, 0, 1,  0, 0, 3, -1, 0,
                         1,  2,  0, -1, 3, 0, 1, 0,  2};
    std::memcpy(vertices.mapped, tri, 72);
    if (video_quad) {
      // Conventional six-vertex rectangle with top-left UV origin, as observed
      // in the real video draw. This is synthetic geometry, not captured bytes.
      const float quad[] = {-1, 1,  0, 1, 0, 0, -1, -1, 0, 1, 0, 1,
                            1,  -1, 0, 1, 1, 1, -1, 1,  0, 1, 0, 0,
                            1,  -1, 0, 1, 1, 1, 1,  1,  0, 1, 1, 0};
      std::memcpy(vertices.mapped, quad, sizeof(quad));
    }
    struct Image {
      VkImage h;
      VkDeviceMemory memory;
      VkImageView view;
    };
    auto image = [&](uint32_t w, uint32_t h, VkFormat format,
                     VkImageUsageFlags usage) {
      Image r{};
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = format;
      ci.extent = {w, h, 1};
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = usage;
      VKOK(vkCreateImage(device, &ci, nullptr, &r.h));
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(device, r.h, &req);
      r.memory = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VKOK(vkBindImageMemory(device, r.h, r.memory, 0));
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = r.h;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VKOK(vkCreateImageView(device, &vi, nullptr, &r.view));
      return r;
    };
    auto source =
        image(W, H * 3 / 2, VK_FORMAT_R8_UNORM,
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto target = image(OW, OH, VK_FORMAT_R32G32B32A32_SFLOAT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    VKOK(vkCreateSampler(device, &si, nullptr, &sampler));
    VkDescriptorSetLayoutBinding bb[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr}};
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout empty, layout;
    VkDescriptorSetLayoutBinding vb{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2,
                                    VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    if (native_vertex) {
      li.bindingCount = 1;
      li.pBindings = &vb;
    }
    VKOK(vkCreateDescriptorSetLayout(device, &li, nullptr, &empty));
    li.bindingCount = 3;
    li.pBindings = bb;
    VKOK(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
    VkDescriptorPoolSize pools[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = pools;
    VkDescriptorPool pool;
    VKOK(vkCreateDescriptorPool(device, &pi, nullptr, &pool));
    VkDescriptorSetAllocateInfo dai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &layout;
    VkDescriptorSet set;
    VKOK(vkAllocateDescriptorSets(device, &dai, &set));
    VkDescriptorSet vertex_set{};
    if (native_vertex) {
      dai.pSetLayouts = &empty;
      VKOK(vkAllocateDescriptorSets(device, &dai, &vertex_set));
      VkDescriptorBufferInfo inputs[] = {
          {matrix.h, 0, 64}, {vertices.h, 0, video_quad ? 144u : 72u}};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = vertex_set;
      w.descriptorCount = 2;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w.pBufferInfo = inputs;
      vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
    VkDescriptorBufferInfo bi{constants.h, 0, 4};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, source.view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        ss{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = bb[i].descriptorType;
    }
    writes[0].pBufferInfo = &bi;
    writes[1].pImageInfo = &ii;
    writes[2].pImageInfo = &ss;
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    VkPushConstantRange push{
        VkShaderStageFlags(VK_SHADER_STAGE_FRAGMENT_BIT |
                           (native_vertex ? VK_SHADER_STAGE_VERTEX_BIT : 0)),
        0, 64};
    VkDescriptorSetLayout sets[] = {empty, layout};
    VkPipelineLayoutCreateInfo pli{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    VkPipelineLayout pipeline_layout;
    VKOK(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout));
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0] = {VK_SUBPASS_EXTERNAL,
               0,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               0,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               0};
    deps[1] = {0,
               VK_SUBPASS_EXTERNAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT,
               0};
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = 1;
    ri.pAttachments = &attachment;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 2;
    ri.pDependencies = deps;
    VkRenderPass renderpass;
    VKOK(vkCreateRenderPass(device, &ri, nullptr, &renderpass));
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = renderpass;
    fi.attachmentCount = 1;
    fi.pAttachments = &target.view;
    fi.width = OW;
    fi.height = OH;
    fi.layers = 1;
    VkFramebuffer framebuffer;
    VKOK(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer));
    auto module = [&](const std::vector<uint32_t> &words) {
      VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = words.size() * 4;
      ci.pCode = words.data();
      VkShaderModule s;
      VKOK(vkCreateShaderModule(device, &ci, nullptr, &s));
      return s;
    };
    auto vm = module(vs), pm = module(ps);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; ++i) {
      stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage =
          i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
      stages[i].module = i ? pm : vm;
      stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(OW), float(OH), 0, 1};
    if (video_quad) {
      vp.y = float(OH);
      vp.height = -float(OH);
    }
    VkRect2D sc{{0, 0}, {OW, OH}};
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo gpi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vertex;
    gpi.pInputAssemblyState = &assembly;
    gpi.pViewportState = &viewport;
    gpi.pRasterizationState = &raster;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.layout = pipeline_layout;
    gpi.renderPass = renderpass;
    VkPipeline pipeline;
    VKOK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr,
                                   &pipeline));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cp;
    VKOK(vkCreateCommandPool(device, &cpi, nullptr, &cp));
    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cp;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VKOK(vkAllocateCommandBuffers(device, &cai, &cmd));
    const std::array<std::array<uint8_t, 3>, 4> cases{
        {{16, 128, 128}, {200, 128, 128}, {100, 90, 240}, {16, 128, 128}}};
    for (size_t test = 0; test < cases.size(); ++test) {
      auto [y, u, v] = cases[test];
      auto *data = static_cast<uint8_t *>(staging.mapped);
      std::memset(data, y, W * H);
      if (test == 3)
        for (uint32_t py = 0; py < H; ++py)
          for (uint32_t px = 0; px < W; ++px)
            data[py * W + px] =
                ((px / (W / OW) + py / (H / OH)) & 1u) ? 200 : 16;
      for (size_t i = W * H; i < W * H * 3 / 2; i += 2) {
        data[i] = u;
        data[i + 1] = v;
      }
      VKOK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      VKOK(vkBeginCommandBuffer(cmd, &begin));
      VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      host.dstAccessMask =
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                           0, 1, &host, 0, nullptr, 0, nullptr);
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = test ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcAccessMask = test ? VK_ACCESS_SHADER_READ_BIT : 0;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
          VK_QUEUE_FAMILY_IGNORED;
      barrier.image = source.h;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd,
                           test ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {W, H * 3 / 2, 1};
      vkCmdCopyBufferToImage(cmd, staging.h, source.h,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
      VkClearValue clear{};
      clear.color.float32[0] = -9;
      VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rb.renderPass = renderpass;
      rb.framebuffer = framebuffer;
      rb.renderArea = sc;
      rb.clearValueCount = 1;
      rb.pClearValues = &clear;
      vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (native_vertex) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, 0, 1, &vertex_set, 0, nullptr);
      }
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_layout, 1, 1, &set, 0, nullptr);
      std::array<uint32_t, 16> zeros{};
      vkCmdPushConstants(cmd, pipeline_layout, push.stageFlags, 0, 64,
                         zeros.data());
      vkCmdDraw(cmd, video_quad ? 6 : 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
      copy.imageExtent = {OW, OH, 1};
      vkCmdCopyImageToBuffer(cmd, target.h,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.h, 1,
                             &copy);
      VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ready.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &ready, 0, nullptr,
                           0, nullptr);
      VKOK(vkEndCommandBuffer(cmd));
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd;
      VKOK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      VKOK(vkQueueWaitIdle(queue));
      auto *rgb = static_cast<float *>(output.mapped);
      float yy = float(y) / 255.f, uu = float(u) / 255.f, vv = float(v) / 255.f;
      std::array<float, 4> expected{
          1.1644f * (yy - 1.f / 16.f) + 1.7927f * (vv - .5f),
          1.1644f * (yy - 1.f / 16.f) - .2133f * (uu - .5f) -
              .5329f * (vv - .5f),
          1.1644f * (yy - 1.f / 16.f) + 2.1124f * (uu - .5f), 1};
      float error = 0;
      for (size_t i = 0; i < OW * OH; ++i) {
        auto pixel_expected = expected;
        if (test == 3 && ((i % OW + i / OW) & 1u)) {
          const float delta = 1.1644f * (200.f - 16.f) / 255.f;
          for (size_t c = 0; c < 3; ++c)
            pixel_expected[c] += delta;
        }
        for (size_t c = 0; c < 4; ++c) {
          CHECK(std::isfinite(rgb[i * 4 + c]));
          error = std::max(error, std::abs(rgb[i * 4 + c] - pixel_expected[c]));
        }
      }
      std::printf("[ASTRO-VIDEO-SHADER-PROBE] device=%s case=%zu YUV=%u,%u,%u "
                  "rgb=%g,%g,%g,%g expected=%g,%g,%g,%g max_error=%g "
                  "game_frame=not-tested\n",
                  prop.deviceName, test, y, u, v, rgb[0], rgb[1], rgb[2],
                  rgb[3], expected[0], expected[1], expected[2], expected[3],
                  error);
      CHECK(error < .006f);
    }
    std::printf("[ASTRO-VIDEO-SHADER-PROBE] native_vertex=%u native_fragment=1 "
                "synthetic_input=1 video_quad=%u negative_viewport=%u "
                "patterned_case=checked\n",
                native_vertex, video_quad, video_quad);
    VKOK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, cp, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, vm, nullptr);
    vkDestroyShaderModule(device, pm, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, renderpass, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, empty, nullptr);
    vkDestroySampler(device, sampler, nullptr);
    for (auto r : {source, target}) {
      vkDestroyImageView(device, r.view, nullptr);
      vkDestroyImage(device, r.h, nullptr);
      vkFreeMemory(device, r.memory, nullptr);
    }
    for (auto b : {staging, output, constants, matrix, vertices}) {
      vkUnmapMemory(device, b.memory);
      vkDestroyBuffer(device, b.h, nullptr);
      vkFreeMemory(device, b.memory, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
  } catch (const std::exception &e) {
    std::printf("[ASTRO-VIDEO-SHADER-PROBE] FAIL %s\n", e.what());
    return 1;
  }
}
