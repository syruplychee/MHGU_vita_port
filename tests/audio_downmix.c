#include <math.h>
#include <stdio.h>
#include "audio_downmix.h"

int main(void) {
    /* Independent channel impulses must route to their intended speaker(s).
     * Distinct gains catch coefficient swaps hidden by the title's equal
     * centre/LFE/surround gains. */
    const float gains[4] = {1.0f, 0.5f, 0.25f, 0.125f};
    const float expected[6][2] = {
        {1, 0}, {0, 1}, {0.5f, 0.5f}, {0.25f, 0.25f},
        {0.125f, 0}, {0, 0.125f}
    };
    for (unsigned ch = 0; ch < 6; ++ch) {
        float in[6] = {0}, out[2];
        in[ch] = 1;
        vp_audio_downmix6(in, gains, out);
        if (out[0] != expected[ch][0] || out[1] != expected[ch][1]) return 1;
    }
    const float in[6] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
    float out[2];
    vp_audio_downmix6(in, gains, out);
    if (fabsf(out[0] - 0.2125f) > 0.000001f ||
        fabsf(out[1] + 0.225f) > 0.000001f) return 2;
    puts("PASS audio downmix: six channel impulses and signed mixed signal");
    return 0;
}
