// SPDX-License-Identifier: GPL-2.0-or-later
// Offline adapter to the pinned shader-compiler-rs Hades frontend/GLSL backend.
// No CPU emulation or GPU runtime is linked into the Vita application.
#include <fstream>
#include <iostream>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <iterator>
#include <shader_compiler/environment.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/backend/glsl/emit_glsl.h>

namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string& s) { std::cerr << "warning: " << s << '\n'; }
void Error(const std::string& s) { std::cerr << "error: " << s << '\n'; }
}
class CapturedEnvironment final : public Shader::Environment {
    std::vector<u8> code;
    u32 local{}, bound{}, viewport{};
    std::map<u64,u32> textures, formats, cbufs, replacements;
    static u32 lookup(const std::map<u64,u32>& m, u64 k, const char* kind) {
        auto it=m.find(k);
        if (it==m.end()) throw std::runtime_error(std::string("missing captured ")+kind+" key "+std::to_string(k));
        return it->second;
    }
public:
    // Constant-buffer keys, texture types and texture formats the shader read
    // but the offline environment did not supply. Each is answered with a
    // documented default so the translation still completes, and recorded so a
    // later run knows exactly what real state to provide.
    std::set<u64> missing_cbufs;
    std::set<u32> missing_texture_types;
    std::set<u32> missing_texture_formats;
    CapturedEnvironment(const char* binary, const char* metadata) {
        std::ifstream b(binary,std::ios::binary);
        if (!b) throw std::runtime_error("cannot read Maxwell input");
        code.assign(std::istreambuf_iterator<char>(b),{});
        if (code.size()<88 || code.size()%8) throw std::runtime_error("invalid SPH/code length");
        std::memcpy(&sph,code.data(),sizeof(sph));
        std::ifstream f(metadata); unsigned stage_id; size_t nt,nf,nc,nr;
        if (!(f>>stage_id>>start_address>>local>>bound>>viewport>>nt>>nf>>nc>>nr))
            throw std::runtime_error("invalid environment header");
        if ((stage_id!=0 && stage_id!=4) || bound>=18 || viewport>1 ||
            nt>4096 || nf>4096 || nc>65536 || nr>65536 ||
            u64(start_address)+code.size()>0x100000000ULL)
            throw std::runtime_error("unsupported environment/stage");
        stage=static_cast<Shader::Stage>(stage_id);
        if (sph.common0.shader_type.Value()!=(stage_id==0?1:5))
            throw std::runtime_error("SPH/captured stage mismatch");
        for (auto [m,n]: {std::pair{&textures,nt},{&formats,nf},{&cbufs,nc},{&replacements,nr}}) {
            for(size_t i=0;i<n;i++) {u64 key;u32 value;
                if (!(f>>key>>value) || !m->emplace(key,value).second)
                    throw std::runtime_error("truncated/duplicate environment entry");
            }
        }
        std::string extra; if(f>>extra) throw std::runtime_error("trailing environment data");
        is_propietary_driver=bound==2;
    }
    // Every address the frontend actually decodes is recorded. Without this the
    // question "did it read the slots I think it read" can only be guessed at, and
    // a whole class of wrong conclusions (mine included) comes from guessing which
    // words a translator consumed.
    std::set<u32> decoded_addresses;
    u64 ReadInstruction(u32 a) override {
        if(a<start_address || u64(a-start_address)+8>code.size() || (a-start_address)%8)
            throw std::runtime_error("instruction outside captured range");
        decoded_addresses.insert(a);
        u64 v; std::memcpy(&v,code.data()+a-start_address,8); return v;
    }
    // Maxwell shaders fold scalar constants out of constant buffers at compile
    // time, so translation needs a value for every (bank, offset) it reads. The
    // game's live constant data is not available offline, so a missing key is
    // answered with 0 and RECORDED instead of aborting: the GLSL keeps correct
    // structure with explicitly unknown constants, and the recorded keys say
    // exactly which values a real run must supply. Aborting would discard an
    // entire shader translation to hide one unknown constant.
    u32 ReadCbufValue(u32 i,u32 o) override {
        const u64 key=(u64(i)<<32)|o;
        auto it=cbufs.find(key);
        if(it!=cbufs.end()) return it->second;
        missing_cbufs.insert(key);
        return 0;
    }
    // A texture whose type was not captured is assumed to be a plain 2D colour
    // texture, which is the overwhelmingly common case for this title, and the
    // assumption is recorded rather than hidden.
    Shader::TextureType ReadTextureType(u32 h) override {
        auto it=textures.find(h);
        if(it==textures.end()) { missing_texture_types.insert(h); return Shader::TextureType::Color2D; }
        if(it->second>=Shader::NUM_TEXTURE_TYPES) throw std::runtime_error("unknown texture type");
        return static_cast<Shader::TextureType>(it->second);
    }
    // The cache pixel-format enum belongs to its producer and no offline mapping
    // exists, so an unknown format is reported as TexturePixelFormat::OTHER
    // instead of guessing a concrete format, and the hash is recorded.
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 h) override {
        auto it=formats.find(h);
        if(it==formats.end()) { missing_texture_formats.insert(h); return Shader::TexturePixelFormat::OTHER; }
        (void)it->second;
        return Shader::TexturePixelFormat::OTHER;
    }
    u32 ReadViewportTransformState() override {return viewport;}
    u32 TextureBoundBuffer() const override {return bound;}
    u32 LocalMemorySize() const override {return local;}
    u32 SharedMemorySize() const override {throw std::runtime_error("compute not supported");}
    std::array<u32,3> WorkgroupSize() const override {throw std::runtime_error("compute not supported");}
    bool HasHLEMacroState() const override {return false;}
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 b,u32 o) override {
        auto it=replacements.find((u64(b)<<32)|o);
        if(it==replacements.end()) return std::nullopt;
        if(it->second>2) throw std::runtime_error("invalid replacement enum");
        return static_cast<Shader::ReplaceConstant>(it->second);
    }
    void Dump(u64) override {}
};
int main(int argc,char** argv) {
    try {
        if(argc!=4) throw std::runtime_error("usage: maxwell-glsl code.bin environment.txt output.glsl");
        CapturedEnvironment env(argv[1],argv[2]);
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
        Shader::ObjectPool<Shader::IR::Inst> inst;
        Shader::ObjectPool<Shader::IR::Block> blocks;
        Shader::Maxwell::Flow::CFG cfg(env,flow,Shader::Maxwell::Location{env.StartAddress()+80});
        Shader::HostTranslateInfo host{};
        host.support_float16=false; host.support_int64=false;
        host.support_snorm_render_buffer=true; host.min_ssbo_alignment=16;
        auto program=Shader::Maxwell::TranslateProgram(inst,blocks,env,cfg,host);
        Shader::Profile profile{};
        profile.need_declared_frag_colors=true;
        Shader::RuntimeInfo runtime{};
        // Varying linking is checked again by our typed stage-pair layer.
        runtime.previous_stage_stores.mask.set();
        Shader::Backend::Bindings bindings{};
        auto glsl=Shader::Backend::GLSL::EmitGLSL(profile,runtime,program,bindings);
        std::ofstream out(argv[3],std::ios::binary);
        if(!out || !(out<<glsl)) throw std::runtime_error("cannot write GLSL");
        std::ofstream reflection(std::string(argv[3])+".resources.json");
        reflection<<"{\"stage\":"<<unsigned(program.stage)<<",\"constant_buffers\":[";
        bool first=true;
        for(const auto& d:program.info.constant_buffer_descriptors) {
            if(!first)reflection<<',';first=false;
            reflection<<"{\"bank\":"<<d.index<<",\"count\":"<<d.count<<'}';
        }
        reflection<<"],\"textures\":[";first=true;
        for(const auto& d:program.info.texture_descriptors) {
            if(!first)reflection<<',';first=false;
            reflection<<"{\"type\":"<<unsigned(d.type)<<",\"depth\":"<<d.is_depth
                <<",\"secondary\":"<<d.has_secondary<<",\"bank\":"<<d.cbuf_index
                <<",\"offset\":"<<d.cbuf_offset<<",\"count\":"<<d.count
                <<",\"shift\":"<<d.shift_left<<",\"size_shift\":"<<d.size_shift<<'}';
        }
        reflection<<"],\"storage_buffers\":"<<program.info.storage_buffers_descriptors.size()
                  <<",\"images\":"<<program.info.image_descriptors.size()<<"}\n";
        if(!reflection)throw std::runtime_error("cannot write reflection");
        // Record every constant the shader read that the environment did not
        // supply: these are the values a real run must provide. The GLSL is
        // structurally complete but folds a 0 wherever a key appears here.
        {
            std::ofstream miss(std::string(argv[3])+".missing-cbufs.json");
            miss<<"{\"count\":"<<env.missing_cbufs.size()<<",\"keys\":[";
            bool firstKey=true;
            for(u64 k:env.missing_cbufs) {
                if(!firstKey)miss<<',';firstKey=false;
                miss<<"{\"bank\":"<<(k>>32)<<",\"offset\":"<<(k&0xFFFFFFFFull)<<'}';
            }
            miss<<"],\"texture_types\":"<<env.missing_texture_types.size()<<",\"texture_formats\":"
                <<env.missing_texture_formats.size()<<"}\n";
            if(!miss)throw std::runtime_error("cannot write missing-cbuf report");
        }
        {
            std::ofstream dec(std::string(argv[3])+".decoded-addresses.txt");
            for(u32 a:env.decoded_addresses) dec<<(a-env.StartAddress())<<"\n";
            if(!dec)throw std::runtime_error("cannot write decoded-address report");
        }
        std::cout<<"decoded="<<env.decoded_addresses.size()<<" "
                 <<"stage="<<unsigned(program.stage)<<" cbufs="<<program.info.constant_buffer_descriptors.size()
                 <<" textures="<<program.info.texture_descriptors.size()<<" bytes="<<glsl.size()
                 <<" missing_cbufs="<<env.missing_cbufs.size()
                 <<" missing_tex_type="<<env.missing_texture_types.size()
                 <<" missing_tex_fmt="<<env.missing_texture_formats.size()<<'\n';
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
