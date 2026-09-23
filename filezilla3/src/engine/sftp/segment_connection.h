#ifndef FILEZILLA_ENGINE_SFTP_SEGMENT_CONNECTION_HEADER
#define FILEZILLA_ENGINE_SFTP_SEGMENT_CONNECTION_HEADER

#include "sftpcontrolsocket.h"

#include <fzssh/agent.hpp>
#include <fzssh/client.hpp>
#include <fzssh/privkey.hpp>

#include <libfilezilla/rate_limited_layer.hpp>
#include <libfilezilla/socket.hpp>

#include <set>

class activity_logger_layer;
class ProxyBase;
class CSftpFileTransferOpData;

// Per-channel adaptive pipelining state, modeled on the single-stream logic
struct segment_pipe_state
{
	fz::monotonic_clock rtt_requested_;
	uint64_t rtt_offset_{};
	uint64_t requested_offset_{};
	uint64_t response_offset_{};
	size_t max_pending_{16};
	size_t next_segment_{};
};

// An additional SSH connection used to download file segments in parallel.
// Never prompts the user: authentication reuses the credentials of the
// primary connection and the host key must be identical to the primary's.
class segment_connection final : public fz::event_handler
{
public:
	segment_connection(CSftpFileTransferOpData & op, CSftpControlSocket & controlSocket);
	~segment_connection();

	void start();

	bool ready() const { return ready_; }

	CSftpFileTransferOpData & op_;
	CSftpControlSocket & controlSocket_;

	std::unique_ptr<fz::socket> socket_;
	std::unique_ptr<activity_logger_layer> activity_logger_layer_;
	std::unique_ptr<fz::rate_limited_layer> ratelimit_layer_;
	std::unique_ptr<ProxyBase> proxy_layer_;
	fz::socket_interface* active_layer_{};

	std::unique_ptr<fz::ssh::client> ssh_;
	std::unique_ptr<fz::ssh::sftp::sftp_client> sftp_;

	segment_pipe_state pipe_;

private:
	virtual void operator()(fz::event_base const& ev) override;

	void OnSocketEvent(fz::socket_event_source*, fz::socket_event_flag t, int error);
	void OnHostAddress(fz::socket_event_source*, std::string const& address);

	void on_hostkey_verification(fz::ssh::session*, std::unique_ptr<fz::ssh::public_key> & key, fz::ssh::algorithm_info &);
	void on_auth_requested(fz::ssh::session*, std::string const& methods, bool is_continuation);
	void on_auth_done(fz::ssh::session*);
	void on_auth_pubkey_ok(fz::ssh::session*);
	void on_auth_signature_failed(fz::ssh::session*);
	void on_auth_keyboard_interactive_prompt(fz::ssh::session*, std::string const& name, std::string const& instruction, std::vector<fz::ssh::keyboard_interactive_prompt> & prompts);
	void on_auth_password_change_requested(fz::ssh::session*);
	void on_sftp_ready(fz::ssh::sftp::sftp_client*);
	void on_sftp_done(fz::ssh::sftp::sftp_client*);
	void on_session_done(fz::ssh::session*);
	void on_agent_keys(fz::ssh::agent_connection* conn, std::vector<std::unique_ptr<fz::ssh::private_key>> & keys);

	void next_auth();
	bool load_keys();
	bool auth_with_key();
	void set_keys_loaded();
	bool interactive_prompt_asks_for_password(std::string_view prompt);
	void teardown();
	void fail();
	void OnEstablishTimeout(fz::timer_id);

	std::vector<fz::ssh::private_key_info> keys_;
	std::unique_ptr<fz::ssh::agent_connection> agent_;
	std::set<std::string> used_keys_;

	fz::timer_id connect_timer_{};
	std::string methods_;
	bool agent_pending_{};
	bool keys_loaded_{};
	bool tried_pw_{};
	bool tried_interactive_{};
	bool tried_key_{};
	bool failed_{};
	bool ready_{};
};

#endif
