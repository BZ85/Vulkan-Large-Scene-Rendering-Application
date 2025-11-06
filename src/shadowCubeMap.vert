//
//#extension GL_EXT_multiview : require // required for enabling mult-view in GLSL

#include <Chapter11/07_MyFinalDemo/src/commonShadowPass.sp>

layout (location=0) in vec3 in_pos;
layout (location=1) in vec2 in_tc;
layout (location=2) in vec3 in_normal;

layout (location=0) out vec2 uv;
layout (location=1) out flat uint materialId;

layout (location=2) out vec3 worldPos;
layout (location=3) out float factor;


void main() {
  mat4 model = pc.transforms.model[pc.drawData.dd[gl_BaseInstance].transformId];
  gl_Position = pc.viewProj * model * vec4(in_pos, 1.0);
  uv = vec2(in_tc.x, 1.0-in_tc.y);
  materialId = pc.drawData.dd[gl_BaseInstance].materialId;

  vec4 posClip = model * vec4(in_pos, 1.0);
  worldPos = posClip.xyz/posClip.w;
 // factor = gl_ViewIndex;
 factor = gl_Position.w;
}
