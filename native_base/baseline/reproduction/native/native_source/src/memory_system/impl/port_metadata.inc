  
  
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
