/*
 *
 *      pcm_params.c
 *      ULSA hardware parameter space: masks, intervals, rules and refinement
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <libs/std/stdarg.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#if CONFIG_AUDIO

/*
 * The parameter space is the one alsa-lib negotiates through HW_REFINE: three
 * bit masks (access, format, subformat) and twelve intervals.  A refine pass
 * first intersects the request with the static constraints of the substream,
 * then applies the dependency rules (frame_bits = sample_bits * channels, ...)
 * until nothing changes any more.  The semantics follow Linux
 * sound/core/pcm_native.c and pcm_lib.c so that alsa-lib's view of the space
 * converges to the same configuration it would get on Linux.
 */

#    define PARAM_BIT(var)   (1U << (var))
#    define MASK_WORDS       (ULSA_MASK_BITS / 32)
#    define IS_MASK(var)     ((var) >= SNDRV_PCM_HW_PARAM_FIRST_MASK && (var) <= SNDRV_PCM_HW_PARAM_LAST_MASK)
#    define IS_INTERVAL(var) ((var) >= SNDRV_PCM_HW_PARAM_FIRST_INTERVAL && (var) <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL)
#    define UINT_MAX_VALUE   0xffffffffU

/* ------------------------------------------------------------- masks */

/* True when no bit is set. */
int ulsa_mask_empty(const struct snd_mask *mask)
{
    for (int i = 0; i < MASK_WORDS; i++)
        if (mask->bits[i]) return 0;
    return 1;
}

/* True when exactly one bit is set. */
int ulsa_mask_single(const struct snd_mask *mask)
{
    int found = 0;

    for (int i = 0; i < MASK_WORDS; i++) {
        uint32_t w = mask->bits[i];
        if (!w) continue;
        if (w & (w - 1)) return 0;
        if (found) return 0;
        found = 1;
    }
    return 1;
}

/* Lowest set bit. */
unsigned int ulsa_mask_min(const struct snd_mask *mask)
{
    for (int i = 0; i < MASK_WORDS; i++)
        if (mask->bits[i]) return (unsigned int)__builtin_ctz(mask->bits[i]) + ((unsigned int)i << 5);
    return 0;
}

/* Highest set bit. */
unsigned int ulsa_mask_max(const struct snd_mask *mask)
{
    for (int i = MASK_WORDS - 1; i >= 0; i--)
        if (mask->bits[i]) return 31U - (unsigned int)__builtin_clz(mask->bits[i]) + ((unsigned int)i << 5);
    return 0;
}

/* Test one bit. */
int ulsa_mask_test(const struct snd_mask *mask, unsigned int val)
{
    if (val >= SNDRV_MASK_MAX) return 0;
    return (mask->bits[val >> 5] >> (val & 31)) & 1U;
}

/* Allow every value the ABI currently defines. */
void ulsa_mask_any(struct snd_mask *mask)
{
    memset(mask, 0, sizeof(*mask));
    for (int i = 0; i < MASK_WORDS; i++) mask->bits[i] = 0xffffffffU;
}

/* Allow nothing. */
void ulsa_mask_none(struct snd_mask *mask)
{
    memset(mask, 0, sizeof(*mask));
}

/* Set one bit. */
void ulsa_mask_set(struct snd_mask *mask, unsigned int val)
{
    if (val < SNDRV_MASK_MAX) mask->bits[val >> 5] |= 1U << (val & 31);
}

/* Clear one bit. */
void ulsa_mask_reset(struct snd_mask *mask, unsigned int val)
{
    if (val < SNDRV_MASK_MAX) mask->bits[val >> 5] &= ~(1U << (val & 31));
}

/* Keep only the given bit. */
static void ulsa_mask_leave(struct snd_mask *mask, unsigned int val)
{
    uint32_t keep = mask->bits[val >> 5] & (1U << (val & 31));

    ulsa_mask_none(mask);
    mask->bits[val >> 5] = keep;
}

/* Intersect mask with v; 1 = changed, 0 = unchanged, -EINVAL = emptied. */
int ulsa_mask_refine(struct snd_mask *mask, const struct snd_mask *v)
{
    int changed = 0;

    for (int i = 0; i < MASK_WORDS; i++) {
        uint32_t n = mask->bits[i] & v->bits[i];
        if (n != mask->bits[i]) changed = 1;
        mask->bits[i] = n;
    }
    if (ulsa_mask_empty(mask)) return -EINVAL;
    return changed;
}

/* Reduce a mask to its lowest value. */
static int ulsa_mask_refine_first(struct snd_mask *mask)
{
    if (ulsa_mask_single(mask)) return 0;
    ulsa_mask_leave(mask, ulsa_mask_min(mask));
    return 1;
}

/* Reduce a mask to its highest value. */
static int ulsa_mask_refine_last(struct snd_mask *mask)
{
    if (ulsa_mask_single(mask)) return 0;
    ulsa_mask_leave(mask, ulsa_mask_max(mask));
    return 1;
}

/* --------------------------------------------------------- intervals */

/* The whole unsigned range. */
void ulsa_interval_any(struct snd_interval *i)
{
    i->min     = 0;
    i->openmin = 0;
    i->max     = UINT_MAX_VALUE;
    i->openmax = 0;
    i->integer = 0;
    i->empty   = 0;
}

/* Mark an interval empty. */
static void ulsa_interval_none(struct snd_interval *i)
{
    i->empty = 1;
}

/* True when the bounds no longer describe any value. */
static int ulsa_interval_checkempty(const struct snd_interval *i)
{
    return i->min > i->max || (i->min == i->max && (i->openmin || i->openmax));
}

/* True when exactly one value remains. */
int ulsa_interval_single(const struct snd_interval *i)
{
    return i->min == i->max || (i->min + 1 == i->max && (i->openmin || i->openmax));
}

/* The value of a single interval. */
int ulsa_interval_value(const struct snd_interval *i)
{
    if (i->openmin && !i->openmax) return (int)i->max;
    return (int)i->min;
}

/* True when val lies inside the interval. */
int ulsa_interval_test(const struct snd_interval *i, unsigned int val)
{
    return !(i->min > val || (i->min == val && i->openmin) || i->max < val || (i->max == val && i->openmax));
}

/* Restrict the interval to integers. */
int ulsa_interval_setinteger(struct snd_interval *i)
{
    if (i->integer) return 0;
    if (i->openmin && i->openmax && i->min == i->max) return -EINVAL;
    i->integer = 1;
    return 1;
}

/* Intersect i with v; 1 = changed, 0 = unchanged, -EINVAL = emptied. */
int ulsa_interval_refine(struct snd_interval *i, const struct snd_interval *v)
{
    int changed = 0;

    if (i->empty) return -EINVAL;
    if (i->min < v->min) {
        i->min     = v->min;
        i->openmin = v->openmin;
        changed    = 1;
    } else if (i->min == v->min && !i->openmin && v->openmin) {
        i->openmin = 1;
        changed    = 1;
    }
    if (i->max > v->max) {
        i->max     = v->max;
        i->openmax = v->openmax;
        changed    = 1;
    } else if (i->max == v->max && !i->openmax && v->openmax) {
        i->openmax = 1;
        changed    = 1;
    }
    if (!i->integer && v->integer) {
        i->integer = 1;
        changed    = 1;
    }
    if (i->integer) {
        if (i->openmin) {
            i->min++;
            i->openmin = 0;
        }
        if (i->openmax) {
            i->max--;
            i->openmax = 0;
        }
    } else if (!i->openmin && !i->openmax && i->min == i->max) {
        i->integer = 1;
    }
    if (ulsa_interval_checkempty(i)) {
        ulsa_interval_none(i);
        return -EINVAL;
    }
    return changed;
}

/* Reduce an interval to its lowest value. */
static int ulsa_interval_refine_first(struct snd_interval *i)
{
    unsigned int last_max = i->max;

    if (i->empty) return -EINVAL;
    if (ulsa_interval_single(i)) return 0;
    i->max = i->min;
    if (i->openmin) i->max++;
    i->openmax = i->openmax && i->max >= last_max;
    return 1;
}

/* Reduce an interval to its highest value. */
static int ulsa_interval_refine_last(struct snd_interval *i)
{
    unsigned int last_min = i->min;

    if (i->empty) return -EINVAL;
    if (ulsa_interval_single(i)) return 0;
    i->min = i->max;
    if (i->openmax) i->min--;
    i->openmin = i->openmin && i->min <= last_min;
    return 1;
}

/* a / b, saturating on a zero divisor. */
static unsigned int div_down(unsigned int a, unsigned int b)
{
    return b ? a / b : UINT_MAX_VALUE;
}

/* ceil(a / b), saturating on a zero divisor. */
static unsigned int div_up(unsigned int a, unsigned int b)
{
    if (!b) return UINT_MAX_VALUE;
    return a / b + (a % b ? 1U : 0U);
}

/* a * b, saturating. */
static unsigned int mul_sat(unsigned int a, unsigned int b)
{
    uint64_t r = (uint64_t)a * b;
    return r > UINT_MAX_VALUE ? UINT_MAX_VALUE : (unsigned int)r;
}

/* a * b / c with remainder, saturating. */
static unsigned int muldiv32(unsigned int a, unsigned int b, unsigned int c, unsigned int *rem)
{
    uint64_t n;

    if (!c) {
        *rem = 0;
        return UINT_MAX_VALUE;
    }
    n    = (uint64_t)a * b;
    *rem = (unsigned int)(n % c);
    n /= c;
    if (n >= UINT_MAX_VALUE) {
        *rem = 0;
        return UINT_MAX_VALUE;
    }
    return (unsigned int)n;
}

/* c = a * b. */
static void interval_mul(const struct snd_interval *a, const struct snd_interval *b, struct snd_interval *c)
{
    if (a->empty || b->empty) {
        ulsa_interval_none(c);
        return;
    }
    c->empty   = 0;
    c->min     = mul_sat(a->min, b->min);
    c->openmin = a->openmin || b->openmin;
    c->max     = mul_sat(a->max, b->max);
    c->openmax = a->openmax || b->openmax;
    c->integer = a->integer && b->integer;
}

/* c = a / b. */
static void interval_div(const struct snd_interval *a, const struct snd_interval *b, struct snd_interval *c)
{
    if (a->empty || b->empty) {
        ulsa_interval_none(c);
        return;
    }
    c->empty   = 0;
    c->min     = div_down(a->min, b->max);
    c->openmin = (b->max && a->min % b->max) || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = div_down(a->max, b->min);
        if (a->max % b->min) {
            c->max++;
            c->openmax = 1;
        } else {
            c->openmax = a->openmax || b->openmin;
        }
    } else {
        c->max     = UINT_MAX_VALUE;
        c->openmax = 0;
    }
    c->integer = 0;
}

/* c = a * b / k. */
static void interval_muldivk(const struct snd_interval *a, const struct snd_interval *b, unsigned int k, struct snd_interval *c)
{
    unsigned int r;

    if (a->empty || b->empty) {
        ulsa_interval_none(c);
        return;
    }
    c->empty   = 0;
    c->min     = muldiv32(a->min, b->min, k, &r);
    c->openmin = r || a->openmin || b->openmin;
    c->max     = muldiv32(a->max, b->max, k, &r);
    if (r) {
        c->max++;
        c->openmax = 1;
    } else {
        c->openmax = a->openmax || b->openmax;
    }
    c->integer = 0;
}

/* c = a * k / b. */
static void interval_mulkdiv(const struct snd_interval *a, unsigned int k, const struct snd_interval *b, struct snd_interval *c)
{
    unsigned int r;

    if (a->empty || b->empty) {
        ulsa_interval_none(c);
        return;
    }
    c->empty   = 0;
    c->min     = muldiv32(a->min, k, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv32(a->max, k, b->min, &r);
        if (r) {
            c->max++;
            c->openmax = 1;
        } else {
            c->openmax = a->openmax || b->openmin;
        }
    } else {
        c->max     = UINT_MAX_VALUE;
        c->openmax = 0;
    }
    c->integer = 0;
}

/* Restrict i to the values of list (optionally filtered by a bit mask). */
int ulsa_interval_list(struct snd_interval *i, unsigned int count, const unsigned int *list, unsigned int mask)
{
    struct snd_interval range;

    if (!count) {
        ulsa_interval_none(i);
        return -EINVAL;
    }
    ulsa_interval_any(&range);
    range.min = UINT_MAX_VALUE;
    range.max = 0;
    for (unsigned int k = 0; k < count; k++) {
        if (mask && (k >= 32 || !(mask & (1U << k)))) continue;
        if (!ulsa_interval_test(i, list[k])) continue;
        if (list[k] < range.min) range.min = list[k];
        if (list[k] > range.max) range.max = list[k];
    }
    return ulsa_interval_refine(i, &range);
}

/* Round the bounds of i onto multiples of step. */
static int ulsa_interval_step(struct snd_interval *i, unsigned int step)
{
    unsigned int n;
    int          changed = 0;

    if (!step) return 0;
    n = i->min % step;
    if (n || i->openmin) {
        i->min += step - n;
        i->openmin = 0;
        changed    = 1;
    }
    n = i->max % step;
    if (n || i->openmax) {
        i->max -= n;
        i->openmax = 0;
        changed    = 1;
    }
    if (ulsa_interval_checkempty(i)) {
        ulsa_interval_none(i);
        return -EINVAL;
    }
    return changed;
}

/* Restrict i to the rates reachable as num / den with the given denominators. */
static int ulsa_interval_ratnum(struct snd_interval *i, unsigned int rats_count, const ulsa_ratnum_t *rats, unsigned int *nump, unsigned int *denp)
{
    unsigned int        best_num = 0, best_den = 0, result_num, result_den;
    int64_t             best_diff = 0, result_diff;
    struct snd_interval t;
    int                 err;

    for (unsigned int k = 0; k < rats_count; k++) {
        unsigned int num = rats[k].num;
        unsigned int q   = i->min ? i->min : 1;
        unsigned int den = div_up(num, q);
        int64_t      diff;

        if (den < rats[k].den_min) continue;
        if (den > rats[k].den_max) {
            den = rats[k].den_max;
        } else if (rats[k].den_step) {
            den -= (den - rats[k].den_min) % rats[k].den_step;
        }
        diff = (int64_t)num - (int64_t)q * den;
        if (diff < 0) diff = -diff;
        if (best_num == 0 || diff * best_den < best_diff * den) {
            best_diff = diff;
            best_den  = den;
            best_num  = num;
        }
    }
    if (!best_den) {
        ulsa_interval_none(i);
        return -EINVAL;
    }
    t.min       = div_down(best_num, best_den);
    t.openmin   = !!(best_num % best_den);
    result_num  = best_num;
    result_den  = best_den;
    result_diff = best_diff;

    best_num = best_den = 0;
    best_diff           = 0;
    for (unsigned int k = 0; k < rats_count; k++) {
        unsigned int num = rats[k].num;
        unsigned int q   = i->max;
        unsigned int den;
        int64_t      diff;

        if (!q) {
            ulsa_interval_none(i);
            return -EINVAL;
        }
        den = div_down(num, q);
        if (den > rats[k].den_max) continue;
        if (den < rats[k].den_min) {
            den = rats[k].den_min;
        } else if (rats[k].den_step) {
            unsigned int r = (den - rats[k].den_min) % rats[k].den_step;
            if (r) den += rats[k].den_step - r;
        }
        diff = (int64_t)q * den - (int64_t)num;
        if (diff < 0) diff = -diff;
        if (best_num == 0 || diff * best_den < best_diff * den) {
            best_diff = diff;
            best_den  = den;
            best_num  = num;
        }
    }
    if (!best_den) {
        ulsa_interval_none(i);
        return -EINVAL;
    }
    t.max     = div_up(best_num, best_den);
    t.openmax = !!(best_num % best_den);
    t.integer = 0;
    t.empty   = 0;
    err       = ulsa_interval_refine(i, &t);
    if (err < 0) return err;

    if (ulsa_interval_single(i)) {
        if (best_diff * result_den < result_diff * best_den) {
            result_num = best_num;
            result_den = best_den;
        }
        if (nump) *nump = result_num;
        if (denp) *denp = result_den;
    }
    return err;
}

/* ----------------------------------------------------- params access */

/* Mask of a parameter. */
struct snd_mask *ulsa_hw_param_mask(struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var)
{
    return &params->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

/* Interval of a parameter. */
struct snd_interval *ulsa_hw_param_interval(struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var)
{
    return &params->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Read-only mask of a parameter. */
const struct snd_mask *ulsa_hw_param_mask_c(const struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var)
{
    return &params->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

/* Read-only interval of a parameter. */
const struct snd_interval *ulsa_hw_param_interval_c(const struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var)
{
    return &params->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Constraint mask of a parameter. */
static struct snd_mask *constrs_mask(ulsa_pcm_hw_constraints_t *c, snd_pcm_hw_param_t var)
{
    return &c->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

/* Constraint interval of a parameter. */
static struct snd_interval *constrs_interval(ulsa_pcm_hw_constraints_t *c, snd_pcm_hw_param_t var)
{
    return &c->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Reset a parameter block to "anything goes". */
void ulsa_pcm_hw_params_any(struct snd_pcm_hw_params *params)
{
    memset(params, 0, sizeof(*params));
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++) {
        ulsa_mask_any(ulsa_hw_param_mask(params, k));
        params->cmask |= PARAM_BIT(k);
        params->rmask |= PARAM_BIT(k);
    }
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) {
        ulsa_interval_any(ulsa_hw_param_interval(params, k));
        params->cmask |= PARAM_BIT(k);
        params->rmask |= PARAM_BIT(k);
    }
    params->info = ~0U;
}

/* Fixed access type. */
snd_pcm_access_t ulsa_params_access(const struct snd_pcm_hw_params *p)
{
    return (snd_pcm_access_t)ulsa_mask_min(ulsa_hw_param_mask_c(p, SNDRV_PCM_HW_PARAM_ACCESS));
}

/* Fixed sample format. */
snd_pcm_format_t ulsa_params_format(const struct snd_pcm_hw_params *p)
{
    return (snd_pcm_format_t)ulsa_mask_min(ulsa_hw_param_mask_c(p, SNDRV_PCM_HW_PARAM_FORMAT));
}

/* Fixed subformat. */
snd_pcm_subformat_t ulsa_params_subformat(const struct snd_pcm_hw_params *p)
{
    return (snd_pcm_subformat_t)ulsa_mask_min(ulsa_hw_param_mask_c(p, SNDRV_PCM_HW_PARAM_SUBFORMAT));
}

/* Fixed channel count. */
unsigned int ulsa_params_channels(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_CHANNELS)->min;
}

/* Fixed rate. */
unsigned int ulsa_params_rate(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_RATE)->min;
}

/* Fixed period size in frames. */
unsigned int ulsa_params_period_size(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
}

/* Fixed period size in bytes. */
size_t ulsa_params_period_bytes(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min;
}

/* Fixed period count. */
unsigned int ulsa_params_periods(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_PERIODS)->min;
}

/* Fixed buffer size in frames. */
unsigned int ulsa_params_buffer_size(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
}

/* Fixed buffer size in bytes. */
size_t ulsa_params_buffer_bytes(const struct snd_pcm_hw_params *p)
{
    return ulsa_hw_param_interval_c(p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->min;
}

/* ------------------------------------------------------------- rules */

/* Register a dependency rule; dep... is a -1 terminated list of parameters. */
int ulsa_pcm_hw_rule_add(ulsa_pcm_runtime_t *runtime, unsigned int cond, int var, ulsa_pcm_hw_rule_func_t func, void *private, int dep, ...)
{
    ulsa_pcm_hw_constraints_t *c = &runtime->hw_constraints;
    ulsa_pcm_hw_rule_t        *r;
    va_list                    args;
    unsigned int               k = 0;

    if (c->rules_num >= c->rules_all) {
        unsigned int        n     = c->rules_all + 16;
        ulsa_pcm_hw_rule_t *rules = malloc(n * sizeof(*rules));
        if (!rules) return -ENOMEM;
        if (c->rules) {
            memcpy(rules, c->rules, c->rules_num * sizeof(*rules));
            free(c->rules);
        }
        c->rules     = rules;
        c->rules_all = n;
    }
    r          = &c->rules[c->rules_num];
    r->cond    = cond;
    r->func    = func;
    r->var     = var;
    r->private = private;

    va_start(args, dep);
    for (;;) {
        if (k >= sizeof(r->deps) / sizeof(r->deps[0])) {
            va_end(args);
            return -EINVAL;
        }
        r->deps[k++] = dep;
        if (dep < 0) break;
        dep = va_arg(args, int);
    }
    va_end(args);
    c->rules_num++;
    return EOK;
}

/* var = deps[0] * deps[1] */
static int rule_mul(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    struct snd_interval t;
    interval_mul(ulsa_hw_param_interval_c(params, rule->deps[0]), ulsa_hw_param_interval_c(params, rule->deps[1]), &t);
    return ulsa_interval_refine(ulsa_hw_param_interval(params, rule->var), &t);
}

/* var = deps[0] / deps[1] */
static int rule_div(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    struct snd_interval t;
    interval_div(ulsa_hw_param_interval_c(params, rule->deps[0]), ulsa_hw_param_interval_c(params, rule->deps[1]), &t);
    return ulsa_interval_refine(ulsa_hw_param_interval(params, rule->var), &t);
}

/* var = deps[0] * deps[1] / k */
static int rule_muldivk(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    struct snd_interval t;
    interval_muldivk(ulsa_hw_param_interval_c(params, rule->deps[0]), ulsa_hw_param_interval_c(params, rule->deps[1]), (unsigned int)(uintptr_t)rule->private, &t);
    return ulsa_interval_refine(ulsa_hw_param_interval(params, rule->var), &t);
}

/* var = deps[0] * k / deps[1] */
static int rule_mulkdiv(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    struct snd_interval t;
    interval_mulkdiv(ulsa_hw_param_interval_c(params, rule->deps[0]), (unsigned int)(uintptr_t)rule->private, ulsa_hw_param_interval_c(params, rule->deps[1]), &t);
    return ulsa_interval_refine(ulsa_hw_param_interval(params, rule->var), &t);
}

/* Drop formats whose physical width falls outside sample_bits. */
static int rule_format(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    const struct snd_interval *i    = ulsa_hw_param_interval_c(params, rule->deps[0]);
    struct snd_mask           *mask = ulsa_hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
    struct snd_mask            m;

    ulsa_mask_any(&m);
    for (int k = 0; k <= SNDRV_PCM_FORMAT_LAST; k++) {
        int bits;
        if (!ulsa_mask_test(mask, (unsigned int)k)) continue;
        bits = ulsa_pcm_format_physical_width(k);
        if (bits <= 0) continue;
        if ((unsigned int)bits < i->min || (unsigned int)bits > i->max) ulsa_mask_reset(&m, (unsigned int)k);
    }
    return ulsa_mask_refine(mask, &m);
}

/* sample_bits = the span of physical widths of the remaining formats. */
static int rule_sample_bits(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    const struct snd_mask *mask = ulsa_hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);
    struct snd_interval    t;

    ulsa_interval_any(&t);
    t.min = UINT_MAX_VALUE;
    t.max = 0;
    for (int k = 0; k <= SNDRV_PCM_FORMAT_LAST; k++) {
        int bits;
        if (!ulsa_mask_test(mask, (unsigned int)k)) continue;
        bits = ulsa_pcm_format_physical_width(k);
        if (bits <= 0) continue;
        if (t.min > (unsigned int)bits) t.min = (unsigned int)bits;
        if (t.max < (unsigned int)bits) t.max = (unsigned int)bits;
    }
    t.integer = 1;
    return ulsa_interval_refine(ulsa_hw_param_interval(params, rule->var), &t);
}

/* Rates of the ULSA_PCM_RATE_* bits, in bit order. */
static const unsigned int known_rates[] = {5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000, 12000, 24000, 128000};

/* Restrict the rate to the hardware's ULSA_PCM_RATE_* bits. */
static int rule_rate(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    const ulsa_pcm_hardware_t *hw = rule->private;
    if (!hw->rates) {
        ulsa_interval_none(ulsa_hw_param_interval(params, rule->var));
        return -EINVAL;
    }
    return ulsa_interval_list(ulsa_hw_param_interval(params, rule->var), sizeof(known_rates) / sizeof(known_rates[0]), known_rates, hw->rates);
}

/* Cap buffer_bytes at what the substream can allocate. */
static int rule_buffer_bytes_max(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    ulsa_pcm_substream_t *substream = rule->private;
    struct snd_interval   t;

    ulsa_interval_any(&t);
    t.max     = substream->buffer_bytes_max > UINT_MAX_VALUE ? UINT_MAX_VALUE : (unsigned int)substream->buffer_bytes_max;
    t.integer = 1;
    return ulsa_interval_refine(ulsa_hw_param_interval(params, rule->var), &t);
}

/* Subformats are STD plus MSBITS_MAX for linear formats (and driver-provided S32 widths). */
static int rule_subformats(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    struct snd_mask       *sfmask     = ulsa_hw_param_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT);
    const struct snd_mask *fmask      = ulsa_hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);
    const uint32_t        *subformats = rule->private;
    struct snd_mask        m;

    ulsa_mask_none(&m);
    ulsa_mask_set(&m, SNDRV_PCM_SUBFORMAT_STD);
    for (int f = 0; f <= SNDRV_PCM_FORMAT_LAST; f++) {
        if (!ulsa_mask_test(fmask, (unsigned int)f)) continue;
        if (f == SNDRV_PCM_FORMAT_S32_LE && *subformats) {
            m.bits[0] |= *subformats;
        } else if (ulsa_pcm_format_linear(f)) {
            ulsa_mask_set(&m, SNDRV_PCM_SUBFORMAT_MSBITS_MAX);
        }
    }
    return ulsa_mask_refine(sfmask, &m);
}

/* Restrict var to a list. */
static int rule_list(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    const ulsa_pcm_hw_constraint_list_t *list = rule->private;
    return ulsa_interval_list(ulsa_hw_param_interval(params, rule->var), list->count, list->list, list->mask);
}

/* Restrict var to num/den combinations. */
static int rule_ratnums(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    const ulsa_pcm_hw_constraint_ratnums_t *r   = rule->private;
    unsigned int                            num = 0, den = 0;
    int                                     err;

    err = ulsa_interval_ratnum(ulsa_hw_param_interval(params, rule->var), r->nrats, r->rats, &num, &den);
    if (err >= 0 && den && rule->var == SNDRV_PCM_HW_PARAM_RATE) {
        params->rate_num = num;
        params->rate_den = den;
    }
    return err;
}

/* Round var onto multiples of a step. */
static int rule_step(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    return ulsa_interval_step(ulsa_hw_param_interval(params, rule->var), (unsigned int)(uintptr_t)rule->private);
}

/* Restrict var to powers of two. */
static int rule_pow2(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    static const unsigned int pow2[] = {1U << 0,  1U << 1,  1U << 2,  1U << 3,  1U << 4,  1U << 5,  1U << 6,  1U << 7,  1U << 8,  1U << 9,  1U << 10, 1U << 11, 1U << 12, 1U << 13, 1U << 14, 1U << 15,
                                        1U << 16, 1U << 17, 1U << 18, 1U << 19, 1U << 20, 1U << 21, 1U << 22, 1U << 23, 1U << 24, 1U << 25, 1U << 26, 1U << 27, 1U << 28, 1U << 29, 1U << 30};
    return ulsa_interval_list(ulsa_hw_param_interval(params, rule->var), sizeof(pow2) / sizeof(pow2[0]), pow2, 0);
}

/* Report the significant bits for formats of the given width. */
static int rule_msbits(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule)
{
    unsigned int               l      = (unsigned int)(uintptr_t)rule->private;
    unsigned int               width  = l & 0xffff;
    unsigned int               msbits = l >> 16;
    const struct snd_interval *i      = ulsa_hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);

    if (!ulsa_interval_single(i)) return 0;
    if (ulsa_interval_value(i) == (int)width || (width == 0 && ulsa_interval_value(i) > (int)msbits)) {
        if (!params->msbits || msbits < params->msbits) params->msbits = msbits;
    }
    return 0;
}

/* --------------------------------------------- constraint helpers */

/* Intersect a mask constraint with a 64-bit value set. */
int ulsa_pcm_hw_constraint_mask64(ulsa_pcm_runtime_t *runtime, snd_pcm_hw_param_t var, uint64_t mask)
{
    struct snd_mask *m = constrs_mask(&runtime->hw_constraints, var);

    m->bits[0] &= (uint32_t)mask;
    m->bits[1] &= (uint32_t)(mask >> 32);
    memset(m->bits + 2, 0, sizeof(m->bits) - 2 * sizeof(m->bits[0]));
    if (!m->bits[0] && !m->bits[1]) return -EINVAL;
    return EOK;
}

/* Require integral values of an interval. */
int ulsa_pcm_hw_constraint_integer(ulsa_pcm_runtime_t *runtime, snd_pcm_hw_param_t var)
{
    return ulsa_interval_setinteger(constrs_interval(&runtime->hw_constraints, var));
}

/* Clamp an interval to [min, max]. */
int ulsa_pcm_hw_constraint_minmax(ulsa_pcm_runtime_t *runtime, snd_pcm_hw_param_t var, unsigned int min, unsigned int max)
{
    struct snd_interval t;

    ulsa_interval_any(&t);
    t.min = min;
    t.max = max;
    return ulsa_interval_refine(constrs_interval(&runtime->hw_constraints, var), &t);
}

/* Restrict an interval to a list of values. */
int ulsa_pcm_hw_constraint_list(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var, const ulsa_pcm_hw_constraint_list_t *list)
{
    return ulsa_pcm_hw_rule_add(runtime, cond, var, rule_list, (void *)list, var, -1);
}

/* Restrict an interval to num/den combinations. */
int ulsa_pcm_hw_constraint_ratnums(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var, const ulsa_pcm_hw_constraint_ratnums_t *r)
{
    return ulsa_pcm_hw_rule_add(runtime, cond, var, rule_ratnums, (void *)r, var, -1);
}

/* Restrict an interval to multiples of step. */
int ulsa_pcm_hw_constraint_step(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var, unsigned long step)
{
    return ulsa_pcm_hw_rule_add(runtime, cond, var, rule_step, (void *)(uintptr_t)step, var, -1);
}

/* Restrict an interval to powers of two. */
int ulsa_pcm_hw_constraint_pow2(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var)
{
    return ulsa_pcm_hw_rule_add(runtime, cond, var, rule_pow2, NULL, var, -1);
}

/* Report msbits for formats of the given physical width (0 = any wider format). */
int ulsa_pcm_hw_constraint_msbits(ulsa_pcm_runtime_t *runtime, unsigned int cond, unsigned int width, unsigned int msbits)
{
    uintptr_t l = (uintptr_t)((msbits << 16) | width);
    return ulsa_pcm_hw_rule_add(runtime, cond, -1, rule_msbits, (void *)l, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, -1);
}

/* ------------------------------------------- open-time constraints */

/* Install the generic constraints and the rules tying the parameters together. */
int ulsa_pcm_hw_constraints_init(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t        *runtime = substream->runtime;
    ulsa_pcm_hw_constraints_t *c       = &runtime->hw_constraints;
    int                        err     = 0;

    for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++) ulsa_mask_any(constrs_mask(c, k));
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) ulsa_interval_any(constrs_interval(c, k));

    ulsa_interval_setinteger(constrs_interval(c, SNDRV_PCM_HW_PARAM_CHANNELS));
    ulsa_interval_setinteger(constrs_interval(c, SNDRV_PCM_HW_PARAM_BUFFER_SIZE));
    ulsa_interval_setinteger(constrs_interval(c, SNDRV_PCM_HW_PARAM_BUFFER_BYTES));
    ulsa_interval_setinteger(constrs_interval(c, SNDRV_PCM_HW_PARAM_SAMPLE_BITS));
    ulsa_interval_setinteger(constrs_interval(c, SNDRV_PCM_HW_PARAM_FRAME_BITS));

    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FORMAT, rule_format, NULL, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, rule_sample_bits, NULL, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, rule_div, NULL, SNDRV_PCM_HW_PARAM_FRAME_BITS, SNDRV_PCM_HW_PARAM_CHANNELS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FRAME_BITS, rule_mul, NULL, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, SNDRV_PCM_HW_PARAM_CHANNELS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FRAME_BITS, rule_mulkdiv, (void *)8, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FRAME_BITS, rule_mulkdiv, (void *)8, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, rule_div, NULL, SNDRV_PCM_HW_PARAM_FRAME_BITS, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, rule_mulkdiv, (void *)1000000, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_PERIOD_TIME, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, rule_mulkdiv, (void *)1000000, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_BUFFER_TIME, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIODS, rule_div, NULL, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, rule_div, NULL, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIODS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, rule_mulkdiv, (void *)8, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, SNDRV_PCM_HW_PARAM_FRAME_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, rule_muldivk, (void *)1000000, SNDRV_PCM_HW_PARAM_PERIOD_TIME, SNDRV_PCM_HW_PARAM_RATE, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, rule_mul, NULL, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_PERIODS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, rule_mulkdiv, (void *)8, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, SNDRV_PCM_HW_PARAM_FRAME_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, rule_muldivk, (void *)1000000, SNDRV_PCM_HW_PARAM_BUFFER_TIME, SNDRV_PCM_HW_PARAM_RATE, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, rule_muldivk, (void *)8, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_FRAME_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, rule_muldivk, (void *)8, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_FRAME_BITS, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_TIME, rule_mulkdiv, (void *)1000000, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, SNDRV_PCM_HW_PARAM_RATE, -1);
    err |= ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_TIME, rule_mulkdiv, (void *)1000000, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_RATE, -1);
    return err ? -ENOMEM : EOK;
}

/* Apply runtime->hw (filled by the driver's open) on top of the generic constraints. */
int ulsa_pcm_hw_constraints_complete(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t  *runtime = substream->runtime;
    ulsa_pcm_hardware_t *hw      = &runtime->hw;
    uint64_t             access  = 0;
    int                  err;

    if (hw->info & SNDRV_PCM_INFO_INTERLEAVED) access |= 1ULL << SNDRV_PCM_ACCESS_RW_INTERLEAVED;
    if (hw->info & SNDRV_PCM_INFO_NONINTERLEAVED) access |= 1ULL << SNDRV_PCM_ACCESS_RW_NONINTERLEAVED;
    if (ulsa_pcm_mmap_supported(substream)) {
        if (hw->info & SNDRV_PCM_INFO_INTERLEAVED) access |= 1ULL << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
        if (hw->info & SNDRV_PCM_INFO_NONINTERLEAVED) access |= 1ULL << SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED;
        if (hw->info & SNDRV_PCM_INFO_COMPLEX) access |= 1ULL << SNDRV_PCM_ACCESS_MMAP_COMPLEX;
    }
    err = ulsa_pcm_hw_constraint_mask64(runtime, SNDRV_PCM_HW_PARAM_ACCESS, access);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_mask64(runtime, SNDRV_PCM_HW_PARAM_FORMAT, hw->formats);
    if (err < 0) return err;
    err = ulsa_pcm_hw_rule_add(runtime, 0, -1, rule_subformats, &hw->subformats, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_HW_PARAM_FORMAT, -1);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_CHANNELS, hw->channels_min, hw->channels_max);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_RATE, hw->rate_min, hw->rate_max);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, (unsigned int)hw->period_bytes_min, (unsigned int)hw->period_bytes_max);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIODS, hw->periods_min, hw->periods_max);
    if (err < 0) return err;
    err = ulsa_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, (unsigned int)hw->period_bytes_min, (unsigned int)hw->buffer_bytes_max);
    if (err < 0) return err;
    err = ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, rule_buffer_bytes_max, substream, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, -1);
    if (err < 0) return err;
    if (!(hw->rates & (ULSA_PCM_RATE_KNOT | ULSA_PCM_RATE_CONTINUOUS))) {
        err = ulsa_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, rule_rate, hw, SNDRV_PCM_HW_PARAM_RATE, -1);
        if (err < 0) return err;
    }
    return ulsa_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) < 0 ? -EINVAL : EOK;
}

/* Release the rule table of a runtime. */
void ulsa_pcm_hw_constraints_free(ulsa_pcm_runtime_t *runtime)
{
    free(runtime->hw_constraints.rules);
    runtime->hw_constraints.rules     = NULL;
    runtime->hw_constraints.rules_num = 0;
    runtime->hw_constraints.rules_all = 0;
}

/* -------------------------------------------------------- refinement */

/* Intersect each requested mask with its static constraint. */
static int constrain_masks(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    ulsa_pcm_hw_constraints_t *c = &substream->runtime->hw_constraints;

    for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++) {
        struct snd_mask *m = ulsa_hw_param_mask(params, k);
        int              changed;

        if (ulsa_mask_empty(m)) return -EINVAL;
        if (!(params->rmask & PARAM_BIT(k))) continue;
        changed = ulsa_mask_refine(m, constrs_mask(c, k));
        if (changed < 0) return changed;
        if (changed) params->cmask |= PARAM_BIT(k);
    }
    return EOK;
}

/* Intersect each requested interval with its static constraint. */
static int constrain_intervals(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    ulsa_pcm_hw_constraints_t *c = &substream->runtime->hw_constraints;

    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) {
        struct snd_interval *i = ulsa_hw_param_interval(params, k);
        int                  changed;

        if (i->empty) return -EINVAL;
        if (!(params->rmask & PARAM_BIT(k))) continue;
        changed = ulsa_interval_refine(i, constrs_interval(c, k));
        if (changed < 0) return changed;
        if (changed) params->cmask |= PARAM_BIT(k);
    }
    return EOK;
}

/*
 * Apply the rules until the space stops shrinking.  A rule is only re-run when
 * one of its dependencies changed after its own last application; parameters
 * the caller did not request start out "old" and therefore never trigger.
 */
static int constrain_by_rules(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    ulsa_pcm_hw_constraints_t *c = &substream->runtime->hw_constraints;
    unsigned int               vstamps[SNDRV_PCM_HW_PARAM_LAST_INTERVAL + 1];
    unsigned int              *rstamps;
    unsigned int               stamp = 2;
    bool                       again;

    rstamps = calloc(c->rules_num ? c->rules_num : 1, sizeof(*rstamps));
    if (!rstamps) return -ENOMEM;
    for (int k = 0; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) vstamps[k] = (params->rmask & PARAM_BIT(k)) ? 1 : 0;

    do {
        again = false;
        for (unsigned int k = 0; k < c->rules_num; k++) {
            ulsa_pcm_hw_rule_t *r = &c->rules[k];
            unsigned int        d;
            int                 changed;

            if (r->cond && !(r->cond & params->flags)) continue;
            for (d = 0; r->deps[d] >= 0; d++)
                if (vstamps[r->deps[d]] > rstamps[k]) break;
            if (r->deps[d] < 0) continue;

            changed = r->func(params, r);
            if (changed < 0) {
                free(rstamps);
                return changed;
            }
            if (changed && r->var >= 0) {
                params->cmask |= PARAM_BIT(r->var);
                vstamps[r->var] = stamp;
                again           = true;
            }
            rstamps[k] = stamp++;
        }
    } while (again);

    free(rstamps);
    return EOK;
}

/* Refine params against the substream's constraints (HW_REFINE semantics). */
int ulsa_pcm_hw_refine(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    int err;

    params->info      = 0;
    params->fifo_size = 0;
    if (params->rmask & PARAM_BIT(SNDRV_PCM_HW_PARAM_SAMPLE_BITS)) params->msbits = 0;
    if (params->rmask & PARAM_BIT(SNDRV_PCM_HW_PARAM_RATE)) {
        params->rate_num = 0;
        params->rate_den = 0;
    }
    err = constrain_masks(substream, params);
    if (err < 0) return err;
    err = constrain_intervals(substream, params);
    if (err < 0) return err;
    err = constrain_by_rules(substream, params);
    if (err < 0) return err;
    params->rmask = 0;
    return EOK;
}

/* Fix var to its first (or last) value and refine the rest of the space. */
static int param_pin(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var, bool last)
{
    int changed;

    if (IS_MASK(var)) {
        struct snd_mask *m = ulsa_hw_param_mask(params, var);
        changed            = last ? ulsa_mask_refine_last(m) : ulsa_mask_refine_first(m);
    } else if (IS_INTERVAL(var)) {
        struct snd_interval *i = ulsa_hw_param_interval(params, var);
        changed                = last ? ulsa_interval_refine_last(i) : ulsa_interval_refine_first(i);
    } else {
        return -EINVAL;
    }
    if (changed < 0) return changed;
    if (changed > 0) {
        params->cmask |= PARAM_BIT(var);
        params->rmask |= PARAM_BIT(var);
    }
    if (params->rmask) {
        int err = ulsa_pcm_hw_refine(substream, params);
        if (err < 0) return err;
    }
    return changed;
}

/*
 * Pick one configuration out of the refined space, in the Linux order: first
 * access, format, subformat, min channels, min rate, min period time, max
 * buffer size, min tick time.
 */
int ulsa_pcm_hw_params_choose(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    static const int vars[] = {SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_HW_PARAM_FORMAT,      SNDRV_PCM_HW_PARAM_SUBFORMAT,   SNDRV_PCM_HW_PARAM_CHANNELS,
                               SNDRV_PCM_HW_PARAM_RATE,   SNDRV_PCM_HW_PARAM_PERIOD_TIME, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_TICK_TIME};

    for (size_t k = 0; k < sizeof(vars) / sizeof(vars[0]); k++) {
        int err = param_pin(substream, params, vars[k], vars[k] == SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
        if (err < 0) return err;
    }
    return EOK;
}

/* Fill the informational fields (msbits, rate_num/den, fifo_size, info) a refine leaves out. */
int ulsa_pcm_fixup_unreferenced_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params)
{
    ulsa_pcm_runtime_t        *runtime = substream->runtime;
    const struct snd_interval *i;
    const struct snd_mask     *m;

    if (!params->msbits) {
        i = ulsa_hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS);
        if (ulsa_interval_single(i)) params->msbits = (unsigned int)ulsa_interval_value(i);
        m = ulsa_hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);
        if (ulsa_mask_single(m)) {
            int width = ulsa_pcm_format_width((snd_pcm_format_t)ulsa_mask_min(m));
            if (width > 0) params->msbits = (unsigned int)width;
        }
    }
    if (params->msbits) {
        m = ulsa_hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);
        if (ulsa_mask_single(m)) {
            snd_pcm_format_t format = (snd_pcm_format_t)ulsa_mask_min(m);
            if (ulsa_pcm_format_linear(format) && ulsa_pcm_format_width(format) != (int)params->msbits) {
                struct snd_mask *sf = ulsa_hw_param_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT);
                ulsa_mask_reset(sf, SNDRV_PCM_SUBFORMAT_MSBITS_MAX);
                if (ulsa_mask_empty(sf)) return -EINVAL;
            }
        }
    }
    if (!params->rate_den) {
        i = ulsa_hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_RATE);
        if (ulsa_interval_single(i)) {
            params->rate_num = (unsigned int)ulsa_interval_value(i);
            params->rate_den = 1;
        }
    }
    if (!params->fifo_size) {
        m = ulsa_hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);
        i = ulsa_hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS);
        if (ulsa_mask_single(m) && ulsa_interval_single(i)) {
            params->fifo_size = runtime->hw.fifo_size;
            if (!(runtime->hw.info & SNDRV_PCM_INFO_FIFO_IN_FRAMES)) {
                int    width = ulsa_pcm_format_physical_width((snd_pcm_format_t)ulsa_mask_min(m));
                size_t frame = width > 0 ? (size_t)width * (unsigned int)ulsa_interval_value(i) / 8 : 0;
                if (frame) params->fifo_size /= frame;
            }
        }
    }
    if (!params->info) {
        params->info = runtime->hw.info & ~(SNDRV_PCM_INFO_FIFO_IN_FRAMES | SNDRV_PCM_INFO_DRAIN_TRIGGER);
        if (!ulsa_pcm_mmap_supported(substream)) params->info &= ~(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID);
    }
    return EOK;
}

#endif
