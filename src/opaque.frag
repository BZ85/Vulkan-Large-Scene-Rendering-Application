//

#include <Chapter11/07_MyFinalDemo/src/common.sp>
#include <data/shaders/AlphaTest.sp>
#include <data/shaders/Shadow.sp>
#include <data/shaders/UtilsPBR.sp>

layout (location=0) in vec2 uv;
layout (location=1) in vec3 normal;
layout (location=2) in vec3 worldPos;
layout (location=3) in flat uint materialId;
layout (location=4) in vec4 shadowCoords;

layout (location=0) out vec4 out_FragColor;


  // PCF3X3 kernal for cubemap and point light shadow
  float PCF3x3CubeMap(vec3 uvw, float currentDepth, uint textureid, uint samplerid) {
  float size = 1.0 / textureSize(nonuniformEXT(kTexturesCube[textureid]), 0).x; // assume square texture
  float shadow = 0.0f;
  for (int v=-1; v<=+1; v++)
    for (int u=-1; u<=+1; u++){
      
      shadow += currentDepth > textureBindlessCube(textureid, samplerid, uvw + size * vec3(u, v, 0)).r + 0.01 ? 0.0 : 1.0;
 }
 return shadow / 9;
}

// shadow function for point light shadow (cubemap), return the averaged shadow value
float shadowCubeMap(vec3 s, float currentDepth, uint textureid, uint samplerid) {
  
    float shadowSample = PCF3x3CubeMap(s, currentDepth, textureid, samplerid);
    return mix(0.1, 1.0, shadowSample);

}



void main() {
  MetallicRoughnessDataGPU mat = pc.materials.material[materialId];

  vec4 emissiveColor = vec4(mat.emissiveFactorAlphaCutoff.rgb, 0) * textureBindless2D(mat.emissiveTexture, 0, uv);
  vec4 baseColor     = mat.baseColorFactor * (mat.baseColorTexture > 0 ? textureBindless2D(mat.baseColorTexture, 0, uv) : vec4(1.0));

  // scale alpha-cutoff by fwidth() to prevent alpha-tested foliage geometry from vanishing at large distances
  // https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f
  runAlphaTest(baseColor.a, mat.emissiveFactorAlphaCutoff.w / max(32.0 * fwidth(uv.x), 1.0));

  // world-space normal
  vec3 n = normalize(normal);

  // normal mapping: skip missing normal maps
  vec3 normalSample = textureBindless2D(mat.normalTexture, 0, uv).xyz;
  if (length(normalSample) > 0.5)
    n = perturbNormal(n, worldPos, normalSample, uv);

  const bool hasSkybox = pc.texSkyboxIrradiance > 0;

  // one directional light
  float NdotL = clamp(dot(n, -normalize(pc.light.lightDir.xyz)), 0.1, 1.0);

  // IBL diffuse - not trying to be PBR-correct here, just make it simple & shiny
  const vec4 f0 = vec4(0.04);
  vec3 sky = vec3(-n.x, n.y, -n.z); // rotate skybox
  vec4 diffuse = (textureBindlessCube(pc.texSkyboxIrradiance, 0, sky) + vec4(NdotL)) * baseColor * (vec4(1.0) - f0);

  // point light
  vec4 diffusePointLight = vec4(0,0,0,0);

  //for(int i = 0; i < pc.pointLight.lightsCount; i++){
  vec3 toLight;
  float distance;

  for(int i = 0; i < 4; i++){
  vec3 pointLightPos = pc.pointLight.pointLightParam[i].lightPos.xyz;
 // vec3 pointLightColor = pc.pointLight.pointLightParam[i].color.xyz;
 // float pointLightIntensity = pc.pointLight.pointLightParam[i].intensity;
  float pointLightRadius = pc.pointLight.pointLightParam[i].radius;
  //float pointLightRadius = 10.f;
 
  toLight = pointLightPos - worldPos;
  distance = length(toLight);


  float NdotLPointLight = max(dot(n, normalize(toLight)), 0.0);
 // float attentuation = (1.0 - clamp(distance / pointLightRadius, 0.0, 1.0));
//  diffusePointLight += NdotLPointLight * (1.0 / max(distance * distance, 1e-4) ) * attentuation * pointLightIntensity* vec4(pointLightColor, 1.0f) * baseColor ;
float attentuation = (1.0 - clamp(distance / pointLightRadius, 0.0, 1.0));
toLight.z = - toLight.z;
if(i == 0 || i == 1){
float currentDistance = (length(toLight) - 0.1f) / 9.9f;

// not using PCF
float storedDistance = textureBindlessCube(pc.pointLight.shadowCubeMapTexture[i], 0, -normalize(toLight)).r;
float pointLightShadow = currentDistance > storedDistance + 0.01? 0.0 : 1.0;

// using PCF
//float pointLightShadow = shadowCubeMap(-normalize(toLight), currentDistance, pc.pointLight.shadowCubeMapTexture[i], 0);
diffusePointLight += pointLightShadow * NdotLPointLight * (1.0 / max(distance * distance, 1e-4) ) * attentuation * vec4(1.0f, 1.0f, 1.0f, 1.0f) * baseColor ;
}


else diffusePointLight += NdotLPointLight * (1.0 / max(distance * distance, 1e-4) ) * attentuation * 0.1 * vec4(1.0f, 1.0f, 1.0f, 1.0f)  * baseColor ;

  }
  
 // toLight.z = - toLight.z;

//  float storedDistance = textureBindlessCube(pc.pointLight.shadowCubeMapTexture, 0, -normalize(toLight)).r;
 // float currentDistance = (length(lightToFrag) - 0.1f) / 9.9f;

 // float pointLightShadow = currentDistance > storedDistance + 0.01? 0.0 : 1.0;
 // diffusePointLight *= pointLightShadow;
  //diffusePointLight *= shadowCubeMap(-normalize(toLight), currentDistance, pc.pointLight.shadowCubeMapTexture, 0); 
 
 out_FragColor = emissiveColor + diffusePointLight +  0.1 * diffuse * shadow(shadowCoords, pc.light.shadowTexture, pc.light.shadowSampler);
 
 
}
