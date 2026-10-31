#include "gpu_stream.h"
#include <assert.h>
#include <stdio.h>

typedef struct { vp_gpu_command_buffer *buffer; size_t count; } Sink;
static void store(void *ctx,const vp_gpu_write *write) {
    Sink *sink=ctx;
    assert(sink->count<sink->buffer->capacity);
    sink->buffer->writes[sink->count++]=*write;
}
int main(void) {
    vp_gpu_command_buffer buffer={0};
    vp_gpu_decode_stats stats;
    const size_t words=8716u;
    assert(vp_gpu_command_buffer_reserve(&buffer,words)==0);
    /* Immediate writes exercise the worst case: one output per input word.
     * Put the final draw past the old 8192-word boundary. */
    for (size_t i=0;i<words;i++) buffer.words[i]=(4u<<29)|((i&0x1fffu)<<16)|0x360u;
    buffer.words[words-1]=(4u<<29)|(1u<<16)|0x585u;
    Sink sink={&buffer,0};
    assert(vp_gpu_decode(buffer.words,words,store,&sink,&stats)==0);
    assert(stats.writes==words && sink.count==words);
    assert(buffer.writes[8192].method==0x360u);
    assert(buffer.writes[words-1].method==0x585u);
    assert(buffer.writes[words-1].value==1u);
    void *old=buffer.storage;
    assert(vp_gpu_command_buffer_reserve(&buffer,0x200000u)<0);
    assert(buffer.storage==old && buffer.capacity==words);
    assert(vp_gpu_command_buffer_reserve(&buffer,32u)==0 && buffer.storage==old);
    assert(vp_gpu_command_buffer_reserve(&buffer,32769u)==0);
    buffer.words[0]=5u<<29; /* Zero-argument increment-once is invalid. */
    assert(vp_gpu_decode(buffer.words,1,NULL,NULL,&stats)<0);
    buffer.words[0]=(3u<<29)|(2u<<16)|0x360u;
    assert(vp_gpu_decode(buffer.words,1,NULL,NULL,&stats)<0);
    vp_gpu_command_buffer_free(&buffer);
    assert(!buffer.storage && !buffer.capacity && !buffer.writes);
    puts("gpu command buffer: PASS (8716 writes, tail draw, bounds, malformed packets)");
    return 0;
}
