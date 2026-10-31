int mc_channels=1;
std::vector<std::vector<float>> mc_svpu,mc_rankout;
std::vector<std::array<uint16_t,64>> mc_pu_max,mc_gathered_max;
std::vector<std::array<bool,64>> mc_gathered_valid;
std::vector<Clk_t> mc_observer_deadline;
std::vector<uint64_t> mc_requests=std::vector<uint64_t>(4,0);
uint64_t mc_merge_cycles=0,mc_merge_bytes=0;
int mc_token_channel(int token)const{return (token/32)%mc_channels;}
struct MCScope{
 NumericPIMDRAM8*p;int c;bool active;
 MCScope(NumericPIMDRAM8*p_,int c_):p(p_),c(c_),active(p_->mc_channels>1){if(active){p->svpu.swap(p->mc_svpu[c]);p->rankout.swap(p->mc_rankout[c]);}}
 ~MCScope(){if(active){p->svpu.swap(p->mc_svpu[c]);p->rankout.swap(p->mc_rankout[c]);}}
};
void mc_enqueue(int op,Addr_t addr,int x,int y,int z){
 bool broadcast=op==QWrite||op==QMove||op==PMove||op==VMove||op==StatMove;
 int channel=0;
 if(op==QMac||op==VMac)channel=(token_packed?x:x/2)%mc_channels;
 else if(op==EncoderRawRead||op==EncoderKWrite||op==AppendK||op==AppendV||op==NormLoad)channel=mc_token_channel(x);
 for(int c=0;c<(broadcast?mc_channels:1);c++){
  int dest=broadcast?c:channel;
  tasks.push_back({next_id++,op,x,y,z,q,h,addr,0,dest});++mc_requests[dest];
 }
 if(next_id<0)throw std::runtime_error("Request ID exhausted");
}
uint64_t mc_merge_service(){
 if(mc_channels==1)return 0;
 uint64_t dims=2*(token_packed?64:std::min<int>(64,D-dtile*64)),bytes=4*dims*(mc_channels+1),ops=dims*(mc_channels-1);
 uint64_t work=std::max((bytes+scratch_bpc-1)/scratch_bpc,(ops+port_alu_lanes-1)/port_alu_lanes)+max_batch_merge_cycles;
 mc_merge_bytes+=bytes;mc_merge_cycles+=work;return work;
}
void mc_merge_rankout(){
 if(mc_channels==1)return;
 std::fill(rankout.begin(),rankout.end(),0);
 for(int c=0;c<mc_channels;c++)for(int i=0;i<128;i++)rankout[i]+=mc_rankout[c][i];
}
uint64_t mc_stat_service(){
 uint64_t bytes=128ull*(mc_channels+1),ops=64ull*(mc_channels-1),work=std::max((bytes+scratch_bpc-1)/scratch_bpc,(ops+port_alu_lanes-1)/port_alu_lanes)+max_batch_merge_cycles;
 mc_merge_bytes+=bytes;mc_merge_cycles+=work;return work;
}
void mc_merge_statistics(){
 gathered_max.fill(0);gathered_valid.fill(true);
 for(int c=0;c<mc_channels;c++)for(int i=0;i<64;i++){
  if(!mc_gathered_valid[c][i])throw std::runtime_error("Incomplete per-channel amax return");
  gathered_max[i]=std::max(gathered_max[i],mc_gathered_max[c][i]);
 }
}
