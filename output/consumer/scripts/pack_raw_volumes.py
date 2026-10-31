                                                                                   
from consumer_stats import *
import zipfile,tempfile,shutil,os
D=R/'deliverables';paths=sorted(C.glob('*_native/*/record.json.gz'))+sorted(C.glob('adversarial_reference/*/record.json.gz'))
cases=[];unique={}
for p in paths:
 r=read(p);raw=p.parent/'raw.tar.gz';assert raw.exists()and sha(raw)==r['archive_sha256']
 h=r['archive_sha256'];row=dict(record=str(p.relative_to(R)),archive_sha256=h,archive_bytes=raw.stat().st_size)
 cases.append(row)
 if h not in unique:unique[h]=dict(**row,raw=raw,member=str(raw.relative_to(R)))
volumes=[];batch=[];size=0
def commit(batch):
 number=len(volumes)+1;name=f'DG_PIM_Consumer_Raw_{number:02}.zip'
 with tempfile.TemporaryDirectory(prefix='consumer_raw_volume_',dir='/dev/shm')as td:
  stage=Path(td)/name
  with zipfile.ZipFile(stage,'w',compression=zipfile.ZIP_STORED,allowZip64=True)as z:
   for x in batch:z.write(x['raw'],x['member'])
  with zipfile.ZipFile(stage)as z:
   assert z.testzip()is None
   for x in batch:
    with z.open(x['member'])as f:assert hashlib.file_digest(f,'sha256').hexdigest()==x['archive_sha256']
  h=sha(stage);atomic=D/'.rsync-tmp'/('consumer_volume_'+str(number));atomic.parent.mkdir(exist_ok=True);assert not atomic.exists();shutil.copyfile(stage,atomic);assert sha(atomic)==h;os.replace(atomic,D/name)
  volume=dict(file=name,bytes=(D/name).stat().st_size,sha256=h,unique_archives=len(batch),status='LOCAL_VERIFIED_DOWNLOAD',persistence='Not asserted by this scientific catalog;see final save receipt')
  volumes.append(volume)
  for x in batch:
   for row in cases:
    if row['archive_sha256']!=x['archive_sha256']:continue
    case=(R/row['record']).parent;raw=case/'raw.tar.gz';assert sha(raw)==x['archive_sha256'];loc=dict(kind='VERIFIED_CONSUMER_RAW_VOLUME',volume='deliverables/'+name,volume_sha256=h,member=x['member'],archive_sha256=x['archive_sha256'],archive_bytes=x['archive_bytes'],scope='Closed volume verified by CRC and full per-member SHA before deleting an exact duplicate case archive. Extract volume at project root to restore raw.tar.gz.')
    (case/'RAW_LOCATION.json').write_text(json.dumps(loc,indent=2));row.update(volume='deliverables/'+name,volume_sha256=h,member=x['member']);raw.unlink()
  (C/'CONSUMER_RAW_CATALOG.json').write_text(json.dumps(dict(status='PACKING_VERIFIED_VOLUMES_IN_PROGRESS',volumes=volumes,cases=cases),indent=2))
 print('VERIFIED_CONSUMER_RAW_VOLUME',number,len(batch),volume['bytes'],flush=True)
for x in unique.values():
 if batch and size+x['archive_bytes']>240*1024*1024:commit(batch);batch=[];size=0
 batch.append(x);size+=x['archive_bytes']
if batch:commit(batch)
(C/'CONSUMER_RAW_CATALOG.json').write_text(json.dumps(dict(status='EVERY_COMPLETED_CONSUMER_ARCHIVE_INDEXED_IN_READBACK_VERIFIED_VOLUMES',placements=len(cases),unique_archives=len(unique),volumes=volumes,cases=cases,scope='Includes exploratory completed protocol placements as well as registered matrices;archive count is not scientific-case or source-sample count. Four exact reused raw placements are stored once. Canonical original numerical arrays remain in earlier frozen-reference archives.'),indent=2))
print('ALL_CONSUMER_RAW_VOLUMES_COMPLETE',len(volumes),len(cases),len(unique),flush=True)
