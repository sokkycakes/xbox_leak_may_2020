//
//	qrand.cpp
//
///////////////////////////////////////////////////////////////////////////////
//  Copyright (C) 2001, Pipeworks Software Inc.
#include "precomp.h"
#include "qrand.h"

// The original is x86 assembly: seed = ror(seed, 13) - (seed - 11), and the
// scaled variant returns the high 32 bits of seed * scale (unsigned).
static inline unsigned int QRandStep(int* seed)
{
	unsigned int a = (unsigned int)*seed;
	unsigned int r = (a >> 13) | (a << 19);
	unsigned int n = r - (a - 11u);
	*seed = (int)n;
	return n;
}

int	QRand::Rand(int scale)
{
	unsigned int n = QRandStep(&seed);
	return (int)(((unsigned long long)n * (unsigned int)scale) >> 32);
}

int	QRand::Rand()
{
	return (int)QRandStep(&seed);
}
