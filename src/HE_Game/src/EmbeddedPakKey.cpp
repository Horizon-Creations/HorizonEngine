#include "EmbeddedPakKey.h"

// The ONLY place in any shipped binary where the magic appears contiguously —
// the exporter assembles its search pattern from pieces at runtime, so a false
// hit on the pattern literal inside the engine libraries is impossible.
extern "C" EmbeddedPakKeyBlock g_hePakKeyBlock = {
	{ 'H','E','_','E','M','B','E','D','D','E','D','_',
	  'P','A','K','K','E','Y','_','V','1','\0','\0','\0' },
	0,
	{ 0 },
	{ 0 }
};
