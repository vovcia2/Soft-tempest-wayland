/* GLSL ES 3.0 sources for the noise overlay.
 *
 * The fragment shader is shared with the Android version of the project: a
 * per-pixel, per-frame PCG hash seeded with gl_FragCoord and a frame counter so
 * that the noise is statistically independent from one frame to the next
 * (static noise averages out as sqrt(N) over N captured frames).
 */
#ifndef SOFT_TEMPEST_SHADERS_H
#define SOFT_TEMPEST_SHADERS_H

/* Full-screen triangle generated from gl_VertexID, no vertex buffer needed. */
static const char noise_vs[] =
    "#version 300 es\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

/* Modes:
 *   0  white   – uniform white noise
 *   1  hf      – energy pushed into high horizontal frequencies (pixel clock):
 *                alternating sign every column, randomly modulated
 *   2  temporal – additional random per-pixel/per-frame alpha modulation
 *
 * uFrame is a monotonically increasing frame counter supplied by the host. It
 * is used as the time seed instead of uTime so that the seed keeps changing
 * every frame even after hours of uptime (float precision of uTime * 240
 * would otherwise degrade). uTime is still provided for compatibility.
 */
static const char noise_fs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "out vec4 fragColor;\n"
    "uniform float uTime;\n"
    "uniform float uAmplitude;\n"
    "uniform int   uMode;\n"
    "uniform vec2  uResolution;\n"
    "uniform uint  uFrame;\n"
    "\n"
    "uint pcg(uint v) {\n"
    "    uint s = v * 747796405u + 2891336453u;\n"
    "    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;\n"
    "    return (w >> 22u) ^ w;\n"
    "}\n"
    "uint hash3(uvec3 v) { return pcg(v.x ^ pcg(v.y ^ pcg(v.z))); }\n"
    "float rnd(uvec3 v) {\n"
    "    return float(hash3(v) & 0x00FFFFFFu) / float(0x01000000u);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    uvec2 pix = uvec2(gl_FragCoord.xy);\n"
    "    uint frame = uFrame;\n"
    "    float n = rnd(uvec3(pix, frame));\n"
    "    float a = uAmplitude;\n"
    "    if (uMode == 1) {\n"
    "        float alt = ((pix.x & 1u) == 0u) ? 1.0 : -1.0;\n"
    "        n = 0.5 + 0.5 * alt * (2.0 * n - 1.0);\n"
    "    } else if (uMode == 2) {\n"
    "        a *= rnd(uvec3(pix.x ^ 2654435769u, pix.y, frame ^ 2246822519u));\n"
    "    }\n"
    "    fragColor = vec4(vec3(n), a);\n"
    "}\n";

#endif /* SOFT_TEMPEST_SHADERS_H */
