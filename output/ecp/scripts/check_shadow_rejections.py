                                                                             
from common import *
import tarfile,tempfile,subprocess,os,resource,yaml
rp=rec('priority_shadow_cache__real_m0_c0_q1024');r=read(rp)
with tarfile.open(rp.parent/'raw.tar.gz')as tar:cfg=yaml.load(tar.extractfile('config.yaml'),Loader=yaml.CSafeLoader)
cases={'missing_pipeline':dict(qk_reuse_pipeline=False),'missing_priority':dict(qk_priority_first=False),'functional_cache_also_enabled':dict(qk_exact_exp_cache=True),'non_inplace':dict(inplace_exp=False)};rows=[]
for name,mutation in cases.items():
 with tempfile.TemporaryDirectory(prefix='ecp26_reject_',dir='/dev/shm')as td:
  d=Path(td);c=yaml.load(yaml.safe_dump(cfg),Loader=yaml.CSafeLoader);m=c['MemorySystem'];m.update(mutation);m['fixture']=str(resolve(r['task']['input']));m['encoder_raw_keys']=str(resolve(r['task']['raw_keys']));m['output_directory']=str(d);m['Controller']['plugins'][0]['ControllerPlugin']['path']=str(d/'commands.log');(d/'config.yaml').write_text(yaml.safe_dump(c))
  p=subprocess.run(['/lib64/ld-linux-x86-64.so.2',str(U/'build/bin/ramulator2'),'-f',str(d/'config.yaml')],env=dict(os.environ,LD_LIBRARY_PATH=str(U/'build/bin')),capture_output=True,text=True,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)),timeout=60)
  assert p.returncode!=0 and 'Invalid cache-only reservation ablation'in p.stderr,(name,p.returncode,p.stderr)
  dest=U/'rejection_controls'/name;dest.mkdir(parents=True,exist_ok=True);(dest/'config.yaml').write_text((d/'config.yaml').read_text());(dest/'stderr.log').write_text(p.stderr)
  rows.append(dict(name=name,mutation=mutation,rejected=True,returncode=p.returncode,reason='Invalid cache-only reservation ablation',config_sha256=sha(dest/'config.yaml'),stderr_sha256=sha(dest/'stderr.log')))
save(U/'results/SHADOW_REJECTION_CONTROLS.json',dict(status='ALL4_INVALID_SHADOW_CONFIGURATIONS_REJECTED',controls=rows,scope='Initialization/configuration rejection controls, not performance samples or additional independent fixtures.'))
print('ALL4_SHADOW_REJECTIONS_PASS',flush=True)
