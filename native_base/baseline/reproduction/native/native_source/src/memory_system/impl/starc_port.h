#pragma once




#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace StarcPort {
struct Input {
  int L=0,D=0,q=0,h=0,H=1,layer=2,budget=-1;
  std::function<float(int,int)> getK,getV;
  std::function<float(int)> getQ;
};
struct Plan {
  std::vector<int> order;
  std::vector<uint8_t> keep;
  uint64_t fma_ops=0,add_ops=0,mul_ops=0,div_ops=0,sqrt_ops=0,exp_ops=0,compare_ops=0,round_ops=0;
  uint64_t scratch_read_bytes=0,scratch_write_bytes=0,metadata_read_bytes=0,metadata_write_bytes=0;
  uint64_t scratch_peak_bytes=0,metadata_live_bytes=0;
  int key_read_begin=0,key_read_end=0;
  bool remap=false;
  int clustered_tokens=0,clusters=0,selected_tokens=0,tail_tokens=0,iterations=0;
  bool layer_bypass=false;
};
struct State {
  int D=0,clustered=0,lastL=0,lastq=-1;
  std::vector<float> centers; 
  std::vector<int> labels,order; 
};
inline float dot(const float* a,const float* b,int D,Plan& p) {
  float s=0;
  for(int d=0;d<D;++d)s=std::fma(a[d],b[d],s);
  p.fma_ops+=D;p.scratch_read_bytes+=(a==b?4ull:8ull)*D;
  return s;
}
inline void cluster_window(const Input& in,State& st,Plan& p,int begin,int end) {
  const int W=end-begin,D=in.D,C=(W+15)/16,offset=int(st.centers.size())/D;
  std::vector<float> keys(W*D),centers(C*D),next(C*D),xn(W),cn(C);
  std::vector<int> lab(W),counts(C),local(W);
  for(int t=0;t<W;++t) {
    for(int d=0;d<D;++d)keys[t*D+d]=in.getK(begin+t,d);
    p.scratch_write_bytes+=4ull*D;
    float n=std::sqrt(dot(keys.data()+t*D,keys.data()+t*D,D,p));++p.sqrt_ops;
    ++p.compare_ops;n=std::max(n,1e-12f);
    for(int d=0;d<D;++d)keys[t*D+d]/=n;
    p.div_ops+=D;p.scratch_read_bytes+=4ull*D;p.scratch_write_bytes+=4ull*D;
  }
  std::copy_n(keys.begin(),C*D,centers.begin());
  p.scratch_read_bytes+=4ull*C*D;p.scratch_write_bytes+=4ull*C*D;
  for(int it=0;it<16;++it) {
    ++p.iterations;
    for(int t=0;t<W;++t)xn[t]=dot(keys.data()+t*D,keys.data()+t*D,D,p);
    for(int c=0;c<C;++c)cn[c]=dot(centers.data()+c*D,centers.data()+c*D,D,p);
    p.scratch_write_bytes+=4ull*(W+C);
    for(int t=0;t<W;++t) {
      int arg=0;float best=INFINITY;
      for(int c=0;c<C;++c) {
        float inner=dot(keys.data()+t*D,centers.data()+c*D,D,p);
        volatile float twice=2.0f*inner;
        float dist=(xn[t]+cn[c])-twice;
        p.add_ops+=2;++p.mul_ops;p.scratch_read_bytes+=8;
        ++p.compare_ops;if(dist<best){best=dist;arg=c;}
      }
      lab[t]=arg;p.scratch_write_bytes+=4;
    }
    std::fill(next.begin(),next.end(),0.f);std::fill(counts.begin(),counts.end(),0);
    p.scratch_write_bytes+=4ull*(C*D+C);
    for(int t=0;t<W;++t) {
      int c=lab[t];++counts[c];++p.add_ops;
      p.scratch_read_bytes+=8;p.scratch_write_bytes+=4;
      for(int d=0;d<D;++d)next[c*D+d]+=keys[t*D+d];
      p.add_ops+=D;p.scratch_read_bytes+=8ull*D;p.scratch_write_bytes+=4ull*D;
    }
    for(int c=0;c<C;++c) {
      ++p.compare_ops;int count=std::max(counts[c],1);
      for(int d=0;d<D;++d)next[c*D+d]/=float(count);
      p.div_ops+=D;p.scratch_read_bytes+=4ull*D;p.scratch_write_bytes+=4ull*D;
    }
    bool close=true;
    for(int i=0;i<C*D;++i) {
      float delta=std::fabs(centers[i]-next[i]);
      volatile float relative=1e-5f*std::fabs(next[i]);
      float tolerance=1e-4f+relative;
      close=(delta<=tolerance)&&close;
    }
    p.add_ops+=2ull*C*D;p.mul_ops+=C*D;p.compare_ops+=C*D;p.scratch_read_bytes+=8ull*C*D;
    centers.swap(next);
    if(close)break;
  }
  
  std::vector<int> prefix(C+1,0);
  for(int t=0;t<W;++t){++prefix[lab[t]+1];++p.add_ops;}
  for(int c=0;c<C;++c){prefix[c+1]+=prefix[c];++p.add_ops;}
  for(int t=0;t<W;++t){local[prefix[lab[t]]++]=t;++p.add_ops;}
  p.scratch_read_bytes+=12ull*W+8ull*C;p.scratch_write_bytes+=12ull*W+4ull*C;
  if(int(st.labels.size())<end)st.labels.resize(end,-1);
  for(int t=0;t<W;++t)st.labels[begin+t]=offset+lab[t];
  for(int t:local)st.order.push_back(begin+t);
  st.centers.insert(st.centers.end(),centers.begin(),centers.end());
  p.metadata_write_bytes+=8ull*W+4ull*C*D;
  p.scratch_peak_bytes=std::max<uint64_t>(p.scratch_peak_bytes,
    4ull*(W*D+2*C*D+3*W+3*C+1));
}
inline std::vector<int> rank_centroids(const std::vector<float>& scores,Plan& p) {
  const int C=int(scores.size());std::vector<int> a(C),b(C);std::iota(a.begin(),a.end(),0);
  p.scratch_write_bytes+=4ull*C;
  for(int w=1;w<C;w*=2) {
    for(int start=0;start<C;start+=2*w) {
      int i=start,j=std::min(start+w,C),mid=j,stop=std::min(start+2*w,C),k=start;
      while(i<mid&&j<stop) {
        ++p.compare_ops;p.scratch_read_bytes+=16;
        if(scores[a[i]]>=scores[a[j]])b[k++]=a[i++];else b[k++]=a[j++];
        p.scratch_write_bytes+=4;
      }
      while(i<mid){b[k++]=a[i++];p.scratch_read_bytes+=4;p.scratch_write_bytes+=4;}
      while(j<stop){b[k++]=a[j++];p.scratch_read_bytes+=4;p.scratch_write_bytes+=4;}
    }
    a.swap(b);
  }
  return a;
}
inline Plan step(const Input& in,State& st) {
  if(in.L<1||in.D<1||!in.getK||!in.getQ)throw std::runtime_error("Invalid STARC input");
  Plan p;p.keep.assign(in.L,1);p.order.resize(in.L);std::iota(p.order.begin(),p.order.end(),0);
  if(in.layer<2){p.layer_bypass=true;p.selected_tokens=in.L;return p;}
  const int B=std::min(in.L,std::max(1,in.budget<0?(in.L+1)/2:in.budget));
  if(st.lastq<0) {
    if(in.h!=0)throw std::runtime_error("STARC first metadata build must precede all query heads");
    st.D=in.D;p.key_read_begin=0;p.key_read_end=in.L;
    for(int begin=0;begin<in.L;begin+=64)cluster_window(in,st,p,begin,std::min(begin+64,in.L));
    st.clustered=in.L;st.lastq=in.q;st.lastL=in.L;
  } else {
    if(st.D!=in.D||in.L<st.lastL||in.q<st.lastq)throw std::runtime_error("Stale STARC owner/length");
    if(in.q!=st.lastq) {
      if(in.h!=0||in.q!=st.lastq+1||in.L!=st.lastL+1)throw std::runtime_error("STARC requires causal single-token decode");
      
      p.metadata_write_bytes+=8ull*(in.L-st.lastL);
      if(in.L-st.clustered>=64) {
        p.key_read_begin=st.clustered;p.key_read_end=in.L;
        cluster_window(in,st,p,st.clustered,in.L);st.clustered=in.L;
      }
      st.lastq=in.q;st.lastL=in.L;
    }
  }
  p.order=st.order;
  for(int t=st.clustered;t<in.L;++t)p.order.push_back(t);
  p.remap=p.key_read_end>p.key_read_begin;
  p.clustered_tokens=st.clustered;p.tail_tokens=in.L-st.clustered;
  const int C=int(st.centers.size())/in.D;p.clusters=C;
  p.metadata_live_bytes=4ull*C*in.D+8ull*in.L+64;
  p.metadata_read_bytes+=4ull*C*in.D+8ull*in.L;
  std::vector<float> query(in.D),scores(C);std::vector<int> counts(C,0);
  for(int d=0;d<in.D;++d)query[d]=in.getQ(d);
  p.scratch_write_bytes+=4ull*in.D;
  float scale=1.f/std::sqrt(float(in.D));++p.sqrt_ops;++p.div_ops;
  for(int c=0;c<C;++c){scores[c]=dot(query.data(),st.centers.data()+c*in.D,in.D,p)*scale;++p.mul_ops;}
  p.scratch_write_bytes+=4ull*C;
  auto ranked=rank_centroids(scores,p);
  for(int t=0;t<st.clustered;++t){++counts[st.labels[t]];++p.add_ops;p.scratch_read_bytes+=8;p.scratch_write_bytes+=4;}
  p.keep.assign(in.L,0);
  for(int t=st.clustered;t<in.L;++t)p.keep[t]=1;
  int budget=B-p.tail_tokens,cumulative=0,boundary=-1,leftover=0;
  std::vector<uint8_t> chosen(C,0);
  for(int c:ranked) {
    int previous=cumulative;cumulative+=counts[c];++p.add_ops;
    ++p.compare_ops;if(cumulative<=budget)chosen[c]=1;
    p.compare_ops+=2;if(cumulative>budget&&previous<budget){boundary=c;leftover=budget-previous;++p.add_ops;}
    p.scratch_read_bytes+=8;p.scratch_write_bytes+=1;
  }
  int rank_in_boundary=0;
  for(int token:p.order) {
    if(token>=st.clustered)continue;
    int c=st.labels[token];bool keep=chosen[c];
    ++p.compare_ops;if(c==boundary){++rank_in_boundary;++p.add_ops;++p.compare_ops;if(rank_in_boundary<=leftover)keep=true;}
    p.keep[token]=keep;p.scratch_read_bytes+=9;p.scratch_write_bytes+=1;
  }
  for(auto bit:p.keep)p.selected_tokens+=bool(bit);
  p.scratch_peak_bytes=std::max<uint64_t>(p.scratch_peak_bytes,4ull*in.D+17ull*C+in.L);
  return p;
}
} 
