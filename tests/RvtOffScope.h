#pragma once
// RVT 2.0 (Thema 335): the Lit auto landscape material is two programs (pages before the RVT Write, live shader after the RVT Read)
// while RVT is on. A test that asks for "the whole material" in one shader (its textures, its parameters, its tap counts) generates
// it with RVT off, where the pair is inlined (matInlineRvt) and the shader computes everything per pixel.
#include <MaterialGraph/RvtSplit.h>

namespace HE_TEST
{
	struct RvtOffScope
	{
		bool was = HE::matRvtEnabled();
		RvtOffScope() { HE::matSetRvtEnabled(false); }
		~RvtOffScope() { HE::matSetRvtEnabled(was); }
		RvtOffScope(const RvtOffScope&) = delete;
		RvtOffScope& operator=(const RvtOffScope&) = delete;
	};
}
