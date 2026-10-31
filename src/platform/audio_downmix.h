#ifndef VP_AUDIO_DOWNMIX_H
#define VP_AUDIO_DOWNMIX_H

/* Reviewed device-sink order: FL, FR, C, LFE, BL, BR. Coefficients are
 * supplied by the locked SDK's sink parameters, never guessed by the host. */
static inline void vp_audio_downmix6(const float in[6], const float coeff[4],
                                     float out[2]) {
    const float shared = in[2] * coeff[1] + in[3] * coeff[2];
    out[0] = in[0] * coeff[0] + shared + in[4] * coeff[3];
    out[1] = in[1] * coeff[0] + shared + in[5] * coeff[3];
}

#endif
