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
 if(argc!=7)throw std::runtime_error("input rawkeys strategy B packed outdir");
 std::ifstream f(argv[1],std::ios::binary),raw(argv[2],std::ios::binary);char magic[8];std::array<uint32_t,4> dims;f.read(magic,8);f.read((char*)dims.data(),16);auto[N,H,D,Q]=dims;int B=std::stoi(argv[4]);bool packed=std::stoi(argv[5])&&D==32,hybrid=std::string(argv[3])=="hybrid";
 std::vector<uint32_t>L;std::vector<uint16_t>K,V,q;std::vector<float>rawk;rd(f,L,Q);rd(f,K,N*D);rd(f,V,N*D);rd(f,q,Q*H*D);rd(raw,rawk,N*D);
 int nb=(N+B-1)/B;std::vector<float>A(nb*D),def(D);std::vector<uint8_t>flags(nb);std::vector<bool>ready(nb);std::vector<float>scores(Q*H*N);std::vector<uint16_t>encoded(N*D);
 for(unsigned qi=0;qi<Q;qi++){
  unsigned begin=qi?L[qi-1]:0;
  while(begin<L[qi]){
   unsigned b=begin/B,end=std::min<unsigned>(L[qi],(b+1)*B);std::vector<float>a(D);
   if(!ready[b]){
    for(unsigned d=0;d<D;d++){a[d]=rawk[begin*D+d];for(unsigned t=begin+1;t<end;t++)a[d]+=rawk[t*D+d];a[d]/=float(end-begin);}
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
    float s=0;for(float x:acc)s+=x;scores[(qi*H+h)*N+t]=s+cor[t/B];
   }
  }
 }
 std::filesystem::path out=argv[6];std::filesystem::create_directories(out);wr(out/"encoded_keys.f16",encoded);wr(out/"block_anchors.f32",A);wr(out/"block_flags.u8",flags);wr(out/"scores.f32",scores);
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
