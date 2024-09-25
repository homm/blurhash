#include "encode.h"
#include "common.h"

#include <string.h>

static float *multiplyBasisFunction(
	int xComponent, int yComponent, int width, int height, uint8_t *rgb, size_t bytesPerRow,
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

	float factors[yComponents][xComponents][3];
	memset(factors, 0, sizeof(factors));

	init_sRGBToLinear_cache();

	float *cosX = (float *)malloc(sizeof(float) * width * xComponents);
	if (! cosX) return NULL;
	float *cosY = (float *)malloc(sizeof(float) * height);
	if (! cosY) {
		free(cosX);
		return NULL;
	}
	for(int x = 0; x < xComponents; x++) {
		for(int i = 0; i < width; i++) {
			cosX[x * width + i] = cosf(M_PI * x * i / width);
		}
	}
	for(int y = 0; y < yComponents; y++) {
		for(int i = 0; i < height; i++) {
			cosY[i] = cosf(M_PI * y * i / height);
		}
		for(int x = 0; x < xComponents; x++) {
			float *factor = multiplyBasisFunction(x, y, width, height, rgb, bytesPerRow,
				cosX + x * width, cosY);
			factors[y][x][0] = factor[0];
			factors[y][x][1] = factor[1];
			factors[y][x][2] = factor[2];
		}
	}
	free(cosX);
	free(cosY);

	float *dc = factors[0][0];
	float *ac = dc + 3;
	int acCount = xComponents * yComponents - 1;
	char *ptr = buffer;

	int sizeFlag = (xComponents - 1) + (yComponents - 1) * 9;
	ptr = encode_int(sizeFlag, 1, ptr);

	float maximumValue;
	if(acCount > 0) {
		float actualMaximumValue = 0;
		for(int i = 0; i < acCount * 3; i++) {
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
		ptr = encode_int(encodeAC(ac[i * 3 + 0], ac[i * 3 + 1], ac[i * 3 + 2], maximumValue), 2, ptr);
	}

	*ptr = 0;

	return buffer;
}

static float *multiplyBasisFunction(
	int xComponent, int yComponent, int width, int height, uint8_t *rgb, size_t bytesPerRow,
	float *cosX, float *cosY
) {
	float r = 0, g = 0, b = 0;
	float normalisation = (xComponent == 0 && yComponent == 0) ? 1 : 2;

	for(int y = 0; y < height; y++) {
		uint8_t *src = rgb + y * bytesPerRow;
		for(int x = 0; x < width; x++) {
			float basis = cosY[y] * cosX[x];
			r += basis * sRGBToLinear_cache[src[3 * x + 0]];
			g += basis * sRGBToLinear_cache[src[3 * x + 1]];
			b += basis * sRGBToLinear_cache[src[3 * x + 2]];
		}
	}

	float scale = normalisation / (width * height);

	static float result[3];
	result[0] = r * scale;
	result[1] = g * scale;
	result[2] = b * scale;

	return result;
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
