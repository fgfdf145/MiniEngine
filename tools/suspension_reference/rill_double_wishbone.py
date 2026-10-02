import numpy as np
def sk(v): return np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]])
def rot(e,a): return np.outer(e,e)+(np.eye(3)-np.outer(e,e))*np.cos(a)+sk(e)*np.sin(a)
def trigon(a,b,c):
    if b<0: return np.arcsin(-c/np.hypot(a,b))-np.arctan2(-a,-b)
    return np.arcsin(c/np.hypot(a,b))-np.arctan2(a,b)
dt=dict(w=[0,.768,0],a=[-.251,.32,-.08],b=[.148,.32,-.094],c=[.013,.737,-.145],d=[-.105,.435,.196],e=[.122,.435,.23],f=[-.025,.68,.162],r=[-.15,.38,-.038],q=[-.137,.69,-.088])
P={k:np.array(v,float) for k,v in dt.items()}
def kin(phi,u):
    eab=(P['b']-P['a']);eab/=np.linalg.norm(eab);aphi=rot(eab,phi)
    rcfk=P['f']-P['c'];rdfk=P['f']-P['d'];racv=aphi@(P['c']-P['a']);rcdv=P['d']-(P['a']+racv)
    ede=P['e']-P['d'];ede/=np.linalg.norm(ede);E=np.outer(ede,ede)
    a=rcdv@(np.eye(3)-E)@rdfk;b=rcdv@np.cross(ede,rdfk);c=-rcdv@E@rdfk-.5*(rdfk@rdfk+rcdv@rcdv-rcfk@rcfk)
    psi=trigon(a,b,c);apsi=rot(ede,psi)
    rvcv=P['a']+racv;rvfv=P['d']+apsi@rdfk;rcfv=rvfv-rvcv
    be=trigon(rcfk[0],rcfk[2],rcfv[0]);abe=np.array([[np.cos(be),0,np.sin(be)],[0,1,0],[-np.sin(be),0,np.cos(be)]])
    al=trigon(rcfv[1],rcfv[2],rcfk[1]);aal=np.array([[1,0,0],[0,np.cos(al),-np.sin(al)],[0,np.sin(al),np.cos(al)]])
    rvrv=P['r']+np.array([0,u,0]);rrcv=rvcv-rvrv;rrcht=rrcv@aal@abe
    rrqk=P['q']-P['r'];rcqk=P['q']-P['c'];ecf=rcfk/np.linalg.norm(rcfk);F=np.outer(ecf,ecf)
    a=rrcht@(np.eye(3)-F)@rcqk;b=rrcht@np.cross(ecf,rcqk);c=-(rrcht@F@rcqk+.5*(rrcv@rrcv+rcqk@rcqk-rrqk@rrqk))
    de=trigon(a,b,c);avw=aal@abe@rot(ecf,de);return avw,rvcv+avw@(P['w']-P['c']),de,(np.linalg.norm(rvfv-rvcv)-np.linalg.norm(rcfk))
# design position values
toe0=0;camb0=.8/180*np.pi;rs=.285;en=np.array([0,0,1.])
ey=np.array([toe0,1,-camb0]);ey/=np.linalg.norm(ey);exk=np.cross(ey,en);exk/=np.linalg.norm(exk);eyk=np.cross(en,exk);ezk=np.cross(exk,ey);rwp=-rs*ezk
ecf=(P['f']-P['c']);ecf/=np.linalg.norm(ecf)
print('kingpin',np.degrees(np.arctan2(-ecf[1],ecf[2])),'caster',np.degrees(np.arctan2(-ecf[0],ecf[2])))
rcp=P['w']+rwp-P['c'];rsc=-(en@rcp)/(en@ecf)*ecf
print('caster offset',-exk@(rsc+rcp),'scrub',eyk@(rsc+rcp))
avw,w,de,_=kin(0,0);print('design check',w,np.degrees(de))
for ph in np.radians([-10,-5,0,5,10]):
    avw,w,de,err=kin(ph,0);e=avw@ey;p=w+avw@rwp
    print(f"phi={np.degrees(ph):5.1f} dz={1000*w[2]:7.2f}mm dxW={1000*w[0]:6.2f} dyP={1000*(p[1]-(P['w']+rwp)[1]):6.2f} dxP={1000*(p[0]-(P['w']+rwp)[0]):6.2f} toe(+z)={np.degrees(np.arctan2(-e[0],e[1])):6.3f} camb(+x)={np.degrees(np.arctan2(e[2],e[1])):6.3f} cferr={err:.1e}")
