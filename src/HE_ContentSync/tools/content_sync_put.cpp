// content_sync_put — add or replace a FEW files on the EngineContent server without touching the rest.
//
//   content_sync_put put    <engine-content-root> <relative/path.hasset>...
//   content_sync_put verify <engine-content-root> <relative/path.hasset>...
//
// "put" uploads exactly the named files and merges their entries into the remote manifest.json
// (publishEngineContentBlocking would replace the manifest with the scan of <root>, which delists
// everything <root> does not hold). "verify" compares size and manifest hash with the server.
// The SFTP endpoint is the one compiled into HorizonContentSync (see its CMakeLists); nothing about it
// is printed here.
#include "ContentSync/EngineContentPublish.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if (argc < 4 || (std::string(argv[1]) != "put" && std::string(argv[1]) != "verify"))
	{
		std::fprintf(stderr, "usage: content_sync_put put|verify <engine-content-root> <relative/path.hasset>...\n");
		return 2;
	}
	const bool put = std::string(argv[1]) == "put";
	const std::string root = argv[2];
	std::vector<std::string> rel(argv + 3, argv + argc);
	const auto log = [](const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); };
	const HE::Cs::PublishResult r = put ? HE::Cs::publishEngineContentFilesBlocking(root, rel, log)
	                                    : HE::Cs::verifyEngineContentFilesBlocking(root, rel, log);
	if (!r.ok) { std::fprintf(stderr, "%s failed: %s\n", argv[1], r.error.c_str()); return 1; }
	std::printf("%s ok: %zu uploaded, %zu unchanged/verified\n", argv[1], r.filesUploaded, r.filesUnchanged);
	return 0;
}
