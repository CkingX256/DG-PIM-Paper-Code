                                                                             
from common import R
import sys
sys.path.insert(0,str(R/'native_base/numerics'))
from exact_quality import exact_metrics

def qualify(actual,captured,dense,Q,H,D):
 assert len(actual)==len(captured)==len(dense)==Q*H*D*4
 answer=[]
 for q in range(Q):
  for h in range(H):
   sl=slice((q*H+h)*D*4,(q*H+h+1)*D*4)
   fp32=exact_metrics(actual[sl],captured[sl]);image=exact_metrics(actual[sl],dense[sl])
   answer.append(dict(query=q,head=h,relative01_pass=fp32['relative_epsilon_pass'],relative_l2=fp32['relative_l2'],absolute01_dense_pass=image['absolute_epsilon_pass'],absolute_dense_l2=image['absolute_l2']))
 return answer
