#pragma once
#include "libs/audio.h"

namespace Libs::Audio::AvPlayer {
using AvPlayerAllocate = KYTY_SYSV_ABI void *(*)(void *, uint32_t, uint32_t);
using AvPlayerDeallocate = KYTY_SYSV_ABI void (*)(void *, void *);
using AvPlayerAllocateTexture = KYTY_SYSV_ABI void *(*)(void *, uint32_t, uint32_t);
using AvPlayerDeallocateTexture = KYTY_SYSV_ABI void (*)(void *, void *);
using AvPlayerOpenFile = KYTY_SYSV_ABI int (*)(void *, const char *);
using AvPlayerCloseFile = KYTY_SYSV_ABI int (*)(void *);
using AvPlayerReadOffsetFile = KYTY_SYSV_ABI int (*)(void *, uint8_t *, uint64_t, uint32_t);
using AvPlayerSizeFile = KYTY_SYSV_ABI uint64_t (*)(void *);
using AvPlayerEventCallback = KYTY_SYSV_ABI void (*)(void *, int32_t, int32_t, void *);
using Bool = uint8_t;

constexpr int32_t AVPLAYER_EVENT_STATE_STOP = 0x01;
constexpr int32_t AVPLAYER_EVENT_STATE_READY = 0x02;
constexpr int32_t AVPLAYER_EVENT_STATE_PLAY = 0x03;
constexpr int32_t AVPLAYER_EVENT_STATE_PAUSE = 0x04;
constexpr int32_t AVPLAYER_EVENT_WARNING_ID = 0x20;
constexpr int32_t AVPLAYER_ERROR_INVALID_PARAMS = -2140536831;
constexpr int32_t AVPLAYER_ERROR_OPERATION_FAILED = -2140536830;
constexpr int32_t AVPLAYER_ERROR_NO_MEMORY = -2140536829;
constexpr int32_t AVPLAYER_ERROR_NOT_SUPPORTED = -2140536828;
constexpr int32_t AVPLAYER_WARNING_LOOPING_BACK = -2140536671;
constexpr int32_t AVPLAYER_WARNING_JUMP_COMPLETE = -2140536669;
constexpr int32_t AVPLAYER_TRICK_SPEED_NORMAL = 100;
constexpr int32_t AVPLAYER_TRICK_SPEED_MIN = 400;
constexpr int32_t AVPLAYER_TRICK_SPEED_MAX = 3200;

enum AvPlayerUriType : uint32_t { AvPlayerUriTypeSource = 0 };
enum AvPlayerSourceType : uint32_t {
    AvPlayerSourceUnknown = 0,
    AvPlayerSourceFileMp4 = 1,
    AvPlayerSourceFileWebm = 2,
    AvPlayerSourceHls = 8
};

enum AvPlayerDebuglevels : int { AvplayerDbgNone, AvplayerDbgInfo, AvplayerDbgWarnings, AvplayerDbgAll };
enum AvPlayerStreamType : uint32_t {
    AvPlayerStreamUnknown = 0,
    AvPlayerStreamVideo = 1,
    AvPlayerStreamAudio = 2,
    AvPlayerStreamTimedText = 3
};
enum AvPlayerVideoDecoderType : uint32_t {
    AvPlayerVideoDecoderDefault = 0,
    AvPlayerVideoDecoderSoftware2 = 1
};
enum AvPlayerAudioDecoderType : uint32_t { AvPlayerAudioDecoderDefault = 0 };
enum AvPlayerAudioChannelOrder : uint32_t {
    AvPlayerAudioChannelOrderDefault = 0,
    AvPlayerAudioChannelOrderExt = 1,
    AvPlayerAudioChannelOrderLsRs = 2
};

struct AvPlayerMemAllocator {
    void *object_pointer = nullptr;
    AvPlayerAllocate allocate = nullptr;
    AvPlayerDeallocate deallocate = nullptr;
    AvPlayerAllocateTexture allocate_texture = nullptr;
    AvPlayerDeallocateTexture deallocate_texture = nullptr;
};
struct AvPlayerFileReplacement {
    void *object_pointer = nullptr;
    AvPlayerOpenFile open = nullptr;
    AvPlayerCloseFile close = nullptr;
    AvPlayerReadOffsetFile read_offset = nullptr;
    AvPlayerSizeFile size = nullptr;
};
struct AvPlayerEventReplacement {
    void *object_pointer = nullptr;
    AvPlayerEventCallback event_callback = nullptr;
};
struct AvPlayerUri {
    const char *name = nullptr;
    uint32_t length = 0;
};
struct AvPlayerSourceDetails {
    AvPlayerUri uri;
    uint8_t reserved1[64] = {};
    AvPlayerSourceType source_type = AvPlayerSourceUnknown;
    uint8_t reserved2[44] = {};
};
struct AvPlayerInitData {
    AvPlayerMemAllocator memory_replacement;
    AvPlayerFileReplacement file_replacement;
    AvPlayerEventReplacement event_replacement;
    AvPlayerDebuglevels debug_level = AvplayerDbgNone;
    uint32_t base_priority = 0;
    int32_t num_output_video_framebuffers = 0;
    Bool auto_start = 0;
    uint8_t reserved[3] = {};
    const char *default_language = nullptr;
};
struct AvPlayerThreadInfo {
    uint32_t priority = 0;
    uint32_t stack_size = 0;
    uint64_t affinity = 0;
    uint8_t reserved[32] = {};
};
struct AvPlayerInitDataEx {
    size_t this_size = 0;
    AvPlayerMemAllocator memory_replacement;
    AvPlayerFileReplacement file_replacement;
    AvPlayerEventReplacement event_replacement;
    const char *default_language = nullptr;
    AvPlayerDebuglevels debug_level = AvplayerDbgNone;
    Bool auto_start = 0;
    uint8_t reserved[3] = {};
    AvPlayerThreadInfo audio_decoder;
    AvPlayerThreadInfo video_decoder;
    AvPlayerThreadInfo demuxer;
    AvPlayerThreadInfo event;
    AvPlayerThreadInfo call_queue;
    AvPlayerThreadInfo http_command_processor;
    AvPlayerThreadInfo http_segment_manager;
    AvPlayerThreadInfo http_streamlist;
    AvPlayerThreadInfo file_streaming;
    int32_t num_output_video_framebuffers = 0;
    uint8_t reserved2[4] = {};
};
struct AvPlayerDecoderInit {
    union {
        AvPlayerVideoDecoderType video_type;
        AvPlayerAudioDecoderType audio_type;
        uint8_t reserved[4];
    } decoder_type{};
    uint8_t reserved[4] = {};
    union {
        struct {
            uint64_t cpu_affinity_mask;
            int32_t cpu_thread_priority;
            uint8_t decode_input_queue_depth;
            uint8_t compute_pipe_id;
            uint8_t compute_queue_id;
            uint8_t reserved[17];
        } video_sw2;
        struct {
            AvPlayerAudioChannelOrder audio_channel_order;
            uint8_t reserved[28];
        } audio;
        uint8_t reserved[32];
    } decoder_params{};
};
struct AvPlayerHttpContext {
    uint32_t http_context_id = 0;
    uint32_t ssl_context_id = 0;
};
struct AvPlayerPostInitData {
    uint32_t demux_video_buffer_size = 0;
    uint8_t reserved[4] = {};
    AvPlayerDecoderInit video_decoder_init;
    AvPlayerDecoderInit audio_decoder_init;
    AvPlayerHttpContext http_context;
    uint8_t reserved2[56] = {};
};
struct AvPlayerStartInfoEx {
    size_t this_size = 0;
    uint64_t start_time_milliseconds = 0;
};
struct AvPlayerAudioEx {
    uint16_t channel_count = 0;
    uint8_t reserved[2] = {};
    uint32_t sample_rate = 0;
    uint32_t size = 0;
    uint8_t language_code[4] = {};
    uint8_t reserved1[64] = {};
};
struct AvPlayerAudio {
    uint16_t channel_count = 0;
    uint8_t reserved[2] = {};
    uint32_t sample_rate = 0;
    uint32_t size = 0;
    uint8_t language_code[4] = {};
};
struct AvPlayerVideoEx {
    uint32_t width = 0;
    uint32_t height = 0;
    float aspect_ratio = 0.0f;
    uint8_t language_code[4] = {};
    uint8_t reserved[4] = {};
    uint32_t crop_left_offset = 0;
    uint32_t crop_right_offset = 0;
    uint32_t crop_top_offset = 0;
    uint32_t crop_bottom_offset = 0;
    uint32_t pitch = 0;
    uint8_t luma_bit_depth = 0;
    uint8_t chroma_bit_depth = 0;
    Bool video_full_range_flag = 0;
    uint8_t reserved2[5] = {};
    double framerate = 0.0;
    uint32_t colour_primaries = 0;
    uint32_t transfer_characteristics = 0;
    uint8_t reserved3[16] = {};
};
struct AvPlayerVideo {
    uint32_t width = 0;
    uint32_t height = 0;
    float aspect_ratio = 0.0f;
    uint8_t language_code[4] = {};
};
struct AvPlayerTextPosition {
    int16_t top = 0;
    int16_t left = 0;
    int16_t bottom = 0;
    int16_t right = 0;
};
struct AvPlayerTimedTextEx {
    uint8_t language_code[4] = {};
    uint16_t text_size = 0;
    uint16_t font_size = 0;
    AvPlayerTextPosition position;
    uint8_t reserved1[64] = {};
};
struct AvPlayerTimedText {
    uint8_t language_code[4] = {};
    uint16_t text_size = 0;
    uint16_t font_size = 0;
    AvPlayerTextPosition position;
};
union AvPlayerStreamDetailsEx {
    AvPlayerAudioEx audio;
    AvPlayerVideoEx video;
    AvPlayerTimedTextEx subs;
    uint8_t reserved1[80];
};
union AvPlayerStreamDetails {
    AvPlayerAudio audio;
    AvPlayerVideo video;
    AvPlayerTimedText subs;
    uint8_t reserved[16];
};
struct AvPlayerFrameInfo {
    void *data = nullptr;
    uint8_t reserved[4] = {};
    uint64_t time_stamp = 0;
    AvPlayerStreamDetails details{};
};
struct AvPlayerFrameInfoEx {
    void *data = nullptr;
    uint8_t reserved[4] = {};
    uint64_t time_stamp = 0;
    AvPlayerStreamDetailsEx details{};
};
struct AvPlayerStreamInfo {
    AvPlayerStreamType type = AvPlayerStreamUnknown;
    uint8_t reserved[4] = {};
    AvPlayerStreamDetails details{};
    uint64_t duration = 0;
};
struct AvPlayerStreamInfoEx {
    size_t this_size = 0;
    AvPlayerStreamType type = AvPlayerStreamUnknown;
    uint8_t reserved[4] = {};
    AvPlayerStreamDetailsEx details{};
    uint64_t duration = 0;
};

static_assert(sizeof(AvPlayerInitData) == 120);
static_assert(offsetof(AvPlayerInitData, base_priority) == 100);
static_assert(offsetof(AvPlayerInitData, num_output_video_framebuffers) == 104);
static_assert(offsetof(AvPlayerInitData, default_language) == 112);
static_assert(offsetof(AvPlayerPostInitData, video_decoder_init) == 8);
static_assert(offsetof(AvPlayerVideoEx, framerate) == 48);
static_assert(sizeof(AvPlayerStreamDetailsEx) == 80);
static_assert(offsetof(AvPlayerStreamInfoEx, duration) == 96);
static_assert(offsetof(AvPlayerFrameInfoEx, details) == 24);

} // namespace Libs::Audio::AvPlayer
