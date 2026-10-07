#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

struct Packet { unsigned path; bool hl, image, continuation; uint32_t id; };
static void submit(GifArbiter &arbiter, const Packet &p)
{
    std::array<uint8_t, 16> data{};
    uint64_t tag = (p.image ? 2ull : 0ull) << 58;
    std::memcpy(data.data(), &tag, 8);
    std::memcpy(data.data() + 8, &p.id, 4);
    arbiter.submit(static_cast<GifPathId>(p.path), data.data(), data.size(), p.hl, p.continuation);
}
int main()
{
    std::vector<uint32_t> actual;
    GifArbiter arbiter([&](const uint8_t *bytes, uint32_t size, GifPathId, bool continuation) {
        assert(size == 16); uint32_t id; std::memcpy(&id, bytes + 8, 4);
        assert(continuation == (id == 6)); actual.push_back(id);
    });
    std::array<Packet, 7> packets{{
        {1,false,false,false,0}, {2,true,false,false,1},
        {2,false,false,false,2}, {3,false,false,false,3},
        {3,false,true,false,4}, {3,false,false,false,5},
        {3,false,true,true,6}
    }};
    // Regression: normal PATH2 behind DIRECTHL cannot overtake its head.
    submit(arbiter, packets[1]);submit(arbiter, packets[2]);submit(arbiter, packets[4]);
    arbiter.drain();assert((actual == std::vector<uint32_t>{4,1,2}));actual.clear();
    // IMAGE cannot overtake earlier PATH3 setup, even with DIRECTHL waiting.
    submit(arbiter, packets[3]);submit(arbiter, packets[4]);submit(arbiter, packets[1]);
    arbiter.drain();assert((actual == std::vector<uint32_t>{1,3,4}));actual.clear();
    std::array<unsigned, 7> order{0,1,2,3,4,5,6};unsigned tested=0;
    do {
        std::array<std::vector<uint32_t>,3> input,output;
        for(unsigned i:order){submit(arbiter,packets[i]);input[packets[i].path-1].push_back(i);}
        arbiter.drain();assert(arbiter.empty());assert(actual.size()==7);
        assert(actual.front()==0); // PATH1 always wins.
        for(unsigned i:actual)output[packets[i].path-1].push_back(i);
        assert(input==output);actual.clear();tested++;
    } while(std::next_permutation(order.begin(),order.end()));
    // A callback can enqueue a new packet without invalidating the current one.
    bool again=true;
    arbiter.setProcessPacketFn([&](const uint8_t* bytes,uint32_t,GifPathId,bool){
        uint32_t id;std::memcpy(&id,bytes+8,4);actual.push_back(id);
        if(again){again=false;submit(arbiter,packets[4]);}
    });
    submit(arbiter,packets[3]);arbiter.drain();assert(!arbiter.empty());
    arbiter.drain();assert((actual==std::vector<uint32_t>{3,4}));assert(arbiter.empty());
    // A flattened DMA chain can transfer ownership without copying its payload.
    again=false;actual.clear();
    std::vector<uint8_t> owned(4u * 1024u * 1024u, 0);
    uint32_t id=5;
    std::memcpy(owned.data()+8,&id,4);
    const uint8_t *original=owned.data();
    arbiter.setProcessPacketFn([&](const uint8_t *bytes,uint32_t size,GifPathId path,bool){
        assert(bytes==original && size==4u*1024u*1024u && path==GifPathId::Path3);
        uint32_t received;std::memcpy(&received,bytes+8,4);actual.push_back(received);
    });
    arbiter.submit(GifPathId::Path3,std::move(owned));
    arbiter.drain();assert((actual==std::vector<uint32_t>{5}));
    std::printf("PASS: %u permutations preserve per-path FIFO, PATH1 priority, DIRECTHL regression and continuation/reentrancy\n",tested);
}
