#include "encode.h"
#include "common.h"

#include <string.h>

#ifdef __SSE__
#include <xmmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(_MSC_VER)
    #define ALIGN(x) __declspec(align(x))
#elif defined(__GNUC__) || defined(__clang__)
    #define ALIGN(x) __attribute__((aligned(x)))
#else
    #define ALIGN(x)
#endif

static void multiplyBasisFunction(
	float factors[][4], int factorsCount, int width, int height, uint8_t *rgb, size_t bytesPerRow,
	float *cosX, float *cosY);
static char *encode_int(int value, int length, char *destination);

static int encodeDC(float r, float g, float b);
static int encodeAC(float r, float g, float b, float maximumValue);

float *sRGBToLinear_cache = NULL;

static void init_sRGBToLinear_cache() {
	if (sRGBToLinear_cache != NULL) {
		return;
	}
	sRGBToLinear_cache = (float *)malloc(sizeof(float) * 256);
	for (int x = 0; x < 256; x++) {
		sRGBToLinear_cache[x] = sRGBToLinear(x);
	}
}

const char *blurHashForPixels(int xComponents, int yComponents, int width, int height, uint8_t *rgb, size_t bytesPerRow) {
	static char buffer[2 + 4 + (9 * 9 - 1) * 2 + 1];

	if(xComponents < 1 || xComponents > 9) return NULL;
	if(yComponents < 1 || yComponents > 9) return NULL;

	float ALIGN(16) factors[yComponents * xComponents][4];
	int factorsCount = xComponents * yComponents;
	memset(factors, 0, sizeof(factors));

	init_sRGBToLinear_cache();

	float *cosX = (float *)malloc(sizeof(float) * width * factorsCount);
	if (! cosX) return NULL;
	float *cosY = (float *)malloc(sizeof(float) * height * factorsCount);
	if (! cosY) {
		free(cosX);
		return NULL;
	}
	for(int i = 0; i < width; i++) {
		for(int x = 0; x < xComponents; x++) {
			float weight = cosf(M_PI * x * i / width);
			for(int y = 0; y < yComponents; y++) {
				cosX[i * factorsCount + y * xComponents + x] = weight;
			}
		}
	}
	for(int i = 0; i < height; i++) {
		for(int y = 0; y < yComponents; y++) {
			float weight = cosf(M_PI * y * i / height);
			for(int x = 0; x < xComponents; x++) {
				cosY[i * factorsCount + y * xComponents + x] = weight;
			}
		}
	}
	multiplyBasisFunction(factors, factorsCount, width, height, rgb, bytesPerRow, cosX, cosY);
	free(cosX);
	free(cosY);

	float *dc = factors[0];
	float *ac = dc + 4;
	int acCount = factorsCount - 1;
	char *ptr = buffer;

	int sizeFlag = (xComponents - 1) + (yComponents - 1) * 9;
	ptr = encode_int(sizeFlag, 1, ptr);

	float maximumValue;
	if(acCount > 0) {
		float actualMaximumValue = 0;
		for(int i = 0; i < acCount * 4; i++) {
			actualMaximumValue = fmaxf(fabsf(ac[i]), actualMaximumValue);
		}

		int quantisedMaximumValue = fmaxf(0, fminf(82, floorf(actualMaximumValue * 166 - 0.5)));
		maximumValue = ((float)quantisedMaximumValue + 1) / 166;
		ptr = encode_int(quantisedMaximumValue, 1, ptr);
	} else {
		maximumValue = 1;
		ptr = encode_int(0, 1, ptr);
	}

	ptr = encode_int(encodeDC(dc[0], dc[1], dc[2]), 4, ptr);

	for(int i = 0; i < acCount; i++) {
		ptr = encode_int(encodeAC(ac[i * 4 + 0], ac[i * 4 + 1], ac[i * 4 + 2], maximumValue), 2, ptr);
	}

	*ptr = 0;

	return buffer;
}

static void multiplyBasisFunction(
	float factors[][4], int factorsCount, int width, int height, uint8_t *rgb, size_t bytesPerRow,
	float *cosX, float *cosY
) {
	int fi = 0;
#if defined(__aarch64__)
	for (int y = 0; y < height; y++) {
		fi = 0;
		for (; fi < factorsCount - 11; fi += 12) {
			float32x4_t factor0 = vld1q_f32(factors[fi+0]);
			float32x4_t factor1 = vld1q_f32(factors[fi+1]);
			float32x4_t factor2 = vld1q_f32(factors[fi+2]);
			float32x4_t factor3 = vld1q_f32(factors[fi+3]);
			float32x4_t factor4 = vld1q_f32(factors[fi+4]);
			float32x4_t factor5 = vld1q_f32(factors[fi+5]);
			float32x4_t factor6 = vld1q_f32(factors[fi+6]);
			float32x4_t factor7 = vld1q_f32(factors[fi+7]);
			float32x4_t factor8 = vld1q_f32(factors[fi+8]);
			float32x4_t factor9 = vld1q_f32(factors[fi+9]);
			float32x4_t factor10 = vld1q_f32(factors[fi+10]);
			float32x4_t factor11 = vld1q_f32(factors[fi+11]);
			uint8_t *src = rgb + y * bytesPerRow;
			float *cosYLocal = cosY + y * factorsCount;
			int x = 0;
			float32x4_t pixel = vaddq_f32(
				vaddq_f32(
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+0) + 0]], sRGBToLinear_cache[src[3 * (x+0) + 1]], sRGBToLinear_cache[src[3 * (x+0) + 2]]},
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+1) + 0]], sRGBToLinear_cache[src[3 * (x+1) + 1]], sRGBToLinear_cache[src[3 * (x+1) + 2]]}),
				vaddq_f32(
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+2) + 0]], sRGBToLinear_cache[src[3 * (x+2) + 1]], sRGBToLinear_cache[src[3 * (x+2) + 2]]},
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+3) + 0]], sRGBToLinear_cache[src[3 * (x+3) + 1]], sRGBToLinear_cache[src[3 * (x+3) + 2]]})
			);
			float32x4_t next_pixel = vaddq_f32(
				vaddq_f32(
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+4) + 0]], sRGBToLinear_cache[src[3 * (x+4) + 1]], sRGBToLinear_cache[src[3 * (x+4) + 2]]},
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+5) + 0]], sRGBToLinear_cache[src[3 * (x+5) + 1]], sRGBToLinear_cache[src[3 * (x+5) + 2]]}),
				vaddq_f32(
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+6) + 0]], sRGBToLinear_cache[src[3 * (x+6) + 1]], sRGBToLinear_cache[src[3 * (x+6) + 2]]},
					(float32x4_t){sRGBToLinear_cache[src[3 * (x+7) + 0]], sRGBToLinear_cache[src[3 * (x+7) + 1]], sRGBToLinear_cache[src[3 * (x+7) + 2]]})
			);
			for(; x < width - 11; x += 4) {
				float *cosXLocal = cosX + x * factorsCount;
				float32x4_t next_next_pixel = vaddq_f32(
					vaddq_f32(
						(float32x4_t){sRGBToLinear_cache[src[3 * (x+8) + 0]], sRGBToLinear_cache[src[3 * (x+8) + 1]], sRGBToLinear_cache[src[3 * (x+8) + 2]]},
						(float32x4_t){sRGBToLinear_cache[src[3 * (x+9) + 0]], sRGBToLinear_cache[src[3 * (x+9) + 1]], sRGBToLinear_cache[src[3 * (x+9) + 2]]}),
					vaddq_f32(
						(float32x4_t){sRGBToLinear_cache[src[3 * (x+10) + 0]], sRGBToLinear_cache[src[3 * (x+10) + 1]], sRGBToLinear_cache[src[3 * (x+10) + 2]]},
						(float32x4_t){sRGBToLinear_cache[src[3 * (x+11) + 0]], sRGBToLinear_cache[src[3 * (x+11) + 1]], sRGBToLinear_cache[src[3 * (x+11) + 2]]})
				);
				
				float32x4_t basis = vmulq_f32(vld1q_f32(&cosYLocal[fi]), vld1q_f32(&cosXLocal[fi]));
				factor0 = vmlaq_laneq_f32(factor0, pixel, basis, 0);
				factor1 = vmlaq_laneq_f32(factor1, pixel, basis, 1);
				factor2 = vmlaq_laneq_f32(factor2, pixel, basis, 2);
				factor3 = vmlaq_laneq_f32(factor3, pixel, basis, 3);
				basis = vmulq_f32(vld1q_f32(&cosYLocal[fi+4]), vld1q_f32(&cosXLocal[fi+4]));
				factor4 = vmlaq_laneq_f32(factor4, pixel, basis, 0);
				factor5 = vmlaq_laneq_f32(factor5, pixel, basis, 1);
				factor6 = vmlaq_laneq_f32(factor6, pixel, basis, 2);
				factor7 = vmlaq_laneq_f32(factor7, pixel, basis, 3);
				basis = vmulq_f32(vld1q_f32(&cosYLocal[fi+8]), vld1q_f32(&cosXLocal[fi+8]));
				factor8 = vmlaq_laneq_f32(factor8, pixel, basis, 0);
				factor9 = vmlaq_laneq_f32(factor9, pixel, basis, 1);
				factor10 = vmlaq_laneq_f32(factor10, pixel, basis, 2);
				factor11 = vmlaq_laneq_f32(factor11, pixel, basis, 3);
				pixel = next_pixel;
				next_pixel = next_next_pixel;
			}
			for(; x < width; x++) {
				float *cosXLocal = cosX + x * factorsCount;
				float32x4_t pixel = {sRGBToLinear_cache[src[3 * x + 0]],
					sRGBToLinear_cache[src[3 * x + 1]], sRGBToLinear_cache[src[3 * x + 2]]};
				
				float32x4_t basis = vmulq_f32(vld1q_f32(&cosYLocal[fi]), vld1q_f32(&cosXLocal[fi]));
				factor0 = vmlaq_laneq_f32(factor0, pixel, basis, 0);
				factor1 = vmlaq_laneq_f32(factor1, pixel, basis, 1);
				factor2 = vmlaq_laneq_f32(factor2, pixel, basis, 2);
				factor3 = vmlaq_laneq_f32(factor3, pixel, basis, 3);
				basis = vmulq_f32(vld1q_f32(&cosYLocal[fi+4]), vld1q_f32(&cosXLocal[fi+4]));
				factor4 = vmlaq_laneq_f32(factor4, pixel, basis, 0);
				factor5 = vmlaq_laneq_f32(factor5, pixel, basis, 1);
				factor6 = vmlaq_laneq_f32(factor6, pixel, basis, 2);
				factor7 = vmlaq_laneq_f32(factor7, pixel, basis, 3);
				basis = vmulq_f32(vld1q_f32(&cosYLocal[fi+8]), vld1q_f32(&cosXLocal[fi+8]));
				factor8 = vmlaq_laneq_f32(factor8, pixel, basis, 0);
				factor9 = vmlaq_laneq_f32(factor9, pixel, basis, 1);
				factor10 = vmlaq_laneq_f32(factor10, pixel, basis, 2);
				factor11 = vmlaq_laneq_f32(factor11, pixel, basis, 3);
			}
			vst1q_f32(factors[fi+0], factor0);
			vst1q_f32(factors[fi+1], factor1);
			vst1q_f32(factors[fi+2], factor2);
			vst1q_f32(factors[fi+3], factor3);
			vst1q_f32(factors[fi+4], factor4);
			vst1q_f32(factors[fi+5], factor5);
			vst1q_f32(factors[fi+6], factor6);
			vst1q_f32(factors[fi+7], factor7);
			vst1q_f32(factors[fi+8], factor8);
			vst1q_f32(factors[fi+9], factor9);
			vst1q_f32(factors[fi+10], factor10);
			vst1q_f32(factors[fi+11], factor11);
		}
	}
#endif
	for (int y = 0; y < height; y++) {
		uint8_t *src = rgb + y * bytesPerRow;
		float *cosYLocal = cosY + y * factorsCount;
		int x = 0;
		for(; x < width - 3; x += 4) {
			float *cosXLocal = cosX + x * factorsCount;
#ifdef __SSE__
			__m128 pixel0 = _mm_set_ps(0, sRGBToLinear_cache[src[3 * (x+0) + 2]], sRGBToLinear_cache[src[3 * (x+0) + 1]], sRGBToLinear_cache[src[3 * (x+0) + 0]]);
			__m128 pixel1 = _mm_set_ps(0, sRGBToLinear_cache[src[3 * (x+1) + 2]], sRGBToLinear_cache[src[3 * (x+1) + 1]], sRGBToLinear_cache[src[3 * (x+1) + 0]]);
			__m128 pixel2 = _mm_set_ps(0, sRGBToLinear_cache[src[3 * (x+2) + 2]], sRGBToLinear_cache[src[3 * (x+2) + 1]], sRGBToLinear_cache[src[3 * (x+2) + 0]]);
			__m128 pixel3 = _mm_set_ps(0, sRGBToLinear_cache[src[3 * (x+3) + 2]], sRGBToLinear_cache[src[3 * (x+3) + 1]], sRGBToLinear_cache[src[3 * (x+3) + 0]]);
			for (int i = 0; i < factorsCount; i++) {
				__m128 basis0 = _mm_set1_ps(cosYLocal[i] * cosXLocal[i + 0 * factorsCount]);
				__m128 basis1 = _mm_set1_ps(cosYLocal[i] * cosXLocal[i + 1 * factorsCount]);
				__m128 basis2 = _mm_set1_ps(cosYLocal[i] * cosXLocal[i + 2 * factorsCount]);
				__m128 basis3 = _mm_set1_ps(cosYLocal[i] * cosXLocal[i + 3 * factorsCount]);
				__m128 factor = _mm_loadu_ps(factors[i]);
				factor = _mm_add_ps(factor, _mm_mul_ps(basis0, pixel0));
				factor = _mm_add_ps(factor, _mm_mul_ps(basis1, pixel1));
				factor = _mm_add_ps(factor, _mm_mul_ps(basis2, pixel2));
				factor = _mm_add_ps(factor, _mm_mul_ps(basis3, pixel3));
				_mm_storeu_ps(factors[i], factor);
			}
#else
			float pixel0[4] = {sRGBToLinear_cache[src[3 * (x+0) + 0]], sRGBToLinear_cache[src[3 * (x+0) + 1]], sRGBToLinear_cache[src[3 * (x+0) + 2]]};
			float pixel1[4] = {sRGBToLinear_cache[src[3 * (x+1) + 0]], sRGBToLinear_cache[src[3 * (x+1) + 1]], sRGBToLinear_cache[src[3 * (x+1) + 2]]};
			float pixel2[4] = {sRGBToLinear_cache[src[3 * (x+2) + 0]], sRGBToLinear_cache[src[3 * (x+2) + 1]], sRGBToLinear_cache[src[3 * (x+2) + 2]]};
			float pixel3[4] = {sRGBToLinear_cache[src[3 * (x+3) + 0]], sRGBToLinear_cache[src[3 * (x+3) + 1]], sRGBToLinear_cache[src[3 * (x+3) + 2]]};
			for (int i = fi; i < factorsCount; i++) {
				float basis0 = cosYLocal[i] * cosXLocal[i + 0 * factorsCount];
				float basis1 = cosYLocal[i] * cosXLocal[i + 1 * factorsCount];
				float basis2 = cosYLocal[i] * cosXLocal[i + 2 * factorsCount];
				float basis3 = cosYLocal[i] * cosXLocal[i + 3 * factorsCount];
				factors[i][0] += basis0 * pixel0[0] + basis1 * pixel1[0] + basis2 * pixel2[0] + basis3 * pixel3[0];
				factors[i][1] += basis0 * pixel0[1] + basis1 * pixel1[1] + basis2 * pixel2[1] + basis3 * pixel3[1];
				factors[i][2] += basis0 * pixel0[2] + basis1 * pixel1[2] + basis2 * pixel2[2] + basis3 * pixel3[2];
			}
#endif
		}
		for(; x < width; x++) {
			float pixel[4];
			float *cosXLocal = cosX + x * factorsCount;
			pixel[0] = sRGBToLinear_cache[src[3 * x + 0]];
			pixel[1] = sRGBToLinear_cache[src[3 * x + 1]];
			pixel[2] = sRGBToLinear_cache[src[3 * x + 2]];
			for (int i = fi; i < factorsCount; i++) {
				float basis = cosYLocal[i] * cosXLocal[i];
				factors[i][0] += basis * pixel[0];
				factors[i][1] += basis * pixel[1];
				factors[i][2] += basis * pixel[2];
			}
		}
	}

	for (int i = 0; i < factorsCount; i++) {
		float normalisation = (i == 0) ? 1 : 2;
		float scale = normalisation / (width * height);
		factors[i][0] *= scale;
		factors[i][1] *= scale;
		factors[i][2] *= scale;
	}
}



static int encodeDC(float r, float g, float b) {
	int roundedR = linearTosRGB(r);
	int roundedG = linearTosRGB(g);
	int roundedB = linearTosRGB(b);
	return (roundedR << 16) + (roundedG << 8) + roundedB;
}

static int encodeAC(float r, float g, float b, float maximumValue) {
	int quantR = fmaxf(0, fminf(18, floorf(signPow(r / maximumValue, 0.5) * 9 + 9.5)));
	int quantG = fmaxf(0, fminf(18, floorf(signPow(g / maximumValue, 0.5) * 9 + 9.5)));
	int quantB = fmaxf(0, fminf(18, floorf(signPow(b / maximumValue, 0.5) * 9 + 9.5)));

	return quantR * 19 * 19 + quantG * 19 + quantB;
}

static char characters[83]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

static char *encode_int(int value, int length, char *destination) {
	int divisor = 1;
	for(int i = 0; i < length - 1; i++) divisor *= 83;

	for(int i = 0; i < length; i++) {
		int digit = (value / divisor) % 83;
		divisor /= 83;
		*destination++ = characters[digit];
	}
	return destination;
}
