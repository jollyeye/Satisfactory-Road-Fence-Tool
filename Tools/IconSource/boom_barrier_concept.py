import math
W,H=1800,1120
C30,S30=math.cos(math.radians(30)),math.sin(math.radians(30))
def P(x,y,z,ox,oy,s):
    return (ox+(x-y)*C30*s, oy-z*s+(x+y)*S30*s)
def poly(pts,fill,stroke="#15171a",sw=1.6,extra=""):
    return '<polygon points="%s" fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round" %s/>'%(" ".join("%.1f,%.1f"%p for p in pts),fill,stroke,sw,extra)
def box(x0,x1,y0,y1,z0,z1,top,left,right,ox,oy,s,sw=1.6):
    # visible faces for this projection: top (z1), front-left (y=y1), front-right (x=x1)
    q=lambda x,y,z:P(x,y,z,ox,oy,s)
    out=[]
    out.append(poly([q(x0,y1,z0),q(x1,y1,z0),q(x1,y1,z1),q(x0,y1,z1)],left,sw=sw))
    out.append(poly([q(x1,y0,z0),q(x1,y1,z0),q(x1,y1,z1),q(x1,y0,z1)],right,sw=sw))
    out.append(poly([q(x0,y0,z1),q(x1,y0,z1),q(x1,y1,z1),q(x0,y1,z1)],top,sw=sw))
    return out
OR=("#ffb35c","#f08a24","#c46a14")   # orange top/left/right
GR=("#8b9198","#5d636b","#43484f")   # steel
DK=("#4a4f57","#33373d","#25282d")   # dark
def scene(ox,oy,s,open_):
    q=lambda x,y,z:P(x,y,z,ox,oy,s)
    e=[]
    # road slab
    e.append(poly([q(-110,-90,0),q(460,-90,0),q(460,120,0),q(-110,120,0)],"#3a3d42",sw=0))
    e.append(poly([q(30,-90,0.1),q(420,-90,0.1),q(420,120,0.1),q(30,120,0.1)],"#2c2f33",sw=0))
    # lane edge lines and stop line
    for x in (40,410):
        e.append(poly([q(x-3,-90,0.2),q(x+3,-90,0.2),q(x+3,120,0.2),q(x-3,120,0.2)],"#e8e3d4",sw=0))
    e.append(poly([q(222,-90,0.2),q(228,-90,0.2),q(228,120,0.2),q(222,120,0.2)],"#e9b23a",sw=0,extra='stroke-dasharray="0"'))
    for y in range(-150,250,60):
        pass
    e.append(poly([q(40,40,0.3),q(410,40,0.3),q(410,52,0.3),q(40,52,0.3)],"#f2efe6",sw=0))
    # concrete pad + plinth
    e+=box(-70,30,-60,60,0,6,"#9fa3a8","#7c8086","#62666b",ox,oy,s)
    e+=box(-50,10,-40,40,6,16,*DK,ox,oy,s)
    # landing post (far side)
    e+=box(405,423,-9,9,0,78,*GR,ox,oy,s)
    e+=box(400,428,-14,14,78,86,*OR,ox,oy,s)
    # counterweight / rear block
    e+=box(-78,-46,-14,14,70,104,*DK,ox,oy,s)
    # housing: orange cabinet with bevelled top
    e+=box(-46,6,-32,32,16,96,*OR,ox,oy,s)
    # bevel cap
    e.append(poly([q(-46,32,96),q(6,32,96),q(0,26,108),q(-40,26,108)],"#ffc27a"))
    e.append(poly([q(6,-32,96),q(6,32,96),q(0,26,108),q(0,-26,108)],"#d9781c"))
    e.append(poly([q(-40,-26,108),q(0,-26,108),q(0,26,108),q(-40,26,108)],"#ffd29a"))
    # front panel (y=32 face): dark inset with vents
    e.append(poly([q(-40,32.2,26),q(0,32.2,26),q(0,32.2,70),q(-40,32.2,70)],"#2b2e33",sw=1.2))
    for zz in range(30,66,6):
        e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#5b6068" stroke-width="2.2"/>'%(*q(-36,32.4,zz),*q(-4,32.4,zz)))
    # status light bar on the right face (x=6)
    col = "#3dff8a" if open_ else "#ff3b30"
    glow = "url(#glowG)" if open_ else "url(#glowR)"
    e.append(poly([q(6.3,-20,40),q(6.3,-12,40),q(6.3,-12,88),q(6.3,-20,88)],col,stroke=col,sw=0.5,extra='filter="url(#blur)"'))
    e.append(poly([q(6.3,-19,42),q(6.3,-13,42),q(6.3,-13,86),q(6.3,-19,86)],"#ffffff",sw=0,extra='opacity="0.75"'))
    # warning chevron sticker on right face
    e.append(poly([q(6.3,6,30),q(6.3,26,30),q(6.3,26,50),q(6.3,6,50)],"#1d1f23",sw=1))
    for k in range(3):
        a=6+k*7
        e.append(poly([q(6.5,a,30),q(6.5,a+3.5,30),q(6.5,a+7,50),q(6.5,a+3.5,50)],"#ffcf3a",sw=0))
    # hinge hub
    e+=box(6,20,-16,16,74,100,*GR,ox,oy,s)
    hx,hy=q(20,0,87)
    e.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#2a2d31" stroke="#15171a" stroke-width="2"/>'%(hx,hy,9*s))
    e.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#9aa0a7"/>'%(hx,hy,4*s))
    # arm
    if not open_:
        L=392
        # core beam
        e+=box(20,20+L,-6,6,82,94,"#f4f1ea","#d8d3c8","#b9b3a6",ox,oy,s)
        # hazard stripes on the visible front face (y=6) and top
        n=12
        for k in range(n):
            if k%2: continue
            a=20+L*k/n; b=20+L*(k+1)/n
            e.append(poly([q(a,6.05,82),q(b,6.05,82),q(b,6.05,94),q(a,6.05,94)],"#e8441f",sw=0.8))
            e.append(poly([q(a,-6,94.05),q(b,-6,94.05),q(b,6,94.05),q(a,6,94.05)],"#ff6a3d",sw=0.8))
        # LED strip under the top edge
        e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%.1f" filter="url(#blur)"/>'%(*q(26,6.2,86),*q(20+L-6,6.2,86),col,2.6*s))
        e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#ffffff" stroke-width="%.1f" opacity="0.8"/>'%(*q(26,6.2,86),*q(20+L-6,6.2,86),0.8*s))
        # tip cap
        e+=box(20+L,20+L+8,-8,8,80,96,*DK,ox,oy,s)
    else:
        # raised: beam along +z (rotated about the hinge), slightly back-tilted
        L=330
        e+=box(14,26,-6,6,94,94+L,"#f4f1ea","#d8d3c8","#b9b3a6",ox,oy,s)
        n=10
        for k in range(n):
            if k%2: continue
            a=94+L*k/n; b=94+L*(k+1)/n
            e.append(poly([q(14,6.05,a),q(26,6.05,a),q(26,6.05,b),q(14,6.05,b)],"#e8441f",sw=0.8))
            e.append(poly([q(26.05,-6,a),q(26.05,6,a),q(26.05,6,b),q(26.05,-6,b)],"#c9351a",sw=0.8))
        e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%.1f" filter="url(#blur)"/>'%(*q(26.2,0,100),*q(26.2,0,94+L-6),col,2.6*s))
        e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#ffffff" stroke-width="%.1f" opacity="0.8"/>'%(*q(26.2,0,100),*q(26.2,0,94+L-6),0.8*s))
        e+=box(12,28,-8,8,94+L,94+L+8,*DK,ox,oy,s)
    # piston from housing to arm
    a=q(6,20,40); b=q(40,7,86) if not open_ else q(20,7,140)
    e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#2a2d31" stroke-width="%.1f" stroke-linecap="round"/>'%(*a,*b,5*s))
    e.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#c9ced4" stroke-width="%.1f" stroke-linecap="round"/>'%(*a,*b,2*s))
    # beacon on top
    bx,by=q(-20,0,108)
    e.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="3" fill="#2a2d31"/>'%(bx-6*s,by-10*s,12*s,10*s))
    e.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" filter="url(#blur)"/>'%(bx,by-14*s,7*s,col))
    e.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#ffffff" opacity="0.85"/>'%(bx,by-14*s,3*s))
    return e
svg=['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'%(W,H,W,H),
'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1b2433"/><stop offset="1" stop-color="#0d1118"/></linearGradient>',
'<filter id="blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="3"/><feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>',
'<rect width="100%" height="100%" fill="url(#bg)"/>']
svg+=scene(470,520,1.5,False)
svg+=scene(1290,610,1.05,True)
svg.append('<text x="60" y="80" font-family="Segoe UI, Arial" font-size="40" font-weight="700" fill="#f3f0e8">Boom Barrier</text>')
svg.append('<text x="60" y="118" font-family="Segoe UI, Arial" font-size="22" fill="#9aa4b2">Road Maker 1.3.0 concept — closed (left), open (right)</text>')
notes=["orange cabinet with bevelled cap, vents, red / green light bar","hinged hub + hydraulic piston, rear counterweight","striped arm with LED strip (red closed, green open)","landing post with orange cap on the far side","beacon on top follows the state"]
for i,t in enumerate(notes):
    svg.append('<text x="60" y="%d" font-family="Segoe UI, Arial" font-size="22" fill="#c9d1db">• %s</text>'%(960+i*28,t))
svg.append('</svg>')
open('boom.svg','w').write("\n".join(svg))
