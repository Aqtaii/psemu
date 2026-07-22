// ============================================================================
// psemu: Kyty graphics subsystem init + Vulkan tetikleme.
// Kyty'nin normal emulator.cpp Init() dizisini (Common::SubsystemsList ile
// subsystem baslatma) BIZIM DERLEDIGIMIZ alt-kumeyle replike eder, sonra
// WindowInit + WindowRun (VulkanCreate: instance/device/swapchain). agc.cpp
// bunu ilk AGC cagrisinda BIR KEZ cagirir. (Audio/Controller/Network
// subsystem'leri derlemedigimiz icin cikarildi; graphics'in controller
// bagimliligi da dusuruldu.)
// Bu dosya gpu/ include yollariyla derlenir; agc.cpp yalnizca
// PsemuInitKytyGraphics()'i cagirir.
// ============================================================================
#include <cstdio>
#include <thread>

#define MARK(x) do { std::printf("[INIT-MARK] " x "\n"); std::fflush(stdout); } while (0)

#include "common/commonSubsystem.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/agc.h" // Libs::Graphics::GraphicsSubsystem
#include "loader/timer.h"

void PsemuInitKytyGraphics() {
	MARK("slist Instance");
	auto* slist = Common::SubsystemsList::Instance();

	MARK("subsystem Instance()'lari");
	auto* config      = Config::ConfigSubsystem::Instance();
	auto* core        = Common::CommonSubsystem::Instance();
	auto* threads     = Common::ThreadsSubsystem::Instance();
	auto* file_system = Libs::LibKernel::FileSystem::FileSystemSubsystem::Instance();
	auto* graphics    = Libs::Graphics::GraphicsSubsystem::Instance();
	auto* log         = Log::LogSubsystem::Instance();
	auto* memory      = Libs::LibKernel::Memory::MemorySubsystem::Instance();
	auto* profiler    = Profiler::ProfilerSubsystem::Instance();
	auto* pthread     = Libs::LibKernel::PthreadSubsystem::Instance();
	auto* timer       = Loader::Timer::TimerSubsystem::Instance();

	// 0. faz (Kyty main.cpp): core + threads. core dep'siz eklenmeli — yoksa
	// digerlerinin {core} bagimliligi ASLA tatmin olmaz (FindNextToInitialize
	// bilinmeyen dep'i bloke eder) ve hicbir subsystem init edilmez. ONCEKI
	// HATA BUYDU: core eklenmiyordu -> g_config null -> VulkanCreate crash.
	MARK("Add(core,threads); InitAll #0");
	slist->Add(core, {});
	slist->Add(threads, {core});
	slist->InitAll(true);

	// 1. faz (Kyty emulator.cpp): Config (digerleri config okur). Default OK.
	MARK("Add(config); InitAll #1");
	slist->Add(config, {core});
	slist->InitAll(true);

	// 2. faz: graphics ve bagimliliklari. (controller/audio/network yok)
	MARK("Add(digerleri); InitAll #2");
	slist->Add(file_system, {core, log, pthread});
	slist->Add(graphics, {core, log, pthread, memory, config, profiler});
	slist->Add(log, {core, config});
	slist->Add(memory, {core, log});
	slist->Add(profiler, {core, config});
	slist->Add(pthread, {core, log, timer});
	slist->Add(timer, {core, log});
	slist->InitAll(true);

	// NOT: WindowInit'i BURADA cagirmiyoruz — GraphicsSubsystem::Init() zaten
	// cagirdi (WindowInit + VideoOutInit + render/shader init, Config dims ile).
	// Cift cagri EXIT_IF(g_window_ctx != nullptr) fatal'ine yol aciyordu.
	MARK("WindowRun thread");
	std::thread([] { Libs::Graphics::WindowRun(); }).detach();
	MARK("WaitForGraphicInitialized");
	Libs::Graphics::WindowWaitForGraphicInitialized();
	MARK("BITTI - Vulkan hazir");
}
