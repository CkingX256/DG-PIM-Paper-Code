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



class NumericPIMDRAM7R : public IMemorySystem, public Implementation, public INumericSystem {
  RAMULATOR_REGISTER_IMPLEMENTATION(IMemorySystem, NumericPIMDRAM7R, "NumericPIMDRAM7R", "Native HBM-PIM with completion-bound numerical actors")
  enum Op {NormLoad,Publish,HeaderLoad,AppendK,AppendV,QWrite,QMac,QMove,SfmKick,PMove,VMac,VMove,StatMove};
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
  Addr_t vaddr(int token,int chunk)const {
    return VBASE+stripe_addr(token%16)+(chunk%4)*BANK+32ll*((chunk/4)*((nmax+15)/16)+token/16);
  }
  Addr_t kaddr(int token,int chunk)const {
    return stripe_addr(token%16)+(chunk%4)*BANK+32ll*((token/16)*dtiles+chunk/4);
  }
  float kval(int token,int dim)const {
    if(token<L&&token>=committed)throw std::runtime_error("Read of uncommitted Key");
    return token<L && dim<int(D) ? hfloat(K[token*D+dim]):0.f;
  }
  float vval(int token,int dim)const {
    if(token<L&&token>=committed)throw std::runtime_error("Read of uncommitted Value");
    return token<L && dim<int(D) ? hfloat(V[token*D+dim]):0.f;
  }
  bool lazy()const {return mode=="lazy"||mode=="lazy_fused"||mode=="uniform_fused"||mode=="quota_fused";}
  bool fused()const {return mode=="zero_fused"||mode=="greedy_fused"||mode=="lazy_fused"||mode=="uniform_fused"||mode=="quota_fused";}
  bool approximate()const {return mode=="greedy"||mode=="bucket"||mode=="sort"||mode=="greedy_fused"||lazy();}
  bool uses_l2()const {return approximate()&&!lazy();}
  bool gate_transport()const {return fused();}
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
    uint64_t masks=fused()?((groups+7)/8+(((groups+15)/16)+7)/8):0;
    if(!legacy_epu_timing) {
      uint64_t rd=(4ull*L+scratch_bpc-1)/scratch_bpc;
      uint64_t packing=rd+(2ull*L+scratch_bpc-1)/scratch_bpc;
      if(fused())packing+=(((groups+63)/64)*8ull+scratch_bpc-1)/scratch_bpc+(((((groups+15)/16)+63)/64)*8ull+scratch_bpc-1)/scratch_bpc;
      if(mode=="quota_fused"&&header_ok)packing=std::max<uint64_t>(packing,uint64_t(quota_cycles)*groups);
      base=std::max<uint64_t>(groups,rd)+std::max<uint64_t>(groups,2*rd)+std::max<uint64_t>(groups,packing);
    }
    uint64_t extra=fused()?((mode=="zero_fused"||(lazy()&&!header_ok))?zero_tail:fused_tail):(lazy()&&!header_ok?zero_tail:0);
    return base+8+extra;
  }
  uint64_t mass_service()const {
    if(legacy_epu_timing)return groups+(active_mode()=="zero"?0:4);
    return std::max<uint64_t>(groups+4,(2ull*L+scratch_bpc-1)/scratch_bpc+(4ull*groups+scratch_bpc-1)/scratch_bpc)+(approximate()&&header_ok?budget_setup:0);
  }
  uint64_t merge_service()const {
    if(legacy_epu_timing)return (D+15)/16;
    uint64_t dims=std::min<int>(64,D-dtile*64);
    return std::max<uint64_t>((dims+15)/16,3*((4*dims+scratch_bpc-1)/scratch_bpc))+2;
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
          int token=t.x*16+stripe,dim=t.y*64+d;
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
    if(seen!=uint64_t(groups*dtiles))throw std::runtime_error("Incomplete mandatory Value observation");
    gathered_valid.fill(false);setphase("stat_move");
    for(int r=0;r<2;r++)for(int b=0;b<4;b++)enqueue(StatMove,HDR+r*RANK+b*BANK,r,b);
  }
  void enqueue(int op,Addr_t addr,int x=0,int y=0,int z=0) {
    tasks.push_back({next_id++,op,x,y,z,q,h,addr,0});
    if(next_id<0) throw std::runtime_error("Request ID exhausted");
  }
  int reqtype(int op)const {
    if(op==NormLoad||op==HeaderLoad)return Request::Type::Read;
    if(op==Publish||op==AppendK||op==AppendV)return Request::Type::Write;
    if(op==QWrite)return Request::Type::PIM_WR_GB;
    if(op==QMac||op==VMac)return Request::Type::PIM_MAC_AB;
    if(op==QMove||op==VMove||op==StatMove)return Request::Type::PIM_MV_SB;
    if(op==PMove)return Request::Type::PIM_MV_GB;
    return Request::Type::PIM_SFM;
  }
  void setphase(const std::string& next) {
    if(phase!="start") {
      YAML::Node x;x["query"]=phase_q;x["head"]=phase_h;x["phase"]=phase;
      x["start"]=phase_start;x["end"]=clk;x["cycles"]=clk-phase_start;phase_records.push_back(x);
    }
    phase=next;phase_start=clk;phase_q=q;phase_h=h;
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
  void initial_writes() {
    build_cursor=committed;build_end=std::min(L,committed+64);normsum.fill(0);
    setphase("init_write");
    for(int t=committed;t<build_end;t++)for(int c=0;c<int(D)/16;c++) {
      enqueue(AppendK,kaddr(t,c),t,c);enqueue(AppendV,vaddr(t,c),t,c);
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
      enqueue(AppendK,kaddr(t,c),t,c);enqueue(AppendV,vaddr(t,c),t,c);
    }
  }
  void start_head() {
    if(committed!=L)throw std::runtime_error("Head started before cache writes committed");
    ++h;bootstrap_head=false;head_choice=lazy()?(header_ok?(mode=="quota_fused"?"quota":(mode=="uniform_fused"?"uniform":"greedy")):"cold"):"";batch=0;dtile=0;bound=0;comparisons=0;skipped=0;skip_mass=0;total_mass=0;allowance=0;
    std::fill(scores.begin(),scores.end(),0);std::fill(qpu.begin(),qpu.end(),0);
    std::fill(gbvalid.begin(),gbvalid.end(),0);std::fill(skip.begin(),skip.end(),0);
    setphase("q_write");
    for(int b=0;b<4;b++)for(int tile=0;tile<dtiles;tile++)enqueue(QWrite,b*BANK+tile,b,tile);
  }
  void q_macs() {
    setphase("q_mac");
    for(int g=batch;g<std::min(groups,batch+16);g++)for(int t=0;t<dtiles;t++)enqueue(QMac,32ll*(g*dtiles+t),g,t);
  }
  void q_moves() {
    setphase("q_move");
    for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)enqueue(QMove,r*RANK+bg*BG,r,bg);
  }
  void softmax() {
    float scale=1.f/std::sqrt(float(D)),mx=-std::numeric_limits<float>::infinity();
    for(int t=0;t<L;t++)mx=std::max(mx,scores[t]*scale);
    std::vector<float> expv(L);float z=0;
    for(int g=0;g<groups;g++) {
      std::array<float,16> a{};
      for(int s=0;s<16;s++){int t=g*16+s;if(t<L)a[s]=expv[t]=std::exp(scores[t]*scale-mx);}
      for(int stride=1;stride<16;stride*=2)for(int i=0;i<16;i+=2*stride)a[i]+=a[i+stride];
      z+=a[0];
    }
    if(!(z>0) || !std::isfinite(z))throw std::runtime_error("Invalid implemented softmax");
    std::fill(prob.begin(),prob.end(),0);
    if(!fused())for(int t=0;t<L;t++)prob[t]=hbits(expv[t]/z);
    else {
      double u=std::ldexp(1.,-24);int k=(L+15)/16+15;gamma=up((k*u)/(1-k*u));
      total_mass=0;allowance=0;fused_skip_mass=0;fused_skipped=0;fused_zero=0;
      std::fill(skip.begin(),skip.end(),false);transport_frames.assign((L+255)/256,false);
      if(approximate()&&header_ok) {
        if(budget_epoch!=generation||fault=="budget_wrong_epoch")throw std::runtime_error("Shared budget epoch mismatch");
        allowance=prefix_allowance;gamma=prefix_gamma;
      }
      uint64_t remaining=allowance;
      for(int g=0;g<groups;g++) {
        uint32_t m=0;
        for(int lane=0;lane<16;lane++) {
          int t=g*16+lane;if(t>=L)break;
          prob[t]=hbits(expv[t]/z);m+=punits(prob[t]);
        }
        total_mass+=m;fused_zero+=(m==0);bool take=false;
        if(approximate()&&header_ok) {
          bool ok=loaded_radius==0||(mode=="quota_fused"?uint64_t(m)*(groups-g)<=remaining:(mode=="uniform_fused"?m<=uniform_cut:m<=remaining));
          if(ok){if(m>remaining)throw std::runtime_error("Policy budget underflow");take=true;remaining-=m;}
        }
        else if(mode=="zero_fused"||(lazy()&&!lazy_bound_ready))take=(m==0);
        if(take){skip[g]=true;fused_skip_mass+=m;++fused_skipped;}else transport_frames[g/16]=true;
      }
      if(total_mass>(513ull<<15))throw std::runtime_error("Proven FP16 softmax mass cap violated");
    }
    size_t off=(q*H+h)*nmax;
    std::copy(scores.begin(),scores.end(),all_scores.begin()+off);
    std::copy(prob.begin(),prob.end(),all_prob.begin()+off);
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
    skipped=0;skip_mass=0;
    if(fused()){skipped=fused_skipped;skip_mass=fused_skip_mass;}else for(int g=0;g<groups;g++)if(skip[g]){++skipped;skip_mass+=masses[g];}
    if(active_mode()=="zero" && skip_mass)throw std::runtime_error("Unsafe zero command gate");
    if(approximate()&&header_ok&&skip_mass>allowance)throw std::runtime_error("Integer allowance overrun");
    if(skip_mass&&loaded_radius>0)bound=up(loaded_radius*up(std::ldexp(double(skip_mass),-24)+up(up(2*gamma)*std::ldexp(double(total_mass),-24))));
    if(skip_mass&&bound>epsilon)throw std::runtime_error("Rounded omission certificate exceeds budget");
    size_t off=(q*H+h)*((nmax+15)/16);std::copy(skip.begin(),skip.end(),all_skip.begin()+off);
    setphase("p_move");
    for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)for(int c=0;c<(L+255)/256;c++)
      if(!gate_transport()||transport_frames[c])enqueue(PMove,r*RANK+bg*BG+c,r,bg,c);
  }
  void v_macs() {
    setphase("v_mac");std::fill(svpu.begin(),svpu.end(),0);
    for(int g=0;g<groups;g++)if(!skip[g])enqueue(VMac,VBASE+32ll*(dtile*((nmax+15)/16)+g),g,dtile);
  }
  void v_moves() {
    setphase("v_move");std::fill(rankout.begin(),rankout.end(),0);
    for(int r=0;r<2;r++)for(int b=0;b<4;b++)enqueue(VMove,VBASE+r*RANK+b*BANK,r,b);
  }
  void finish_head() {
    YAML::Node r;r["query"]=q;r["head"]=h;r["length"]=L;r["active_mode"]=active_mode();
    r["G"]=bootstrap_head?0:loaded_radius;r["gamma"]=gamma;r["mass_units"]=total_mass;r["allowance_units"]=allowance;
    r["skipped_mass_units"]=skip_mass;r["skipped_groups"]=skipped;r["groups"]=groups;
    r["error_bound_vs_implemented_dense"]=bound;r["sort_comparisons"]=comparisons;
    r["metadata_valid"]=bootstrap_head?false:header_ok;r["bound_kind"]=lazy()?(bank_envelope?"bank_amax_envelope":"amax_envelope"):"row_l2";
    for(auto a:bank_max)r["bank_amax_bits"].push_back(a);r["bank_envelope"]=bank_envelope;r["amax_bits"]=amax_bits;r["reserve_mass_units"]=fused()?(513ull<<15):total_mass;
    r["fused"]=fused();r["bootstrap"]=bootstrap_head;head_records.push_back(r);
    if(h+1<int(H)){start_head();return;}
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
      case AppendK:case AppendV:
        if(t.x<committed||t.x>=L)throw std::runtime_error("Append ownership violation");
        if(t.op==AppendV&&uses_l2()) {
          for(int j=0;j<16;j++){float v=hfloat(V[t.x*D+t.y*16+j]);normsum[t.x-committed]=std::fma(v,v,normsum[t.x-committed]);}
          schedule_norm(t,clk);
        }
        if(t.op==AppendV&&observer_request(t))schedule_observer(t);
        if(initializing)++count_initwr;else ++count_kvwr;break;
      case QWrite:
        for(int j=0;j<16;j++){int d=t.y*64+t.x*16+j;qgb[d]=d<int(D)?queries[(q*H+h)*D+d]:0;}break;
      case QMac:
        if(req.addr/32/dtiles!=t.x || req.addr/32%dtiles!=t.y)
          throw std::runtime_error("Physical QK command coverage mismatch");
        for(int s=0;s<16;s++)for(int b=0;b<4;b++)for(int j=0;j<16;j++) {
          int d=t.y*64+b*16+j,token=t.x*16+s,ix=(t.x-batch)*64+s*4+b;
          qpu[ix]=std::fma(kval(token,d),hfloat(qgb[d]),qpu[ix]);
        }
        ++count_qmac;
        if(fault=="duplicate_completion"&&!duplicate_injected){duplicate_injected=true;completed(id,req);}break;
      case QMove:
        for(int pc=0;pc<2;pc++)for(int slot=0;slot<std::min(16,groups-batch);slot++) {
          int stripe=pc*8+t.x*4+t.y,token=(batch+slot)*16+stripe;
          float v=0;for(int b=0;b<4;b++)v+=qpu[slot*64+stripe*4+b];
          if(token<L)scores[token]=v;
        }break;
      case SfmKick:break;
      case PMove:
        ++probability_moves;
        for(int pc=0;pc<2;pc++)for(int i=0;i<16;i++) {
          int stripe=pc*8+t.x*4+t.y,token=(t.z*16+i)*16+stripe;
          if(token<int(nmax)){pgb[token]=token<L?prob[token]:0;gbvalid[token]=1;}
        }break;
      case VMac:
        if((req.addr-VBASE)/32%((nmax+15)/16)!=t.x || (req.addr-VBASE)/32/((nmax+15)/16)!=t.y)
          throw std::runtime_error("Physical Value command coverage mismatch");
        for(int s=0;s<16;s++) {
          int token=t.x*16+s;
          if(token<int(nmax)&&!gbvalid[token])throw std::runtime_error("MAC consumed an unready probability");
          float p=token<L?hfloat(pgb[token]):0;
          for(int d=0;d<64;d++)svpu[s*64+d]=std::fma(p,vval(token,t.y*64+d),svpu[s*64+d]);
        }
        if(observer_request(t))schedule_observer(t);++count_vmac;break;
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
      if(cache_init=="write"){committed=0;initial_writes();}
      else if(uses_l2()){build_cursor=0;build_loads();}
      else {initializing=false;init_cycles=0;start_step();}
    } else if(phase=="build_load") {
      setphase("build_norm");int rows=build_end-build_cursor;
      epuservice(rows,[this,rows]{finish_norm(rows);});
    } else if(phase=="build_norm") {
      build_cursor=build_end;if(build_cursor<committed)build_loads();else publish();
    } else if(phase=="init_write") {
      if(uses_l2()) {
        setphase("init_write_norm");int rows=build_end-committed;
        epuservice(rows,[this,rows]{finish_norm(rows);committed=build_end;});
      } else {committed=build_end;if(committed<L)initial_writes();else {initializing=false;init_cycles=clk-start_clk;start_step();}}
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
    else if(phase=="q_write")q_macs();
    else if(phase=="q_mac")q_moves();
    else if(phase=="q_move") {
      batch+=16;if(batch<groups){std::fill(qpu.begin(),qpu.end(),0);q_macs();}
      else {setphase("sfm_kick");enqueue(SfmKick,0);}
    } else if(phase=="sfm_kick") {
      setphase("softmax");
      epuservice(softmax_service(),[this]{softmax();});
    } else if(phase=="softmax") {
      if(fused())finish_plan();
      else if(active_mode()=="dense"||active_mode()=="bootstrap") {total_mass=0;finish_plan();}
      else {setphase("mass");epuservice(mass_service(),[this]{mass_pass();});}
    } else if(phase=="mass") {
      setphase("plan");uint64_t work=plan();epuservice(work,[]{});
    } else if(phase=="plan")finish_plan();
    else if(phase=="p_move")v_macs();
    else if(phase=="v_mac")v_moves();
    else if(phase=="v_move") {
      setphase("merge");epuservice(merge_service(),[this]{
        for(int j=0;j<64;j++)if(dtile*64+j<int(D)){
          float x=rankout[j]+rankout[64+j];outputs[(q*H+h)*D+dtile*64+j]=x==0?0:x;
        }});
    } else if(phase=="merge") {
      if(++dtile<dtiles)v_macs();else if(bootstrap_head)gather_observer();else finish_head();
    } else if(phase=="stat_move") {
      for(bool v:gathered_valid)if(!v)throw std::runtime_error("Incomplete amax return");
      setphase("stat_reduce");epuservice(std::max(8,(258+scratch_bpc-1)/scratch_bpc),[this]{for(int i=0;i<64;i++){auto v=gathered_max[i];amax_bits=std::max(amax_bits,v);bank_max[i%4]=std::max(bank_max[i%4],v);}lazy_bound_ready=true;});
    } else if(phase=="stat_reduce")publish();
    else if(phase=="bootstrap_header") {
      if(fused()&&header_ok){setphase("bootstrap_budget");epuservice(shared_budget_service(),[this]{prepare_shared_budget();});}else finish_head();
    } else if(phase=="bootstrap_budget")finish_head();
  }
  void actor_tick() {
    if(finished)return;
    commit_observers();
    while(!norm_finishes.empty()&&norm_finishes.front()<=clk){norm_finishes.pop_front();--norm_reserved;}
    if(epu_commit){if(clk<epu_until)return;auto action=std::move(epu_commit);epu_commit={};action();}
    if(tasks.empty()&&outstanding.empty()&&norm_reserved==0&&observer.empty())advance();
    while(!tasks.empty()&&outstanding.size()<64) {
      Task t=tasks.front();Request req(t.addr,reqtype(t.op),t.id,[this,id=t.id](Request& r){completed(id,r);});
      if(needs_norm(t)&&norm_reserved>=64)break;
      if(observer_request(t)&&observer.size()+outstanding.size()>=64)break;
      t.accepted=clk;
      if(!send(req))break;
      outstanding.emplace(t.id,t);tasks.pop_front();++submitted;
      if(needs_norm(t)){++norm_reserved;norm_peak=std::max(norm_peak,norm_reserved);}
      max_inflight=std::max<uint64_t>(max_inflight,outstanding.size());
    }
  }
  void dump() {
    events.flush();events.close();norm_events.flush();norm_events.close();observer_events.flush();observer_events.close();resident_events.flush();resident_events.close();
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
    mode=param<std::string>("mode").required();fault=param<std::string>("fault").default_val("none");
    cache_init=param<std::string>("cache_initialization").default_val("existing");
    if(cache_init!="existing"&&cache_init!="write")throw std::runtime_error("Unknown cache initialization mode");
    epsilon=param<double>("epsilon").default_val(0.1);
    std::string dir=param<std::string>("output_directory").required();directory=dir;std::filesystem::create_directories(directory);
    mac_latency=param<int>("mac_latency").default_val(6);move_latency=param<int>("move_latency").default_val(4);
    observer_latency=param<int>("observer_latency").default_val(4);
    observer_interval=param<int>("observer_interval").default_val(1);
    fused_tail=param<int>("fused_tail").default_val(4);budget_setup=param<int>("budget_setup").default_val(24);
    zero_tail=param<int>("zero_tail").default_val(1);scratch_bpc=param<int>("scratch_bytes_per_cycle").default_val(32);
    legacy_epu_timing=param<bool>("legacy_epu_timing").default_val(false);
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
    if(!nmax||nmax>16384||!H||H>6||!D||D>128||D%16||!Q||Q>128)throw std::runtime_error("Fixture exceeds declared resource limits");
    dtiles=(D+63)/64;readvec(in,lengths,Q);readvec(in,K,nmax*D);readvec(in,V,nmax*D);readvec(in,queries,Q*H*D);
    if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Trailing fixture bytes");
    for(auto* v:{&K,&V,&queries})for(auto b:*v)if(!std::isfinite(hfloat(b)))throw std::runtime_error("Nonfinite fixture operand");
    for(unsigned i=0;i<Q;i++)if(!lengths[i]||lengths[i]>nmax||(i&&lengths[i]<lengths[i-1]))throw std::runtime_error("Illegal causal prefix sequence");
    
    
    uint64_t scratch=10ull*nmax+13ull*((nmax+15)/16)+8192+4ull*Q*H*D+2ull*dtiles*64+8192+2048+(nmax+7)/8+(dtiles*((nmax+15)/16)+7)/8;
    
    scratch-=4ull*(Q-1)*H*D;
    if(scratch>scratch_limit||2ull*nmax>probability_limit)throw std::runtime_error("Finite scratch/GB capacity exceeded");
    result["peak_live_scratch_bytes"]=scratch;result["pim_probability_buffer_bytes"]=2ull*nmax;result["norm_fifo_bytes"]=2048;
    result["max_tokens"]=nmax;result["H"]=H;result["D"]=D;result["Q"]=Q;result["epsilon"]=epsilon;
    prob.resize(nmax);pgb.resize(nmax);gbvalid.resize(nmax);qgb.resize(dtiles*64);
    scores.resize(nmax);qpu.resize(1024);svpu.resize(1024);rankout.resize(128);
    masses.resize(fused()?0:(nmax+15)/16);skip.resize((nmax+15)/16);all_skip.resize(Q*H*((nmax+15)/16));
    outputs.resize(Q*H*D);all_prob.resize(Q*H*nmax);all_scores.resize(Q*H*nmax);
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
