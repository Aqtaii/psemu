#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/asyncJob.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/displayBuffer.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <list>
#include <map>   // m_mutex sahiplik tablosu (tani)
#include <mutex> // sahiplik tablosunun kendi kilidi (tani)
#include <chrono> // kademeli geri cekilme: sleep_for
#include <thread>
#include <vector>

#include <windows.h> // GetCurrentThreadId (tani: kilit sahibi)

namespace Libs::Graphics {

// GPU anlik goruntu (govdeleri asagida): sicak yol isareti, thread adi ve
// takilma nobetcisi.
void PsemuGpuMark(const char* site, uint64_t a, uint64_t b, uint64_t c);
void PsemuGpuMarkIdle(const char* site, uint64_t a, uint64_t b, uint64_t c);
void PsemuGpuThreadName(const char* name);
void PsemuGpuStallWatchdogStart();

// ============================================================================
// TANI: m_mutex SAHIPLIK IZI
// ----------------------------------------------------------------------------
// Olculdu: bir CommandProcessor::BufferFlush cagrisi m_mutex'i bekleyerek
// sonsuza kadar asili kaliyor (bekleyen 24 / alan 23) ve surec ~170 sn
// donuyor. Vulkan gonderimi ve fence beklemesi SAGLIKLI (23/23), yani sorun
// CPU tarafinda: kilidi biri alip birakmiyor.
//
// Tek tek fonksiyon avlamak yerine (bu oturumda sayac farkindan yanlis
// "takilma" sonucu cikarmak alti kez yanilttı) DOGRUDAN SAHIPLIK yaziyoruz:
// her alim noktasi, kilidi alan thread'i ve fonksiyon adini kaydeder;
// birakinca siler. BufferFlush beklemeye girerken "su an kim tutuyor"
// sorusunu bu tablodan cevaplar. Sayac orani degil, sahiplik.
//
// Kayit mutex ADRESINE gore: her CommandProcessor'un kendi m_mutex'i var.
// ============================================================================
namespace {
struct CpLockInfo {
	unsigned long tid  = 0;
	const char*   site = nullptr;
};
std::mutex                             g_cp_lock_mtx;
std::map<const void*, CpLockInfo>      g_cp_lock_owner;

void CpLockSet(const void* m, const char* site) {
	std::lock_guard<std::mutex> lk(g_cp_lock_mtx);
	g_cp_lock_owner[m] = CpLockInfo {static_cast<unsigned long>(GetCurrentThreadId()), site};
}
void CpLockClear(const void* m) {
	std::lock_guard<std::mutex> lk(g_cp_lock_mtx);
	g_cp_lock_owner.erase(m);
}
CpLockInfo CpLockGet(const void* m) {
	std::lock_guard<std::mutex> lk(g_cp_lock_mtx);
	auto it = g_cp_lock_owner.find(m);
	return (it == g_cp_lock_owner.end()) ? CpLockInfo {} : it->second;
}

// LockGuard'dan SONRA kurulur (yani kilit alinmisken), ondan ONCE yikilir.
struct CpLockScope {
	explicit CpLockScope(const void* m, const char* site): m_m(m) { CpLockSet(m, site); }
	~CpLockScope() { CpLockClear(m_m); }
	CpLockScope(const CpLockScope&)            = delete;
	CpLockScope& operator=(const CpLockScope&) = delete;
	const void* m_m;
};
} // namespace


// TANI koprusu (psemu src/core.cpp): sayfa durumu/korumasi + tahsisat tabani.
extern "C" unsigned long long PsemuQueryProtect(unsigned long long addr,
                                                unsigned long long* out_alloc_base);

static thread_local CommandProcessor* g_current_run_cp         = nullptr;
static thread_local uint32_t          g_submission_pause_depth = 0;
static thread_local bool              g_gpu_mutex_owned        = false;

class GpuMutexLock final {
public:
	explicit GpuMutexLock(Common::Mutex& mutex): m_mutex(mutex) {
		if (g_gpu_mutex_owned) {
			EXIT("recursive GPU mutex acquisition\n");
		}
		g_gpu_mutex_owned = true;
		m_mutex.Lock();
	}
	~GpuMutexLock() {
		if (!g_gpu_mutex_owned) {
			EXIT("invalid GPU mutex release\n");
		}
		m_mutex.Unlock();
		g_gpu_mutex_owned = false;
	}

private:
	Common::Mutex& m_mutex;
};

struct OwnedCmdBuffer {
	std::vector<uint32_t> storage;
	uint32_t*             data   = nullptr;
	uint32_t              num_dw = 0;

	OwnedCmdBuffer() = default;
	OwnedCmdBuffer(const uint32_t* src, uint32_t count) { Assign(src, count); }

	OwnedCmdBuffer(const OwnedCmdBuffer& other): storage(other.storage), num_dw(other.num_dw) {
		data = (storage.empty() ? other.data : storage.data());
	}

	OwnedCmdBuffer& operator=(const OwnedCmdBuffer& other) {
		if (this != &other) {
			storage = other.storage;
			num_dw  = other.num_dw;
			data    = (storage.empty() ? other.data : storage.data());
		}
		return *this;
	}

	OwnedCmdBuffer(OwnedCmdBuffer&& other) noexcept
	    : storage(std::move(other.storage)), data(other.data), num_dw(other.num_dw) {
		data = (storage.empty() ? other.data : storage.data());
	}

	OwnedCmdBuffer& operator=(OwnedCmdBuffer&& other) noexcept {
		if (this != &other) {
			storage = std::move(other.storage);
			num_dw  = other.num_dw;
			data    = (storage.empty() ? other.data : storage.data());
		}
		return *this;
	}

	void Assign(const uint32_t* src, uint32_t count) {
		num_dw = count;
		if (src != nullptr && count != 0) {
			storage.assign(src, src + count);
			data = storage.data();
		} else {
			storage.clear();
			data = nullptr;
		}
	}
};

class GraphicsRing {
public:
	GraphicsRing(): m_draw_job("Thread_Gfx_Draw"), m_constant_job("Thread_Gfx_Const") {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	}
	~GraphicsRing() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(GraphicsRing);

	void Submit(OwnedCmdBuffer draw_buffer, OwnedCmdBuffer const_buffer, int handle, int index,
	            int flip_mode, int64_t flip_arg, bool trigger_agc_interrupt_on_done);
	void SubmitFlipPreparation();
	void Done();
	void WaitForIdle();
	bool IsIdle();

	void SetCp(CommandProcessor* cp) {
		m_cp = cp;
		Start();
	}

private:
	void Start() {
		Common::Thread t(ThreadBatchRun, this);
		t.Detach();
	}

	struct CmdBatch {
		OwnedCmdBuffer draw_buffer;
		OwnedCmdBuffer const_buffer;

		CommandProcessor::FlipInfo flip;
		bool                       trigger_agc_interrupt_on_done = false;
		bool                       prepare_cpu_flip              = false;
	};

	static void ThreadBatchRun(void* data);

	CmdBatch GetCmdBatch();

	Common::Mutex       m_mutex;
	Common::CondVar     m_cond_var;
	Common::CondVar     m_idle_cond_var;
	std::list<CmdBatch> m_cmd_batches;
	bool                m_done = true;
	bool                m_idle = true;

	AsyncJob m_draw_job;
	AsyncJob m_constant_job;

	CommandProcessor* m_cp = nullptr;
};

class ComputeRing {
public:
	ComputeRing() = default;
	~ComputeRing() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(ComputeRing);

	void Submit(OwnedCmdBuffer buffer, bool trigger_agc_interrupt_on_done);
	void Done();
	void WaitForIdle();
	bool IsIdle();

	void SetCp(CommandProcessor* cp) {
		m_cp = cp;
		Start();
	}

	[[nodiscard]] int GetQueueId() const { return m_queue_id; }
	void              SetQueueId(int id) { m_queue_id = id; }

private:
	void Start() {
		Common::Thread t(ThreadRun, this);
		t.Detach();
	}

	static void ThreadRun(void* data);

	Common::Mutex   m_mutex;
	Common::CondVar m_cond_var;
	Common::CondVar m_idle_cond_var;
	bool            m_done = true;
	bool            m_idle = true;

	CommandProcessor* m_cp       = nullptr;
	int               m_queue_id = -1;

	struct DirectBatch {
		OwnedCmdBuffer buffer;
		bool           trigger_agc_interrupt_on_done = false;
	};

	std::list<DirectBatch> m_direct_batches;
};

class Gpu {
public:
	Gpu() {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		Init();
	}
	~Gpu() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(Gpu);

	void Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
	            uint32_t num_const_dw, bool trigger_agc_interrupt_on_done);
	void SubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
	                   bool trigger_agc_interrupt_on_done);
	void SubmitFlipPreparation();
	void Done();
	void PauseSubmissions();
	void ResumeSubmissions();
	int  GetFrameNum();

private:
	void Init();
	void WaitLocked();

	ComputeRing* GetRing(uint32_t ring_id);

	Common::Mutex m_mutex;

	CommandProcessor* m_gfx_cp   = nullptr;
	GraphicsRing*     m_gfx_ring = nullptr;

	CommandProcessor* m_compute_cp[8]    = {};
	ComputeRing*      m_compute_ring[64] = {};

	std::atomic_int m_done_num = 0;
};

static Gpu* g_gpu = nullptr;

static bool GraphicsRunDebugDumpEnabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::OutputDirection::Silent;
}

void GraphicsRunInit() {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	EXIT_IF(g_gpu != nullptr);

	GraphicsInitJmpTables();

	PsemuGpuStallWatchdogStart(); // takilma anlik goruntusu

	g_gpu = new Gpu;
}

void Gpu::Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
                 uint32_t num_const_dw, bool trigger_agc_interrupt_on_done) {
	OwnedCmdBuffer draw_buffer(cmd_draw_buffer, num_draw_dw);
	OwnedCmdBuffer const_buffer(cmd_const_buffer, num_const_dw);
	GpuMutexLock   lock(m_mutex);

	m_gfx_ring->Submit(std::move(draw_buffer), std::move(const_buffer), 0, 0, 0, 0,
	                   trigger_agc_interrupt_on_done);
}

void Gpu::SubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                        bool trigger_agc_interrupt_on_done) {
	OwnedCmdBuffer buffer(cmd_buffer, num_dw);
	GpuMutexLock   lock(m_mutex);

	constexpr uint32_t compute_queue_base = 0x20u;
	constexpr uint32_t compute_queue_num  = 7u * 8u;
	EXIT_NOT_IMPLEMENTED(queue < compute_queue_base ||
	                     queue >= compute_queue_base + compute_queue_num);

	uint32_t compute_queue = queue - compute_queue_base;
	uint32_t pipe_id       = (compute_queue >> 3u) & 0x7u;
	uint32_t queue_id      = compute_queue & 0x7u;
	EXIT_NOT_IMPLEMENTED(pipe_id >= 7u);
	EXIT_NOT_IMPLEMENTED(queue_id >= 8u);

	uint32_t ring_id = compute_queue + 1u;

	auto* ring = GetRing(ring_id);

	ring->Submit(std::move(buffer), trigger_agc_interrupt_on_done);
}

void Gpu::SubmitFlipPreparation() {
	GpuMutexLock lock(m_mutex);
	m_gfx_ring->SubmitFlipPreparation();
}

void Gpu::Done() {
	GraphicsRing*     gfx_ring = nullptr;
	CommandProcessor* gfx_cp   = nullptr;
	ComputeRing*      compute_rings[64] {};
	CommandProcessor* compute_cps[8] {};

	{
		GpuMutexLock lock(m_mutex);

		gfx_ring = m_gfx_ring;
		gfx_cp   = m_gfx_cp;

		std::copy(std::begin(m_compute_ring), std::end(m_compute_ring), std::begin(compute_rings));
		std::copy(std::begin(m_compute_cp), std::end(m_compute_cp), std::begin(compute_cps));

		m_done_num++;
	}

	if (gfx_ring != nullptr) {
		gfx_ring->Done();
	}
	for (auto& cr: compute_rings) {
		if (cr != nullptr) {
			cr->Done();
		}
	}
	if (gfx_ring != nullptr) {
		gfx_ring->WaitForIdle();
	}
	if (gfx_cp != nullptr) {
		gfx_cp->BufferWait();
	}
	for (auto& cr: compute_rings) {
		if (cr != nullptr) {
			cr->WaitForIdle();
		}
	}
	for (auto& cp: compute_cps) {
		if (cp != nullptr) {
			cp->BufferWait();
		}
	}
}

int Gpu::GetFrameNum() {
	// Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	return m_done_num;
}

void Gpu::WaitLocked() {
	GraphicsRing*     gfx_ring = nullptr;
	CommandProcessor* gfx_cp   = nullptr;
	ComputeRing*      compute_rings[64] {};
	CommandProcessor* compute_cps[8] {};

	gfx_ring = m_gfx_ring;
	gfx_cp   = m_gfx_cp;
	std::copy(std::begin(m_compute_ring), std::end(m_compute_ring), std::begin(compute_rings));
	std::copy(std::begin(m_compute_cp), std::end(m_compute_cp), std::begin(compute_cps));

	if (gfx_ring != nullptr) {
		gfx_ring->WaitForIdle();
	}
	if (gfx_cp != nullptr) {
		gfx_cp->BufferWait();
	}
	for (auto& cr: compute_rings) {
		if (cr != nullptr) {
			cr->WaitForIdle();
		}
	}
	for (auto& cp: compute_cps) {
		if (cp != nullptr) {
			cp->BufferWait();
		}
	}
}

void Gpu::Init() {
	EXIT_IF(m_gfx_cp != nullptr);
	EXIT_IF(m_gfx_ring != nullptr);

	m_gfx_cp   = new CommandProcessor;
	m_gfx_ring = new GraphicsRing;
	m_gfx_cp->SetQueue(GraphicContext::QUEUE_GFX);
	m_gfx_ring->SetCp(m_gfx_cp);

	EXIT_IF(GraphicContext::QUEUE_COMPUTE_NUM < 8);
}

ComputeRing* Gpu::GetRing(uint32_t ring_id) {
	int v        = static_cast<int>(ring_id - 1);
	int pipe_id  = v / 8;
	int queue_id = v % 8;

	if (m_compute_cp[pipe_id] == nullptr) {
		m_compute_cp[pipe_id] = new CommandProcessor;
		m_compute_cp[pipe_id]->SetQueue(GraphicContext::QUEUE_COMPUTE_START + pipe_id);
	}

	if (m_compute_ring[v] == nullptr) {
		m_compute_ring[v] = new ComputeRing;
		m_compute_ring[v]->SetQueueId(queue_id);
		m_compute_ring[v]->SetCp(m_compute_cp[pipe_id]);
	}

	return m_compute_ring[v];
}

void CommandProcessor::SetQueue(int queue) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	if (queue < 0 || queue >= GraphicContext::QUEUES_NUM ||
	    (m_processors[queue] != nullptr && m_processors[queue] != this)) {
		EXIT("invalid command-processor queue registration: queue=%d owner=%p\n", queue,
		     static_cast<const void*>(m_processors[queue]));
	}
	m_scheduler.SetQueue(queue);
	m_processors[queue] = this;
}

void CommandProcessor::FinishReadbackTransaction() {
	if (GraphicsRunCurrentCommandProcessor() == nullptr || !m_readback_active) {
		EXIT("GPU readback finish requires a command-processor thread\n");
	}
	if (m_readback_finished) {
		return;
	}
	FinishCommandProcessors();
	m_readback_finished = true;
}

void CommandProcessor::FinishCommandProcessors() {
	std::array<CommandProcessor*, GraphicContext::QUEUES_NUM> processors {};
	uint32_t                                                  processor_count = 0;
	for (auto* processor: m_processors) {
		if (processor == nullptr ||
		    std::find(processors.begin(), processors.begin() + processor_count, processor) !=
		        processors.begin() + processor_count) {
			continue;
		}
		processors[processor_count++] = processor;
		// TANI: tek bir komut islemcisi SynchronizeGpu() icinde asili kaliyor
		// ([SYNC-GPU] giris var, cikis yok). Asagidaki iki cagri, o
		// fonksiyonun icindeki YEGANE bekleme adaylari. Hangisinin
		// donmedigini giris/cikis ciftleriyle NOKTA ATISI belirliyoruz:
		//   Submit asili  -> Vulkan kuyruguna gonderirken oluyoruz
		//   Resume asili  -> GPU isi bitiremiyor (shader hang / fence)
		{
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 200000) {
				printf("[RB-SUBMIT] giris (cp=%p)\n", static_cast<void*>(processor));
				fflush(stdout);
			}
		}
		processor->m_scheduler.SubmitForReadback();
		{
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 200000) {
				printf("[RB-SUBMIT] cikis (cp=%p)\n", static_cast<void*>(processor));
				fflush(stdout);
			}
		}
	}
	for (uint32_t i = 0; i < processor_count; i++) {
		{
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 200000) {
				printf("[RB-RESUME] giris (cp=%p)\n", static_cast<void*>(processors[i]));
				fflush(stdout);
			}
		}
		processors[i]->m_scheduler.ResumeAfterReadback();
		{
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 200000) {
				printf("[RB-RESUME] cikis (cp=%p)\n", static_cast<void*>(processors[i]));
				fflush(stdout);
			}
		}
	}
}

void CommandProcessor::Reset() {
	BufferWait();

	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	Sync::DeleteBuffers();

	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_user_data_marker                 = HW::UserSgprType::Unknown;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

void CommandProcessor::BufferInit() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	m_scheduler.Init();
}

void CommandProcessor::BufferFlush() {
	// TANI: kilitte mi, gonderimde mi, fence'te mi asili kaliyoruz?
	// Beklemeye girerken kilidi O AN kimin tuttugunu da yaz - asil soru bu.
	{
		const auto owner = CpLockGet(&m_mutex);
		printf("[CP-KILIT] BufferFlush bekliyor (bu thread=%lu) | sahip thread=%lu site=%s\n",
		       static_cast<unsigned long>(GetCurrentThreadId()), owner.tid,
		       owner.site != nullptr ? owner.site : "(serbest)");
		fflush(stdout);
	}
	PsemuGpuMark("BufferFlush: m_mutex bekleniyor", 0, 0, 0);
	CommandScheduler::SchedTrace("BufferFlush: m_mutex bekleniyor", -1);
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	PsemuGpuMark("BufferFlush: m_mutex alindi", 0, 0, 0);
	CommandScheduler::SchedTrace("BufferFlush: m_mutex alindi", -1);
	m_scheduler.Flush();
	PsemuGpuMark("BufferFlush: cikis", 0, 0, 0);
	CommandScheduler::SchedTrace("BufferFlush: cikis", -1);
}

void CommandProcessor::BufferFlushAndWait() {
	CommandBuffer* submitted = nullptr;

	{
		Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
		submitted = m_scheduler.FlushAndGetSubmitted();
	}

	submitted->WaitForFence();
}

void CommandProcessor::BufferWait() {
	if (g_current_run_cp != this) {
		m_run_mutex.Lock();
		BufferInit();

		std::array<CommandBuffer*, CommandScheduler::BuffersNum> buffers {};
		{
			Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
			m_scheduler.CopyBuffers(&buffers);
		}

		for (auto* buf: buffers) {
			buf->WaitForFenceAndReset();
		}

		m_run_mutex.Unlock();
		return;
	}

	BufferInit();

	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	m_scheduler.WaitAll();
}

void CommandProcessor::ResetDeCe() {
	m_de_counter.mutex.Lock();
	m_de_counter.value = 0;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
	m_ce_counter.mutex.Lock();
	m_ce_counter.value = 0;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitCe() {
	m_de_counter.mutex.Lock();
	auto de_value = m_de_counter.value;
	m_de_counter.mutex.Unlock();

	m_ce_counter.mutex.Lock();
	while (!(m_ce_counter.value > de_value)) {
		m_ce_counter.cond_var.Wait(&m_ce_counter.mutex);
	}
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitDeDiff(uint32_t diff) {
	m_ce_counter.mutex.Lock();
	auto ce_value = m_ce_counter.value;
	m_ce_counter.mutex.Unlock();

	m_de_counter.mutex.Lock();
	while (!(ce_value - m_de_counter.value < diff)) {
		m_de_counter.cond_var.Wait(&m_de_counter.mutex);
	}
	m_de_counter.mutex.Unlock();
}

void CommandProcessor::IncremenetDe() {
	BufferFlush();
	BufferWait();

	m_de_counter.mutex.Lock();
	m_de_counter.value++;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
}

void CommandProcessor::IncremenetCe() {
	m_ce_counter.mutex.Lock();
	m_ce_counter.value++;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	memcpy(m_const_ram + offset / 4, src, static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	memcpy(dst, m_const_ram + offset / 4, static_cast<size_t>(dw_num) * 4);
}

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func) {
	switch (func) {
		case 0: return true;
		case 1: return (value & mask) < ref;
		case 2: return (value & mask) <= ref;
		case 3: return (value & mask) == ref;
		case 4: return (value & mask) != ref;
		case 5: return (value & mask) >= ref;
		case 6: return (value & mask) > ref;
		default: EXIT("unknown wait compare function: %" PRIu32 "\n", func);
	}

	return false;
}

static void YieldCommandProcessorWait(uint32_t poll_interval_cycles) noexcept {
	// PS5 specifies GPU poll cycles, not host microseconds. Yield at the scheduler boundary now;
	// a future resumable guest GPU scheduler can replace this one function without touching polling
	// or BufferCache coherence.
	(void)poll_interval_cycles;
	std::this_thread::yield();
}

// ============================================================================
// KADEMELI GERI CEKILME (exponential backoff) - bos spin'i uykuya cevirir
// ----------------------------------------------------------------------------
// OLCUM: wait_reg_mem el sikismalari CALISIYOR ama her tur SANIYELER suruyor
// (bir etikette 3.400.000 spin; 497 dword'luk tampon 4 dakikada ancak 183.
// ofsete geldi). std::this_thread::yield() isletim sistemine "bekliyorum"
// demez, yalnizca "bu dilimi gec" der ve aninda geri doner - bekleyen
// thread'ler CPU'yu doldurup YAZAN thread'i acikta birakiyor olabilir.
//
// Uc kademe: once saf yield (kisa el sikismalar bedava kalsin), sonra
// mikro-uyku, sonra 1 ms. 1 ms CPU olceginde uzun bir sure; yazan tarafa
// arasina girip etiketi yazmasi icin bol zaman verir.
//
// AYARLANABILIR: PSEMU_CP_SPIN_YIELD (varsayilan 50000) tur sayisindan
// sonra uykuya gecilir. 0 -> eski davranis (hep yield), boylece degisiklik
// tek degiskenle geri alinabilir ve A/B olculebilir.
static void BackoffCommandProcessorWait(uint64_t spin_count, uint32_t poll_interval_cycles) noexcept {
	static const uint64_t kYieldRounds = [] {
		const char* e = std::getenv("PSEMU_CP_SPIN_YIELD");
		if (e == nullptr) {
			return static_cast<uint64_t>(50000);
		}
		const long long v = atoll(e);
		return static_cast<uint64_t>(v < 0 ? 0 : v);
	}();

	if (kYieldRounds == 0 || spin_count < kYieldRounds) {
		YieldCommandProcessorWait(poll_interval_cycles);
		return;
	}
	if (spin_count < kYieldRounds * 2u) {
		std::this_thread::sleep_for(std::chrono::microseconds(100));
		return;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
}


// ============================================================================
// GPU ANLIK GORUNTU (stall snapshot)
// ----------------------------------------------------------------------------
// NEDEN: takilma noktasi kosudan kosuya DEGISIYOR (ayni 497 dw'lik tamponda
// bir kosuda ofset 162/ACQUIRE_MEM, digerinde 427/EVENT_WRITE). Tek noktaya
// iz koyup kovalamak bu yuzden her seferinde baska bir "suclu" gosteriyor -
// bu oturumda yedi kez boyle yanildik.
//
// COZUM: her GPU thread'i kendi son konumunu KILITSIZ bir kayda yazar
// (printf yok, yalnizca statik dizge isaretcisi + uc sayi). Ayri bir
// nobetci thread ilerlemeyi izler; global sayac N saniye artmazsa TUM
// thread'lerin konumunu TEK SEFERDE doker. Boylece degisken hedefi tek tek
// izlerle kovalamak yerine, kilitlendigi ANIN tam fotografini aliriz.
//
// Ayarlar: PSEMU_GPU_STALL_SEC (varsayilan 10), PSEMU_GPU_STALL_DUMPS
// (varsayilan 5 - kac kez doksun).
// ============================================================================
namespace {

struct GpuCrumb {
	unsigned long        tid       = 0;
	const char*          thread_id = "?";   // statik dizge
	std::atomic<const char*> site {"(baslangic)"}; // statik dizge
	std::atomic<uint64_t>    a {0};
	std::atomic<uint64_t>    b {0};
	std::atomic<uint64_t>    c {0};
	std::atomic<uint64_t>    seq {0};
};

std::mutex                              g_crumb_mtx;
std::vector<std::unique_ptr<GpuCrumb>>  g_crumbs;
std::atomic<uint64_t>                   g_crumb_global_seq {0};
thread_local GpuCrumb*                  t_crumb = nullptr;

GpuCrumb* CrumbSelf() {
	if (t_crumb == nullptr) {
		auto  owned = std::make_unique<GpuCrumb>();
		auto* raw   = owned.get();
		raw->tid    = static_cast<unsigned long>(GetCurrentThreadId());
		{
			std::lock_guard<std::mutex> lk(g_crumb_mtx);
			g_crumbs.push_back(std::move(owned));
		}
		t_crumb = raw;
	}
	return t_crumb;
}

} // namespace

// Sicak yol: kilit yok, yalnizca birkac atomik yazma.
void PsemuGpuMark(const char* site, uint64_t a, uint64_t b, uint64_t c) {
	auto* cr = CrumbSelf();
	cr->site.store(site, std::memory_order_relaxed);
	cr->a.store(a, std::memory_order_relaxed);
	cr->b.store(b, std::memory_order_relaxed);
	cr->c.store(c, std::memory_order_relaxed);
	cr->seq.fetch_add(1, std::memory_order_relaxed);
	g_crumb_global_seq.fetch_add(1, std::memory_order_relaxed);
}

// SESSIZ isaret: konumu gunceller ama GLOBAL ILERLEME sayacini ARTIRMAZ.
// Bos donen bekleme dongulerinde sart - aksi halde spin "ilerleme" sayilir
// ve takilma nobetcisi kilitlenmeyi hic goremez (ilk surumde tam bu oldu:
// wait_reg_mem her turda isaret koydugu icin nobetci hic tetiklenmedi).
void PsemuGpuMarkIdle(const char* site, uint64_t a, uint64_t b, uint64_t c) {
	auto* cr = CrumbSelf();
	cr->site.store(site, std::memory_order_relaxed);
	cr->a.store(a, std::memory_order_relaxed);
	cr->b.store(b, std::memory_order_relaxed);
	cr->c.store(c, std::memory_order_relaxed);
	cr->seq.fetch_add(1, std::memory_order_relaxed);
}

// Bu thread'e okunabilir bir ad ver (bir kez).
void PsemuGpuThreadName(const char* name) {
	CrumbSelf()->thread_id = name;
}

namespace {

void GpuSnapshotDump(const char* reason) {
	std::lock_guard<std::mutex> lk(g_crumb_mtx);
	printf("\n=============== GPU ANLIK GORUNTU (%s) ===============\n", reason);
	printf("%-22s %-8s %-34s %12s %12s %12s %10s\n", "thread", "tid", "konum", "a", "b", "c",
	       "adim");
	for (const auto& cr: g_crumbs) {
		printf("%-22s %-8lu %-34s %12llu %12llu %12llu %10llu\n", cr->thread_id, cr->tid,
		       cr->site.load(std::memory_order_relaxed),
		       static_cast<unsigned long long>(cr->a.load(std::memory_order_relaxed)),
		       static_cast<unsigned long long>(cr->b.load(std::memory_order_relaxed)),
		       static_cast<unsigned long long>(cr->c.load(std::memory_order_relaxed)),
		       static_cast<unsigned long long>(cr->seq.load(std::memory_order_relaxed)));
	}
	printf("=====================================================================\n\n");
	fflush(stdout);
}

void GpuStallWatchdog(void* /*unused*/) {
	const int stall_sec = [] {
		const char* e = std::getenv("PSEMU_GPU_STALL_SEC");
		const int   v = (e != nullptr) ? atoi(e) : 10;
		return v > 0 ? v : 10;
	}();
	const int max_dumps = [] {
		const char* e = std::getenv("PSEMU_GPU_STALL_DUMPS");
		const int   v = (e != nullptr) ? atoi(e) : 5;
		return v > 0 ? v : 5;
	}();

	uint64_t last_seq   = 0;
	int      quiet_sec  = 0;
	int      dumps_done = 0;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const uint64_t now = g_crumb_global_seq.load(std::memory_order_relaxed);
		if (now != last_seq) {
			last_seq  = now;
			quiet_sec = 0;
			continue;
		}
		// Hicbir GPU thread'i ilerlemedi.
		if (++quiet_sec >= stall_sec && dumps_done < max_dumps) {
			char reason[96];
			snprintf(reason, sizeof(reason), "%d sn ilerleme yok, dokum #%d", quiet_sec,
			         dumps_done + 1);
			GpuSnapshotDump(reason);
			dumps_done++;
			quiet_sec = 0;
		}
	}
}

} // namespace

void PsemuGpuStallWatchdogStart() {
	static std::atomic_bool started {false};
	if (started.exchange(true)) {
		return;
	}
	Common::Thread t(GpuStallWatchdog, nullptr);
	t.Detach();
}

// Tani: asagida tanimli (gonderilen tamponlarda etiket aramasi).
static void PsemuScanSubmittedForLabel(uint64_t label);

template <typename T>
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t poll,
                                  uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	const auto addr_value = reinterpret_cast<uint64_t>(addr);
	const auto log_width  = static_cast<int>(sizeof(T) * 2u);
	const auto bits       = static_cast<unsigned>(sizeof(T) * 8u);

	// Bu deger BASKA bir thread tarafindan yaziliyor (release-mem'i isleyen
	// komut islemcisi), dolayisiyla yoklama yuklemesi volatile OLMALI. AGC
	// API'sinde adres 'const volatile void*' olarak geliyordu ama bu katmanda
	// volatile dusuyordu.
	//
	// DURUSTLUK NOTU: bu, asagidaki kilitlenmenin SEBEBI DEGILDI - denendi ve
	// olculdu, davranis hic degismedi (1085 ve 497 dw'lik tamponlar yine
	// bitmiyor). Yine de dogru oldugu icin birakildi.
	//
	// Cozulmemis olcum: [EOP-YAZ] etikete 0x1 yazildigini gosteriyor
	// (cache_action=0x38 -> write64 -> memcpy calisiyor) ve bu yazim
	// beklemeler BASLADIKTAN SONRA oluyor; buna ragmen [CP-BEKLIYOR] ayni
	// adres icin spin 600000'e kadar "deger=0x0" raporluyor. Yani yazan ile
	// okuyan ayni adresi kullandigi halde ayni BELLEGI gormuyor gibi.
	const volatile T* vaddr = addr;

	uint64_t spin_count = 0;
	for (;;) {
		const T value = *vaddr;
		if (TestWaitRegMemValue(value, ref, mask, func)) {
			break;
		}
		if ((++spin_count % 100000u) == 0) {
			LOGF("\t wait_reg_mem%u still waiting: addr = 0x%016" PRIx64 ", value = 0x%0*" PRIx64
			     ", ref = 0x%0*" PRIx64 ", mask = 0x%0*" PRIx64 ", func = %" PRIu32 "\n",
			     bits, addr_value, log_width, static_cast<uint64_t>(value), log_width,
			     static_cast<uint64_t>(ref), log_width, static_cast<uint64_t>(mask), func);
			// TANI: yukaridaki LOGF varsayilan gunluk yapilandirmasinda
			// gorunmuyor, bu yuzden komut islemcisinin BURADA sonsuza kadar
			// dondugu olculemedi. Olcum: CP, 497 dword'luk tamponu
			// ayristirirken bitirmiyor ve flip tasiyan tampon (8384 dw)
			// kuyrukta bekliyor. Bu satir hangi adresin, hangi degeri
			// bekledigini SOYLER - yani o degeri kimin yazmasi gerektigini.
			static std::atomic<uint32_t> s_stuck {0};
			const uint32_t               sn = s_stuck.fetch_add(1);
			if (sn < 400) {
				unsigned long long ab = 0;
				const unsigned long long pr = PsemuQueryProtect(addr_value, &ab);
				printf("[CP-BEKLIYOR] wait_reg_mem%u TAKILDI: ptr=%p deger=0x%llx "
				       "ref=0x%llx maske=0x%llx func=%u spin=%llu | state=0x%llx prot=0x%llx "
				       "alloc_base=0x%llx\n",
				       bits, static_cast<const void*>(addr),
				       static_cast<unsigned long long>(value),
				       static_cast<unsigned long long>(ref),
				       static_cast<unsigned long long>(mask), func,
				       static_cast<unsigned long long>(spin_count), pr >> 16, pr & 0xffffull, ab);
				fflush(stdout);
			}
			// Her FARKLI etiket icin bir kez tara (ayni adresi tekrar
			// taramak bilgi vermiyordu; kilitlenme zincirini gormek icin
			// takilan TUM adresler lazim).
			{
				static std::mutex             s_seen_mtx;
				static std::vector<uint64_t>  s_seen;
				bool                          first = false;
				{
					std::lock_guard<std::mutex> lk(s_seen_mtx);
					if (std::find(s_seen.begin(), s_seen.end(), addr_value) == s_seen.end() &&
					    s_seen.size() < 6) {
						s_seen.push_back(addr_value);
						first = true;
					}
				}
				if (first) {
					PsemuScanSubmittedForLabel(addr_value);
				}
			}
		}
		PsemuGpuMarkIdle("CP: wait_reg_mem bekliyor", addr_value, static_cast<uint64_t>(value), spin_count);
		BackoffCommandProcessorWait(spin_count, poll);
	}
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t, uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t, uint32_t);

// ============================================================================
// TANI: gonderilen DCB'leri kaydet, kilitlenince ICLERINDE etiketi ARA
// ----------------------------------------------------------------------------
// PM4 akisini elle yurumek hataya acikti (ilk denemem sifir paket buldu).
// Bunun yerine gonderilen tamponlari saklayip, komut islemcisi bir etikette
// kilitlendiginde o etiketin adresini tamponlarda DUZ TARAMA ile ariyoruz:
// adres dusuk/yuksek dword'leri yan yana gecerse o tampon bu etikete
// dokunuyor demektir. Boylece "yazan paket sonraki tamponlarda mi (sirali
// halkada kilitlenme) yoksa oyunun akisinda HIC yok mu (surucunun yazmasi
// gerekiyor)" sorusu kesin cevaplanir.
// ============================================================================
namespace {
struct SubmittedBuffer {
	const uint32_t* addr = nullptr;
	uint32_t        dw   = 0;
};
constexpr size_t              kMaxRecordedSubmits = 16;
SubmittedBuffer               g_recorded[kMaxRecordedSubmits] {};
std::atomic<uint32_t>         g_recorded_n {0};
} // namespace

void PsemuRecordSubmittedDcb(const uint32_t* addr, uint32_t dw) {
	const uint32_t i = g_recorded_n.fetch_add(1, std::memory_order_relaxed);
	if (i < kMaxRecordedSubmits) {
		g_recorded[i].addr = addr;
		g_recorded[i].dw   = dw;
	}
}

static void PsemuScanSubmittedForLabel(uint64_t label) {
	const auto lo = static_cast<uint32_t>(label & 0xffffffffu);
	const auto hi = static_cast<uint32_t>((label >> 32u) & 0x3ffffu);
	const uint32_t n =
	    std::min<uint32_t>(g_recorded_n.load(std::memory_order_relaxed), kMaxRecordedSubmits);
	printf("[ETIKET-ARA] 0x%016llx icin %u gonderilen tampon taraniyor\n",
	       static_cast<unsigned long long>(label), n);
	for (uint32_t b = 0; b < n; b++) {
		const auto& s = g_recorded[b];
		if (s.addr == nullptr) {
			continue;
		}
		uint32_t hits = 0;
		for (uint32_t i = 0; i + 1 < s.dw; i++) {
			if (s.addr[i] == lo && (s.addr[i + 1] & 0x3ffffu) == hi) {
				// Paket turunu ayirt et: WAIT_MEM_64'te adres cmd[1..2]
				// (yani cmd_id ofset-1'de), RELEASE_MEM'de cmd[3..4]
				// (cmd_id ofset-3'te). "Kim bekliyor, kim yaziyor"
				// sorusunu ancak boyle cevaplayabiliriz.
				const char* kind = "?";
				if (i >= 1) {
					const uint32_t id = s.addr[i - 1];
					if (((id >> 8u) & 0xffu) == Pm4::IT_NOP &&
					    KYTY_PM4_R(id) == Pm4::R_WAIT_MEM_64) {
						kind = "BEKLIYOR";
					}
				}
				if (kind[0] == '?' && i >= 3) {
					const uint32_t id = s.addr[i - 3];
					if ((((id >> 8u) & 0xffu) == Pm4::IT_NOP &&
					     KYTY_PM4_R(id) == Pm4::R_RELEASE_MEM) ||
					    ((id >> 8u) & 0xffu) == Pm4::IT_RELEASE_MEM) {
						kind = "YAZIYOR";
					}
				}
				printf("[ETIKET-ARA]   tampon#%u (dw=%u) ofset %u: %s\n", b + 1, s.dw, i, kind);
				if (++hits >= 4) {
					break;
				}
			}
		}
		if (hits == 0) {
			printf("[ETIKET-ARA]   tampon#%u (dw=%u): yok\n", b + 1, s.dw);
		}
	}
	fflush(stdout);
}

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	const uint32_t dst_sel      = ((write_control >> 30u) & 0x1u) | ((write_control >> 7u) & 0x1eu);
	const uint32_t cache_policy = (write_control >> 25u) & 0x3u;
	const uint32_t increment    = (write_control >> 16u) & 0x1u;
	const uint32_t write_confirm = (write_control >> 20u) & 0x1u;

	if (dst_sel != 0 && dst_sel != 2 && dst_sel != 4 && dst_sel != 5) {
		EXIT("unsupported writeData destination selector 0x%02" PRIx32 "\n", dst_sel);
	}
	EXIT_NOT_IMPLEMENTED(increment != 0);

	if (cache_policy > 3 || write_confirm > 1) {
		LOGF("\t warning: unexpected write_data control 0x%08" PRIx32 "\n", write_control);
	}
	if (dw_num == 0) {
		return;
	}

	// TANI: EOP yazimlarini ([EOP-YAZ]) logladik ama WRITE_DATA AYRI bir
	// yol - beklenen etikete (0x...2540) belki buradan yaziliyor. Olcum
	// bosluguydu, kapatiyoruz.
	{
		static std::atomic<uint32_t> s_wd {0};
		if (s_wd.fetch_add(1) < 400) {
			printf("[WD-YAZ] dst=0x%016llx dw=%u ilk_deger=0x%08x dst_sel=%u\n",
			       static_cast<unsigned long long>(reinterpret_cast<uint64_t>(dst)), dw_num,
			       src[0], dst_sel);
			fflush(stdout);
		}
	}

	memcpy(dst, src, static_cast<size_t>(dw_num) * sizeof(uint32_t));
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	const auto value = Sync::ReadReferenceClock();
	std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64 " size=%u\n",
	     dst_address, value, num_bytes);
}

void CommandProcessor::DmaData(uint8_t engine, uint8_t dst_sel, uint8_t dst_cache_policy,
                               uint64_t dst_address_or_offset, uint8_t src_sel,
                               uint8_t  src_cache_policy,
                               uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes,
                               uint8_t wait_for_previous, uint8_t write_confirm,
                               uint8_t block_engine) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	EXIT_NOT_IMPLEMENTED(engine > 1);
	if (num_bytes == 0) {
		return;
	}
	// TANI: 4'un kati olmayan DMA burada oluyordu. Once NE istendigini
	// olcelim - boyut, adresler, seciciler ve hangi yola (sabit doldurma /
	// bellek kopyasi) gidecegi. Kalan bayt destegini yazmadan once istegin
	// gercekte ne oldugunu bilmek gerekiyor.
	if ((num_bytes & 3u) != 0) {
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 64) {
			const char* yol = (src_sel == 2)                        ? "FillBuffer (sabit deger)"
			                  : (src_sel == 0 || src_sel == 3)      ? "CopyBuffer (bellek->bellek)"
			                                                        : "desteklenmeyen kaynak";
			printf("[DMA-KUSURAT] num_bytes=%u (kalan=%u) dst=0x%016llx src/imm=0x%016llx "
			       "dst_sel=%u src_sel=%u engine=%u dst_cache=%u src_cache=%u wait=%u "
			       "confirm=%u block=%u -> %s\n",
			       num_bytes, num_bytes & 3u,
			       static_cast<unsigned long long>(dst_address_or_offset),
			       static_cast<unsigned long long>(src_address_or_offset_or_immediate), dst_sel,
			       src_sel, engine, dst_cache_policy, src_cache_policy, wait_for_previous,
			       write_confirm, block_engine, yol);
			fflush(stdout);
		}
	}
	// GUVENLIK: adres olarak YORUMLANAMAYACAK kadar kucuk degerler.
	//
	// Olculdu: oyun num_bytes=1, dst=0x2, src=0x4, dst_sel=src_sel=3,
	// engine=1, write_confirm=1 ile bir DMA_DATA gonderiyor. Ham kelimeler
	// elle cozuldu (control=0x60300001, control2=0x80000001) ve cozumleme
	// TUTARLI - yani sel gercekten "adres" diyor ama degerler adres degil.
	// PM4'te 1 baytlik, minik "adresli", write-confirm'li DMA klasik bir
	// SENKRONIZASYON deyimidir; veri kopyasi degildir. Nitekim hemen
	// yukarida Sony'ye ozel baska bir DMA_DATA kodlamasi (PrefetchL2) icin
	// de ozel durum var.
	//
	// Bu istegi bayt kopyasi olarak uygulamak 0x2 adresine YAZMAK olurdu.
	// Onun yerine atliyoruz: kopya yapilmadigi icin bellek bozulmaz, ve
	// EXIT yerine devam edildigi icin boru hatti ilerleyebilir.
	// Kacis kapisi: PSEMU_DMA_STRICT=1 -> eski davranis (EXIT).
	{
		constexpr uint64_t kMinPlausibleAddress = 0x10000;
		const bool dst_is_addr = (dst_sel == 0 || dst_sel == 3);
		const bool src_is_addr = (src_sel == 0 || src_sel == 3);
		const bool bogus = (dst_is_addr && dst_address_or_offset < kMinPlausibleAddress) ||
		                   (src_is_addr && src_sel != 2 &&
		                    src_address_or_offset_or_immediate < kMinPlausibleAddress);
		static const bool strict = [] {
			const char* e = std::getenv("PSEMU_DMA_STRICT");
			return e != nullptr && e[0] == '1';
		}();
		if (bogus && !strict) {
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 16) {
				printf("[DMA-ATLA] adres olamayacak kadar kucuk deger -> istek atlandi "
				       "(num_bytes=%u dst=0x%llx src=0x%llx dst_sel=%u src_sel=%u)\n",
				       num_bytes, static_cast<unsigned long long>(dst_address_or_offset),
				       static_cast<unsigned long long>(src_address_or_offset_or_immediate),
				       dst_sel, src_sel);
				fflush(stdout);
			}
			return;
		}
	}

	EXIT_NOT_IMPLEMENTED((num_bytes & 3u) != 0);
	EXIT_NOT_IMPLEMENTED(dst_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(src_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(wait_for_previous > 1);
	EXIT_NOT_IMPLEMENTED(write_confirm > 1);
	EXIT_NOT_IMPLEMENTED(block_engine > 1);
	const bool dst_memory = dst_sel == 0 || dst_sel == 3;
	const bool src_memory = src_sel == 0 || src_sel == 3;
	if (!dst_memory) {
		// TANI: PM4'te DST_SEL=1 GDS demektir. Emulatorde GDS altyapisi
		// ZATEN var (SPIR-V tarafinda gds_variable/EmitGdsElementPointer,
		// host tarafinda GetGdsBuffer). Baglamadan once ISTEGIN NE OLDUGUNU
		// olcuyoruz: boyut, GDS icindeki hedef ofset ve kaynagin turu.
		//
		// SINANACAK HIPOTEZ: dort duvar once compute shader'da
		// "GDS append/consume requires a zero instruction offset" hatasi
		// almistik ve o komutun ofseti 8'di (raw=[0xd8fa0008 ...]).
		// Buradaki hedef ofset de 8 cikarsa, CP'nin ILKLENDIRDIGI sayac ile
		// shader'in OKUDUGU sayacin AYNI oldugu kanitlanir.
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 24) {
			const char* src_tur = (src_sel == 2)                   ? "SABIT DEGER (immediate)"
			                      : (src_sel == 0 || src_sel == 3) ? "BELLEK adresi"
			                      : (src_sel == 1)                 ? "GDS"
			                                                       : "bilinmeyen";
			printf("[DMA-GDS] dst_sel=%u (GDS) num_bytes=%u gds_hedef_ofset=%llu "
			       "src_sel=%u (%s) src/imm=0x%016llx engine=%u confirm=%u\n",
			       dst_sel, num_bytes,
			       static_cast<unsigned long long>(dst_address_or_offset), src_sel, src_tur,
			       static_cast<unsigned long long>(src_address_or_offset_or_immediate), engine,
			       write_confirm);
			fflush(stdout);
		}
		// GDS HEDEFI BAGLANDI (DST_SEL=1).
		//
		// Olculen istek: num_bytes=4, gds_hedef_ofset=4, src_sel=2 (sabit
		// deger), imm=0 -> GDS'teki 4 baytlik bir sayacin SIFIRLANMASI.
		// Bu, dort duvar once compute shader'da karsilastigimiz GDS
		// append/consume ile ayni mekanizmanin CP tarafi: motor sayaci
		// CP-DMA ile ilklendiriyor, shader append/consume ile guncelliyor.
		// (Shader'daki ofset 8'di, buradaki 4 - ayni GDS sayac alaninda
		//  komsu slotlar.)
		//
		// Yeni bir birim yazmiyoruz: GdsBuffer zaten var ve Clear() dword
		// hizali doldurma yapiyor, sinir kontrolu de kendi icinde.
		if (dst_sel == 1u && src_sel == 2u && (num_bytes & 3u) == 0u &&
		    (dst_address_or_offset & 3u) == 0u) {
			g_render_ctx->GetGdsBuffer()->Clear(
			    g_render_ctx->GetGraphicCtx(), dst_address_or_offset / 4u, num_bytes / 4u,
			    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu));
			return;
		}
		EXIT("unsupported dmaData destination selector 0x%02" PRIx8 "\n", dst_sel);
	}
	if (src_sel == 2) {
		GetGpuResources()->FillBuffer(
		    CurrentBuffer(), dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu));
		return;
	}
	if (src_memory) {
		GetGpuResources()->CopyBuffer(CurrentBuffer(), dst_address_or_offset,
		                              src_address_or_offset_or_immediate, num_bytes);
		return;
	}
	// GDS KAYNAGI BAGLANDI (SRC_SEL=1): GDS -> bellek.
	//
	// Hedef yonun (DST_SEL=1) simetrigi. Motor GDS'te tuttugu sayaci geri
	// belege yaziyor; tipik kullanim, shader'in append/consume ile
	// buyuttugu sayaci CPU'nun veya sonraki bir paketin okuyabilecegi bir
	// adrese tasimak.
	//
	// CPU tarafi okuma bu kod tabaninda zaten yerlesik bir yol:
	// Sync::ReadGds (sync.cpp:412) ayni GdsBuffer::Read cagrisini yapiyor.
	// Yazma tarafinda ise dogrudan memcpy yerine FillBuffer kullaniyoruz ki
	// hedef adres GPU tarafindan izlenen bir tampon ise onbellek/gecerlilik
	// takibi bozulmasin.
	if (src_sel == 1u && (src_address_or_offset_or_immediate & 3u) == 0u) {
		constexpr uint32_t kMaxDw = 64;
		const uint32_t     dw_num = num_bytes / 4u;
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 24) {
			printf("[DMA-GDS-KAYNAK] src_sel=1 (GDS) num_bytes=%u gds_kaynak_ofset=%llu "
			       "dst_sel=%u hedef_adres=0x%016llx\n",
			       num_bytes,
			       static_cast<unsigned long long>(src_address_or_offset_or_immediate),
			       dst_sel, static_cast<unsigned long long>(dst_address_or_offset));
			fflush(stdout);
		}
		// Kucuk sayac geri-okumasi disinda bir sey gelirse SESSIZCE yanlis
		// veri uretmek yerine duruyoruz; boylece gercek sekli olcup
		// gerekirse GPU tarafi vkCmdCopyBuffer'a gecebiliriz.
		EXIT_NOT_IMPLEMENTED(dw_num == 0 || dw_num > kMaxDw);
		uint32_t dw[kMaxDw] = {};
		g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dw,
		                                   static_cast<uint32_t>(
		                                       src_address_or_offset_or_immediate / 4u),
		                                   dw_num);
		for (uint32_t i = 0; i < dw_num; i++) {
			GetGpuResources()->FillBuffer(CurrentBuffer(),
			                              dst_address_or_offset + uint64_t {i} * 4u, 4, dw[i]);
		}
		return;
	}
	EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
}

void GraphicsRing::Submit(OwnedCmdBuffer draw_buffer, OwnedCmdBuffer const_buffer, int handle,
                          int index, int flip_mode, int64_t flip_arg,
                          bool trigger_agc_interrupt_on_done) {
	EXIT_IF(m_cp == nullptr);

	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();

	if (m_done) {
		while (!m_idle) {
			m_idle_cond_var.Wait(&m_mutex);
		}
		m_done = false;

		m_cp->Reset();
	}

	auto& buf                         = m_cmd_batches.emplace_back();
	buf.draw_buffer                   = std::move(draw_buffer);
	buf.const_buffer                  = std::move(const_buffer);
	buf.flip.handle                   = handle;
	buf.flip.index                    = index;
	buf.flip.flip_mode                = flip_mode;
	buf.flip.flip_arg                 = flip_arg;
	buf.trigger_agc_interrupt_on_done = trigger_agc_interrupt_on_done;

	m_idle = false;
	m_cond_var.Signal();
}

void GraphicsRing::SubmitFlipPreparation() {
	EXIT_IF(m_cp == nullptr);
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();
	if (m_done) {
		while (!m_idle) {
			m_idle_cond_var.Wait(&m_mutex);
		}
		m_done = false;
		m_cp->Reset();
	}

	auto& batch            = m_cmd_batches.emplace_back();
	batch.prepare_cpu_flip = true;
	m_idle                 = false;
	m_cond_var.Signal();
}

void GraphicsRing::Done() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	if (m_done) {
		while (!m_idle) {
			m_idle_cond_var.Wait(&m_mutex);
		}
	}
	m_done = true;
}

void GraphicsRing::WaitForIdle() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	while (!m_idle) {
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool GraphicsRing::IsIdle() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	return m_idle;
}

GraphicsRing::CmdBatch GraphicsRing::GetCmdBatch() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	// TANI [CP-FETCH]: dispatcher'in "kuyruktan is cekme" adimi. Sorulan
	// sorular: 497 bittikten sonra buraya GERI DONULUYOR mu, kuyrukta is
	// var mi (derinlik), yoksa bos diye cond-var'da mi uyunuyor?
	// Kuyruk sinirsiz bir std::list; derinlik >0 iken burada uyumak
	// imkansiz olmali - oyleyse takilma bu fonksiyonun DISINDA demektir.
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 200000) {
			printf("[CP-FETCH] cekme denemesi: kuyruk_derinligi=%zu %s\n", m_cmd_batches.size(),
			       m_cmd_batches.empty() ? "-> BOS, cond-var'da uyunacak" : "-> is var");
			fflush(stdout);
		}
	}

	while (m_cmd_batches.empty()) {
		m_idle = true;
		m_idle_cond_var.Signal();

		m_cond_var.Wait(&m_mutex);
	}

	m_idle = false;

	CmdBatch buf = std::move(m_cmd_batches.front());
	m_cmd_batches.pop_front();

	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 200000) {
			printf("[CP-FETCH] is alindi: draw_dw=%u const_dw=%u kalan_kuyruk=%zu\n",
			       buf.draw_buffer.num_dw, buf.const_buffer.num_dw, m_cmd_batches.size());
			fflush(stdout);
		}
	}

	return buf;
}

void GraphicsRing::ThreadBatchRun(void* data) {
	EXIT_IF(data == nullptr);

	static std::atomic_uint64_t seq = 0;

	PsemuGpuThreadName("Grafik-Halkasi");
	auto* ring = static_cast<GraphicsRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	// TANI ASAMALARI: flip tasiyan tampon (dw=8384) kuyruga giriyor ama hic
	// ayristirilmiyor. Kuyruk sinirsiz bir std::list oldugu icin Submit
	// BLOKE OLAMAZ - demek ki bu thread bir asamada takiliyor. Asagidaki
	// isaretler hangisinde durdugunu SOYLER (ilk 24 tur loglanir).
	// NOT: bu sayac 24*6 = 144 ile sinirliydi; oturumda kirpilmis tani
	// ciktisindan bes kez yanlis sonuc cikardigim icin sinir kaldirildi.
	auto stage = [](const char* what, uint64_t n) {
		PsemuGpuMark(what, n, 0, 0);
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 200000) {
			printf("[CP-ASAMA] %s (tur %llu)\n", what, static_cast<unsigned long long>(n));
			fflush(stdout);
		}
	};

	uint64_t round = 0;
	for (;;) {
		stage("kuyruktan is bekleniyor", round);
		CmdBatch buf = ring->GetCmdBatch();
		++round;
		stage("is alindi -> RunLock", round);

		cp->RunLock();
		stage("RunLock alindi", round);
		{
			cp->BufferInit();
			cp->SetSubmitId(++seq);
			if (buf.prepare_cpu_flip) {
				cp->PrepareCpuFlip();
			} else {
				cp->ResetDeCe();
				cp->SetFlip(buf.flip);
				ring->m_draw_job.Execute(
				    [cp, buf] { cp->Run(buf.draw_buffer.data, buf.draw_buffer.num_dw); });
				ring->m_constant_job.Execute(
				    [cp, buf] { cp->Run(buf.const_buffer.data, buf.const_buffer.num_dw); });
				ring->m_draw_job.Wait();
				ring->m_constant_job.Wait();
				stage("draw+const isleri bitti -> BufferFlush", round);
				cp->BufferFlush();
				stage("BufferFlush bitti", round);
				if (buf.trigger_agc_interrupt_on_done) {
					Sync::TriggerEopEvent(0);
				}
			}
		}
		cp->RunUnlock();
	}
}

void Gpu::PauseSubmissions() {
	if (g_gpu_mutex_owned) {
		EXIT("GPU submissions are already paused by this thread\n");
	}
	g_gpu_mutex_owned = true;
	m_mutex.Lock();
	WaitLocked();
	LabelDrain();
}

void Gpu::ResumeSubmissions() {
	if (!g_gpu_mutex_owned) {
		EXIT("GPU submissions resumed without an active pause\n");
	}
	m_mutex.Unlock();
	g_gpu_mutex_owned = false;
}

void ComputeRing::ThreadRun(void* data) {
	EXIT_IF(data == nullptr);

	PsemuGpuThreadName("Hesaplama-Halkasi");
	auto* ring = static_cast<ComputeRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	KYTY_PROFILER_THREAD("Thread_Compute");

	ring->m_mutex.Lock();

	for (;;) {
		while (ring->m_direct_batches.empty()) {
			ring->m_idle = true;
			ring->m_idle_cond_var.Signal();
			ring->m_cond_var.Wait(&ring->m_mutex);
		}

		ring->m_idle = false;

		uint32_t*   buffer                        = nullptr;
		uint32_t    num_dw                        = 0;
		bool        trigger_agc_interrupt_on_done = false;
		DirectBatch direct_batch;

		direct_batch = std::move(ring->m_direct_batches.front());
		ring->m_direct_batches.pop_front();
		buffer                        = direct_batch.buffer.data;
		num_dw                        = direct_batch.buffer.num_dw;
		trigger_agc_interrupt_on_done = direct_batch.trigger_agc_interrupt_on_done;

		static std::atomic<uint32_t> compute_batch_log_count {0};
		if (num_dw <= 128 && buffer != nullptr && compute_batch_log_count.fetch_add(1) < 32) {
			LOGF("compute direct batch: queue=%d, data=0x%016" PRIx64 ", num_dw=%" PRIu32 "\n",
			     ring->m_queue_id, reinterpret_cast<uint64_t>(buffer), num_dw);
			for (uint32_t i = 0; i < std::min<uint32_t>(num_dw, 16); i++) {
				LOGF("\t compute[%02" PRIu32 "] = 0x%08" PRIx32 "\n", i, buffer[i]);
			}
		}

		ring->m_mutex.Unlock();

		cp->RunLock();
		{
			cp->BufferInit();
			cp->ResetDeCe();

			GraphicsDbgDumpDcb("cc", num_dw, buffer);

			cp->Run(buffer, num_dw);

			cp->BufferFlush();
			if (trigger_agc_interrupt_on_done) {
				Sync::TriggerAgcUserInterrupt();
			}
		}
		cp->RunUnlock();

		ring->m_mutex.Lock();
	}
}

void ComputeRing::Submit(OwnedCmdBuffer buffer, bool trigger_agc_interrupt_on_done) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	EXIT_IF(buffer.data == nullptr);
	EXIT_IF(buffer.num_dw == 0);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();

	if (m_done) {
		m_done = false;
	}

	DirectBatch batch {};
	batch.buffer                        = std::move(buffer);
	batch.trigger_agc_interrupt_on_done = trigger_agc_interrupt_on_done;
	m_direct_batches.push_back(std::move(batch));

	m_idle = false;
	m_cond_var.Signal();
}

void ComputeRing::Done() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	m_done = true;
}

void ComputeRing::WaitForIdle() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	while (!m_idle) {
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool ComputeRing::IsIdle() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	return m_idle;
}

void CommandProcessor::Run(uint32_t* data, uint32_t num_dw) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Run");

	struct RunScope {
		explicit RunScope(CommandProcessor* cp): prev(g_current_run_cp) { g_current_run_cp = cp; }
		~RunScope() { g_current_run_cp = prev; }

		CommandProcessor* prev;
	} run_scope(this);

	// psemu tanisi: flip paketi gonderilen aralikta (son 6 dword) ama
	// CpOpFlip hic calismiyor ve R_FLIP hic dagitilmiyor. Bu satir, hangi
	// tamponun GERCEKTEN ayristirildigini soyler: flip'i tasiyan 8384
	// dword'luk tampon listede yoksa, ayristiriciya hic girmiyor demektir.
	{
		static std::atomic<uint32_t> s_run {0};
		if (s_run.fetch_add(1) < 32) {
			printf("[CP-RUN] ayristiriliyor: addr=0x%llx num_dw=%u\n",
			       static_cast<unsigned long long>(reinterpret_cast<uint64_t>(data)), num_dw);
			fflush(stdout);
		}
	}

	auto* cmd = data;
	auto  dw  = num_dw;
	for (;;) {
		if (dw == 0) {
			break;
		}

		EXIT_NOT_IMPLEMENTED(dw > num_dw);

		auto cmd_id = *cmd++;

		if (cmd_id == 0x80000000u) {
			dw--;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(dw < 2);

		auto op = (cmd_id >> 8u) & 0xffu;
		if (GraphicsRunDebugDumpEnabled()) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     num_dw - dw, cmd_id, op, KYTY_PM4_LEN(cmd_id));
		}

		if ((cmd_id & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			auto packet_len = KYTY_PM4_LEN(cmd_id);
			EXIT_NOT_IMPLEMENTED(packet_len == 0 || packet_len > dw);
			static std::atomic<uint32_t> skip_log_count {0};
			if (skip_log_count.fetch_add(1) < 2048) {
				LOGF("\t predicated skip: op=0x%02" PRIx32 ", r=0x%02" PRIx32 ", len=%" PRIu32
				     ", packet=0x%016" PRIx64 ", cmd_id=0x%08" PRIx32 "\n",
				     op, KYTY_PM4_R(cmd_id), packet_len, reinterpret_cast<uint64_t>(cmd - 1),
				     cmd_id);
			}
			if (op == Pm4::IT_NOP && KYTY_PM4_R(cmd_id) == Pm4::R_RELEASE_MEM && packet_len >= 7) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 128) {
					const auto dst = cmd[2] | (static_cast<uint64_t>(cmd[3]) << 32u);
					const auto val = cmd[4] | (static_cast<uint64_t>(cmd[5]) << 32u);
					LOGF("\t predicated skip: R_RELEASE_MEM dst=0x%016" PRIx64
					     ", value=0x%016" PRIx64 ", action=0x%08" PRIx32
					     ", gcr/data/int=0x%08" PRIx32 "\n",
					     dst, val, cmd[0], cmd[1]);
				}
			}
			cmd += packet_len - 1u;
			dw -= packet_len;
			continue;
		}

		auto pfunc = g_cp_op_func[op];

		if (pfunc == nullptr) {
			const auto offset = num_dw - dw;
			LOGF("unknown PM4 packet: data=0x%016" PRIx64 ", num_dw=%" PRIu32
			     ", offset=0x%05" PRIx32 ", current=0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(data), num_dw, offset,
			     reinterpret_cast<uint64_t>(cmd - 1));
			const auto dump_begin = (offset > 8 ? offset - 8 : 0);
			const auto dump_end   = std::min<uint32_t>(num_dw, offset + 16);
			for (uint32_t i = dump_begin; i < dump_end; i++) {
				LOGF("\t%05" PRIx32 "%s %08" PRIx32 "\n", i, (i == offset ? ":" : " "), data[i]);
			}
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n", num_dw - dw, cmd_id);
		}

		// TANI: PSEMU_CP_TRACE_DW=<n> -> yalnizca num_dw'si n olan tamponun
		// paketlerini tek tek yaz. Takilan tamponda EN SON hangi paketin
		// gorundugu, bloke eden paketi dogrudan verir. (Butun tamponlari
		// izlemek log'u bogar; tek tampon secmek olcumu temiz tutar.)
		{
			static const uint32_t kTraceDw = [] {
				const char* e = std::getenv("PSEMU_CP_TRACE_DW");
				return static_cast<uint32_t>((e != nullptr) ? atoi(e) : 0);
			}();
			if (kTraceDw != 0 && num_dw == kTraceDw) {
				printf("[CP-IZ] ofset %5u cmd_id=0x%08x op=0x%02x R=0x%02x len=%u\n", num_dw - dw,
				       cmd_id, op, KYTY_PM4_R(cmd_id), KYTY_PM4_LEN(cmd_id));
				fflush(stdout);
			}
		}

		// Anlik goruntu icin ekmek kirintisi: hangi tamponun hangi
		// ofsetindeki hangi pakette oldugumuz. Kilitsiz, printf'siz.
		PsemuGpuMark("CP: paket isleniyor", num_dw - dw, cmd_id, num_dw);

		auto s = pfunc(this, cmd_id & ~1u, cmd, dw, num_dw);

		PsemuGpuMark("CP: paket bitti", num_dw - dw, cmd_id, num_dw);

		// LOGF("\t %05" PRIx32 ": %u\n", num_dw - dw, s);

		cmd += s;
		dw -= s + 1;
	}

	// TANI: [CP-RUN] yalnizca GIRISI bildiriyordu, o yuzden "497 ayristirildi"
	// ile "497'yi ayristirmaya BASLADI ve icinde takildi" ayirt edilemiyordu.
	// Cikisi da bildirelim: flip tasiyan tampon kuyrukta beklerken CP'nin
	// nerede durdugunu ancak boyle daraltabiliriz.
	{
		static std::atomic<uint32_t> s_end {0};
		if (s_end.fetch_add(1) < 32) {
			printf("[CP-RUN-BITTI] num_dw=%u\n", num_dw);
			fflush(stdout);
		}
	}
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	m_index_type_and_size = index_type_and_size & 0x3u;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	m_draw_indirect_args_base_addr = draw_indirect_args_base_addr;
}

void CommandProcessor::SetDispatchIndirectArgsBaseAddress(
    uint64_t dispatch_indirect_args_base_addr) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	m_dispatch_indirect_args_base_addr = dispatch_indirect_args_base_addr;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	if (num_instances == 0) {
		num_instances = 1;
	}

	m_num_instances = num_instances;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address, uint32_t count_in_dwords) {
	if (wait_op != 0) {
		BufferFlushAndWait();
	}

	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	(void)count_in_dwords;

	switch (op) {
		case 0x00: {
			m_predicate_skip = false;
		} break;
		case 0x03: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);

			auto value = *reinterpret_cast<const volatile uint64_t*>(address);

			switch (condition) {
				case 0x00: m_predicate_skip = (value != 0); break;
				case 0x01: m_predicate_skip = (value == 0); break;
				default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
			}
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 128) {
				LOGF("\t bool predication: addr=0x%016" PRIx64 ", value=0x%016" PRIx64
				     ", condition=%" PRIu32 ", skip=%u, wait_op=%" PRIu32 "\n",
				     reinterpret_cast<uint64_t>(address), value, condition,
				     m_predicate_skip ? 1u : 0u, wait_op);
			}
		} break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
}

void CommandProcessor::DrawIndex(uint32_t index_count, const void* index_addr, uint32_t flags,
                                 uint32_t type, uint32_t instance_count, const void* object_ids,
                                 uint32_t render_target_slice_offset, int32_t vertex_offset_add,
                                 uint32_t first_instance) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	if (instance_count == 0) {
		instance_count = m_num_instances;
	}
	if (object_ids != nullptr) {
		LOGF("\t draw indexed multi-instanced objectIds = 0x%016" PRIx64 "\n",
		     reinterpret_cast<uint64_t>(object_ids));
	}
	if (render_target_slice_offset != 0) {
		LOGF("\t draw render target slice offset = %" PRIu32 "\n", render_target_slice_offset);
	}
	if (vertex_offset_add != 0 || first_instance != 0) {
		LOGF("\t draw indexed offsets: vertex_offset_add = %" PRId32 ", first_instance = %" PRIu32
		     "\n",
		     vertex_offset_add, first_instance);
	}
	const auto frame_num = GraphicsRunGetFrameNum();
	if (frame_num >= 480) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
			const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
			LOGF("QueuePoint DrawIndex: frame=%u submit=%" PRIu64
			     " queue=%d index_count=%u instances=%u prim=%u "
			     "es=0x%016" PRIx64 " ps=0x%016" PRIx64
			     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32 "\n",
			     frame_num, m_submit_id, m_scheduler.Queue(), index_count, instance_count,
			     m_ucfg.GetPrimType(), m_sh_ctx.GetVs().es_regs.data_addr,
			     m_sh_ctx.GetPs().ps_regs.data_addr, m_ucfg.GetGdsOaState().GetIndex(),
			     oa.IsCounterEnabled() ? "true" : "false", oa.GetAddressBytes(),
			     oa.GetSpaceAvailable());
		}
	}

	RenderDrawIndex(m_submit_id, CurrentBuffer(), &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size,
	                index_count, index_addr, flags, type, instance_count,
	                render_target_slice_offset, vertex_offset_add, first_instance);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count,
                                       uint32_t flags) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(index_offset) * index_size);

	RenderDrawIndex(m_submit_id, CurrentBuffer(), &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size,
	                index_count, index_addr, flags, 1, m_num_instances);
}

void CommandProcessor::DrawIndirect(uint32_t data_offset, uint32_t draw_initiator, bool indexed) {
	struct DrawIndirectArgs {
		uint32_t vertex_count_per_instance;
		uint32_t instance_count;
		uint32_t start_vertex_location;
		uint32_t start_instance_location;
	};
	struct DrawIndexedIndirectArgs {
		uint32_t index_count_per_instance;
		uint32_t instance_count;
		uint32_t start_index_location;
		uint32_t base_vertex_location;
		uint32_t start_instance_location;
	};

	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != 2u);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

	const auto* args_addr =
	    reinterpret_cast<const void*>(m_draw_indirect_args_base_addr + data_offset);

	if (!indexed) {
		DrawIndirectArgs args {};
		std::memcpy(&args, args_addr, sizeof(args));
		if (args.instance_count != 1u || args.start_vertex_location != 0u ||
		    args.start_instance_location != 0u) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 64) {
				LOGF("\t warning: partial DrawIndirect args: vertex_count=%" PRIu32
				     ", instance_count=%" PRIu32 ", start_vertex=%" PRIu32
				     ", start_instance=%" PRIu32 "\n",
				     args.vertex_count_per_instance, args.instance_count,
				     args.start_vertex_location, args.start_instance_location);
			}
		}
		DrawIndexAuto(args.vertex_count_per_instance, 0, 0, args.instance_count,
		              args.start_vertex_location, args.start_instance_location);
		return;
	}

	DrawIndexedIndirectArgs args {};
	std::memcpy(&args, args_addr, sizeof(args));
	// TANI: DOLAYLI CIZIM ARGUMANLARI. Oyunun gercek sahne geometrisi GPU'ya
	// hic ulasmiyor - olculdu: 40 cizimin 40'i da 3 vertex, yani hepsi tam
	// ekran post-process gecisi. PS5 oyunlari sahneyi GPU-driven ciziyor:
	// compute eleme gecisi gorunur nesne sayisini bir tampona yazar,
	// DrawIndexIndirect o tampondan okur. Tampon sifirsa sifir nesne cizilir.
	//
	// Bu log iki senaryoyu ayirir:
	//   index_count=0        -> tampon BOS (eleme gecisi calismamis/atlanmis)
	//   index_count>0        -> argumanlar dolu, sorun asagida
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 32) {
			std::printf("[DOLAYLI-CIZIM] index_count=%u instance=%u start_index=%u "
			            "base_vertex=%u start_instance=%u | args_adres=%p\n",
			            args.index_count_per_instance, args.instance_count,
			            args.start_index_location, args.base_vertex_location,
			            args.start_instance_location, static_cast<const void*>(args_addr));
			std::fflush(stdout);
		}
	}
	if (args.base_vertex_location != 0u || args.start_instance_location != 0u) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 64) {
			LOGF("\t warning: partial DrawIndexIndirect args: index_count=%" PRIu32
			     ", instance_count=%" PRIu32 ", start_index=%" PRIu32 ", base_vertex=%" PRIu32
			     ", start_instance=%" PRIu32 "\n",
			     args.index_count_per_instance, args.instance_count, args.start_index_location,
			     args.base_vertex_location, args.start_instance_location);
		}
	}

	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(args.start_index_location) * index_size);

	const uint32_t index_count =
	    (m_index_buffer_size != 0 ? std::min(args.index_count_per_instance, m_index_buffer_size)
	                              : args.index_count_per_instance);
	if (GraphicsRunDebugDumpEnabled() && index_count != args.index_count_per_instance) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
			LOGF("\t DrawIndexIndirect: clamped index_count from %" PRIu32 " to %" PRIu32
			     " using INDEX_BUFFER_SIZE\n",
			     args.index_count_per_instance, index_count);
		}
	}

	DrawIndex(index_count, index_addr, 0, 1, args.instance_count, nullptr, 0,
	          static_cast<int32_t>(args.base_vertex_location), args.start_instance_location);
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, uint32_t draw_initiator,
                                         bool indexed) {
	struct DrawIndirectArgs {
		uint32_t vertex_count_per_instance;
		uint32_t instance_count;
		uint32_t start_vertex_location;
		uint32_t start_instance_location;
	};
	struct DrawIndexedIndirectArgs {
		uint32_t index_count_per_instance;
		uint32_t instance_count;
		uint32_t start_index_location;
		uint32_t base_vertex_location;
		uint32_t start_instance_location;
	};

	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != 2u);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

	uint32_t draw_count = max_count_or_count;
	if (count_addr != nullptr) {
		draw_count = *count_addr;
		if (draw_count > max_count_or_count) {
			draw_count = max_count_or_count;
		}
	}

	if (draw_count == 0) {
		return;
	}

	const auto args_size = indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs);
	EXIT_NOT_IMPLEMENTED(stride_in_bytes < args_size);

	for (uint32_t i = 0; i < draw_count; i++) {
		const auto args_addr = m_draw_indirect_args_base_addr + data_offset +
		                       static_cast<uint64_t>(i) * stride_in_bytes;

		if (!indexed) {
			auto* args = reinterpret_cast<const DrawIndirectArgs*>(args_addr);
			if (args->instance_count != 1u || args->start_vertex_location != 0u ||
			    args->start_instance_location != 0u) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 64) {
					LOGF("\t warning: partial DrawIndirectMulti args[%u]: vertex_count=%" PRIu32
					     ", instance_count=%" PRIu32 ", start_vertex=%" PRIu32
					     ", start_instance=%" PRIu32 "\n",
					     i, args->vertex_count_per_instance, args->instance_count,
					     args->start_vertex_location, args->start_instance_location);
				}
			}
			DrawIndexAuto(args->vertex_count_per_instance, 0, 0, args->instance_count,
			              args->start_vertex_location, args->start_instance_location);
			continue;
		}

		auto* args = reinterpret_cast<const DrawIndexedIndirectArgs*>(args_addr);
		if (args->base_vertex_location != 0u || args->start_instance_location != 0u) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1) < 64) {
				LOGF("\t warning: partial DrawIndexIndirectMulti args[%u]: index_count=%" PRIu32
				     ", instance_count=%" PRIu32 ", start_index=%" PRIu32 ", base_vertex=%" PRIu32
				     ", start_instance=%" PRIu32 "\n",
				     i, args->index_count_per_instance, args->instance_count,
				     args->start_index_location, args->base_vertex_location,
				     args->start_instance_location);
			}
		}

		uint64_t index_size = 0;
		switch (m_index_type_and_size) {
			case 0: index_size = 2; break;
			case 1: index_size = 4; break;
			case 2: index_size = 1; break;
			default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
		}

		auto* index_addr = reinterpret_cast<const void*>(
		    m_index_base_addr + static_cast<uint64_t>(args->start_index_location) * index_size);

		const uint32_t index_count =
		    (m_index_buffer_size != 0
		         ? std::min(args->index_count_per_instance, m_index_buffer_size)
		         : args->index_count_per_instance);
		if (GraphicsRunDebugDumpEnabled() && index_count != args->index_count_per_instance) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
				LOGF("\t DrawIndexIndirectMulti: clamped index_count from %" PRIu32 " to %" PRIu32
				     " using INDEX_BUFFER_SIZE\n",
				     args->index_count_per_instance, index_count);
			}
		}

		DrawIndex(index_count, index_addr, 0, 1, args->instance_count, nullptr, 0,
		          static_cast<int32_t>(args->base_vertex_location), args->start_instance_location);
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	uint32_t frame_num = 0;
	uint32_t local_x   = 1;
	uint32_t local_y   = 1;
	uint32_t local_z   = 1;

	{
		Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

		CheckBuffer();
		frame_num = GraphicsRunGetFrameNum();
		if (GraphicsRunDebugDumpEnabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
				const auto& cs = m_sh_ctx.GetCs().cs_regs;
				const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
				LOGF("QueuePoint DispatchDirect: frame=%u submit=%" PRIu64
				     " queue=%d groups=%ux%ux%u local=%ux%ux%u mode=0x%08" PRIx32
				     " wave=%u cs=0x%016" PRIx64 " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32
				     " oa_space=0x%08" PRIx32 "\n",
				     frame_num, m_submit_id, m_scheduler.Queue(), thread_group_x, thread_group_y,
				     thread_group_z, std::max(cs.num_thread_x, 1u), std::max(cs.num_thread_y, 1u),
				     std::max(cs.num_thread_z, 1u), mode, static_cast<uint32_t>(cs.wave_size),
				     cs.data_addr, m_ucfg.GetGdsOaState().GetIndex(),
				     oa.IsCounterEnabled() ? "true" : "false", oa.GetAddressBytes(),
				     oa.GetSpaceAvailable());
			}
		}

		const auto& cs = m_sh_ctx.GetCs().cs_regs;
		local_x        = std::max(cs.num_thread_x, 1u);
		local_y        = std::max(cs.num_thread_y, 1u);
		local_z        = std::max(cs.num_thread_z, 1u);
		if (cs.wave_size == 64u) {
			static std::atomic_bool logged_wave64_shader {false};
			if (!logged_wave64_shader.exchange(true, std::memory_order_relaxed)) {
				LOGF("warning: executing wave64 compute shader cs=0x%016" PRIx64 "\n",
				     cs.data_addr);
				std::printf("warning: executing wave64 compute shader cs=0x%016" PRIx64 "\n",
				            cs.data_addr);
				std::fflush(stdout);
			}
		}

		RenderDispatchDirect(m_submit_id, CurrentBuffer(), &m_ctx, &m_sh_ctx, thread_group_x,
		                     thread_group_y, thread_group_z, mode);
	}

	constexpr uint32_t DispatchInitiatorUseThreadDimensions = 1u << 5u;
	auto               group_count = [](uint32_t threads, uint32_t group_size) {
		return (threads == 0
		            ? 0u
		            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
	};

	auto groups_x = thread_group_x;
	auto groups_y = thread_group_y;
	auto groups_z = thread_group_z;
	if ((mode & DispatchInitiatorUseThreadDimensions) != 0) {
		groups_x = group_count(thread_group_x, local_x);
		groups_y = group_count(thread_group_y, local_y);
		groups_z = group_count(thread_group_z, local_z);
	}

	const uint64_t invocations =
	    static_cast<uint64_t>(groups_x) * groups_y * groups_z * local_x * local_y * local_z;
	if (invocations != 0) {
		// TANI: komut islemcisi IT_DISPATCH_DIRECT paketinde (ofset 154)
		// sert takiliyor, ama SYNC-GPU / RB-SUBMIT / RB-RESUME sayaclari
		// DENGELI - yani readback yolunda degil. Geriye bu kaliyor:
		// compute dispatch'ten sonraki fence beklemesi. Giris/cikis
		// yazilirsa "GPU isi bitiremiyor" kesinlesir.
		{
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 200000) {
				printf("[DISP-BEKLE] giris (cp=%p gruplar=%ux%ux%u yerel=%ux%ux%u cagri=%llu)\n",
				       static_cast<void*>(this), groups_x, groups_y, groups_z, local_x, local_y,
				       local_z, static_cast<unsigned long long>(invocations));
				fflush(stdout);
			}
		}
		BufferFlushAndWait();
		{
			static std::atomic<uint32_t> s_n {0};
			if (s_n.fetch_add(1) < 200000) {
				printf("[DISP-BEKLE] cikis (cp=%p)\n", static_cast<void*>(this));
				fflush(stdout);
			}
		}
	}
}

void CommandProcessor::DispatchIndirect(uint32_t data_offset, uint32_t mode) {
	struct DispatchIndirectArgs {
		uint32_t thread_group_x;
		uint32_t thread_group_y;
		uint32_t thread_group_z;
	};

	EXIT_NOT_IMPLEMENTED(m_dispatch_indirect_args_base_addr == 0);

	const auto args_addr = m_dispatch_indirect_args_base_addr + data_offset;
	auto*      args      = reinterpret_cast<const DispatchIndirectArgs*>(args_addr);

	DispatchDirect(args->thread_group_x, args->thread_group_y, args->thread_group_z, mode);
}

void CommandProcessor::DrawIndexAuto(uint32_t index_count, uint32_t flags,
                                     uint32_t render_target_slice_offset, uint32_t instance_count,
                                     uint32_t first_vertex, uint32_t first_instance) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();
	const auto frame_num = GraphicsRunGetFrameNum();
	if (frame_num >= 480) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
			const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
			LOGF("QueuePoint DrawIndexAuto: frame=%u submit=%" PRIu64
			     " queue=%d index_count=%u instances=%u prim=%u "
			     "first_vertex=%u first_instance=%u es=0x%016" PRIx64 " ps=0x%016" PRIx64
			     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32 "\n",
			     frame_num, m_submit_id, m_scheduler.Queue(), index_count, instance_count,
			     m_ucfg.GetPrimType(), first_vertex, first_instance,
			     m_sh_ctx.GetVs().es_regs.data_addr, m_sh_ctx.GetPs().ps_regs.data_addr,
			     m_ucfg.GetGdsOaState().GetIndex(), oa.IsCounterEnabled() ? "true" : "false",
			     oa.GetAddressBytes(), oa.GetSpaceAvailable());
		}
	}

	RenderDrawIndexAuto(m_submit_id, CurrentBuffer(), &m_ctx, &m_ucfg, &m_sh_ctx, index_count,
	                    flags, render_target_slice_offset, instance_count, first_vertex,
	                    first_instance);
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush();

	// TANI: DCB(497) ayristirma sirasinda bitmiyor ama takilan wait_reg_mem
	// etiketlerinden HICBIRINI beklemiyor - baska bir bloke edici paket var.
	// Bu, oyunun sceAgcDcbWaitUntilSafeForRendering cagrisindan gelen
	// R_WAIT_FLIP_DONE. Giris/cikis yazilirsa "burada mi asili kaldi"
	// sorusu kesinlesir.
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 8) {
			printf("[FLIP-BEKLE] giris: handle=%u tampon_index=%u\n", video_out_handle,
			       display_buffer_index);
			fflush(stdout);
		}
	}

	VideoOut::VideoOutWaitFlipDone(static_cast<int>(video_out_handle),
	                               static_cast<int>(display_buffer_index));

	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 8) {
			printf("[FLIP-BEKLE] cikis: handle=%u tampon_index=%u\n", video_out_handle,
			       display_buffer_index);
			fflush(stdout);
		}
	}
}

template <typename T>
void CommandProcessor::WriteAtEndOfPipe(uint32_t cache_policy, uint32_t event_write_dest,
                                        uint32_t eop_event_type, uint32_t cache_action,
                                        uint32_t event_index, uint32_t event_write_source,
                                        void* dst_gpu_addr, T value, uint32_t interrupt_selector,
                                        uint32_t interrupt_context_id) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		const auto bits      = static_cast<unsigned>(sizeof(T) * 8u);
		const auto log_width = static_cast<int>(sizeof(T) * 2u);

		LOGF("CommandProcessor::WriteAtEndOfPipe%u()\n"
		     "\t cache_policy        = 0x%08" PRIx32 "\n"
		     "\t event_write_dest    = 0x%08" PRIx32 "\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t event_index         = 0x%08" PRIx32 "\n"
		     "\t event_write_source  = 0x%08" PRIx32 "\n"
		     "\t interrupt_selector  = 0x%08" PRIx32 "\n"
		     "\t interrupt_context   = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%0*" PRIx64 "\n",
		     bits, cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
		     event_write_source, interrupt_selector, interrupt_context_id,
		     reinterpret_cast<uint64_t>(dst_gpu_addr), log_width, static_cast<uint64_t>(value));
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);

	// TANI: komut islemcisi wait_reg_mem64 ile 0x...2540 ve 0x...25c0
	// adreslerinin 1 olmasini bekleyip sonsuza kadar donuyor. Etiketi
	// yazmasi gereken yol BURASI. Hangi adrese, hangi degerin, hangi
	// interrupt_selector ile yazildigini (ya da YAZILMADIGINI) gorelim.
	{
		// NOT: sinir once 40'ti ve dolabiliyordu; "su etikete hic yazilmadi"
		// sonucu o yuzden ARTEFAKT olabilirdi. Buyutuldu.
		static std::atomic<uint32_t> s_eop {0};
		if (s_eop.fetch_add(1) < 400) {
			printf("[EOP-YAZ] dst=0x%016llx deger=0x%llx boyut=%u int_sel=%u ctx=%u "
			       "event_index=0x%02x src=%u eop_event=0x%02x cache_action=0x%02x\n",
			       static_cast<unsigned long long>(reinterpret_cast<uint64_t>(dst_gpu_addr)),
			       static_cast<unsigned long long>(value),
			       static_cast<unsigned>(sizeof(T) * 8u), interrupt_selector, interrupt_context_id,
			       event_index, event_write_source, eop_event_type, cache_action);
			fflush(stdout);
		}
	}

	bool with_interrupt = false;
	switch (interrupt_selector) {
		case 0x00:
		case 0x03: with_interrupt = false; break;
		case 0x01:
			// DIKKAT: bu yol etiketi YAZMADAN donuyor. Dogru mu, olcum
			// gosterecek: beklenen adreslerden biri buraya dusuyorsa
			// kilitlenmenin sebebi budur.
			{
				static std::atomic<uint32_t> s_skip {0};
				if (s_skip.fetch_add(1) < 20) {
					printf("[EOP-ATLA] int_sel=1 -> dst=0x%016llx ETIKET YAZILMADAN donuluyor\n",
					       static_cast<unsigned long long>(reinterpret_cast<uint64_t>(dst_gpu_addr)));
					fflush(stdout);
				}
			}
			Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), interrupt_context_id);
			return;
		case 0x02: with_interrupt = true; break;
		default: EXIT("unknown interrupt_selector\n");
	}

	auto write32 = [&](bool with_writeback) {
		auto* dst  = static_cast<uint32_t*>(dst_gpu_addr);
		auto  data = static_cast<uint32_t>(value);
		std::memcpy(dst, &data, sizeof(data));

		if (with_interrupt) {
			if (with_writeback) {
				Sync::WriteAtEndOfPipeWithInterruptWriteBack32(m_submit_id, CurrentBuffer(), dst,
				                                               data, interrupt_context_id);
			} else {
				Sync::WriteAtEndOfPipeWithInterrupt32(m_submit_id, CurrentBuffer(), dst, data,
				                                      interrupt_context_id);
			}
		} else if (with_writeback) {
			Sync::WriteAtEndOfPipeWithWriteBack32(m_submit_id, CurrentBuffer(), dst, data);
		} else {
			Sync::WriteAtEndOfPipe32(m_submit_id, CurrentBuffer(), dst, data);
		}
	};

	switch (event_write_source) {
		case 0x01:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && cache_action == 0x00 && event_index == 0x06) {
					auto* dst = static_cast<uint32_t*>(dst_gpu_addr);
					SynchronizeGpu();
					Sync::ReadGds(dst, value & 0xffffu, value >> 16u);
					Sync::WriteAtEndOfPipeGds32(m_submit_id, CurrentBuffer(), dst, value & 0xffffu,
					                            value >> 16u);
					return;
				}
			} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05) {
				write32(false);
				return;
			}
			break;
		case 0x02:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && event_index == 0x06) {
					switch (cache_action) {
						case 0x00: write32(false); return;
						case 0x38: write32(true); return;
						default: break;
					}
				}
			} else {
				auto write64 = [&](bool with_writeback) {
					auto* dst = static_cast<uint64_t*>(dst_gpu_addr);
					std::memcpy(dst, &value, sizeof(value));

					// TANI: memcpy'den HEMEN SONRA geri oku. Bekleyen taraf
					// ayni adreste sonsuza kadar 0 goruyor; bu satir
					// "yazma bellege dustu mu, yoksa golge/staging bir
					// tampona mi gitti" sorusunu ayirir. Sayfa korumasi da
					// yazilir: readonly bir sayfaya memcpy sessizce
					// gecemez - geciyorsa hedef asil misafir bellek degildir.
					{
						static std::atomic<uint32_t> s_n {0};
						if (s_n.fetch_add(1) < 64) {
							const uint64_t back = *static_cast<const volatile uint64_t*>(dst);
							unsigned long long ab = 0;
							const unsigned long long pr =
							    PsemuQueryProtect(reinterpret_cast<unsigned long long>(dst), &ab);
							printf("[YAZ-DOGRULA] ptr=%p yazilan=0x%llx geri_okunan=0x%llx "
							       "state=0x%llx prot=0x%llx alloc_base=0x%llx\n",
							       static_cast<void*>(dst),
							       static_cast<unsigned long long>(value),
							       static_cast<unsigned long long>(back), pr >> 16, pr & 0xffffull,
							       ab);
							fflush(stdout);
						}
					}

					if (with_interrupt) {
						if (with_writeback) {
							Sync::WriteAtEndOfPipeWithInterruptWriteBack64(
							    m_submit_id, CurrentBuffer(), dst, value, interrupt_context_id);
						} else {
							Sync::WriteAtEndOfPipeWithInterrupt64(m_submit_id, CurrentBuffer(), dst,
							                                      value, interrupt_context_id);
						}
					} else if (with_writeback) {
						Sync::WriteAtEndOfPipeWithWriteBack64(m_submit_id, CurrentBuffer(), dst,
						                                      value);
					} else {
						Sync::WriteAtEndOfPipe64(m_submit_id, CurrentBuffer(), dst, value);
					}
				};

				switch (cache_action) {
					case 0x00:
						switch (eop_event_type) {
							case 0x04:
							case 0x28:
								if ((eop_event_type == 0x04 && event_index == 0x05) ||
								    (eop_event_type == 0x28 && event_index == 0x00)) {
									write64(false);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
							case 0x2f:
							case 0x30:
								if (event_index == 0x00 && !with_interrupt) {
									write64(false);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x38:
						switch (eop_event_type) {
							case 0x04:
							case 0x14:
							case 0x28:
								if (((eop_event_type == 0x04 || eop_event_type == 0x28) &&
								     event_index == 0x05 && !with_interrupt) ||
								    (event_index == 0x00)) {
									write64(true);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
								if (event_index == 0x00 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							case 0x2f:
								if (event_index == 0x06 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x3b:
						if (eop_event_type == 0x04 && event_index == 0x05 && with_interrupt) {
							write64(true);
							return;
						}
						break;
					default: break;
				}
			}
			break;
		case 0x04:
			if constexpr (sizeof(T) == sizeof(uint64_t)) {
				const auto clock = Sync::ReadReferenceClock();
				std::memcpy(dst_gpu_addr, &clock, sizeof(clock));
				switch (cache_action) {
					case 0x00:
						if (((eop_event_type == 0x04 && event_index == 0x05) ||
						     (eop_event_type == 0x28 && event_index == 0x00)) &&
						    !with_interrupt) {
							Sync::WriteAtEndOfPipeClockCounter(m_submit_id, CurrentBuffer(),
							                                   static_cast<uint64_t*>(dst_gpu_addr),
							                                   clock);
							return;
						}
						break;
					case 0x38:
						if (((eop_event_type == 0x04 &&
						      (event_index == 0x00 || event_index == 0x05)) ||
						     (eop_event_type == 0x28 && event_index == 0x00)) &&
						    !with_interrupt) {
							Sync::WriteAtEndOfPipeClockCounterWithWriteBack(
							    m_submit_id, CurrentBuffer(), static_cast<uint64_t*>(dst_gpu_addr),
							    clock);
							return;
						}
						break;
					default: break;
				}
			}
			break;
		default: break;
	}

	EXIT("unknown event type\n");
}

void CommandProcessor::WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint32_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint64_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::MemoryBarrier() {
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 40) {
			printf("[MEM-BARIYER] giris (cp=%p)\n", static_cast<void*>(this));
			fflush(stdout);
		}
	}
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	GraphicsRenderMemoryBarrier(CurrentBuffer());
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 40) {
			printf("[MEM-BARIYER] cikis (cp=%p)\n", static_cast<void*>(this));
			fflush(stdout);
		}
	}
}

void CommandProcessor::TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), interrupt_context_id);
}

void CommandProcessor::RenderTextureBarrier(uint64_t vaddr, uint64_t size) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	GraphicsRenderTextureBarrier(CurrentBuffer(), vaddr, size);
}

void CommandProcessor::DepthStencilBarrier(uint64_t vaddr, uint64_t size) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	GraphicsRenderDepthStencilBarrier(CurrentBuffer(), vaddr, size);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index) {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n",
		     event_type, event_index);
	}

	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: MemoryBarrier(); break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			MemoryBarrier();
			SynchronizeGpu();
			break;
		// DbDataWritebackInvalidate, DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			MemoryBarrier();
			break;
		case 0x0000000d:
		case 0x0000000e:
		case 0x00000012:
		case 0x00000017:
		case 0x00000018:
		case 0x00000019:
		case 0x0000001a:
		case 0x0000001b:
		case 0x00000038:
		case 0x00000039:
		case 0x0000003a:
			LOGF("\t temporary: ignoring unsupported event_write type 0x%08" PRIx32
			     ", index 0x%08" PRIx32 "\n",
			     event_type, event_index);
			break;
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::Flip() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	auto* command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeOnlyFlip(m_submit_id, command, m_flip.handle, m_flip.index,
	                               m_flip.flip_mode, m_flip.flip_arg, request);
	m_scheduler.Flush();
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n"
		     "\t dst_gpu_addr = 0x%016" PRIx64 "\n"
		     "\t value        = 0x%08" PRIx32 "\n",
		     reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto* command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithFlip32(m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr),
	                                 value, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                 m_flip.flip_arg, request);
	m_scheduler.Flush();
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action,
                                         void* dst_gpu_addr, uint32_t value) {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);

	CheckBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::FlipWithInterrupt()\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%08" PRIx32 "\n",
		     eop_event_type, cache_action, reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	if (eop_event_type != 0x00000004 || cache_action != 0x00000038) {
		EXIT("unknown event type\n");
	}
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto* command = CurrentBuffer();
	auto  request = Sync::PrepareDisplayBufferFlip(command, m_flip.handle, m_flip.index,
	                                               m_flip.flip_mode, m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithInterruptWriteBackFlip32(
	    m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle,
	    m_flip.index, m_flip.flip_mode, m_flip.flip_arg, request);
	m_scheduler.Flush();
}

void CommandProcessor::PrepareCpuFlip() {
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	CheckBuffer();
	if (g_current_run_cp != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	struct RunScope {
		explicit RunScope(CommandProcessor* cp) { g_current_run_cp = cp; }
		~RunScope() { g_current_run_cp = nullptr; }
	};
	RunScope run_scope(this);

	auto prepared_id = Presentation::DisplayBufferPrepareNextFlipOnGpu(CurrentBuffer());
	m_scheduler.Flush();
	Presentation::DisplayBufferCompleteFlipFromGpu(prepared_id);
}

void CommandProcessor::SynchronizeGpu() {
	// TANI: iki komut islemcisi de ACQUIRE_MEM paketinde duruyor ve hicbiri
	// wait_reg_mem'de degil. Bu fonksiyon TUM islemcileri bekliyor
	// (FinishCommandProcessors); ikisi de birbirini beklerse karsilikli
	// kilit olusur. Giris/cikis yazilirsa bu KANITLANIR ya da CURUTULUR.
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 40) {
			printf("[SYNC-GPU] giris  (cp=%p)\n", static_cast<void*>(this));
			fflush(stdout);
		}
	}
	Common::LockGuard lock(m_mutex);
	CpLockScope _cp_lock_scope(&m_mutex, __func__);
	FinishCommandProcessors();
	{
		static std::atomic<uint32_t> s_n {0};
		if (s_n.fetch_add(1) < 40) {
			printf("[SYNC-GPU] cikis  (cp=%p)\n", static_cast<void*>(this));
			fflush(stdout);
		}
	}
}

// psemu: adapter/metrics.cpp — canli performans metrikleri (pencere basligi).
extern "C" void PsemuMetricAddSubmit(uint64_t ticks);

void GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
                       uint32_t num_const_dw, bool trigger_agc_interrupt_on_done) {
	EXIT_IF(cmd_draw_buffer == nullptr);
	EXIT_IF(num_draw_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	// psemu perf olcumu: PM4 gonderiminde (komut isleme + Vulkan kayit) gecen
	// sureyi topla. Present suresiyle karsilastirip darbogazi bulacagiz.
	{
		// PM4 gonderim suresini canli metriklere bildir.
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		g_gpu->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw,
		              trigger_agc_interrupt_on_done);
		QueryPerformanceCounter(&t1);
		PsemuMetricAddSubmit(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
	}
	return;
}

void GraphicsRunSubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                              bool trigger_agc_interrupt_on_done) {
	EXIT_IF(cmd_buffer == nullptr);
	EXIT_IF(num_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->SubmitCompute(queue, cmd_buffer, num_dw, trigger_agc_interrupt_on_done);
}

void GraphicsRunSubmitFlipPreparation() {
	EXIT_IF(g_gpu == nullptr);
	g_gpu->SubmitFlipPreparation();
}

void GraphicsRunWait() {
	GraphicsRunSubmissionLock lock;
}

GraphicsRunSubmissionLock::GraphicsRunSubmissionLock() {
	if (g_gpu == nullptr || g_current_run_cp != nullptr || g_submission_pause_depth == UINT32_MAX) {
		EXIT("cannot acquire GPU submission lock in the current state\n");
	}
	if (g_submission_pause_depth++ == 0) {
		g_gpu->PauseSubmissions();
	}
}

GraphicsRunSubmissionLock::~GraphicsRunSubmissionLock() {
	if (g_gpu == nullptr || g_submission_pause_depth == 0) {
		EXIT("GPU submission lock released without ownership\n");
	}
	if (--g_submission_pause_depth == 0) {
		g_gpu->ResumeSubmissions();
	}
}

void GraphicsRunDone() {
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Done();
}

int GraphicsRunGetFrameNum() {
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->GetFrameNum();
}

bool GraphicsRunIsCommandProcessorThread() noexcept {
	return g_current_run_cp != nullptr;
}

CommandProcessor* GraphicsRunCurrentCommandProcessor() noexcept {
	return g_current_run_cp;
}

void GraphicsRunFinishCommandProcessors() {
	if (g_current_run_cp == nullptr) {
		EXIT("GPU readback finish requires a command-processor thread\n");
	}
	g_current_run_cp->FinishReadbackTransaction();
}

bool GraphicsRunSubmissionLockHeld() noexcept {
	return g_submission_pause_depth != 0;
}

bool GraphicsRunGpuLockHeld() noexcept {
	return g_gpu_mutex_owned;
}

} // namespace Libs::Graphics
