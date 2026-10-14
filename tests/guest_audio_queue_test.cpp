#include "guest_audio_queue.h"
#include <cassert>
int main() {
    const GuestAudioContextParam defaults;
    assert(defaults.max_ports==256 && defaults.queue_depth==4 && defaults.num_grains==512);
    GuestAudioQueue queue(2,480);
    assert(queue.Available()==2 && queue.grain_micros==10000);
    assert(queue.Push(100000) && queue.Push(100000));
    assert(!queue.Push(109999) && queue.queued==2 && queue.Available()==0);
    queue.Advance(110000);
    assert(queue.queued==1 && queue.Available()==1 && queue.last_update==110000);
    assert(queue.Push(115000) && queue.queued==2);
    queue.Advance(119999);assert(queue.queued==2);
    queue.Advance(120000);assert(queue.queued==1);
    queue.Advance(130000);assert(queue.queued==0 && queue.Available()==2);
    assert(queue.Push(999999));queue.Advance(1);assert(queue.queued==1);
    GuestAudioQueue fallback(0,0);
    assert(fallback.depth==4 && fallback.grain_micros==10666);
    GuestAudioQueue large(1,0xffffffffu);
    assert(large.grain_micros==uint64_t(0xffffffffu)*1000000/48000);
}
