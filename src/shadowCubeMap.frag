//

#include <Chapter11/07_MyFinalDemo/src/commonShadowPass.sp>
#include <data/shaders/AlphaTest.sp>
#include <data/shaders/UtilsPBR.sp>

layout (location=0) in vec2 uv;
layout (location=1) in flat uint materialId;

layout (location=2) in vec3 worldPos;

layout (location=3) in float factor;

layout (location=0) out vec4 out_FragColor;


void main() {
  MetallicRoughnessDataGPU mat = pc.materials.material[materialId];

 // if (mat.emissiveFactorAlphaCutoff.w > 0.5) discard;

//  float linearDepth = length(pc.lightPos - worldPos) * factor / 100.0f ;
  
//  vec3 lightPos = pc.cubemapIndex == 0? pc.lightPos[0] : pc.lightPos[1];
  float linearDepth = clamp ((length(pc.lightPos[pc.cubemapIndex].xyz - worldPos) - 0.1f)  / 9.9f, 0.f, 1.f);

  out_FragColor = vec4(linearDepth, 0.0f, 0.0f, 1.0f);

}
