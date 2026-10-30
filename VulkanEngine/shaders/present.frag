#version 450

// Final post-process:
// HDR + Bloom -> camera motion blur -> exposure -> ACES -> gamma -> swapchain.
// Camera motion vectors are reconstructed by projecting each pixel's world
// position with the previous frame's view-projection matrix.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

struct PointLight
{
    vec4 position;
    vec4 color;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projectionView;
    vec4 ambientLightColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 cameraPos;
    int numLights;
    float exposure;
    PointLight pointLights[100];
    mat4 previousProjectionView;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D hdrScene;
layout(set = 1, binding = 1) uniform sampler2D bloomTex;
layout(set = 1, binding = 2) uniform sampler2D worldPosTex;

layout(push_constant) uniform Push {
    float exposure;
    float bloomIntensity;
    float bloomEnabled;
    float motionBlurEnabled;
    float motionBlurStrength;
    float motionBlurMaxRadius;
    float motionBlurFrame;
    float pad;
} pc;

const int MOTION_BLUR_SAMPLES = 32;

vec3 SampleScene(vec2 uv)
{
    vec3 hdr = texture(hdrScene, uv).rgb;
    if (pc.bloomEnabled > 0.5)
        hdr += texture(bloomTex, uv).rgb * pc.bloomIntensity;
    return hdr;
}

vec2 CameraMotionVector(vec2 uv)
{
    vec3 worldPos = texture(worldPosTex, uv).xyz;
    vec4 previousClip = ubo.previousProjectionView * vec4(worldPos, 1.0);
    if (previousClip.w <= 0.0001)
        return vec2(0.0);

    vec2 previousUV = previousClip.xy / previousClip.w * 0.5 + 0.5;
    return uv - previousUV;
}

float Hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 ACESFilm(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 hdr;
    if (pc.motionBlurEnabled > 0.5)
    {
        vec2 velocity = CameraMotionVector(inUV) * pc.motionBlurStrength;
        float velocityLength = length(velocity);

        if (velocityLength > 0.00001)
        {
            velocity *= min(velocityLength, pc.motionBlurMaxRadius) / velocityLength;

            // Fixed sample positions produce visible ghost bands when the
            // velocity is large. Jitter each pixel's samples with a spatial
            // noise pattern and keep one sharp center sample.
            // Stochastic stratified sampling. Without the per-pixel temporal
            // jitter, a large velocity shows the discrete taps as ghost bands.
            const float frameSeed = pc.motionBlurFrame;
            const vec2 texel = 1.0 / vec2(textureSize(hdrScene, 0));
            hdr = SampleScene(inUV) * 2.0;
            float totalWeight = 2.0;

            for (int i = 0; i < MOTION_BLUR_SAMPLES; ++i)
            {
                float sampleNoise = Hash12(gl_FragCoord.xy +
                    vec2(float(i) * 13.17 + frameSeed * 3.1,
                         float(i) * 5.31 + frameSeed * 9.7));
                float lateralNoise = Hash12(gl_FragCoord.xy +
                    vec2(float(i) * 7.73 - frameSeed * 5.3,
                         float(i) * 19.21 + frameSeed * 2.9));

                float t = (float(i) + sampleNoise) / float(MOTION_BLUR_SAMPLES);
                vec2 subpixelJitter = (vec2(sampleNoise, lateralNoise) - 0.5) * texel;
                vec2 sampleUV = clamp(inUV - velocity * t + subpixelJitter,
                    vec2(0.0), vec2(1.0));

                // A smoother, center-biased kernel avoids hard ghost bands
                // while still leaving a visible trail at high angular speed.
                float weight = exp(-6.0 * t * t);
                hdr += SampleScene(sampleUV) * weight;
                totalWeight += weight;
            }
            hdr /= totalWeight;
        }
        else
        {
            hdr = SampleScene(inUV);
        }
    }
    else
    {
        hdr = SampleScene(inUV);
    }

    vec3 color = ACESFilm(hdr * pc.exposure);
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}
