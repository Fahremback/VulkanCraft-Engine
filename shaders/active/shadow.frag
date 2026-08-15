#version 450
layout(location=0) in vec3 shadowUV;
layout(binding=0) uniform sampler2DArray albedoSampler;
void main(){
    if(shadowUV.z>=0.0 && texture(albedoSampler,shadowUV).a<0.30) discard;
}
