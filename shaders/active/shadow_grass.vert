#version 450
layout(location=0) in vec4 inPositionRotation;
layout(location=0) out vec3 shadowUV;
layout(push_constant) uniform PushConstants{mat4 mvp;vec4 cameraPos;vec4 sunDirection;vec4 sunColor;vec4 environment;}push;
const vec2 C[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
float hash12(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
vec3 projectShadow(vec3 worldPosition){
 vec3 lightDir=normalize(push.environment.y>.03?push.sunDirection.xyz:-push.sunDirection.xyz);vec3 referenceUp=abs(lightDir.y)>.96?vec3(0,0,1):vec3(0,1,0);
 vec3 right=normalize(cross(referenceUp,lightDir));vec3 up=normalize(cross(lightDir,right));const float snap=.125;
 vec2 center=floor(vec2(dot(push.cameraPos.xyz,right),dot(push.cameraPos.xyz,up))/snap+.5)*snap;
 vec2 projected=(vec2(dot(worldPosition,right),dot(worldPosition,up))-center)/512.0;float distortion=.16+.84*clamp(length(projected),0.0,1.0);
 return vec3(projected/distortion,clamp(.5-dot(worldPosition-push.cameraPos.xyz,lightDir)/1280.0,0.0,1.0));
}
void main(){
 vec2 uv=C[gl_VertexIndex];float random=hash12(inPositionRotation.xz);float viewDistance=length(push.cameraPos.xyz-inPositionRotation.xyz);
 float lod=smoothstep(26.0,210.0,viewDistance);float instanceHash=hash12(inPositionRotation.xz*vec2(31.73,57.91)+inPositionRotation.yy*4.17);
 float frontierFade=1.0;if(push.sunColor.w>=0.0){vec2 center=(floor(push.cameraPos.xz/16.0)+vec2(.5))*16.0;
  float halfExtent=(push.sunColor.w+.5)*16.0;float d=max(abs(inPositionRotation.x-center.x),abs(inPositionRotation.z-center.y));
  frontierFade=1.0-smoothstep(max(0.0,halfExtent-28.0),halfExtent,d);}
 if(instanceHash>mix(1.0,.105,lod)*frontierFade){gl_Position=vec4(2,2,2,1);shadowUV=vec3(0,0,-1);return;}
 float width=(.18+random*.06)*mix(1.0,3.65,lod);float height=(.24+random*.12)*mix(1.0,1.62,lod);
 int edgeMask=int(floor(inPositionRotation.w));float rotation=fract(inPositionRotation.w)*6.2831853;vec2 axis=vec2(cos(rotation),sin(rotation));
 vec3 pos=inPositionRotation.xyz;pos.xz+=axis*((uv.x-.5)*width);pos.y+=uv.y*height;
 if(uv.y>0.0){float wind=sin(pos.x*1.31+pos.z*1.73+push.environment.x*1.65)*.025;pos.xz+=vec2(wind,wind*.63)*uv.y;}
 vec2 blockMin=floor(inPositionRotation.xz);const float inset=.002;
 if((edgeMask&1)!=0)pos.x=max(pos.x,blockMin.x+inset);if((edgeMask&2)!=0)pos.x=min(pos.x,blockMin.x+1.0-inset);
 if((edgeMask&4)!=0)pos.z=max(pos.z,blockMin.y+inset);if((edgeMask&8)!=0)pos.z=min(pos.z,blockMin.y+1.0-inset);
 int tile=int(floor(hash12(inPositionRotation.xy+inPositionRotation.zw)*4.0));
 shadowUV=vec3(vec2(uv.x,1.0-uv.y)*.5+vec2(float(tile&1),float(tile>>1))*.5,15.0);
 gl_Position=vec4(projectShadow(pos),1);
}
