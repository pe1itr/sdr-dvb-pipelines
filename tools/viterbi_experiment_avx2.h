/* Research-only floating-point ACS kernel, injected into an isolated copy of
 * probe_constellation.c by dvbt_viterbi_experiment.py. No quantization, no
 * reassociation, and the same first-predecessor / first-state tie rules. */
#include <immintrin.h>

static int bench_exp0[64], bench_exp1[64];

__attribute__((target("avx2")))
static void bench_acs_avx2(const float *old_metrics, float *new_metrics,
                           const float cost[4], uint8_t history[8],
                           float *best_metric, int *best_state)
{
    const __m256i order = _mm256_setr_epi32(0,1,4,5,2,3,6,7);
    const __m128 c = _mm_loadu_ps(cost);
    const __m256 costs = _mm256_insertf128_ps(_mm256_castps128_ps256(c),c,1);
    for (unsigned s=0; s<64; s+=8) {
        unsigned p=(s & 31)*2;
        __m256 a=_mm256_loadu_ps(old_metrics+p);
        __m256 b=_mm256_loadu_ps(old_metrics+p+8);
        __m256 even=_mm256_permutevar8x32_ps(_mm256_shuffle_ps(a,b,0x88),order);
        __m256 odd=_mm256_permutevar8x32_ps(_mm256_shuffle_ps(a,b,0xdd),order);
        __m256 c0=_mm256_permutevar8x32_ps(costs,_mm256_loadu_si256((const __m256i *)(bench_exp0+s)));
        __m256 c1=_mm256_permutevar8x32_ps(costs,_mm256_loadu_si256((const __m256i *)(bench_exp1+s)));
        __m256 m0=_mm256_add_ps(even,c0), m1=_mm256_add_ps(odd,c1);
        __m256 choose1=_mm256_cmp_ps(m1,m0,_CMP_LT_OQ);
        _mm256_storeu_ps(new_metrics+s,_mm256_blendv_ps(m0,m1,choose1));
        history[s/8]=(uint8_t)_mm256_movemask_ps(choose1);
    }
    /* Keep scalar reduction order, including ties and the original bound. */
    float best=1.0e30f;
    int state=0;
    for (unsigned s=0; s<64; ++s) {
        if (new_metrics[s]<best) { best=new_metrics[s]; state=(int)s; }
    }
    *best_metric=best;
    *best_state=state;
}
