#include "LineCanvas.h"

static const char* codeVS = R"(
layout (location = 0) out vec4 out_color;

struct Vertex {
  vec4 pos;
  vec4 rgba;
};

layout(std430, buffer_reference) readonly buffer VertexBuffer {
  Vertex vertices[];
};

layout(push_constant) uniform PushConstants {
  mat4 mvp;
  VertexBuffer vb;
};

void main() {
  // Vertex v = vb.vertices[gl_VertexIndex]; <--- does not work on Snapdragon Adreno
  out_color = vb.vertices[gl_VertexIndex].rgba;
  gl_Position = mvp * vb.vertices[gl_VertexIndex].pos;
})";

static const char* codeFS = R"(
layout (location = 0) in vec4 in_color;
layout (location = 0) out vec4 out_color;

void main() {
  out_color = in_color;
})";

// 2D lines are drawn by ImGui renderer through BackgroundDrawList
// 2D lines are rendered together with ImGui windows, UI and graphs (the same pipeline)
void LineCanvas2D::render(const char* nameImGuiWindow)
{
  LVK_PROFILER_FUNCTION();

  // create a new full-screen window (the same size with main viewport)
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
  // disable all decorations and user input
  ImGui::Begin(
      nameImGuiWindow, nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);

  // background draw list contains all shapes that are behind all ImGui windows and widgets
  ImDrawList* drawList = ImGui::GetBackgroundDrawList();

  // add colored lines into the list one by one
  for (const LineData& l : lines_) {
    drawList->AddLine(ImVec2(l.p1.x, l.p1.y), ImVec2(l.p2.x, l.p2.y), ImColor(l.color.r, l.color.g, l.color.b, l.color.a));
  }

  ImGui::End();
}

void LineCanvas3D::line(const vec3& p1, const vec3& p2, const vec4& c)
{
	// add two colored vec3 points to the container
  lines_.push_back({ .pos = vec4(p1, 1.0f), .color = c });
  lines_.push_back({ .pos = vec4(p2, 1.0f), .color = c });
}

void LineCanvas3D::plane(
    const vec3& o, const vec3& v1, const vec3& v2, int n1, int n2, float s1, float s2, const vec4& color, const vec4& outlineColor)
{
  // draw 4 outer lines representing a plane segment
  // the plane is spanned by v1 and v2, which are two axises
  // s1 and s2 are the full length along each axis 
  // here we use the half-sizes of s1 and s2
  line(o - s1 / 2.0f * v1 - s2 / 2.0f * v2, o - s1 / 2.0f * v1 + s2 / 2.0f * v2, outlineColor);
  line(o + s1 / 2.0f * v1 - s2 / 2.0f * v2, o + s1 / 2.0f * v1 + s2 / 2.0f * v2, outlineColor);

  line(o - s1 / 2.0f * v1 + s2 / 2.0f * v2, o + s1 / 2.0f * v1 + s2 / 2.0f * v2, outlineColor);
  line(o - s1 / 2.0f * v1 - s2 / 2.0f * v2, o + s1 / 2.0f * v1 - s2 / 2.0f * v2, outlineColor);

  // draw n1 horizontal lines and n2 vertical lines
  for (int i = 1; i < n1; i++) {
    float t       = ((float)i - (float)n1 / 2.0f) * s1 / (float)n1;
    const vec3 o1 = o + t * v1;
    line(o1 - s2 / 2.0f * v2, o1 + s2 / 2.0f * v2, color);
  }

  for (int i = 1; i < n2; i++) {
    const float t = ((float)i - (float)n2 / 2.0f) * s2 / (float)n2;
    const vec3 o2 = o + t * v2;
    line(o2 - s1 / 2.0f * v1, o2 + s1 / 2.0f * v1, color);
  }
}

void LineCanvas3D::box(const mat4& m, const vec3& size, const vec4& c)
{
	// initial position of 8 corner points
  vec3 pts[8] = {
    vec3(+size.x, +size.y, +size.z), vec3(+size.x, +size.y, -size.z), vec3(+size.x, -size.y, +size.z), vec3(+size.x, -size.y, -size.z),
    vec3(-size.x, +size.y, +size.z), vec3(-size.x, +size.y, -size.z), vec3(-size.x, -size.y, +size.z), vec3(-size.x, -size.y, -size.z),
  };

  // transform 8 corner points using the provided model matrix (of the object that is surrounded by the box)
  for (auto& p : pts)
    p = vec3(m * vec4(p, 1.f));

  // render all 12 edges of box using the line function
  line(pts[0], pts[1], c);
  line(pts[2], pts[3], c);
  line(pts[4], pts[5], c);
  line(pts[6], pts[7], c);

  line(pts[0], pts[2], c);
  line(pts[1], pts[3], c);
  line(pts[4], pts[6], c);
  line(pts[5], pts[7], c);

  line(pts[0], pts[4], c);
  line(pts[1], pts[5], c);
  line(pts[2], pts[6], c);
  line(pts[3], pts[7], c);
}

// a trivial wrapper over the above box function, using the BoundingBox class
// the bounding box's center should be the same as the object's center
void LineCanvas3D::box(const mat4& m, const BoundingBox& box, const glm::vec4& color)
{
 // the low level box function expects the center of the box to be the origin
 // so we need to translate it to the bounding box's actual center 
  this->box(m * glm::translate(mat4(1.f), .5f * (box.min_ + box.max_)), 0.5f * vec3(box.max_ - box.min_), color);
}

void LineCanvas3D::frustum(const mat4& camView, const mat4& camProj, const vec4& color)
{
  const vec3 corners[] = { vec3(-1, -1, -1), vec3(+1, -1, -1), vec3(+1, +1, -1), vec3(-1, +1, -1),
                           vec3(-1, -1, +1), vec3(+1, -1, +1), vec3(+1, +1, +1), vec3(-1, +1, +1) };

  vec3 pp[8];

  for (int i = 0; i < 8; i++) {
    // the transformation matrix for each point is the inverse of provided view-projection matrix
	 // inverse(P * V) = inverse(V) * inverse(P)
    vec4 q = glm::inverse(camView) * glm::inverse(camProj) * vec4(corners[i], 1.0f);
    pp[i]  = vec3(q.x / q.w, q.y / q.w, q.z / q.w);
  }
  // side edges of the camera frustum
  line(pp[0], pp[4], color);
  line(pp[1], pp[5], color);
  line(pp[2], pp[6], color);
  line(pp[3], pp[7], color);

  // the near plane
  line(pp[0], pp[1], color);
  line(pp[1], pp[2], color);
  line(pp[2], pp[3], color);
  line(pp[3], pp[0], color);
  // the cross "X" inside the near plane
  line(pp[0], pp[2], color);
  line(pp[1], pp[3], color);

  // the far plane
  line(pp[4], pp[5], color);
  line(pp[5], pp[6], color);
  line(pp[6], pp[7], color);
  line(pp[7], pp[4], color);
  // the cross "X" inside the near plane
  line(pp[4], pp[6], color);
  line(pp[5], pp[7], color);

  const vec4 gridColor = color * 0.7f;
  const int gridLines  = 100;

  // draw the proportional lines on four sides
  // bottom
  {
    vec3 p1       = pp[0];
    vec3 p2       = pp[1];
    const vec3 s1 = (pp[4] - pp[0]) / float(gridLines);
    const vec3 s2 = (pp[5] - pp[1]) / float(gridLines);
    for (int i = 0; i != gridLines; i++, p1 += s1, p2 += s2)
      line(p1, p2, gridColor);
  }
  // top
  {
    vec3 p1       = pp[2];
    vec3 p2       = pp[3];
    const vec3 s1 = (pp[6] - pp[2]) / float(gridLines);
    const vec3 s2 = (pp[7] - pp[3]) / float(gridLines);
    for (int i = 0; i != gridLines; i++, p1 += s1, p2 += s2)
      line(p1, p2, gridColor);
  }
  // left
  {
    vec3 p1       = pp[0];
    vec3 p2       = pp[3];
    const vec3 s1 = (pp[4] - pp[0]) / float(gridLines);
    const vec3 s2 = (pp[7] - pp[3]) / float(gridLines);
    for (int i = 0; i != gridLines; i++, p1 += s1, p2 += s2)
      line(p1, p2, gridColor);
  }
  // right
  {
    vec3 p1       = pp[1];
    vec3 p2       = pp[2];
    const vec3 s1 = (pp[5] - pp[1]) / float(gridLines);
    const vec3 s2 = (pp[6] - pp[2]) / float(gridLines);
    for (int i = 0; i != gridLines; i++, p1 += s1, p2 += s2)
      line(p1, p2, gridColor);
  }
}

// 3D lines have its own pipeline which is different from ImGui and 2D lines' pipeline
void LineCanvas3D::render(lvk::IContext& ctx, const lvk::Framebuffer& desc, lvk::ICommandBuffer& buf, uint32_t numSamples)
{
  LVK_PROFILER_FUNCTION();

  if (lines_.empty()) {
    return;
  }

  const uint32_t requiredSize = lines_.size() * sizeof(LineData);

  // if the current buffer size is smaller than the required size, then reallocate the buffer
  if (currentBufferSize_[currentFrame_] < requiredSize) {
    linesBuffer_[currentFrame_] = ctx.createBuffer( // we use programmable vertex pulling, so the storage type of buffer is used
        { .usage = lvk::BufferUsageBits_Storage, .storage = lvk::StorageType_HostVisible, .size = requiredSize, .data = lines_.data() });
    currentBufferSize_[currentFrame_] = requiredSize;
  } else {
    ctx.upload(linesBuffer_[currentFrame_], lines_.data(), requiredSize);
  }

  // if there is no rendering pipeline available, then create a new one
  if (pipeline_.empty() || pipelineSamples_ != numSamples) {
    pipelineSamples_ = numSamples;

    vert_     = ctx.createShaderModule({ codeVS, lvk::Stage_Vert, "Shader Module: imgui (vert)" });
    frag_     = ctx.createShaderModule({ codeFS, lvk::Stage_Frag, "Shader Module: imgui (frag)" });
    pipeline_ = ctx.createRenderPipeline(
        {
            .topology     = lvk::Topology_Line,
            .smVert       = vert_,
            .smFrag       = frag_,
            .color        = { {
                       .format            = ctx.getFormat(desc.color[0].texture),
                       .blendEnabled      = true,
                       .srcRGBBlendFactor = lvk::BlendFactor_SrcAlpha, // simple alpha blending
                       .dstRGBBlendFactor = lvk::BlendFactor_OneMinusSrcAlpha,
            } },
            .depthFormat  = desc.depthStencil.texture ? ctx.getFormat(desc.depthStencil.texture) : lvk::Format_Invalid,
            .cullMode     = lvk::CullMode_None,
            .samplesCount = numSamples,
        },
        nullptr);
  }

  // mvp matrix and the buffer (contains line data) device address is updated by push constants
  struct {
    mat4 mvp;
    uint64_t addr;
  } pc{
    .mvp  = mvp_,
    .addr = ctx.gpuAddress(linesBuffer_[currentFrame_]),
  };
  buf.cmdBindRenderPipeline(pipeline_);
  buf.cmdPushConstants(pc);
  buf.cmdDraw(lines_.size());

  // switch to the next line buffer for next frame
  currentFrame_ = (currentFrame_ + 1) % LVK_ARRAY_NUM_ELEMENTS(linesBuffer_);
}
