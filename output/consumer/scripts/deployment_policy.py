



   
def consumer_parameters(queries,channels=1,validated_profile=False,variant='full_cache',owner_dim=None):
 if variant not in ('full_cache','light_pipeline','profile_policy'):
  raise ValueError('Unknown evaluated consumer variant')
 enabled=validated_profile and queries in (1024,2048) and channels==1
 cache=enabled and (variant=='full_cache'or variant=='profile_policy'and owner_dim==128)
 return dict(qk_reuse_pipeline=enabled,qk_priority_first=enabled,
             qk_readout_first=enabled,qk_exact_exp_cache=cache,
             qk_exp_policy='predictive',inplace_exp=not cache)
