#!/usr/bin/env python3
"""Waveshare ESP32-S3 3.97 cat enclosure V5: reproducible CAD sources.
All dimensions are mm. Original functional apertures retained as coordinate constraints.
Requires cadquery, OCP, trimesh, shapely and numpy.
"""
from pathlib import Path
import math, tempfile
import cadquery as cq
from cadquery import Vector
import trimesh
from shapely.geometry import LineString
from OCP.BRepAlgoAPI import BRepAlgoAPI_Fuse, BRepAlgoAPI_Cut
from OCP.BRepCheck import BRepCheck_Analyzer
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRepTools import BRepTools
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_SOLID
from OCP.GProp import GProp_GProps
from OCP.BRepGProp import BRepGProp
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.Interface import Interface_Static
from OCP.StlAPI import StlAPI_Writer
from OCP.BRep import BRep_Builder
from OCP.TopoDS import TopoDS_Compound
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib

D = Path(__file__).resolve().parent / "output"
D.mkdir(parents=True, exist_ok=True)
PREFIX = "Waveshare_3.97_cat_v5_original_openings"

def raw(x):
    return x.wrapped if hasattr(x, "wrapped") else x

def count(s):
    e = TopExp_Explorer(s, TopAbs_SOLID); n = 0
    while e.More():
        n += 1
        e.Next()
    return n

def val(s):
    p = GProp_GProps()
    BRepGProp.VolumeProperties_s(s, p)
    return round(p.Mass(), 2)

def bounds(s):
    b = Bnd_Box()
    BRepBndLib.Add_s(s, b)
    return tuple(round(v, 2) for v in b.Get())

def comp(ss):
    b = BRep_Builder()
    c = TopoDS_Compound()
    b.MakeCompound(c)
    for s in ss:
        b.Add(c, raw(s))
    return c

def op(cls, a, b, name, check=True):
    print("OP", name, flush=True)
    r = cls(raw(a), raw(b))
    r.SetFuzzyValue(1.e-5)
    r.Build()
    if not r.IsDone():
        raise RuntimeError(name + " BUILD FAILED")
    s = r.Shape()
    ok = BRepCheck_Analyzer(s).IsValid()
    n = count(s)
    print(name, "valid", ok, "solids", n, "volume", val(s), "bbox", bounds(s), flush=True)
    if not ok or (check and n != 1):
        raise RuntimeError(name + " INVALID")
    return s

def rrect(cx, cy, w, h, r, z0, dz):
    return cq.Workplane("XY").center(cx,cy).rect(w,h).extrude(dz).edges("|Z").fillet(r).translate((0,0,z0)).val()

def cyl(x,y,r,z0,dz):
    return cq.Solid.makeCylinder(r,dz,Vector(x,y,z0))

def slot(x,y,L,W,angle,z0,dz):
    return cq.Workplane("XY").center(x,y).slot2D(L,W,angle=angle).extrude(dz).translate((0,0,z0)).val()

def rbox_xyz(x0,y0,z0,dx,dy,dz):
    return cq.Solid.makeBox(dx,dy,dz,Vector(x0,y0,z0))

def polyline_cut(points,width,z0,dz):
    geo = LineString(points).buffer(width/2,resolution=8,cap_style=1,join_style=1)
    return cq.Workplane("XY").polyline(list(geo.exterior.coords)[:-1]).close().extrude(dz).translate((0,0,z0)).val()

# Integrated cat-ear silhouette; x=2.8 is enclosure center.
HALF = [
    (0,39.7),(17,39.9),(25,40.5),(30,45.5),(35,51.7),(38.6,54.5),
    (41.1,54.7),(44.0,51.2),(47.6,43.0),(51.3,39.2),
    (55.1,35.0),(57.2,29),(57.1,20),(55.2,9),(54.5,0),
    (55.3,-13),(56.8,-24),(54.0,-34),(48.2,-41),(38,-45),
    (22,-47.2),(0,-47.6)
]
OUT = HALF + [(-x,y) for x,y in HALF[-2:0:-1]]

def silhouette(z,sx,sy,earfactor=1.0):
    pts=[]
    for rx,ry in OUT:
        y=39.7+(ry-39.7)*earfactor if ry>39.7 else ry
        pts.append((2.8+rx*sx,y*sy))
    # one Chaikin round to avoid spline overshoot
    for _ in range(1):
        new=[]
        for i,p in enumerate(pts):
            q=pts[(i+1)%len(pts)]
            new.append((.75*p[0]+.25*q[0],.75*p[1]+.25*q[1]))
            new.append((.25*p[0]+.75*q[0],.25*p[1]+.75*q[1]))
        pts=new
    return cq.Wire.makePolygon([Vector(x,y,z) for x,y in pts],close=True)

def smooth_loft(stations,label):
    body=cq.Solid.makeLoft([silhouette(*v) for v in stations],ruled=True)
    assert body.isValid(), label
    return body

SCREWS = [(-41.7,-34.1),(47.3,-34.1),(-41.7,34.1),(47.3,34.1)]
FRONT_STATIONS=[
    (-14.3,.940,.965,.98),(-9.0,1.022,1.013,1.0),
    (5.0,1.026,1.017,1.0),(15.0,.975,.979,1.0)]
front=smooth_loft(FRONT_STATIONS,"organic_front")
board_keepout=smooth_loft([(-8.75,.937,.932,1.0),(15.6,.937,.932,1.0)],"board_keepout")
front=op(BRepAlgoAPI_Cut,front,board_keepout,"front board/electronics cavity")

def aperture_wire(w,h,z):
    x=w/2; y=h/2
    return cq.Wire.makePolygon([Vector(-x,-y,z),Vector(x,-y,z),Vector(x,y,z),Vector(-x,y,z)],close=True)

window=cq.Solid.makeLoft([
    aperture_wire(89.38,54.82,-14.95),
    aperture_wire(89.38,54.82,-14.30),
    aperture_wire(87.38,52.82,-13.30),
    aperture_wire(87.38,52.82,-10.40)],ruled=True)
seat=rrect(0,0,94.1,58.4,2.8,-10.55,2.4)
front=op(BRepAlgoAPI_Cut,front,window,"screen outer aperture")
front=op(BRepAlgoAPI_Cut,front,seat,"screen inner stepped seat")

posts=[]
for idx,(x,y) in enumerate(SCREWS):
    post=cyl(x,y,5.4,4.9,10.1)
    rib=rrect(-48.0 if x<0 else 53.6,y,15.5,4.8,1.15,4.9,10.1)
    posts.append(op(BRepAlgoAPI_Fuse,post,rib,"load transfer bridge "+str(idx)))
front=op(BRepAlgoAPI_Fuse,front,comp(posts),"four hidden screw pillars")
for i,(x,y) in enumerate(SCREWS):
    front=op(BRepAlgoAPI_Cut,front,cyl(x,y,1.08,5.2,9.95),"blind screw pilot "+str(i))

rails=[
    rrect(-47.0,-19.5,2.5,13.0,.85,-8.9,3.2),
    rrect(52.5,19.5,2.5,13.0,.85,-8.9,3.2),
    rrect(-27.0,-31.0,13.5,2.2,.75,-8.9,3.2),
    rrect(28.0,31.0,13.5,2.2,.75,-8.9,3.2)]
front=op(BRepAlgoAPI_Fuse,front,comp(rails),"PCB guide ledges")

# IMPORTANT: these three independent openings are based on original supplied 3MF.
ORIGINAL_OPENINGS=[
    ("right_side",rbox_xyz(47.5,-25.42,-9.50,20.0,32.84,9.50)),
    ("top_edge",rbox_xyz(-34.20,29.75,-9.50,14.0,40.0,9.50)),
    ("bottom_edge",rbox_xyz(-34.20,-69.75,-9.50,16.0,40.0,9.50))
]
for label,opening in ORIGINAL_OPENINGS:
    front=op(BRepAlgoAPI_Cut,front,opening,"restore original "+label)

nose=(cq.Workplane("XY").polyline([(-1.5,-36.0),(7.0,-36.0),(2.75,-40.3)])
      .close().extrude(1.25).edges("|Z").fillet(.95).translate((0,0,-14.65))).val()
facial=[nose,
    polyline_cut([(2.75,-40.55),(2.1,-41.9),(.7,-42.3),(-.6,-41.8),(-1.6,-40.7)],.78,-14.7,1.18),
    polyline_cut([(2.75,-40.55),(3.45,-41.9),(4.85,-42.3),(6.15,-41.8),(7.15,-40.7)],.78,-14.7,1.18)]
facial += [
    slot(-23,-36.65,12.6,1.03,9,-14.67,1.0),
    slot(-23,-41.4,12.4,1.03,-10,-14.67,1.0),
    slot(28.6,-36.65,12.6,1.03,-9,-14.67,1.0),
    slot(28.6,-41.4,12.4,1.03,10,-14.67,1.0),
    cyl(-11.4,-39.4,2.15,-14.65,.9),
    cyl(16.9,-39.4,2.15,-14.65,.9)]
for i,detail in enumerate(facial):
    front=op(BRepAlgoAPI_Cut,front,detail,"front engraving "+str(i))
for k,xc in enumerate((-35.8,41.4)):
    ear=(cq.Workplane("XY").polyline([(xc-5.9,41.8),(xc+5.9,41.8),(xc+1.0,50.3),(xc-1.0,50.3)])
         .close().extrude(1.1).edges("|Z").fillet(1.45).translate((0,0,-14.7))).val()
    front=op(BRepAlgoAPI_Cut,front,ear,"inner cat ear "+str(k))

BACK_STATIONS=[(15.0,.975,.979,1.0),(19.2,.968,.976,1.0),(22.7,.922,.924,.94)]
back=smooth_loft(BACK_STATIONS,"organic_back")
back=op(BRepAlgoAPI_Cut,back,rrect(2.8,0,104.0,68.4,8.4,14.5,4.65),"rear pocket")
outer_lip=rrect(2.8,0,101.0,62.6,5.4,12.0,7.8)
inner_lip=rrect(2.8,0,97.2,58.8,4.1,11.9,8.1)
lip=op(BRepAlgoAPI_Cut,outer_lip,inner_lip,"rear locating lip")
back=op(BRepAlgoAPI_Fuse,back,lip,"attach rear locating lip")
reliefs=[cyl(x,y,6.05,11.8,3.5) for x,y in SCREWS]
back=op(BRepAlgoAPI_Cut,back,comp(reliefs),"screw pillar clearances")
holes=[cyl(x,y,1.55,14.6,8.5) for x,y in SCREWS]
holes += [cyl(x,y,2.65,20.35,3.0) for x,y in SCREWS]
back=op(BRepAlgoAPI_Cut,back,comp(holes),"rear screw counterbores")
rows=[-2.08,1.42,4.92,8.42,11.92,15.42,18.92,22.42]
vents=[slot(-31.7,y,18,2,0,18.4,5.2) for y in rows]
back=op(BRepAlgoAPI_Cut,back,comp(vents),"20x30 speaker grille")
s_out=rrect(-31.7,9.92,24.3,34.2,3.4,16.5,3.7)
s_in=rrect(-31.7,9.92,20.8,30.8,2.6,16.4,3.95)
s_ring=op(BRepAlgoAPI_Cut,s_out,s_in,"speaker frame")
back=op(BRepAlgoAPI_Fuse,back,s_ring,"attach speaker frame")
paw=[cyl(-13.2,6.7,2.45,22.13,.75),
     cyl(-17.7,10.5,1.05,22.13,.75),
     cyl(-14.9,11.95,1.05,22.13,.75),
     cyl(-11.9,11.8,1.05,22.13,.75),
     cyl(-9.0,10.0,1.05,22.13,.75)]
back=op(BRepAlgoAPI_Cut,back,comp(paw),"rear paw detail")

def export(stem,shape):
    assert BRepTools.Write_s(shape,str(D/(stem+".brep")))
    writer=STEPControl_Writer()
    Interface_Static.SetCVal_s("write.step.schema","AP214")
    assert writer.Transfer(shape,STEPControl_AsIs)==1
    assert writer.Write(str(D/(stem+".step")))==1
    with tempfile.NamedTemporaryFile(suffix=".stl") as tmp:
        BRepMesh_IncrementalMesh(shape,.085,False,.16,False)
        assert StlAPI_Writer().Write(shape,tmp.name)
        mesh=trimesh.load_mesh(tmp.name,process=True)
        assert mesh.is_watertight and mesh.is_winding_consistent and mesh.volume>2000
        if stem.endswith("_back"):
            mesh.apply_transform(trimesh.transformations.rotation_matrix(math.pi,[1,0,0]))
        mesh.apply_translation([0,0,-mesh.bounds[0,2]])
        mesh.export(D/(stem+".stl"),file_type="stl")
        mesh.export(D/(stem+".3mf"),file_type="3mf")
    print("EXPORTED",stem,flush=True)

export(PREFIX+"_front",front)
export(PREFIX+"_back",back)
print("CAD models complete in",D)
