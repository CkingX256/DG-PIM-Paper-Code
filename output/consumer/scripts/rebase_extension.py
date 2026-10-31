                                                                                   
from pathlib import Path
import argparse,json
ap=argparse.ArgumentParser();ap.add_argument('--manifest',type=Path,required=True);ap.add_argument('--root',type=Path,required=True);ap.add_argument('--binary',type=Path,required=True);ap.add_argument('--pipeline-auditor',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();root=a.root.resolve()
def locate(value):
 p=Path(value)
 for marker in ['/output/','/native_base/','/restored_inputs/']:
  if marker in str(p):return str(root/marker.strip('/')/str(p).split(marker,1)[1])
 return str(p)
tasks=json.load(open(a.manifest))
for t in tasks:
 for k in ['input','raw_keys','reference','expected_record']:
  if k in t:t[k]=locate(t[k]);assert Path(t[k]).is_file(),(k,t[k])
 t['binary']=str(a.binary.resolve());t['auditor']=str(a.binary.resolve().parent/'audit_commands');t['pipeline_auditor']=str(a.pipeline_auditor.resolve())
a.output.write_text(json.dumps(tasks,indent=2));print('RELOCATED_REPLAY_COPY',len(tasks),a.output)
