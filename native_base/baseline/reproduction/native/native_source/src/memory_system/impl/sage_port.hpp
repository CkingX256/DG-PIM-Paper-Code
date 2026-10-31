


#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace SagePort {
inline std::uint16_t half_bits(float x) { return std::bit_cast<std::uint16_t>(_Float16(x)); }
inline float half_float(std::uint16_t x) { return float(std::bit_cast<_Float16>(x)); }
constexpr double Log2EHost = 1.44269504;
constexpr float Log2E = 1.44269504f;
constexpr int KeyBlock = 64;
struct Input {
  int L=0,D=0,q=0,h=0,H=0;
  std::function<float(int,int)> getK;
  std::function<float(int)> getQ;
  bool keys_prepared=false;
};
struct Counters {
  std::uint64_t fma_ops=0,add_ops=0,mul_ops=0,div_ops=0,sqrt_ops=0,exp_ops=0,compare_ops=0,round_ops=0;
  std::uint64_t scratch_read_bytes=0,scratch_write_bytes=0,metadata_read_bytes=0,metadata_write_bytes=0;
  std::uint64_t scratch_peak_bytes=0,metadata_live_bytes=0;
};
struct State {
  int query=-1,length=-1,dimension=-1;
  
  std::vector<std::uint16_t> key_bits,mean_bits;
  std::vector<float> key_scale;
};
struct Plan : Counters {
  std::vector<std::uint16_t> key_bits,query_bits;
  
  std::vector<float> score_multiplier;
  float query_scale=0.f;
  int key_read_begin=0,key_read_end=0,key_read_passes=0;
  int key_rewrite_begin=0,key_rewrite_end=0;
  std::uint64_t extra_dram_bytes=0;
  int zero_key_blocks=0;
  bool zero_query=false,rebuild_keys=false;
};
inline int quantize(float x,float scale) {
  if(!std::isfinite(x)||!std::isfinite(scale)||scale<0.f)
    throw std::runtime_error("Nonfinite or negative Sage quantizer scale/input");
  if(scale==0.f) return 0; 
  float y=x/scale;
  y += y>=0.f ? .5f : -.5f;
  const int z=int(y); 
  if(z < -127 || z > 127)throw std::runtime_error("Sage quantizer out of signed127 range");
  return z;
}
inline Plan prepare(const Input& in,State& state) {
  if(in.L<=0 || in.D<=0 || in.h<0 || in.h>=in.H || (!in.keys_prepared&&in.h==0&&!in.getK) || !in.getQ)
    throw std::runtime_error("Invalid Sage input");
  Plan p;
  const int blocks=(in.L+KeyBlock-1)/KeyBlock;
  if(in.h==0 && !in.keys_prepared) {
    state.query=in.q;state.length=in.L;state.dimension=in.D;
    state.key_bits.assign(std::size_t(in.L)*in.D,0);
    state.mean_bits.assign(in.D,0);state.key_scale.assign(blocks,0.f);
    std::vector<float> sum(in.D,0.f);
    for(int t=0;t<in.L;t++)for(int d=0;d<in.D;d++) {
      const float x=in.getK(t,d);
      if(!std::isfinite(x))throw std::runtime_error("Nonfinite Sage key");
      sum[d]+=x;
    }
    for(int d=0;d<in.D;d++)state.mean_bits[d]=half_bits(sum[d]/float(in.L));
    std::vector<std::uint16_t> tile;
    for(int b=0;b<blocks;b++) {
      const int begin=b*KeyBlock,end=std::min(in.L,begin+KeyBlock);
      tile.resize(std::size_t(end-begin)*in.D);
      float amax=0.f;
      for(int t=begin;t<end;t++)for(int d=0;d<in.D;d++) {
        const auto centered=half_bits(in.getK(t,d)-half_float(state.mean_bits[d]));
        tile[std::size_t(t-begin)*in.D+d]=centered;
        amax=std::max(amax,std::abs(half_float(centered)));
      }
      const float scale=amax/127.f;state.key_scale[b]=scale;
      if(scale==0.f)++p.zero_key_blocks;
      for(int t=begin;t<end;t++)for(int d=0;d<in.D;d++)
        state.key_bits[std::size_t(t)*in.D+d]=half_bits(float(quantize(half_float(tile[std::size_t(t-begin)*in.D+d]),scale)));
    }
    p.rebuild_keys=true;p.key_read_begin=0;p.key_read_end=in.L;p.key_read_passes=2;
    p.key_rewrite_begin=0;p.key_rewrite_end=in.L;
    const std::uint64_t elements=std::uint64_t(in.L)*in.D;
    
    
    p.add_ops+=2*elements; 
    p.add_ops+=elements;   
    p.div_ops+=in.D+blocks+elements;
    p.compare_ops+=2*elements+elements; 
    p.round_ops+=in.D+2*elements; 
    p.scratch_read_bytes+=4*elements+4*in.D+2*elements+2*elements;
    p.scratch_write_bytes+=4*elements+2*in.D+2*elements;
    p.metadata_write_bytes+=2*in.D+4*blocks;
  } else if(state.query!=in.q || state.length!=in.L || state.dimension!=in.D) {
    throw std::runtime_error("Sage Key representation reused across a wrong causal prefix");
  }
  p.key_bits=state.key_bits;p.query_bits.resize(in.D);
  std::vector<float> x(in.D);
  
  const float sm=float((1.0/std::sqrt(double(in.D)))*Log2EHost);
  float qmax=0.f;
  for(int d=0;d<in.D;d++) {
    x[d]=in.getQ(d)*sm;
    if(!std::isfinite(x[d]))throw std::runtime_error("Nonfinite Sage query");
    qmax=std::max(qmax,std::abs(x[d]));
  }
  const float qs=qmax/127.f;p.query_scale=qs;p.zero_query=qs==0.f;
  for(int d=0;d<in.D;d++)p.query_bits[d]=half_bits(float(quantize(x[d],qs)));
  p.score_multiplier.resize(in.L);
  const float root=std::sqrt(float(in.D));
  for(int b=0;b<blocks;b++) {
    
    
    float multiplier=((qs*state.key_scale[b])*root)/Log2E;
    for(int t=b*KeyBlock;t<std::min(in.L,(b+1)*KeyBlock);t++)p.score_multiplier[t]=multiplier;
  }
  p.mul_ops+=in.D+2*blocks+in.L; 
  p.add_ops+=in.D;
  p.div_ops+=in.D+blocks+2;p.sqrt_ops+=2;
  p.compare_ops+=3*in.D;p.round_ops+=in.D;
  p.scratch_read_bytes+=4*in.D+4*blocks;
  p.scratch_write_bytes+=4*in.D+4*blocks;
  p.metadata_read_bytes+=4*blocks;p.metadata_write_bytes+=4; 
  p.metadata_live_bytes=2*in.D+4*blocks+4;
  p.scratch_peak_bytes=2*std::uint64_t(KeyBlock)*in.D+8*in.D+4*blocks+64;
  
  p.extra_dram_bytes=2*std::uint64_t(in.L)*in.D+p.metadata_live_bytes;
  return p;
}
inline Plan make_plan(const Input& in,State& state){return prepare(in,state);}
} 
