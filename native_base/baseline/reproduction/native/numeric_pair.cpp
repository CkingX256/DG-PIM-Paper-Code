#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <iomanip>
#include <vector>
#include "memory_system/memory_system.h"
#include "memory_system/numeric_bridge.h"
#include "dram_controller/controller.h"
#include "addr_mapper/addr_mapper.h"
#include "dram/dram.h"
#include "starc_port.h"
#include "sparge_filter.hpp"
#include "sage_port.hpp"

namespace Ramulator {
namespace {
float hfloat(uint16_t x) {return float(std::bit_cast<_Float16>(x));}
uint16_t hbits(float x) {return std::bit_cast<uint16_t>(_Float16(x));}
uint32_t punits(uint16_t x) {
  if((x&0x8000) || (x&0x7c00)==0x7c00) throw std::runtime_error("Invalid probability bits");
  unsigned e=(x>>10)&31, m=x&1023;
  if(e>15) throw std::runtime_error("Probability exceeds one");
  return e ? ((1024u+m)<<(e-1)) : m;
}
double up(double x) {return std::nextafter(x,std::numeric_limits<double>::infinity());}
double down(double x) {return std::nextafter(x,-std::numeric_limits<double>::infinity());}
template<class T> void readvec(std::ifstream& f,std::vector<T>& v,size_t n) {
  v.resize(n);f.read(reinterpret_cast<char*>(v.data()),n*sizeof(T));
  if(!f) throw std::runtime_error("Truncated numerical fixture");
}
template<class T> void writevec(const std::filesystem::path& p,const std::vector<T>& v) {
  std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(v.data()),v.size()*sizeof(T));
  f.flush();if(!f) throw std::runtime_error("Cannot write numerical output");
}
}



class NumericPIMDRAM8 : public IMemorySystem, public Implementation, public INumericSystem {
  RAMULATOR_REGISTER_IMPLEMENTATION(IMemorySystem, NumericPIMDRAM8, "NumericPIMDRAM8", "Native HBM-PIM with completion-bound numerical actors")
  bool token_requested=false,token_packed=false,token_use_route=false;
  std::string token_disabled_reason;
  uint64_t token_qmac=0,token_vmac=0,migration_reads=0,migration_writes=0,migration_epu_cycles=0;
  bool migrating=false;
  std::vector<uint16_t> migration_k,migration_v;
  std::vector<bool> migration_k_valid,migration_v_valid;
  int hardware_groups()const{return token_packed?(groups+1)/2:groups;}
  int frame_index(int g)const{return token_packed?2*(g/32)+g%2:g/16;}
  int frame_count()const{return token_packed?2*((groups+31)/32):(groups+15)/16;}
  bool pair_requested=false,pair_enabled=false,lean_scalar=true;
  int pair_count=1,pair_dispatch=4,route_bpc=256,route_latency=4,route_credits=4;
  uint64_t route_peak=0,pair_batches=0,paired_qmac=0,paired_vmac=0;
  Clk_t route_deadline=0;std::string requested_mode,pair_disabled_reason;
  std::vector<float> scores_b;std::vector<uint16_t> prob_b,pgb_b;
  std::vector<bool> skip_b,gbvalid_b,frames_b;
  std::array<int,2> gb_heads{-1,-1};
  std::ofstream route_events;
  struct Route {int id,op,x,y,q,h;Clk_t ready,start,end;uint64_t epoch;};
  std::deque<Route> routes;
  enum Op {NormLoad,Publish,HeaderLoad,AppendK,AppendV,QWrite,QMac,QMove,SfmKick,PMove,VMac,VMove,StatMove,MigrateKLoad,MigrateVLoad,PortKeyRead,PortMetaRead,PortMetaWrite,PortQuantWrite,PortRemapKRead,PortRemapVRead,PortRemapKWrite,PortRemapVWrite};
  struct Task {int id,op,x=0,y=0,z=0,q=-1,h=-1; Addr_t addr=0; Clk_t accepted=0;};
  IDRAM* dram=nullptr; IAddrMapper* mapper=nullptr;
  std::vector<IDRAMController*> controllers;
  std::deque<Task> tasks;std::map<int,Task> outstanding;
  int next_id=1;uint64_t callbacks=0,submitted=0,max_inflight=0;
  Clk_t clk=0,start_clk=0,epu_until=0,phase_start=0,step_start=0,init_cycles=0;
  std::function<void()> epu_commit;
  std::string phase="start",mode,fault,cache_init;
  std::filesystem::path directory;
  std::ofstream events,norm_events,observer_events;
  struct Obs {Task task;Clk_t ready,start,end;uint64_t epoch;};
  std::deque<Obs> observer;
  std::array<uint16_t,64> pu_max{},gathered_max{};
  std::array<bool,64> gathered_valid{};
  std::vector<bool> observed_coverage;
  uint16_t amax_bits=0;
  Clk_t observer_deadline=0;
  uint64_t observer_mac_jobs=0,observer_append_jobs=0,observer_peak=0,bootstrap_heads=0,probability_moves=0;
  int observer_latency=4,observer_interval=1,fused_tail=4,budget_setup=24,zero_tail=1,scratch_bpc=32;
  bool legacy_epu_timing=false,resident_certificate=false,evict_each_step=false;
  bool stream_max_fusion=false,inplace_exp=false;
  int max_group_cycles=1,max_batch_merge_cycles=4;
  float ingress_max=-std::numeric_limits<float>::infinity(),ingress_scale=0;
  uint32_t ingress_index=UINT32_MAX,ingress_seen=0;
  
  
  std::vector<bool> audit_score_coverage;
  uint64_t exact_exp_calls=0,softmax_score_read_bytes=0;
  uint64_t exp_scratch_read_bytes=0,exp_scratch_write_bytes=0;
  uint64_t streamed_max_tokens=0,streamed_max_batches=0,streamed_max_cycles=0;
  YAML::Node exact_softmax_heads;
  float counted_exp(float x){++exact_exp_calls;return std::exp(x);}
  void observe_score(int token,float value){
    if(!stream_max_fusion)return;
    if(token<0||token>=L||audit_score_coverage[token])throw std::runtime_error("Nonunique streamed score coverage");
    audit_score_coverage[token]=true;float scaled=value*ingress_scale;
    if(!std::isfinite(scaled))throw std::runtime_error("Nonfinite streamed score");
    if(scaled>ingress_max||(scaled==ingress_max&&uint32_t(token)<ingress_index)){
      ingress_max=scaled;ingress_index=uint32_t(token);
    }
    ++ingress_seen;++streamed_max_tokens;
  }
  uint64_t streamed_batch_work(){
    int hw=std::min(16,hardware_groups()-batch),first=(token_packed?2:1)*batch;
    int logical=std::min(groups-first,(token_packed?2:1)*hw);
    if(external()){
      logical=0;for(int g=first;g<std::min(groups,first+(token_packed?2:1)*hw);g++){
        bool any=false;for(int t=g*16;t<std::min(L,(g+1)*16);t++)any|=port_keep_slot[t];logical+=any;
      }
    }
    uint64_t work=uint64_t(logical)*max_group_cycles+max_batch_merge_cycles;
    streamed_max_cycles+=work;++streamed_max_batches;return work;
  }
  void finish_q_move_batch(){
    batch+=16;if(batch<hardware_groups()){std::fill(qpu.begin(),qpu.end(),0);q_macs();}
    else {setphase("sfm_kick");enqueue(SfmKick,0);}
  }
  bool bank_envelope=true,envelope_dirty=true,budget_dirty=true;std::array<uint16_t,4> bank_max{};uint64_t budget_reuses=0;
  uint64_t resident_reads=0,resident_publications=0,resident_evictions=0;
  std::ofstream resident_events;
  bool bootstrap_head=false,lazy_bound_ready=false;
  std::string head_choice;
  uint64_t prefix_allowance=0,fused_skip_mass=0,budget_setups=0,budget_epoch=0;
  uint32_t fused_skipped=0,fused_zero=0;double prefix_gamma=0;uint64_t uniform_cut=0;int quota_cycles=3;

  YAML::Node result,head_records,step_records,phase_records;
  uint32_t nmax=0,H=0,D=0,Q=0;
  std::vector<uint32_t> lengths;
  std::vector<uint16_t> K,V,queries,prob,pgb,qgb;
  std::vector<float> scores,qpu,svpu,rankout,outputs,all_scores;
  std::vector<uint16_t> all_prob;
  std::vector<uint8_t> all_skip;std::vector<bool> skip,gbvalid,transport_frames;
  std::vector<uint32_t> masses;
  std::array<float,64> normsum{};
  int q=0,h=0,L=0,committed=0,groups=0,dtiles=0,phase_q=-1,phase_h=-1;
  int build_cursor=0,build_end=0,batch=0,dtile=0;
  bool finished=false,header_ok=false,header_valid=false,duplicate_injected=false,initializing=true;
  uint64_t norm_reserved=0,norm_peak=0,norm_busy=0,sqrt_count=0;
  Clk_t norm_deadline=0;std::deque<Clk_t> norm_finishes;
  float max_norm_sum=0;
  uint64_t owner=0x444750494d36ull,generation=1,stored_generation=0,stored_owner=0;
  int stored_length=0;
  double radius=0,stored_radius=0,loaded_radius=0,epsilon=0,gamma=0,bound=0;
  uint64_t total_mass=0,skip_mass=0,allowance=0;
  int skipped=0;uint64_t comparisons=0;
  int mac_latency=6,move_latency=4,norm_lanes=4,sort_width=1;
  uint64_t scratch_limit=262144,probability_limit=32768,watchdog=20000000;
  int fault_step=0;
  uint64_t count_qmac=0,count_vmac=0,count_normld=0,count_headerld=0,count_kvwr=0,count_metawr=0,count_initwr=0;
  static constexpr Addr_t BANK=1ll<<24,BG=1ll<<26,RANK=1ll<<28,PCH=1ll<<29;
  static constexpr Addr_t VBASE=1ll<<23,HDR=3ll<<22;
  Addr_t stripe_addr(int stripe)const {return (stripe/8)*PCH+((stripe/4)%2)*RANK+(stripe%4)*BG;}
  Addr_t base_vaddr(int token,int chunk)const {
    if(token_packed)return VBASE+stripe_addr(token%16)+(chunk+2*((token/16)%2))*BANK+32ll*(token/32);
    return VBASE+stripe_addr(token%16)+(chunk%4)*BANK+32ll*((chunk/4)*((nmax+15)/16)+token/16);
  }
  Addr_t original_kaddr(int token,int chunk)const {
    return stripe_addr(token%16)+(chunk%4)*BANK+32ll*((token/16)*dtiles+chunk/4);
  }
  Addr_t original_vaddr(int token,int chunk)const {
    return VBASE+stripe_addr(token%16)+(chunk%4)*BANK+32ll*((chunk/4)*((nmax+15)/16)+token/16);
  }
  Addr_t base_kaddr(int token,int chunk)const {
    if(token_packed)return stripe_addr(token%16)+(chunk+2*((token/16)%2))*BANK+32ll*(token/32);
    return stripe_addr(token%16)+(chunk%4)*BANK+32ll*((token/16)*dtiles+chunk/4);
  }
  Addr_t kaddr(int token,int chunk)const {
    Addr_t a=base_kaddr(token,chunk);
    if(external()&&token/16<int(port_shadow_k.size())&&port_shadow_k[token/16])a+=PORT_SHADOW;
    if(external_policy=="sage"&&port_key_quantized)a+=PORT_SHADOW;
    return a;
  }
  Addr_t vaddr(int token,int chunk)const {
    Addr_t a=base_vaddr(token,chunk);
    if(external()&&token/16<int(port_shadow_v.size())&&port_shadow_v[token/16])a+=PORT_SHADOW;
    return a;
  }
  Addr_t append_kaddr(int token,int chunk)const {
    return external_policy=="sage"?base_kaddr(token,chunk):kaddr(token,chunk);
  }
  float kval(int token,int dim)const {
    if(token<L&&token>=committed)throw std::runtime_error("Read of uncommitted Key");
    if(token>=L||dim>=int(D))return 0.f;
    if(external_policy=="sage"){
      if(!port_key_quantized||sage_state.query!=q)throw std::runtime_error("QK consumed an unpublished quantized Key image");
      if(!port_quant_written[token*(D/16)+dim/16])throw std::runtime_error("QK used unpublished quantized Key chunk");
      return hfloat(port_dram_word(false,1,token,dim));
    }
    if(external())return hfloat(port_dram_word(false,port_shadow_k[token/16],token,dim));
    return hfloat(K[token*D+dim]);
  }
  float vval(int token,int dim)const {
    if(token<L&&token>=committed)throw std::runtime_error("Read of uncommitted Value");
    if(token>=L||dim>=int(D))return 0.f;
    return hfloat(external()?port_dram_word(true,port_shadow_v[token/16],token,dim):V[token*D+dim]);
  }
  bool paired()const{return pair_enabled&&pair_count==2;}
  uint64_t route_bytes()const{return token_packed?2048:1024;}
  uint64_t route_interval()const{return (route_bytes()+route_bpc-1)/route_bpc;}
  void pair_qmac(int group) {
    for(int stripe=0;stripe<16;stripe++)for(int b=0;b<4;b++)for(int j=0;j<16;j++){
      int dim=(b%2)*16+j,token=group*16+stripe,ix=(group-batch)*64+stripe*4+b;
      qpu[ix]=std::fma(kval(token,dim),hfloat(qgb[b*16+j]),qpu[ix]);
    }
    ++count_qmac;++paired_qmac;
  }
  void pair_vmac(int group) {
    for(int stripe=0;stripe<16;stripe++){
      int token=group*16+stripe;
      for(int dim=0;dim<64;dim++){
        int hp=dim/32;bool active=token<L&&!(hp?skip_b[group]:skip[group]);float p=0;
        if(active){
          if(gb_heads[hp]!=h+hp||!(hp?gbvalid_b[token]:gbvalid[token]))throw std::runtime_error("Paired probability owner/readiness mismatch");
          p=hfloat(hp?pgb_b[token]:pgb[token]);
        }
        svpu[stripe*64+dim]=std::fma(p,vval(token,dim%32),svpu[stripe*64+dim]);
      }
    }
    ++count_vmac;++paired_vmac;
  }
  void token_q_mac(int group){
    for(int stripe=0;stripe<16;stripe++)for(int b=0;b<4;b++)for(int j=0;j<16;j++){
      int dim=(b%2)*16+j,token=(2*group+b/2)*16+stripe,ix=(group-batch)*64+stripe*4+b;
      qpu[ix]=std::fma(kval(token,dim),hfloat(qgb[b*16+j]),qpu[ix]);
    }
    ++count_qmac;++token_qmac;
  }
  void token_v_mac(int group){
    for(int stripe=0;stripe<16;stripe++)for(int dim=0;dim<64;dim++){
      int gg=2*group+dim/32,token=gg*16+stripe;float p=0;
      if(token<L&&!skip[gg]){
        if(!gbvalid[token])throw std::runtime_error("Token-packed probability readiness violation");
        p=hfloat(pgb[token]);
      }
      svpu[stripe*64+dim]=std::fma(p,vval(token,dim%32),svpu[stripe*64+dim]);
    }
    ++count_vmac;++token_vmac;
  }
  void commit_routes(){
    while(!routes.empty()&&routes.front().end<=clk){
      auto x=routes.front();routes.pop_front();
      if(x.q!=q||x.h!=h||x.epoch!=generation||fault=="route_epoch")throw std::runtime_error("Paired route epoch/owner mismatch");
      route_events<<x.id<<'\t'<<x.ready<<'\t'<<x.start<<'\t'<<x.end<<'\t'<<x.q<<'\t'<<x.h<<'\t'<<x.op<<'\t'<<x.epoch<<'\t'<<route_bytes()<<'\n';
      if(token_packed){
        if(x.op==QMac)token_q_mac(x.x);
        else {token_v_mac(x.x);Task t{x.id,VMac,x.x,x.y,0,x.q,x.h,0,0};if(observer_request(t))schedule_observer(t);}
      }
      else if(x.op==QMac)pair_qmac(x.x);else pair_vmac(x.x);
    }
  }
  void issue_q_writes(){
    setphase("q_write");
    for(int b=0;b<4;b++)for(int tile=0;tile<dtiles;tile++)
      if(token_packed||paired()||!lean_scalar||tile*64+b*16<int(D))enqueue(QWrite,b*BANK+tile,b,tile);
  }
  void pair_softmax(){
    auto one=[this](const std::vector<float>& sc,int head,std::vector<uint16_t>& pp,std::vector<bool>& sk,std::vector<bool>& fr){
      float scale=1.f/std::sqrt(float(D)),mx=-std::numeric_limits<float>::infinity();
      for(int t=0;t<L;t++)mx=std::max(mx,sc[t]*scale);
      std::vector<float> ev(L);float z=0;
      for(int g=0;g<groups;g++){
        std::array<float,16>a{};for(int j=0;j<16;j++){int t=g*16+j;if(t<L)a[j]=ev[t]=std::exp(sc[t]*scale-mx);}
        for(int stride=1;stride<16;stride*=2)for(int j=0;j<16;j+=stride*2)a[j]+=a[j+stride];z+=a[0];
      }
      if(!(z>0)||!std::isfinite(z))throw std::runtime_error("Invalid paired softmax");
      std::fill(pp.begin(),pp.end(),0);std::fill(sk.begin(),sk.end(),false);fr.assign((L+255)/256,false);
      for(int g=0;g<groups;g++){
        uint32_t m=0;for(int j=0;j<16;j++){int t=g*16+j;if(t<L){pp[t]=hbits(ev[t]/z);m+=punits(pp[t]);}}
        sk[g]=(mode=="zero_fused"&&m==0);if(!sk[g])fr[g/16]=true;
      }
      size_t off=(q*H+head)*nmax;std::copy(sc.begin(),sc.end(),all_scores.begin()+off);std::copy(pp.begin(),pp.end(),all_prob.begin()+off);
    };
    one(scores,h,prob,skip,transport_frames);one(scores_b,h+1,prob_b,skip_b,frames_b);
  }
  void pair_finish_plan(){
    for(int hp=0;hp<2;hp++){
      auto& sk=hp?skip_b:skip;size_t off=(q*H+h+hp)*((nmax+15)/16);std::copy(sk.begin(),sk.end(),all_skip.begin()+off);
    }
    setphase("p_move");
    for(int hp=0;hp<2;hp++)for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)for(int c=0;c<(L+255)/256;c++)
      if(mode=="dense"||(hp?frames_b[c]:transport_frames[c]))enqueue(PMove,r*RANK+bg*BG+hp*2*BANK+c,r,bg+4*hp,c);
  }
  bool lazy()const {return mode=="lazy"||mode=="lazy_fused"||mode=="uniform_fused"||mode=="quota_fused";}
  bool fused()const {return mode=="zero_fused"||mode=="greedy_fused"||mode=="lazy_fused"||mode=="uniform_fused"||mode=="quota_fused";}
  bool approximate()const {return mode=="greedy"||mode=="bucket"||mode=="sort"||mode=="greedy_fused"||lazy();}
  bool uses_l2()const {return approximate()&&!lazy();}
  bool gate_transport()const {return fused()||(external()&&!port_bypass());}
  bool resident()const {return resident_certificate&&approximate();}
  void resident_publish() {
    stored_radius=radius;stored_length=committed;stored_generation=generation;stored_owner=owner;header_valid=true;++resident_publications;
    resident_events<<clk<<"\tpublish\t"<<q<<'\t'<<h<<'\t'<<stored_owner<<'\t'<<stored_generation<<'\t'<<stored_length<<'\t'<<amax_bits<<'\t'<<std::setprecision(17)<<stored_radius<<"\t1\n";
  }
  void resident_load() {
    loaded_radius=stored_radius;uint64_t gen=stored_generation,own=stored_owner;int len=stored_length;bool valid=header_valid;
    if(q==fault_step){if(fault=="stale_generation")--gen;if(fault=="wrong_owner")--own;if(fault=="wrong_length")--len;if(fault=="invalid")valid=false;if(fault=="nan_radius")loaded_radius=std::numeric_limits<double>::quiet_NaN();}
    header_ok=valid&&own==owner&&gen==generation&&len==L&&std::isfinite(loaded_radius)&&loaded_radius>=0;
    resident_events<<clk<<"\tread\t"<<q<<'\t'<<h<<'\t'<<own<<'\t'<<gen<<'\t'<<len<<'\t'<<amax_bits<<'\t'<<std::setprecision(17)<<loaded_radius<<'\t'<<header_ok<<'\n';
    if(!header_ok)loaded_radius=0;++resident_reads;
  }
  uint64_t shared_budget_service()const {return budget_epoch&&!budget_dirty?4:budget_setup+(mode=="uniform_fused"&&!std::has_single_bit(unsigned(groups))?8:0);}
  void prepare_shared_budget() {
    if(budget_epoch&&!budget_dirty){budget_epoch=generation;++budget_reuses;return;}
    double u=std::ldexp(1.,-24);int k=(L+15)/16+15;prefix_gamma=up((k*u)/(1-k*u));prefix_allowance=0;
    if(loaded_radius==0)prefix_allowance=(513ull<<15);
    else {
      double reserve=up(up(2*prefix_gamma)*(513./512.));
      double available=down(down(epsilon/loaded_radius)-reserve);
      if(available>0)prefix_allowance=uint64_t(std::floor(std::ldexp(std::min(available,513./512.),24)));
    }
    if(mode=="uniform_fused")uniform_cut=std::has_single_bit(unsigned(groups))?prefix_allowance>>std::countr_zero(unsigned(groups)):prefix_allowance/groups;budget_epoch=generation;budget_dirty=false;++budget_setups;
  }
  uint64_t softmax_service()const {
    uint64_t base=3ull*groups;
    uint64_t masks=(fused()||(external()&&!port_bypass()))?((groups+7)/8+(((groups+15)/16)+7)/8):0;
    if(!legacy_epu_timing) {
      uint64_t rd=(4ull*L+scratch_bpc-1)/scratch_bpc;
      uint64_t packing=rd+(2ull*L+scratch_bpc-1)/scratch_bpc;
      if(fused()||(external()&&!port_bypass()))packing+=(((groups+63)/64)*8ull+scratch_bpc-1)/scratch_bpc+(((((groups+15)/16)+63)/64)*8ull+scratch_bpc-1)/scratch_bpc;
      if(mode=="quota_fused"&&header_ok)packing=std::max<uint64_t>(packing,uint64_t(quota_cycles)*groups);
      base=(stream_max_fusion?0:std::max<uint64_t>(groups,rd))+std::max<uint64_t>(groups,2*rd)+std::max<uint64_t>(groups,packing);
    }
    uint64_t extra=fused()?((mode=="zero_fused"||(lazy()&&!header_ok))?zero_tail:fused_tail):(lazy()&&!header_ok?zero_tail:0);
    if(external()&&!port_bypass())extra=fused_tail;return base+8+extra;
  }
  uint64_t mass_service()const {
    if(legacy_epu_timing)return groups+(active_mode()=="zero"?0:4);
    return std::max<uint64_t>(groups+4,(2ull*L+scratch_bpc-1)/scratch_bpc+(4ull*groups+scratch_bpc-1)/scratch_bpc)+(approximate()&&header_ok?budget_setup:0);
  }
  uint64_t merge_service()const {
    if(legacy_epu_timing)return (D+15)/16;
    uint64_t dims=token_packed?64:std::min<int>(64,D-dtile*64);
    return std::max<uint64_t>((dims+15)/16,3*((4*dims+scratch_bpc-1)/scratch_bpc))+2+(token_packed?4:0);
  }
  std::string active_mode()const {
    if(lazy())return head_choice;
    if(mode=="zero_fused")return "zero";
    if(approximate()&&!header_ok)return "dense";
    return mode=="greedy_fused"?"greedy":mode;
  }
  bool observer_request(const Task& t)const {
    return lazy()&&((t.op==VMac&&bootstrap_head)||(t.op==AppendV&&lazy_bound_ready));
  }
  void schedule_observer(const Task& t) {
    Clk_t begin=std::max(clk,observer_deadline);observer_deadline=begin+observer_interval;
    if(t.op==VMac&&begin!=clk)throw std::runtime_error("PU observer cannot accept native rate without a spill buffer");
    observer.push_back({t,clk,begin,begin+observer_latency,generation});
    observer_peak=std::max<uint64_t>(observer_peak,observer.size());
    if(observer.size()>64)throw std::runtime_error("Observer credits exceeded");
  }
  void commit_observers() {
    while(!observer.empty()&&observer.front().end<=clk) {
      auto o=observer.front();observer.pop_front();auto t=o.task;
      if(o.epoch!=generation||t.q!=q||t.h!=h||fault=="observer_wrong_epoch")throw std::runtime_error("Observer epoch/owner mismatch");
      observer_events<<t.id<<'\t'<<o.ready<<'\t'<<o.start<<'\t'<<o.end<<'\t'<<t.op<<'\t'<<t.x<<'\t'<<t.y<<'\t'<<generation<<'\n';
      if(t.op==VMac) {
        auto ix=t.y*((nmax+15)/16)+t.x;
        if(observed_coverage[ix])throw std::runtime_error("Duplicate observer coverage");
        if(!(fault=="observer_missing_group"&&t.x==0&&t.y==0))observed_coverage[ix]=1;
        for(int stripe=0;stripe<16;stripe++)for(int d=0;d<64;d++) {
          int token=(token_packed?2*t.x+d/32:t.x)*16+stripe,dim=token_packed?d%32:t.y*64+d;
          if(token<L&&dim<int(D))pu_max[stripe*4+d/16]=std::max<uint16_t>(pu_max[stripe*4+d/16],V[token*D+dim]&0x7fff);
        }
        ++observer_mac_jobs;
      } else {
        for(int j=0;j<16;j++){auto a=uint16_t(V[t.x*D+t.y*16+j]&0x7fff);if(bank_envelope?a>bank_max[t.y%4]:a>amax_bits){envelope_dirty=true;budget_dirty=true;}amax_bits=std::max(amax_bits,a);bank_max[t.y%4]=std::max(bank_max[t.y%4],a);}
        ++observer_append_jobs;
      }
    }
  }
  void gather_observer() {
    uint64_t seen=std::accumulate(observed_coverage.begin(),observed_coverage.end(),uint64_t(0));
    if(seen!=uint64_t(hardware_groups()*dtiles))throw std::runtime_error("Incomplete mandatory Value observation");
    gathered_valid.fill(false);setphase("stat_move");
    for(int r=0;r<2;r++)for(int b=0;b<4;b++)enqueue(StatMove,HDR+r*RANK+b*BANK,r,b);
  }
  void enqueue(int op,Addr_t addr,int x=0,int y=0,int z=0) {
    tasks.push_back({next_id++,op,x,y,z,q,h,addr,0});
    if(next_id<0) throw std::runtime_error("Request ID exhausted");
  }
  int reqtype(int op)const {
    if(op==NormLoad||op==HeaderLoad||op==MigrateKLoad||op==MigrateVLoad||op==PortKeyRead||op==PortMetaRead||op==PortRemapKRead||op==PortRemapVRead)return Request::Type::Read;
    if(op==Publish||op==AppendK||op==AppendV||op==PortMetaWrite||op==PortQuantWrite||op==PortRemapKWrite||op==PortRemapVWrite)return Request::Type::Write;
    if(op==QWrite)return Request::Type::PIM_WR_GB;
    if(op==QMac||op==VMac)return Request::Type::PIM_MAC_AB;
    if(op==QMove||op==VMove||op==StatMove)return Request::Type::PIM_MV_SB;
    if(op==PMove)return Request::Type::PIM_MV_GB;
    return Request::Type::PIM_SFM;
  }
  void setphase(const std::string& next) {
    if(phase!="start") {
      YAML::Node x;x["query"]=phase_q;x["head"]=phase_h;x["phase"]=phase;
      x["start"]=phase_start;x["end"]=clk;x["cycles"]=clk-phase_start;x["port_initial_setup"]=port_phase_setup;phase_records.push_back(x);
    }
    phase=next;phase_start=clk;phase_q=q;phase_h=h;port_phase_setup=port_context_setup;
  }
  void epuservice(uint64_t cycles,std::function<void()> action) {
    if(epu_commit)throw std::runtime_error("EPU overcommit");
    epu_until=clk+std::max<uint64_t>(1,cycles);epu_commit=std::move(action);
  }
  void build_loads() {
    build_end=std::min(committed,build_cursor+64);normsum.fill(0);
    setphase("build_load");
    for(int t=build_cursor;t<build_end;t++)for(int c=0;c<int(D)/16;c++)enqueue(NormLoad,vaddr(t,c),t,c);
  }
  void migration_loads(){
    build_cursor=committed;build_end=std::min(L,committed+64);setphase("migration_load");
    migration_k_valid.assign((build_end-build_cursor)*(D/16),false);migration_v_valid=migration_k_valid;
    migration_k.resize((build_end-build_cursor)*D);migration_v.resize((build_end-build_cursor)*D);
    for(int t=build_cursor;t<build_end;t++)for(int c=0;c<int(D)/16;c++){
      enqueue(MigrateKLoad,original_kaddr(t,c),t,c);enqueue(MigrateVLoad,original_vaddr(t,c),t,c);
    }
  }
  void initial_writes() {
    build_cursor=committed;build_end=std::min(L,committed+64);normsum.fill(0);
    setphase("init_write");
    for(int t=committed;t<build_end;t++)for(int c=0;c<int(D)/16;c++) {
      enqueue(AppendK,append_kaddr(t,c),t,c);enqueue(AppendV,vaddr(t,c),t,c);
    }
  }
  bool needs_norm(const Task& t)const {return uses_l2()&&(t.op==NormLoad||t.op==AppendV);}
  void schedule_norm(const Task& t,Clk_t ready) {
    int work=(16+norm_lanes-1)/norm_lanes;
    Clk_t begin=std::max(norm_deadline,ready);norm_deadline=begin+work;
    norm_finishes.push_back(norm_deadline);norm_busy+=work;
    norm_events<<t.id<<'\t'<<ready<<'\t'<<begin<<'\t'<<norm_deadline<<'\t'<<t.x<<'\t'<<t.y<<'\n';
  }
  void finish_norm(int rows) {
    for(int i=0;i<rows;i++)max_norm_sum=std::max(max_norm_sum,normsum[i]);
  }
  void update_radius() {
    if(lazy()) {
      double a=hfloat(amax_bits);
      if(bank_envelope&&D>16){double sq=0;for(int b=0;b<4;b++){int dims=16*((int(D)/16+3-b)/4);double v=hfloat(bank_max[b]);if(dims&&v)sq=up(sq+dims*v*v);}radius=sq==0?0:up(std::sqrt(sq));}
      else radius=a==0?0:up(std::sqrt(double(D)*a*a));
      ++sqrt_count;
      if(!std::isfinite(radius))throw std::runtime_error("Invalid amax envelope");
      return;
    }
    if(max_norm_sum==0){radius=0;++sqrt_count;return;}
    double u=std::ldexp(1.,-24);double gn=up((double(D)*u)/(1-double(D)*u));
    double den=down(1-gn);
    radius=up(std::sqrt(up(double(max_norm_sum)/den)));++sqrt_count;
    if(!std::isfinite(radius))throw std::runtime_error("Invalid Value norm radius");
  }
  void begin_publish(){setphase("publish");if(resident())epuservice(4,[this]{resident_publish();});else enqueue(Publish,HDR);}
  void publish() {
    if(lazy()&&!envelope_dirty)begin_publish();
    else {setphase("radius");epuservice(lazy()&&bank_envelope&&D>16?24:16,[this]{update_radius();envelope_dirty=false;});}
  }
  void start_step() {
    
    
    bootstrap_head=false;
    int next_length=int(lengths[q]),next_groups=(next_length+15)/16;if(next_groups!=groups||uses_l2())budget_dirty=true;L=next_length;groups=next_groups;h=-1;step_start=clk;
    if(resident()&&evict_each_step&&q>0){lazy_bound_ready=false;header_valid=false;header_ok=false;budget_epoch=0;budget_dirty=true;envelope_dirty=true;amax_bits=0;bank_max.fill(0);radius=0;loaded_radius=0;stored_radius=0;pu_max.fill(0);++resident_evictions;}
    header_ok=false;if(q>0)header_valid=false;
    if(q==0) {
      if(uses_l2()){setphase("header");if(resident())epuservice(4,[this]{resident_load();});else enqueue(HeaderLoad,HDR);} else if(lazy()&&lazy_bound_ready){setphase("header");if(resident())epuservice(4,[this]{resident_load();});else enqueue(HeaderLoad,HDR);}
      else start_head();
      return;
    }
    ++generation;normsum.fill(0);setphase("append");
    if(L-committed>64)throw std::runtime_error("Append batch exceeds finite tile");
    for(int t=committed;t<L;t++)for(int c=0;c<int(D)/16;c++) {
      enqueue(AppendK,append_kaddr(t,c),t,c);enqueue(AppendV,vaddr(t,c),t,c);
    }
  }
  void start_head() {
    if(committed!=L)throw std::runtime_error("Head started before cache writes committed");
    h+=(h<0?1:pair_count);pair_count=pair_enabled&&h+1<int(H)?2:1;
    if(paired()){++pair_batches;std::fill(scores_b.begin(),scores_b.end(),0);std::fill(prob_b.begin(),prob_b.end(),0);std::fill(gbvalid_b.begin(),gbvalid_b.end(),false);gb_heads={-1,-1};}
    ingress_max=-std::numeric_limits<float>::infinity();ingress_index=UINT32_MAX;ingress_seen=0;
    ingress_scale=1.f/std::sqrt(float(D));if(stream_max_fusion)audit_score_coverage.assign(L,false);
    bootstrap_head=false;head_choice=lazy()?(header_ok?(mode=="quota_fused"?"quota":(mode=="uniform_fused"?"uniform":"greedy")):"cold"):"";batch=0;dtile=0;bound=0;comparisons=0;skipped=0;skip_mass=0;total_mass=0;allowance=0;
    std::fill(scores.begin(),scores.end(),0);std::fill(qpu.begin(),qpu.end(),0);
    std::fill(gbvalid.begin(),gbvalid.end(),0);std::fill(skip.begin(),skip.end(),0);
    if(external()){port_begin_head();return;}
    if(paired()||token_packed){setphase(token_packed?"token_dispatch":"pair_dispatch");epuservice(pair_dispatch,[this]{issue_q_writes();});}else issue_q_writes();
  }
  void q_macs() {
    if(external()){
      while(batch<hardware_groups()){
        bool any=false;for(int g=batch;g<std::min(hardware_groups(),batch+16);g++)any|=port_active_group(g);
        if(any)break;batch+=16;
      }
      if(batch>=hardware_groups()){setphase("sfm_kick");enqueue(SfmKick,0);return;}
    }
    setphase("q_mac");
    for(int g=batch;g<std::min(hardware_groups(),batch+16);g++)if(!external()||port_active_group(g))for(int t=0;t<dtiles;t++){
      Addr_t addr=32ll*(g*dtiles+t);
      if(external()&&((external_policy=="sage"&&port_key_quantized)||port_shadow_k[token_packed?2*g:g]))addr+=PORT_SHADOW;
      enqueue(QMac,addr,g,t);
    }
  }
  void q_moves() {
    setphase("q_move");
    for(int hp=0;hp<((paired()||token_packed)?2:1);hp++)for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)enqueue(QMove,r*RANK+bg*BG+hp*32,r,bg+4*hp);
  }
  void softmax() {
    float scale=1.f/std::sqrt(float(D)),mx=-std::numeric_limits<float>::infinity();
    if(stream_max_fusion){
      uint32_t expected=external()?port_valid_tokens:L;
      if(ingress_seen!=expected)throw std::runtime_error("Incomplete streamed maximum");
      for(int t=0;t<L;t++)if(audit_score_coverage[t]!=(!external()||bool(port_keep_slot[t])))throw std::runtime_error("Wrong sparse streamed maximum coverage");
      mx=ingress_max;
    }else for(int t=0;t<L;t++)mx=std::max(mx,scores[t]*scale);
    
    
    size_t trace_off=(q*H+h)*nmax;
    if(external()){for(int t=0;t<L;t++)all_scores[trace_off+port_order[t]]=scores[t];}
    else std::copy(scores.begin(),scores.end(),all_scores.begin()+trace_off);
    std::vector<float> expv(inplace_exp?0:L);float z=0;
    for(int g=0;g<groups;g++) {
      std::array<float,16> a{};
      for(int s=0;s<16;s++){int t=g*16+s;if(t<L){
        float e=(!external()||port_keep_slot[t])?counted_exp(scores[t]*scale-mx):0.f;
        a[s]=e;if(inplace_exp)scores[t]=e;else expv[t]=e;
      }}
      for(int stride=1;stride<16;stride*=2)for(int i=0;i<16;i+=2*stride)a[i]+=a[i+stride];
      z+=a[0];
    }
    if(!(z>0) || !std::isfinite(z))throw std::runtime_error("Invalid implemented softmax");
    std::fill(prob.begin(),prob.end(),0);
    if(!fused())for(int t=0;t<L;t++)prob[t]=hbits((inplace_exp?scores[t]:expv[t])/z);
    else {
      double u=std::ldexp(1.,-24);int k=(L+15)/16+15;gamma=up((k*u)/(1-k*u));
      total_mass=0;allowance=0;fused_skip_mass=0;fused_skipped=0;fused_zero=0;
      std::fill(skip.begin(),skip.end(),false);transport_frames.assign(frame_count(),false);
      if(approximate()&&header_ok) {
        if(budget_epoch!=generation||fault=="budget_wrong_epoch")throw std::runtime_error("Shared budget epoch mismatch");
        allowance=prefix_allowance;gamma=prefix_gamma;
      }
      uint64_t remaining=allowance;
      for(int g=0;g<groups;g++) {
        uint32_t m=0;
        for(int lane=0;lane<16;lane++) {
          int t=g*16+lane;if(t>=L)break;
          prob[t]=hbits((inplace_exp?scores[t]:expv[t])/z);m+=punits(prob[t]);
        }
        total_mass+=m;fused_zero+=(m==0);bool take=false;
        if(approximate()&&header_ok) {
          bool ok=loaded_radius==0||(mode=="quota_fused"?uint64_t(m)*(groups-g)<=remaining:(mode=="uniform_fused"?m<=uniform_cut:m<=remaining));
          if(ok){if(m>remaining)throw std::runtime_error("Policy budget underflow");take=true;remaining-=m;}
        }
        else if(mode=="zero_fused"||(lazy()&&!lazy_bound_ready))take=(m==0);
        if(take){skip[g]=true;fused_skip_mass+=m;++fused_skipped;}else transport_frames[frame_index(g)]=true;
      }
      if(total_mass>(513ull<<15))throw std::runtime_error("Proven FP16 softmax mass cap violated");
    }
    if(external())port_build_masks();
    size_t off=(q*H+h)*nmax;
    if(external()){for(int t=0;t<L;t++)all_prob[off+port_order[t]]=prob[t];}
    else std::copy(prob.begin(),prob.end(),all_prob.begin()+off);
    uint64_t score_bytes=4ull*L*(stream_max_fusion?1:2);
    softmax_score_read_bytes+=score_bytes;exp_scratch_read_bytes+=4ull*L;exp_scratch_write_bytes+=4ull*L;
    YAML::Node row;row["query"]=q;row["head"]=h;row["length"]=L;
    row["max_bits"]=std::bit_cast<uint32_t>(mx);row["denominator_bits"]=std::bit_cast<uint32_t>(z);
    row["exp_calls"]=external()?port_valid_tokens:L;row["score_read_bytes"]=score_bytes;
    row["exp_scratch_read_bytes"]=4ull*L;row["exp_scratch_write_bytes"]=4ull*L;
    row["streamed_max_tokens"]=stream_max_fusion?ingress_seen:0;exact_softmax_heads.push_back(row);
    if(fused()||lazy()) {
      if(!fused())mass_pass();
      if(lazy()&&!header_ok) {
        unsigned zero=fused()?fused_zero:0;if(!fused())for(int g=0;g<groups;g++)zero+=(masses[g]==0);
        if(!lazy_bound_ready&&epsilon>0&&zero*2<unsigned(groups)) {
          head_choice="bootstrap";bootstrap_head=true;++bootstrap_heads;
          pu_max.fill(0);observed_coverage.assign(dtiles*((nmax+15)/16),0);
        } else head_choice=lazy_bound_ready?"dense":"zero";
      }
      if(fused()&&bootstrap_head){std::fill(skip.begin(),skip.end(),false);std::fill(transport_frames.begin(),transport_frames.end(),true);fused_skip_mass=0;fused_skipped=0;}
    }
  }
  void mass_pass() {
    total_mass=0;
    for(int g=0;g<groups;g++) {
      uint32_t m=0;for(int s=0;s<16;s++)if(g*16+s<L)m+=punits(prob[g*16+s]);
      masses[g]=m;total_mass+=m;
    }
    double u=std::ldexp(1.,-24);int k=(L+15)/16+15;
    gamma=up((k*u)/(1-k*u));
    if(approximate()&&header_ok) {
      if(loaded_radius==0)allowance=total_mass;
      else {
        
        if(fused()&&total_mass>(513ull<<15))throw std::runtime_error("Softmax mass cap violated");
        double reserve=up(up(2*gamma)*(fused()?(513./512.):std::ldexp(double(total_mass),-24)));
        double available=down(down(epsilon/loaded_radius)-reserve);
        if(available>0)allowance=uint64_t(std::floor(std::ldexp(std::min(available,std::ldexp(double(total_mass),-24)),24)));
      }
    }
  }
  uint64_t plan() {
    std::string am=active_mode();std::fill(skip.begin(),skip.end(),0);
    if(am=="zero") {for(int g=0;g<groups;g++)skip[g]=(masses[g]==0);return 1;}
    if(am=="dense"||am=="bootstrap"||am=="cold")return 1;
    uint64_t remaining=allowance;
    if(am=="greedy") {
      for(int g=0;g<groups;g++)if(masses[g]<=remaining){skip[g]=1;remaining-=masses[g];}
      return groups;
    }
    if(am=="bucket") {
      std::array<uint32_t,32> counts{};std::vector<int> bucket(groups,-1);
      for(int g=0;g<groups;g++) {
        uint32_t m=masses[g];if(!m){skip[g]=1;continue;}
        int e=std::bit_width(m-1);bucket[g]=e;++counts[e];
      }
      std::array<uint32_t,32> quota{};
      for(int e=0;e<32;e++) {
        uint64_t price=1ull<<e,n=std::min<uint64_t>(counts[e],remaining/price);
        quota[e]=n;remaining-=n*price;
        if(n<counts[e])break;
      }
      for(int g=0;g<groups;g++)if(bucket[g]>=0&&quota[bucket[g]]){skip[g]=1;--quota[bucket[g]];}
      return groups+32;
    }
    
    unsigned n=std::bit_ceil(unsigned(groups));
    std::vector<std::pair<uint32_t,uint32_t>> a(n,{UINT32_MAX,UINT32_MAX});
    for(int g=0;g<groups;g++)a[g]={masses[g],uint32_t(g)};
    for(unsigned k=2;k<=n;k*=2)for(unsigned j=k/2;j;j/=2)for(unsigned i=0;i<n;i++) {
      unsigned t=i^j;if(t<=i)continue;++comparisons;
      if((a[i]>a[t])==((i&k)==0))std::swap(a[i],a[t]);
    }
    for(auto [m,g]:a) {
      if(g>=unsigned(groups)||m>remaining)break;
      skip[g]=1;remaining-=m;
    }
    return std::max<uint64_t>((comparisons+sort_width-1)/sort_width,legacy_epu_timing?0:(32*comparisons+scratch_bpc-1)/scratch_bpc)+groups;
  }
  void finish_plan() {
    if(paired()){pair_finish_plan();return;}
    skipped=0;skip_mass=0;
    if(fused()){skipped=fused_skipped;skip_mass=fused_skip_mass;}else for(int g=0;g<groups;g++)if(skip[g]){++skipped;skip_mass+=masses[g];}
    if(active_mode()=="zero" && skip_mass)throw std::runtime_error("Unsafe zero command gate");
    if(approximate()&&header_ok&&skip_mass>allowance)throw std::runtime_error("Integer allowance overrun");
    if(skip_mass&&loaded_radius>0)bound=up(loaded_radius*up(std::ldexp(double(skip_mass),-24)+up(up(2*gamma)*std::ldexp(double(total_mass),-24))));
    if(skip_mass&&bound>epsilon)throw std::runtime_error("Rounded omission certificate exceeds budget");
    size_t off=(q*H+h)*((nmax+15)/16);std::copy(skip.begin(),skip.end(),all_skip.begin()+off);
    setphase("p_move");
    if(token_packed){
      for(int hp=0;hp<2;hp++)for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)for(int c=0;c<(groups+31)/32;c++){
        bool needed=false;
        for(int i=0;i<16;i++){int g=2*(c*16+i)+hp;if(g<groups&&(!gate_transport()||!skip[g]))needed=true;}
        if(needed)enqueue(PMove,r*RANK+bg*BG+hp*2*BANK+c,r,bg+4*hp,c);
      }
    }else for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)for(int c=0;c<(L+255)/256;c++)
      if(!gate_transport()||transport_frames[c])enqueue(PMove,r*RANK+bg*BG+c,r,bg,c);
  }
  void v_macs() {
    setphase("v_mac");std::fill(svpu.begin(),svpu.end(),0);
    if(token_packed){
      for(int g=0;g<hardware_groups();g++)if(!skip[2*g]||(2*g+1<groups&&!skip[2*g+1]))enqueue(VMac,VBASE+32ll*g+(external()&&port_shadow_v[2*g]?PORT_SHADOW:0),g,0);
    }else for(int g=0;g<groups;g++)if(!skip[g]||(paired()&&!skip_b[g]))enqueue(VMac,VBASE+32ll*(dtile*((nmax+15)/16)+g)+(external()&&port_shadow_v[g]?PORT_SHADOW:0),g,dtile);
  }
  void v_moves() {
    setphase("v_move");std::fill(rankout.begin(),rankout.end(),0);
    for(int r=0;r<2;r++)for(int b=0;b<4;b++)if(token_packed||paired()||!lean_scalar||dtile*64+b*16<int(D))enqueue(VMove,VBASE+r*RANK+b*BANK,r,b);
  }
  void finish_head() {
    if(external())port_heads[port_heads.size()-1]["final_cost"]=cost_node(port_head_cost);
    YAML::Node r;r["query"]=q;r["head"]=h;r["length"]=L;r["active_mode"]=active_mode();
    r["G"]=bootstrap_head?0:loaded_radius;r["gamma"]=gamma;r["mass_units"]=total_mass;r["allowance_units"]=allowance;
    r["skipped_mass_units"]=skip_mass;r["skipped_groups"]=skipped;r["groups"]=groups;
    r["error_bound_vs_implemented_dense"]=bound;r["sort_comparisons"]=comparisons;
    r["metadata_valid"]=bootstrap_head?false:header_ok;r["bound_kind"]=lazy()?(bank_envelope?"bank_amax_envelope":"amax_envelope"):"row_l2";
    for(auto a:bank_max)r["bank_amax_bits"].push_back(a);r["bank_envelope"]=bank_envelope;r["amax_bits"]=amax_bits;r["reserve_mass_units"]=fused()?(513ull<<15):total_mass;
    r["fused"]=fused();r["bootstrap"]=bootstrap_head;r["token_packed"]=token_packed;r["error_bound_reference"]=token_packed?"same_layout_dense":"scalar_dense";
    if(paired()){
      for(int hp=0;hp<2;hp++){
        auto rr=YAML::Clone(r);uint64_t mass=0,omitted=0;int ks=0;auto& pp=hp?prob_b:prob;auto& sk=hp?skip_b:skip;
        for(int g=0;g<groups;g++){uint32_t m=0;for(int j=0;j<16;j++)if(g*16+j<L)m+=punits(pp[g*16+j]);mass+=m;if(sk[g]){omitted+=m;++ks;}}
        if(omitted)throw std::runtime_error("Paired exact gate omitted nonzero mass");
        rr["head"]=h+hp;rr["mass_units"]=mass;rr["skipped_mass_units"]=omitted;rr["skipped_groups"]=ks;rr["paired_batch"]=true;rr["lane_offset"]=hp*32;head_records.push_back(rr);
      }
    }else{r["paired_batch"]=false;head_records.push_back(r);}
    if(h+pair_count<int(H)){start_head();return;}
    YAML::Node s;s["query"]=q;s["length"]=L;s["start"]=step_start;s["end"]=clk;s["cycles"]=clk-step_start;
    step_records.push_back(s);
    if(++q<int(Q)){start_step();return;}
    setphase("done");finished=true;dump();
  }
  void completed(int id,Request& req) {
    auto it=outstanding.find(id);
    if(it==outstanding.end())throw std::runtime_error("Duplicate or stale completion ID");
    Task t=it->second;outstanding.erase(it);
    if(t.q!=q || t.h!=h)throw std::runtime_error("Completion crossed query/head ownership");
    if(req.addr!=t.addr||req.source_id!=id)throw std::runtime_error("Command source/address mismatch");
    int lat=req.type_id==Request::Type::Read?dram->m_read_latency:(req.type_id==Request::Type::Write?
      dram->m_timing_vals("nWR")+dram->m_timing_vals("nBL"):(req.type_id==Request::Type::PIM_MAC_AB?mac_latency:move_latency));
    if(req.depart>clk)throw std::runtime_error("Completion fired before modeled readiness");
    events<<id<<'\t'<<t.accepted<<'\t'<<req.depart-lat<<'\t'<<clk<<'\t'<<q<<'\t'<<h<<'\t'<<t.op<<'\t'<<t.addr<<'\t'<<t.x<<'\t'<<t.y<<'\t'<<t.z<<'\n';
    ++callbacks;
    if(token_packed&&!token_use_route&&(t.op==QMac||t.op==VMac)){
      Addr_t expected=(t.op==QMac?32ll*t.x:VBASE+32ll*t.x);
      if(external()&&((t.op==QMac&&((external_policy=="sage"&&port_key_quantized)||port_shadow_k[2*t.x]))||(t.op==VMac&&port_shadow_v[2*t.x])))expected+=PORT_SHADOW;
      if(t.y!=0||t.x<0||t.x>=hardware_groups()||req.addr!=expected)throw std::runtime_error("Invalid local packed native source");
      if(t.op==QMac)token_q_mac(t.x);
      else {token_v_mac(t.x);if(observer_request(t))schedule_observer(t);}
      if(fault=="duplicate_completion"&&!duplicate_injected){duplicate_injected=true;completed(id,req);}
      return;
    }
    if((paired()||(token_packed&&token_use_route))&&(t.op==QMac||t.op==VMac)){
      if(t.y!=0||t.x<0||t.x>=hardware_groups()||req.addr!=(t.op==QMac?32ll*t.x:VBASE+32ll*t.x))throw std::runtime_error("Invalid paired native source");
      Clk_t begin=std::max(clk,route_deadline);route_deadline=begin+route_interval();
      routes.push_back({t.id,t.op,t.x,t.y,q,h,clk,begin,begin+route_interval()+route_latency,generation});route_peak=std::max<uint64_t>(route_peak,routes.size());
      if(routes.size()>unsigned(route_credits))throw std::runtime_error("Paired route credits exceeded");
      if(fault=="duplicate_completion"&&!duplicate_injected){duplicate_injected=true;completed(id,req);}return;
    }
    switch(t.op) {
      case NormLoad: {
        if(t.addr!=vaddr(t.x,t.y)||t.x>=committed)throw std::runtime_error("Invalid builder source");
        int stripe=req.addr_vec[1]*8+req.addr_vec[2]*4+req.addr_vec[3];
        int linear=req.addr_vec[5]*32+req.addr_vec[6]-int(VBASE/32),ng=(nmax+15)/16;
        if((linear%ng)*16+stripe!=t.x || (linear/ng)*4+req.addr_vec[4]!=t.y)
          throw std::runtime_error("Physical Value load coverage mismatch");
        for(int j=0;j<16;j++){float v=hfloat(V[t.x*D+t.y*16+j]);normsum[t.x-build_cursor]=std::fma(v,v,normsum[t.x-build_cursor]);}
        schedule_norm(t,clk);++count_normld;break;
      }
      case Publish:stored_radius=radius;stored_length=committed;stored_generation=generation;stored_owner=owner;header_valid=true;++count_metawr;break;
      case HeaderLoad: {
        loaded_radius=stored_radius;uint64_t gen=stored_generation,own=stored_owner;int len=stored_length;bool valid=header_valid;
        if(q==fault_step) {if(fault=="stale_generation")--gen;if(fault=="wrong_owner")--own;if(fault=="wrong_length")--len;if(fault=="invalid")valid=false;if(fault=="nan_radius")loaded_radius=std::numeric_limits<double>::quiet_NaN();}
        header_ok=valid&&own==owner&&gen==generation&&len==L&&std::isfinite(loaded_radius)&&loaded_radius>=0;
        if(!header_ok)loaded_radius=0;++count_headerld;break;
      }
      case MigrateKLoad:case MigrateVLoad:{
        if(!migrating||t.x<build_cursor||t.x>=build_end||t.addr!=(t.op==MigrateKLoad?original_kaddr(t.x,t.y):original_vaddr(t.x,t.y)))throw std::runtime_error("Invalid migration source");
        auto& valid=t.op==MigrateKLoad?migration_k_valid:migration_v_valid;auto& data=t.op==MigrateKLoad?migration_k:migration_v;auto& source=t.op==MigrateKLoad?K:V;
        int ix=(t.x-build_cursor)*(D/16)+t.y;if(valid[ix])throw std::runtime_error("Duplicate migration load");valid[ix]=true;
        for(int j=0;j<16;j++)data[(t.x-build_cursor)*D+t.y*16+j]=source[t.x*D+t.y*16+j];++migration_reads;break;
      }
      case AppendK:case AppendV:
        if(migrating){
          auto& data=t.op==AppendK?migration_k:migration_v;auto& source=t.op==AppendK?K:V;
          if(t.x<build_cursor||t.x>=build_end)throw std::runtime_error("Migration buffer ownership violation");
          for(int j=0;j<16;j++)if(data[(t.x-build_cursor)*D+t.y*16+j]!=source[t.x*D+t.y*16+j])throw std::runtime_error("Migration changed canonical bytes");
          ++migration_writes;
        }
        if(t.x<committed||t.x>=L)throw std::runtime_error("Append ownership violation");
        if(external())port_publish_append(t);
        if(t.op==AppendV&&uses_l2()) {
          for(int j=0;j<16;j++){float v=hfloat(V[t.x*D+t.y*16+j]);normsum[t.x-committed]=std::fma(v,v,normsum[t.x-committed]);}
          schedule_norm(t,clk);
        }
        if(t.op==AppendV&&observer_request(t))schedule_observer(t);
        if(initializing)++count_initwr;else ++count_kvwr;break;
      case QWrite:
        for(int j=0;j<16;j++){
          int d=t.y*64+t.x*16+j,sd=token_packed?d%32:d;
          if(paired())qgb[d]=queries[(q*H+h+d/32)*D+d%32];
          else qgb[d]=sd<int(D)?(port_query_bits.empty()?queries[(q*H+h)*D+sd]:port_query_bits[sd]):0;
        }break;
      case QMac:{
        Addr_t logical=external()?(req.addr&~PORT_SHADOW):req.addr;
        if(logical/32/dtiles!=t.x || logical/32%dtiles!=t.y)
          throw std::runtime_error("Physical QK command coverage mismatch");
        for(int s=0;s<16;s++)for(int b=0;b<4;b++)for(int j=0;j<16;j++) {
          int d=t.y*64+b*16+j,token=t.x*16+s,ix=(t.x-batch)*64+s*4+b;
          qpu[ix]=std::fma(kval(token,d),hfloat(qgb[d]),qpu[ix]);
        }
        ++count_qmac;
        if(fault=="duplicate_completion"&&!duplicate_injected){duplicate_injected=true;completed(id,req);}break;}
      case QMove:
        if(token_packed){
          int hp=t.y/4,bg=t.y%4;
          for(int pc=0;pc<2;pc++)for(int slot=0;slot<std::min(16,hardware_groups()-batch);slot++){
            int stripe=pc*8+t.x*4+bg,token=(2*(batch+slot)+hp)*16+stripe;float x=0;
            x+=qpu[slot*64+stripe*4+hp*2];x+=qpu[slot*64+stripe*4+hp*2+1];x+=0.f;x+=0.f;
            if(token<L&&(!external()||port_keep_slot[token])){if(external_policy=="sage")x*=port_multiplier[token];scores[token]=x;observe_score(token,x);}
          }
          break;
        }
        if(paired()){
          int hp=t.y/4,bg=t.y%4;
          for(int pc=0;pc<2;pc++)for(int slot=0;slot<std::min(16,groups-batch);slot++){
            int stripe=pc*8+t.x*4+bg,token=(batch+slot)*16+stripe;float x=0;
            x+=qpu[slot*64+stripe*4+hp*2];x+=qpu[slot*64+stripe*4+hp*2+1];x+=0.f;x+=0.f;
            if(token<L)(hp?scores_b:scores)[token]=x;
          }
          break;
        }
        for(int pc=0;pc<2;pc++)for(int slot=0;slot<std::min(16,groups-batch);slot++) {
          int stripe=pc*8+t.x*4+t.y,token=(batch+slot)*16+stripe;
          float v=0;for(int b=0;b<4;b++)v+=qpu[slot*64+stripe*4+b];
          if(token<L&&(!external()||port_keep_slot[token])){if(external_policy=="sage")v*=port_multiplier[token];scores[token]=v;observe_score(token,v);}
        }break;
      case PortKeyRead:{
        if(t.addr!=port_original_key_addr(t.x,t.y)||t.x<port_cursor||t.x>=port_end)throw std::runtime_error("Invalid external Key read");
        int ix=(t.x-port_cursor)*(D/16)+t.y;if(port_tile_valid[ix])throw std::runtime_error("Duplicate external Key tile read");port_tile_valid[ix]=1;
        int slot=port_canonical_slot(t.x);int version=external_policy=="sage"?0:port_shadow_k[slot/16];
        for(int j=0;j<16;j++)port_tile[(t.x-port_cursor)*D+t.y*16+j]=port_dram_word(false,version,slot,t.y*16+j);++port_read_commands;break;
      }
      case PortMetaRead:port_complete_metadata(false,t);++port_metadata_reads;break;
      case PortMetaWrite:port_complete_metadata(true,t);++port_metadata_writes;break;
      case PortQuantWrite:{
        if(t.addr!=base_kaddr(t.x,t.y)+PORT_SHADOW||t.x<port_cursor||t.x>=port_end)throw std::runtime_error("Invalid quantized Key publication");
        for(int j=0;j<16;j++){
          auto v=sage_state.key_bits[t.x*D+t.y*16+j];if(!std::isfinite(hfloat(v)))throw std::runtime_error("Invalid quantized Key payload");
          port_quant_dram[t.x*D+t.y*16+j]=v;port_k_dram[1][t.x*D+t.y*16+j]=v;
        }
        port_quant_written[t.x*(D/16)+t.y]=1;port_k_valid[1][t.x*(D/16)+t.y]=1;
        ++port_key_representation_writes;++port_write_commands;break;
      }
      case PortRemapKRead:case PortRemapVRead:{
        bool value=t.op==PortRemapVRead;
        if(t.addr!=port_remap_source(t.x,t.y,value)||t.x<port_cursor||t.x>=port_end)throw std::runtime_error("Invalid KV remap source");
        int old=port_old_inverse[port_proposed_order[t.x]],version=(value?port_old_shadow_v:port_old_shadow_k)[old/16];
        auto& dst=value?port_remap_v:port_remap_k;
        for(int j=0;j<16;j++)dst[(t.x-port_cursor)*D+t.y*16+j]=port_dram_word(value,version,old,t.y*16+j);
        ++port_remap_read_commands;++port_read_commands;break;
      }
      case PortRemapKWrite:case PortRemapVWrite:{
        bool value=t.op==PortRemapVWrite;int g=port_remap_groups[port_remap_cursor];
        Addr_t expected=(value?base_vaddr(t.x,t.y):base_kaddr(t.x,t.y))+((value?port_old_shadow_v[g]:port_old_shadow_k[g])?0:PORT_SHADOW);
        if(t.addr!=expected)throw std::runtime_error("Invalid alternate-group KV write");
        int canonical=port_proposed_order[t.x];auto& data=value?port_remap_v:port_remap_k;const auto& gold=value?V:K;
        int version=1-(value?port_old_shadow_v[g]:port_old_shadow_k[g]);
        auto& image=value?port_v_dram[version]:port_k_dram[version];auto& valid=value?port_v_valid[version]:port_k_valid[version];
        for(int j=0;j<16;j++){
          auto word=data[(t.x-port_cursor)*D+t.y*16+j];
          if(word!=gold[canonical*D+t.y*16+j])throw std::runtime_error("KV remap changed operand bytes");
          image[t.x*D+t.y*16+j]=word;
        }
        valid[t.x*(D/16)+t.y]=1;
        ++port_remap_write_commands;++port_write_commands;break;
      }
      case SfmKick:break;
      case PMove:
        ++probability_moves;
        if(token_packed){
          int hp=t.y/4,bg=t.y%4;
          for(int pc=0;pc<2;pc++)for(int i=0;i<16;i++){
            int stripe=pc*8+t.x*4+bg,token=(2*(t.z*16+i)+hp)*16+stripe;
            if(token<int(nmax)){pgb[token]=token<L?prob[token]:0;gbvalid[token]=true;}
          }
          break;
        }
        if(paired()){
          int hp=t.y/4,bg=t.y%4;gb_heads[hp]=(fault=="operand_alias"&&hp==1)?h:h+hp;
          for(int pc=0;pc<2;pc++)for(int i=0;i<16;i++){
            int stripe=pc*8+t.x*4+bg,token=(t.z*16+i)*16+stripe;
            if(token<int(nmax)){(hp?pgb_b:pgb)[token]=token<L?(hp?prob_b:prob)[token]:0;(hp?gbvalid_b:gbvalid)[token]=true;}
          }
          break;
        }
        for(int pc=0;pc<2;pc++)for(int i=0;i<16;i++) {
          int stripe=pc*8+t.x*4+t.y,token=(t.z*16+i)*16+stripe;
          if(token<int(nmax)){pgb[token]=token<L?prob[token]:0;gbvalid[token]=1;}
        }break;
      case VMac:{
        Addr_t logical=external()?(req.addr&~PORT_SHADOW):req.addr;
        if((logical-VBASE)/32%((nmax+15)/16)!=t.x || (logical-VBASE)/32/((nmax+15)/16)!=t.y)
          throw std::runtime_error("Physical Value command coverage mismatch");
        for(int s=0;s<16;s++) {
          int token=t.x*16+s;
          if(token<int(nmax)&&!gbvalid[token])throw std::runtime_error("MAC consumed an unready probability");
          float p=token<L?hfloat(pgb[token]):0;
          for(int d=0;d<64;d++)svpu[s*64+d]=std::fma(p,vval(token,t.y*64+d),svpu[s*64+d]);
        }
        if(observer_request(t))schedule_observer(t);++count_vmac;break;}
      case StatMove:
        for(int pc=0;pc<2;pc++)for(int bg=0;bg<4;bg++) {
          int b=(pc*8+t.x*4+bg)*4+t.y;
          if(gathered_valid[b])throw std::runtime_error("Duplicate amax packet");
          gathered_max[b]=pu_max[b];gathered_valid[b]=true;
        }break;
      case VMove:
        for(int j=0;j<16;j++) {
          int d=t.y*16+j;float v=0;
          for(int pc=0;pc<2;pc++)for(int bg=0;bg<4;bg++)v+=svpu[(pc*8+t.x*4+bg)*64+d];
          rankout[t.x*64+d]=v;
        }break;
    }
  }
  void advance() {
    if(phase=="start") {
      start_clk=clk;committed=lengths[0];L=committed;h=-1;
      if(token_packed&&cache_init=="migrate"){committed=0;migrating=true;migration_loads();}
      else if(cache_init=="write"){committed=0;initial_writes();}
      else if(uses_l2()){build_cursor=0;build_loads();}
      else {initializing=false;init_cycles=0;start_step();}
    } else if(phase=="build_load") {
      setphase("build_norm");int rows=build_end-build_cursor;
      epuservice(rows,[this,rows]{finish_norm(rows);});
    } else if(phase=="build_norm") {
      build_cursor=build_end;if(build_cursor<committed)build_loads();else publish();
    } else if(phase=="migration_load"){
      for(bool x:migration_k_valid)if(!x)throw std::runtime_error("Incomplete Key migration");
      for(bool x:migration_v_valid)if(!x)throw std::runtime_error("Incomplete Value migration");
      setphase("migration_retile");
      uint64_t work=(16ull*(build_end-build_cursor)*D+scratch_bpc-1)/scratch_bpc;migration_epu_cycles+=work;
      epuservice(work,[]{});
    } else if(phase=="migration_retile")initial_writes();
    else if(phase=="init_write") {
      if(uses_l2()) {
        setphase("init_write_norm");int rows=build_end-committed;
        epuservice(rows,[this,rows]{finish_norm(rows);committed=build_end;});
      } else {
        committed=build_end;
        if(committed<L){if(migrating)migration_loads();else initial_writes();}
        else {migrating=false;initializing=false;init_cycles=clk-start_clk;start_step();}
      }
    } else if(phase=="init_write_norm") {
      if(committed<L)initial_writes();else publish();
    } else if(phase=="radius") {
      begin_publish();
    } else if(phase=="publish") {
      if(initializing){initializing=false;init_cycles=clk-start_clk;start_step();}
      else {setphase(bootstrap_head?"bootstrap_header":"header");if(resident())epuservice(4,[this]{resident_load();});else enqueue(HeaderLoad,HDR);}
    } else if(phase=="append") {
      int added=L-committed;
      if(uses_l2()) {setphase("append_norm");epuservice(added,[this,added]{finish_norm(added);committed=L;});}
      else if(lazy()&&lazy_bound_ready){committed=L;publish();}
      else {committed=L;start_head();}
    } else if(phase=="append_norm")publish();
    else if(phase=="header") {
      if(fused()&&approximate()&&header_ok){setphase("shared_budget");epuservice(shared_budget_service(),[this]{prepare_shared_budget();});}else start_head();
    } else if(phase=="shared_budget")start_head();
    else if(phase=="pair_dispatch"){}
    else if(phase=="port_key_read")port_process_tile();
    else if(phase=="port_compute")port_after_epu();
    else if(phase=="port_meta_read")port_after_meta();
    else if(phase=="port_meta_write"){
      if(port_metadata_merges_ready())port_service_metadata_merges();else port_after_meta();
    }
    else if(phase=="port_quant_write"){
      port_meta_transfer(true,port_tile_meta_write,2ull*D+4ull*(port_cursor/64),"next_tile");
    }else if(phase=="port_remap_read"){
      PortCost c;c.scratch_read_bytes=c.scratch_write_bytes=4ull*(port_end-port_cursor)*D;c.scratch_peak_bytes=4ull*(port_end-port_cursor)*D;
      charge_port("alternate_region_retile",c,"remap_copy");
    }else if(phase=="port_remap_write"){++port_remap_cursor;port_remap_read_group();}
    else if(phase=="q_write")q_macs();
    else if(phase=="q_mac")q_moves();
    else if(phase=="q_move") {
      uint64_t work=stream_max_fusion?streamed_batch_work():0;
      if(external_policy=="sage")work+=port_dequant_work();
      if(work){setphase("q_move_epu");epuservice(work,[]{});}else finish_q_move_batch();
    } else if(phase=="q_move_epu")finish_q_move_batch();
    else if(phase=="sfm_kick") {
      if(external_policy=="sparge")port_stage2();
      else {setphase("softmax");epuservice(softmax_service()*(paired()?2:1),[this]{if(paired())pair_softmax();else softmax();});}
    } else if(phase=="softmax") {
      if(external()||paired()||fused())finish_plan();
      else if(active_mode()=="dense"||active_mode()=="bootstrap") {total_mass=0;finish_plan();}
      else {setphase("mass");epuservice(mass_service(),[this]{mass_pass();});}
    } else if(phase=="mass") {
      setphase("plan");uint64_t work=plan();epuservice(work,[]{});
    } else if(phase=="plan")finish_plan();
    else if(phase=="p_move")v_macs();
    else if(phase=="v_mac")v_moves();
    else if(phase=="v_move") {
      setphase("merge");epuservice(merge_service()*(paired()?2:1),[this]{
        if(paired()){
          for(int hp=0;hp<2;hp++)for(int j=0;j<32;j++){int d=hp*32+j;float x=rankout[d]+rankout[64+d];outputs[(q*H+h+hp)*D+j]=x==0?0:x;}
          return;
        }
        if(token_packed){
          for(int j=0;j<32;j++){float a=rankout[j]+rankout[64+j],b=rankout[32+j]+rankout[96+j],x=a+b;outputs[(q*H+h)*D+j]=x==0?0:x;}
          return;
        }
        for(int j=0;j<64;j++)if(dtile*64+j<int(D)){
          float x=rankout[j]+rankout[64+j];outputs[(q*H+h)*D+dtile*64+j]=x==0?0:x;
        }});
    } else if(phase=="merge") {
      if(++dtile<dtiles)v_macs();else if(bootstrap_head)gather_observer();else finish_head();
    } else if(phase=="stat_move") {
      for(bool v:gathered_valid)if(!v)throw std::runtime_error("Incomplete amax return");
      setphase("stat_reduce");epuservice(std::max(8,(258+scratch_bpc-1)/scratch_bpc),[this]{for(int i=0;i<64;i++){auto v=gathered_max[i];amax_bits=std::max(amax_bits,v);bank_max[token_packed?i%2:i%4]=std::max(bank_max[token_packed?i%2:i%4],v);}lazy_bound_ready=true;});
    } else if(phase=="stat_reduce")publish();
    else if(phase=="bootstrap_header") {
      if(fused()&&header_ok){setphase("bootstrap_budget");epuservice(shared_budget_service(),[this]{prepare_shared_budget();});}else finish_head();
    } else if(phase=="bootstrap_budget")finish_head();
  }
  void actor_tick() {
    if(finished)return;
    commit_routes();commit_observers();
    while(!norm_finishes.empty()&&norm_finishes.front()<=clk){norm_finishes.pop_front();--norm_reserved;}
    if(epu_commit){if(clk<epu_until)return;auto action=std::move(epu_commit);epu_commit={};action();}
    if(tasks.empty()&&outstanding.empty()&&norm_reserved==0&&observer.empty()&&routes.empty())advance();
    while(!tasks.empty()&&outstanding.size()<unsigned(actor_inflight_cap)) {
      Task t=tasks.front();Request req(t.addr,reqtype(t.op),t.id,[this,id=t.id](Request& r){completed(id,r);});
      if((paired()||(token_packed&&token_use_route))&&(t.op==QMac||t.op==VMac)){
        unsigned n=routes.size();for(const auto& kv:outstanding)if(kv.second.op==QMac||kv.second.op==VMac)++n;
        if(n>=unsigned(route_credits))break;
      }
      if(needs_norm(t)&&norm_reserved>=64)break;
      if(observer_request(t)&&observer.size()+outstanding.size()>=64)break;
      t.accepted=clk;
      if(!send(req))break;
      outstanding.emplace(t.id,t);tasks.pop_front();++submitted;
      if(needs_norm(t)){++norm_reserved;norm_peak=std::max(norm_peak,norm_reserved);}
      max_inflight=std::max<uint64_t>(max_inflight,outstanding.size());
    }
  }
  
  struct PortCost {
    uint64_t fma_ops=0,add_ops=0,mul_ops=0,div_ops=0,sqrt_ops=0,exp_ops=0,compare_ops=0,round_ops=0;
    uint64_t scratch_read_bytes=0,scratch_write_bytes=0,metadata_read_bytes=0,metadata_write_bytes=0;
    uint64_t scratch_peak_bytes=0,metadata_live_bytes=0;
  };
  std::string external_policy="none";
  int actor_inflight_cap=64,port_alu_lanes=16,port_layer=2;
  double port_budget_ratio=.5;
  bool port_use_cdf=false,port_key_quantized=false,port_prepared=false,external_warm_start=false;
  bool port_context_setup=false,port_phase_setup=false;
  float port_theta=-.1f,port_cdf=.98f;
  StarcPort::State starc_state;
  SpargePort::State sparge_state;
  SagePort::State sage_state;
  std::vector<int> port_order,port_proposed_order,port_old_inverse,port_remap_groups;
  std::vector<uint8_t> port_shadow_k,port_shadow_v,port_old_shadow_k,port_old_shadow_v;
  std::vector<uint8_t> port_keep_canonical,port_keep_slot,port_pv_keep_slot,external_keep;
  std::vector<int32_t> external_order;
  std::vector<uint16_t> port_tile,port_query_bits,port_remap_k,port_remap_v,port_quant_dram;
  std::vector<uint8_t> port_quant_written;
  
  std::array<std::vector<uint16_t>,2> port_k_dram,port_v_dram;
  std::array<std::vector<uint8_t>,2> port_k_valid,port_v_valid;
  void port_initialize_backing(){
    if(!external())return;
    for(int v=0;v<2;v++){
      port_k_dram[v].assign(uint64_t(nmax)*D,0);port_v_dram[v].assign(uint64_t(nmax)*D,0);
      port_k_valid[v].assign(uint64_t(nmax)*(D/16),0);port_v_valid[v].assign(uint64_t(nmax)*(D/16),0);
    }
    if(cache_init=="existing"){
      for(int t=0;t<int(lengths[0]);t++){
        int canonical=external_warm_start&&external_policy=="starc"?port_order[t]:t;
        for(int d=0;d<int(D);d++){port_k_dram[0][t*D+d]=K[canonical*D+d];port_v_dram[0][t*D+d]=V[canonical*D+d];}
        for(int c=0;c<int(D)/16;c++){port_k_valid[0][t*(D/16)+c]=1;port_v_valid[0][t*(D/16)+c]=1;}
      }
      if(external_warm_start&&external_policy=="sage"){
        for(int t=0;t<int(lengths[0]);t++)for(int d=0;d<int(D);d++)port_k_dram[1][t*D+d]=sage_state.key_bits[t*D+d];
        std::fill_n(port_k_valid[1].begin(),uint64_t(lengths[0])*(D/16),1);
      }
    }
  }
  uint16_t port_dram_word(bool value,int version,int slot,int dim)const{
    if(version<0||version>1||slot<0||slot>=int(nmax)||dim<0||dim>=int(D))throw std::runtime_error("Physical KV image index invalid");
    const auto& valid=value?port_v_valid[version]:port_k_valid[version];
    if(!valid[slot*(D/16)+dim/16])throw std::runtime_error("Physical KV chunk consumed before native publication");
    return (value?port_v_dram[version]:port_k_dram[version])[slot*D+dim];
  }
  void port_publish_append(const Task&t){
    bool value=t.op==AppendV;int version=0;
    const auto& shadows=value?port_shadow_v:port_shadow_k;
    if((value||external_policy!="sage")&&t.x/16<int(shadows.size()))version=shadows[t.x/16];
    Addr_t expected=(value?base_vaddr(t.x,t.y):base_kaddr(t.x,t.y))+(version?PORT_SHADOW:0);
    if(t.addr!=expected)throw std::runtime_error("Physical append image address mismatch");
    auto& image=value?port_v_dram[version]:port_k_dram[version];
    auto& valid=value?port_v_valid[version]:port_k_valid[version];const auto& source=value?V:K;
    for(int j=0;j<16;j++)image[t.x*D+t.y*16+j]=source[t.x*D+t.y*16+j];
    valid[t.x*(D/16)+t.y]=1;
  }
  int port_canonical_slot(int token)const{
    if(external_policy!="starc")return token;
    auto it=std::find(port_order.begin(),port_order.end(),token);
    if(it==port_order.end())throw std::runtime_error("Physical cache lacks requested canonical token");return int(it-port_order.begin());
  }
  std::vector<float> port_mean_sum,port_multiplier;
  std::vector<uint8_t> port_tile_valid;
  uint64_t port_read_commands=0,port_write_commands=0,port_metadata_reads=0,port_metadata_writes=0;
  uint64_t port_remap_read_commands=0,port_remap_write_commands=0,port_key_representation_writes=0;
  uint64_t port_epu_cycles=0,port_max_scratch=0,port_max_metadata=0,port_extra_dram=0;
  uint64_t port_mask_logical_ops=0,port_mask_scratch_bytes=0,port_native_query_start=0;
  uint64_t port_meta_bytes=0,port_meta_offset=0,port_tile_meta_write=0;
  int port_cursor=0,port_end=0,port_pass=0,port_remap_cursor=0,port_valid_tokens=0;
  std::string port_after_compute,port_after_metadata;
  PortCost port_head_cost,port_pending_cost;
  YAML::Node port_cost_records,port_heads;
  static constexpr Addr_t PORT_SHADOW=1ll<<21,PORT_META=1ll<<20;
  bool external()const{return external_policy!="none";}
  bool port_bypass()const{return external_policy=="starc"&&port_layer<2;}
  template<class C> PortCost as_port_cost(const C& c)const {
    PortCost x;
    x.fma_ops=c.fma_ops;x.add_ops=c.add_ops;x.mul_ops=c.mul_ops;x.div_ops=c.div_ops;
    x.sqrt_ops=c.sqrt_ops;x.exp_ops=c.exp_ops;x.compare_ops=c.compare_ops;x.round_ops=c.round_ops;
    x.scratch_read_bytes=c.scratch_read_bytes;x.scratch_write_bytes=c.scratch_write_bytes;
    x.metadata_read_bytes=c.metadata_read_bytes;x.metadata_write_bytes=c.metadata_write_bytes;
    x.scratch_peak_bytes=c.scratch_peak_bytes;x.metadata_live_bytes=c.metadata_live_bytes;return x;
  }
  static uint64_t port_ops(const PortCost& c){return c.fma_ops+c.add_ops+c.mul_ops+c.div_ops+c.sqrt_ops+c.exp_ops+c.compare_ops+c.round_ops;}
  static YAML::Node cost_node(const PortCost& c){
    YAML::Node x;
    x["fma_ops"]=c.fma_ops;x["add_ops"]=c.add_ops;x["mul_ops"]=c.mul_ops;x["div_ops"]=c.div_ops;
    x["sqrt_ops"]=c.sqrt_ops;x["exp_ops"]=c.exp_ops;x["compare_ops"]=c.compare_ops;x["round_ops"]=c.round_ops;
    x["scratch_read_bytes"]=c.scratch_read_bytes;x["scratch_write_bytes"]=c.scratch_write_bytes;
    x["metadata_read_bytes"]=c.metadata_read_bytes;x["metadata_write_bytes"]=c.metadata_write_bytes;
    x["scratch_peak_bytes"]=c.scratch_peak_bytes;x["metadata_live_bytes"]=c.metadata_live_bytes;return x;
  }
  void add_port_cost(const PortCost& c){
    port_head_cost.fma_ops+=c.fma_ops;port_head_cost.add_ops+=c.add_ops;port_head_cost.mul_ops+=c.mul_ops;
    port_head_cost.div_ops+=c.div_ops;port_head_cost.sqrt_ops+=c.sqrt_ops;port_head_cost.exp_ops+=c.exp_ops;
    port_head_cost.compare_ops+=c.compare_ops;port_head_cost.round_ops+=c.round_ops;
    port_head_cost.scratch_read_bytes+=c.scratch_read_bytes;port_head_cost.scratch_write_bytes+=c.scratch_write_bytes;
    port_head_cost.metadata_read_bytes+=c.metadata_read_bytes;port_head_cost.metadata_write_bytes+=c.metadata_write_bytes;
    port_head_cost.scratch_peak_bytes=std::max(port_head_cost.scratch_peak_bytes,c.scratch_peak_bytes);
    port_head_cost.metadata_live_bytes=std::max(port_head_cost.metadata_live_bytes,c.metadata_live_bytes);
    port_max_scratch=std::max(port_max_scratch,c.scratch_peak_bytes);port_max_metadata=std::max(port_max_metadata,c.metadata_live_bytes);
    uint64_t baseline=result["base_peak_live_scratch_bytes"].as<uint64_t>();
    if(baseline+port_max_scratch+port_metadata_fifo_peak_bytes>scratch_limit)throw std::runtime_error("External policy exceeds bounded device scratch");
  }
  void charge_port(const std::string& tag,const PortCost& c,const std::string& next){
    add_port_cost(c);uint64_t a=(port_ops(c)+port_alu_lanes-1)/port_alu_lanes;
    uint64_t b=(c.scratch_read_bytes+c.scratch_write_bytes+scratch_bpc-1)/scratch_bpc;
    uint64_t cycles=std::max(a,b)+(port_ops(c)||b?max_batch_merge_cycles:0);
    YAML::Node row=cost_node(c);row["query"]=q;row["head"]=h;row["stage"]=tag;
    row["alu_lanes"]=port_alu_lanes;row["scratch_bpc"]=scratch_bpc;row["cycles"]=cycles;
    row["start"]=clk;row["end"]=clk+cycles;row["initial_setup"]=port_context_setup;
    port_cost_records.push_back(row);port_epu_cycles+=cycles;
    port_after_compute=next;setphase("port_compute");epuservice(cycles,[]{});
  }
  Addr_t port_meta_addr(uint64_t chunk)const{return PORT_META+stripe_addr(chunk%16)+((chunk/16)%4)*BANK+32ll*(chunk/64);}
  Addr_t port_original_key_addr(int token,int chunk)const {
    if(external_policy=="sage")return base_kaddr(token,chunk);
    int slot=token;
    if(external_policy=="starc"){
      auto it=std::find(port_order.begin(),port_order.end(),token);
      if(it==port_order.end())throw std::runtime_error("Key metadata scan lost canonical token");slot=int(it-port_order.begin());
    }
    return kaddr(slot,chunk);
  }
  float port_loaded_key(int token,int dim)const {
    if(token<port_cursor||token>=port_end||dim<0||dim>=int(D))throw std::runtime_error("Selector accessed an unstreamed Key tile");
    int ci=(token-port_cursor)*(D/16)+dim/16;
    if(!port_tile_valid[ci])throw std::runtime_error("Selector used Key before its native callback");
    return hfloat(port_tile[(token-port_cursor)*D+dim]);
  }
  float port_query(int dim)const{return hfloat(queries[(q*H+h)*D+dim]);}
  void port_warm_bootstrap(){
    if(!external()||!external_warm_start)return;
    if(cache_init!="existing")throw std::runtime_error("Warm metadata is only legal with declared-layout resident cache");
    L=lengths[0];groups=(L+15)/16;
    port_order.resize(L);std::iota(port_order.begin(),port_order.end(),0);
    port_shadow_k.assign(groups,0);port_shadow_v.assign(groups,0);
    
    
    
    auto get=[this](int t,int d){if(t<0||t>=L||d<0||d>=int(D))throw std::runtime_error("Warm image looked past causal prefix");return hfloat(K[t*D+d]);};
    if(external_policy=="starc"){
      StarcPort::Input i;i.L=L;i.D=D;i.q=0;i.h=0;i.H=H;i.layer=port_layer;i.budget=std::max(1,int(std::ceil(port_budget_ratio*L)));
      i.getK=get;i.getQ=[](int){return 0.f;};auto p=StarcPort::step(i,starc_state);port_order=p.order;
    }else if(external_policy=="sparge"){
      SpargePort::Input i;i.L=L;i.D=D;i.q=0;i.h=0;i.H=H;i.config=sparge_config();i.getK=get;i.getQ=[](int){return 0.f;};SpargePort::stage1(i,sparge_state);
    }else {
      SagePort::Input i;i.L=L;i.D=D;i.q=0;i.h=0;i.H=H;i.getK=get;i.getQ=[](int){return 0.f;};SagePort::prepare(i,sage_state);
      port_quant_dram=sage_state.key_bits;port_quant_dram.resize(uint64_t(nmax)*D,0);
      port_quant_written.assign(uint64_t(nmax)*(D/16),0);std::fill_n(port_quant_written.begin(),uint64_t(L)*(D/16),1);port_key_quantized=true;
      port_extra_dram=2ull*nmax*D+2ull*D+4ull*((nmax+63)/64)+4;
    }
    port_encode_metadata();port_meta_dram=port_meta_pending;port_meta_written=port_meta_pending_valid;
    result["warm_initial_state_no_query_data"]=true;result["warm_initial_metadata_bytes"]=port_meta_dram.size();
  }
  StarcPort::Input starc_input()const {
    StarcPort::Input i;i.L=L;i.D=D;i.q=q;i.h=h;i.H=H;i.layer=port_layer;
    i.budget=std::max(1,int(std::ceil(port_budget_ratio*L)));
    i.getK=[this](int t,int d){return port_loaded_key(t,d);};i.getQ=[this](int d){return port_query(d);};return i;
  }
  SpargePort::Config sparge_config()const {
    SpargePort::Config c;c.topk=float(port_budget_ratio);c.use_cdf=port_use_cdf;c.theta=port_theta;c.cdf=port_cdf;return c;
  }
  SpargePort::Input sparge_input()const {
    SpargePort::Input i;i.L=L;i.D=D;i.q=q;i.h=h;i.H=H;i.config=sparge_config();
    i.getK=[this](int t,int d){return port_loaded_key(t,d);};i.getQ=[this](int d){return port_query(d);};return i;
  }
  void port_meta_transfer(bool write,uint64_t bytes,uint64_t offset,const std::string& next){
    port_meta_transfer_segments(write,{{offset,bytes}},next);
  }
  void port_begin_head(){
    port_head_cost={};port_query_bits.clear();port_multiplier.clear();port_valid_tokens=L;
    if(port_order.size()>unsigned(L))throw std::runtime_error("External cache length regressed");
    for(int t=int(port_order.size());t<L;t++)port_order.push_back(t);
    port_shadow_k.resize(groups,0);port_shadow_v.resize(groups,0);
    port_keep_canonical.assign(L,1);port_keep_slot.assign(L,1);port_pv_keep_slot.assign(L,1);
    if(external_policy=="starc"&&port_layer<2){port_select();return;}
    bool build=false;port_pass=0;
    if(external_policy=="starc"){
      if(starc_state.lastq<0){starc_state.D=D;port_cursor=0;build=true;}
      else if(h==0&&L-starc_state.clustered>=64){port_cursor=starc_state.clustered;build=true;}
    }else if(external_policy=="sparge"){
      if(sparge_state.D!=int(D)||sparge_state.L>L||sparge_state.blocks.empty()){sparge_state={};sparge_state.D=D;port_cursor=0;build=true;}
      else if(sparge_state.L!=L){port_cursor=(sparge_state.L/64)*64;build=true;}
      sparge_state.blocks.resize((L+63)/64);
    }else if(external_policy=="sage"&&h==0&&!(external_warm_start&&q==0)){
      port_cursor=0;build=true;port_mean_sum.assign(D,0);sage_state.key_bits.assign(uint64_t(L)*D,0);
      sage_state.mean_bits.assign(D,0);sage_state.key_scale.assign((L+63)/64,0);
      std::fill(port_quant_written.begin(),port_quant_written.end(),0);
    }
    if(build)port_read_tile();else port_read_metadata();
  }
  void port_read_tile(){
    if(port_cursor>=L){port_finish_preparation();return;}
    port_context_setup=q==0&&h==0;
    port_end=std::min(L,port_cursor+64);port_tile.assign((port_end-port_cursor)*D,0);
    port_tile_valid.assign((port_end-port_cursor)*(D/16),0);setphase("port_key_read");
    for(int t=port_cursor;t<port_end;t++)for(int c=0;c<int(D)/16;c++)enqueue(PortKeyRead,port_original_key_addr(t,c),t,c);
  }
  void port_process_tile(){
    for(auto x:port_tile_valid)if(!x)throw std::runtime_error("Incomplete external Key tile");
    PortCost c;port_tile_meta_write=0;
    if(external_policy=="starc"){
      StarcPort::Plan p;StarcPort::cluster_window(starc_input(),starc_state,p,port_cursor,port_end);c=as_port_cost(p);
      c.scratch_peak_bytes+=2ull*(port_end-port_cursor)*D;port_tile_meta_write=c.metadata_write_bytes;
    }else if(external_policy=="sparge"){
      SpargePort::Plan p;sparge_state.blocks[port_cursor/64]=SpargePort::summarize(sparge_input(),port_cursor,port_end,p);
      c=as_port_cost(p);c.scratch_peak_bytes+=2ull*(port_end-port_cursor)*D;
      c.metadata_write_bytes=2ull*D+8;port_tile_meta_write=c.metadata_write_bytes;
    }else if(port_pass==0){
      uint64_t e=uint64_t(port_end-port_cursor)*D;
      for(int t=port_cursor;t<port_end;t++)for(int d=0;d<int(D);d++)port_mean_sum[d]+=port_loaded_key(t,d);
      c.add_ops=e;c.scratch_read_bytes=4*e;c.scratch_write_bytes=4*e;c.scratch_peak_bytes=2*e+4*D;
    }else {
      uint64_t e=uint64_t(port_end-port_cursor)*D;std::vector<uint16_t> centered(e);float amax=0;
      for(int t=port_cursor;t<port_end;t++)for(int d=0;d<int(D);d++){
        auto v=SagePort::half_bits(port_loaded_key(t,d)-hfloat(sage_state.mean_bits[d]));
        if(!std::isfinite(hfloat(v)))throw std::runtime_error("Nonfinite centered Sage producer operand");
        centered[(t-port_cursor)*D+d]=v;amax=std::max(amax,std::abs(hfloat(v)));
      }
      float scale=amax/127.f;sage_state.key_scale[port_cursor/64]=scale;
      for(int t=port_cursor;t<port_end;t++)for(int d=0;d<int(D);d++)
        sage_state.key_bits[t*D+d]=SagePort::half_bits(float(SagePort::quantize(hfloat(centered[(t-port_cursor)*D+d]),scale)));
      c.add_ops=2*e;c.div_ops=e+1;c.compare_ops=3*e;c.round_ops=2*e;
      c.scratch_read_bytes=4*e;c.scratch_write_bytes=2*e;c.scratch_peak_bytes=4*e+2*D+64;
      c.metadata_write_bytes=4;port_tile_meta_write=4;
    }
    charge_port("producer_tile",c,"producer_tile");
  }
  void port_finish_preparation(){
    if(external_policy=="sage"&&port_pass==0){
      for(int d=0;d<int(D);d++)sage_state.mean_bits[d]=SagePort::half_bits(port_mean_sum[d]/float(L));
      PortCost c;c.div_ops=D;c.round_ops=D;c.scratch_read_bytes=4*D;c.scratch_write_bytes=2*D;c.metadata_write_bytes=2*D;c.scratch_peak_bytes=6*D;
      charge_port("key_mean_finalize",c,"sage_mean");return;
    }
    if(external_policy=="starc"){
      starc_state.clustered=L;if(starc_state.lastq<0){starc_state.lastq=q;starc_state.lastL=L;}
      port_context_setup=q==0&&h==0;port_meta_transfer(true,64,starc_descriptor_offset(),"read_query_metadata");return;
    }
    if(external_policy=="sparge"){
      sparge_state.L=L;port_meta_transfer(true,32,0,"read_query_metadata");return;
    }
    sage_state.query=q;sage_state.length=L;sage_state.dimension=D;port_key_quantized=true;
    port_extra_dram=std::max<uint64_t>(port_extra_dram,2ull*nmax*D+2ull*D+4ull*((nmax+63)/64)+4);
    port_read_metadata();
  }
  void port_read_metadata(){
    port_context_setup=false;
    if(q==0&&h==0){
      writevec(directory/"initial_metadata.bin",port_meta_dram);
      if(external_policy=="sage")writevec(directory/"initial_quantized_key.f16",port_quant_dram);
    }
    uint64_t bytes=0;
    if(external_policy=="starc"){
      port_meta_transfer_segments(false,{{0,4ull*starc_state.centers.size()},
        {starc_labels_offset(),4ull*starc_state.clustered},{starc_order_offset(),4ull*starc_state.clustered}},"select");return;
    }
    else if(external_policy=="sparge")bytes=32ull+((L+63)/64)*(2ull*D+8);
    else bytes=4ull*((L+63)/64);
    port_meta_transfer(false,bytes,external_policy=="sage"?2ull*D:0,"select");
  }
  void port_select(){
    port_context_setup=false;
    if(!(external_policy=="starc"&&port_layer<2))port_restore_metadata();
    PortCost c;std::vector<int> order;std::vector<uint8_t> keep;
    if(external_policy=="starc"){
      auto p=StarcPort::step(starc_input(),starc_state);if(p.key_read_end!=p.key_read_begin)throw std::runtime_error("STARC tried to bypass native preprocessing reads");
      c=as_port_cost(p);if(!p.layer_bypass)c.metadata_read_bytes-=8ull*(L-starc_state.clustered);order=p.order;keep=p.keep;
    }else if(external_policy=="sparge"){
      auto p=SpargePort::stage1(sparge_input(),sparge_state);if(p.key_read_end!=p.key_read_begin)throw std::runtime_error("Sparge tried to bypass native preprocessing reads");
      c=as_port_cost(p);keep=p.keep;
      result["sparge_prediction_nonfinite_fallback_heads"]=result["sparge_prediction_nonfinite_fallback_heads"].as<uint64_t>(0)+uint64_t(p.prediction_nonfinite_fallback);
    }else {
      SagePort::Input in;in.L=L;in.D=D;in.q=q;in.h=h;in.H=H;in.keys_prepared=true;
      in.getQ=[this](int d){return port_query(d);};auto p=SagePort::prepare(in,sage_state);
      if(p.rebuild_keys)throw std::runtime_error("Sage tried to bypass streaming native producer");
      c=as_port_cost(p);port_query_bits=p.query_bits;port_multiplier=p.score_multiplier;keep.assign(L,1);port_metadata_query_scale=p.query_scale;
      c.mul_ops-=L; 
      port_extra_dram=std::max<uint64_t>(port_extra_dram,p.extra_dram_bytes);
    }
    if(order.empty()){order.resize(L);std::iota(order.begin(),order.end(),0);}
    if(int(order.size())!=L||int(keep.size())!=L)throw std::runtime_error("Wrong external selector shape");
    std::vector<uint8_t> seen(L,0);for(int x:order)if(x<0||x>=L||seen[x]++)throw std::runtime_error("Invalid physical Key permutation");
    port_proposed_order=std::move(order);port_keep_canonical=std::move(keep);
    
    
    c.scratch_peak_bytes+=c.metadata_live_bytes;
    c.scratch_write_bytes+=c.metadata_read_bytes;
    port_extra_dram=std::max<uint64_t>(port_extra_dram,c.metadata_live_bytes);
    port_valid_tokens=std::count(port_keep_canonical.begin(),port_keep_canonical.end(),uint8_t(1));
    if(!port_valid_tokens)throw std::runtime_error("External selector omitted every token");
    port_pending_cost=c;
    if(!port_ops(c)&&!c.scratch_read_bytes&&!c.scratch_write_bytes&&!c.metadata_write_bytes){port_begin_remap();return;}
    charge_port("query_selection",c,"selection");
  }
  void port_begin_remap(){
    port_remap_groups.clear();port_remap_cursor=0;
    if(external_policy=="starc")for(int g=0;g<groups;g++){
      bool changed=false;for(int t=g*16;t<std::min(L,(g+1)*16);t++)changed|=port_order[t]!=port_proposed_order[t];
      if(changed)port_remap_groups.push_back(g);
    }
    if(token_packed&&!port_remap_groups.empty()){
      std::vector<uint8_t> marked(groups,0);for(int g:port_remap_groups){marked[g]=1;if((g^1)<groups)marked[g^1]=1;}
      port_remap_groups.clear();for(int g=0;g<groups;g++)if(marked[g])port_remap_groups.push_back(g);
    }
    port_old_inverse.resize(L);for(int t=0;t<L;t++)port_old_inverse[port_order[t]]=t;
    port_old_shadow_k=port_shadow_k;port_old_shadow_v=port_shadow_v;
    if(port_remap_groups.empty()){port_commit_selection();return;}
    port_extra_dram=std::max<uint64_t>(port_extra_dram,4ull*nmax*D+port_max_metadata);
    port_remap_read_group();
  }
  Addr_t port_remap_source(int slot,int chunk,bool value)const{
    int old=port_old_inverse[port_proposed_order[slot]];
    
    Addr_t a=value?base_vaddr(old,chunk):base_kaddr(old,chunk);
    return a+((value?port_old_shadow_v[old/16]:port_old_shadow_k[old/16])?PORT_SHADOW:0);
  }
  void port_remap_read_group(){
    if(port_remap_cursor>=int(port_remap_groups.size())){port_commit_selection();return;}
    port_context_setup=q==0&&h==0;
    int g=port_remap_groups[port_remap_cursor];port_cursor=g*16;port_end=std::min(L,port_cursor+16);
    port_remap_k.assign((port_end-port_cursor)*D,0);port_remap_v=port_remap_k;setphase("port_remap_read");
    for(int t=port_cursor;t<port_end;t++)for(int c=0;c<int(D)/16;c++){
      enqueue(PortRemapKRead,port_remap_source(t,c,false),t,c);enqueue(PortRemapVRead,port_remap_source(t,c,true),t,c);
    }
  }
  void port_remap_write_group(){
    setphase("port_remap_write");int g=port_remap_groups[port_remap_cursor];
    for(int t=port_cursor;t<port_end;t++)for(int c=0;c<int(D)/16;c++){
      enqueue(PortRemapKWrite,base_kaddr(t,c)+(port_old_shadow_k[g]?0:PORT_SHADOW),t,c);
      enqueue(PortRemapVWrite,base_vaddr(t,c)+(port_old_shadow_v[g]?0:PORT_SHADOW),t,c);
    }
  }
  void port_commit_selection(){
    for(int g:port_remap_groups){port_shadow_k[g]^=1;port_shadow_v[g]^=1;}
    port_order=port_proposed_order;
    if(q==0&&h==0)writevec(directory/"initial_order.i32",port_order);
    for(int t=0;t<L;t++){port_keep_slot[t]=port_keep_canonical[port_order[t]];port_pv_keep_slot[t]=port_keep_slot[t];}
    if(stream_max_fusion)audit_score_coverage.assign(L,false);
    std::fill(scores.begin(),scores.end(),-std::numeric_limits<float>::infinity());
    size_t off=(q*H+h)*nmax;
    for(int t=0;t<L;t++){external_keep[off+t]=port_keep_canonical[t];external_order[off+t]=port_order[t];}
    YAML::Node row=cost_node(port_head_cost);row["query"]=q;row["head"]=h;row["length"]=L;
    row["selected_tokens"]=port_valid_tokens;row["external_policy"]=external_policy;row["metadata_generation"]=generation;row["remapped_groups"]=port_remap_groups.size();
    row["warm_start"]=external_warm_start;port_heads.push_back(row);port_prepared=true;port_context_setup=false;
    if(q==0&&h==0)port_native_query_start=clk;
    if(token_packed){setphase("token_dispatch");epuservice(pair_dispatch,[this]{issue_q_writes();});}else issue_q_writes();
  }
  bool port_active_group(int g)const{
    int first=(token_packed?2*g:g)*16,end=std::min(L,first+(token_packed?32:16));
    for(int t=first;t<end;t++)if(port_keep_slot[t])return true;return false;
  }
  void port_stage2(){
    std::vector<float> scaled(L);float scale=1.f/std::sqrt(float(D));for(int t=0;t<L;t++)scaled[t]=scores[t]*scale;
    auto p=SpargePort::stage2(scaled,port_keep_slot,sparge_config());port_pv_keep_slot=p.keep;
    p.mul_ops+=L;p.scratch_read_bytes+=4ull*L;p.scratch_write_bytes+=4ull*L;p.scratch_peak_bytes+=4ull*L;
    charge_port("softmax_aware_filter",as_port_cost(p),"stage2");
  }
  void port_build_masks(){
    total_mass=0;transport_frames.assign(frame_count(),false);
    if(port_bypass()){
      std::fill(skip.begin(),skip.end(),false);std::fill(masses.begin(),masses.end(),0);
      std::fill(transport_frames.begin(),transport_frames.end(),true);return;
    }
    for(int g=0;g<groups;g++){
      bool any=false;uint32_t m=0;
      for(int t=g*16;t<std::min(L,(g+1)*16);t++){any|=port_pv_keep_slot[t];m+=punits(prob[t]);}
      skip[g]=!any;masses[g]=m;total_mass+=m;if(any)transport_frames[frame_index(g)]=true;
    }
    
    
    port_mask_logical_ops+=3ull*L+2ull*groups;
    port_mask_scratch_bytes+=2ull*L+((groups+7)/8)+((frame_count()+7)/8);
  }
  uint64_t port_dequant_work(){
    int first=(token_packed?2:1)*batch*16;
    int end=std::min(L,first+(token_packed?2:1)*16*16);
    uint64_t tokens=end-first;PortCost c;c.mul_ops=tokens;add_port_cost(c);
    uint64_t cycles=(tokens+port_alu_lanes-1)/port_alu_lanes;
    YAML::Node row=cost_node(c);row["query"]=q;row["head"]=h;row["stage"]="post_qk_dequant";
    row["cycles"]=cycles;row["start"]=clk;row["end"]=clk+cycles;row["initial_setup"]=false;
    port_cost_records.push_back(row);port_epu_cycles+=cycles;return cycles;
  }
  void port_softmax_start(){setphase("softmax");epuservice(softmax_service(),[this]{softmax();});}
  void port_after_epu(){
    if(port_after_compute=="producer_tile"){
      if(external_policy=="sage"&&port_pass==1){
        setphase("port_quant_write");for(int t=port_cursor;t<port_end;t++)for(int c=0;c<int(D)/16;c++)enqueue(PortQuantWrite,base_kaddr(t,c)+PORT_SHADOW,t,c);
      }else if(port_tile_meta_write){
        if(external_policy=="starc"){
          int C=(port_end-port_cursor+15)/16;
          port_meta_transfer_segments(true,{{4ull*(starc_state.centers.size()-uint64_t(C)*D),4ull*C*D},
            {starc_labels_offset()+4ull*port_cursor,4ull*(port_end-port_cursor)},
            {starc_order_offset()+4ull*port_cursor,4ull*(port_end-port_cursor)}},"next_tile");
        }else port_meta_transfer(true,port_tile_meta_write,32ull+uint64_t(port_cursor/64)*(2ull*D+8),"next_tile");
      }
      else {port_cursor=port_end;port_read_tile();}
    }else if(port_after_compute=="sage_mean")port_meta_transfer(true,2ull*D,0,"sage_second_pass");
    else if(port_after_compute=="selection"){
      if(port_pending_cost.metadata_write_bytes){
        if(external_policy=="starc")port_meta_transfer_segments(true,{{starc_labels_offset()+4ull*(L-1),4},{starc_order_offset()+4ull*(L-1),4}},"begin_remap");
        else if(external_policy=="sage")port_meta_transfer(true,4,2ull*D+4ull*((L+63)/64),"begin_remap");
        else port_meta_transfer(true,port_pending_cost.metadata_write_bytes,0,"begin_remap");
      }else port_begin_remap();
    }else if(port_after_compute=="remap_copy")port_remap_write_group();
    else if(port_after_compute=="stage2")port_softmax_start();
    else if(port_after_compute=="metadata_dispatch")port_dispatch_metadata_packets();
    else if(port_after_compute=="metadata_merge_dispatch")port_dispatch_metadata_merges();
    else throw std::runtime_error("Unknown external EPU continuation");
  }
  void port_after_meta(){
    if(port_after_metadata=="next_tile"){port_cursor=port_end;port_read_tile();}
    else if(port_after_metadata=="sage_second_pass"){port_pass=1;port_cursor=0;port_read_tile();}
    else if(port_after_metadata=="read_query_metadata")port_read_metadata();
    else if(port_after_metadata=="select")port_select();
    else if(port_after_metadata=="begin_remap")port_begin_remap();
    else throw std::runtime_error("Unknown external metadata continuation");
  }

  
  
  std::vector<uint8_t> port_meta_dram,port_meta_pending,port_meta_cache;
  std::vector<uint8_t> port_meta_written,port_meta_loaded,port_meta_pending_valid;
  std::vector<uint8_t> port_meta_write_required,port_meta_read_required;
  std::map<uint64_t,std::array<uint8_t,32>> port_meta_packet_payload,port_meta_packet_valid;
  std::vector<uint64_t> port_meta_dispatch_packets,port_meta_rmw_ready;
  float port_metadata_query_scale=0;
  uint64_t port_metadata_rmw_reads=0,port_metadata_restore_count=0;
  uint64_t port_metadata_fifo_peak_bytes=0;
  uint64_t port_starc_label_offset()const{return 4ull*((nmax+15)/16)*D;}
  uint64_t port_starc_order_offset()const{return port_starc_label_offset()+4ull*nmax;}
  uint64_t starc_labels_offset()const{return port_starc_label_offset();}
  uint64_t starc_order_offset()const{return port_starc_order_offset();}
  uint64_t starc_descriptor_offset()const{return port_starc_order_offset()+4ull*nmax;}
  uint64_t port_sparge_record_offset(int g)const{return 32ull+uint64_t(g)*(2ull*D+8);}
  uint64_t port_sage_scale_offset()const{return 2ull*D;}
  uint64_t port_sage_query_scale_offset()const{return 2ull*D+4ull*((L+63)/64);}
  uint64_t port_metadata_capacity()const{
    uint64_t n=external_policy=="starc"?starc_descriptor_offset()+64:
      external_policy=="sparge"?32ull+((nmax+63)/64)*(2ull*D+8):2ull*D+4ull*((nmax+63)/64)+4;
    return ((n+31)/32)*32;
  }
  void port_metadata_allocate(){
    const uint64_t n=port_metadata_capacity();
    if(port_meta_dram.size()>n)throw std::runtime_error("Metadata allocation shrank across a live owner");
    port_meta_dram.resize(n,0);port_meta_written.resize(n,0);
    port_meta_cache.resize(n,0);port_meta_loaded.resize(n,0);
  }
  void port_meta_store_u16(uint64_t o,uint16_t v){
    if(o+2>port_meta_pending.size())throw std::runtime_error("Metadata uint16 exceeds allocation");
    for(int j=0;j<2;++j){port_meta_pending[o+j]=uint8_t(v>>(8*j));port_meta_pending_valid[o+j]=1;}
  }
  void port_meta_store_u32(uint64_t o,uint32_t v){
    if(o+4>port_meta_pending.size())throw std::runtime_error("Metadata uint32 exceeds allocation");
    for(int j=0;j<4;++j){port_meta_pending[o+j]=uint8_t(v>>(8*j));port_meta_pending_valid[o+j]=1;}
  }
  uint16_t port_meta_load_u16(uint64_t o)const{
    if(o+2>port_meta_cache.size())throw std::runtime_error("Metadata load exceeds allocation");
    uint16_t v=0;for(int j=0;j<2;++j){if(!port_meta_loaded[o+j])throw std::runtime_error("Metadata used before valid native read");v|=uint16_t(port_meta_cache[o+j])<<(8*j);}return v;
  }
  uint32_t port_meta_load_u32(uint64_t o)const{
    if(o+4>port_meta_cache.size())throw std::runtime_error("Metadata load exceeds allocation");
    uint32_t v=0;for(int j=0;j<4;++j){if(!port_meta_loaded[o+j])throw std::runtime_error("Metadata used before valid native read");v|=uint32_t(port_meta_cache[o+j])<<(8*j);}return v;
  }
  void port_encode_metadata(){
    port_metadata_allocate();port_meta_pending.assign(port_meta_dram.size(),0);port_meta_pending_valid.assign(port_meta_dram.size(),0);
    if(external_policy=="starc"){
      if(starc_state.centers.size()>uint64_t((nmax+15)/16)*D||starc_state.labels.size()>nmax||starc_state.order.size()>nmax)throw std::runtime_error("STARC metadata shape overflow");
      for(uint64_t i=0;i<starc_state.centers.size();++i)port_meta_store_u32(4*i,std::bit_cast<uint32_t>(starc_state.centers[i]));
      for(uint64_t i=0;i<starc_state.labels.size();++i)port_meta_store_u32(port_starc_label_offset()+4*i,uint32_t(int32_t(starc_state.labels[i])));
      for(uint64_t i=0;i<starc_state.order.size();++i)port_meta_store_u32(port_starc_order_offset()+4*i,uint32_t(int32_t(starc_state.order[i])));
      
      
      for(uint64_t i=starc_state.labels.size();i<L;++i)port_meta_store_u32(port_starc_label_offset()+4*i,uint32_t(int32_t(-1)));
      for(uint64_t i=starc_state.order.size();i<L;++i)port_meta_store_u32(port_starc_order_offset()+4*i,uint32_t(i));
      const uint32_t descriptor[16]={0x53544131u,uint32_t(D),uint32_t(nmax),uint32_t(starc_state.clustered),uint32_t(starc_state.lastL),uint32_t(starc_state.lastq),uint32_t(starc_state.centers.size()/D),uint32_t(generation),uint32_t(generation>>32),0,0,0,0,0,0,0};
      for(int i=0;i<16;++i)port_meta_store_u32(starc_descriptor_offset()+4*i,descriptor[i]);
    }else if(external_policy=="sparge"){
      const uint32_t header[8]={0x53504731u,uint32_t(D),uint32_t(sparge_state.L),uint32_t((L+63)/64),64u,uint32_t(q),uint32_t(generation),uint32_t(generation>>32)};
      for(int i=0;i<8;++i)port_meta_store_u32(4*i,header[i]);
      for(uint64_t g=0;g<sparge_state.blocks.size();++g){const auto& a=sparge_state.blocks[g];
        if(a.size==0)continue;
        if(a.mean.size()!=D||a.size<1||a.size>64)throw std::runtime_error("Invalid produced Sparge metadata row");
        const uint64_t o=port_sparge_record_offset(int(g));
        for(uint64_t d=0;d<D;++d)port_meta_store_u16(o+2*d,hbits(a.mean[d]));
        port_meta_store_u32(o+2*D,std::bit_cast<uint32_t>(a.similarity));
        port_meta_store_u32(o+2*D+4,(uint32_t(a.size)<<1)|uint32_t(a.eligible));
      }
    }else if(external_policy=="sage"){
      if(sage_state.mean_bits.size()!=D||sage_state.key_scale.size()>uint64_t((nmax+63)/64))throw std::runtime_error("Sage metadata shape overflow");
      for(uint64_t d=0;d<D;++d)port_meta_store_u16(2*d,sage_state.mean_bits[d]);
      for(uint64_t g=0;g<sage_state.key_scale.size();++g)port_meta_store_u32(port_sage_scale_offset()+4*g,std::bit_cast<uint32_t>(sage_state.key_scale[g]));
      port_meta_store_u32(port_sage_query_scale_offset(),std::bit_cast<uint32_t>(port_metadata_query_scale));
    }else throw std::runtime_error("Unknown metadata owner");
  }
  void port_meta_transfer_segments(bool write,const std::vector<std::pair<uint64_t,uint64_t>>&segments,const std::string&next){
    port_metadata_allocate();if(write)port_encode_metadata();
    const uint64_t n=port_meta_dram.size();port_after_metadata=next;
    port_meta_write_required.assign(n,0);port_meta_read_required.assign(n,0);
    port_meta_packet_payload.clear();port_meta_packet_valid.clear();
    if(!write)std::fill(port_meta_loaded.begin(),port_meta_loaded.end(),0);
    std::vector<uint64_t> packets;
    for(auto [o,len]:segments){
      if(o>n||len>n-o)throw std::runtime_error("Metadata segment exceeds allocated image");
      for(uint64_t a=o;a<o+len;++a){
        if(write){if(!port_meta_pending_valid[a])throw std::runtime_error("Attempt to publish unproduced metadata byte");port_meta_write_required[a]=1;}
        else port_meta_read_required[a]=1;
      }
      if(len)for(uint64_t a=(o/32)*32;a<o+len;a+=32)packets.push_back(a);
    }
    std::sort(packets.begin(),packets.end());packets.erase(std::unique(packets.begin(),packets.end()),packets.end());
    port_meta_rmw_ready.clear();port_meta_dispatch_packets=packets;
    if(!write){
      setphase("port_meta_read");
      for(uint64_t o:packets)enqueue(PortMetaRead,port_meta_addr(o/32),int(o),0);
      if(packets.empty())port_after_meta();return;
    }
    if(packets.empty()){port_after_meta();return;}
    
    
    
    const uint64_t fifo=32ull*packets.size();
    port_metadata_fifo_peak_bytes=std::max(port_metadata_fifo_peak_bytes,fifo);
    for(uint64_t o:packets){
      bool full=true;for(int j=0;j<32;++j)full=full&&port_meta_write_required[o+j];
      if(full){
        std::array<uint8_t,32> data{},valid{};
        for(int j=0;j<32;++j){data[j]=port_meta_pending[o+j];valid[j]=1;}
        port_meta_packet_payload[o]=data;port_meta_packet_valid[o]=valid;
      }
    }
    PortCost c;c.scratch_read_bytes=fifo;c.scratch_write_bytes=fifo;c.scratch_peak_bytes=fifo;
    charge_port("metadata_packet_assembly",c,"metadata_dispatch");
  }
  void port_dispatch_metadata_packets(){
    setphase("port_meta_write");
    for(uint64_t o:port_meta_dispatch_packets){
      if(port_meta_packet_payload.count(o))enqueue(PortMetaWrite,port_meta_addr(o/32),int(o),0);
      else{
        ++port_metadata_rmw_reads;enqueue(PortMetaRead,port_meta_addr(o/32),int(o),1);
      }
    }
  }
  bool port_metadata_merges_ready()const{return !port_meta_rmw_ready.empty();}
  void port_service_metadata_merges(){
    if(port_meta_rmw_ready.empty())throw std::runtime_error("Metadata merge service lacks completed read packets");
    
    
    
    PortCost c;const uint64_t n=port_meta_rmw_ready.size();
    c.compare_ops=32*n;c.scratch_read_bytes=64*n;c.scratch_write_bytes=32*n;
    c.scratch_peak_bytes=32ull*port_meta_dispatch_packets.size();
    charge_port("metadata_packet_merge",c,"metadata_merge_dispatch");
  }
  void port_dispatch_metadata_merges(){
    if(port_meta_rmw_ready.empty())throw std::runtime_error("Metadata merge dispatch lacks serviced packets");
    setphase("port_meta_write");
    for(uint64_t o:port_meta_rmw_ready)enqueue(PortMetaWrite,port_meta_addr(o/32),int(o),1);
    port_meta_rmw_ready.clear();
  }
  void port_complete_metadata(bool write,const Task&t){
    if(t.x<0||t.x%32)throw std::runtime_error("Unaligned native metadata packet");
    const uint64_t o=uint64_t(t.x);
    if(o+32>port_meta_dram.size()||t.addr!=port_meta_addr(o/32))throw std::runtime_error("Wrong native metadata address");
    if(write){
      auto d=port_meta_packet_payload.find(o),v=port_meta_packet_valid.find(o);
      if(d==port_meta_packet_payload.end()||v==port_meta_packet_valid.end())throw std::runtime_error("Metadata write lacks a produced packet");
      for(int j=0;j<32;++j){port_meta_dram[o+j]=d->second[j];port_meta_written[o+j]=v->second[j];}
      port_meta_packet_payload.erase(d);port_meta_packet_valid.erase(v);
    }else if(t.y==1){
      std::array<uint8_t,32> data{},valid{};
      for(int j=0;j<32;++j){
        uint64_t a=o+j;data[j]=port_meta_dram[a];valid[j]=port_meta_written[a];
        if(port_meta_write_required[a]){if(!port_meta_pending_valid[a])throw std::runtime_error("RMW publishes an unproduced byte");data[j]=port_meta_pending[a];valid[j]=1;}
      }
      port_meta_packet_payload[o]=data;port_meta_packet_valid[o]=valid;
      
      
      port_meta_rmw_ready.push_back(o);
    }else{
      for(int j=0;j<32;++j){
        uint64_t a=o+j;if(port_meta_read_required[a]&&!port_meta_written[a])throw std::runtime_error("Native read of uninitialized required metadata");
        port_meta_cache[a]=port_meta_dram[a];port_meta_loaded[a]=port_meta_written[a];
      }
    }
  }
  void port_restore_metadata(){
    if(external_policy=="starc"){
      const uint64_t C=starc_state.centers.size()/D;
      if(starc_state.centers.size()%D||starc_state.clustered<0||starc_state.clustered>int(L))throw std::runtime_error("STARC controller metadata shape mismatch");
      for(uint64_t i=0;i<C*D;++i)starc_state.centers[i]=std::bit_cast<float>(port_meta_load_u32(4*i));
      starc_state.labels.resize(starc_state.clustered);starc_state.order.resize(starc_state.clustered);
      std::vector<uint8_t>seen(starc_state.clustered,0);
      for(int i=0;i<starc_state.clustered;++i){
        int32_t label=std::bit_cast<int32_t>(port_meta_load_u32(port_starc_label_offset()+4ull*i));
        int32_t token=std::bit_cast<int32_t>(port_meta_load_u32(port_starc_order_offset()+4ull*i));
        if(label<0||uint64_t(label)>=C||token<0||token>=starc_state.clustered||seen[token]++)throw std::runtime_error("STARC metadata is not a valid cluster/order image");
        starc_state.labels[i]=label;starc_state.order[i]=token;
      }
    }else if(external_policy=="sparge"){
      const int G=(L+63)/64;
      if(port_meta_load_u32(0)!=0x53504731u||port_meta_load_u32(4)!=D||port_meta_load_u32(8)!=L||port_meta_load_u32(12)!=uint32_t(G)||port_meta_load_u32(16)!=64u||port_meta_load_u32(20)!=uint32_t(q)||port_meta_load_u32(24)!=uint32_t(generation)||port_meta_load_u32(28)!=uint32_t(generation>>32))throw std::runtime_error("Sparge metadata owner/length/generation mismatch");
      sparge_state.D=D;sparge_state.L=L;sparge_state.blocks.resize(G);
      for(int g=0;g<G;++g){auto&a=sparge_state.blocks[g];uint64_t o=port_sparge_record_offset(g);a.mean.resize(D);
        for(uint64_t d=0;d<D;++d)a.mean[d]=hfloat(port_meta_load_u16(o+2*d));
        a.similarity=std::bit_cast<float>(port_meta_load_u32(o+2*D));uint32_t flag=port_meta_load_u32(o+2*D+4);
        a.eligible=flag&1u;a.size=flag>>1;
        if(a.size!=std::min(64,int(L)-64*g)||!std::isfinite(a.similarity))throw std::runtime_error("Invalid readback Sparge summary");
      }
    }else if(external_policy=="sage"){
      const int G=(L+63)/64;sage_state.key_scale.resize(G);
      for(int g=0;g<G;++g){float x=std::bit_cast<float>(port_meta_load_u32(port_sage_scale_offset()+4ull*g));if(!std::isfinite(x)||x<0)throw std::runtime_error("Invalid readback Sage scale");sage_state.key_scale[g]=x;}
    }else throw std::runtime_error("Unknown metadata restore owner");
    ++port_metadata_restore_count;
  }

  void dump() {
    events.flush();events.close();norm_events.flush();norm_events.close();observer_events.flush();observer_events.close();resident_events.flush();resident_events.close();
    result["external_policy"]=external_policy;result["actor_inflight_cap"]=actor_inflight_cap;
    result["external_warm_start"]=external_warm_start;result["port_first_head_qwrite_cycle"]=port_native_query_start;
    uint64_t setup=0;for(auto p:phase_records)if(p["port_initial_setup"].as<bool>(false))setup+=p["cycles"].as<uint64_t>();
    result["port_initial_setup_cycles"]=setup;result["port_metadata_rmw_reads"]=port_metadata_rmw_reads;
    result["port_metadata_restore_count"]=port_metadata_restore_count;
    result["port_metadata_fifo_peak_bytes"]=port_metadata_fifo_peak_bytes;
    result["port_mask_logical_ops"]=port_mask_logical_ops;result["port_mask_scratch_bytes"]=port_mask_scratch_bytes;
    result["port_costs"]=port_cost_records;result["ext_heads"]=port_heads;result["port_alu_lanes"]=port_alu_lanes;
    result["port_key_reads"]=port_read_commands;result["port_writes"]=port_write_commands;
    result["port_metadata_read_commands"]=port_metadata_reads;result["port_metadata_write_commands"]=port_metadata_writes;
    result["port_remap_read_commands"]=port_remap_read_commands;result["port_remap_write_commands"]=port_remap_write_commands;
    result["port_key_representation_writes"]=port_key_representation_writes;result["port_epu_cycles"]=port_epu_cycles;
    result["port_peak_scratch_bytes"]=port_max_scratch+port_metadata_fifo_peak_bytes;result["port_peak_metadata_bytes"]=port_max_metadata;result["port_extra_dram_bytes"]=port_extra_dram;
    result["peak_live_scratch_bytes"]=result["base_peak_live_scratch_bytes"].as<uint64_t>()+port_max_scratch+port_metadata_fifo_peak_bytes;
    if(external()){writevec(directory/"external_keep.u8",external_keep);writevec(directory/"external_order.i32",external_order);}
    route_events.flush();route_events.close();
    result["migration_reads"]=migration_reads;result["migration_writes"]=migration_writes;result["migration_epu_cycles"]=migration_epu_cycles;
    result["migration_buffer_bytes"]=(token_packed&&cache_init=="migrate")?8192:0;
    result["token_use_route"]=token_use_route;result["packed_operand_data_path"]=token_use_route?"explicit_2048B_route":"native_bank_local";
    result["native_mac_operand_bytes"]=2048*(count_qmac+count_vmac);
    result["token_packed"]=token_packed;result["token_packed_disabled_reason"]=token_disabled_reason;
    result["token_qk_mac"]=token_qmac;result["token_value_mac"]=token_vmac;
    result["requested_mode"]=requested_mode;result["pair_enabled"]=pair_enabled;result["pair_disabled_reason"]=pair_disabled_reason;
    result["pair_batches"]=pair_batches;result["paired_qk_mac"]=paired_qmac;result["paired_value_mac"]=paired_vmac;
    result["route_jobs_peak"]=route_peak;result["route_buffer_bytes"]=pair_enabled?route_credits*1024:(token_packed&&token_use_route?route_credits*2048:0);
    result["route_payload_bytes"]=1024*(paired_qmac+paired_vmac)+(token_use_route?2048*(token_qmac+token_vmac):0);result["lean_scalar"]=lean_scalar;
    writevec(directory/"output.f32",outputs);writevec(directory/"probability.f16",all_prob);
    writevec(directory/"scores.f32",all_scores);writevec(directory/"mask.u8",all_skip);
    result["resident_publications"]=resident_publications;result["resident_reads"]=resident_reads;result["resident_evictions"]=resident_evictions;
    result["active_envelope_bytes"]=bank_envelope?8:2;result["resident_descriptor_bytes"]=32;result["active_budget_register_bytes"]=24;
    result["status"]="completed";result["scope"]="native AttAcc memory plus new numerical PU/EPU model; attention kernel only";
    result["mode"]=mode;result["fault"]=fault;result["heads"]=head_records;result["steps"]=step_records;result["phases"]=phase_records;
    result["bootstrap_heads"]=bootstrap_heads;result["observer_mac_jobs"]=observer_mac_jobs;
    result["observer_append_jobs"]=observer_append_jobs;result["observer_peak_jobs"]=observer_peak;
    result["observer_pipeline_register_bytes"]=2048+128*std::max(0,observer_latency-4);result["statistic_return_bytes"]=128*bootstrap_heads;
    result["shared_budget_reuses"]=budget_reuses;result["active_bound_budget_state_bytes"]=128;result["shared_budget_setups"]=budget_setups;result["probability_move_commands"]=probability_moves;result["probability_move_payload_bytes"]=64*probability_moves;
    result["scratch_bytes_per_cycle"]=scratch_bpc;result["legacy_epu_timing"]=legacy_epu_timing;
    result["statistic_move_commands"]=8*bootstrap_heads;
    result["stream_max_fusion"]=stream_max_fusion;result["inplace_exp"]=inplace_exp;
    result["max_group_cycles"]=max_group_cycles;result["max_batch_merge_cycles"]=max_batch_merge_cycles;
    result["exact_exp_calls"]=exact_exp_calls;result["softmax_score_read_bytes"]=softmax_score_read_bytes;
    result["exp_scratch_read_bytes"]=exp_scratch_read_bytes;result["exp_scratch_write_bytes"]=exp_scratch_write_bytes;
    result["streamed_max_tokens"]=streamed_max_tokens;result["streamed_max_batches"]=streamed_max_batches;
    result["streamed_max_service_cycles"]=streamed_max_cycles;result["exact_softmax_heads"]=exact_softmax_heads;
    result["removed_exp_scratch_bytes"]=inplace_exp?4ull*nmax:0;result["stream_max_live_bytes"]=stream_max_fusion?32:0;
    result["cache_initialization"]=cache_init;result["norm_peak_reserved_vectors"]=norm_peak;
    result["norm_busy_cycles"]=norm_busy;result["sqrt_operations"]=sqrt_count;result["initial_cache_writes"]=count_initwr;
    result["initialization_cycles"]=init_cycles;result["actor_end_cycle"]=clk;
    result["submitted"]=submitted;result["callbacks"]=callbacks;result["peak_inflight"]=max_inflight;
    result["qk_mac"]=count_qmac;result["value_mac"]=count_vmac;result["builder_reads"]=count_normld;
    result["header_reads"]=count_headerld;result["kv_append_writes"]=count_kvwr;result["metadata_writes"]=count_metawr;
    std::ofstream out(directory/"result.yaml");out<<result;out.flush();
    if(!out)throw std::runtime_error("Cannot commit numerical result");
  }
public:
  void init() override {
    m_clock_ratio=param<uint>("clock_ratio").required();
    std::string path=param<std::string>("fixture").required();
    mode=param<std::string>("mode").required();requested_mode=mode;
    external_policy=param<std::string>("external_policy").default_val("none");
    external_warm_start=param<bool>("external_warm_start").default_val(false);
    if(mode=="starc_port"||mode=="sparge_port"||mode=="sage_port"){external_policy=mode.substr(0,mode.size()-5);mode="dense";}
    if(external_policy!="none"&&external_policy!="starc"&&external_policy!="sparge"&&external_policy!="sage")throw std::runtime_error("Unknown external policy");
    actor_inflight_cap=param<int>("actor_inflight_cap").default_val(64);port_alu_lanes=param<int>("port_alu_lanes").default_val(16);
    port_layer=param<int>("port_layer").default_val(2);port_budget_ratio=param<double>("port_budget_ratio").default_val(.5);
    port_use_cdf=param<bool>("port_use_cdf").default_val(false);port_theta=param<float>("port_theta").default_val(-.1f);port_cdf=param<float>("port_cdf").default_val(.98f);
    if(actor_inflight_cap<1||actor_inflight_cap>64||port_alu_lanes<1||port_alu_lanes>16||!(port_budget_ratio>0&&port_budget_ratio<=1))throw std::runtime_error("Invalid external resource configuration");
    token_requested=mode=="token_dense"||mode=="token_zero"||mode=="token_lazy";
    if(token_requested)mode=mode=="token_dense"?"dense":(mode=="token_zero"?"zero_fused":"lazy_fused");
    pair_requested=mode=="paired_dense"||mode=="paired_zero";
    if(external()&&(pair_requested||(mode!="dense"&&mode!="token_dense")))throw std::runtime_error("External policy requires dense base arithmetic/layout");
    if(pair_requested)mode=mode=="paired_dense"?"dense":"zero_fused";
    route_bpc=param<int>("route_bytes_per_cycle").default_val(256);route_latency=param<int>("route_latency").default_val(4);route_credits=param<int>("route_credits").default_val(4);pair_dispatch=param<int>("pair_dispatch_cycles").default_val(4);
    token_use_route=param<bool>("token_use_route").default_val(false);
    lean_scalar=param<bool>("lean_scalar").default_val(true);
    if(route_bpc<16||route_bpc>1024||route_latency<1||route_latency>128||route_credits<1||route_credits>16||pair_dispatch<1)throw std::runtime_error("Invalid pair resource config");
    fault=param<std::string>("fault").default_val("none");
    cache_init=param<std::string>("cache_initialization").default_val("existing");
    if(cache_init!="existing"&&cache_init!="write"&&cache_init!="migrate")throw std::runtime_error("Unknown cache initialization mode");
    epsilon=param<double>("epsilon").default_val(0.1);
    std::string dir=param<std::string>("output_directory").required();directory=dir;std::filesystem::create_directories(directory);
    mac_latency=param<int>("mac_latency").default_val(6);move_latency=param<int>("move_latency").default_val(4);
    observer_latency=param<int>("observer_latency").default_val(4);
    observer_interval=param<int>("observer_interval").default_val(1);
    fused_tail=param<int>("fused_tail").default_val(4);budget_setup=param<int>("budget_setup").default_val(24);
    zero_tail=param<int>("zero_tail").default_val(1);scratch_bpc=param<int>("scratch_bytes_per_cycle").default_val(32);
    legacy_epu_timing=param<bool>("legacy_epu_timing").default_val(false);
    stream_max_fusion=param<bool>("stream_max_fusion").default_val(false);
    inplace_exp=param<bool>("inplace_exp").default_val(false);
    max_group_cycles=param<int>("max_group_cycles").default_val(1);max_batch_merge_cycles=param<int>("max_batch_merge_cycles").default_val(4);
    if(max_group_cycles<1||max_group_cycles>16||max_batch_merge_cycles<0||max_batch_merge_cycles>64)throw std::runtime_error("Invalid maximum service");
    if(legacy_epu_timing&&(stream_max_fusion||inplace_exp))throw std::runtime_error("New schedule requires explicit EPU model");
    if(pair_requested&&(stream_max_fusion||inplace_exp))throw std::runtime_error("New schedule scope excludes paired-head API");
    quota_cycles=param<int>("quota_cycles").default_val(3);if(quota_cycles<1||quota_cycles>16)throw std::runtime_error("Invalid quota throughput");
    resident_certificate=param<bool>("resident_certificate").default_val(false);bank_envelope=param<bool>("bank_envelope").default_val(true);evict_each_step=param<bool>("evict_each_step").default_val(false);
    if(observer_latency<1||observer_latency>32||observer_interval<1||observer_interval>4||fused_tail<0||budget_setup<0||zero_tail<0||scratch_bpc<16||scratch_bpc>128||!std::has_single_bit(unsigned(scratch_bpc)))throw std::runtime_error("Invalid v7 timing");
    norm_lanes=param<int>("norm_lanes").default_val(4);sort_width=param<int>("sort_width").default_val(1);
    watchdog=param<uint64_t>("watchdog").default_val(20000000);
    if(mode!="dense"&&mode!="zero"&&mode!="zero_fused"&&!approximate())throw std::runtime_error("Unknown numeric method");
    if(!std::isfinite(epsilon)||epsilon<0||norm_lanes<1||norm_lanes>16||sort_width<1)throw std::runtime_error("Invalid numerical config");
    std::ifstream in(path,std::ios::binary);std::array<char,8> magic{};in.read(magic.data(),8);
    if(std::string(magic.data(),8)!="DGNUM006")throw std::runtime_error("Invalid numerical fixture magic");
    std::array<uint32_t,4> dims{};in.read(reinterpret_cast<char*>(dims.data()),16);nmax=dims[0];H=dims[1];D=dims[2];Q=dims[3];
    if(!nmax||nmax>16384||!H||H>7||!D||D>128||D%16||!Q||Q>128)throw std::runtime_error("Fixture exceeds declared resource limits");
    
    token_packed=token_requested&&D==32&&nmax>=512&&nmax<=8192;
    if(token_requested&&!token_packed)token_disabled_reason=D!=32?"dimension":"context_limit";
    pair_enabled=pair_requested&&D==32&&H>=2&&nmax<=8192;
    if(fault=="owner_mismatch")pair_enabled=false;
    if(pair_requested&&!pair_enabled)pair_disabled_reason=fault=="owner_mismatch"?"owner_mismatch":(D!=32?"dimension":(H<2?"single_head":"scratch_limit"));
    dtiles=(D+63)/64;readvec(in,lengths,Q);readvec(in,K,nmax*D);readvec(in,V,nmax*D);readvec(in,queries,Q*H*D);
    if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Trailing fixture bytes");
    for(auto* v:{&K,&V,&queries})for(auto b:*v)if(!std::isfinite(hfloat(b)))throw std::runtime_error("Nonfinite fixture operand");
    for(unsigned i=0;i<Q;i++)if(!lengths[i]||lengths[i]>nmax||(i&&lengths[i]<lengths[i-1]))throw std::runtime_error("Illegal causal prefix sequence");
    
    
    uint64_t scratch=10ull*nmax+13ull*((nmax+15)/16)+8192+4ull*Q*H*D+2ull*dtiles*64+8192+2048+(nmax+7)/8+(dtiles*((nmax+15)/16)+7)/8;
    
    scratch-=4ull*(Q-1)*H*D;
    if(token_packed)scratch+=(token_use_route?route_credits*2048:0)+64+(cache_init=="migrate"?8192:0);
    if(pair_enabled)scratch+=8ull*nmax+((nmax+15)/16+7)/8+(nmax+7)/8+route_credits*1024+64;
    if(inplace_exp)scratch-=4ull*nmax;if(stream_max_fusion)scratch+=32;
    if(scratch>scratch_limit||(pair_enabled?4ull:2ull)*nmax>probability_limit)throw std::runtime_error("Finite scratch/GB capacity exceeded");
    result["base_peak_live_scratch_bytes"]=scratch;result["peak_live_scratch_bytes"]=scratch;result["pim_probability_buffer_bytes"]=(pair_enabled?4ull:2ull)*nmax;result["norm_fifo_bytes"]=2048;
    result["max_tokens"]=nmax;result["H"]=H;result["D"]=D;result["Q"]=Q;result["epsilon"]=epsilon;
    if(pair_enabled){scores_b.resize(nmax);prob_b.resize(nmax);pgb_b.resize(nmax);skip_b.resize((nmax+15)/16);gbvalid_b.resize(nmax);}
    route_events.open(directory/"route_events.tsv");route_events<<"id\tready\tstart\tend\tquery\thead\top\tepoch\tbytes\n";
    prob.resize(nmax);pgb.resize(nmax);gbvalid.resize(nmax);qgb.resize(dtiles*64);
    scores.resize(nmax);qpu.resize(1024);svpu.resize(1024);rankout.resize(128);
    masses.resize(fused()?0:(nmax+15)/16);skip.resize((nmax+15)/16);all_skip.resize(Q*H*((nmax+15)/16));
    outputs.resize(Q*H*D);all_prob.resize(Q*H*nmax);all_scores.resize(Q*H*nmax);
    if(external()){
      external_keep.assign(Q*H*nmax,0);external_order.assign(Q*H*nmax,-1);
      if(external_policy=="sage"){port_quant_dram.assign(uint64_t(nmax)*D,0);port_quant_written.assign(uint64_t(nmax)*(D/16),0);}
      port_warm_bootstrap();port_initialize_backing();
    }
    events.open(directory/"events.tsv");events<<"id\taccepted\tissued\tcompleted\tquery\thead\top\taddr\tx\ty\tz\n";
    resident_events.open(directory/"resident_events.tsv");resident_events<<"cycle\taction\tquery\thead\towner\tepoch\tlength\tamax_bits\tradius\tvalid\n";
    observer_events.open(directory/"observer_events.tsv");observer_events<<"id\tready\tstart\tend\top\tx\ty\tepoch\n";
    norm_events.open(directory/"norm_events.tsv");norm_events<<"id\tready\tstart\tend\ttoken\tchunk\n";
    dram=create_child_ifce<IDRAM>();mapper=create_child_ifce<IAddrMapper>();
    for(int i=0;i<dram->get_level_size("channel");i++) {
      auto* c=create_child_ifce<IDRAMController>();c->m_impl->set_id(fmt::format("Channel {}",i));c->m_channel_id=i;controllers.push_back(c);
    }
    register_stat(clk).name("memory_system_cycles");register_stat(submitted).name("numeric_submitted");register_stat(callbacks).name("numeric_callbacks");
  }
  void setup(IFrontEnd*,IMemorySystem*) override {}
  bool send(Request req) override {mapper->apply(req);return controllers.at(req.addr_vec[0])->send(req);}
  void tick() override {
    ++clk;if(uint64_t(clk)>watchdog)throw std::runtime_error("Native numerical watchdog exceeded");
    dram->tick();for(auto* c:controllers)c->tick();actor_tick();
  }
  bool numeric_finished()const override {return finished;}
  bool is_pending() override {for(auto* c:controllers)if(c->is_pending())return true;return !finished;}
  float get_tCK() override {return dram->m_timing_vals("tCK_ps")/1000.f;}
};
}
