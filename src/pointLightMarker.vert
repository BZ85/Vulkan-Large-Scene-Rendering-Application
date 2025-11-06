//

layout(std430, buffer_reference) readonly buffer Matrices {
	mat4 mtx[];
};

layout(push_constant) uniform PushConstants {
   mat4 viewproj;
   Matrices matrices;
  
}pc;

layout(location = 0) in vec3 pos;

// Optional: uniform color for marker
layout(location = 0) out vec3 markerColor;

void main() {
 
   gl_Position = pc.viewproj * pc.matrices.mtx[gl_InstanceIndex] * vec4(pos, 1.0f);
   markerColor = vec3(1.0f, 1.0f, 1.0f);
}
