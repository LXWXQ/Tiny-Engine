#version 450

layout(location = 0) in vec3 inWorldPos;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outPBR;

// 引入 UBO
layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec3 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
} ubo;

void main() {
    // ==========================================
    // ✨ 核心修正 1：定义网格的物理尺寸
    // ==========================================
    float gridSize = 100.0;
    vec2 pos = inWorldPos.xz / gridSize;
    
    // 偏导数抗锯齿
    vec2 fw = fwidth(pos); 
    vec2 grid = fract(pos);
    
    vec2 edge = smoothstep(vec2(0.5) - fw, vec2(0.5) + fw, grid);
    float checker = abs(edge.x - edge.y); 
    
    // ==========================================
    // ✨ 核心修正 2：同步放大视距衰减！
    // ==========================================
    float dist = length(inWorldPos - ubo.cameraPos.xyz);
    
    // 既然水壶有几百米，我们就让格子在 3000 米才开始模糊，10000 米外才完全消失
    float fade = smoothstep(3000.0, 10000.0, dist); 
    
    checker = mix(checker, 0.5, fade);

    // 最终上色
    vec3 color = mix(vec3(0.0), vec3(1.0), checker);

    outPosition = vec4(inWorldPos, 1.0);
    outNormal = vec4(0.0, 1.0, 0.0, 0.0); 
    outAlbedo = vec4(color, 1.0);
    outPBR = vec4(0.0, 0.9, 0.0, 0.0);    
}