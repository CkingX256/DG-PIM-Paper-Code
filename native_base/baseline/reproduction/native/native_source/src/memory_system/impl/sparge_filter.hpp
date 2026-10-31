#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>



namespace SpargePort {
struct Config {
  int block_tokens=64;
  float theta=-0.1f, topk=0.5f, cdf=0.98f, pv_threshold=50.0f;
  bool use_cdf=false;
};
struct Counters {
  uint64_t fma_ops=0,add_ops=0,mul_ops=0,div_ops=0,sqrt_ops=0,exp_ops=0,compare_ops=0,round_ops=0;
  uint64_t scratch_read_bytes=0,scratch_write_bytes=0,metadata_read_bytes=0,metadata_write_bytes=0;
  uint64_t scratch_peak_bytes=0,metadata_live_bytes=0;
};
struct Input {
  int L=0,D=0,q=0,h=0,H=1;
  std::function<float(int,int)> getK;
  std::function<float(int)> getQ;
  Config config;
};
struct Summary {std::vector<float> mean; float similarity=0; bool eligible=false; int size=0;};
struct State {int L=0,D=0;std::vector<Summary> blocks;};
struct Plan: Counters {
  std::vector<uint8_t> keep;
  std::vector<int> order; 
  int key_read_begin=0,key_read_end=0;
  int selected_blocks=0,total_blocks=0,eligible_blocks=0,pv_skipped_blocks=0;
  bool prediction_nonfinite_fallback=false;
  std::vector<float> pooled_scores,pooled_probability,block_similarity;
};
inline float half(float x) {return float((_Float16)x);}
inline uint64_t ceildiv(uint64_t x,uint64_t y){return (x+y-1)/y;}

inline Summary summarize(const Input& in,int begin,int end,Plan& p) {
  const int D=in.D,n=end-begin;
  Summary result;result.mean.assign(D,0);result.size=n;
  std::vector<float> normalized_sum(D,0),row(D);bool zero_row=false;
  p.scratch_peak_bytes=std::max<uint64_t>(p.scratch_peak_bytes,12ull*D);
  for(int i=0;i<n;++i) {
    float norm2=0;
    for(int d=0;d<D;++d) {
      float x=in.getK(begin+i,d);row[d]=x;
      result.mean[d]+=x;++p.add_ops;norm2=std::fma(x,x,norm2);++p.fma_ops;
      p.scratch_write_bytes+=4; 
    }
    float norm=std::sqrt(norm2);++p.sqrt_ops;
    zero_row=zero_row||norm==0;++p.compare_ops;
    for(int d=0;d<D;++d) {
      
      float x=norm>0?row[d]/norm:0;++p.div_ops;
      float normalized=half(x);++p.round_ops;
      normalized_sum[d]+=normalized;++p.add_ops;
      p.scratch_read_bytes+=8;p.scratch_write_bytes+=4;
    }
  }
  for(int d=0;d<D;++d){result.mean[d]=half(result.mean[d]/n);++p.div_ops;++p.round_ops;}
  float gram_sum=0;
  
  
  
  for(int d=0;d<D;++d){gram_sum=std::fma(normalized_sum[d],normalized_sum[d],gram_sum);++p.fma_ops;p.scratch_read_bytes+=4;}
  result.similarity=gram_sum/float(n*n);++p.div_ops;
  result.eligible=!zero_row&&result.similarity>in.config.theta;++p.compare_ops;
  return result;
}

inline Plan stage1(const Input& in,State& state) {
  if(in.L<1||in.D<1||in.config.block_tokens!=64||!in.getK||!in.getQ)
    throw std::runtime_error("Invalid Sparge decode port input");
  Plan p;p.keep.assign(in.L,0);const int B=64,G=(in.L+B-1)/B;
  bool reset=state.D!=in.D||state.L>in.L||state.blocks.empty();
  if(reset){state=State{};state.D=in.D;}
  if(reset||state.L!=in.L){
    int begin=reset?0:(state.L/B)*B;
    p.key_read_begin=begin;p.key_read_end=in.L;
    state.blocks.resize(G);
    for(int g=begin/B;g<G;++g){state.blocks[g]=summarize(in,g*B,std::min(in.L,(g+1)*B),p);p.metadata_write_bytes+=2ull*in.D+8;}
    p.metadata_write_bytes+=32;state.L=in.L;
  }
  p.total_blocks=G;p.metadata_live_bytes=32ull+G*(2ull*in.D+8);
  p.metadata_read_bytes=32ull+G*(2ull*in.D+8);
  std::vector<float> q(in.D),normalized_q(in.D);float qnorm2=0;
  for(int d=0;d<in.D;++d){q[d]=in.getQ(d);qnorm2=std::fma(q[d],q[d],qnorm2);++p.fma_ops;p.scratch_read_bytes+=2;}
  float qnorm=std::sqrt(qnorm2);++p.sqrt_ops;float qsim=0;
  for(int d=0;d<in.D;++d){normalized_q[d]=half(qnorm>0?q[d]/qnorm:0);++p.div_ops;++p.round_ops;qsim=std::fma(normalized_q[d],normalized_q[d],qsim);++p.fma_ops;p.scratch_write_bytes+=2;}
  bool query_eligible=qnorm>0&&qsim>in.config.theta;++p.compare_ops;
  std::vector<float> scores(G,-std::numeric_limits<float>::infinity()),probability(G,0);
  std::vector<int> idx(G);std::iota(idx.begin(),idx.end(),0);
  float scale=1.0f/std::sqrt(float(in.D));++p.sqrt_ops;++p.div_ops;
  for(int g=0;g<G;++g){
    const auto&s=state.blocks[g];p.block_similarity.push_back(s.similarity);p.eligible_blocks+=s.eligible;
    float dot=0;for(int d=0;d<in.D;++d){dot=std::fma(q[d],s.mean[d],dot);++p.fma_ops;p.scratch_read_bytes+=2;}
    float score=half(half(dot)*scale);p.round_ops+=2;++p.mul_ops;
    if(s.eligible){scores[g]=score;if(!std::isfinite(score))p.prediction_nonfinite_fallback=true;++p.compare_ops;}
  }
  
  
  float m=-std::numeric_limits<float>::infinity();
  for(float x:scores){m=std::max(m,x);++p.compare_ops;}
  float z=0;
  if(std::isfinite(m)&&!p.prediction_nonfinite_fallback){
    for(int g=0;g<G;++g){float e=std::exp(scores[g]-m);++p.exp_ops;++p.add_ops;probability[g]=e;z+=e;++p.add_ops;}
    for(float&v:probability){v=half(v/z);++p.div_ops;++p.round_ops;}
  } else {
    
    
    p.exp_ops+=G;p.add_ops+=2ull*G;p.div_ops+=G;p.round_ops+=G;
  }
  
  
  for(int i=1;i<G;++i){
    int x=idx[i],j=i;p.scratch_read_bytes+=4;
    while(j>0){++p.compare_ops;p.scratch_read_bytes+=12;
      if(!(probability[x]>probability[idx[j-1]]))break;
      idx[j]=idx[j-1];--j;p.scratch_write_bytes+=4;
    }
    idx[j]=x;p.scratch_write_bytes+=4;
  }
  int count=0;
  if(in.config.use_cdf){
    float cdf=0;
    for(int rank=0;rank<G;++rank){cdf=half(cdf+probability[idx[rank]]);++p.add_ops;++p.round_ops;++p.compare_ops;if(cdf<=in.config.cdf)++count;}
  }else{count=int(in.config.topk*G);++p.mul_ops;++p.round_ops;}
  count=std::clamp(std::max(count,1),1,G);
  std::vector<uint8_t> keep_blocks(G,0);
  for(int rank=0;rank<count;++rank)keep_blocks[idx[rank]]=1;
  for(int g=0;g<G;++g){
    if(p.prediction_nonfinite_fallback||!query_eligible||!state.blocks[g].eligible)keep_blocks[g]=1;
    if(keep_blocks[g]){++p.selected_blocks;for(int t=g*B;t<std::min(in.L,(g+1)*B);++t)p.keep[t]=1;}
  }
  p.pooled_scores=scores;p.pooled_probability=probability;
  p.scratch_read_bytes+=8ull*G; 
  p.scratch_write_bytes+=17ull*G+in.L;
  p.scratch_peak_bytes=std::max<uint64_t>(p.scratch_peak_bytes,10ull*in.D+17ull*G+in.L);
  return p;
}

inline Plan stage2(const std::vector<float>& scaled_scores,const std::vector<uint8_t>& qk_keep,const Config& cfg=Config{}) {
  if(scaled_scores.size()!=qk_keep.size()||cfg.block_tokens!=64)throw std::runtime_error("Invalid Sparge stage2 input");
  const int L=int(qk_keep.size()),G=(L+63)/64;Plan p;p.keep.assign(L,0);p.total_blocks=G;
  std::vector<int> selected;
  for(int g=0;g<G;++g){bool any=false;for(int t=g*64;t<std::min(L,(g+1)*64);++t)any=any||qk_keep[t];if(any)selected.push_back(g);}
  float running=-std::numeric_limits<float>::infinity();
  for(size_t rank=0;rank<selected.size();++rank){
    int g=selected[rank];float local=-std::numeric_limits<float>::infinity();
    for(int t=g*64;t<std::min(L,(g+1)*64);++t)if(qk_keep[t]){local=std::max(local,scaled_scores[t]);++p.compare_ops;p.scratch_read_bytes+=4;}
    float delta=(local-running)*1.4426950408889634f;++p.add_ops;++p.mul_ops;
    running=std::max(running,local);++p.compare_ops;
    
    
    bool keep=rank+2>=selected.size()||(delta+cfg.pv_threshold>0);++p.add_ops;++p.compare_ops;
    if(keep){++p.selected_blocks;for(int t=g*64;t<std::min(L,(g+1)*64);++t)p.keep[t]=qk_keep[t];}
    else ++p.pv_skipped_blocks;
  }
  p.scratch_write_bytes=L;p.scratch_peak_bytes=L+4ull*G+16;
  return p;
}
} 
