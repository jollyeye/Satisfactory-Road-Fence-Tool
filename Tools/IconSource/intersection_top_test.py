# Python port of RMGeometry IsectSmoothTop (intersection top surface), tested on several outlines; writes intersection_25m.png
# Python port of IsectSmoothTop (same rules) run on several test outlines
import numpy as np, math
def inside(R,p):
    b=False; n=len(R); j=n-1
    for i in range(n):
        A=R[i];B=R[j]
        if (A[1]>p[1])!=(B[1]>p[1]) and p[0] < (B[0]-A[0])*(p[1]-A[1])/(B[1]-A[1])+A[0]: b=not b
        j=i
    return b
def dist(R,p):
    best=1e18
    for i in range(len(R)):
        A=np.array(R[i][:2]);B=np.array(R[(i+1)%len(R)][:2]);Q=np.array(p[:2]);AB=B-A
        t=min(max(np.dot(Q-A,AB)/max(AB@AB,1),0),1); best=min(best,np.linalg.norm(Q-(A+AB*t)))
    return best
def delaunay(P):
    N=len(P); mid=np.mean([p[:2] for p in P],axis=0)
    pts=[np.array(p[:2])-mid for p in P]; ext=max(100,max(abs(q).max() for q in pts)); S=ext*20
    pts+= [np.array([-S,-S]),np.array([S,-S]),np.array([0,S])]
    def make(a,b,c):
        A,B,C=pts[a],pts[b],pts[c]; D=2*(A[0]*(B[1]-C[1])+B[0]*(C[1]-A[1])+C[0]*(A[1]-B[1]))
        if abs(D)<1e-9: return (a,b,c,np.zeros(2),1e300)
        A2,B2,C2=A@A,B@B,C@C
        cen=np.array([(A2*(B[1]-C[1])+B2*(C[1]-A[1])+C2*(A[1]-B[1]))/D,(A2*(C[0]-B[0])+B2*(A[0]-C[0])+C2*(B[0]-A[0]))/D])
        return (a,b,c,cen,((cen-A)**2).sum())
    tris=[make(N,N+1,N+2)]
    for i in range(N):
        edges=[]; keep=[]
        for t in tris:
            if ((t[3]-pts[i])**2).sum()<t[4]: edges+= [(t[0],t[1]),(t[1],t[2]),(t[2],t[0])]
            else: keep.append(t)
        tris=keep
        for e in range(len(edges)):
            sh=any(e!=f and ((edges[e][0]==edges[f][1] and edges[e][1]==edges[f][0]) or edges[e]==edges[f]) for f in range(len(edges)))
            if not sh: tris.append(make(edges[e][0],edges[e][1],i))
    return [(t[0],t[1],t[2]) for t in tris if max(t[:3])<N]
def smooth(R,G):
    N=len(R); P=[list(r) for r in R]; xs=[r[0] for r in R]; ys=[r[1] for r in R]; mz=np.mean([r[2] for r in R])
    x=min(xs)+G/2
    while x<max(xs):
        y=min(ys)+G/2
        while y<max(ys):
            p=(x,y,mz)
            if inside(R,p) and dist(R,p)>G*0.6: P.append(list(p))
            y+=G
        x+=G
    if len(P)==N: return None,"nothing inside"
    allt=delaunay(P); area=sum((R[k][0]*R[(k+1)%N][1]-R[(k+1)%N][0]*R[k][1]) for k in range(N))*0.5
    out=[];cov=0
    for a,b,c in allt:
        A,B,C=np.array(P[a][:2]),np.array(P[b][:2]),np.array(P[c][:2]); cen=(A+B+C)/3
        def si(U,V): M=(U+V)/2; return inside(R,M+(cen-M)*0.02)
        if not inside(R,cen) or not si(A,B) or not si(B,C) or not si(C,A): continue
        u=B-A;v=C-A;cov+=abs(u[0]*v[1]-u[1]*v[0])/2; out.append((a,b,c))
    if abs(cov-abs(area))>abs(area)*0.002+100: return None,f"area {cov:.0f} vs {abs(area):.0f}"
    sides={tuple(sorted((t[e],t[(e+1)%3]))) for t in out for e in range(3)}
    miss=[k for k in range(N) if tuple(sorted((k,(k+1)%N))) not in sides]
    if miss: return None,f"missing sides {len(miss)}"
    return (P,out),f"ok {len(out)} tris"
def outline(mouths,W=800,R=1100,fil=8):
    ring=[];corners=[]
    for ang,h in mouths:
        a=math.radians(ang); d=np.array([math.cos(a),math.sin(a)]); r=np.array([-d[1],d[0]]); c=d*R
        corners.append((c-r*W/2,c+r*W/2,h))
    for i,(A,B,h) in enumerate(corners):
        nA,nB,nh=corners[(i+1)%len(corners)]
        ring+=[(A[0]+1e5,A[1]+2e5,h),(B[0]+1e5,B[1]+2e5,h)]
        ctrl=(B+nA)/2*0.35
        for t in np.linspace(0,1,fil+2)[1:-1]:
            pt=(1-t)**2*B+2*(1-t)*t*ctrl+t**2*nA; ring.append((pt[0]+1e5,pt[1]+2e5,h*(1-t)+nh*t))
    return ring
tests={"3-way":[(90,0),(210,0),(330,300)],"4-way":[(0,0),(90,100),(180,0),(270,-200)],"T":[(0,0),(180,0),(270,250)],
"5-way":[(0,0),(72,0),(144,200),(216,0),(288,0)],"2-way bend":[(0,0),(100,300)],"6-way":[(a,0) for a in range(0,360,60)]}
for name,m in tests.items():
    for G in (250,500):
        r,msg=smooth(outline(m),G); print(name,G,msg)
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
R=outline(tests["3-way"]); (P,T),msg=smooth(R,250)
N=len(R); nb=[set() for _ in P]
for a,b,c in T: nb[a]|={b,c}; nb[b]|={a,c}; nb[c]|={a,b}
for _ in range(300):
    for i in range(N,len(P)): P[i][2]=np.mean([P[j][2] for j in nb[i]])
P=np.array(P); P[:,0]-=1e5; P[:,1]-=2e5; P/=100
fig=plt.figure(figsize=(14,6.5),facecolor="white")
ax=fig.add_subplot(1,2,1,projection="3d")
ax.plot_trisurf(P[:,0],P[:,1],P[:,2],triangles=T,color="#3d85c6",alpha=0.4,edgecolor="#222",linewidth=0.7)
ax.view_init(elev=24,azim=-105); ax.set_box_aspect((1,1,0.3),zoom=1.3); ax.set_zlim(0,4); ax.set_title(f"Mod's algorithm, 2.5 m grid: {len(T)} triangles",fontsize=13)
ax2=fig.add_subplot(1,2,2); ax2.set_aspect("equal"); ax2.triplot(P[:,0],P[:,1],T,color="#222",lw=0.7)
ax2.fill(P[:N,0],P[:N,1],color="#ddd",zorder=0); ax2.axis("off"); ax2.set_title("From above (right end 3 m higher)",fontsize=13)
plt.tight_layout(); plt.savefig("intersection_25m.png",dpi=90)
