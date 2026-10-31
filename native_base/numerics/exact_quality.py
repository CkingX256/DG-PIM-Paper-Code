                      




   
import math,struct
from fractions import Fraction

UNIT_SQUARED_DENOMINATOR=1<<298

def binary32_units(bits):
    bits=int(bits);exp=(bits>>23)&255;frac=bits&0x7fffff
    if exp==255:raise ValueError('Nonfinite binary32 operand')
    n=frac if exp==0 else ((1<<23)|frac)<<(exp-1)
    return -n if bits>>31 else n

def vector_units(data):
    if len(data)%4:raise ValueError('Partial binary32 vector')
    return [binary32_units(x[0]) for x in struct.iter_unpack('<I',data)]

def exact_metrics(actual_bytes,reference_bytes,epsilon=Fraction(1,10),relative_epsilon=Fraction(1,100)):
    if len(actual_bytes)!=len(reference_bytes):raise ValueError('Shape mismatch')
    a=vector_units(actual_bytes);b=vector_units(reference_bytes)
    err=sum((x-y)**2 for x,y in zip(a,b));norm=sum(x*x for x in b)
    abs_ok=err*epsilon.denominator**2<=epsilon.numerator**2*UNIT_SQUARED_DENOMINATOR
    rel_ok=err*relative_epsilon.denominator**2<=relative_epsilon.numerator**2*norm
    floats_a=[v[0]for v in struct.iter_unpack('<f',actual_bytes)]
    floats_b=[v[0]for v in struct.iter_unpack('<f',reference_bytes)]
    error=math.sqrt(math.fsum((x-y)**2 for x,y in zip(floats_a,floats_b)))
    rn=math.sqrt(math.fsum(x*x for x in floats_b))
    relative=error/rn if rn else (0. if error==0 else 'inf')
    return {'absolute_l2':error,'relative_l2':relative,'reference_l2':rn,
            'absolute_epsilon_pass':abs_ok,'relative_epsilon_pass':rel_ok,
            'squared_error_units_2p298':str(err),'squared_reference_norm_units_2p298':str(norm)}

def controls():
    pack=lambda *xs:struct.pack('<'+'f'*len(xs),*xs)
    zero=pack(0.)
    assert exact_metrics(pack(.1),zero)['absolute_epsilon_pass']is False
    assert exact_metrics(pack(.08,.06),pack(0.,0.))['absolute_epsilon_pass']is True
    assert exact_metrics(pack(1.),zero)['relative_epsilon_pass']is False
    assert exact_metrics(zero,zero)['relative_epsilon_pass']is True
    for bits in (0,0x80000000,1,0x80000001,0x7f7fffff,0xff7fffff,0x00800000,0x3f800000):
        value=struct.unpack('<f',struct.pack('<I',bits))[0]
        assert Fraction(binary32_units(bits),1<<149)==Fraction.from_float(value)
    for bits in (0x7f800000,0xff800000,0x7fc00000):
        try:binary32_units(bits)
        except ValueError:pass
        else:raise AssertionError('Nonfinite not rejected')
    return {'status':'PASS_BINARY32_INTEGER_GATE_CONTROLS','unit':'2^-149',
            'absolute_gate':'sum squared errors <= (1/10)^2 exactly',
            'relative_gate':'sum squared errors <= reference norm squared * (1/100)^2 exactly'}

if __name__=='__main__':
    import json
    print(json.dumps(controls()))
