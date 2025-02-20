/*
 *
 *		math.h
 *		数学处理的内联函数库头文件
 *
 *		2024/10/2 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_MATH_H_
#define INCLUDE_MATH_H_

#define _X86_

#define M_E				2.7182818284590452354
#define M_LOG2E			1.4426950408889634074
#define M_LOG10E		0.43429448190325182765
#define M_LN2			0.69314718055994530942
#define M_LN10			2.30258509299404568402
#define M_PI			3.14159265358979323846
#define M_PI_2			1.57079632679489661923
#define M_PI_4			0.78539816339744830962
#define M_1_PI			0.31830988618379067154
#define M_2_PI			0.63661977236758134308
#define M_2_SQRTPI		1.12837916709551257390
#define M_SQRT2			1.41421356237309504880
#define M_SQRT1_2		0.70710678118654752440

/* IEEE 754 classication */
#define _FPCLASS_SNAN	0x0001 /* Signaling "Not a Number" */
#define _FPCLASS_QNAN	0x0002 /* Quiet "Not a Number" */
#define _FPCLASS_NINF	0x0004 /* Negative Infinity */
#define _FPCLASS_NN		0x0008 /* Negative Normal */
#define _FPCLASS_ND		0x0010 /* Negative Denormal */
#define _FPCLASS_NZ		0x0020 /* Negative Zero */
#define _FPCLASS_PZ		0x0040 /* Positive Zero */
#define _FPCLASS_PD		0x0080 /* Positive Denormal */
#define _FPCLASS_PN		0x0100 /* Positive Normal */
#define _FPCLASS_PINF	0x0200 /* Positive Infinity */

double sin(double x);
double cos(double x);
double tan(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);
double sqrt(double x);
double ceil(double x);
double floor(double x);

/* 7.12.7.2 The fabs functions: Double in C89 */
float fabsf(float x);
long double fabsl(long double x);
double fabs(double x);

double ldexp(double x,int y);
double frexp(double x,int *y);
double modf(double x,double *y);
double fmod(double x,double y);

void sincos(double x, double *p_sin, double *p_cos);
void sincosl(long double x, long double *p_sin, long double *p_cos);
void sincosf(float x, float *p_sin, float *p_cos);

int abs(int x);
long labs(long x);

double atof(const char *str);
// double _atof_l(const char *string,_locale_t _Locale);

#define EDOM 33
#define ERANGE 34

// double _cabs(struct _complex _ComplexA); Overridden to use our cabs.
double _hypot(double x,double y);
double _j0(double x);
double _j1(double x);
double _jn(int x,double y);
double y0(double x);
double y1(double x);
double yn(int x,double y);

// int _matherr(struct _exception *);

double _chgsign(double x);
double _copysign(double _Number,double _Sign);
double _logb(double);
double _nextafter(double, double);
double _scalb(double, long);
int _finite(double);
int _fpclass(double);
int _isnan(double);

double chgsign (double);

int finit(double);
int fpclass(double);

#define FP_SNAN		_FPCLASS_SNAN
#define FP_QNAN		_FPCLASS_QNAN
#define FP_NINF		_FPCLASS_NINF
#define FP_PINF		_FPCLASS_PINF
#define FP_NDENORM	_FPCLASS_ND
#define FP_PDENORM	_FPCLASS_PD
#define FP_NZERO	_FPCLASS_NZ
#define FP_PZERO	_FPCLASS_PZ
#define FP_NNORM	_FPCLASS_NN
#define FP_PNORM	_FPCLASS_PN

#define HUGE_VALF	__builtin_huge_valf()
#define HUGE_VALL	__builtin_huge_vall()
#define INFINITY	__builtin_inff()
#define NAN			__builtin_nanf("")

/* 7.12.3.1 */
/*
	Return values for fpclassify.
	These are based on Intel x87 fpu condition codes
	in the high byte of status word and differ from
	the return values for MS IEEE 754 extension _fpclass()
*/
#define FP_NAN			0x0100
#define FP_NORMAL		0x0400
#define FP_INFINITE		(FP_NAN | FP_NORMAL)
#define FP_ZERO			0x4000
#define FP_SUBNORMAL	(FP_NORMAL | FP_ZERO)
/* 0x0200 is signbit mask */

/*
	We can't inline float or double, because we want to ensure truncation
	to semantic type before classification. 
	(A normal long double value might become subnormal when 
	converted to double, and zero when converted to float.)
*/

int __fpclassifyl(long double x);
int __fpclassifyf(float);
int __fpclassify(double);

/* 7.12.3.4 */
/* We don't need to worry about truncation here: A NaN stays a NaN. */

int __isnan(double);
int __isnanf(float);
int __isnanl(long double x);

int __signbit(double);
int __signbitf(float);
int __signbitl(long double x);

/* 7.12.4 Trigonometric functions: Double in C89 */
float sinf(float x);
long double sinl(long double x);

float cosf(float x);
long double cosl(long double x);

float tanf(float x);
long double tanl(long double x);
float asinf(float x);
long double asinl(long double x);

float acosf(float);
long double acosl(long double x);

float atanf(float);
long double atanl(long double x);

float atan2f(float, float);
long double atan2l(long double, long double);

/* 7.12.5 Hyperbolic functions: Double in C89 */
float sinhf(float x);
long double sinhl(long double x);
float coshf(float x);
long double coshl(long double x);
float tanhf(float x);
long double tanhl(long double x);

/* Inverse hyperbolic trig functions */ 
/* 7.12.5.1 */
double acosh(double);
float acoshf(float);
long double acoshl(long double x);

/* 7.12.5.2 */
double asinh(double);
float asinhf(float);
long double asinhl(long double x);

/* 7.12.5.3 */
double atanh(double);
float atanhf(float);
long double atanhl(long double x);

/* Exponentials and logarithms	*/
/* 7.12.6.1 Double in C89 */
float expf(float x);
long double expl(long double x);

/* 7.12.6.2 */
double exp2(double);
float exp2f(float);
long double exp2l(long double x);

/* 7.12.6.3 The expm1 functions */
/* TODO: These could be inlined */
double expm1(double);
float expm1f(float);
long double expm1l(long double x);

/* 7.12.6.4 Double in C89 */
float frexpf(float x,int *y);
long double frexpl(long double,int *);

#define FP_ILOGB0 ((int)0x80000000)
#define FP_ILOGBNAN ((int)0x7fffffff)

int ilogb(double);
int ilogbf(float);
int ilogbl(long double x);

/* 7.12.6.6	Double in C89 */
float ldexpf(float x,int y);
long double ldexpl(long double, int);

/* 7.12.6.7 Double in C89 */
float logf(float);
long double logl(long double x);

/* 7.12.6.8 Double in C89 */
float log10f(float);
long double log10l(long double x);

/* 7.12.6.9 */
double log1p(double);
float log1pf(float);
long double log1pl(long double x);

/* 7.12.6.10 */
double log2(double);
float log2f(float);
long double log2l(long double x);

/* 7.12.6.11 */
double logb(double);
float logbf(float);
long double logbl(long double x);

/* 7.12.6.12 Double in C89 */
float modff(float, float*);
long double modfl(long double, long double*);

/* 7.12.6.13 */
double scalbn(double, int);
float scalbnf(float, int);
long double scalbnl(long double, int);

double scalbln(double, long);
float scalblnf(float, long);
long double scalblnl(long double, long);

/* 7.12.7.1 */
/* Implementations adapted from Cephes versions */ 
double cbrt(double);
float cbrtf(float);
long double cbrtl(long double x);

/* 7.12.7.3 */
double hypot(double, double); // in libmoldname.a
float hypotf(float x, float y);
long double hypotl(long double, long double);

/* 7.12.7.4 The pow functions. Double in C89 */
float powf(float x,float y);
long double powl(long double, long double);

/* 7.12.7.5 The sqrt functions. Double in C89. */
float sqrtf(float);
long double sqrtl(long double x);

/* 7.12.8.1 The erf functions */
double erf(double);
float erff(float);
long double erfl(long double x);

/* 7.12.8.2 The erfc functions */
double erfc(double);
float erfcf(float);
long double erfcl(long double x);

/* 7.12.8.3 The lgamma functions */
double lgamma(double);
float lgammaf(float);
long double lgammal(long double x);
int signgam;

/* 7.12.8.4 The tgamma functions */
double tgamma(double);
float tgammaf(float);
long double tgammal(long double x);

/* 7.12.9.1 Double in C89 */
float ceilf(float);
long double ceill(long double x);

/* 7.12.9.2 Double in C89 */
float floorf(float);
long double floorl(long double x);

/* 7.12.9.3 */
double nearbyint( double);
float nearbyintf(float);
long double nearbyintl(long double x);

/* 7.12.9.4 */
/* round, using fpu control word settings */
double rint(double);
float rintf(float);
long double rintl(long double x);

/* 7.12.9.5 */
long lrint (double);
long lrintf (float);
long lrintl (long double x);

long long llrint(double);
long long llrintf(float);
long long llrintl(long double x);

/* 7.12.9.6 */
/* round away from zero, regardless of fpu control word settings */
double round(double);
float roundf(float);
long double roundl(long double x);

/* 7.12.9.7 */
long lround(double);
long lroundf(float);
long lroundl(long double x);
long long llround(double);
long long llroundf(float);
long long llroundl(long double x);

/* 7.12.9.8 */
/* round towards zero, regardless of fpu control word settings */
double trunc(double);
float truncf(float);
long double truncl(long double x);

/* 7.12.10.1 Double in C89 */
float fmodf(float, float);
long double fmodl(long double, long double);

/* 7.12.10.2 */ 
double remainder(double, double);
float remainderf(float, float);
long double remainderl(long double, long double);

/* 7.12.10.3 */
double remquo(double, double, int *);
float remquof(float, float, int *);
long double remquol(long double, long double, int *);

/* 7.12.11.1 */
double copysign(double, double); // in libmoldname.a
float copysignf(float, float);
long double copysignl(long double, long double);

/* 7.12.11.2 Return a NaN */
double nan(const char *tagp);
float nanf(const char *tagp);
long double nanl(const char *tagp);

/* 7.12.11.3 */
double nextafter(double, double); // in libmoldname.a
float nextafterf(float, float);
long double nextafterl (long double, long double);

/* 7.12.11.4 The nexttoward functions */
double nexttoward(double, long double);
float nexttowardf(float, long double);
long double nexttowardl(long double, long double);

/* 7.12.12.1 */
/* x > y ? (x - y) : 0.0 */
double fdim(double x, double y);
float fdimf(float x, float y);
long double fdiml(long double x, long double y);

/* fmax and fmin. */
/*
	NaN arguments are treated as missing data: if one argument is a NaN
	and the other numeric, then these functions choose the numeric
	value.
*/

/* 7.12.12.2 */
double fmax(double, double);
float fmaxf(float, float);
long double fmaxl(long double, long double);

/* 7.12.12.3 */
double fmin(double, double);
float fminf(float, float);
long double fminl(long double, long double);

/* 7.12.13.1 */
/* return x * y + z as a ternary op */ 
double fma(double, double, double);
float fmaf(float, float, float);
long double fmal(long double, long double, long double);

/* 7.12.14 */
/* 
	With these functions, comparisons involving quiet NaNs set the FP
	condition code to "unordered".	The IEEE floating-point spec
	dictates that the result of floating-point comparisons should be
	false whenever a NaN is involved, with the exception of the != op, 
	which always returns true: yes, (NaN != NaN) is true).
*/

#ifdef __GNUC__

#define isgreater(x, y) __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y) __builtin_isless(x, y)
#define islessequal(x, y) __builtin_islessequal(x, y)
#define islessgreater(x, y) __builtin_islessgreater(x, y)
#define isunordered(x, y) __builtin_isunordered(x, y)

#endif // __GNUC__

#endif // INCLUDE_MATH_H_
