#include "HorizonScene/LandscapePatchMesh.h"

namespace HE
{

LandscapePatchMesh LandscapePatchMesh::build(int cells)
{
	LandscapePatchMesh m;
	if (cells < 2 || (cells & 1) || cells > 255) return m;
	m.cells = cells;
	const int half = cells / 2, stride = cells + 1;
	m.indices.reserve(static_cast<size_t>(cells) * cells * 6);
	for (int q = 0; q < 4; ++q)
	{
		const int i0 = (q & 1) * half, j0 = (q >> 1) * half;
		m.quadrantFirst[q] = static_cast<uint32_t>(m.indices.size());
		for (int j = j0; j < j0 + half; ++j)
			for (int i = i0; i < i0 + half; ++i)
			{
				const uint16_t v00 = static_cast<uint16_t>(j * stride + i), v10 = static_cast<uint16_t>(v00 + 1);
				const uint16_t v01 = static_cast<uint16_t>(v00 + stride), v11 = static_cast<uint16_t>(v01 + 1);
				// the "\" diagonal v00-v11
				m.indices.insert(m.indices.end(), { v00, v11, v10, v00, v01, v11 });
			}
		m.quadrantCount[q] = static_cast<uint32_t>(m.indices.size()) - m.quadrantFirst[q];
	}
	return m;
}

LandscapeMorphSource landscapeMorphSource(int i, int j)
{
	LandscapeMorphSource s;
	const bool oi = (i & 1) != 0, oj = (j & 1) != 0;
	if (!oi && !oj) return s;
	s.n = 2;
	if (oi && !oj) { s.aI = i - 1; s.aJ = j;     s.bI = i + 1; s.bJ = j; }
	else if (!oi && oj) { s.aI = i; s.aJ = j - 1; s.bI = i;     s.bJ = j + 1; }
	else { s.aI = i - 1; s.aJ = j - 1; s.bI = i + 1; s.bJ = j + 1; }
	return s;
}

} // namespace HE
