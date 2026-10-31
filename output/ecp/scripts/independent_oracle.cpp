#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <vector>
#include <stdexcept>
float hf(uint16_t x){return float(std::bit_cast<_Float16>(x));}uint16_t hb(float x){return std::bit_cast<uint16_t>(_Float16(x));}
template<class T>void rd(std::ifstream&f,std::vector<T>&v,size_t n){v.resize(n);f.read((char*)v.data(),n*sizeof(T));if(!f)throw std::runtime_error("truncated");}
template<class T>void wr(std::filesystem::path p,const std::vector<T>&v){std::ofstream f(p,std::ios::binary);f.write((const char*)v.data(),v.size()*sizeof(T));}
int main(int argc,char**argv){try{
 if(argc!=7&&argc!=8)throw std::runtime_error("input rawkeys strategy B packed outdir");
 std::ifstream f(argv[1],std::ios::binary),raw(argv[2],std::ios::binary);char magic[8];std::array<uint32_t,4> dims;f.read(magic,8);f.read((char*)dims.data(),16);auto[N,H,D,Q]=dims;bool global=std::string(argv[3])=="global";int sample=global?std::stoi(argv[4]):0;int B=global?N:std::stoi(argv[4]);int channels=argc==8?std::stoi(argv[7]):1;bool packed=std::stoi(argv[5])&&D==32,hybrid=std::string(argv[3])=="hybrid";
 std::vector<uint32_t>L;std::vector<uint16_t>K,V,q;std::vector<float>rawk;rd(f,L,Q);rd(f,K,N*D);rd(f,V,N*D);rd(f,q,Q*H*D);rd(raw,rawk,N*D);
 int nb=(N+B-1)/B;std::vector<float>A(nb*D),def(D);std::vector<uint8_t>flags(nb);std::vector<bool>ready(nb);std::vector<float>scores(Q*H*N),dense_output(Q*H*D);std::vector<uint16_t>all_prob(Q*H*N);std::vector<uint16_t>encoded(N*D);
 for(unsigned qi=0;qi<Q;qi++){
  unsigned begin=qi?L[qi-1]:0;
  while(begin<L[qi]){
   unsigned b=begin/B,end=std::min<unsigned>(L[qi],(b+1)*B);std::vector<float>a(D);
   if(!ready[b]){
    unsigned anchor_end=(global&&sample>0)?std::min<unsigned>(end,begin+sample):end;for(unsigned d=0;d<D;d++){a[d]=rawk[begin*D+d];for(unsigned t=begin+1;t<anchor_end;t++)a[d]+=rawk[t*D+d];a[d]/=float(anchor_end-begin);}
    if(b==0)def=a;
    float e0=0,eb=0;
    if(hybrid&&b){for(unsigned t=begin;t<end;t++)for(unsigned d=0;d<D;d++){float r0=rawk[t*D+d]-def[d],rb=rawk[t*D+d]-a[d];e0=std::max(e0,std::abs(hf(hb(r0))-r0));eb=std::max(eb,std::abs(hf(hb(rb))-rb));}}
    flags[b]=!hybrid||b==0||e0>2*eb;
    if(flags[b])for(unsigned d=0;d<D;d++)A[b*D+d]=a[d];
    ready[b]=true;
   }
   for(unsigned t=begin;t<end;t++)for(unsigned d=0;d<D;d++)encoded[t*D+d]=hb(rawk[t*D+d]-(flags[b]?A[b*D+d]:def[d]));
   begin=end;
  }
  for(unsigned h=0;h<H;h++){
   std::vector<float>cor(nb);
   for(int b=1;b<nb;b++)if(ready[b]&&flags[b])for(unsigned d=0;d<D;d++)cor[b]=std::fma(hf(q[(qi*H+h)*D+d]),A[b*D+d]-A[d],cor[b]);
   for(unsigned t=0;t<L[qi];t++){
    std::array<float,4> acc{};
    if(packed){for(unsigned b=0;b<2;b++)for(unsigned j=0;j<16;j++){unsigned d=b*16+j;acc[b]=std::fma(hf(encoded[t*D+d]),hf(q[(qi*H+h)*D+d]),acc[b]);}}
    else for(unsigned tile=0;tile<(D+63)/64;tile++)for(unsigned b=0;b<4;b++)for(unsigned j=0;j<16;j++){unsigned d=tile*64+b*16+j;acc[b]=std::fma(d<D?hf(encoded[t*D+d]):0.f,d<D?hf(q[(qi*H+h)*D+d]):0.f,acc[b]);}
    float sc=0;for(float x:acc)sc+=x;scores[(qi*H+h)*N+t]=global?sc:sc+cor[t/B];
   }
   
   float scale=1.f/std::sqrt(float(D)),mx=-INFINITY,z=0;unsigned groups=(L[qi]+15)/16;std::vector<float>ex(L[qi]);
   for(unsigned t=0;t<L[qi];t++)mx=std::max(mx,scores[(qi*H+h)*N+t]*scale);
   for(unsigned g=0;g<groups;g++){
    std::array<float,16>a{};for(unsigned j=0;j<16;j++){unsigned t=g*16+j;if(t<L[qi])a[j]=ex[t]=std::exp(scores[(qi*H+h)*N+t]*scale-mx);}
    for(unsigned stride=1;stride<16;stride*=2)for(unsigned j=0;j<16;j+=stride*2)a[j]+=a[j+stride];z+=a[0];
   }
   for(unsigned t=0;t<L[qi];t++)all_prob[(qi*H+h)*N+t]=hb(ex[t]/z);
   for(unsigned tile=0;tile<(packed?1:(D+63)/64);tile++){
    std::vector<std::vector<float>>pu(channels,std::vector<float>(1024)),rank(channels,std::vector<float>(128));
    unsigned ng=packed?(groups+1)/2:groups;
    for(unsigned g=0;g<ng;g++){
     unsigned ch=(packed?g:g/2)%channels;
     for(unsigned stripe=0;stripe<16;stripe++)for(unsigned d=0;d<64;d++){
      unsigned t=(packed?2*g+d/32:g)*16+stripe,dim=packed?d%32:tile*64+d;
      float prob=t<L[qi]?hf(all_prob[(qi*H+h)*N+t]):0.f,value=t<L[qi]&&dim<D?hf(V[t*D+dim]):0.f;
      pu[ch][stripe*64+d]=std::fma(prob,value,pu[ch][stripe*64+d]);
     }
    }
    for(int ch=0;ch<channels;ch++)for(unsigned r=0;r<2;r++)for(unsigned d=0;d<64;d++){
     float v=0;for(unsigned pc=0;pc<2;pc++)for(unsigned bg=0;bg<4;bg++)v+=pu[ch][(pc*8+r*4+bg)*64+d];rank[ch][r*64+d]=v;
    }
    std::vector<float>sum=rank[0];if(channels>1){std::fill(sum.begin(),sum.end(),0);for(int ch=0;ch<channels;ch++)for(unsigned d=0;d<128;d++)sum[d]+=rank[ch][d];}
    for(unsigned j=0;j<(packed?32:64);j++)if(tile*64+j<D){float x=packed?(sum[j]+sum[64+j])+(sum[32+j]+sum[96+j]):sum[j]+sum[64+j];dense_output[(qi*H+h)*D+tile*64+j]=x==0?0:x;}
   }
  }
 }
 std::filesystem::path out=argv[6];std::filesystem::create_directories(out);wr(out/"encoded_keys.f16",encoded);wr(out/"block_anchors.f32",A);wr(out/"block_flags.u8",flags);wr(out/"scores.f32",scores);wr(out/"dense_output.f32",dense_output);wr(out/"probability.f16",all_prob);
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
