



   
from common import *
from concurrent.futures import ThreadPoolExecutor,as_completed
import argparse,threading,tempfile,subprocess,os,time,tarfile,shutil,yaml,gc,ctypes,fcntl,ijson
ap=argparse.ArgumentParser();ap.add_argument('--tasks',type=Path,required=True);ap.add_argument('--workers',type=int,default=2);ap.add_argument('--host-light-lane',action='store_true');ap.add_argument('--second-low-lane',action='store_true');ap.add_argument('--host-cache-lane',action='store_true');ap.add_argument('--defer-busy-cases',action='store_true');a=ap.parse_args();tasks=read(a.tasks);lock=threading.Lock()
def log(v):
 with lock:
  with(U/'NATIVE_ATTEMPTS.jsonl').open('a')as f:f.write(json.dumps(dict(time=time.time(),**v))+'\n')
def atomic(p,value):
 p=Path(p);p.parent.mkdir(parents=True,exist_ok=True);q=p.parent/('.commit_'+p.name);save(q.with_suffix('.gz')if p.suffix=='.gz'else q,value)
                                                                           
 q=q.with_suffix('.gz')if p.suffix=='.gz'else q;os.replace(q,p)
def certificate(rp):
 rp=resolve(rp);p=rp.parent/'ARRAY_CERTIFICATE.json';h=sha(rp)
 with(U/'HOST_CERTIFICATE_LOCK').open('a')as guard:
  fcntl.flock(guard,fcntl.LOCK_EX)
  if p.exists():
   z=read(p);assert z['record_sha256']==h;return z
  z={'arrays':{},'record_sha256':h}
  with gzip.open(rp,'rb')as f:
   for prefix,event,value in ijson.parse(f):
    if prefix in ['status','archive_sha256','native_cycles']and event in ['string','number']:z[prefix]=value
    elif prefix=='result.callbacks'and event=='number':z['callbacks']=int(value)
    elif prefix.startswith('arrays.')and event=='string':z['arrays'][prefix[7:]]=value
  assert len(z['arrays'])==6 and z['callbacks']>0;atomic(p,z);return z
def _run(t):
 dest=U/'native'/t['name'];rp=dest/'record.json.gz'
 if rp.exists():
  with gzip.open(rp,'rb')as f:previous_task=next(ijson.items(f,'task'))
  r=certificate(rp);assert previous_task==t and sha(dest/'raw.tar.gz')==r['archive_sha256'];return r['callbacks']
                                                                              
                                                                          
 if dest.exists()and any(dest.iterdir()):
  evidence=U/'host_interruptions'/'automatic_uncommitted_replay'/(t['name']+'__'+str(time.time_ns()))
  evidence.parent.mkdir(parents=True,exist_ok=True);shutil.copytree(dest,evidence)
  save(evidence/'REPLAY_CLASSIFICATION.json',dict(reason='Uncommitted attempt only; exact registered case replay; no acceptance or data selection',task=t))
  shutil.rmtree(dest)
 dest.mkdir(parents=True,exist_ok=True)
 for k in ['input','raw_keys','reference']:assert resolve(t[k]).is_file(),(t['name'],k)
 expected=certificate(t['expected_record'])if t.get('expected_record')else None
 with tempfile.TemporaryDirectory(prefix='dgpim_native_',dir=os.environ.get('DG_PIM_TMP_DIR', '/dev/shm' if Path('/dev/shm').is_dir() else None))as td:
  d=Path(td);binary=resolve(t['binary']);lib=binary.parent
  cfg=yaml.load((R/'native_base/baseline/reproduction/native/BASE_CONFIG.yaml').read_text(),Loader=yaml.CSafeLoader);m=cfg['MemorySystem']
  m.update(fixture=str(resolve(t['input'])),output_directory=str(d),mode=t['mode'],external_policy='none',epsilon=.1,cache_initialization=t['cache'],stream_max_fusion=True,inplace_exp=True,actor_inflight_cap=64,max_group_cycles=1,max_batch_merge_cycles=4,encoder_enabled=True,encoder_raw_keys=str(resolve(t['raw_keys'])),port_alu_lanes=t['alu'],scratch_bytes_per_cycle=t['bytes'],watchdog=200000000)
  m.update(t['parameters']);m['Controller']['plugins'][0]['ControllerPlugin']['path']=str(d/'commands.log');m['DRAM']['org']['channel']=t['channels'];m['Controller'].update(t.get('controller_parameters',{}));(d/'config.yaml').write_text(yaml.safe_dump(cfg,sort_keys=False))
  ids={k:sha(p)for k,p in [('library',lib/'libramulator.so'),('input',resolve(t['input'])),('raw_keys',resolve(t['raw_keys'])),('pipeline_auditor',resolve(t['pipeline_auditor']))]};log(dict(event='start',name=t['name'],identities=ids));begin=time.monotonic()
  gate=None
  try:
   save(d/'RUN_TASK.json',t)
   with(d/'stdout.log').open('w')as stdout,(d/'stderr.log').open('w')as stderr:
    p=subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(binary),'-f',str(d/'config.yaml')],env=dict(os.environ,LD_LIBRARY_PATH=str(lib),OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1'),stdout=stdout,stderr=stderr,timeout=3600)
   assert p.returncode==0,(t['name'],p.returncode,(d/'stderr.log').read_text()[-2000:])
                                                                             
                                                                           
   gate=(U/'HOST_ANALYSIS_LOCK').open('a');fcntl.flock(gate,fcntl.LOCK_EX)
   z=yaml.load((d/'result.yaml').read_text(),Loader=yaml.CSafeLoader);assert z['status']=='completed'and z['submitted']==z['callbacks'];assert sum(x['cycles']for x in z['phases'])+1==z['actor_end_cycle'];assert z['peak_live_scratch_bytes']<=t['capacity']
   p=subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(resolve(t['auditor'])),str(d)],capture_output=True,text=True);assert p.returncode==0,p.stderr;pa=json.loads(p.stdout);assert pa['status']=='PASS'and pa['events']==z['callbacks'];save(d/'physical_audit.json',pa)
   p=subprocess.run([str(resolve(t['pipeline_auditor'])),str(d)],capture_output=True,text=True);assert p.returncode==0,p.stderr;qa=json.loads(p.stdout);assert qa['batches']==z['qpipe_batches']and qa['qk_callbacks']==z['qpipe_mac']and qa['readout_callbacks']==z['qpipe_moves']and qa['physical_readout_epu_overlap_cycles']==0;save(d/'pipeline_audit.json',qa)
   with(d/'events.tsv').open('rb')as f:p=subprocess.run([str(U/'scripts/audit_mac_order'),str(z['D'])],stdin=f,capture_output=True);assert p.returncode==0,p.stderr;ma=json.loads(p.stdout);assert ma['callbacks']==z['qk_mac'];save(d/'mac_order_audit.json',ma)
   names=['output.f32','scores.f32','probability.f16','mask.u8','encoded_keys.f16','anchor.f32'];arrays={n:sha(d/n)for n in names}
   if expected:
    assert arrays==expected['arrays'],('SIX_ARRAY_CONTRACT_CHANGED',t['name'])
    if t.get('expected_equal_cycles'):assert z['actor_end_cycle']==expected['native_cycles']
   else:assert t['arm']=='serial'and t['producer']=='L'
   files={p.name:dict(bytes=p.stat().st_size,sha256=sha(p))for p in d.iterdir()if p.is_file()};omitted={n:files.pop(n)for n in names if expected and n!='output.f32'}
   if omitted:save(d/'UNCHANGED_ARRAY_REFERENCES.json',dict(reference=t['expected_record'],archive_sha256=expected['archive_sha256'],arrays=omitted));files['UNCHANGED_ARRAY_REFERENCES.json']=dict(bytes=(d/'UNCHANGED_ARRAY_REFERENCES.json').stat().st_size,sha256=sha(d/'UNCHANGED_ARRAY_REFERENCES.json'))
   stage=dest/'.rsync-tmp';stage.mkdir();archive=stage/'raw.tar.gz'
   with tarfile.open(archive,'w:gz',compresslevel=5)as tar:
    for n in sorted(files):tar.add(d/n,arcname=n,recursive=False)
   seen=set()
   with tarfile.open(archive,'r|gz')as tar:
    for x in tar:
     assert x.name in files and x.size==files[x.name]['bytes'];assert hashlib.file_digest(tar.extractfile(x),'sha256').hexdigest()==files[x.name]['sha256'];seen.add(x.name)
   assert seen==set(files);os.replace(archive,dest/'raw.tar.gz');stage.rmdir();shutil.copy2(d/'output.f32',dest/'output.f32')
   record=dict(status='PASS_NATIVE_PHYSICAL_PIPELINE_MAC_ORDER_AND_ARRAY_CONTRACT',task=t,identities=ids,result=z,native_cycles=z['actor_end_cycle'],arrays=arrays,physical_audit=pa,pipeline_audit=qa,mac_order_audit=ma,raw_files=files,unchanged_array_references=omitted,archive_sha256=sha(dest/'raw.tar.gz'),expected_record=t.get('expected_record'),host_seconds_diagnostic=time.monotonic()-begin)
   atomic(rp,record);atomic(dest/'ARRAY_CERTIFICATE.json',dict(status=record['status'],arrays=arrays,native_cycles=z['actor_end_cycle'],callbacks=z['callbacks'],archive_sha256=record['archive_sha256'],record_sha256=sha(rp)));log(dict(event='committed',name=t['name'],cycles=z['actor_end_cycle'],callbacks=z['callbacks'],record_sha256=sha(rp)));answer=z['callbacks'];del record,z,p,pa,qa,ma;gc.collect();ctypes.CDLL(None).malloc_trim(0);return answer
  except BaseException as e:
   for n in ['config.yaml','stdout.log','stderr.log']:
    if(d/n).exists():shutil.copy2(d/n,dest/n)
   shutil.copytree(d,dest/'FAILED_NATIVE_EVIDENCE',dirs_exist_ok=True)
   log(dict(event='failure',name=t['name'],error=repr(e)));raise
  finally:
   if gate is not None:fcntl.flock(gate,fcntl.LOCK_UN);gate.close()
def run(t):
                                                    
                                                                            
 if rec(t['name']).exists():return _run(t)
 h=fixture_header(resolve(t['input']));locks=U/'HOST_CASE_LOCKS';locks.mkdir(exist_ok=True)
 with(locks/t['name']).open('a')as caseguard:
  try:fcntl.flock(caseguard,fcntl.LOCK_EX|(fcntl.LOCK_NB if a.defer_busy_cases else 0))
  except BlockingIOError:
   log(dict(event='host_deferred_busy_case',name=t['name']));return 0
  if a.host_cache_lane:
                                                                              
                                                                               
                                                                               
   assert h['Q']==2048 and h['L0']==257 and h['H']==7 and h['D']==64 and t['arm']=='full'and t['parameters']['qk_exact_exp_cache']
   with(U/'HOST_SINGLE_LIGHT_LANE_LOCK').open('a')as firstguard,(U/'HOST_SECOND_LOW_LANE_LOCK').open('a')as secondguard:
    fcntl.flock(firstguard,fcntl.LOCK_EX);fcntl.flock(secondguard,fcntl.LOCK_EX);return _run(t)
  if a.second_low_lane:
   assert h['Q']==2048 and h['L0']==257 and h['H']<=3 and h['D']<=128
   with(U/'HOST_SECOND_LOW_LANE_LOCK').open('a')as secondguard:
    fcntl.flock(secondguard,fcntl.LOCK_EX);return _run(t)
  if a.host_light_lane:
   assert h['L0']==257 and ((h['Q']==1024 and h['H']*h['D']<=448)or(h['Q']==2048 and h['H']<=3 and h['D']<=128))
   with(U/'HOST_SINGLE_LIGHT_LANE_LOCK').open('a')as lightguard:
    fcntl.flock(lightguard,fcntl.LOCK_EX);return _run(t)
  with(U/'HOST_NATIVE_RESOURCE_LOCK').open('a')as guard:
   fcntl.flock(guard,fcntl.LOCK_EX if h['Q']>=2048 else fcntl.LOCK_SH)
   return _run(t)
callbacks=0
with ThreadPoolExecutor(max_workers=a.workers)as pool:
 futures=[pool.submit(run,t)for t in tasks]
 for i,f in enumerate(as_completed(futures),1):
  callbacks+=f.result()
  if i%5==0 or i==len(tasks):print('NATIVE_COMPLETE',i,len(tasks),flush=True)
missing=[t['name']for t in tasks if not rec(t['name']).exists()]
assert not missing or a.defer_busy_cases
summary=dict(status='PARTIAL_BUSY_CASES_DEFERRED'if missing else'ALL_REGISTERED_CASES_COMPLETE',runs=len(tasks)-len(missing),requested=len(tasks),pending_busy_cases=missing,callbacks=callbacks,task_manifest_sha256=sha(a.tasks));save(U/'results'/(a.tasks.stem+'_COMPLETE.json'),summary);print(json.dumps(summary),flush=True)
