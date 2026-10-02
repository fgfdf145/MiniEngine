import numpy as np, importlib.util, io, contextlib
spec=importlib.util.spec_from_file_location('s',__import__('os').path.join(__import__('os').path.dirname(__file__),'macpherson_kinematics.py')); m=importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()): spec.loader.exec_module(m)
def expm(w):
    a=np.linalg.norm(w); return np.eye(3) if a<1e-15 else m.rot(w/a,a)
def newton(P,phi,u):
    C=P['A']+m.rot(P['B']-P['A'],phi)@(P['C']-P['A']); d0=(P['T']-P['S'])/np.linalg.norm(P['T']-P['S'])
    ell=np.linalg.norm(P['Q']-P['R']); Rr=P['R']+np.array([0,u,0]); R=np.eye(3)
    def f(R):
        S=C+R@(P['S']-P['C']); d=R@d0; t=P['T']-S; perp=t-(t@d)*d
        # two components of perp in a basis orthogonal to d, plus tie rod
        b1=np.cross(d,[0,0,1.]); b1/=np.linalg.norm(b1); b2=np.cross(d,b1)
        Q=C+R@(P['Q']-P['C']); return np.array([perp@b1,perp@b2,np.linalg.norm(Q-Rr)-ell])
    for it in range(50):
        r=f(R); 
        if np.abs(r).max()<1e-14: break
        J=np.zeros((3,3)); h=1e-7
        for k in range(3): w=np.zeros(3); w[k]=h; J[:,k]=(f(expm(w)@R)-r)/h
        R=expm(np.linalg.solve(J,-r))@R
    return R,it
for name,sec in (('front',m.F),('rear',m.Rr)):
    P=m.acset(sec); s=m.Strut(P); worst=0
    for ph in np.radians(np.linspace(-20,20,9)):
        for u in ([-0.04,0,0.04] if name=='front' else [0]):
            Rc,_,_,_=s.solve(ph,u); Rn,it=newton(P,ph,u); worst=max(worst,np.abs(Rc-Rn).max())
    print(name,'max |R_closed - R_newton| =',f'{worst:.1e}','(Newton iterations last case:',it,')')
