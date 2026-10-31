                                                                                   
from common import *
import zipfile,shutil,subprocess,copy,tarfile,yaml
capsule=U/'portability_capsule.zip';fresh=R/'portability_check/verified_capsule';assert not fresh.exists()
names=['full__real_m0_c0_q1024','serial_bank_cache__real_m1_c0_q1024'];alltasks=read(U/'ALL_REGISTERED_TASKS.json');chosen=[next(t for t in alltasks if t['name']==n)for n in names]
with zipfile.ZipFile(capsule,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=5)as z:
 for p in (R/'native_base').rglob('*'):
  if p.is_file()and'__pycache__'not in p.parts:z.write(p,str(p.relative_to(R)))
 for sub in ['scripts','variant']:
  for p in(U/sub).iterdir():
   if p.is_file()and'__pycache__'not in p.parts:z.write(p,str(p.relative_to(R)))
 for t in chosen:
  for key in ['input','raw_keys','reference']:
   p=resolve(t[key]);z.write(p,str(p.relative_to(R)))
  for file in ['record.json.gz','ARRAY_CERTIFICATE.json']:
   p=rec(t['name']).parent/file;z.write(p,str(p.relative_to(R)))
fresh.mkdir(parents=True)
with zipfile.ZipFile(capsule)as z:assert z.testzip()is None;z.extractall(fresh)
fu=fresh/'output'/U.name;commands=[['python',str(fu/'scripts/bootstrap_tools.py')],['python',str(fu/'scripts/rebuild.py'),'--output',str(fresh/'rebuilt')]]
log=U/'portable_capsule.log'
with log.open('w')as f:
 for cmd in commands:subprocess.run(cmd,check=True,stdout=f,stderr=subprocess.STDOUT)
 tasks=[]
 for old in chosen:
  t=copy.deepcopy(old);t['name']='portable_archive__'+old['name'];t['binary']=str(fresh/'rebuilt/bin/ramulator2');t['auditor']=str(fresh/'rebuilt/bin/audit_commands');t['expected_record']=str(rec(old['name']));t['expected_equal_cycles']=True;tasks.append(t)
 save(fu/'FRESH_ARCHIVE_CONTROLS.json',tasks)
 cmd=['python',str(fu/'scripts/run_tasks.py'),'--tasks',str(fu/'FRESH_ARCHIVE_CONTROLS.json'),'--workers','1'];commands.append(cmd);subprocess.run(cmd,check=True,stdout=f,stderr=subprocess.STDOUT)
controls=[]
for t in tasks:
 rp=fu/'native'/t['name']/'record.json.gz';rr=read(rp);original=read(t['expected_record']);assert rr['arrays']==original['arrays']and rr['native_cycles']==original['native_cycles'];assert rr['physical_audit']==original['physical_audit']
 with tarfile.open(rp.parent/'raw.tar.gz')as tar:cfg=yaml.load(tar.extractfile('config.yaml'),Loader=yaml.CSafeLoader)
 for key in ['fixture','encoder_raw_keys']:assert Path(cfg['MemorySystem'][key]).is_relative_to(fresh),('Non-relocated execution input',key)
 controls.append(dict(name=t['name'],elapsed_exact=True,six_arrays_exact=True,physical_audit_exact=True,execution_inputs_relocated=True,callbacks=rr['result']['callbacks'],record_sha256=sha(rp),array_sha256=rr['arrays']))
save(U/'results/SOURCE_PACKAGE_PORTABILITY.json',dict(status='FRESH_ARCHIVE_EXTRACT_REBUILD_AND2_RELOCATED_CONTROLS_PASS',capsule_sha256=sha(capsule),source_rebuild=read(fresh/'rebuilt/BUILD_IDENTITY.json'),commands=commands,controls=controls,scope='Two reproduction controls, not additional scientific contexts or quality samples. Existing evidence untouched. Execution paths, not only recorded historical labels, must relocate.'))
print('FRESH_SOURCE_CAPSULE_VERIFIED',flush=True)
