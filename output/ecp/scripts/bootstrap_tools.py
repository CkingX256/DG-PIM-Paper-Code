                                                                                    
from common import *
import subprocess
commands=[]
for name in ['audit_pipeline','audit_mac_order','independent_oracle']:
 cmd=['g++','-O3','-march=x86-64-v3','-ffp-contract=off','-std=c++20',str(U/'scripts'/(name+'.cpp')),'-o',str(U/'scripts'/name)];subprocess.run(cmd,check=True);(U/'scripts'/name).chmod(0o755);commands.append(cmd)
save(U/'TOOL_BUILD_IDENTITY.json',dict(commands=commands,files={n:sha(U/'scripts'/n)for n in ['audit_pipeline','audit_mac_order','independent_oracle']}))
print('PORTABLE_TOOLS_BUILT',flush=True)
