# Preview of the lane arrow layouts (same numbers as RMBuildableMarking.cpp BuildArrow). python3 arrow_layout.py -> arrows.png
# Physical units: X along road (up in the picture), Y across (right). Same data as the C++.
from PIL import Image, ImageDraw
TOP, BOT = 1.98, -1.98
SH = 0.14            # shaft half width
HH = 0.5             # forward head half width
HL = 1.53            # forward head length
SL = 0.5             # side head length (across)
SB = 1.3             # side head base (along)
BT = 0.4             # side bar thickness (along)
def shapes(v):
    R=[];T=[]
    def rect(x0,x1,y0,y1): R.append((x0,x1,y0,y1))
    def fwd(y): T.append(((TOP-HL,y-HH),(TOP,y),(TOP-HL,y+HH)))
    def side(s, xc, ybase):  # head pointing s (+1 right) with base at ybase
        T.append(((xc-SB/2,ybase),(xc+SB/2,ybase),(xc,ybase+s*SL)))
    if v==0:
        rect(BOT,TOP-HL,-SH,SH); fwd(0)
    elif v in (1,2):
        s=-1 if v==1 else 1
        xc=TOP-SB/2
        ys=-s*(0.5-SH)               # shaft against the far side of the box
        rect(BOT,xc+BT/2,ys-SH,ys+SH)
        yb=s*(0.5-SL)                # head base
        rect(xc-BT/2,xc+BT/2,min(ys+s*SH,yb),max(ys+s*SH,yb))
        side(s,xc,yb)
    elif v in (3,4):
        s=-1 if v==3 else 1
        tip=SH+0.16+SL
        shift=-s*(tip-HH)/2
        rect(BOT,TOP-HL,shift-SH,shift+SH); fwd(shift)
        xc=TOP-HL-0.15-SB/2
        yb=shift+s*(SH+0.16)
        rect(xc-BT/2,xc+BT/2,min(shift+s*SH,yb),max(shift+s*SH,yb))
        side(s,xc,yb)
    else:
        xc=TOP-SB/2
        rect(BOT,xc+BT/2,-SH,SH)
        for s in (-1,1):
            yb=s*(SH+0.16)
            rect(xc-BT/2,xc+BT/2,min(s*SH,yb),max(s*SH,yb))
            side(s,xc,yb)
    return R,T
K=90; W=6*260; H=480
im=Image.new('RGB',(W,H),(30,140,190)); d=ImageDraw.Draw(im)
for v in range(6):
    cx=130+v*260; cy=H//2
    pt=lambda x,y:(cx+y*K, cy-x*K)
    d.rectangle([pt(TOP,-0.5),pt(BOT,0.5)],outline=(255,0,0))
    R,T=shapes(v)
    for x0,x1,y0,y1 in R: d.polygon([pt(x0,y0),pt(x1,y0),pt(x1,y1),pt(x0,y1)],fill=(240,255,220))
    for t in T: d.polygon([pt(*p) for p in t],fill=(240,255,220))
im.save('arrows.png')
