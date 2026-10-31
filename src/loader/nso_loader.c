#include "nso_loader.h"
#include <string.h>

static uint32_t rd(const unsigned char *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint32_t ror(uint32_t x, unsigned n) { return (x>>n)|(x<<(32-n)); }
void vp_sha256(const void *input, size_t bytes, unsigned char out[32]) {
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const unsigned char *p=input;
    size_t blocks=bytes/64+1+((bytes%64)>=56);
    for (size_t block=0;block<blocks;++block) {
        unsigned char b[64]={0}; size_t start=block*64;
        for (unsigned i=0;i<64;++i) {
            size_t pos=start+i;
            if (pos<bytes) b[i]=p[pos];
            else if (pos==bytes) b[i]=0x80;
        }
        if (block==blocks-1) {
            uint64_t bits=(uint64_t)bytes*8;
            for (unsigned i=0;i<8;++i) b[63-i]=(unsigned char)(bits>>(8*i));
        }
        uint32_t w[64];
        for (unsigned i=0;i<16;++i) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
        for (unsigned i=16;i<64;++i) {
            uint32_t a=w[i-15],z=w[i-2];
            w[i]=w[i-16]+(ror(a,7)^ror(a,18)^(a>>3))+w[i-7]+(ror(z,17)^ror(z,19)^(z>>10));
        }
        uint32_t a=h[0],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7],v=h[1];
        for (unsigned i=0;i<64;++i) {
            uint32_t t=z+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            uint32_t u=(ror(a,2)^ror(a,13)^ror(a,22))+((a&v)^(a&c)^(v&c));
            z=g;g=f;f=e;e=d+t;d=c;c=v;v=a;a=t+u;
        }
        h[0]+=a;h[1]+=v;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    for (unsigned i=0;i<32;++i) out[i]=(unsigned char)(h[i/4]>>(24-8*(i%4)));
}
int vp_nso_inspect(VPNSO *n,const void *input,uint32_t bytes,const unsigned char sha[32],const unsigned char build[32]) {
    if (!n || !input || bytes<256 || !sha || !build) return VP_NSO_INPUT;
    const unsigned char *p=input; VPNSO next; memset(&next,0,sizeof(next));
    vp_sha256(p,bytes,next.file_sha256);
    if (memcmp(next.file_sha256,sha,32) || memcmp(p+0x40,build,32)) return VP_NSO_HASH;
    if (memcmp(p,"NSO0",4) || rd(p+4) || rd(p+8) || (rd(p+12)&~0x3fu) || (rd(p+12)&0x38)!=0x38) return VP_NSO_INPUT;
    next.flags=rd(p+12);next.file_bytes=bytes;memcpy(next.build_id,build,32);
    uint32_t end=0;
    for (unsigned i=0;i<3;++i) {
        VPNSOSegment *s=&next.segments[i];
        s->source=rd(p+16+i*16);s->offset=rd(p+20+i*16);s->bytes=rd(p+24+i*16);
        s->stored=(next.flags&(1u<<i))?rd(p+0x60+i*4):s->bytes;
        memcpy(s->sha256,p+0xa0+i*32,32);
        if (s->source<256 || s->source>bytes || !s->stored || s->stored>bytes-s->source
            || (s->offset&4095u) || s->offset<end || s->offset>0x8000000u
            || !s->bytes || s->bytes>0x8000000u-s->offset || (!i && s->offset)) return VP_NSO_LAYOUT;
        for (unsigned j=0;j<i;++j) {
            const VPNSOSegment *t=&next.segments[j];
            if (s->source<t->source+t->stored && t->source<s->source+s->stored) return VP_NSO_LAYOUT;
        }
        end=s->offset+s->bytes;
    }
    if (rd(p+0x3c)>0x8000000u-end) return VP_NSO_LAYOUT;
    next.bss_begin=end;next.bss_end=next.span=end+rd(p+0x3c);
    uint32_t ro=next.segments[1].offset, size=next.segments[1].bytes;
    uint32_t str=rd(p+0x90),sy=rd(p+0x98);
    next.string_bytes=rd(p+0x94);next.symbol_bytes=rd(p+0x9c);
    if (str>size || next.string_bytes>size-str || sy>size || next.symbol_bytes>size-sy
        || !next.string_bytes || !next.symbol_bytes || next.symbol_bytes%16) return VP_NSO_LAYOUT;
    next.strings=ro+str;next.symbols=ro+sy;*n=next;return 0;
}
static int extension(const unsigned char *p,uint32_t bytes,uint32_t *pos,uint32_t *length) {
    unsigned v;
    do {
        if (*pos>=bytes) return VP_NSO_LZ4;
        v=p[(*pos)++];
        if (*length>UINT32_MAX-v) return VP_NSO_LZ4;
        *length+=v;
    } while (v==255);
    return 0;
}
static int lz4(const unsigned char *p,uint32_t bytes,unsigned char *out,uint32_t capacity) {
    uint32_t pos=0,w=0;
    while (pos<bytes) {
        unsigned token=p[pos++];uint32_t literal=token>>4;
        if (literal==15 && extension(p,bytes,&pos,&literal)) return VP_NSO_LZ4;
        if (literal>bytes-pos || literal>capacity-w) return VP_NSO_LZ4;
        memcpy(out+w,p+pos,literal);pos+=literal;w+=literal;
        if (pos==bytes) return w==capacity?0:VP_NSO_LZ4;
        if (bytes-pos<2) return VP_NSO_LZ4;
        uint32_t distance=p[pos]|((uint32_t)p[pos+1]<<8);pos+=2;
        if (!distance || distance>w) return VP_NSO_LZ4;
        uint32_t match=(token&15)+4;
        if ((token&15)==15 && extension(p,bytes,&pos,&match)) return VP_NSO_LZ4;
        if (match>capacity-w) return VP_NSO_LZ4;
        for (uint32_t i=0;i<match;++i) { out[w]=out[w-distance];++w; }
    }
    return VP_NSO_LZ4;
}
int vp_nso_decode(const VPNSO *n,const void *input,uint32_t bytes,void *memory,uint32_t capacity) {
    if (!n || !input || !memory || bytes!=n->file_bytes || n->span>capacity) return VP_NSO_CAPACITY;
    uintptr_t source=(uintptr_t)input,destination=(uintptr_t)memory;
    if (source>UINTPTR_MAX-bytes || destination>UINTPTR_MAX-n->span
        || (source<destination+n->span && destination<source+bytes)) return VP_NSO_INPUT;
    VPNSO checked;
    int rc=vp_nso_inspect(&checked,input,bytes,n->file_sha256,n->build_id);
    if (rc || memcmp(&checked,n,sizeof(checked))) return rc?rc:VP_NSO_INPUT;
    unsigned char *out=memory;const unsigned char *p=input;memset(out,0,n->span);
    for (unsigned i=0;i<3;++i) {
        const VPNSOSegment *s=&n->segments[i];
        if (n->flags&(1u<<i)) rc=lz4(p+s->source,s->stored,out+s->offset,s->bytes);
        else memcpy(out+s->offset,p+s->source,s->bytes);
        unsigned char sha[32];
        if (!rc) { vp_sha256(out+s->offset,s->bytes,sha);if (memcmp(sha,s->sha256,32)) rc=VP_NSO_HASH; }
        if (rc) { memset(out,0,n->span);return rc; }
    }
    return 0;
}
