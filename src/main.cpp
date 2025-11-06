/* Xinjie Zhu's final graphics application
for the whole Vulkan 3D graphics and rendering book*/

#include "shared/VulkanApp.h"
#include "shared/Tonemap.h"

#define DEMO_TEXTURE_MAX_SIZE 2048
#define DEMO_TEXTURE_CACHE_FOLDER ".cache/out_textures_11/"
#define fileNameCachedMeshes ".cache/ch11_bistro.meshes"
#define fileNameCachedMaterials ".cache/ch11_bistro.materials"
#define fileNameCachedHierarchy ".cache/ch11_bistro.scene"

#include "Chapter10/Bistro.h"
#include "Chapter10/Skybox.h"
#include "Chapter11/VKMesh11Lazy.h"

bool drawMeshesOpaque      = true;
bool drawMeshesTransparent = true;
bool drawWireframe         = false;
bool drawBoxes             = false;
bool drawLightFrustum      = false;
// SSAO
bool ssaoEnable          = true;
bool ssaoEnableBlur      = true;
int ssaoNumBlurPasses    = 1;
float ssaoDepthThreshold = 30.0f; // bilateral blur
// OIT
bool oitShowHeatmap   = false;
float oitOpacityBoost = 0.0f;
// HDR
bool hdrDrawCurves       = false;
bool hdrEnableBloom      = true;
float hdrBloomStrength   = 0.01f;
int hdrNumBloomPasses    = 2;
float hdrAdaptationSpeed = 3.0f;
// Culling
enum CullingMode {
  CullingMode_None = 0,
  CullingMode_CPU  = 1,
  CullingMode_GPU  = 2,
};
mat4 cullingView       = mat4(1.0f);
int cullingMode        = CullingMode_CPU;
bool freezeCullingView = false;

int frameCount = 0; // use if we don't do culling every frame 
bool cullingEveryFrame = true;
bool compactedBuffer = true;

// the directional light params struct isn't uploaded to GPU
// but is used to compute light view and proj matrices, then the martices are uploaded to GPU
// depth bias parameters are set by cmdSetDepthBias function
struct LightParams {
  float theta          = +90.0f;
  float phi            = -26.0f;
  float depthBiasConst = 1.1f;
  float depthBiasSlope = 2.0f;

  bool operator==(const LightParams&) const = default;
} light;


const uint32_t pointLightsNum = 8;

bool pointLightChanged = true; // it's true for the first frame when updating shadow cubemap
bool drawPointLightMarker = false;

struct PointLightData {
  // mat4 viewProjBias;
  vec4 lightPos   = vec4(8.0f, 3.0f, 1.0f, 1.0f);
  vec4 color      = vec4(1.0f, 1.0f, 1.0f, 1.0f);
  float radius    = 10.0f;
  float intensity = 1.0f; // Optional, default 1.0
                        
  float pad[2] = { 0.0f, 0.0f };// add padding to align to 16 bytes, necessary for GPU data array
  //  uint32_t shadowTexture;
  //  uint32_t shadowSampler;
  bool operator==(const PointLightData&) const = default;
  //} pointLightData[pointLightsNum];
};

struct PointLightBlock {
  uint32_t count = pointLightsNum;
 // uint32_t pad[3] = { 0, 0, 0 };
  uint32_t pad = 0;
  uint32_t shadowCubeMapTexture[2] = { 0, 0 };
  PointLightData pointLightData[pointLightsNum];
} pointLightBlock;

// tile based rendering parameters

struct PointLightDataForTile {
  // xyz is position and a is radius
  vec4 lightPos_Radius = vec4(0.0f, 0.0f, 0.0f, 1.0f); 
} pointLightDataForTile[pointLightsNum];


const uint32_t tileSizeX = 16;
const uint32_t tileSizeY = 16;
uint32_t tileCountX = 0;
uint32_t tileCountY = 0;
uint32_t numtiles = 0;




int main()
{
  MeshData meshData;
  Scene scene;
  loadBistro(meshData, scene);

  VulkanApp app({
      .initialCameraPos    = vec3(-18.621f, 4.621f, -6.359f),
      .initialCameraTarget = vec3(0, +5.0f, 0),
  });

  app.positioner_.maxSpeed_ = 1.5f;

  LineCanvas3D canvas3d;

  std::unique_ptr<lvk::IContext> ctx(app.ctx_.get());

  // get the dimension (size) of the swapchain image (window size)
  const lvk::Dimensions sizeFb = ctx->getDimensions(ctx->getCurrentSwapchainTexture());

  // compute the tile counts using size of framebuffer
  tileCountX = (sizeFb.width + tileSizeX - 1) / tileSizeX;
  tileCountY = (sizeFb.height + tileSizeY - 1) / tileSizeY;
  numtiles   = tileCountX * tileCountY;

  // MSAA sample count
  const uint32_t kNumSamples         = 8;
  // the format set for HDR rendering pipeline
  const lvk::Format kOffscreenFormat = lvk::Format_RGBA_F16;

  // MSAA
  lvk::Holder<lvk::TextureHandle> msaaColor = ctx->createTexture({
      .format     = kOffscreenFormat,
      .dimensions = sizeFb,
      .numSamples = kNumSamples,
      .usage      = lvk::TextureUsageBits_Attachment,
      .storage    = lvk::StorageType_Memoryless,
      .debugName  = "msaaColor",
  });

  lvk::Holder<lvk::TextureHandle> msaaDepth = ctx->createTexture({
      .format     = app.getDepthFormat(),
      .dimensions = sizeFb,
      .numSamples = kNumSamples,
      .usage      = lvk::TextureUsageBits_Attachment,
      .storage    = lvk::StorageType_Memoryless,
      .debugName  = "msaaDepth",
  });

  // resolve texture for msaaDepth
  lvk::Holder<lvk::TextureHandle> texOpaqueDepth = ctx->createTexture({
      .format     = app.getDepthFormat(),
      .dimensions = sizeFb,
      .usage      = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled,
      .debugName  = "opaqueDepth",
  });

  // resolve texture for msaaColor
  lvk::Holder<lvk::TextureHandle> texOpaqueColor = ctx->createTexture({
      .format     = kOffscreenFormat,
      .dimensions = sizeFb,
      .usage      = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
      .debugName  = "opaqueColor",
  });

  // store the opaque objects scene with SSAO effect applied 
  lvk::Holder<lvk::TextureHandle> texOpaqueColorWithSSAO = ctx->createTexture({
      .format     = kOffscreenFormat,
      .dimensions = sizeFb,
      .usage      = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
      .debugName  = "opaqueColorWithSSAO",
  });
  // final HDR scene color (SSAO + OIT)
  lvk::Holder<lvk::TextureHandle> texSceneColor = ctx->createTexture({
      .format     = kOffscreenFormat,
      .dimensions = sizeFb,
      .usage      = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
      .debugName  = "sceneColor",
  });

  // HDR light adaptation
  const lvk::Dimensions sizeBloom = { 512, 512 };

  lvk::Holder<lvk::TextureHandle> texBrightPass = ctx->createTexture({
      .format     = kOffscreenFormat,
      .dimensions = sizeBloom,
      .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
      .debugName  = "texBrightPass",
  });
  lvk::Holder<lvk::TextureHandle> texBloomPass  = ctx->createTexture({
       .format     = kOffscreenFormat,
       .dimensions = sizeBloom,
       .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
       .debugName  = "texBloomPass",
  });
  // ping-pong
  lvk::Holder<lvk::TextureHandle> texBloom[] = {
    ctx->createTexture({
        .format     = kOffscreenFormat,
        .dimensions = sizeBloom,
        .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
        .debugName  = "texBloom0",
    }),
    ctx->createTexture({
        .format     = kOffscreenFormat,
        .dimensions = sizeBloom,
        .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
        .debugName  = "texBloom1",
    }),
  };

  const lvk::ComponentMapping swizzle = { .r = lvk::Swizzle_R, .g = lvk::Swizzle_R, .b = lvk::Swizzle_R, .a = lvk::Swizzle_1 };

  lvk::Holder<lvk::TextureHandle> texLumViews[10] = { ctx->createTexture({
      .format       = lvk::Format_R_F16,
      .dimensions   = sizeBloom,
      .usage        = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
      .numMipLevels = lvk::calcNumMipLevels(sizeBloom.width, sizeBloom.height),
      .swizzle      = swizzle,
      .debugName    = "texLuminance",
  }) };

  for (uint32_t v = 1; v != LVK_ARRAY_NUM_ELEMENTS(texLumViews); v++) {
    texLumViews[v] = ctx->createTextureView(texLumViews[0], { .mipLevel = v, .swizzle = swizzle }, "texLumViews[]");
  }

  const uint16_t brightPixel = glm::packHalf1x16(50.0f);

  // ping-pong textures for iterative luminance adaptation
  const lvk::TextureDesc luminanceTextureDesc{
    .format     = lvk::Format_R_F16,
    .dimensions = {1, 1},
    .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
    .swizzle    = swizzle,
    .data       = &brightPixel,
  };
  lvk::Holder<lvk::TextureHandle> texAdaptedLum[2] = {
    ctx->createTexture(luminanceTextureDesc, "texAdaptedLuminance0"),
    ctx->createTexture(luminanceTextureDesc, "texAdaptedLuminance1"),
  };
  // shadows
  // 2D shadow map for directional light
  lvk::Holder<lvk::TextureHandle> texShadowMap = ctx->createTexture({
      .type       = lvk::TextureType_2D,
      .format     = lvk::Format_Z_UN16,
      .dimensions = { 4096, 4096 },
      .usage      = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled,
		// use swizzling to make the map to be grayscale
		.swizzle    = { .r = lvk::Swizzle_R, .g = lvk::Swizzle_R, .b = lvk::Swizzle_R, .a = lvk::Swizzle_1 },
      .debugName  = "Shadow map",
  });

  // shadow cubemap for point lights
  lvk::Holder<lvk::TextureHandle> texShadowCubeMap[2] = {
    ctx->createTexture({
                        .type = lvk::TextureType_Cube,
                        // .format     = lvk::Format_Z_UN16,
        .format     = lvk::Format_RGBA_F32,
                        .dimensions = { 2048, 2048 },
                        // used as both color attachment (in shadow pass) and sampled image (in lighting pass)
        .usage = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled,
                        // use swizzling to make the map to be grayscale
        // we don't need swizzling if we only use the r channel of the texture
        // .swizzle   = { .r = lvk::Swizzle_R, .g = lvk::Swizzle_R, .b = lvk::Swizzle_R, .a = lvk::Swizzle_1 },
        .debugName = "Shadow cubemap",
                        }
    ),

	 ctx->createTexture({
                        .type = lvk::TextureType_Cube,      
                        .format     = lvk::Format_RGBA_F32,
                        .dimensions = { 2048, 2048 },
                        // used as both color attachment (in shadow pass) and sampled image (in lighting pass)
                        .usage = lvk::TextureUsageBits_Attachment | lvk::TextureUsageBits_Sampled,
                        .debugName = "Shadow cubemap",
                        }
    ),

  };

	// the depth texture used in shadow pass should be the same resolution as the shadow cubemap
	 lvk::Holder<lvk::TextureHandle> texDepthShadowPass = ctx->createTexture({
       .type       = lvk::TextureType_2D,
       .format     = lvk::Format_Z_F32,
       .dimensions = { 2048, 2048 },
       .usage      = lvk::TextureUsageBits_Attachment,
       .debugName  = "Depth buffer for cubemap shadow pass",
   });


 // shadow sampler can be shared by both 2D shadow pass (directional light) and cube map shadow pass (point light)
  lvk::Holder<lvk::SamplerHandle> samplerShadow = ctx->createSampler({
      .wrapU               = lvk::SamplerWrap_Clamp,
      .wrapV               = lvk::SamplerWrap_Clamp,
      .depthCompareOp      = lvk::CompareOp_LessEqual,
      .depthCompareEnabled = true, // compare operation is necessary to be enabled for shadow sampler
      .debugName           = "Sampler: shadow",
  });

  // create the texture views of shadow cube maps for showing on the UI
  lvk::Holder<lvk::TextureHandle> texShadowCubeMapFaces[6];

  for (uint32_t i = 0; i < 6; i++) {
    texShadowCubeMapFaces[i] = ctx->createTextureView(texShadowCubeMap[0], { .layer = i, .swizzle = swizzle }, "shadow cubemap faces");
  }

  pointLightBlock.shadowCubeMapTexture[0] = texShadowCubeMap[0].index();
  pointLightBlock.shadowCubeMapTexture[1] = texShadowCubeMap[1].index();

  // directional light
  struct LightData {
    mat4 viewProjBias;
    vec4 lightDir;
    uint32_t shadowTexture;
    uint32_t shadowSampler;
  };

  // buffer to pass directional light data to GPU
  lvk::Holder<lvk::BufferHandle> bufferLight = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(LightData),
      .debugName = "Buffer: light",
  });

  // point lights positions initialization
  // use default values for other parameters like color and radius 
  pointLightBlock.pointLightData[0].lightPos = glm::vec4(8.0f, 2.5f, 1.0f, 1.0f);
  pointLightBlock.pointLightData[1].lightPos = glm::vec4(3.25f, 2.5f, -4.845f, 1.0f);
  pointLightBlock.pointLightData[2].lightPos = glm::vec4(0.475f, 2.5f, 0.662f, 1.0f);
  pointLightBlock.pointLightData[3].lightPos = glm::vec4(-0.73f, 2.5f, -2.81f, 1.0f);
  pointLightBlock.pointLightData[4].lightPos = glm::vec4(5.0f, 1.0f, 1.0f, 1.0f);
  pointLightBlock.pointLightData[5].lightPos = glm::vec4(4.0f, 1.0f, 1.0f, 1.0f);
  pointLightBlock.pointLightData[6].lightPos = glm::vec4(3.0f, 1.0f, 1.0f, 1.0f);
  pointLightBlock.pointLightData[7].lightPos = glm::vec4(2.0f, 1.0f, 1.0f, 1.0f);

  // set the same pos and radius for point light data for tile pass
  for (int i = 0; i < pointLightsNum; i++)
    pointLightDataForTile[i].lightPos_Radius =  glm::vec4(glm::vec3(pointLightBlock.pointLightData[i].lightPos), pointLightBlock.pointLightData[i].radius);



  // create an array storing pointLightData from last frame
  // and copy the default value from pointLightData into it
  PointLightData pointLightDataPrevious[pointLightsNum] = {};
  std::copy(std::begin(pointLightBlock.pointLightData), std::end(pointLightBlock.pointLightData), pointLightDataPrevious);

   // buffer to pass point light data to GPU
  lvk::Holder<lvk::BufferHandle> bufferPointLight = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(PointLightBlock),
      .data      = &pointLightBlock,
      .debugName = "Buffer: pointLight",
  });
  

  // for tile pass, only upload the point light data array (not entire point light block)
  // since we don't need texture indices, and for light counts we can set it as push constant
  lvk::Holder<lvk::BufferHandle> bufferPointLightForTilePass = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(PointLightDataForTile) * pointLightsNum,
      .data      = pointLightDataForTile,
      .debugName = "Buffer: pointLight for tile pass",
  });




  lvk::Holder<lvk::TextureHandle> texSSAO = ctx->createTexture({
                     .format     = ctx->getSwapchainFormat(),
                     .dimensions = ctx->getDimensions(ctx->getCurrentSwapchainTexture()),
                     .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
                     .debugName  = "texSSAO",
  });
  lvk::Holder<lvk::TextureHandle> texBlur[] = {
    ctx->createTexture({
                     .format     = ctx->getSwapchainFormat(),
                     .dimensions = ctx->getDimensions(ctx->getCurrentSwapchainTexture()),
                     .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
                     .debugName  = "texBlur0",
    }),
    ctx->createTexture({
                     .format     = ctx->getSwapchainFormat(),
                     .dimensions = ctx->getDimensions(ctx->getCurrentSwapchainTexture()),
                     .usage      = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Storage,
                     .debugName  = "texBlur1",
    }),
  };

  lvk::Holder<lvk::SamplerHandle> samplerClamp = ctx->createSampler({
      .wrapU = lvk::SamplerWrap_Clamp,
      .wrapV = lvk::SamplerWrap_Clamp,
      .wrapW = lvk::SamplerWrap_Clamp,
  });

  // OIT setup
  const Skybox skyBox(
      ctx, "data/immenstadter_horn_2k_prefilter.ktx", "data/immenstadter_horn_2k_irradiance.ktx", kOffscreenFormat, app.getDepthFormat(),
      kNumSamples);
  VKMesh11Lazy mesh(ctx, meshData, scene);
  const VKPipeline11 pipelineOpaque(
      ctx, meshData.streams, kOffscreenFormat, app.getDepthFormat(), kNumSamples,
      loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/main.vert"), loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/opaque.frag"));
  const VKPipeline11 pipelineTransparent(
      ctx, meshData.streams, kOffscreenFormat, app.getDepthFormat(), kNumSamples,
      loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/main.vert"), loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/transparent.frag"));
  const VKPipeline11 pipelineShadow(
      ctx, meshData.streams, lvk::Format_Invalid, ctx->getFormat(texShadowMap), 1,
      loadShaderModule(ctx, "Chapter11/03_DirectionalShadows/src/shadow.vert"),
      loadShaderModule(ctx, "Chapter11/03_DirectionalShadows/src/shadow.frag"));

   const VKPipeline11 pipelineShadowCubeMap(
      ctx, meshData.streams, ctx->getFormat(texShadowCubeMap[0]), app.getDepthFormat(), 1,
      loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/shadowCubeMap.vert"),
      loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/shadowCubeMap.frag"));

  lvk::Holder<lvk::ShaderModuleHandle> vertOIT       = loadShaderModule(ctx, "data/shaders/QuadFlip.vert");
  lvk::Holder<lvk::ShaderModuleHandle> fragOIT       = loadShaderModule(ctx, "Chapter11/04_OIT/src/oit.frag");
  lvk::Holder<lvk::RenderPipelineHandle> pipelineOIT = ctx->createRenderPipeline({
      .smVert = vertOIT,
      .smFrag = fragOIT,
      .color  = { { .format = kOffscreenFormat } },
  });

  lvk::Holder<lvk::ShaderModuleHandle> compBrightPass        = loadShaderModule(ctx, "Chapter10/05_HDR/src/BrightPass.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineBrightPass = ctx->createComputePipeline({ .smComp = compBrightPass });

  lvk::Holder<lvk::ShaderModuleHandle> compAdaptationPass        = loadShaderModule(ctx, "Chapter10/06_HDR_Adaptation/src/Adaptation.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineAdaptationPass = ctx->createComputePipeline({ .smComp = compAdaptationPass });

  const uint32_t kHorizontal = 1;
  const uint32_t kVertical   = 0;

  lvk::Holder<lvk::ShaderModuleHandle> compBloomPass     = loadShaderModule(ctx, "Chapter10/05_HDR/src/Bloom.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineBloomX = ctx->createComputePipeline({
      .smComp   = compBloomPass,
      .specInfo = {.entries = { { .constantId = 0, .size = sizeof(uint32_t) } }, .data = &kHorizontal, .dataSize = sizeof(uint32_t)},
  });
  lvk::Holder<lvk::ComputePipelineHandle> pipelineBloomY = ctx->createComputePipeline({
      .smComp   = compBloomPass,
      .specInfo = {.entries = { { .constantId = 0, .size = sizeof(uint32_t) } }, .data = &kVertical, .dataSize = sizeof(uint32_t)},
  });

  lvk::Holder<lvk::ShaderModuleHandle> vertToneMap = loadShaderModule(ctx, "data/shaders/QuadFlip.vert");
  lvk::Holder<lvk::ShaderModuleHandle> fragToneMap = loadShaderModule(ctx, "Chapter10/05_HDR/src/ToneMap.frag");

  lvk::Holder<lvk::RenderPipelineHandle> pipelineToneMap = ctx->createRenderPipeline({
      .smVert = vertToneMap,
      .smFrag = fragToneMap,
      .color  = { { .format = ctx->getSwapchainFormat() } },
  });

  lvk::Holder<lvk::ShaderModuleHandle> compSSAO        = loadShaderModule(ctx, "Chapter10/04_SSAO/src/SSAO.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineSSAO = ctx->createComputePipeline({
      .smComp = compSSAO,
  });

  // SSAO
  lvk::Holder<lvk::TextureHandle> texRotations = loadTexture(ctx, "data/rot_texture.bmp");

  lvk::Holder<lvk::ShaderModuleHandle> compBlur         = loadShaderModule(ctx, "data/shaders/Blur.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineBlurX = ctx->createComputePipeline({
      .smComp   = compBlur,
      .specInfo = {.entries = { { .constantId = 0, .size = sizeof(uint32_t) } }, .data = &kHorizontal, .dataSize = sizeof(uint32_t)},
  });
  lvk::Holder<lvk::ComputePipelineHandle> pipelineBlurY = ctx->createComputePipeline({
      .smComp   = compBlur,
      .specInfo = {.entries = { { .constantId = 0, .size = sizeof(uint32_t) } }, .data = &kVertical, .dataSize = sizeof(uint32_t)},
  });

  lvk::Holder<lvk::ShaderModuleHandle> vertCombine       = loadShaderModule(ctx, "data/shaders/QuadFlip.vert");
  lvk::Holder<lvk::ShaderModuleHandle> fragCombine       = loadShaderModule(ctx, "Chapter10/04_SSAO/src/combine.frag");
  lvk::Holder<lvk::RenderPipelineHandle> pipelineCombineSSAO = ctx->createRenderPipeline({
      .smVert = vertCombine,
      .smFrag = fragCombine,
      .color  = { { .format = kOffscreenFormat } },
  });

  // camera frustum culling pipeline
  lvk::Holder<lvk::ShaderModuleHandle> compCulling        = loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/FrustumCulling.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineCulling = ctx->createComputePipeline({
      .smComp = compCulling,
  });

  // tile computing pass
  lvk::Holder<lvk::ShaderModuleHandle> compTile        = loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/tile.comp");
  lvk::Holder<lvk::ComputePipelineHandle> pipelineTile = ctx->createComputePipeline({
       .smComp = compTile,
  });

  // point light marker
  const lvk::VertexInput vdesc = {
      .attributes    = {{ .location = 0, .format = lvk::VertexFormat::Float3, .offset = 0 },
 },
      .inputBindings = { { .stride = 3 * sizeof(float) } },
    };
  lvk::Holder<lvk::ShaderModuleHandle> vertPointLightMarker       = loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/pointLightMarker.vert");
  lvk::Holder<lvk::ShaderModuleHandle> fragPointLightMarker       = loadShaderModule(ctx, "Chapter11/07_MyFinalDemo/src/pointLightMarker.frag");
  
  lvk::Holder<lvk::RenderPipelineHandle> pipelinePointLightMarker = ctx->createRenderPipeline({
	   .vertexInput = vdesc,
	   .smVert = vertPointLightMarker,
      .smFrag = fragPointLightMarker,
      .color       = { { .format = kOffscreenFormat } },
      .depthFormat = app.getDepthFormat(),
      .cullMode         = lvk::CullMode_None,
      .samplesCount     = kNumSamples,
      .minSampleShading = kNumSamples > 1 ? 0.25f : 0.0f,
  });
  
  vec3 markerVertices[8] = {
    { -1, -1, -1 },
    {  1, -1, -1 },
    {  1,  1, -1 },
    { -1,  1, -1 },
    { -1, -1,  1 },
    {  1, -1,  1 },
    {  1,  1,  1 },
    { -1,  1,  1 }
  };

  // Cube marker indices (12 triangles)
  uint32_t markerIndices[36] = {
    0, 1, 2, 2, 3, 0, // bottom
    4, 5, 6, 6, 7, 4, // top
    0, 1, 5, 5, 4, 0, // front
    2, 3, 7, 7, 6, 2, // back
    1, 2, 6, 6, 5, 1, // right
    3, 0, 4, 4, 7, 3  // left
  };

   lvk::Holder<lvk::BufferHandle> bufferIndicesPointLightMarker = ctx->createBuffer(
   { .usage     = lvk::BufferUsageBits_Index,
     .storage   = lvk::StorageType_Device,
     .size      = sizeof(uint32_t) * 36,
     .data      = markerIndices,
     .debugName = "Buffer: indices" },
      nullptr);

  lvk::Holder<lvk::BufferHandle> bufferVerticesPointLightMarker = ctx->createBuffer(
      { .usage     = lvk::BufferUsageBits_Vertex,
        .storage   = lvk::StorageType_Device,
        .size      = 8 * 3 *sizeof(float),
        .data      = markerVertices,
        .debugName = "Buffer: vertices" },
      nullptr);

  //mat4 modelMartix = glm::mat4(1.0f);                                            // Identity
  mat4 markerMatrices[pointLightsNum] = {};

  //mat4 markerMatricesPrevious[pointLightsNum] = {};

  for (int i = 0; i < pointLightsNum; i++) {
    markerMatrices[i] = glm::translate(mat4(1.0f), vec3(pointLightBlock.pointLightData[i].lightPos)); // Move to light position
    markerMatrices[i] = glm::scale(markerMatrices[i], vec3(0.05, 0.05, 0.05));
   // markerMatricesPrevious[i] = glm::translate(mat4(1.0f), vec3(pointLightData[i].lightPos)); // Move to light position
   // markerMatricesPrevious[i] = glm::scale(markerMatricesPrevious[i], vec3(0.05, 0.05, 0.05));
  }

    lvk::Holder<lvk::BufferHandle> bufferPointLightMarkerMatrices = ctx->createBuffer(
		 {
      .usage   = lvk::BufferUsageBits_Storage,
      .storage = lvk::StorageType_Device,
      .size    = sizeof(mat4) * pointLightsNum,
      .data      = markerMatrices,
      .debugName = "Buffer: marker matrices",
  });


  app.addKeyCallback([](GLFWwindow* window, int key, int scancode, int action, int mods) {
    const bool pressed = action != GLFW_RELEASE;
    if (!pressed || ImGui::GetIO().WantCaptureKeyboard)
      return;
    if (key == GLFW_KEY_P)
      freezeCullingView = !freezeCullingView;
    if (key == GLFW_KEY_N)
      cullingMode = CullingMode_None;
    if (key == GLFW_KEY_C)
      cullingMode = CullingMode_CPU;
    if (key == GLFW_KEY_G)
      cullingMode = CullingMode_GPU;
  });

  // pretransform bounding boxes to world space
  std::vector<BoundingBox> reorderedBoxes;
  reorderedBoxes.resize(scene.globalTransform.size());
  for (auto& p : scene.meshForNode) {
    reorderedBoxes[p.first] = meshData.boxes[p.second].getTransformed(scene.globalTransform[p.first]);
  }

  lvk::Holder<lvk::BufferHandle> bufferAABBs = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = reorderedBoxes.size() * sizeof(BoundingBox),
      .data      = reorderedBoxes.data(),
      .debugName = "Buffer: AABBs",
  });

  // create the scene AABB in world space
  BoundingBox bigBoxWS = reorderedBoxes.front();
  for (const auto& b : reorderedBoxes) {
    bigBoxWS.combinePoint(b.min_);
    bigBoxWS.combinePoint(b.max_);
  }

  struct CullingData {
    vec4 frustumPlanes[6];
    vec4 frustumCorners[8];
    uint32_t numMeshesToCull  = 0;
    uint32_t numVisibleMeshes = 0; // GPU
  } emptyCullingData;

  int numVisibleMeshes = 0; // CPU

  // round-robin
  const lvk::BufferDesc cullingDataDesc = {
    .usage     = lvk::BufferUsageBits_Storage,
    .storage   = lvk::StorageType_HostVisible,
    .size      = sizeof(CullingData),
    .data      = &emptyCullingData,
    .debugName = "Buffer: CullingData 0",
  };
  lvk::Holder<lvk::BufferHandle> bufferCullingData[] = {
    ctx->createBuffer(cullingDataDesc, "Buffer: CullingData 0"),
    ctx->createBuffer(cullingDataDesc, "Buffer: CullingData 1"),
  };
  lvk::SubmitHandle submitHandle[LVK_ARRAY_NUM_ELEMENTS(bufferCullingData)] = {};

  uint32_t currentBufferId = 0; // for culling stats

  struct {
    uint64_t commands;
    uint64_t drawData;
    uint64_t AABBs;
    uint64_t meshes;
    uint64_t compactedCommands;
  } pcCulling = {
    .commands = 0,
    .drawData = ctx->gpuAddress(mesh.bufferDrawData_),
    .AABBs    = ctx->gpuAddress(bufferAABBs),
  };

  // filtered indirect buffers (only for opaque draw commands or transparent draw commands)
  VKIndirectBuffer11 meshesOpaque(ctx, mesh.numMeshes_, lvk::StorageType_HostVisible);
  VKIndirectBuffer11 meshesTransparent(ctx, mesh.numMeshes_, lvk::StorageType_HostVisible);

  VKIndirectBuffer11 meshesOpaqueNotCulled(ctx, mesh.numMeshes_, lvk::StorageType_HostVisible);

  // round-robin buffers for indirect command buffers for drawing opaque objects (CPU camera culling and compacted buffer)
  VKIndirectBuffer11 meshesOpaqueArray[2] = { VKIndirectBuffer11(ctx, mesh.numMeshes_, lvk::StorageType_HostVisible),
                                         VKIndirectBuffer11(ctx, mesh.numMeshes_, lvk::StorageType_HostVisible) };

  // GPU compacted indirect command buffer for drawing opaque obejcts (GPU camera culling)
  VKIndirectBuffer11 meshesOpaqueGPU(ctx, mesh.numMeshes_, lvk::StorageType_HostVisible);

  auto isTransparent = [&meshData, &mesh](const DrawIndexedIndirectCommand& c) -> bool {
    const uint32_t mtlIndex = mesh.drawData_[c.baseInstance].materialId;
    const Material& mtl     = meshData.materials[mtlIndex];
    return (mtl.flags & sMaterialFlags_Transparent) > 0;
  };

  // filter the indirect buffers
  mesh.indirectBuffer_.selectTo(meshesOpaque, [&isTransparent](const DrawIndexedIndirectCommand& c) -> bool { return !isTransparent(c); });
  mesh.indirectBuffer_.selectTo(
      meshesTransparent, [&isTransparent](const DrawIndexedIndirectCommand& c) -> bool { return isTransparent(c); });

  mesh.indirectBuffer_.selectTo(meshesOpaqueNotCulled, [&isTransparent](const DrawIndexedIndirectCommand& c) -> bool { return !isTransparent(c); });

  mesh.indirectBuffer_.selectTo(meshesOpaqueArray[0], [&isTransparent](const DrawIndexedIndirectCommand& c) -> bool { return !isTransparent(c); });
  mesh.indirectBuffer_.selectTo(meshesOpaqueArray[1], [&isTransparent](const DrawIndexedIndirectCommand& c) -> bool { return !isTransparent(c); });

  //mesh.indirectBuffer_.selectTo(meshesOpaqueGPU, [&isTransparent](const DrawIndexedIndirectCommand& c) -> bool { return !isTransparent(c); });

  std::vector<DrawIndexedIndirectCommand> fullDrawCommands = meshesOpaque.drawCommands_;

  struct TransparentFragment {
    uint64_t rgba; // f16vec4
    float depth;
    uint32_t next;
  };

  const uint32_t kMaxOITFragments = sizeFb.width * sizeFb.height * kNumSamples;

  lvk::Holder<lvk::BufferHandle> bufferAtomicCounter = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(uint32_t),
      .debugName = "Buffer: atomic counter",
  });

  lvk::Holder<lvk::BufferHandle> bufferListsOIT = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(TransparentFragment) * kMaxOITFragments,
      .debugName = "Buffer: transparency lists",
  });

  lvk::Holder<lvk::TextureHandle> texHeadsOIT = ctx->createTexture({
      .format     = lvk::Format_R_UI32,
      .dimensions = sizeFb,
      .usage      = lvk::TextureUsageBits_Storage,
      .debugName  = "oitHeads",
  });

  const struct OITBuffer {
    uint64_t bufferAtomicCounter;
    uint64_t bufferTransparencyLists;
    uint32_t texHeadsOIT;
    uint32_t maxOITFragments;
  } oitBufferData = {
    .bufferAtomicCounter     = ctx->gpuAddress(bufferAtomicCounter),
    .bufferTransparencyLists = ctx->gpuAddress(bufferListsOIT),
    .texHeadsOIT             = texHeadsOIT.index(),
    .maxOITFragments         = kMaxOITFragments,
  };

  lvk::Holder<lvk::BufferHandle> bufferOIT = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(oitBufferData),
      .data      = &oitBufferData,
      .debugName = "Buffer: OIT",
  });


  // since the maximum size of push constants (in rendering scene pass) cannot hold all the buffer address
  // so here create an address table to hold some of the buffer addresses (not frequently accessed in the shader)
  // we cannot put all buffer addresses here since double pointer chasing will cause significant frame rate dropping
  // transform and drawdata buffer are proper to be put in the table (for double pointer access)
 const struct AddressTable {
    uint64_t bufferTransforms;
    uint64_t bufferDrawData;
    //uint64_t bufferMaterials;
    //uint32_t texSkybox;
    //uint32_t texSkyboxIrradiance;
   
 } addressTable = {
    .bufferTransforms    = ctx->gpuAddress(mesh.bufferTransforms_),
    .bufferDrawData      = ctx->gpuAddress(mesh.bufferDrawData_),
    //.bufferMaterials     = ctx->gpuAddress(mesh.bufferMaterials_),
    //.texSkybox           = skyBox.texSkybox.index(),
   // .texSkyboxIrradiance = skyBox.texSkyboxIrradiance.index(),
  };

   lvk::Holder<lvk::BufferHandle> bufferAddressTable = ctx->createBuffer({
      .usage     = lvk::BufferUsageBits_Storage,
      .storage   = lvk::StorageType_Device,
      .size      = sizeof(AddressTable),
      .data      = &addressTable,
      .debugName = "Buffer: AddressTable",
  });
  


  // update shadow map
  LightParams prevLight = { .depthBiasConst = 0 };

  // clang-format off
  const mat4 scaleBias = mat4(0.5, 0.0, 0.0, 0.0,
                              0.0, 0.5, 0.0, 0.0,
                              0.0, 0.0, 1.0, 0.0,
                              0.5, 0.5, 0.0, 1.0);
  // clang-format on

  auto clearTransparencyBuffers = [&bufferAtomicCounter, &texHeadsOIT, sizeFb](lvk::ICommandBuffer& buf) {
    buf.cmdClearColorImage(texHeadsOIT, { .uint32 = { 0xffffffff } });
    buf.cmdFillBuffer(bufferAtomicCounter, 0, sizeof(uint32_t), 0);
  };

  struct {
    uint32_t texDepth;
    uint32_t texRotation;
    uint32_t texOut;
    uint32_t sampler;
    float zNear;
    float zFar;
    float radius;
    float attScale;
    float distScale;
  } pcSSAO = {
    .texDepth    = texOpaqueDepth.index(),
    .texRotation = texRotations.index(),
    .texOut      = texSSAO.index(),
    .sampler     = samplerClamp.index(),
    .zNear       = 0.01f,
    .zFar        = 200.0f,
    .radius      = 0.01f,
    .attScale    = 0.95f,
    .distScale   = 1.7f,
  };

  struct {
    uint32_t texColor;
    uint32_t texSSAO;
    uint32_t sampler;
    float scale;
    float bias;
  } pcCombineSSAO = {
    .texColor = texOpaqueColor.index(),
    .texSSAO  = texSSAO.index(),
    .sampler  = samplerClamp.index(),
    .scale    = 1.1f,
    .bias     = 0.1f,
  };

  struct {
    uint32_t texColor;
    uint32_t texLuminance;
    uint32_t texBloom;
    uint32_t sampler;
    int drawMode = ToneMapping_Uchimura;

    float exposure      = 0.95f;
    float bloomStrength = 0.0f;

    // Reinhard
    float maxWhite = 1.0f;

    // Uchimura
    float P = 1.0f;  // max display brightness
    float a = 1.05f; // contrast
    float m = 0.1f;  // linear section start
    float l = 0.8f;  // linear section length
    float c = 3.0f;  // black tightness
    float b = 0.0f;  // pedestal

    // Khronos PBR
    float startCompression = 0.8f;  // highlight compression start
    float desaturation     = 0.15f; // desaturation speed
  } pcHDR = {
    .texColor     = texSceneColor.index(),
    .texLuminance = texAdaptedLum[0].index(), // 1x1
    .texBloom     = texBloomPass.index(),
    .sampler      = samplerClamp.index(),
  };

  app.run([&](uint32_t width, uint32_t height, float aspectRatio, float deltaSeconds) {

    // loading texture asynchronously
	 mesh.processLoadedTextures();

    const mat4 view = app.camera_.getViewMatrix();
    const mat4 proj = glm::perspective(45.0f, aspectRatio, pcSSAO.zNear, pcSSAO.zFar);

    // prepare culling data
    if (!freezeCullingView)
      cullingView = app.camera_.getViewMatrix();

    CullingData cullingData = {
      .numMeshesToCull = static_cast<uint32_t>(meshesOpaque.drawCommands_.size()),
    };

	 // extract viewing frustum planes and corners
    getFrustumPlanes(proj * cullingView, cullingData.frustumPlanes);
    getFrustumCorners(proj * cullingView, cullingData.frustumCorners);

    // directional light
    const glm::mat4 rot1 = glm::rotate(mat4(1.f), glm::radians(light.theta), glm::vec3(0, 1, 0));
    const glm::mat4 rot2 = glm::rotate(rot1, glm::radians(light.phi), glm::vec3(1, 0, 0));
    const vec3 lightDir  = glm::normalize(vec3(rot2 * vec4(0.0f, -1.0f, 0.0f, 1.0f)));
    const mat4 lightView = glm::lookAt(glm::vec3(0.0f), lightDir, vec3(0, 0, 1));

    // transform scene AABB to light space
    const BoundingBox boxLS = bigBoxWS.getTransformed(lightView);
    const mat4 lightProj    = glm::orthoLH_ZO(boxLS.min_.x, boxLS.max_.x, boxLS.min_.y, boxLS.max_.y, boxLS.max_.z, boxLS.min_.z);

	 // point light
    //const vec3 pointLightPos = vec3(pointLightBlock.pointLightData[0].lightPos);
	 // each cube map face has its own light view matrix
	 mat4 pointLightViews[2][6];
    for (uint8_t i = 0; i < 2; i++) {
      const vec3 pointLightPos = vec3(pointLightBlock.pointLightData[i].lightPos);
      pointLightViews[i][0] = glm::lookAt(pointLightPos, pointLightPos + vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));   // +X face
      pointLightViews[i][1] = glm::lookAt(pointLightPos, pointLightPos + vec3(-1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));  // -X face
      pointLightViews[i][2] = glm::lookAt(pointLightPos, pointLightPos + vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f));   // +Y face
      pointLightViews[i][3] = glm::lookAt(pointLightPos, pointLightPos + vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f)); // -Y face
      // the +Z and -Z faces order are swapped since the cubemap is left-handed coordinate system, but Vulkan and direction vector uvw we
      // provide is right so we need to flip the Z axis in uvw sample vector and also swap the order of Z faces
      pointLightViews[i][4] = glm::lookAt(pointLightPos, pointLightPos + vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f)); // -Z face
      pointLightViews[i][5] = glm::lookAt(pointLightPos, pointLightPos + vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 1.0f, 0.0f));  // +Z face
    }

	 // the far plane should be the radius of point light
    const mat4 pointLightProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, pointLightBlock.pointLightData[0].radius);



	 // store the bool values of whether the object is culled or not
	 // for drawing the boundary box when we use the compacted command buffer way
    // here we cannot use the instance count way to judge whether the object is culled or not
	 // so we store the bool value in a vectorw
	 std::vector<bool> ifCulling;

    lvk::ICommandBuffer& buf = ctx->acquireCommandBuffer();
    {
		// clear the OIT buffers 
      clearTransparencyBuffers(buf);

		if (frameCount % 3 == 0 || cullingEveryFrame) {
        // cull scene (we only cull opaque meshes)
        // because we only cull opaque meshes, only the meshesOpaque indirect buffer has been culled (modified)
        // not culling mode
        if (cullingMode == CullingMode_None) {
          numVisibleMeshes                = static_cast<uint32_t>(scene.meshForNode.size()); // all meshes
          DrawIndexedIndirectCommand* cmd = meshesOpaque.getDrawIndexedIndirectCommandPtr();
          for (auto& c : meshesOpaque.drawCommands_) {
            (cmd++)->instanceCount = 1;
          }
          ctx->flushMappedMemory(meshesOpaque.bufferIndirect_, 0, meshesOpaque.drawCommands_.size() * sizeof(DrawIndexedIndirectCommand));
        }
        // CPU culling mode
        else if (cullingMode == CullingMode_CPU) {


			  if (compactedBuffer) { // if we use the compacted command buffer way instead of setting the instance count to be 0
            std::vector<DrawIndexedIndirectCommand> compactedDrawCommands;
            numVisibleMeshes =
                static_cast<uint32_t>(meshesTransparent.drawCommands_.size()); // all transparent meshes are visible - we don't cull them

				ifCulling.resize(fullDrawCommands.size());
            // get the CPU mapped pointer of the GPU indirect buffer (host visible), and update the data on CPU
           // DrawIndexedIndirectCommand* cmd = meshesOpaque.getDrawIndexedIndirectCommandPtr();
            DrawIndexedIndirectCommand* cmd = fullDrawCommands.data();
            for (size_t i = 0; i != fullDrawCommands.size(); i++) {
              const BoundingBox box = reorderedBoxes[mesh.drawData_[cmd->baseInstance].transformId];

              ifCulling[i] = true;
              if (isBoxInFrustum(cullingData.frustumPlanes, cullingData.frustumCorners, box)) {
                compactedDrawCommands.push_back(*cmd);
                numVisibleMeshes++;
                ifCulling[i] = false;
              }
              cmd++;
              
            }
            meshesOpaqueArray[currentBufferId].drawCommands_ = std::move(compactedDrawCommands);
            
            meshesOpaqueArray[currentBufferId].uploadIndirectBuffer();
          
            // flush memory here is not needed since it has already been done in the uploadIndirectBuffer function
          }
		  
            
			 else {
          
            numVisibleMeshes = static_cast<uint32_t>(meshesTransparent.drawCommands_.size()); // all transparent meshes are visible - we don't cull them

            // get the CPU mapped pointer of the GPU indirect buffer (host visible), and update the data on CPU
            DrawIndexedIndirectCommand* cmd = meshesOpaque.getDrawIndexedIndirectCommandPtr();
            for (size_t i = 0; i != meshesOpaque.drawCommands_.size(); i++) {
              const BoundingBox box = reorderedBoxes[mesh.drawData_[cmd->baseInstance].transformId];
               const uint32_t count   = isBoxInFrustum(cullingData.frustumPlanes, cullingData.frustumCorners, box) ? 1 : 0;
               (cmd++)->instanceCount = count;
                numVisibleMeshes += count;
             
            }
            
            // we need to flush the mapped memory to notify GPU that the indirect buffer data on CPU has been updated
            ctx->flushMappedMemory(meshesOpaque.bufferIndirect_, 0, meshesOpaque.drawCommands_.size() * sizeof(DrawIndexedIndirectCommand));

			  }
			  
			  }
        // GPU culling mode
        else if (cullingMode == CullingMode_GPU) {
          buf.cmdBindComputePipeline(pipelineCulling);
          pcCulling.meshes   = ctx->gpuAddress(bufferCullingData[currentBufferId]);
          pcCulling.commands = ctx->gpuAddress(meshesOpaque.bufferIndirect_);
          pcCulling.compactedCommands = ctx->gpuAddress(meshesOpaqueGPU.bufferIndirect_);

		    // set the numVisibleMeshes to be 0 since it'll be the index for indirect commands on GPU
          cullingData.numVisibleMeshes = 0;
			 buf.cmdPushConstants(pcCulling);
          // cullingData buffer uses round robin buffers
          buf.cmdUpdateBuffer(bufferCullingData[currentBufferId], cullingData);
          buf.cmdDispatchThreadGroups(
              {
                  1 + cullingData.numMeshesToCull / 64
          },
              { .buffers = { lvk::BufferHandle(meshesOpaque.bufferIndirect_), lvk::BufferHandle(meshesOpaqueGPU.bufferIndirect_) } });
        }
      }

		
		frameCount++;

      // 0-1. Update 2D shadow map for directional light
		// the shadow map is not be culled since we don't use the meshesOpaque indirect buffer when drawing the mesh
		// we use the default indirect buffer
      if (prevLight != light) {
        prevLight = light;
        buf.cmdBeginRendering(
            lvk::RenderPass{
                .depth = {.loadOp = lvk::LoadOp_Clear, .clearDepth = 1.0f}
        },
            lvk::Framebuffer{ .depthStencil = { .texture = texShadowMap } });
        buf.cmdPushDebugGroupLabel("Shadow map", 0xff0000ff);
        buf.cmdSetDepthBias(light.depthBiasConst, light.depthBiasSlope);
        buf.cmdSetDepthBiasEnable(true);
       // mesh.draw(buf, pipelineShadow, lightView, lightProj); // render the shadow map for both opaque and transparent objects
       // mesh.draw(buf, pipelineShadow, lightView, lightProj, {}, false, &meshesOpaque); // wrong way, since meshOpaque has been culled through camera frustum, and cannot be used for shadow map rendering (from light frustum)
        mesh.draw(buf, pipelineShadow, lightView, lightProj, {}, false, &meshesOpaqueNotCulled); // only render shadow map for opaque objects (not for transparent objects)
        buf.cmdSetDepthBiasEnable(false);
        buf.cmdPopDebugGroupLabel();
        buf.cmdEndRendering();
		  // the bufferLight data will be used in the scene rendering, but not used in shadow mapping
		  // so update it after the shadow map is rendered
        buf.cmdUpdateBuffer(
            bufferLight, LightData{
                             .viewProjBias  = scaleBias * lightProj * lightView,
                             .lightDir      = vec4(lightDir, 0.0f),
                             .shadowTexture = texShadowMap.index(),
                             .shadowSampler = samplerShadow.index(),

                         });

		  

								 
      }
      //buf.cmdUpdateBuffer( bufferPointLight, pointLightBlock);

		// 0-2. Update shadow cube map for point lights
		// should only update the cubemap when the point light data is changed
		if (pointLightChanged) {

			// there are two point lights enabled shadows
			for (uint8_t j = 0; j < 2; j++) { 
          const lvk::Framebuffer cubeMapFrameBuffer = { .color        = { { .texture = texShadowCubeMap[j] } },
                                                        .depthStencil = { .texture = texDepthShadowPass } };

          // for each point light with shadows enabled
			 // loop six times, each time for rendering one specific face of the shadow cubemap
          for (uint8_t i = 0; i < 6; i++) {
            // i = 3;
            /*
            buf.cmdBeginRendering(
                lvk::RenderPass{ // set the layer to be i, which means the ith face of the cubemap
                    .depth = { .loadOp = lvk::LoadOp_Clear, .layer = i, .clearDepth = 1.0f }
            }, cubeMapFrameBuffer);
          */

            // we write the linear distance from the lightPos to the fragment into the cubemap faces as the color attachment
            // instead of just using hardware depth values and set cubemap faces as depth attachment
            buf.cmdBeginRendering(
                lvk::RenderPass{
                    // set the layer to be i, which means the ith face of the cubemap
                    .color = { { .loadOp     = lvk::LoadOp_Clear,
                                 .storeOp    = lvk::StoreOp_Store,
                                 .layer      = i,
                                 .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f } } },
                    .depth = { .loadOp = lvk::LoadOp_Clear, .clearDepth = 1.0f }
            },
                cubeMapFrameBuffer);

            buf.cmdPushDebugGroupLabel("Shadow map", 0xff0000ff);
            //  buf.cmdSetDepthBias(light.depthBiasConst, light.depthBiasSlope);
            // buf.cmdSetDepthBiasEnable(true);

            const struct {
              mat4 viewProj;
              uint64_t bufferTransforms;
              uint64_t bufferDrawData;
              uint64_t bufferMaterials;
              uint32_t padding0;
              uint32_t cubemapIndex;
              vec4     lightPos[2]; // each shadowed point light has one position
            } shadowPassPC = { .viewProj         = pointLightProj * pointLightViews[j][i],
                               .bufferTransforms = ctx->gpuAddress(mesh.bufferTransforms_),
                               .bufferDrawData   = ctx->gpuAddress(mesh.bufferDrawData_),
                               .bufferMaterials  = ctx->gpuAddress(mesh.bufferMaterials_),
                               .padding0         = 0,
					                .cubemapIndex     = j,
                               .lightPos         = { pointLightBlock.pointLightData[0].lightPos, pointLightBlock.pointLightData[1].lightPos },

				};

            // mesh.draw( // set the correct view matrix for each cube map face
            //   buf, pipelineShadowCubeMap, pointLightViews[i], pointLightProj, {}, false,
            //   &meshesOpaqueNotCulled); // only render shadow map for opaque objects (not for transparent objects)

            mesh.draw( // set the correct view matrix for each cube map face
                buf, pipelineShadowCubeMap, &shadowPassPC, sizeof(shadowPassPC),
                { .compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true }, false,
                &meshesOpaqueNotCulled); // only render shadow map for opaque objects (not for transparent objects)

            // buf.cmdSetDepthBiasEnable(false);
            buf.cmdPopDebugGroupLabel();
            buf.cmdEndRendering();
          }
        }
      }
		
      // 1. Render scene
		// using MSAA textures as render target and resolve it
      const lvk::Framebuffer framebufferMSAA = {
        .color        = { { .texture = msaaColor, .resolveTexture = texOpaqueColor } },
        .depthStencil = { .texture = msaaDepth, .resolveTexture = texOpaqueDepth },
      };
      buf.cmdBeginRendering(
          lvk::RenderPass{
              .color = { { .loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_MsaaResolve, .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f } } },
              .depth = { .loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_MsaaResolve, .clearDepth = 1.0f }
      },
          framebufferMSAA,
          { .buffers = { lvk::BufferHandle(meshesOpaque.bufferIndirect_), lvk::BufferHandle(meshesOpaqueGPU.bufferIndirect_) } });
      skyBox.draw(buf, view, proj);

		// push constants cannot hold all buffer addresses due to size limit
		// so we put an address table buffer in the push constant, holding some of the buffer addresses
      const struct {
        mat4 viewProj;
        vec4 cameraPos;
       // uint64_t bufferTransforms;
       // uint64_t bufferDrawData;
        uint64_t bufferMaterials;
        uint64_t bufferOIT;
        uint64_t bufferLight;
        uint64_t bufferPointLight; // added point light data buffer
        uint64_t bufferAddressTable;
		  uint32_t texSkybox;
        uint32_t texSkyboxIrradiance;
      } pc = {
        .viewProj            = proj * view,
        .cameraPos           = vec4(app.camera_.getPosition(), 1.0f),
        //.bufferTransforms    = ctx->gpuAddress(mesh.bufferTransforms_),
       // .bufferDrawData      = ctx->gpuAddress(mesh.bufferDrawData_),
        .bufferMaterials     = ctx->gpuAddress(mesh.bufferMaterials_),
        .bufferOIT           = ctx->gpuAddress(bufferOIT),
        .bufferLight         = ctx->gpuAddress(bufferLight),
        .bufferPointLight    = ctx->gpuAddress(bufferPointLight), // added point light data buffer's gpu address
        .bufferAddressTable  = ctx->gpuAddress(bufferAddressTable),
		  .texSkybox           = skyBox.texSkybox.index(),
        .texSkyboxIrradiance = skyBox.texSkyboxIrradiance.index(),
      };
		
      /*
		 const struct {
        mat4 viewProj;
        vec4 cameraPos;
        uint64_t renderingPreFrameData;
      } pc = {
        .viewProj            = proj * view,
        .cameraPos           = vec4(app.camera_.getPosition(), 1.0f),
        .renderingPreFrameData = ctx->gpuAddress(bufferRenderingPreFrameData)
      };
		*/
		// draw the opaque meshes (using the filtered indirect buffer meshesOpaque)
      // meshOpaque has been processed by CPU or GPU camera culling
      if (drawMeshesOpaque) {
        buf.cmdPushDebugGroupLabel("Mesh opaque", 0xff0000ff);

		  // if CPU culling and compacted buffer are used
		  if (cullingMode == CullingMode_CPU && compactedBuffer)
		  mesh.draw(
            buf, pipelineOpaque, &pc, sizeof(pc), { .compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true }, drawWireframe,
           // &meshesOpaque);
        &meshesOpaqueArray[currentBufferId]);

		  // if GPU culling is used (default to be compacted buffer for GPU)
		  else if (cullingMode == CullingMode_GPU) {
          mesh.draw(
              buf, pipelineOpaque, &pc, sizeof(pc), { .compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true }, drawWireframe,
              &meshesOpaqueGPU);
		  }
		  // if CPU culling is used but compacted buffer is not used
		  else
          mesh.draw(buf, pipelineOpaque, &pc, sizeof(pc), { .compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true }, drawWireframe, &meshesOpaque);
              
        buf.cmdPopDebugGroupLabel();
      }


  // draw point light markers (can be toggled off through UI)
	if (drawPointLightMarker) {
    
        const struct {
          mat4 viewproj;
          // mat4 model;
          uint64_t bufferMatrices;

        } pcPointLightMarker{
          .viewproj       = proj * view,
          .bufferMatrices = ctx->gpuAddress(bufferPointLightMarkerMatrices),

        };

        buf.cmdPushDebugGroupLabel("point light markers", 0xff0000ff);
        buf.cmdBindRenderPipeline(pipelinePointLightMarker);

        buf.cmdBindVertexBuffer(0, bufferVerticesPointLightMarker);
        buf.cmdBindIndexBuffer(bufferIndicesPointLightMarker, lvk::IndexFormat_UI32);
        buf.cmdPushConstants(pcPointLightMarker);
        buf.cmdBindDepthState({ .compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true });
        buf.cmdDrawIndexed(36, pointLightsNum);

        // buf.cmdDraw(36, 1); // kNumCubes is the number of instances
        buf.cmdPopDebugGroupLabel();
      }




		// draw the transparent meshes
      if (drawMeshesTransparent) {
        buf.cmdPushDebugGroupLabel("Mesh transparent", 0xff0000ff);
        mesh.draw(
            buf, pipelineTransparent, &pc, sizeof(pc), { .compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = false }, drawWireframe,
            &meshesTransparent);
        buf.cmdPopDebugGroupLabel();
      }
      app.drawGrid(buf, proj, vec3(0, -1.0f, 0), kNumSamples, kOffscreenFormat);
      canvas3d.clear();
      canvas3d.setMatrix(proj * view);
      if (freezeCullingView)
        canvas3d.frustum(cullingView, proj, vec4(1, 1, 0, 1));
      if (drawLightFrustum)
        canvas3d.frustum(lightView, lightProj, vec4(1, 1, 0, 1));
      // render all bounding boxes
      if (drawBoxes) {
		  // draw transparent boxes (always visible)
        for (auto& c : meshesTransparent.drawCommands_) {
          const uint32_t transformId = mesh.drawData_[c.baseInstance].transformId;
          const uint32_t meshId      = scene.meshForNode[transformId];
          const BoundingBox box      = meshData.boxes[meshId];
          canvas3d.box(scene.globalTransform[transformId], box, vec4(0, 1, 0, 1));
        }
        // draw opaque boxes
        const DrawIndexedIndirectCommand* cmd = meshesOpaque.getDrawIndexedIndirectCommandPtr();
        int boundingBoxCount                  = 0;
		  for (auto& c : meshesOpaque.drawCommands_) {
          const uint32_t transformId = mesh.drawData_[c.baseInstance].transformId;
          const uint32_t meshId      = scene.meshForNode[transformId];
          const BoundingBox box      = meshData.boxes[meshId];
          // when using the compacted buffer way for CPU culling
			 // we cannot use the instance count way to judge whether the object is culled or not
			 if (cullingMode == CullingMode_CPU && compactedBuffer) { 
            canvas3d.box(scene.globalTransform[transformId], box, ifCulling[boundingBoxCount++] ? vec4(1, 0, 0, 1) : vec4(0, 1, 0, 1));
			 }
			 else {
         canvas3d.box(scene.globalTransform[transformId], box, (cmd++)->instanceCount ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1));
			 }

        }
      }
      canvas3d.render(*ctx.get(), framebufferMSAA, buf, kNumSamples);
      buf.cmdEndRendering();

		// update the buffer after one dynamic rendering (a render pass) is ended
		pointLightChanged = false;
		for (int i = 0; i < pointLightsNum; i++) {
			if (pointLightBlock.pointLightData[i] != pointLightDataPrevious[i]) {
           pointLightChanged = true;
           break;
		  }
		}

		// if at least one of the point light is changed
		// then update the matrices data and buffer
		// we don't update the buffer every frame
		if (pointLightChanged) {
      for (int i = 0; i < pointLightsNum; i++) {
        markerMatrices[i] = glm::translate(mat4(1.0f), vec3(pointLightBlock.pointLightData[i].lightPos));
        markerMatrices[i] = glm::scale(markerMatrices[i], vec3(0.05, 0.05, 0.05));
		}

      buf.cmdUpdateBuffer(bufferPointLightMarkerMatrices, markerMatrices);

		std::copy(std::begin(pointLightBlock.pointLightData), std::end(pointLightBlock.pointLightData), pointLightDataPrevious);
      buf.cmdUpdateBuffer(bufferPointLight, pointLightBlock);
		}

      // 2. Compute SSAO
      if (ssaoEnable) {
        buf.cmdBindComputePipeline(pipelineSSAO);
        buf.cmdPushConstants(pcSSAO);
        // clang-format off
        buf.cmdDispatchThreadGroups(
            { .width  = 1 + (uint32_t)sizeFb.width  / 16,
              .height = 1 + (uint32_t)sizeFb.height / 16 },
            { .textures = { lvk::TextureHandle(texOpaqueDepth),
                            lvk::TextureHandle(texSSAO) } });
		  // clang-format on

        // 3. Blur SSAO
        if (ssaoEnableBlur) {
          const lvk::Dimensions blurDim = {
            .width  = 1 + (uint32_t)sizeFb.width / 16,
            .height = 1 + (uint32_t)sizeFb.height / 16,
          };
          struct BlurPC {
            uint32_t texDepth;
            uint32_t texIn;
            uint32_t texOut;
            float depthThreshold;
          };
          struct BlurPass {
            lvk::TextureHandle texIn;
            lvk::TextureHandle texOut;
          };
          std::vector<BlurPass> passes;
          {
            passes.reserve(2 * ssaoNumBlurPasses);
            passes.push_back({ texSSAO, texBlur[0] });
            for (int i = 0; i != ssaoNumBlurPasses - 1; i++) {
              passes.push_back({ texBlur[0], texBlur[1] });
              passes.push_back({ texBlur[1], texBlur[0] });
            }
            passes.push_back({ texBlur[0], texSSAO });
          }
          for (uint32_t i = 0; i != passes.size(); i++) {
            const BlurPass p = passes[i];
            buf.cmdBindComputePipeline(i & 1 ? pipelineBlurX : pipelineBlurY);
            buf.cmdPushConstants(BlurPC{
                .texDepth       = texOpaqueDepth.index(),
                .texIn          = p.texIn.index(),
                .texOut         = p.texOut.index(),
                .depthThreshold = pcSSAO.zFar * ssaoDepthThreshold,
            });
            // clang-format off
            buf.cmdDispatchThreadGroups(blurDim, { .textures = {p.texIn, p.texOut, lvk::TextureHandle(texOpaqueDepth)} });
				// clang-format on
          }
        }

        // combine SSAO
		  // combine SSAO with opaque scene (SSAO is only applied to opaque objects)
        // clang-format off
        buf.cmdBeginRendering(
            { .color = {{ .loadOp = lvk::LoadOp_Load, .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f } }} },
            { .color = { { .texture = texOpaqueColorWithSSAO } } },
            { .textures = { lvk::TextureHandle(texSSAO), lvk::TextureHandle(texOpaqueColor) } });
        // clang-format on
        buf.cmdBindRenderPipeline(pipelineCombineSSAO);
        buf.cmdPushConstants(pcCombineSSAO);
        buf.cmdBindDepthState({});
        buf.cmdDraw(3);
        buf.cmdEndRendering();
      }

      // combine OIT with the opaque SSAO scene
      const lvk::Framebuffer framebufferOffscreen = {
        .color = { { .texture = texSceneColor } },
      };
      // clang-format off
      buf.cmdBeginRendering(
          lvk::RenderPass{ .color = {{ .loadOp = lvk::LoadOp_Load, .storeOp = lvk::StoreOp_Store }} },
          framebufferOffscreen,
          { .textures = { lvk::TextureHandle(texHeadsOIT), lvk::TextureHandle(texOpaqueColor), lvk::TextureHandle(texOpaqueColorWithSSAO) },
            .buffers  = { lvk::BufferHandle(bufferListsOIT) } });
		// clang-format on
      const struct {
        uint64_t bufferTransparencyLists;
        uint32_t texColor;
        uint32_t texHeadsOIT;
        float time;
        float opacityBoost;
        uint32_t showHeatmap;
      } pcOIT = {
        .bufferTransparencyLists = ctx->gpuAddress(bufferListsOIT),
        .texColor                = (ssaoEnable ? texOpaqueColorWithSSAO : texOpaqueColor).index(),
        .texHeadsOIT             = texHeadsOIT.index(),
        .time                    = static_cast<float>(glfwGetTime()),
        .opacityBoost            = oitOpacityBoost,
        .showHeatmap             = oitShowHeatmap ? 1u : 0u,
      };
      buf.cmdBindRenderPipeline(pipelineOIT);
      buf.cmdPushConstants(pcOIT);
      buf.cmdBindDepthState({});
      buf.cmdDraw(3);
      buf.cmdEndRendering();

		// the tone mapping code starts here
      // 2. Bright pass - extract luminance and bright areas
      const struct {
        uint32_t texColor;
        uint32_t texOut;
        uint32_t texLuminance;
        uint32_t sampler;
        float exposure;
      } pcBrightPass = {
        .texColor     = texSceneColor.index(),
        .texOut       = texBrightPass.index(),
        .texLuminance = texLumViews[0].index(),
        .sampler      = samplerClamp.index(),
        .exposure     = pcHDR.exposure,
      };
      buf.cmdBindComputePipeline(pipelineBrightPass);
      buf.cmdPushConstants(pcBrightPass);
		// clang-format off
      buf.cmdDispatchThreadGroups(sizeBloom.divide2D(16), { .textures = {lvk::TextureHandle(texSceneColor), lvk::TextureHandle(texLumViews[0])} });
		// clang-format on
      buf.cmdGenerateMipmap(texLumViews[0]);

      // 2.1. Bloom
      struct BlurPC {
        uint32_t texIn;
        uint32_t texOut;
        uint32_t sampler;
      };
      struct StreaksPC {
        uint32_t texIn;
        uint32_t texOut;
        uint32_t texRotationPattern;
        uint32_t sampler;
      };
      struct BlurPass {
        lvk::TextureHandle texIn;
        lvk::TextureHandle texOut;
      };
      std::vector<BlurPass> passes;
      {
        passes.reserve(2 * hdrNumBloomPasses);
        passes.push_back({ texBrightPass, texBloom[0] });
        for (int i = 0; i != hdrNumBloomPasses - 1; i++) {
          passes.push_back({ texBloom[0], texBloom[1] });
          passes.push_back({ texBloom[1], texBloom[0] });
        }
        passes.push_back({ texBloom[0], texBloomPass });
      }
      for (uint32_t i = 0; i != passes.size(); i++) {
        const BlurPass p = passes[i];
        buf.cmdBindComputePipeline(i & 1 ? pipelineBloomX : pipelineBloomY);
        buf.cmdPushConstants(BlurPC{
            .texIn   = p.texIn.index(),
            .texOut  = p.texOut.index(),
            .sampler = samplerClamp.index(),
        });
        if (hdrEnableBloom)
          buf.cmdDispatchThreadGroups(
              sizeBloom.divide2D(16), {
                                          .textures = {p.texIn, p.texOut, lvk::TextureHandle(texBrightPass)}
          });
      }

      // 3. Light adaptation pass
      const struct {
        uint32_t texCurrSceneLuminance;
        uint32_t texPrevAdaptedLuminance;
        uint32_t texNewAdaptedLuminance;
        float adaptationSpeed;
      } pcAdaptationPass = {
        .texCurrSceneLuminance   = texLumViews[LVK_ARRAY_NUM_ELEMENTS(texLumViews) - 1].index(), // 1x1,
        .texPrevAdaptedLuminance = texAdaptedLum[0].index(),
        .texNewAdaptedLuminance  = texAdaptedLum[1].index(),
        .adaptationSpeed         = deltaSeconds * hdrAdaptationSpeed,
      };
      buf.cmdBindComputePipeline(pipelineAdaptationPass);
      buf.cmdPushConstants(pcAdaptationPass);
      // clang-format off
      buf.cmdDispatchThreadGroups(
          { 1, 1, 1 },
          { .textures = {
                lvk::TextureHandle(texLumViews[0]), // transition the entire mip-pyramid
                lvk::TextureHandle(texAdaptedLum[0]),
                lvk::TextureHandle(texAdaptedLum[1]),
            } });
		// clang-format on

      // HDR light adaptation: render tone-mapped scene into a swapchain image
      const lvk::RenderPass renderPassMain = {
        .color = { { .loadOp = lvk::LoadOp_Load, .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f } } },
      };
      const lvk::Framebuffer framebufferMain = {
        .color = { { .texture = ctx->getCurrentSwapchainTexture() } },
      };

      // transition the entire mip-pyramid
      buf.cmdBeginRendering(renderPassMain, framebufferMain, { .textures = { lvk::TextureHandle(texAdaptedLum[1]) } });

      buf.cmdBindRenderPipeline(pipelineToneMap);
      buf.cmdPushConstants(pcHDR);
      buf.cmdBindDepthState({});
      buf.cmdDraw(3); // fullscreen triangle

      app.imgui_->beginFrame(framebufferMain);
      app.drawFPS();
      app.drawMemo();

      // render UI
      {
        const ImGuiViewport* v  = ImGui::GetMainViewport();
        const float windowWidth = v->WorkSize.x / 5;
        ImGui::SetNextWindowPos(ImVec2(10, 200));
        ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, v->WorkSize.y - 210));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Checkbox("Draw wireframe", &drawWireframe);
        ImGui::Text("Draw:");
        const float indentSize = 16.0f;
        ImGui::Indent(indentSize);
        ImGui::Checkbox("Opaque meshes", &drawMeshesOpaque);
        ImGui::Checkbox("Transparent meshes", &drawMeshesTransparent);
        ImGui::Checkbox("Bounding boxes", &drawBoxes);
        ImGui::Checkbox("Light frustum", &drawLightFrustum);
        ImGui::Unindent(indentSize);
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Frustum Culling")) {
          ImGui::Indent(indentSize);
          ImGui::RadioButton("None (N)", &cullingMode, CullingMode_None);
          ImGui::RadioButton("CPU  (C)", &cullingMode, CullingMode_CPU);
          ImGui::RadioButton("GPU  (G)", &cullingMode, CullingMode_GPU);
          ImGui::Unindent(indentSize);
          ImGui::Checkbox("Freeze culling frustum (P)", &freezeCullingView);
          ImGui::Checkbox("Using compacted buffer for culling", &compactedBuffer);
			 ImGui::Checkbox("Culling every frame", &cullingEveryFrame);
          ImGui::Separator();
          ImGui::Text("Visible meshes: %i", numVisibleMeshes);
          ImGui::Separator();
        }
        if (ImGui::CollapsingHeader("Order-Independent Transparency")) {
          ImGui::Indent(indentSize);
          ImGui::SliderFloat("Opacity boost", &oitOpacityBoost, -1.0f, +1.0f);
          ImGui::Checkbox("Show transparency heat map", &oitShowHeatmap);
          ImGui::Unindent(indentSize);
          ImGui::Separator();
        }
        if (ImGui::CollapsingHeader("Shadow Mapping")) {
          ImGui::Checkbox("Draw point light markers", &drawPointLightMarker);
          ImGui::Text("Point Light Source:");
          ImGui::Indent(indentSize);
          ImGui::Text("XYZ coordinate:");
          ImGui::SliderFloat3("light0", glm::value_ptr(pointLightBlock.pointLightData[0].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light1", glm::value_ptr(pointLightBlock.pointLightData[1].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light2", glm::value_ptr(pointLightBlock.pointLightData[2].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light3", glm::value_ptr(pointLightBlock.pointLightData[3].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light4", glm::value_ptr(pointLightBlock.pointLightData[4].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light5", glm::value_ptr(pointLightBlock.pointLightData[5].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light6", glm::value_ptr(pointLightBlock.pointLightData[6].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("light7", glm::value_ptr(pointLightBlock.pointLightData[7].lightPos), -7.0f, 10.0f);
          ImGui::SliderFloat3("color", glm::value_ptr(pointLightBlock.pointLightData[0].color), 0.0f, 1.0f);
          ImGui::SliderFloat("radius", &pointLightBlock.pointLightData[0].radius, 0.0f, 10.0f);
          ImGui::SliderFloat("intensity", &pointLightBlock.pointLightData[0].intensity, 0.0f, 1.0f);
			 ImGui::Unindent(indentSize);
          ImGui::Separator();

			 ImGui::Text("Depth bias factor:");
          ImGui::Indent(indentSize);
          ImGui::SliderFloat("Constant", &light.depthBiasConst, 0.0f, 5.0f);
          ImGui::SliderFloat("Slope", &light.depthBiasSlope, 0.0f, 5.0f);
          ImGui::Unindent(indentSize);
          ImGui::Separator();
          ImGui::Text("Light angles:");
          ImGui::Indent(indentSize);
          ImGui::SliderFloat("Theta", &light.theta, -180.0f, +180.0f);
          ImGui::SliderFloat("Phi", &light.phi, -85.0f, +85.0f);
          ImGui::Unindent(indentSize);
          ImGui::Separator();
          ImGui::Text("2D Shadow Map: ");
          ImGui::Image(texShadowMap.index(), ImVec2(512, 512));
          ImGui::Separator();
			 // display all six shadow cubemap faces
			 ImGui::Text("Shadow Cubemap Faces: ");
          for (uint32_t l = 0; l != 6; l++) {
            ImGui::Image(texShadowCubeMapFaces[l].index(), ImVec2(512, 512));
          }
          ImGui::Unindent(indentSize);
          ImGui::Separator();

		  }
        if (ImGui::CollapsingHeader("SSAO")) {
          ImGui::Indent(indentSize);
          ImGui::Checkbox("Enable SSAO", &ssaoEnable);
          ImGui::BeginDisabled(!ssaoEnable);
          ImGui::Checkbox("Enable blur", &ssaoEnableBlur);
          ImGui::BeginDisabled(!ssaoEnableBlur);
          ImGui::SliderFloat("Blur depth threshold", &ssaoDepthThreshold, 0.0f, 50.0f);
          ImGui::SliderInt("Blur num passes", &ssaoNumBlurPasses, 1, 5);
          ImGui::EndDisabled();
          ImGui::SliderFloat("SSAO scale", &pcCombineSSAO.scale, 0.0f, 2.0f);
          ImGui::SliderFloat("SSAO bias", &pcCombineSSAO.bias, 0.0f, 0.3f);
          ImGui::SliderFloat("SSAO radius", &pcSSAO.radius, 0.001f, 0.02f);
          ImGui::SliderFloat("SSAO attenuation scale", &pcSSAO.attScale, 0.5f, 1.5f);
          ImGui::SliderFloat("SSAO distance scale", &pcSSAO.distScale, 0.0f, 2.0f);
          if (ssaoEnable)
            ImGui::Image(texSSAO.index(), ImVec2(windowWidth, windowWidth / aspectRatio));
          ImGui::EndDisabled();
          ImGui::Unindent(indentSize);
          ImGui::Separator();
        }
        if (ImGui::CollapsingHeader("Tone Mapping and HDR")) {
          ImGui::Indent(indentSize);
          ImGui::Checkbox("Draw tone mapping curves", &hdrDrawCurves);
          ImGui::SliderFloat("Exposure", &pcHDR.exposure, 0.1f, 2.0f);
          ImGui::SliderFloat("Adaptation speed", &hdrAdaptationSpeed, 1.0f, 10.0f);
          ImGui::Checkbox("Enable bloom", &hdrEnableBloom);
          pcHDR.bloomStrength = hdrEnableBloom ? hdrBloomStrength : 0.0f;
          ImGui::BeginDisabled(!hdrEnableBloom);
          ImGui::Indent(indentSize);
          ImGui::SliderFloat("Bloom strength", &hdrBloomStrength, 0.0f, 1.0f);
          ImGui::SliderInt("Bloom num passes", &hdrNumBloomPasses, 1, 5);
          ImGui::Unindent(indentSize);
          ImGui::EndDisabled();
          ImGui::Text("Tone mapping mode:");
          ImGui::RadioButton("None", &pcHDR.drawMode, ToneMapping_None);
          ImGui::RadioButton("Reinhard", &pcHDR.drawMode, ToneMapping_Reinhard);
          if (pcHDR.drawMode == ToneMapping_Reinhard) {
            ImGui::Indent(indentSize);
            ImGui::BeginDisabled(pcHDR.drawMode != ToneMapping_Reinhard);
            ImGui::SliderFloat("Max white", &pcHDR.maxWhite, 0.5f, 2.0f);
            ImGui::EndDisabled();
            ImGui::Unindent(indentSize);
          }
          ImGui::RadioButton("Uchimura", &pcHDR.drawMode, ToneMapping_Uchimura);
          if (pcHDR.drawMode == ToneMapping_Uchimura) {
            ImGui::Indent(indentSize);
            ImGui::BeginDisabled(pcHDR.drawMode != ToneMapping_Uchimura);
            ImGui::SliderFloat("Max brightness", &pcHDR.P, 1.0f, 2.0f);
            ImGui::SliderFloat("Contrast", &pcHDR.a, 0.0f, 5.0f);
            ImGui::SliderFloat("Linear section start", &pcHDR.m, 0.0f, 1.0f);
            ImGui::SliderFloat("Linear section length", &pcHDR.l, 0.0f, 1.0f);
            ImGui::SliderFloat("Black tightness", &pcHDR.c, 1.0f, 3.0f);
            ImGui::SliderFloat("Pedestal", &pcHDR.b, 0.0f, 1.0f);
            ImGui::EndDisabled();
            ImGui::Unindent(indentSize);
          }
          ImGui::RadioButton("Khronos PBR Neutral", &pcHDR.drawMode, ToneMapping_KhronosPBR);
          if (pcHDR.drawMode == ToneMapping_KhronosPBR) {
            ImGui::Indent(indentSize);
            ImGui::SliderFloat("Highlight compression start", &pcHDR.startCompression, 0.0f, 1.0f);
            ImGui::SliderFloat("Desaturation speed", &pcHDR.desaturation, 0.0f, 1.0f);
            ImGui::Unindent(indentSize);
          }
          ImGui::Separator();

          ImGui::Text("Average luminance 1x1:");
          ImGui::Image(pcHDR.texLuminance, ImVec2(128, 128));
          ImGui::Separator();
          ImGui::Text("Bright pass:");
          ImGui::Image(texBrightPass.index(), ImVec2(windowWidth, windowWidth / aspectRatio));
          ImGui::Text("Bloom pass:");
          ImGui::Image(texBloomPass.index(), ImVec2(windowWidth, windowWidth / aspectRatio));
          ImGui::Separator();
          ImGui::Text("Luminance pyramid 512x512");
          for (uint32_t l = 0; l != LVK_ARRAY_NUM_ELEMENTS(texLumViews); l++) {
            ImGui::Image(texLumViews[l].index(), ImVec2((int)windowWidth >> l, ((int)windowWidth >> l)));
          }
          ImGui::Unindent(indentSize);
          ImGui::Separator();
        }
        ImGui::End();

        if (hdrDrawCurves) {
          const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
          ImGui::SetNextWindowBgAlpha(0.8f);
          ImGui::SetNextWindowPos({ width * 0.6f, height * 0.7f }, ImGuiCond_Appearing);
          ImGui::SetNextWindowSize({ width * 0.4f, height * 0.3f });
          ImGui::Begin("Tone mapping curve", nullptr, flags);
          const int kNumGraphPoints = 1001;
          float xs[kNumGraphPoints];
          float ysUchimura[kNumGraphPoints];
          float ysReinhard2[kNumGraphPoints];
          float ysKhronosPBR[kNumGraphPoints];
          for (int i = 0; i != kNumGraphPoints; i++) {
            xs[i]           = float(i) / kNumGraphPoints;
            ysUchimura[i]   = uchimura(xs[i], pcHDR.P, pcHDR.a, pcHDR.m, pcHDR.l, pcHDR.c, pcHDR.b);
            ysReinhard2[i]  = reinhard2(xs[i], pcHDR.maxWhite);
            ysKhronosPBR[i] = PBRNeutralToneMapping(xs[i], pcHDR.startCompression, pcHDR.desaturation);
          }
          if (ImPlot::BeginPlot("Tone mapping curves", { width * 0.4f, height * 0.3f }, ImPlotFlags_NoInputs)) {
            ImPlot::SetupAxes("Input", "Output");
            ImPlot::PlotLine("Uchimura", xs, ysUchimura, kNumGraphPoints);
            ImPlot::PlotLine("Reinhard", xs, ysReinhard2, kNumGraphPoints);
            ImPlot::PlotLine("Khronos PBR", xs, ysKhronosPBR, kNumGraphPoints);
            ImPlot::EndPlot();
          }
          ImGui::End();
        }
      }

      app.imgui_->endFrame(buf);

      buf.cmdEndRendering();
    }

	 // set the indirect command count of opaque objects drawing to be 0
	 // for GPU camera culling (compacted buffer way)
	 // if not culling every frame, we reset the command count to 0 only when next frame is about to cull
    if (frameCount % 3 == 0 || cullingEveryFrame)
	 buf.cmdFillBuffer(meshesOpaqueGPU.bufferIndirect_, 0, sizeof(uint32_t), 0);

    submitHandle[currentBufferId] = ctx->submit(buf, ctx->getCurrentSwapchainTexture());

    // retrieve culling results
    currentBufferId = (currentBufferId + 1) % LVK_ARRAY_NUM_ELEMENTS(bufferCullingData);

    if (cullingMode == CullingMode_GPU && app.fpsCounter_.numFrames_ > 1) {
      ctx->wait(submitHandle[currentBufferId]);
      ctx->download(bufferCullingData[currentBufferId], &numVisibleMeshes, sizeof(uint32_t), offsetof(CullingData, numVisibleMeshes));
    }

    // swap ping-pong textures
    std::swap(texAdaptedLum[0], texAdaptedLum[1]);
  });

  ctx.release();

  return 0;
}
