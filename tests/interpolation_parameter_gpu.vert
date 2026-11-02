
#version 450
layout(location=0) out vec4 value;
void main(){
 const vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
 const float w[3]=float[3](1,2,4);
 const float a[3]=float[3](0.125,0.375,0.875);
 gl_Position=vec4(p[gl_VertexIndex]*w[gl_VertexIndex],0,w[gl_VertexIndex]);
 value=vec4(a[gl_VertexIndex]);
}
