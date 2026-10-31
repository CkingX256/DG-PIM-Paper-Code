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



class NumericPIMDRAM : public IMemorySystem, public Implementation, public INumericSystem {
  RAMULATOR_REGISTER_IMPLEMENTATION(IMemorySystem, NumericPIMDRAM, "NumericPIMDRAM", "Native HBM-PIM with completion-bound numerical actors")
  enum Op {NormLoad,Publish,HeaderLoad,AppendK,AppendV,QWrite,QMac,QMove,SfmKick,PMove,VMac,VMove};
  struct Task {int id,op,x=0,y=0,z=0,q=-1,h=-1; Addr_t addr=0; Clk_t accepted=0;};
  IDRAM* dram=nullptr; IAddrMapper* mapper=nullptr;
  std::vector<IDRAMController*> controllers;
  std::deque<Task> tasks;std::map<int,Task> outstanding;
  int next_id=1;uint64_t callbacks=0,submitted=0,max_inflight=0;
  Clk_t clk=0,start_clk=0,epu_until=0,phase_start=0,step_start=0,init_cycles=0;
  std::function<void()> epu_commit;
  std::string phase="start",mode,fault,cache_init;
  std::filesystem::path directory;
  std::ofstream events,norm_events;
  YAML::Node result,head_records,step_records,phase_records;
  uint32_t nmax=0,H=0,D=0,Q=0;
  std::vector<uint32_t> lengths;
  std::vector<uint16_t> K,V,queries,prob,pgb,qgb;
  std::vector<float> scores,qpu,svpu,rankout,outputs,all_scores;
  std::vector<uint16_t> all_prob;
  std::vector<uint8_t> skip,all_skip,gbvalid;
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
  bool approximate()const {return mode=="greedy" || mode=="bucket" || mode=="sort";}
  std::string active_mode()const {return approximate()&&!header_ok ? "dense":mode;}
  void enqueue(int op,Addr_t addr,int x=0,int y=0,int z=0) {
    tasks.push_back({next_id++,op,x,y,z,q,h,addr,0});
    if(next_id<0) throw std::runtime_error("Request ID exhausted");
  }
  int reqtype(int op)const {
    if(op==NormLoad||op==HeaderLoad)return Request::Type::Read;
    if(op==Publish||op==AppendK||op==AppendV)return Request::Type::Write;
    if(op==QWrite)return Request::Type::PIM_WR_GB;
    if(op==QMac||op==VMac)return Request::Type::PIM_MAC_AB;
    if(op==QMove||op==VMove)return Request::Type::PIM_MV_SB;
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
  bool needs_norm(const Task& t)const {return approximate()&&(t.op==NormLoad||t.op==AppendV);}
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
    if(max_norm_sum==0){radius=0;++sqrt_count;return;}
    double u=std::ldexp(1.,-24);double gn=up((double(D)*u)/(1-double(D)*u));
    double den=down(1-gn);
    radius=up(std::sqrt(up(double(max_norm_sum)/den)));++sqrt_count;
    if(!std::isfinite(radius))throw std::runtime_error("Invalid Value norm radius");
  }
  void publish() {setphase("radius");epuservice(16,[this]{update_radius();});}
  void start_step() {
    L=int(lengths[q]);groups=(L+15)/16;h=-1;step_start=clk;
    header_ok=false;if(q>0)header_valid=false;
    if(q==0) {
      if(approximate()){setphase("header");enqueue(HeaderLoad,HDR);}
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
    ++h;batch=0;dtile=0;bound=0;comparisons=0;skipped=0;skip_mass=0;total_mass=0;allowance=0;
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
    for(int t=0;t<L;t++)prob[t]=hbits(expv[t]/z);
    size_t off=(q*H+h)*nmax;
    std::copy(scores.begin(),scores.end(),all_scores.begin()+off);
    std::copy(prob.begin(),prob.end(),all_prob.begin()+off);
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
        double reserve=up(up(2*gamma)*std::ldexp(double(total_mass),-24));
        double available=down(down(epsilon/loaded_radius)-reserve);
        if(available>0)allowance=std::min<uint64_t>(total_mass,uint64_t(std::floor(std::ldexp(available,24))));
      }
    }
  }
  uint64_t plan() {
    std::string am=active_mode();std::fill(skip.begin(),skip.end(),0);
    if(am=="zero") {for(int g=0;g<groups;g++)skip[g]=(masses[g]==0);return 1;}
    if(am=="dense")return 1;
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
    return (comparisons+sort_width-1)/sort_width+groups;
  }
  void finish_plan() {
    skipped=0;skip_mass=0;
    for(int g=0;g<groups;g++)if(skip[g]){++skipped;skip_mass+=masses[g];}
    if(active_mode()=="zero" && skip_mass)throw std::runtime_error("Unsafe zero command gate");
    if(approximate()&&header_ok&&skip_mass>allowance)throw std::runtime_error("Integer allowance overrun");
    if(skip_mass&&loaded_radius>0)bound=up(loaded_radius*up(std::ldexp(double(skip_mass),-24)+up(up(2*gamma)*std::ldexp(double(total_mass),-24))));
    if(skip_mass&&bound>epsilon)throw std::runtime_error("Rounded omission certificate exceeds budget");
    size_t off=(q*H+h)*((nmax+15)/16);std::copy(skip.begin(),skip.end(),all_skip.begin()+off);
    setphase("p_move");
    for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)for(int c=0;c<(L+255)/256;c++)enqueue(PMove,r*RANK+bg*BG+c,r,bg,c);
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
    r["G"]=loaded_radius;r["gamma"]=gamma;r["mass_units"]=total_mass;r["allowance_units"]=allowance;
    r["skipped_mass_units"]=skip_mass;r["skipped_groups"]=skipped;r["groups"]=groups;
    r["error_bound_vs_implemented_dense"]=bound;r["sort_comparisons"]=comparisons;
    r["metadata_valid"]=header_ok;head_records.push_back(r);
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
        if(t.op==AppendV&&approximate()) {
          for(int j=0;j<16;j++){float v=hfloat(V[t.x*D+t.y*16+j]);normsum[t.x-committed]=std::fma(v,v,normsum[t.x-committed]);}
          schedule_norm(t,clk);
        }
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
        ++count_vmac;break;
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
      else if(approximate()){build_cursor=0;build_loads();}
      else {initializing=false;init_cycles=0;start_step();}
    } else if(phase=="build_load") {
      setphase("build_norm");int rows=build_end-build_cursor;
      epuservice(rows,[this,rows]{finish_norm(rows);});
    } else if(phase=="build_norm") {
      build_cursor=build_end;if(build_cursor<committed)build_loads();else publish();
    } else if(phase=="init_write") {
      if(approximate()) {
        setphase("init_write_norm");int rows=build_end-committed;
        epuservice(rows,[this,rows]{finish_norm(rows);committed=build_end;});
      } else {committed=build_end;if(committed<L)initial_writes();else {initializing=false;init_cycles=clk-start_clk;start_step();}}
    } else if(phase=="init_write_norm") {
      if(committed<L)initial_writes();else publish();
    } else if(phase=="radius") {
      setphase("publish");enqueue(Publish,HDR);
    } else if(phase=="publish") {
      if(initializing){initializing=false;init_cycles=clk-start_clk;start_step();}
      else {setphase("header");enqueue(HeaderLoad,HDR);}
    } else if(phase=="append") {
      int added=L-committed;
      if(approximate()) {setphase("append_norm");epuservice(added,[this,added]{finish_norm(added);committed=L;});}
      else {committed=L;start_head();}
    } else if(phase=="append_norm")publish();
    else if(phase=="header")start_head();
    else if(phase=="q_write")q_macs();
    else if(phase=="q_mac")q_moves();
    else if(phase=="q_move") {
      batch+=16;if(batch<groups){std::fill(qpu.begin(),qpu.end(),0);q_macs();}
      else {setphase("sfm_kick");enqueue(SfmKick,0);}
    } else if(phase=="sfm_kick") {
      setphase("softmax");epuservice(3*((L+15)/16)+8,[this]{softmax();});
    } else if(phase=="softmax") {
      if(active_mode()=="dense") {total_mass=0;finish_plan();}
      else {setphase("mass");epuservice(groups+(active_mode()=="zero"?0:4),[this]{mass_pass();});}
    } else if(phase=="mass") {
      setphase("plan");uint64_t work=plan();epuservice(work,[]{});
    } else if(phase=="plan")finish_plan();
    else if(phase=="p_move")v_macs();
    else if(phase=="v_mac")v_moves();
    else if(phase=="v_move") {
      setphase("merge");epuservice((D+15)/16,[this]{
        for(int j=0;j<64;j++)if(dtile*64+j<int(D)){
          float x=rankout[j]+rankout[64+j];outputs[(q*H+h)*D+dtile*64+j]=x==0?0:x;
        }});
    } else if(phase=="merge") {if(++dtile<dtiles)v_macs();else finish_head();}
  }
  void actor_tick() {
    if(finished)return;
    while(!norm_finishes.empty()&&norm_finishes.front()<=clk){norm_finishes.pop_front();--norm_reserved;}
    if(epu_commit){if(clk<epu_until)return;auto action=std::move(epu_commit);epu_commit={};action();}
    if(tasks.empty()&&outstanding.empty()&&norm_reserved==0)advance();
    while(!tasks.empty()&&outstanding.size()<64) {
      Task t=tasks.front();Request req(t.addr,reqtype(t.op),t.id,[this,id=t.id](Request& r){completed(id,r);});
      if(needs_norm(t)&&norm_reserved>=64)break;
      t.accepted=clk;
      if(!send(req))break;
      outstanding.emplace(t.id,t);tasks.pop_front();++submitted;
      if(needs_norm(t)){++norm_reserved;norm_peak=std::max(norm_peak,norm_reserved);}
      max_inflight=std::max<uint64_t>(max_inflight,outstanding.size());
    }
  }
  void dump() {
    events.flush();events.close();norm_events.flush();norm_events.close();
    writevec(directory/"output.f32",outputs);writevec(directory/"probability.f16",all_prob);
    writevec(directory/"scores.f32",all_scores);writevec(directory/"mask.u8",all_skip);
    result["status"]="completed";result["scope"]="native AttAcc memory plus new numerical PU/EPU model; attention kernel only";
    result["mode"]=mode;result["fault"]=fault;result["heads"]=head_records;result["steps"]=step_records;result["phases"]=phase_records;
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
    norm_lanes=param<int>("norm_lanes").default_val(4);sort_width=param<int>("sort_width").default_val(1);
    watchdog=param<uint64_t>("watchdog").default_val(20000000);
    if(mode!="dense"&&mode!="zero"&&!approximate())throw std::runtime_error("Unknown numeric method");
    if(!std::isfinite(epsilon)||epsilon<0||norm_lanes<1||sort_width<1)throw std::runtime_error("Invalid numerical config");
    std::ifstream in(path,std::ios::binary);std::array<char,8> magic{};in.read(magic.data(),8);
    if(std::string(magic.data(),8)!="DGNUM006")throw std::runtime_error("Invalid numerical fixture magic");
    std::array<uint32_t,4> dims{};in.read(reinterpret_cast<char*>(dims.data()),16);nmax=dims[0];H=dims[1];D=dims[2];Q=dims[3];
    if(!nmax||nmax>16384||!H||H>6||!D||D>128||D%16||!Q||Q>128)throw std::runtime_error("Fixture exceeds declared resource limits");
    dtiles=(D+63)/64;readvec(in,lengths,Q);readvec(in,K,nmax*D);readvec(in,V,nmax*D);readvec(in,queries,Q*H*D);
    if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Trailing fixture bytes");
    for(auto* v:{&K,&V,&queries})for(auto b:*v)if(!std::isfinite(hfloat(b)))throw std::runtime_error("Nonfinite fixture operand");
    for(unsigned i=0;i<Q;i++)if(!lengths[i]||lengths[i]>nmax||(i&&lengths[i]<lengths[i-1]))throw std::runtime_error("Illegal causal prefix sequence");
    uint64_t scratch=6ull*nmax+9ull*((nmax+15)/16)+8192+4ull*Q*H*D+2ull*dtiles*64+256+2048;
    
    scratch-=4ull*(Q-1)*H*D;
    if(scratch>scratch_limit||2ull*nmax>probability_limit)throw std::runtime_error("Finite scratch/GB capacity exceeded");
    result["peak_live_scratch_bytes"]=scratch;result["pim_probability_buffer_bytes"]=2ull*nmax;result["norm_fifo_bytes"]=2048;
    result["max_tokens"]=nmax;result["H"]=H;result["D"]=D;result["Q"]=Q;result["epsilon"]=epsilon;
    prob.resize(nmax);pgb.resize(nmax);gbvalid.resize(nmax);qgb.resize(dtiles*64);
    scores.resize(nmax);qpu.resize(1024);svpu.resize(1024);rankout.resize(128);
    masses.resize((nmax+15)/16);skip.resize(masses.size());all_skip.resize(Q*H*masses.size());
    outputs.resize(Q*H*D);all_prob.resize(Q*H*nmax);all_scores.resize(Q*H*nmax);
    events.open(directory/"events.tsv");events<<"id\taccepted\tissued\tcompleted\tquery\thead\top\taddr\tx\ty\tz\n";
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
