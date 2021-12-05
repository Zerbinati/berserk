// Berserk is a UCI compliant chess engine written in C
// Copyright (C) 2021 Jay Honnold

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "bits.h"
#include "board.h"
#include "nn.h"
#include "util.h"

#define INCBIN_PREFIX
#define INCBIN_STYLE INCBIN_STYLE_CAMEL
#include "incbin.h"

#ifndef EVALFILE
#define EVALFILE "default.nn"
#endif

uint64_t DEFAULT_NN_HASH = UINT64_MAX;

INCBIN(Embed, EVALFILE);

const int QUANTIZATION_PRECISION_IN = 32;
const int QUANTIZATION_PRECISION_OUT = 512;

int16_t FEATURE_WEIGHTS[N_FEATURES * N_HIDDEN] __attribute__((aligned(ALIGN_ON)));
int16_t HIDDEN_BIASES[N_HIDDEN] __attribute__((aligned(ALIGN_ON)));
int16_t HIDDEN_WEIGHTS[2 * N_HIDDEN] __attribute__((aligned(ALIGN_ON)));
int32_t OUTPUT_BIAS;

inline void RefreshAccumulator(Accumulator accumulator, Board* board, const int perspective) {
  int kingSq = lsb(PieceBB(KING, perspective));

  memcpy(accumulator, HIDDEN_BIASES, sizeof(Accumulator));

  BitBoard occ = OccBB(BOTH);
  while (occ) {
    int sq = popAndGetLsb(&occ);
    int pc = board->squares[sq];
    int feature = FeatureIdx(pc, sq, kingSq, perspective);

    for (int i = 0; i < N_HIDDEN; i++) accumulator[i] += FEATURE_WEIGHTS[feature * N_HIDDEN + i];
  }
}

#if defined(__AVX2__)
const size_t WIDTH = sizeof(__m256i) / sizeof(int16_t);
const size_t CHUNKS = N_HIDDEN / WIDTH;

int OutputLayer(Accumulator stm, Accumulator xstm) {
  int result = OUTPUT_BIAS * QUANTIZATION_PRECISION_IN;

  const __m256i zero = _mm256_setzero_si256();
  __m256i s0 = _mm256_setzero_si256();
  __m256i s1 = _mm256_setzero_si256();

  for (size_t j = 0; j < CHUNKS; j++) {
    const __m256i ac0 = _mm256_max_epi16(*(__m256i*)&stm[j * WIDTH], zero);
    const __m256i ac1 = _mm256_max_epi16(*(__m256i*)&xstm[j * WIDTH], zero);

    s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(ac0, *(__m256i*)&HIDDEN_WEIGHTS[j * WIDTH]));
    s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(ac1, *(__m256i*)&HIDDEN_WEIGHTS[j * WIDTH + N_HIDDEN]));
  }

  const __m256i r8 = _mm256_add_epi32(s0, s1);
  const __m128i r4 = _mm_add_epi32(_mm256_castsi256_si128(r8), _mm256_extractf128_si256(r8, 1));
  const __m128i r2 = _mm_add_epi32(r4, _mm_srli_si128(r4, 8));
  const __m128i r1 = _mm_add_epi32(r2, _mm_srli_si128(r2, 4));

  result += _mm_cvtsi128_si32(r1);
  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#elif defined(__SSE__)
const size_t WIDTH = sizeof(__m128i) / sizeof(int16_t);
const size_t CHUNKS = N_HIDDEN / WIDTH;

int OutputLayer(Accumulator stm, Accumulator xstm) {
  int result = OUTPUT_BIAS * QUANTIZATION_PRECISION_IN;

  const __m128i zero = _mm_setzero_si128();
  __m128i s0 = _mm_setzero_si128();
  __m128i s1 = _mm_setzero_si128();

  for (size_t j = 0; j < N_HIDDEN / 8; j++) {
    const __m128i ac0 = _mm_max_epi16(*(__m128i*)&stm[j * WIDTH], zero);
    const __m128i ac1 = _mm_max_epi16(*(__m128i*)&xstm[j * WIDTH], zero);

    s0 = _mm_add_epi32(s0, _mm_madd_epi16(ac0, *(__m128i*)&HIDDEN_WEIGHTS[j * WIDTH]));
    s1 = _mm_add_epi32(s1, _mm_madd_epi16(ac1, *(__m128i*)&HIDDEN_WEIGHTS[j * WIDTH + N_HIDDEN]));
  }

  const __m128i r4 = _mm_add_epi32(s0, s1);
  const __m128i r2 = _mm_add_epi32(r4, _mm_srli_si128(r4, 8));
  const __m128i r1 = _mm_add_epi32(r2, _mm_srli_si128(r2, 4));

  result += _mm_cvtsi128_si32(r1);
  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#else
int OutputLayer(Accumulator stm, Accumulator xstm) {
  int result = OUTPUT_BIAS * QUANTIZATION_PRECISION_IN;

  for (int i = 0; i < N_HIDDEN; i++) {
    result += max(stm[i], 0) * HIDDEN_WEIGHTS[i];
    result += max(xstm[i], 0) * HIDDEN_WEIGHTS[i + N_HIDDEN];
  }

  return result / QUANTIZATION_PRECISION_IN / QUANTIZATION_PRECISION_OUT;
}
#endif

int Predict(Board* board) {
  Accumulator stm, xstm;

  RefreshAccumulator(stm, board, board->stm);
  RefreshAccumulator(xstm, board, board->xstm);

  return OutputLayer(stm, xstm);
}

void ApplyUpdates(Board* board, int stm, NNUpdate* updates) {
  int16_t* output = board->accumulators[stm][board->ply];
  int16_t* prev = board->accumulators[stm][board->ply - 1];

  if (updates->nr) {
    for (int j = 0; j < N_HIDDEN; j++) output[j] = prev[j] - FEATURE_WEIGHTS[updates->removals[0] * N_HIDDEN + j];

    for (int i = 1; i < updates->nr; i++)
      for (int j = 0; j < N_HIDDEN; j++) output[j] -= FEATURE_WEIGHTS[updates->removals[i] * N_HIDDEN + j];

    for (int i = 0; i < updates->na; i++)
      for (int j = 0; j < N_HIDDEN; j++) output[j] += FEATURE_WEIGHTS[updates->additions[i] * N_HIDDEN + j];
  } else {
    memcpy(output, prev, sizeof(Accumulator));
  }
}

INLINE int16_t LoadWeight(float v, int precision) { return round(v * precision); }

void LoadDefaultNN() {
  if (EmbedData[0] != 'B' || EmbedData[1] != 'R' || EmbedData[2] != 'K' || EmbedData[3] != 'R')
    printf("info string Berserk was not built using a standard net, use with caution!\n");

  DEFAULT_NN_HASH = *((uint64_t*)(EmbedData + 4));

  float* data = (float*)EmbedData + 3;  // Skip the 4 byte magic and 8 byte hash

  for (int j = 0; j < N_FEATURES * N_HIDDEN; j++) FEATURE_WEIGHTS[j] = LoadWeight(*data++, QUANTIZATION_PRECISION_IN);

  for (int j = 0; j < N_HIDDEN; j++) HIDDEN_BIASES[j] = LoadWeight(*data++, QUANTIZATION_PRECISION_IN);

  for (int j = 0; j < N_HIDDEN * 2; j++) HIDDEN_WEIGHTS[j] = LoadWeight(*data++, QUANTIZATION_PRECISION_OUT);

  OUTPUT_BIAS = round(*data++ * QUANTIZATION_PRECISION_OUT);
}
