#version 450
layout(location=0) in vec4 inPositionScale;
layout(location=0) out vec3 shadowUV;
layout(push_constant) uniform PushConstants { mat4 mvp; vec4 cameraPos; vec4 sunDirection; vec4 sunColor; vec4 environment; } push;
const vec2 C[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
float hash12(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
vec3 projectShadow(vec3 worldPosition){
 vec3 lightDir=normalize(push.environment.y>.03?push.sunDirection.xyz:-push.sunDirection.xyz);vec3 referenceUp=abs(lightDir.y)>.96?vec3(0,0,1):vec3(0,1,0);
 vec3 right=normalize(cross(referenceUp,lightDir));vec3 up=normalize(cross(lightDir,right));const float snap=.5;
 vec2 center=floor(vec2(dot(push.cameraPos.xyz,right),dot(push.cameraPos.xyz,up))/snap+.5)*snap;
 vec2 projected=(vec2(dot(worldPosition,right),dot(worldPosition,up))-center)/512.0;float distortion=.16+.84*clamp(length(projected),0.0,1.0);
 return vec3(projected/distortion,clamp(.5-dot(worldPosition-push.cameraPos.xyz,lightDir)/1280.0,0.0,1.0));
}
void main(){
 int plane=gl_VertexIndex/6; vec2 uv=C[gl_VertexIndex%6];
 float random=hash12(inPositionScale.xz+vec2(inPositionScale.y,float(plane)*7.17));
 float viewDistance=length(push.cameraPos.xyz-inPositionScale.xyz);float lod=smoothstep(48.0,420.0,viewDistance);
 float frontierFade=1.0;if(push.sunColor.w>=0.0){vec2 center=(floor(push.cameraPos.xz/16.0)+vec2(.5))*16.0;
  float halfExtent=(push.sunColor.w+.5)*16.0;float d=max(abs(inPositionScale.x-center.x),abs(inPositionScale.z-center.y));
  frontierFade=1.0-smoothstep(max(0.0,halfExtent-24.0),halfExtent,d);}
 float instanceHash=hash12(inPositionScale.xz*vec2(19.31,43.17)+inPositionScale.yy*2.73);
 if(instanceHash>mix(1.0,.14,lod)*frontierFade){gl_Position=vec4(2,2,2,1);shadowUV=vec3(0,0,-1);return;}
 float angle=random*6.2831853+float(plane)*1.5707963; vec2 axis=vec2(cos(angle),sin(angle));
 float width=inPositionScale.w*mix(1.0,2.85,lod); float height=width*(.78+random*.16); vec3 pos=inPositionScale.xyz;
 if(plane<2){pos.xz+=axis*((uv.x-.5)*width);pos.y+=(uv.y-.5)*height;}
 else{vec2 perp=vec2(-axis.y,axis.x);pos.xz+=axis*((uv.x-.5)*width)+perp*((uv.y-.5)*height);pos.y+=(uv.y-.5)*height*.16;}
 float wind=sin(push.environment.x*1.18+random*6.283+pos.x*.31+pos.z*.27)*(.035+random*.035);
 pos.xz+=vec2(wind,wind*.58)*uv.y*uv.y;
 int tile=int(floor(hash12(inPositionScale.xy+vec2(float(plane)*3.1,inPositionScale.z))*4.0));
 shadowUV=vec3(uv*.5+vec2(float(tile&1),float(tile>>1))*.5,8.0);
 gl_Position=vec4(projectShadow(pos),1);
}
