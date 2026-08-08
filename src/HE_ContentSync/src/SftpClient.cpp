#include "ContentSync/SftpClient.h"
#include "ContentSync/CsLog.h"

#include <Net/Socket.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace HE::Cs {

namespace {

// libssh2_init/exit are process-global and not reference-counted by the
// library itself — call init exactly once, ever, and never call exit (the
// Editor process outlives every possible last user of this module, so there is
// no "last user" moment to hook exit into; leaking the one-time init cost is
// simpler and safer than guessing that moment wrong).
void ensureLibssh2Initialized()
{
	static std::once_flag once;
	std::call_once(once, [] { libssh2_init(0); });
}

// Connect a plain TCP socket to endpoint.host:port, spinning on
// socketConnectPoll (a zero-timeout poll, see Socket.h) with short sleeps —
// acceptable here because every caller of this file runs on a background
// worker thread, never the main/frame thread.
constexpr int kConnectTimeoutMs = 10000;
constexpr int kPollIntervalMs   = 10;

HE::Net::SocketHandle connectBlocking(const std::string& host, std::uint16_t port, std::string& outError)
{
	HE::Net::socketSystemInit();

	HE::Net::SocketHandle h = HE::Net::kInvalidSocket;
	HE::Net::SocketResult r = HE::Net::socketCreateTcpConnecting(host, port, h);
	if (r == HE::Net::SocketResult::Error)
	{
		outError = "Could not resolve/connect to " + host;
		return HE::Net::kInvalidSocket;
	}

	int waitedMs = 0;
	while (r == HE::Net::SocketResult::WouldBlock)
	{
		if (waitedMs >= kConnectTimeoutMs)
		{
			HE::Net::socketClose(h);
			outError = "Timed out connecting to " + host;
			return HE::Net::kInvalidSocket;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
		waitedMs += kPollIntervalMs;
		r = HE::Net::socketConnectPoll(h);
	}
	if (r == HE::Net::SocketResult::Error)
	{
		HE::Net::socketClose(h);
		outError = "Connection to " + host + " refused/failed";
		return HE::Net::kInvalidSocket;
	}

	// libssh2 runs a blocking session on this socket (simplest possible
	// integration — every call site here already lives on its own worker
	// thread, so blocking I/O costs nothing but that thread's own time).
	HE::Net::socketSetNonBlocking(h, false);
	return h;
}

// Owns one TCP+SSH+SFTP session end to end. Never held across calls — each
// SftpClient function opens exactly one of these for the duration of the call.
struct Session
{
	HE::Net::SocketHandle sock = HE::Net::kInvalidSocket;
	LIBSSH2_SESSION*      ssh  = nullptr;
	LIBSSH2_SFTP*         sftp = nullptr;
	std::string           error;

	bool open(const SftpEndpoint& endpoint)
	{
		ensureLibssh2Initialized();

		if (!endpoint.configured())
		{
			error = "EngineContent SFTP endpoint is not configured";
			return false;
		}

		sock = connectBlocking(endpoint.host, endpoint.port, error);
		if (sock == HE::Net::kInvalidSocket) return false;

		ssh = libssh2_session_init();
		if (!ssh)
		{
			error = "libssh2_session_init failed";
			return false;
		}
		libssh2_session_set_blocking(ssh, 1);

		if (libssh2_session_handshake(ssh, static_cast<libssh2_socket_t>(sock)) != 0)
		{
			error = "SSH handshake failed: " + lastSshError();
			return false;
		}

		if (libssh2_userauth_password_ex(ssh,
		                                  endpoint.username.c_str(),
		                                  static_cast<unsigned int>(endpoint.username.size()),
		                                  endpoint.password.c_str(),
		                                  static_cast<unsigned int>(endpoint.password.size()),
		                                  nullptr) != 0)
		{
			error = "SSH authentication failed: " + lastSshError();
			return false;
		}

		sftp = libssh2_sftp_init(ssh);
		if (!sftp)
		{
			error = "Could not start SFTP subsystem: " + lastSshError();
			return false;
		}
		return true;
	}

	std::string lastSshError() const
	{
		if (!ssh) return {};
		char* msg = nullptr;
		int   len = 0;
		libssh2_session_last_error(ssh, &msg, &len, 0);
		return HE::Cs::detail::scrub(msg && len > 0 ? std::string(msg, static_cast<std::size_t>(len)) : std::string());
	}

	~Session()
	{
		if (sftp) libssh2_sftp_shutdown(sftp);
		if (ssh)
		{
			libssh2_session_disconnect_ex(ssh, SSH_DISCONNECT_BY_APPLICATION, "done", "");
			libssh2_session_free(ssh);
		}
		if (sock != HE::Net::kInvalidSocket) HE::Net::socketClose(sock);
	}

	Session()                           = default;
	Session(const Session&)             = delete;
	Session& operator=(const Session&)  = delete;
};

// endpoint.remoteBasePath + "/" + relPath, tolerating either side already
// carrying (or missing) a leading/trailing slash, and an empty base (= the
// account's own root, per the configured deployment).
std::string joinRemotePath(const std::string& base, const std::string& relPath)
{
	std::string rel = relPath;
	while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());

	if (base.empty()) return "/" + rel;

	std::string b = base;
	while (b.size() > 1 && b.back() == '/') b.pop_back();
	if (b.front() != '/') b = "/" + b;
	return b + "/" + rel;
}

} // namespace

SftpResult sftpTestConnection(const SftpEndpoint& endpoint)
{
	Session s;
	if (!s.open(endpoint)) return { false, s.error };
	return { true, {} };
}

SftpResult sftpGetFile(const SftpEndpoint& endpoint, const std::string& remoteRelPath, const std::string& localPath)
{
	Session s;
	if (!s.open(endpoint)) return { false, s.error };

	const std::string remotePath = joinRemotePath(endpoint.remoteBasePath, remoteRelPath);
	LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(s.sftp, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
	if (!handle)
		return { false, "Could not open remote file '" + remoteRelPath + "': " + s.lastSshError() };

	std::error_code ec;
	fs::path local = localPath;
	if (!local.parent_path().empty())
		fs::create_directories(local.parent_path(), ec); // ec ignored: the ofstream open below is the real check

	const fs::path tmpPath = local.string() + ".partial";
	std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		libssh2_sftp_close_handle(handle);
		return { false, "Could not create local file '" + tmpPath.string() + "'" };
	}

	std::vector<char> buf(64 * 1024);
	bool failed = false;
	for (;;)
	{
		const ssize_t n = libssh2_sftp_read(handle, buf.data(), buf.size());
		if (n < 0)
		{
			failed = true;
			break;
		}
		if (n == 0) break; // EOF
		out.write(buf.data(), n);
		if (!out)
		{
			failed = true;
			break;
		}
	}
	const std::string readError = failed ? s.lastSshError() : std::string();
	libssh2_sftp_close_handle(handle);
	out.close();

	if (failed)
	{
		std::error_code rmEc;
		fs::remove(tmpPath, rmEc);
		return { false, "Download of '" + remoteRelPath + "' failed: " + readError };
	}

	fs::rename(tmpPath, local, ec);
	if (ec)
	{
		std::error_code rmEc;
		fs::remove(tmpPath, rmEc);
		return { false, "Could not finalize downloaded file '" + local.string() + "': " + ec.message() };
	}
	return { true, {} };
}

SftpResult sftpPutFile(const SftpEndpoint& endpoint, const std::string& localPath, const std::string& remoteRelPath)
{
	std::ifstream in(localPath, std::ios::binary);
	if (!in) return { false, "Could not open local file '" + localPath + "'" };

	Session s;
	if (!s.open(endpoint)) return { false, s.error };

	const std::string remotePath = joinRemotePath(endpoint.remoteBasePath, remoteRelPath);
	LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(s.sftp, remotePath.c_str(),
	                                                 LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
	                                                 LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
	                                                 LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
	if (!handle)
		return { false, "Could not open remote file '" + remoteRelPath + "' for writing: " + s.lastSshError() };

	std::vector<char> buf(64 * 1024);
	bool failed = false;
	while (in)
	{
		in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
		const std::streamsize got = in.gcount();
		if (got <= 0) break;

		std::streamsize written = 0;
		while (written < got)
		{
			const ssize_t n = libssh2_sftp_write(handle, buf.data() + written,
			                                      static_cast<std::size_t>(got - written));
			if (n < 0) { failed = true; break; }
			written += n;
		}
		if (failed) break;
	}
	const std::string writeError = failed ? s.lastSshError() : std::string();
	libssh2_sftp_close_handle(handle);

	if (failed)
		return { false, "Upload of '" + localPath + "' failed: " + writeError };
	return { true, {} };
}

SftpResult sftpEnsureRemoteDir(const SftpEndpoint& endpoint, const std::string& remoteRelDir)
{
	Session s;
	if (!s.open(endpoint)) return { false, s.error };

	// Build up one path segment at a time — sftp mkdir has no -p equivalent,
	// and creating a deep path in one call fails if any intermediate level is
	// missing.
	std::string rel = remoteRelDir;
	while (!rel.empty() && rel.back() == '/') rel.pop_back();
	while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
	if (rel.empty()) return { true, {} };

	std::string partial;
	std::size_t pos = 0;
	while (pos <= rel.size())
	{
		std::size_t next = rel.find('/', pos);
		if (next == std::string::npos) next = rel.size();
		partial += (partial.empty() ? "" : "/") + rel.substr(pos, next - pos);

		const std::string full = joinRemotePath(endpoint.remoteBasePath, partial);
		// A pre-existing directory is not an error — libssh2_sftp_mkdir_ex
		// returns an error either way, so this deliberately ignores the
		// result rather than trying to distinguish "already exists" (SFTP
		// servers do not agree on the status code for that) from a real
		// failure that the subsequent putFile call will surface anyway.
		libssh2_sftp_mkdir_ex(s.sftp, full.c_str(), static_cast<unsigned int>(full.size()),
		                       LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
		                       LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH);

		if (next == rel.size()) break;
		pos = next + 1;
	}
	return { true, {} };
}

} // namespace HE::Cs
