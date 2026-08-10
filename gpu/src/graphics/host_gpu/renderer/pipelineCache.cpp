#include "graphics/host_gpu/renderer/pipelineCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/psemuGfxTrace.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"

#include <atomic>
#include <cstring>
#include <span>
#include <utility>

namespace Libs::Graphics {

namespace {

void NormalizeStaticParamsForDynamicState(PipelineStaticParameters& static_params) {
	static_params.viewport_scale[0]  = 0.5f;
	static_params.viewport_scale[1]  = 0.5f;
	static_params.viewport_scale[2]  = 1.0f;
	static_params.viewport_offset[0] = 0.5f;
	static_params.viewport_offset[1] = 0.5f;
	static_params.viewport_offset[2] = 0.0f;

	static_params.scissor_ltrb[0] = 0;
	static_params.scissor_ltrb[1] = 0;
	static_params.scissor_ltrb[2] = 1;
	static_params.scissor_ltrb[3] = 1;
}

} // namespace

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::GraphicsPipeline* PipelineCache::CreateGraphicsPipeline(
    VulkanFramebuffer* framebuffer, RenderColorInfo* colors, uint32_t color_count,
    RenderDepthInfo* depth, ShaderVertexInputInfo* vs_input_info, HW::Context* ctx,
    HW::Shader* sh_ctx, ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool ps_active, std::span<const uint32_t> vs_spirv, std::span<const uint32_t> ps_spirv,
    bool mesh) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(framebuffer == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(colors == nullptr);
	EXIT_IF(color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(vs_spirv.empty());
	EXIT_IF(ps_active && ps_spirv.empty());

	Common::LockGuard lock(m_mutex);

	const auto&           vertex_info                              = sh_ctx->GetVs();
	const auto&           ps_regs                                  = sh_ctx->GetPs();
	const HW::BlendColor& bclr                                     = ctx->GetBlendColor();
	uint32_t              color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_mask[i] = (colors[i].vulkan_buffer != nullptr
		                     ? colors[i].export_mapping.ApplyMask(render_target_mask_slot(
		                           ctx->GetRenderTargetMask(), colors[i].target_slot))
		                     : 0);
	}
	// KESKIN NISANCI: indirilen hedef her pikselde RGB=0, A=1.0 iceriyor
	// (dolu_bayt = piksel sayisi, ilk_dolu=7, en_parlak=0x3C). Bu desenin en
	// dogal aciklamasi renk yazma maskesinin YALNIZCA alfayi gecirmesi
	// (color_mask=0x8). Maske iki girdiden dogar; ikisini de ayri ayri bas ki
	// hangisi kirpiyorsa gorunsun.
	if (PsemuGfxTrace()) {
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 400) {
			for (uint32_t i = 0; i < color_count; i++) {
				std::printf("[RENK-MASKE] mesh=%d ek=%u/%u slot=%u ham_slot_maske=0x%01x "
				            "esleme=0x%02x -> color_mask=0x%01x tampon=%p\n",
				            static_cast<int>(mesh), i, color_count, colors[i].target_slot,
				            render_target_mask_slot(ctx->GetRenderTargetMask(),
				                                    colors[i].target_slot),
				            static_cast<uint32_t>(colors[i].export_mapping.packed), color_mask[i],
				            static_cast<const void*>(colors[i].vulkan_buffer));
				std::fflush(stdout);
			}
		}
	}
	const HW::ModeControl& mc = ctx->GetModeControl();

	auto     vs_id = ShaderGetIdVS(&vertex_info, vs_input_info, true);
	ShaderId ps_id {};
	if (ps_active) {
		ps_id = ShaderGetIdPS(&ps_regs, ps_input_info, true);
	}

	PipelineStaticParameters static_params {};
	GraphicsPipeline         p {};
	p.render_pass_id = framebuffer->render_pass_id;
	p.ps_shader_id   = ps_id;
	p.vs_shader_id   = vs_id;

	static_params.color_count = color_count;

	if (ps_active && depth->depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	static_params.negative_one_to_one = !ctx->GetClipControl().dx_clip_space;
	static_params.topology            = topology;
	static_params.mesh                = mesh;
	static_params.with_depth =
	    (depth->format != vk::Format::eUndefined && depth->vulkan_buffer != nullptr);
	static_params.depth_test_enable  = depth->depth_test_enable;
	static_params.depth_write_enable = (depth->depth_write_enable && !depth->depth_clear_enable);
	static_params.depth_compare_op   = depth->depth_compare_op;
	static_params.depth_bounds_test_enable = depth->depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth->depth_min_bounds;
	static_params.depth_max_bounds         = depth->depth_max_bounds;
	static_params.stencil_test_enable      = depth->stencil_test_enable;
	static_params.stencil_front            = depth->stencil_static_front;
	static_params.stencil_back             = depth->stencil_static_back;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		static_params.color_mask[i] = color_mask[i];
	}
	static_params.cull_back  = false;
	static_params.cull_front = false;
	static_params.face       = mc.face;

	for (uint32_t i = 0; i < color_count; i++) {
		const auto& rt                        = ctx->GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx->GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[i]       = bc.color_srcblend;
		static_params.color_comb_fcn[i]       = bc.color_comb_fcn;
		static_params.color_destblend[i]      = bc.color_destblend;
		static_params.alpha_srcblend[i]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[i]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[i]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[i] = bc.separate_alpha_blend;
		static_params.blend_enable[i]         = bc.enable;
		static_params.blend_bypass[i]         = rt.info.blend_bypass;
	}
	static_params.blend_color_red   = bclr.red;
	static_params.blend_color_green = bclr.green;
	static_params.blend_color_blue  = bclr.blue;
	static_params.blend_color_alpha = bclr.alpha;

	// KESKIN NISANCI: renk maskesi 0xf/0x7 cikti (RGB yazilabilir), hedef yine
	// RGB=0 donuyor. Geriye harmanlama kaliyor: srcblend=ZERO ya da
	// dst=ZERO/ONE kombinasyonu, yazilan rengi sessizce yok eder ve cizim
	// yine "yapildi" gorunur. Maskeyle ayni yerden bas ki iki girdi tek
	// satirda karsilastirilabilsin.
	if (PsemuGfxTrace() && mesh) {
		static std::atomic<uint32_t> s_nb {0};
		if (s_nb.fetch_add(1) < 400) {
			for (uint32_t i = 0; i < color_count; i++) {
				std::printf("[HARMAN] mesh=1 ek=%u maske=0x%01x harman_acik=%d bypass=%d "
				            "renk_src=%u renk_op=%u renk_dst=%u alfa_ayri=%d alfa_src=%u "
				            "alfa_op=%u alfa_dst=%u\n",
				            i, color_mask[i], static_cast<int>(static_params.blend_enable[i]),
				            static_cast<int>(static_params.blend_bypass[i]),
				            static_cast<uint32_t>(static_params.color_srcblend[i]),
				            static_cast<uint32_t>(static_params.color_comb_fcn[i]),
				            static_cast<uint32_t>(static_params.color_destblend[i]),
				            static_cast<int>(static_params.separate_alpha_blend[i]),
				            static_cast<uint32_t>(static_params.alpha_srcblend[i]),
				            static_cast<uint32_t>(static_params.alpha_comb_fcn[i]),
				            static_cast<uint32_t>(static_params.alpha_destblend[i]));
				std::fflush(stdout);
			}
		}
	}

	NormalizeStaticParamsForDynamicState(static_params);

	GraphicsPipelineKey key {};
	key.render_pass_id = p.render_pass_id;
	key.vs_shader_id   = p.vs_shader_id;
	key.ps_shader_id   = p.ps_shader_id;
	key.static_params  = static_params;

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return iter->second.get();
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(ps_input_info);
		}
		LOGF("PipelineTrace: shader binaries VS=0x%08" PRIx32 "/0x%08" PRIx32 " words=%" PRIu64
		     " PS=0x%08" PRIx32 "/0x%08" PRIx32 " words=%" PRIu64 "\n",
		     vs_id.hash0, vs_id.crc32, static_cast<uint64_t>(vs_spirv.size()), ps_id.hash0,
		     ps_id.crc32, static_cast<uint64_t>(ps_spirv.size()));
	}

	auto cached = std::make_unique<GraphicsPipeline>(p);
	LogPipelineTrace("CreatePipelineInternal begin", vs_id.hash0, vs_id.crc32, ps_id.hash0,
	                 ps_id.crc32);
	CreatePipelineInternal(cached.get(), framebuffer->render_pass, vs_input_info, vs_spirv,
	                       ps_input_info, ps_spirv, static_params, vs_id.hash0, vs_id.crc32,
	                       ps_id.hash0, ps_id.crc32, ps_active, mesh);
	LogPipelineTrace("CreatePipelineInternal done", vs_id.hash0, vs_id.crc32, ps_id.hash0,
	                 ps_id.crc32);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return iter->second.get();
}

PipelineCache::ComputePipeline*
PipelineCache::CreateComputePipeline(ShaderComputeInputInfo*      input_info,
                                     const HW::ComputeShaderInfo* cs_regs,
                                     std::span<const uint32_t>    cs_spirv) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(cs_regs == nullptr);
	EXIT_IF(cs_spirv.empty());

	Common::LockGuard lock(m_mutex);

	auto cs_id = ShaderGetIdCS(cs_regs, input_info, true);

	ComputePipeline p {};
	p.cs_shader_id = cs_id;

	ComputePipelineKey key {};
	key.cs_shader_id = p.cs_shader_id;

	if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
		return iter->second.get();
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<ComputePipeline>(p);
	CreatePipelineInternal(cached.get(), input_info, cs_spirv);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return iter->second.get();
}
} // namespace Libs::Graphics
