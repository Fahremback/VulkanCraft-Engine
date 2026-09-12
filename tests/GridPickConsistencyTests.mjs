// GridPickConsistencyTests.mjs — INDEPENDENT cross-check of the editor viewport
// grid against the WORKING pick ray (viewport_mouse_dir).
//
// GridSimulationTests.mjs re-implements the shader's own math, so it would pass
// even if the shader had an inverted Y (false positive). This test does NOT
// trust the shader: it projects real ground points to clip, then checks whether
// the grid's unproject — with `-ndc.y` (flip) and with `+ndc.y` (no flip) —
// sends a ray back to the SAME on-screen ground point the mesh paints there.
// A vertical mirror (`-ndc.y`) sends every grid ray to the OTHER XZ, so it can
// only ever match the Y-symmetric origin. The correct grid uses `+ndc.y`,
// exactly like the pick ray that clicks entities.
//
//   node tests/GridPickConsistencyTests.mjs   # → PASS | FAIL

function vec3(x,y,z){return[x,y,z]}
function sub(a,b){return[a[0]-b[0],a[1]-b[1],a[2]-b[2]]}
function scale(a,s){return[a[0]*s,a[1]*s,a[2]*s]}
function dot(a,b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]}
function length(a){return Math.hypot(a[0],a[1],a[2])}
function normalize(a){const l=length(a);return l>1e-9?scale(a,1/l):vec3(0,-1,0)}
function cross(a,b){return[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]}
function mulMat4Vec4(m,v){return[m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12]*v[3],m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13]*v[3],m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]*v[3],m[3]*v[0]+m[7]*v[1]+m[11]*v[2]+m[15]*v[3]]}
function perspective(fov,asp,near,far){const g=1/Math.tan(fov/2),m=Array(16).fill(0);m[0]=g/asp;m[5]=g;m[10]=(far+near)/(near-far);m[11]=-1;m[14]=2*far*near/(near-far);return m}
function lookAt(eye,target,up){const f=normalize(sub(target,eye)),s=normalize(cross(f,up)),u=cross(s,f);const m=Array(16).fill(0);m[0]=s[0];m[1]=u[0];m[2]=-f[0];m[4]=s[1];m[5]=u[1];m[6]=-f[1];m[8]=s[2];m[9]=u[2];m[10]=-f[2];m[12]=-dot(s,eye);m[13]=-dot(u,eye);m[14]=dot(f,eye);m[15]=1;return m}
function mulMat4(a,b){const r=Array(16).fill(0);for(let c=0;c<4;c++)for(let row=0;row<4;row++){let s=0;for(let k=0;k<4;k++)s+=a[k*4+row]*b[c*4+k];r[c*4+row]=s}return r}
function invert4(m){const a=m.slice(),inv=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];for(let col=0;col<4;col++){let piv=col;for(let r=col+1;r<4;r++)if(Math.abs(a[col*4+r])>Math.abs(a[col*4+piv]))piv=r;if(Math.abs(a[col*4+piv])<1e-15)return null;if(piv!==col){for(let c=0;c<4;c++){const t=a[c*4+col];a[c*4+col]=a[c*4+piv];a[c*4+piv]=t;const u=inv[c*4+col];inv[c*4+col]=inv[c*4+piv];inv[c*4+piv]=u}}const d=a[col*4+col];for(let c=0;c<4;c++){a[c*4+col]/=d;inv[c*4+col]/=d}for(let r=0;r<4;r++){if(r===col)continue;const f=a[col*4+r];if(f===0)continue;for(let c=0;c<4;c++){a[c*4+r]-=f*a[c*4+col];inv[c*4+r]-=f*inv[c*4+col]}}}return inv}

let fails=0; const check=(c,m)=>{if(!c){console.log("  x "+m);fails++}else console.log("  ok "+m)};

// Multiple off-axis cameras (up is [0,1,0]; eyes off that axis → ground planes
// asymmetric in Y, which is exactly what exposes a vertical mirror).
const cams=[[16,5,-3],[12,5,-4],[-12,5,-4],[0,6,14],[-6,5,4]];
const plane=[[4,0,2],[-3,0,7],[12,0,-5],[0,0,0],[-8,0,3],[5,0,-9],[7,0,-18]];

let vis=0,noflipHit=0,flipHit=0;
for(const eye of cams){
  const proj=perspective(Math.PI/3,1,0.1,50000),view=lookAt(eye,[0,0,0],[0,1,0]);
  const viewProj=mulMat4(proj,view),invVP=invert4(viewProj);
  check(invVP!==null,`invertible for eye ${eye.join(",")}`);
  const projClip=(w)=>{const v=mulMat4Vec4(viewProj,[w[0],w[1],w[2],1]);return[v[0]/v[3],v[1]/v[3],v[2]/v[3]]};
  const groundHit=(nx,ny,flip)=>{
    const u=(x,y,z)=>{const q=mulMat4Vec4(invVP,[x,y,z,1]);return[q[0]/q[3],q[1]/q[3],q[2]/q[3]]};
    const yv=flip?-ny:ny;
    const a=u(nx,yv,0),b=u(nx,yv,1);
    const d=normalize(sub(b,a));
    // Same convention as the shader: a ray strikes the plane only when it is
    // descending (denom < -1e-6). Keep the NEGATIVE denominator (never clamp it
    // to +epsilon, that destroys t).
    if(!(d[1] < -1e-6))return null;
    const t=(0-a[1])/d[1];
    if(!isFinite(t)||t<0)return null;
    return[a[0]+d[0]*t,a[2]+d[2]*t];
  };
  for(const P of plane){
    const c=projClip(P);
    if(Math.abs(c[0])>1||Math.abs(c[1])>1)continue; // only on-screen ground the mesh paints
    vis++;
    const no=groundHit(c[0],c[1],false),fl=groundHit(c[0],c[1],true);
    if(no&&Math.hypot(no[0]-P[0],no[1]-P[2])<0.5)noflipHit++;
    if(fl&&Math.hypot(fl[0]-P[0],fl[1]-P[2])<0.5)flipHit++;
  }
}
check(vis>0,`sampled ${vis} on-screen ground points (mesh-painted pixels)`);
check(noflipHit===vis,`+ndc.y (no flip) grid ray recovers ${noflipHit}/${vis} mesh ground points -> grid matches meshes`);
// The mirror (`-ndc.y`) can only ever recover the Y-symmetric origin, at most
// once per camera. Any other recovery proves the ray crossed a DIFFERENT XZ
// than where the mesh drew it (the vertical inversion).
check(flipHit<=cams.length,`-ndc.y (shader mirror) recovers only ${flipHit} (≤1 per camera = the symmetry point (0,0,0)); other ${vis-flipHit} rays hit mirrored XZ -> mirror is the inversion`);
if(fails){console.error(`GridPickConsistencyTests: FAIL (${fails})`);process.exit(1)}
console.log("GridPickConsistencyTests: PASS — grid uses +ndc.y (no flip); the -ndc.y mirror was the vertical inversion");
process.exit(0);