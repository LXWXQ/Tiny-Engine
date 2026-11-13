#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragTint;
layout(location = 4) in float fragGradient;
layout(location = 5) in float fragPhase;
layout(location = 6) in float fragLayer;
layout(location = 7) in float fragShadowDisc;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;
layout(location = 3) out vec4 outPBR;

layout(push_constant) uniform Push {
    vec4 timeWind;
    vec4 grassColor;
    vec4 treeColor;
    vec4 trunkColor;
} push;

float hash11(float n)
{
    return fract(sin(n * 127.1 + 311.7) * 43758.5453);
}

void main()
{
    vec3 albedo;
    vec3 normal;

    if (push.timeWind.z < 0.5)
    {
        // Each quad is cut into four blades. Width, height and lean all vary
        // per blade so a clump no longer reads as a flat green card.
        float cell = floor(fragUV.x * 4.0);
        float local = fract(fragUV.x * 4.0) - 0.5;
        float rnd = hash11(cell + floor(fragPhase * 91.0) * 7.0 + 1.0);
        float bladeHeight = 0.54 + 0.42 * rnd;
        float bend = (fragUV.y * fragUV.y) * (rnd - 0.5) * 0.62;
        float width = mix(0.40, 0.070, fragUV.y);
        float distanceToStem = abs(local + bend);
        float blade = 1.0 - smoothstep(width * 0.50, width, distanceToStem);
        blade *= step(fragUV.y, bladeHeight);

        if (blade < 0.28)
            discard;

        float gradient = 0.50 + 0.58 * fragUV.y;
        albedo = fragTint * gradient * (0.86 + 0.26 * rnd);
        albedo *= vec3(0.92 + 0.16 * rnd, 1.0, 0.86 + 0.12 * rnd);
        // A little cool shadow at the root keeps the clump from looking like
        // a glowing green blob in overcast light.
        albedo *= mix(0.86, 1.0, smoothstep(0.0, 0.35, fragUV.y));
        normal = normalize(mix(fragNormal, vec3(0.0, 1.0, 0.0), 0.62));
    }
    else
    {
        // color.r is the canopy mask (0 trunk, 1 canopy), color.g is a
        // per-cluster vertical shade baked by the generator, color.b marks the
        // flat contact-shadow card at the root.
        if (fragShadowDisc > 0.5)
        {
            const float radial = length(fragUV - vec2(0.5)) * 2.0;
            const float edge = smoothstep(0.25, 1.0, radial);
            albedo = vec3(0.035, 0.050, 0.022) * mix(0.55, 1.0, edge);
            normal = vec3(0.0, 1.0, 0.0);
        }
        else if (fragGradient > 0.5)
        {
            float crownShade = 0.72 + 0.28 * clamp(fragLayer, 0.0, 1.0);
            float grain = 0.94 + 0.10 * hash11(dot(fragWorldPos.xz, vec2(3.1, 2.7)));
            albedo = fragTint * crownShade *
                     (0.80 + 0.28 * clamp(fragNormal.y, 0.0, 1.0)) * grain;
            normal = normalize(mix(fragNormal, vec3(0.0, 1.0, 0.0), 0.30));
        }
        else
        {
            albedo = push.trunkColor.rgb * (0.70 + 0.30 * fragUV.y);
            normal = normalize(fragNormal);
        }
    }

    outPosition = vec4(fragWorldPos, 1.0);
    outNormal = vec4(normal, 1.0);
    outAlbedo = vec4(albedo, 1.0);

    float roughness = push.timeWind.z < 0.5 ? 0.90 : 0.80;
    outPBR = vec4(0.0, roughness, 0.0, 1.0);
}
