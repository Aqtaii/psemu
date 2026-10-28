#include "guest_avplayer.h"
#include "libs/avPlayerTypes.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace Libs::Audio::AvPlayer;
namespace {
struct Player {
    AvPlayerInternal *native = nullptr;
    AvPlayerMemAllocator guest_mem{};
    GuestAvWrite write = nullptr;
    std::recursive_mutex mutex;
    ~Player() {
        if (native)
            AvPlayerClose(native);
    }
};
std::mutex registry_mutex;
std::unordered_map<uint64_t, std::shared_ptr<Player>> players;
uint64_t next_handle = 0x4156500000000001ull;
std::atomic_uint call_logs{0}, frame_logs{0}, audio_logs{0};

bool CopyString(uint64_t addr, std::string &out, GuestAvRead read) {
    if (!addr)
        return false;
    for (size_t i = 0; i < 4096; ++i) {
        if (addr > UINT64_MAX - i || !read(reinterpret_cast<const void *>(addr + i), 1))
            return false;
        const char c = *reinterpret_cast<const char *>(addr + i);
        if (!c)
            return true;
        out.push_back(c);
    }
    return false;
}
template <class T> bool Read(uint64_t addr, T &out, GuestAvRead read) {
    if (!addr || !read(reinterpret_cast<const void *>(addr), sizeof(T)))
        return false;
    std::memcpy(&out, reinterpret_cast<const void *>(addr), sizeof(T));
    return true;
}
template <class T> bool Output(uint64_t addr, GuestAvWrite write) {
    return addr && write(reinterpret_cast<void *>(addr), sizeof(T));
}
template <class T> void Store(uint64_t addr, const T &value) {
    std::memcpy(reinterpret_cast<void *>(addr), &value, sizeof(T));
}
void *KYTY_SYSV_ABI Allocate(void *object, uint32_t align, uint32_t size) {
    auto &p = *static_cast<Player *>(object);
    void *data = p.guest_mem.allocate(p.guest_mem.object_pointer, align, size);
    return data && p.write(data, size) ? data : nullptr;
}
void *KYTY_SYSV_ABI AllocateTexture(void *object, uint32_t align, uint32_t size) {
    auto &p = *static_cast<Player *>(object);
    void *data = p.guest_mem.allocate_texture(p.guest_mem.object_pointer, align, size);
    return data && p.write(data, size) ? data : nullptr;
}
void KYTY_SYSV_ABI Deallocate(void *object, void *data) {
    auto &p = *static_cast<Player *>(object);
    p.guest_mem.deallocate(p.guest_mem.object_pointer, data);
}
void KYTY_SYSV_ABI DeallocateTexture(void *object, void *data) {
    auto &p = *static_cast<Player *>(object);
    p.guest_mem.deallocate_texture(p.guest_mem.object_pointer, data);
}
template <class F> bool Callback(F f, GuestAvRead read, bool required = false) {
    return f ? read(reinterpret_cast<const void *>(f), 1) : !required;
}
template <class T>
bool Prepare(T &init, Player &p, GuestAvRead read, GuestAvWrite write, std::string &language) {
    const auto &m = init.memory_replacement;
    const auto &f = init.file_replacement;
    if (!Callback(m.allocate, read, true) || !Callback(m.deallocate, read, true) ||
        !Callback(m.allocate_texture, read, true) || !Callback(m.deallocate_texture, read, true) ||
        !Callback(init.event_replacement.event_callback, read))
        return false;
    const bool file_any = f.open || f.close || f.read_offset || f.size;
    if (file_any && (!Callback(f.open, read, true) || !Callback(f.close, read, true) ||
                     !Callback(f.read_offset, read, true) || !Callback(f.size, read, true)))
        return false;
    if (init.default_language) {
        if (!CopyString(reinterpret_cast<uint64_t>(init.default_language), language, read))
            return false;
        init.default_language = language.c_str();
    }
    p.guest_mem = m;
    p.write = write;
    init.memory_replacement = {&p, Allocate, Deallocate, AllocateTexture, DeallocateTexture};
    return true;
}
uint64_t Register(const std::shared_ptr<Player> &p) {
    if (!p->native)
        return 0;
    std::lock_guard lock(registry_mutex);
    const auto id = next_handle++;
    players.emplace(id, p);
    return id;
}
std::shared_ptr<Player> Find(uint64_t id) {
    std::lock_guard lock(registry_mutex);
    auto it = players.find(id);
    return it == players.end() ? nullptr : it->second;
}
} // namespace

bool GuestAvPlayerCall(std::string_view name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t *result, GuestAvRead read, GuestAvWrite write) {
    if (!name.starts_with("sceAvPlayer") || !result || !read || !write)
        return false;
    static_assert(sizeof(AvPlayerInitData) == 120);
    static_assert(sizeof(AvPlayerFrameInfo) == 40);
    int64_t rc = AVPLAYER_ERROR_INVALID_PARAMS;
    if (name == "sceAvPlayerInit" || name == "sceAvPlayerInitEx") {
        auto p = std::make_shared<Player>();
        std::string language;
        if (name == "sceAvPlayerInit") {
            AvPlayerInitData init{};
            if (Read(a0, init, read) && Prepare(init, *p, read, write, language))
                p->native = AvPlayerInit(&init);
            *result = Register(p);
        } else {
            AvPlayerInitDataEx init{};
            if (Output<uint64_t>(a1, write) && Read(a0, init, read) && init.this_size == sizeof(init) &&
                Prepare(init, *p, read, write, language)) {
                rc = AvPlayerInitEx(&init, &p->native);
                const auto id = Register(p);
                Store(a1, id);
            }
            *result = static_cast<uint64_t>(rc);
        }
    } else if (name == "sceAvPlayerSetLogCallback") {
        *result = static_cast<uint64_t>(
            AvPlayerSetLogCallback(reinterpret_cast<void *>(a0), reinterpret_cast<void *>(a1)));
    } else {
        auto p = Find(a0);
        const bool boolean = name == "sceAvPlayerIsActive" || name == "sceAvPlayerGetVideoData" ||
                             name == "sceAvPlayerGetVideoDataEx" || name == "sceAvPlayerGetAudioData";
        if (boolean || name == "sceAvPlayerCurrentTime")
            rc = 0;
        if (p) {
            std::lock_guard lock(p->mutex);
            auto *h = p->native;
            if (!h) {
                *result = static_cast<uint64_t>(rc);
                return true;
            }
            if (name == "sceAvPlayerClose") {
                {
                    std::lock_guard guard(registry_mutex);
                    players.erase(a0);
                }
                p->native = nullptr;
                rc = AvPlayerClose(h);
            } else if (name == "sceAvPlayerPostInit") {
                AvPlayerPostInitData value{};
                if (Read(a1, value, read))
                    rc = AvPlayerPostInit(h, &value);
            } else if (name == "sceAvPlayerAddSource") {
                std::string path;
                if (CopyString(a1, path, read)) {
                    rc = AvPlayerAddSource(h, path.c_str());
                    printf("[AVPLAYER-SOURCE] handle=0x%llx path=%s rc=%lld\n", a0, path.c_str(), rc);
                }
            } else if (name == "sceAvPlayerAddSourceEx") {
                AvPlayerSourceDetails value{};
                if (Read(a2, value, read) && value.uri.name && value.uri.length && value.uri.length <= 4096 &&
                    read(value.uri.name, value.uri.length)) {
                    std::string path(value.uri.name, value.uri.length);
                    value.uri.name = path.data();
                    rc = AvPlayerAddSourceEx(h, static_cast<uint32_t>(a1), &value);
                    printf("[AVPLAYER-SOURCE] handle=0x%llx path=%s rc=%lld\n", a0, path.c_str(), rc);
                }
            } else if (name == "sceAvPlayerStreamCount")
                rc = AvPlayerStreamCount(h);
            else if (name == "sceAvPlayerGetStreamInfo") {
                if (Output<AvPlayerStreamInfo>(a2, write)) {
                    AvPlayerStreamInfo v{};
                    rc = AvPlayerGetStreamInfo(h, a1, &v);
                    if (!rc)
                        Store(a2, v);
                }
            } else if (name == "sceAvPlayerGetStreamInfoEx") {
                if (Output<AvPlayerStreamInfoEx>(a2, write)) {
                    AvPlayerStreamInfoEx v{};
                    rc = AvPlayerGetStreamInfoEx(h, a1, &v);
                    if (!rc)
                        Store(a2, v);
                }
            } else if (name == "sceAvPlayerEnableStream")
                rc = AvPlayerEnableStream(h, a1);
            else if (name == "sceAvPlayerDisableStream")
                rc = AvPlayerDisableStream(h, a1);
            else if (name == "sceAvPlayerChangeStream")
                rc = AvPlayerChangeStream(h, a1, a2);
            else if (name == "sceAvPlayerStart")
                rc = AvPlayerStart(h);
            else if (name == "sceAvPlayerStartEx") {
                AvPlayerStartInfoEx v{};
                if (!a1)
                    rc = AvPlayerStartEx(h, nullptr);
                else if (Read(a1, v, read) && v.this_size == sizeof(v))
                    rc = AvPlayerStartEx(h, &v);
            } else if (name == "sceAvPlayerStop")
                rc = AvPlayerStop(h);
            else if (name == "sceAvPlayerPause")
                rc = AvPlayerPause(h);
            else if (name == "sceAvPlayerResume")
                rc = AvPlayerResume(h);
            else if (name == "sceAvPlayerSetLooping")
                rc = AvPlayerSetLooping(h, a1 != 0);
            else if (name == "sceAvPlayerSetTrickSpeed")
                rc = AvPlayerSetTrickSpeed(h, static_cast<int32_t>(a1));
            else if (name == "sceAvPlayerSetAvSyncMode")
                rc = AvPlayerSetAvSyncMode(h, a1);
            else if (name == "sceAvPlayerSetAvailableBandwidth")
                rc = AvPlayerSetAvailableBandwidth(h, a1, a2, a3);
            else if (name == "sceAvPlayerIsActive")
                rc = AvPlayerIsActive(h);
            else if (name == "sceAvPlayerCurrentTime")
                rc = static_cast<int64_t>(AvPlayerCurrentTime(h));
            else if (name == "sceAvPlayerJumpToTime")
                rc = AvPlayerJumpToTime(h, a1);
            else if (name == "sceAvPlayerGetVideoDataEx") {
                if (Output<AvPlayerFrameInfoEx>(a1, write)) {
                    AvPlayerFrameInfoEx v{};
                    rc = AvPlayerGetVideoDataEx(h, &v);
                    if (rc) {
                        Store(a1, v);
                        if (frame_logs.fetch_add(1) < 8)
                            printf("[AVPLAYER-VIDEO] native_decode=1 size=%ux%u pts=%llu\n",
                                   v.details.video.width, v.details.video.height, v.time_stamp);
                    }
                }
            } else if (name == "sceAvPlayerGetVideoData" || name == "sceAvPlayerGetAudioData") {
                if (Output<AvPlayerFrameInfo>(a1, write)) {
                    AvPlayerFrameInfo v{};
                    const bool audio = name == "sceAvPlayerGetAudioData";
                    rc = audio ? AvPlayerGetAudioData(h, &v) : AvPlayerGetVideoData(h, &v);
                    if (rc) {
                        Store(a1, v);
                        if (audio && audio_logs.fetch_add(1) < 8) {
                            printf("[AVPLAYER-AUDIO] native_decode=1 channels=%u rate=%u bytes=%u pts=%llu\n",
                                   v.details.audio.channel_count, v.details.audio.sample_rate,
                                   v.details.audio.size, v.time_stamp);
                        } else if (!audio && frame_logs.fetch_add(1) < 8)
                            printf("[AVPLAYER-VIDEO] native_decode=1 size=%ux%u pts=%llu\n",
                                   v.details.video.width, v.details.video.height, v.time_stamp);
                    }
                }
            } else
                rc = AVPLAYER_ERROR_NOT_SUPPORTED;
        }
        *result = static_cast<uint64_t>(rc);
    }
    if (call_logs.fetch_add(1) < 64)
        printf("[AVPLAYER-CALL] %.*s handle_or_arg=0x%llx result=0x%llx\n", static_cast<int>(name.size()),
               name.data(), a0, *result);
    return true;
}