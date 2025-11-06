//

#include <data/shaders/gltf/common_material.sp>
#include <Chapter11/04_OIT/src/common_oit.sp>

struct DrawData {
  uint transformId;
  uint materialId;
};

layout(std430, buffer_reference) readonly buffer TransformBuffer {
  mat4 model[];
};

layout(std430, buffer_reference) readonly buffer DrawDataBuffer {
  DrawData dd[];
};

layout(std430, buffer_reference) readonly buffer MaterialBuffer {
  MetallicRoughnessDataGPU material[];
};

layout(std430, buffer_reference) buffer AtomicCounter {
  uint numFragments;
};

layout(std430, buffer_reference) readonly buffer LightBuffer {
  mat4 viewProjBias;
  vec4 lightDir;
  uint shadowTexture;
  uint shadowSampler;

 // vec2 padding;
//  vec4 lightPos;
 // vec4 color;      
 // float radius;    
//  float intensity;
};


struct PointLightParam{
   vec4 lightPos;
   vec4 color;      
   float radius;    
   float intensity;
   float pad[2];
};

layout(std430, buffer_reference) readonly buffer PointLightBuffer {
 //  vec4 lightPos;
  // vec4 color;      
 //  float radius;    
 //  float intensity;
  uint lightsCount;
  uint pad[1];
  uint shadowCubeMapTexture[2];
  PointLightParam pointLightParam[];
};

layout(std430, buffer_reference) buffer OIT {
  AtomicCounter atomicCounter;
  TransparencyListsBuffer oitLists;
  uint texHeadsOIT;
  uint maxOITFragments;
};


layout(std430, buffer_reference) readonly buffer AddressTable {
  TransformBuffer transforms;
  DrawDataBuffer drawData;
 // MaterialBuffer materials;
//  OIT oit;
//  LightBuffer light; // one directional light
//  PointLightBuffer pointLight; // one point light source
 // uint texSkybox;
//  uint texSkyboxIrradiance;
};


/*
layout(push_constant) uniform PushConstant {
  mat4 viewProj;
  vec4 cameraPos;
  PreFrameData preFrameData;
}pc;
*/


layout(push_constant) uniform PerFrameData {
  mat4 viewProj;
  vec4 cameraPos;
  //TransformBuffer transforms;
  //DrawDataBuffer drawData;
  MaterialBuffer materials;
  OIT oit;
  LightBuffer light; // one directional light
  PointLightBuffer pointLight; // one point light source
  AddressTable addressTable;
  uint texSkybox;
  uint texSkyboxIrradiance;
} pc;
