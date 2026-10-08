"""Generate text-only phase 5b+ VFX proofs, meshes and script-free scenes."""
from pathlib import Path
import json, math, struct
ROOT=Path(__file__).resolve().parents[1]
def write(path,value):
    target=ROOT/path;target.parent.mkdir(parents=True,exist_ok=True)
    target.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
def mesh(name,positions,normals,uvs,indices):
    binary=bytearray();views=[];accessors=[]
    for values,kind,count,component in [(positions,'VEC3',len(positions),5126),(normals,'VEC3',len(normals),5126),(uvs,'VEC2',len(uvs),5126),(indices,'SCALAR',len(indices),5125)]:
        flat=values if kind=='SCALAR' else [v for row in values for v in row]
        data=struct.pack('<'+('I' if component==5125 else 'f')*len(flat),*flat)
        views.append({'buffer':0,'byteOffset':len(binary),'byteLength':len(data)})
        binary.extend(data)
        a={'bufferView':len(views)-1,'componentType':component,'count':count,'type':kind}
        if len(accessors)==0:
            a['min']=[min(p[i] for p in positions) for i in range(3)];a['max']=[max(p[i] for p in positions) for i in range(3)]
        accessors.append(a)
    doc={'asset':{'version':'2.0','generator':'OwnEngine VFX acceptance generator'},'buffers':[{'byteLength':len(binary)}],
         'bufferViews':views,'accessors':accessors,'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},'indices':3}]}],
         'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
    source=json.dumps(doc,separators=(',',':')).encode();source+=b' '*((-len(source))%4);binary+=b'\0'*((-len(binary))%4)
    glb=struct.pack('<III',0x46546c67,2,28+len(source)+len(binary))+struct.pack('<II',len(source),0x4e4f534a)+source+struct.pack('<II',len(binary),0x004e4942)+binary
    target=ROOT/'assets/models'/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(glb)
# Dome: unit hemisphere, base Y=0, 24 sectors and 12 latitude bands.
p=[];n=[];uv=[];idx=[]
for row in range(13):
    theta=row/12*math.pi/2
    for col in range(25):
        angle=col/24*math.tau;v=[math.sin(theta)*math.cos(angle),math.cos(theta),math.sin(theta)*math.sin(angle)]
        p.append(v);n.append(v);uv.append([col/24,row/12])
for row in range(12):
    for col in range(24):
        a=row*25+col;b=a+25;idx.extend([a,b,a+1,a+1,b,b+1])
mesh('vfx_dome.glb',p,n,uv,idx)
# Horizontal ring: unit outer radius, 0.78 inner radius, UV.y measures width.
p=[];n=[];uv=[];idx=[]
for col in range(65):
    angle=col/64*math.tau
    for j,radius in enumerate([0.78,1]):
        p.append([radius*math.cos(angle),0,radius*math.sin(angle)]);n.append([0,1,0]);uv.append([col/64,j])
for col in range(64):
    a=col*2;idx.extend([a,a+2,a+1,a+1,a+2,a+3])
mesh('vfx_ring.glb',p,n,uv,idx)
# Faceted ice crystal: six sides with a pointed top and a flat bottom at Y=-0.5.
p=[];n=[];uv=[];idx=[]
for side in range(6):
    a=side*math.tau/6;b=(side+1)*math.tau/6;mid=(a+b)/2
    vertices=[[.5*math.cos(a),-.5,.5*math.sin(a)],[.5*math.cos(b),-.5,.5*math.sin(b)],
              [.5*math.cos(a),.22,.5*math.sin(a)],[.5*math.cos(b),.22,.5*math.sin(b)],[0,.5,0]]
    base=len(p);p.extend(vertices);n.extend([[math.cos(mid),.25,math.sin(mid)]]*5);uv.extend([[0,0],[1,0],[0,.72],[1,.72],[.5,1]])
    idx.extend([base,base+1,base+2,base+2,base+1,base+3,base+2,base+3,base+4])
mesh('vfx_crystal.glb',p,n,uv,idx)
# Graph construction uses the public surface operations only.
class Graph:
    def __init__(self):self.nodes=[]
    def add(self,op,args=None,value=None,name=None):
        item={'op':op}
        if args is not None:item['args']=args
        if value is not None:item['value']=value
        if name is not None:item['name']=name
        self.nodes.append(item);return len(self.nodes)-1
    def save(self,name,color,emissive=None,uniforms=None):
        doc={'format':'ownengine.shader','nodes':self.nodes,'color':color}
        if emissive is not None:doc['emissive']=emissive
        if uniforms is not None:doc['uniforms']=uniforms
        write('materials/'+name+'.shader.json',doc)
        write('materials/'+name+'.mat.json',{'format':'ownengine.material','shader':'materials/'+name+'.shader.json','unlit':True,'doubleSided':True,'alphaMode':'blend'})
g=Graph();base=g.add('baseColor');tex=g.add('texture',[g.add('uv')]);color=g.add('multiply',[base,tex]);glow=g.add('particleCustom0');energy=g.add('multiply',[color,glow]);energy=g.add('multiply',[energy,g.add('constant',value=[2,2,2,0])]);g.save('vfx_healing',color,energy)
g=Graph();base=g.add('baseColor');uv=g.add('uv');v=g.add('swizzle',[uv],value=[1,1,1,1]);fade=g.add('particleCustom3');mask=g.add('step',[v,fade]);color=g.add('multiply',[base,mask]);glow=g.add('multiply',[color,g.add('particleCustom0')]);g.save('vfx_shockwave',color,glow)
g=Graph();camera=g.add('uniform',name='cameraPosition');pos=g.add('position');view=g.add('normalize',[g.add('subtract',[camera,pos])]);normal=g.add('normalize',[g.add('normal')]);facing=g.add('abs',[g.add('dot',[view,normal])]);one=g.add('constant',value=1);zero=g.add('constant',value=0);rim=g.add('subtract',[one,g.add('clamp',[facing,zero,one])]);rim=g.add('multiply',[rim,rim]);power=g.add('particleCustom0');brightness=g.add('add',[g.add('constant',value=.2),g.add('multiply',[rim,power])]);rgb=g.add('multiply',[g.add('baseColor'),brightness]);rgb=g.add('multiply',[rgb,g.add('constant',value=[1,1,1,0])]);alpha=g.add('add',[g.add('constant',value=.06),g.add('multiply',[rim,g.add('constant',value=.7)])]);alpha=g.add('multiply',[alpha,g.add('particleCustom3')]);alpha=g.add('multiply',[alpha,g.add('constant',value=[0,0,0,1])]);color=g.add('add',[rgb,alpha]);g.save('vfx_shield',color,rgb,{'cameraPosition':[0,0,0,1]})
GLOW='assets/textures/glow.png'
def emitter(name,**values):
    settings={'burst':1,'rate':0,'maxParticles':1,'loop':False,'lifetime':3,'speed':0,'spread':0,'gravity':[0,0,0],
              'startSize':1,'endSize':1,'startOpacity':1,'endOpacity':1,'pixelArt':False}
    settings.update(values);return {'name':name,'emitter':settings}
def asset(name,blocks,params=None):
    doc={'format':'oe.vfx','version':1,'name':name.replace('_',' ').title(),'description':'Phase 5b+ text-only acceptance proof; no gameplay transform script.',
         'budget':{'maxParticles':sum(b['emitter']['maxParticles'] for b in blocks)},'emitters':blocks}
    if params:doc['params']=params
    write('effects/'+name+'.vfx.json',doc)
    scene={'format':'ownengine.scene','version':1,'name':doc['name'],'entities':[
        {'id':1,'name':'Camera','components':{'Transform':{'position':[0,3,9],'rotation':[-10,0,0]},'Camera':{'clearColor':[.02,.025,.045]},'PostProcess':{'bloom':.6,'bloomThreshold':.75}}},
        {'id':2,'name':'Effect','components':{'Transform':{},'VFXSystem':{'asset':'effects/'+name+'.vfx.json'}}},
        {'id':3,'name':'Ground','components':{'Transform':{'position':[0,-.05,0],'scale':[10,.1,10]},'MeshRenderer':{'mesh':'cube','color':[.045,.07,.10],'unlit':True}}}]}
    write('scenes/'+name+'.scene.json',scene)
def parameter(value,lo=None,hi=None):
    p={'default':value,'targets':[]}
    if lo is not None:p['min']=lo;p['max']=hi
    return p
asset('ice_pillar',[
    emitter('pillar',lifetime=3,mesh='assets/models/vfx_crystal.glb',meshScale=[.8,3.6,.8],pivot=[0,-.5,0],material='materials/ice_channels.mat.json',
        expressions={'update':{'scale':'vec3(1,curve(age,0,0.01,0.2,1,1,1),1)','custom0':'curve(age,0,0.1,0.2,0.8,0.8,0.4,1,0)','custom3':'curve(age,0,1,0.75,1,1,0)'}}),
    emitter('frost',burst=32,maxParticles=32,lifetime=1.4,delay=.12,texture=GLOW,blend='add',startColor=[.25,.7,1],startSize=.2,
        expressions={'birth':{'position':'vec3(cos(index*2*pi/max(count,1))*0.5,0.08,sin(index*2*pi/max(count,1))*0.5)'},
                     'update':{'position':'vec3(birthPosition.x*age*2,age*0.6,birthPosition.z*age*2)','size':'birthSize*(1-age)','opacity':'1-age'}}),
    emitter('fragments',burst=12,maxParticles=12,lifetime=1.1,delay=2.25,mesh='assets/models/vfx_crystal.glb',startSize=.16,endSize=.06,material='materials/ice_channels.mat.json',custom3=1,custom0=.3,gravity=[0,-3,0],batching=True,
        expressions={'birth':{'position':'vec3(cos(index*2*pi/max(count,1))*0.3,1+random*2,sin(index*2*pi/max(count,1))*0.3)',
          'velocity':'vec3(cos(index*2*pi/max(count,1))*1.3,random*1.4,sin(index*2*pi/max(count,1))*1.3)','rotation':'vec3(random*180,index*30,random*90)'},
          'update':{'rotation':'vec3(age*180,age*90,age*120)','opacity':'1-age','custom3':'1-age'}})])
asset('tornado',[
    emitter('wind',burst=96,maxParticles=96,lifetime=4,lifetimeVariation=.15,texture='assets/textures/smoke.png',columns=2,rows=2,frames=4,randomFrame=True,startSize=.6,startColor=[.4,.5,.7],batching=True,
        expressions={'birth':{'position':'vec3(0,index/max(count,1)*param.height,0)','rotation':'vec3(0,0,random*360)'},
          'update':{'position':'vec3(cos(age*6*pi+index*0.7)*(0.2+birthPosition.y*0.3),age*param.rise,sin(age*6*pi+index*0.7)*(0.2+birthPosition.y*0.3))',
             'size':'birthSize*(0.5+birthPosition.y*0.25)','opacity':'curve(age,0,0,0.15,0.32,0.8,0.32,1,0)','rotation':'vec3(0,0,age*180)'}}),
    emitter('motes',burst=32,maxParticles=32,lifetime=4,texture=GLOW,blend='add',startSize=.10,startColor=[.25,.7,1],batching=True,
        expressions={'birth':{'position':'vec3(0,index/max(count,1)*param.height,0)'},'update':{
          'position':'vec3(cos(age*6*pi+index*1.3)*(0.2+birthPosition.y*0.3),age*param.rise,sin(age*6*pi+index*1.3)*(0.2+birthPosition.y*0.3))','opacity':'curve(age,0,0,0.1,1,0.8,1,1,0)'}})],
    {'height':parameter(3.2,.5,5),'rise':parameter(.9,0,2)})
asset('shield_dome',[emitter('shield',lifetime=3,mesh='assets/models/vfx_dome.glb',material='materials/vfx_shield.mat.json',startColor=[.15,.65,1],
    expressions={'update':{'scale':'vec3(1,1,1)*param.radius*(1+sin(age*6*pi)*0.05)','custom0':'1+sin(age*4*pi)*0.3','custom3':'curve(age,0,0,0.15,1,0.8,1,1,0)'}})],{'radius':parameter(1.8,.5,3)})
asset('shockwave',[emitter('ring',lifetime=1.4,mesh='assets/models/vfx_ring.glb',material='materials/vfx_shockwave.mat.json',positionOffset=[0,.05,0],startColor=[1,.45,.08],
    expressions={'update':{'scale':'vec3(1,1,1)*(0.3+age*lifetime*param.speed)','custom0':'curve(age,0,1,0.3,2,1,0)','custom3':'curve(age,0,1,0.5,1,1,0)','opacity':'1-age'}})],{'speed':parameter(2,.1,4)})
asset('rain_snow',[
    emitter('rain',burst=12,rate=32,duration=1.8,maxParticles=40,lifetime=.6,texture=GLOW,space='world',shape='box',shapeSize=[1.5,0,1.8],positionOffset=[-1.8,3,0],startSize=.055,endSize=.055,startColor=[.35,.6,.9],align='velocity',stretch=.055,
        expressions={'update':{'velocity':'vec3(0,-birthPosition.y/lifetime,0)','opacity':'curve(age,0,0,0.1,0.9,0.9,0.9,1,0)'}}),
    emitter('snow',burst=16,rate=14,duration=1.8,maxParticles=48,lifetime=2.5,texture=GLOW,space='world',shape='box',shapeSize=[1.5,0,1.8],positionOffset=[1.8,3,0],startSize=.14,endSize=.14,startColor=[.8,.9,1],
        expressions={'update':{'velocity':'vec3(0,-birthPosition.y/lifetime,0)','position':'vec3(sin(age*2*pi+random*2*pi)*0.16,0,cos(age*2*pi+random*2*pi)*0.12)','opacity':'curve(age,0,0,0.1,0.9,0.9,0.9,1,0)'}})])
asset('healing_aura',[emitter('spiral',burst=48,maxParticles=48,lifetime=3,lifetimeVariation=.3,texture=GLOW,material='materials/vfx_healing.mat.json',startSize=.18,endSize=.18,startColor=[.1,1,.4],blend='add',batching=True,
    expressions={'birth':{'position':'vec3(0,0,0)','custom2':'random'},'update':{
      'position':'vec3(cos(age*6*pi+index*2*pi/max(count,1))*param.radius*(1-age*0.5),age*param.height,sin(age*6*pi+index*2*pi/max(count,1))*param.radius*(1-age*0.5))',
      'custom0':'0.3+sin(age*pi)*1.2+random*0.2','custom1':'age','size':'birthSize*(0.6+sin(age*pi)*0.7)','opacity':'curve(age,0,0,0.1,1,0.8,1,1,0)'}})],
    {'radius':parameter(1.2,.2,3),'height':parameter(3,.5,5)})
print('Generated six effects, six scenes, three meshes and three materials.')
