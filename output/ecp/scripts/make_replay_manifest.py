                                                                             
from common import *
import argparse,copy
ap=argparse.ArgumentParser();ap.add_argument('--names',help='comma-separated registered case names; omit for every case');ap.add_argument('--prefix',required=True);ap.add_argument('--binary-dir',type=Path);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();assert a.prefix and all(c.isalnum()or c=='_'for c in a.prefix)
tasks=read(U/'ALL_REGISTERED_TASKS.json');selected=set(a.names.split(','))if a.names else {t['name']for t in tasks};assert selected<={t['name']for t in tasks}
out=[]
for original in tasks:
 if original['name']not in selected:continue
 assert rec(original['name']).exists();t=copy.deepcopy(original);t['name']=a.prefix+'__'+original['name'];t['expected_record']=str(rec(original['name']));t['expected_equal_cycles']=True
 if a.binary_dir:t['binary']=str(a.binary_dir.resolve()/'ramulator2');t['auditor']=str(a.binary_dir.resolve()/'audit_commands')
 out.append(t)
save(a.output,out);print('EXACT_REPLAY_MANIFEST',len(out),'cases; original evidence untouched',flush=True)
