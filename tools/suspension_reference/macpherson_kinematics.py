import numpy as np
def sk(v): return np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]])
def rot(e,a): e=e/np.linalg.norm(e); return np.outer(e,e)+(np.eye(3)-np.outer(e,e))*np.cos(a)+sk(e)*np.sin(a)
def align(a,b):  # minimal rotation taking unit a to unit b
    a=a/np.linalg.norm(a); b=b/np.linalg.norm(b); v=np.cross(a,b); c=a@b
    return np.eye(3)+sk(v)+sk(v)@sk(v)/(1+c)
def trig_all(a,b,c):  # all solutions of a cos x + b sin x = c
    r=np.hypot(a,b); d=r*r-c*c
    if d<0: return []
    th=np.arctan2(b,a); off=np.arccos(np.clip(c/r,-1,1)); return [th+off, th-off]
AC=lambda p: np.array([p[2],-p[0],p[1]])   # AC (x in, y up, z fwd) -> Rill (x fwd, y out/left, z up), wheel centre origin
class Strut:
    def __init__(s,P): s.P=P; s.ab=P['B']-P['A']; s.ab/=np.linalg.norm(s.ab)
    def solve(s,phi,u=0.0,prev=0.0):
        P=s.P; C=P['A']+rot(s.ab,phi)@(P['C']-P['A'])
        sv=P['S']-P['C']; d=P['T']-P['S']; d/=np.linalg.norm(d); L=np.linalg.norm(P['T']-C)
        bq=sv@d; disc=bq*bq-(sv@sv-L*L); lam=-bq+np.sqrt(disc)    # strut length S->T
        vk=sv+lam*d; vw=P['T']-C; R0=align(vk,vw); e=vw/np.linalg.norm(vw)
        Rr=P['R']+np.array([0,u,0]); q0=R0@(P['Q']-P['C']); rc=C-Rr; ell=np.linalg.norm(P['Q']-P['R'])
        E=np.outer(e,e); a=rc@(np.eye(3)-E)@q0; b=rc@np.cross(e,q0); c=0.5*(ell*ell-rc@rc-q0@q0)-rc@E@q0
        sols=trig_all(a,b,c); de=min(sols,key=lambda x:abs(np.angle(np.exp(1j*(x-prev)))))
        R=rot(e,de)@R0; return R,C,lam,de
def report(name,P,r_tyre,phis,us=(0.0,)):
    s=Strut(P); W0=P['W']; ey=np.array([0,1.,0]); P0=W0+np.array([0,0,-r_tyre])
    eS=(P['T']-P['C']);eS/=np.linalg.norm(eS)
    en=np.array([0,0,1.]);rcp=P0-P['C'];rsc=-(en@rcp)/(en@eS)*eS
    print(f"\n{name}: kingpin {np.degrees(np.arctan2(-eS[1],eS[2])):.2f} deg, caster {np.degrees(np.arctan2(-eS[0],eS[2])):.2f} deg, caster trail {1000*-(rsc+rcp)[0]:.1f} mm, scrub {1000*(rsc+rcp)[1]:.1f} mm, strut len {np.linalg.norm(P['T']-P['S'])*1000:.1f} mm")
    for u in us:
      prev=0
      for ph in phis:
        R,C,lam,de=s.solve(np.radians(ph),u,prev); prev=de
        W=C+R@(W0-P['C']); e=R@ey; Pc=W+R@(P0-W0)
        Q=C+R@(P['Q']-P['C']); res=(abs(np.linalg.norm(Q-(P['R']+[0,u,0]))-np.linalg.norm(P['Q']-P['R'])), np.linalg.norm(np.cross(R@(P['T']-P['S'])/np.linalg.norm(P['T']-P['S']), (P['T']-(C+R@(P['S']-P['C'])))/lam)))
        print(f" u={1000*u:5.1f} phi={ph:6.1f} dz={1000*W[2]:7.2f} dx={1000*W[0]:6.2f} dyW={1000*W[1]:6.2f} dyCP={1000*(Pc[1]-P0[1]):6.2f} camber(top-out+)={-np.degrees(np.arcsin(e[2])):6.3f} toe-in+={np.degrees(np.arctan2(e[0],e[1])):7.3f} strut={1000*lam:6.1f} res={max(res):.1e}")
    return s
def acset(sec):
    g=lambda k: AC(np.array(sec[k]))
    return dict(T=g('STRUT_CAR'),S=g('STRUT_TYRE'),A=g('WBCAR_BOTTOM_REAR'),B=g('WBCAR_BOTTOM_FRONT'),C=g('WBTYRE_BOTTOM'),R=g('WBCAR_STEER'),Q=g('WBTYRE_STEER'),W=np.zeros(3))
F=dict(STRUT_CAR=[0.28497,0.40218,-0.08294],STRUT_TYRE=[0.10784,-0.16402,0.01798],WBCAR_BOTTOM_FRONT=[0.43800,-0.16775,0.26073],WBCAR_BOTTOM_REAR=[0.41057,-0.15672,-0.01280],WBTYRE_BOTTOM=[0.10784,-0.16402,0.01798],WBCAR_STEER=[0.48843,-0.09289,0.10865],WBTYRE_STEER=[0.09707,-0.08479,0.14781])
Rr=dict(STRUT_CAR=[0.3355,0.4601,-0.0506],STRUT_TYRE=[0.1106,-0.1804,0.0095],WBCAR_BOTTOM_FRONT=[0.2688,-0.0641,0.6175],WBCAR_BOTTOM_REAR=[0.4054,-0.1744,-0.0565],WBTYRE_BOTTOM=[0.1106,-0.1804,0.0095],WBCAR_STEER=[0.5410,-0.1257,0.1950],WBTYRE_STEER=[0.2082,-0.1351,0.2028])
r=0.335
sf=report('Boxster FRONT (AC data)',acset(F),r,[-25,-15,-8,0,8,15,25])
report('Boxster FRONT steering',acset(F),r,[0],us=[-0.04,-0.02,0,0.02,0.04])
sr=report('Boxster REAR (AC data)',acset(Rr),r,[-20,-10,0,10,20])
# general case: strut axis offset from lower ball joint (S != C)
G=acset(F); G['S']=G['C']+np.array([0.0,-0.03,0.06])
report('Front with strut offset 30 mm out / 60 mm up of C',G,r,[-15,0,15])
