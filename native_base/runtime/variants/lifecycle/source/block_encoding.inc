
std::string block_strategy="global";
int block_size=64,block_prepared_q=-1,block_prepared_h=-1;
std::vector<float> block_default,block_anchor_backing,block_query_anchors,block_corrections;
std::vector<uint8_t> block_flags,block_flags_backing,block_flags_loaded;
std::vector<bool> block_published;
YAML::Node block_jobs;
uint64_t block_service_cycles=0,block_query_reads=0,block_producer_reads=0,block_meta_writes=0;
static constexpr Addr_t BLOCK_FLAGS=ENCODER_ANCHOR+(1ll<<19);
bool blocked()const{return encoder_enabled&&block_strategy!="global";}
bool hybrid_block()const{return block_strategy=="hybrid";}
int nblocks()const{return (nmax+block_size-1)/block_size;}
Addr_t block_meta_addr(int block,int packet)const{
  return block<0?BLOCK_FLAGS+32ll*packet:ENCODER_ANCHOR+4ll*D*block+32ll*packet;
}
void block_charge(const std::string&tag,uint64_t add,uint64_t div,uint64_t sub,uint64_t round,uint64_t fma,uint64_t compare,uint64_t rd,uint64_t wr,std::function<void()> action){
  uint64_t ops=add+div+sub+round+fma+compare;
  uint64_t cycles=std::max((ops+port_alu_lanes-1)/port_alu_lanes,(rd+wr+scratch_bpc-1)/scratch_bpc)+max_batch_merge_cycles;
  YAML::Node x;x["stage"]=tag;x["query"]=q;x["head"]=h;x["initial"]=encoder_initial;
  x["tile_begin"]=encoder_cursor;x["tile_end"]=encoder_end;x["add_ops"]=add;x["div_ops"]=div;x["sub_ops"]=sub;x["round_ops"]=round;x["fma_ops"]=fma;x["compare_abs_ops"]=compare;
  x["scratch_read_bytes"]=rd;x["scratch_write_bytes"]=wr;x["alu_lanes"]=port_alu_lanes;x["scratch_bytes_per_cycle"]=scratch_bpc;x["cycles"]=cycles;x["start"]=clk;x["end"]=clk+cycles;block_jobs.push_back(x);block_service_cycles+=cycles;
  setphase(tag);epuservice(cycles,std::move(action));
}
void block_initial(){
  encoder_initial=true;encoder_cursor=0;encoder_key_published.assign(nmax*(D/16),false);
  block_default.assign(D,0);block_anchor_backing.assign(uint64_t(nblocks())*D,0);block_query_anchors.assign(uint64_t(nblocks())*D,0);
  block_flags.assign(nblocks(),0);block_flags_backing.assign(nblocks(),0);block_flags_loaded.assign(nblocks(),0);block_published.assign(nblocks(),false);block_corrections.assign(nblocks(),0);
  encoder_anchor_dram.assign(D,0);encoder_loaded_anchor.assign(D,0);encoder_anchor.assign(D,0);
  std::fill(K.begin(),K.end(),0);encoder_read_tile(false);
}
void block_source_ready(){
  for(bool x:encoder_raw_valid)if(!x)throw std::runtime_error("Incomplete one-pass source tile");
  int b=encoder_cursor/block_size;uint64_t E=uint64_t(encoder_end-encoder_cursor)*D;
  if(block_published[b]){
    
    block_encode();return;
  }
  block_charge("block_mean",E-D,D,0,0,0,0,8*E-4ull*D,8*E+4ull*D,[this]{
    encoder_anchor.assign(encoder_tile.begin(),encoder_tile.begin()+D);
    for(int t=encoder_cursor+1;t<encoder_end;t++)for(int d=0;d<int(D);d++)encoder_anchor[d]+=encoder_tile[(t-encoder_cursor)*D+d];
    for(auto&x:encoder_anchor)x/=float(encoder_end-encoder_cursor);
    if(encoder_cursor==0)block_default=encoder_anchor;
  });
}
void block_choose(){
  int b=encoder_cursor/block_size;uint64_t E=uint64_t(encoder_end-encoder_cursor)*D;
  if(!hybrid_block()||b==0){
    block_flags[b]=1;encoder_loaded_anchor=encoder_anchor;block_encode();return;
  }
  
  
  block_charge("block_choose",0,0,4*E,2*E,0,4*E,12*E,4*E,[this,b]{
    float e0=0,eb=0;
    for(int t=encoder_cursor;t<encoder_end;t++)for(int d=0;d<int(D);d++){
      float k=encoder_tile[(t-encoder_cursor)*D+d],r0=k-block_default[d],rb=k-encoder_anchor[d];
      e0=std::max(e0,std::abs(hfloat(hbits(r0))-r0));eb=std::max(eb,std::abs(hfloat(hbits(rb))-rb));
    }
    block_flags[b]=e0>2*eb;encoder_loaded_anchor=block_flags[b]?encoder_anchor:block_default;
  });
}
void block_encode(){
  uint64_t E=uint64_t(encoder_end-encoder_cursor)*D;
  block_charge("block_encode",0,0,E,E,0,0,10*E,2*E,[this]{
    for(int t=encoder_cursor;t<encoder_end;t++)for(int d=0;d<int(D);d++){
      float x=encoder_tile[(t-encoder_cursor)*D+d]-encoder_loaded_anchor[d];uint16_t v=hbits(x);
      if(!std::isfinite(hfloat(v)))throw std::runtime_error("Nonfinite block residual");encoder_half[(t-encoder_cursor)*D+d]=v;
    }
  });
}
void block_publish_meta(){
  int b=encoder_cursor/block_size;
  if(block_published[b]){encoder_write_tile();return;}
  uint64_t payload=(b==0||block_flags[b])?4ull*D:0;
  block_charge("block_meta_payload",0,0,0,0,0,2,payload+32,32,[this]{});
}
void block_meta_writes_begin(){
  int b=encoder_cursor/block_size;setphase("block_meta_write");
  if(b==0||block_flags[b])for(int p=0;p<int(D)/8;p++)enqueue(EncoderAnchorWrite,block_meta_addr(b,p),p,b);
  enqueue(EncoderAnchorWrite,block_meta_addr(-1,b/256),b/256,-1);
}
void block_anchor_write_complete(const Task&t){
  if(t.y<0){
    if(t.addr!=block_meta_addr(-1,t.x))throw std::runtime_error("Flag write physical mismatch");
    for(int i=t.x*256;i<std::min(nblocks(),(t.x+1)*256);i++)block_flags_backing[i]=block_flags[i];
  }else{
    int b=t.y;if(t.addr!=block_meta_addr(b,t.x))throw std::runtime_error("Block anchor WR mismatch");
    for(int j=0;j<8;j++)block_anchor_backing[b*D+t.x*8+j]=encoder_loaded_anchor[t.x*8+j];
    if(b==0){encoder_anchor_dram=encoder_loaded_anchor;block_default=encoder_loaded_anchor;}
  }
  ++encoder_anchor_writes;++block_meta_writes;
}
void block_anchor_read_complete(const Task&t){
  if(t.addr!=block_meta_addr(t.y,t.x))throw std::runtime_error("Block metadata RD mismatch");
  if(t.y<0){
    for(int i=t.x*256;i<std::min(nblocks(),(t.x+1)*256);i++)block_flags_loaded[i]=block_flags_backing[i];
  }else{
    if(!block_published[t.y])throw std::runtime_error("Read unpublished block anchor");
    for(int j=0;j<8;j++){
      float x=block_anchor_backing[t.y*D+t.x*8+j];
      if(phase=="block_query_anchor_read")block_query_anchors[t.y*D+t.x*8+j]=x;
      else encoder_loaded_anchor[t.x*8+j]=x;
    }
  }
  ++encoder_anchor_reads;
  if(phase=="block_query_flags"||phase=="block_query_anchor_read")++block_query_reads;else ++block_producer_reads;
}
void block_query_begin(){
  setphase("block_query_flags");int nb=(L+block_size-1)/block_size;
  for(int p=0;p<(nb+255)/256;p++)enqueue(EncoderAnchorRead,block_meta_addr(-1,p),p,-1);
}
void block_query_anchors_begin(){
  setphase("block_query_anchor_read");int nb=(L+block_size-1)/block_size;
  for(int b=0;b<nb;b++)if(b==0||block_flags_loaded[b])for(int p=0;p<int(D)/8;p++)enqueue(EncoderAnchorRead,block_meta_addr(b,p),p,b);
}
void block_query_compute(){
  int nb=(L+block_size-1)/block_size,active=0;
  for(int b=1;b<nb;b++)active+=bool(block_flags_loaded[b]);
  uint64_t E=uint64_t(active)*D,flags=32ull*((nb+255)/256);
  block_charge("block_query_correct",0,0,E,0,E,nb,10*E+2ull*D+flags,4*E+4ull*nb+2ull*D+flags,[this,nb]{
    std::fill(block_corrections.begin(),block_corrections.end(),0);
    for(int b=1;b<nb;b++)if(block_flags_loaded[b]){
      float dot=0;
      for(int d=0;d<int(D);d++)dot=std::fma(hfloat(queries[(q*H+h)*D+d]),block_query_anchors[b*D+d]-block_query_anchors[d],dot);
      block_corrections[b]=dot;
    }
    block_prepared_q=q;block_prepared_h=h;
  });
}
float block_corrected(int token,float value)const{return blocked()?value+block_corrections[token/block_size]:value;}
uint64_t block_score_work(){
  if(!blocked())return 0;
  int start=(token_packed?32:16)*batch,end=std::min(L,(token_packed?32:16)*(batch+16));uint64_t count=end-start;
  return std::max((count+port_alu_lanes-1)/port_alu_lanes,(12*count+scratch_bpc-1)/scratch_bpc)+max_batch_merge_cycles;
}
bool block_advance(){
  if(!blocked())return false;
  if(phase=="block_mean"){block_choose();return true;}
  if(phase=="block_choose"){block_encode();return true;}
  if(phase=="block_encode"){block_publish_meta();return true;}
  if(phase=="block_meta_payload"){block_meta_writes_begin();return true;}
  if(phase=="block_meta_write"){block_published[encoder_cursor/block_size]=true;encoder_write_tile();return true;}
  if(phase=="block_query_flags"){block_query_anchors_begin();return true;}
  if(phase=="block_query_anchor_read"){block_query_compute();return true;}
  if(phase=="block_query_correct"){issue_q_writes();return true;}
  return false;
}
