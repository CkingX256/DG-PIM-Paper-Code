bool enable_tsv_logs=(std::getenv("DGPIM_FULL_TSV")!=nullptr);
bool dynamic_phase_reuse_free=false;



bool qk_reuse_pipeline=false,qk_readout_first=false,qk_cache_reserve_only=false;
uint32_t qpipe_readout_old_max_bits=0;
bool qk_exact_exp_cache=false,qk_priority_first=false,qk_online_softmax=false,qk_online_layout_aware=false;
bool dynamic_phase_reuse=false,dynamic_reuse_charge_cost=true,replay_pressure_guard=false;
std::string dynamic_remap_mode="epoch_tag";
std::array<uint8_t,7> qpipe_late_max_streak{};
uint64_t dynamic_remap_cycles=0,qpipe_online_rescale_events=0,qpipe_online_rescale_cycles=0;
int qpipe_priority_batch=0;
std::string qk_exp_policy="predictive";
bool qpipe_cache_this_head=false;
std::array<uint32_t,7> qpipe_previous_argmax{},qpipe_previous_length{};
uint8_t qpipe_previous_valid=0;
std::vector<bool> qpipe_exp_valid;
uint64_t qpipe_admitted_heads=0,qpipe_cached_batches=0;

std::vector<float> qpipe_exp_cache;
int qpipe_valid_exp_begin=0;
uint64_t qpipe_head_exp_start=0,qpipe_spec_exp_calls=0,qpipe_recomputed_exp_calls=0;
uint64_t qpipe_valid_exp_calls=0,qpipe_exp_epu_cycles=0,qpipe_invalidations=0;
int qk_pipeline_depth=2,qk_pipeline_startup=2;
struct QPipeSlot {int base=-1,sequence=-1,stage=0,mac_left=0,move_left=0;};
std::array<QPipeSlot,2> qpipe_slots;
int qpipe_next=0,qpipe_retired=0,qpipe_max_slot=-1;
bool qpipe_active=false;
uint64_t qpipe_heads=0,qpipe_batches=0,qpipe_mac=0,qpipe_moves=0,qpipe_epu_cycles=0;
uint64_t qpipe_peak_slots=0,qpipe_mac_with_epu_cycles=0,qpipe_mac_with_move_cycles=0;
uint64_t qpipe_startup_cycles=0,qpipe_release_checks=0,qpipe_history_cycles=0;
bool qpipe_history_committed=false;
uint64_t qpipe_spec_score_read_bytes=0,qpipe_spec_exp_write_bytes=0,qpipe_group_sum_write_bytes=0;
std::ofstream qpipe_events;
void qpipe_log(const char*event,int slot){
 if(!enable_tsv_logs)return;
 const auto&s=qpipe_slots[slot];
 qpipe_events<<clk<<'\t'<<q<<'\t'<<h<<'\t'<<s.base<<'\t'<<slot<<'\t'<<event<<'\t'<<s.mac_left<<'\t'<<s.move_left<<'\n';
}
std::vector<float>& qpipe_acc(int slot){return slot?svpu:qpu;}
void qpipe_begin(){
 if(external()||blocked()||paired()||!stream_max_fusion||epu_commit)
  throw std::runtime_error("Unsupported QK accumulator phase-reuse scope");
 if(ingress_mean_window_live||stage_in_window_live||append_source_active)throw std::runtime_error("QK descriptor aliases a live producer window");
 setphase("qk_reuse_pipeline");qpipe_active=true;qpipe_next=0;qpipe_retired=0;qpipe_max_slot=-1;
 for(auto&s:qpipe_slots)s=QPipeSlot{};
 if(qk_exact_exp_cache&&(qpipe_exp_cache.size()<unsigned(L)||masses.size()<unsigned(groups)))throw std::runtime_error("Incomplete charged exponent cache allocation");
 ++qpipe_heads;qpipe_valid_exp_begin=0;qpipe_head_exp_start=exact_exp_calls;
 int chunks=(hardware_groups()+15)/16,last=chunks-1,tokens_per_chunk=token_packed?512:256;
 qpipe_priority_batch=0;
 if(qk_priority_first){
  int guess=(qpipe_previous_valid&(1u<<h))?int(qpipe_previous_argmax[h])/tokens_per_chunk:last;
  qpipe_priority_batch=guess>=last-1?last:guess;
  if(qpipe_priority_batch<0||qpipe_priority_batch>last)throw std::runtime_error("Invalid causal priority prediction");
 }
 bool guard_bypass=(replay_pressure_guard&&qpipe_late_max_streak[h]>=2);
 if(guard_bypass)qpipe_late_max_streak[h]=0;
 qpipe_cache_this_head=(!guard_bypass&&qk_exact_exp_cache&&(qk_exp_policy=="all"||qk_priority_first||((qpipe_previous_valid&(1u<<h))&&4ull*qpipe_previous_argmax[h]<qpipe_previous_length[h])))||qk_online_softmax;
 qpipe_admitted_heads+=qpipe_cache_this_head;
 if(qk_exact_exp_cache)std::fill(qpipe_exp_valid.begin(),qpipe_exp_valid.end(),false);
}
void qpipe_progress(){
 if(!qpipe_active||phase!="qk_reuse_pipeline")throw std::runtime_error("QK pipeline phase mismatch");
 bool mac_pending=false,move_pending=false;
 for(const auto&kv:outstanding){mac_pending|=kv.second.op==QMac;move_pending|=kv.second.op==QMove;}
 for(const auto&t:tasks){mac_pending|=t.op==QMac;move_pending|=t.op==QMove;}
 if(qpipe_max_slot>=0){
  if(move_pending)throw std::runtime_error("Score SRAM readout/max port collision");
  qpipe_mac_with_epu_cycles+=mac_pending;
  if(clk>=epu_until){
   int slot=qpipe_max_slot;auto&s=qpipe_slots[slot];
   if((s.stage!=5&&s.stage!=6)||s.sequence!=qpipe_retired)throw std::runtime_error("QK maximum retirement ordering");
   int first=s.base*(token_packed?32:16),end=std::min(L,(s.base+16)*(token_packed?32:16));
   if(s.stage==5){
    uint32_t old_max=qpipe_readout_old_max_bits;
    
    
    qpipe_log("max_end",slot);
    bool max_changed=(old_max!=std::bit_cast<uint32_t>(ingress_max));
    if((qk_exact_exp_cache||qk_online_softmax)&&max_changed){qpipe_valid_exp_begin=first;std::fill(qpipe_exp_valid.begin(),qpipe_exp_valid.end(),false);++qpipe_invalidations;}
    bool stable=qk_priority_first?int(ingress_index)/(token_packed?512:256)==qpipe_priority_batch:4ull*ingress_index<unsigned(L);
    bool speculate=qk_online_softmax||(qpipe_cache_this_head&&(qk_exp_policy=="all"||(mc_channels==1&&s.sequence+1<(hardware_groups()+15)/16&&stable)));
    if(speculate){
     uint64_t tokens=end-first,gs=(tokens+15)/16,bytes=8*tokens+4*gs+(gs+7)/8;
     uint64_t work=std::max<uint64_t>(gs,(bytes+scratch_bpc-1)/scratch_bpc)+2;
     if(qk_online_softmax){
      uint64_t kv_row_switch=(s.sequence+1<(hardware_groups()+15)/16)?(qk_online_layout_aware?21:93):0;
      uint64_t pv_rmw=(s.sequence>0&&max_changed)?(std::max<uint64_t>((D+15)/16+4,(8ull*D+scratch_bpc-1)/scratch_bpc)+(qk_online_layout_aware?8:18)):0;
      work+=kv_row_switch+pv_rmw;
      if(s.sequence>0&&max_changed){++qpipe_online_rescale_events;qpipe_online_rescale_cycles+=pv_rmw;}
     }
     s.stage=6;epu_until=clk+work;qpipe_exp_epu_cycles+=work;qpipe_epu_cycles+=work;++qpipe_cached_batches;qpipe_log("exp_begin",slot);
    }
   }else{
    for(int g=first/16;g<(end+15)/16;g++){
     std::array<float,16>a{};
     for(int j=0;j<16;j++){int t=16*g+j;if(t<end){a[j]=qpipe_exp_cache[t]=counted_exp(scores[t]*ingress_scale-ingress_max);++qpipe_spec_exp_calls;}}
     for(int stride=1;stride<16;stride*=2)for(int j=0;j<16;j+=2*stride)a[j]+=a[j+stride];
     
     masses[g]=std::bit_cast<uint32_t>(a[0]);qpipe_exp_valid[g]=true;
    }
    qpipe_spec_score_read_bytes+=4ull*(end-first);qpipe_spec_exp_write_bytes+=4ull*(end-first);qpipe_group_sum_write_bytes+=4ull*((end-first+15)/16);
    qpipe_log("exp_end",slot);
   }
   if(s.stage==5||(s.stage==6&&clk>=epu_until)){
    s.stage=0;++qpipe_retired;++qpipe_release_checks;qpipe_max_slot=-1;
   }
  }
 }
 qpipe_mac_with_move_cycles+=mac_pending&&move_pending;
 
 
 bool readout_active=false;
 for(const auto&s:qpipe_slots)readout_active|=s.stage>=3;
 if(!readout_active&&qpipe_max_slot<0){
  int slot=qpipe_retired%qk_pipeline_depth;auto&s=qpipe_slots[slot];
  if(s.stage==2&&s.sequence==qpipe_retired){
   qpipe_readout_old_max_bits=std::bit_cast<uint32_t>(ingress_max);
   s.stage=3;s.move_left=(token_packed?16:8)*mc_channels;qpipe_log("readout_begin",slot);
   for(int hp=0;hp<(token_packed?2:1);hp++)for(int r=0;r<2;r++)for(int bg=0;bg<4;bg++)
    enqueue(QMove,r*RANK+bg*BG+hp*32,r,bg+4*hp,2*s.base+slot);
  }
 }
 if(qpipe_max_slot<0){
  int slot=qpipe_retired%qk_pipeline_depth;auto&s=qpipe_slots[slot];
  if(s.stage==4){
   if(move_pending||s.sequence!=qpipe_retired)throw std::runtime_error("QK readout/max boundary mismatch");
   int first=s.base*(token_packed?2:1),logical=std::min(groups-first,16*(token_packed?2:1));
   uint64_t work=uint64_t(logical)*max_group_cycles+max_batch_merge_cycles+(qk_exact_exp_cache?((((groups+7)/8)+scratch_bpc-1)/scratch_bpc):0);
   s.stage=5;qpipe_max_slot=slot;epu_until=clk+std::max<uint64_t>(1,work);
   qpipe_epu_cycles+=work;streamed_max_cycles+=work;++streamed_max_batches;qpipe_log("max_begin",slot);
  }
 }
 if(qpipe_next*16<hardware_groups()&&(!qk_readout_first||qk_pipeline_depth==1||qpipe_next!=1||qpipe_slots[0].stage>=4)){
  int slot=qpipe_next%qk_pipeline_depth;auto&s=qpipe_slots[slot];
  if(s.stage==0){
   int seq=qpipe_next++;int physical=seq;if(qk_priority_first)physical=seq==0?qpipe_priority_batch:(seq<=qpipe_priority_batch?seq-1:seq);
   s.sequence=seq;s.base=16*physical;s.stage=1;s.mac_left=std::min(16,hardware_groups()-s.base)*dtiles;s.move_left=0;
   std::fill(qpipe_acc(slot).begin(),qpipe_acc(slot).end(),0);++qpipe_batches;qpipe_log("acquire",slot);
   for(int g=s.base;g<std::min(hardware_groups(),s.base+16);g++)for(int t=0;t<dtiles;t++)
    enqueue(QMac,32ll*(g*dtiles+t),g,t,2*s.base+slot);
  }
 }
 uint64_t live=0;for(const auto&s:qpipe_slots)live+=s.stage!=0;qpipe_peak_slots=std::max(qpipe_peak_slots,live);
 if(qpipe_retired==(hardware_groups()+15)/16){
  if(live||qpipe_max_slot>=0||!tasks.empty()||!outstanding.empty())throw std::runtime_error("QK pipeline released live state");
  if(ingress_seen!=unsigned(L))throw std::runtime_error("QK pipeline lost score coverage");
  qpipe_active=false;setphase("sfm_kick");enqueue(SfmKick,0);
 }
}
bool qpipe_complete(const Task&t){
 if(!qpipe_active||(t.op!=QMac&&t.op!=QMove))return false;
 int slot=t.z&1,base=t.z/2;auto&s=qpipe_slots[slot];auto&a=qpipe_acc(slot);
 if(t.z<0||base%16||slot>=qk_pipeline_depth||s.base!=base||t.q!=q||t.h!=h||fault=="qpipe_stale_slot")throw std::runtime_error("QK pipeline stale slot/owner");
 if(t.op==QMac){
  if(s.stage!=1||s.mac_left<=0||t.x<base||t.x>=std::min(hardware_groups(),base+16))throw std::runtime_error("QK pipeline MAC ownership");
  if(token_packed){
   if(t.y!=0||t.addr!=32ll*t.x)throw std::runtime_error("QK pipeline packed address mismatch");
   for(int stripe=0;stripe<16;stripe++)for(int b=0;b<4;b++)for(int j=0;j<16;j++){
    int dim=(b%2)*16+j,token=(2*t.x+b/2)*16+stripe,ix=(t.x-base)*64+stripe*4+b;
    a[ix]=std::fma(kval(token,dim),hfloat(qgb[b*16+j]),a[ix]);
   }
   ++token_qmac;
  }else{
   if(t.addr/32/dtiles!=t.x||t.addr/32%dtiles!=t.y)throw std::runtime_error("QK pipeline scalar address mismatch");
   for(int stripe=0;stripe<16;stripe++)for(int b=0;b<4;b++)for(int j=0;j<16;j++){
    int dim=t.y*64+b*16+j,token=t.x*16+stripe,ix=(t.x-base)*64+stripe*4+b;
    a[ix]=std::fma(kval(token,dim),hfloat(qgb[dim]),a[ix]);
   }
  }
  ++count_qmac;++qpipe_mac;
  if(--s.mac_left==0){s.stage=2;qpipe_log("mac_end",slot);}
 }else{
  if(s.stage!=3||s.move_left<=0||qpipe_max_slot>=0)throw std::runtime_error("QK pipeline readout ownership/port");
  int hp=token_packed?t.y/4:0,bg=token_packed?t.y%4:t.y;
  for(int pc=0;pc<2;pc++)for(int group=0;group<std::min(16,hardware_groups()-s.base);group++){
   int stripe=pc*8+t.x*4+bg,token=(token_packed?2*(s.base+group)+hp:s.base+group)*16+stripe;float x=0;
   if(token_packed){x+=a[group*64+stripe*4+hp*2];x+=a[group*64+stripe*4+hp*2+1];x+=0.f;x+=0.f;}
   else for(int b=0;b<4;b++)x+=a[group*64+stripe*4+b];
   if(token<L&&mc_token_channel(token)==t.channel){scores[token]=x;observe_score(token,x);}
  }
  ++qpipe_moves;if(--s.move_left==0){s.stage=4;qpipe_log("readout_end",slot);}
 }
 return true;
}
