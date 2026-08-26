// rt_miss.rmiss — miss: payload de "sem hit".
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT Payload {
    uint hit;
    float t;
    int  prim;
} payload;

void main() {
    payload.hit  = 0u;
    payload.t    = -1.0;
    payload.prim = -1;
}
