// rt_closesthit.rchit — grava o hit (distância + primitive index) no payload.
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT Payload {
    uint hit;
    float t;
    int  prim;
} payload;

void main() {
    payload.hit  = 1u;
    payload.t    = gl_HitTEXT;
    payload.prim = int(gl_PrimitiveID);
}
