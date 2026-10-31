




   
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor,as_completed
import argparse,gzip,hashlib,json,os,subprocess,tarfile,tempfile,threading,time,datetime,yaml,shutil
from raw_evidence import verify_archive
R=Path(__file__).resolve().parents[3];C=R/'output/consumer';B=R/'native_base/baseline'
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def read(p):
 p=Path(p)
 with (gzip.open(p,'rt')if p.suffix=='.gz'else p.open())as f:return json.load(f)
def save(p,x):
 p=Path(p);p.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.NamedTemporaryFile(dir=p.parent,prefix='.commit_',delete=False)as f:tmp=Path(f.name)
 if p.suffix=='.gz':
  with gzip.open(tmp,'wt',compresslevel=6)as f:json.dump(x,f,separators=(',',':'),allow_nan=False)
 else:tmp.write_text(json.dumps(x,indent=2,allow_nan=False))
 os.replace(tmp,p)
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--tasks',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--workers',type=int,default=2);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True);tasks=read(a.tasks);lock=threading.Lock()
 def log(x):
  with lock:
   with (a.output/'ATTEMPTS.jsonl').open('a')as f:f.write(json.dumps(dict(time=datetime.datetime.now(datetime.timezone.utc).isoformat(),**x))+'\n')
 def run(t):
  dest=a.output/t['name'];rp=dest/'record.json.gz'
  if rp.exists():
   z=read(rp);assert z['task']==t;verify_archive(dest,z['archive_sha256']);return z['result']['callbacks']
  dest.mkdir(exist_ok=True);assert not any(dest.iterdir()),('Retain and classify uncommitted attempt before replay',dest)
  with tempfile.TemporaryDirectory(prefix='native_consumer_',dir='/dev/shm')as td:
   d=Path(td);binary=Path(t['binary']);lib=binary.parent;auditor=Path(t['auditor']);pipeline_auditor=Path(t.get('pipeline_auditor',str(C/'scripts/audit_pipeline')));inp=Path(t['input']);raw=Path(t['raw_keys'])
   cfg=yaml.load((B/'reproduction/native/BASE_CONFIG.yaml').read_text(),Loader=yaml.CSafeLoader);m=cfg['MemorySystem'];m.update(fixture=str(inp),output_directory=str(d),mode=t['mode'],external_policy='none',epsilon=.1,cache_initialization=t['cache'],stream_max_fusion=True,inplace_exp=True,actor_inflight_cap=64,max_group_cycles=1,max_batch_merge_cycles=4,encoder_enabled=True,encoder_raw_keys=str(raw),port_alu_lanes=t.get('alu',16),scratch_bytes_per_cycle=t.get('bytes',32),watchdog=200000000);m.update(t.get('parameters',{}));m['Controller']['plugins'][0]['ControllerPlugin']['path']=str(d/'commands.log');m['DRAM']['org']['channel']=t.get('channels',1)
   if 'controller_parameters'in t:m['Controller'].update(t['controller_parameters'])
   (d/'config.yaml').write_text(yaml.safe_dump(cfg,sort_keys=False));ids={k:sha(p)for k,p in [('binary',binary),('library',lib/'libramulator.so'),('auditor',auditor),('pipeline_auditor',pipeline_auditor),('input',inp),('raw_keys',raw)]};log(dict(event='start',task=t,identities=ids));beg=time.monotonic()
   try:
    p=subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(binary),'-f',str(d/'config.yaml')],env=dict(os.environ,LD_LIBRARY_PATH=str(lib),OPENBLAS_NUM_THREADS='1',OMP_NUM_THREADS='1'),capture_output=True,text=True,timeout=3600);(d/'stdout.log').write_text(p.stdout);(d/'stderr.log').write_text(p.stderr);assert p.returncode==0,(t['name'],p.returncode,p.stderr[-1500:],p.stdout[-1500:])
    z=yaml.load((d/'result.yaml').read_text(),Loader=yaml.CSafeLoader);assert z['status']=='completed'and z['submitted']==z['callbacks'];assert sum(p['cycles']for p in z['phases'])+1==z['actor_end_cycle'];assert z['peak_live_scratch_bytes']<=t.get('scratch_capacity_bytes',262144)
    physical=subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(auditor),str(d)],capture_output=True,text=True);assert physical.returncode==0,physical.stderr;pa=json.loads(physical.stdout);assert pa['status']=='PASS'and pa['events']==z['callbacks'];save(d/'physical_audit.json',pa)
    pipe=subprocess.run([str(pipeline_auditor),str(d)],capture_output=True,text=True);assert pipe.returncode==0,pipe.stderr;qa=json.loads(pipe.stdout);assert qa['batches']==z['qpipe_batches']and qa['qk_callbacks']==z['qpipe_mac']and qa['readout_callbacks']==z['qpipe_moves']and qa['exp_jobs']==z.get('qpipe_cached_batches',0);save(d/'pipeline_audit.json',qa)
    names=['output.f32','scores.f32','probability.f16','mask.u8','encoded_keys.f16','anchor.f32'];arrays={n:sha(d/n)for n in names};old=read(t['expected_record']);assert arrays==old['arrays'],('SIX_ARRAY_CONTRACT_MISMATCH',t['name']);expected=dict(six_arrays_equal=True,cycles_equal=z['actor_end_cycle']==old['native_cycles'],reference_record_sha256=sha(t['expected_record']),reference_record=t['expected_record'])
    if t.get('expected_equal_cycles'):assert expected['cycles_equal']
    observed={p.name:dict(bytes=p.stat().st_size,sha256=sha(p))for p in d.iterdir()if p.is_file()};omitted={n:observed[n]for n in names if n!='output.f32'}
    files={n:v for n,v in observed.items()if n not in omitted}
    save(d/'UNCHANGED_ARRAY_REFERENCES.json',dict(scope='Observed new native bytes were hashed and matched the frozen reference before release; canonical array bytes are in that reference archive.',reference_record=t['expected_record'],reference_archive_sha256=old['archive_sha256'],arrays=omitted));files['UNCHANGED_ARRAY_REFERENCES.json']=dict(bytes=(d/'UNCHANGED_ARRAY_REFERENCES.json').stat().st_size,sha256=sha(d/'UNCHANGED_ARRAY_REFERENCES.json'))
    stage=dest/'.rsync-tmp';stage.mkdir(exist_ok=True);archive=stage/'raw.tar.gz'
    with tarfile.open(archive,'w:gz',compresslevel=5)as tar:
     for n in sorted(files):tar.add(d/n,arcname=n,recursive=False)
    seen=set()
    with tarfile.open(archive,'r|gz')as tar:
     for item in tar:
      assert item.name in files and item.size==files[item.name]['bytes'];f=tar.extractfile(item);h=hashlib.sha256()
      while data:=f.read(1048576):h.update(data)
      assert h.hexdigest()==files[item.name]['sha256'];seen.add(item.name)
    assert seen==set(files);os.replace(archive,dest/'raw.tar.gz');stage.rmdir();shutil.copy2(d/'output.f32',dest/'output.f32')
    result=dict(status='PASS_NATIVE_COMMANDS_PIPELINE_OWNERSHIP_SIX_ARRAYS',task=t,identities=ids,result=z,native_cycles=z['actor_end_cycle'],arrays=arrays,physical_audit=pa,pipeline_audit=qa,raw_files=files,unchanged_array_references=omitted,archive_sha256=sha(dest/'raw.tar.gz'),expected=expected,host_seconds_diagnostic=time.monotonic()-beg)
    save(rp,result);log(dict(event='committed',name=t['name'],cycles=result['native_cycles'],record_sha256=sha(rp),raw_bytes=(dest/'raw.tar.gz').stat().st_size));return z['callbacks']
   except BaseException as e:
    for n in ['config.yaml','stderr.log','stdout.log']:
     if (d/n).exists():shutil.copy2(d/n,dest/n)
    log(dict(event='failure',name=t['name'],error=repr(e)));raise
 done=[]
 with ThreadPoolExecutor(max_workers=a.workers)as ex:
  jobs=[ex.submit(run,t)for t in tasks]
  for job in as_completed(jobs):
   try:done.append(job.result())
   except BaseException:
    for j in jobs:j.cancel()
    raise
   if len(done)%12==0:print(json.dumps(dict(completed=len(done),registered=len(tasks))),flush=True)
 result=dict(status='ALL_REGISTERED_EXTENSION_CASES_COMPLETE',runs=len(done),callbacks=sum(done),task_manifest_sha256=sha(a.tasks));save(a.output/'SUMMARY.json',result);print(json.dumps(result),flush=True)
if __name__=='__main__':main()
