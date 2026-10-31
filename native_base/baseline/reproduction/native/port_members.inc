  
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
