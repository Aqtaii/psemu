#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <span>
#include <vector>
namespace Libs::Graphics::ShaderRecompiler::IR {
struct GuestReadRange {
    uint64_t address=0;
    uint64_t size=0;
    bool operator==(const GuestReadRange&) const = default;
};
inline constexpr std::size_t MaxGuestReadDwords=65536;
inline bool ValidateGuestReadFootprint(std::span<const GuestReadRange> ranges) {
    constexpr uint64_t limit=uint64_t{1}<<48;
    uint64_t end=0,bytes=0;
    if(ranges.size()>MaxGuestReadDwords)return false;
    for(const auto& range:ranges) {
        if(!range.address || range.address>=limit || !range.size || ((range.address|range.size)&3) ||
           range.size>limit-range.address || range.address<end ||
           range.size>MaxGuestReadDwords*4-bytes)return false;
        end=range.address+range.size;bytes+=range.size;
    }
    return true;
}

// Only addresses actually read by the existing source evaluator are recorded.
// Merge adjacent dwords, never fill gaps or invent a larger pointer window.
inline bool BuildGuestReadFootprint(std::span<const uint64_t> input,std::vector<GuestReadRange>* output) {
    constexpr uint64_t limit=uint64_t{1}<<48;
    if(!output || input.size()>MaxGuestReadDwords)return false;
    std::vector<uint64_t> sorted(input.begin(),input.end());
    std::sort(sorted.begin(),sorted.end());
    sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
    std::vector<GuestReadRange> next;
    for(const auto address:sorted) {
        if(!address || address>=limit || address>limit-4 || (address&3))return false;
        if(!next.empty() && next.back().address+next.back().size==address)next.back().size+=4;
        else next.push_back({address,4});
    }
    *output=std::move(next);
    return true;
}
}