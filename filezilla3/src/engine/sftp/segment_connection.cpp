#include "../filezilla.h"

#include "filetransfer.h"
#include "segment_connection.h"

#include "../activity_logger_layer.h"
#include "../proxy.h"

#include "../../include/engine_options.h"

#include <libfilezilla/file.hpp>
#include <libfilezilla/string.hpp>

#include <algorithm>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

using namespace std::literals;

namespace {
bool method_available(std::string_view const& methods, std::string_view method)
{
	for (auto m : fz::strtokenizer(methods, ","sv, true)) {
		if (m == method) {
			return true;
		}
	}
	return false;
}
}

segment_connection::segment_connection(CSftpFileTransferOpData & op, CSftpControlSocket & controlSocket)
	: fz::event_handler(controlSocket, fz::child_event_handler)
	, op_(op)
	, controlSocket_(controlSocket)
{
}

segment_connection::~segment_connection()
{
	teardown();
	remove_handler();
}

void segment_connection::teardown()
{
	if (connect_timer_) {
		stop_timer(connect_timer_);
		connect_timer_ = 0;
	}

	if (agent_ && agent_pending_) {
		agent_->cancel(*this);
		agent_pending_ = false;
	}
	agent_.reset();

	// Channels must be destroyed before the client
	sftp_.reset();
	ssh_.reset();

	// Destroy in reverse order
	proxy_layer_.reset();
	ratelimit_layer_.reset();
	activity_logger_layer_.reset();
	socket_.reset();
}

void segment_connection::start()
{
	auto & engine = controlSocket_.GetEngine();

	socket_ = std::make_unique<fz::socket>(engine.GetThreadPool(), nullptr);
	socket_->set_buffer_sizes(engine.GetOptions().get_int(OPTION_SOCKET_BUFFERSIZE_RECV), -1);
	activity_logger_layer_ = std::make_unique<activity_logger_layer>(nullptr, *socket_, engine.activity_logger_);
	ratelimit_layer_ = std::make_unique<fz::rate_limited_layer>(nullptr, *activity_logger_layer_, &engine.GetRateLimiter());
	active_layer_ = ratelimit_layer_.get();

	int const proxy_type = engine.GetOptions().get_int(OPTION_PROXY_TYPE);
	if (proxy_type > static_cast<int>(ProxyType::NONE) && proxy_type < static_cast<int>(ProxyType::count) && !controlSocket_.currentServer_.GetBypassProxy()) {
		proxy_layer_ = CreateProxy(nullptr, *active_layer_, &controlSocket_, static_cast<ProxyType>(proxy_type),
			fz::to_native(engine.GetOptions().get_string(OPTION_PROXY_HOST)),
			engine.GetOptions().get_int(OPTION_PROXY_PORT),
			engine.GetOptions().get_string(OPTION_PROXY_USER),
			engine.GetOptions().get_string(OPTION_PROXY_PASS));
		if (proxy_layer_) {
			active_layer_ = proxy_layer_.get();
		}
	}

	active_layer_->set_event_handler(this);
	int const res = active_layer_->connect(fz::to_native(controlSocket_.ConvertDomainName(controlSocket_.currentServer_.GetHost())), controlSocket_.currentServer_.GetPort());
	if (res) {
		controlSocket_.log(logmsg::debug_info, L"Segment connection could not connect: %s", fz::socket_error_description(res));
		fail();
		return;
	}

	// Enable TCP_NODELAY, speeds things up a bit.
	socket_->set_flags(fz::socket::flag_nodelay | fz::socket::flag_keepalive, true);

	connect_timer_ = add_timer(fz::duration::from_seconds(30), true);
}

void segment_connection::OnEstablishTimeout(fz::timer_id)
{
	connect_timer_ = 0;
	if (!ready_ && !failed_) {
		controlSocket_.log(logmsg::status, fztranslate("Establishing an additional connection timed out"));
		fail();
	}
}

void segment_connection::fail()
{
	if (failed_) {
		return;
	}
	failed_ = true;

	teardown();

	op_.on_segment_connection_failed(*this);
}

void segment_connection::operator()(fz::event_base const& ev)
{
	fz::dispatch<
		fz::socket_event,
		fz::hostaddress_event,
		fz::ssh::hostkey_verification_event,
		fz::ssh::auth_requested_event,
		fz::ssh::auth_done_event,
		fz::ssh::auth_public_key_okay_event,
		fz::ssh::auth_signature_failure_event,
		fz::ssh::auth_keyboard_interactive_prompt_event,
		fz::ssh::auth_password_change_request_event,
		fz::ssh::sftp::sftp_client::ready_event,
		fz::ssh::sftp::sftp_client::done_event,
		fz::ssh::session_done_event,
		fz::ssh::available_keys_event,
		fz::timer_event
	>(ev, this,
		&segment_connection::OnSocketEvent,
		&segment_connection::OnHostAddress,
		&segment_connection::on_hostkey_verification,
		&segment_connection::on_auth_requested,
		&segment_connection::on_auth_done,
		&segment_connection::on_auth_pubkey_ok,
		&segment_connection::on_auth_signature_failed,
		&segment_connection::on_auth_keyboard_interactive_prompt,
		&segment_connection::on_auth_password_change_requested,
		&segment_connection::on_sftp_ready,
		&segment_connection::on_sftp_done,
		&segment_connection::on_session_done,
		&segment_connection::on_agent_keys,
		&segment_connection::OnEstablishTimeout
	);
}

void segment_connection::OnSocketEvent(fz::socket_event_source*, fz::socket_event_flag t, int error)
{
	controlSocket_.SetAlive();

	switch (t)
	{
	case fz::socket_event_flag::connection_next:
		if (error) {
			controlSocket_.log(logmsg::status, _("Connection attempt failed with \"%s\", trying next address."), fz::socket_error_description(error));
		}
		break;
	case fz::socket_event_flag::connection:
		if (error) {
			controlSocket_.log(logmsg::status, _("Connection attempt failed with \"%s\"."), fz::socket_error_description(error));
			fail();
		}
		else {
			fz::ssh::client_parameters params;
			params.kex_ += ",diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1"sv;
			params.cipher_ += ",aes256-cbc,aes192-cbc,aes128-cbc"sv;
			params.hostkey_signatures_ += ",ssh-rsa"sv;
			params.single_channel_ = true;
			params.softwareversion_ = "FileZilla"sv;
			params.softwareversion_ += ' ';
			params.softwareversion_ += PACKAGE_VERSION;
			for (auto & c : params.softwareversion_) {
				if (c == ' ' || c == '-') {
					c = '_';
				}
			}

			if (controlSocket_.currentServer_.GetExtraParameter("allow_non_crlf_identification_string"sv) == L"1") {
				params.compatibility_flags_ |= fz::ssh::compatibility_flags::identification_string_not_terminated_by_crlf;
			}

			std::string user = (controlSocket_.credentials_.logonType_ == LogonType::anonymous) ? "anonymous" : fz::to_utf8(controlSocket_.currentServer_.GetUser());
			ssh_ = std::make_unique<fz::ssh::client>(params, user, *active_layer_, *this, controlSocket_.logger_);
		}
		break;
	default:
		break;
	}
}

void segment_connection::OnHostAddress(fz::socket_event_source*, std::string const&)
{
	controlSocket_.SetAlive();
}

void segment_connection::on_hostkey_verification(fz::ssh::session*, std::unique_ptr<fz::ssh::public_key> & key, fz::ssh::algorithm_info &)
{
	controlSocket_.SetAlive();

	if (key && !controlSocket_.verified_hostkey_.empty() && key->pubkey_blob() == controlSocket_.verified_hostkey_) {
		ssh_->hostkey_decision(true);
		return;
	}

	// The host key differs from the primary connection's verified host key.
	// This could be a man-in-the-middle attack, abort the transfer.
	failed_ = true;
	controlSocket_.log(logmsg::error, fztranslate("The host key of the additional connection does not match the host key of the primary connection"));
	ssh_->hostkey_decision(false);
	teardown();
	op_.on_segment_hostkey_mismatch();
}

void segment_connection::on_auth_requested(fz::ssh::session*, std::string const& methods, bool is_continuation)
{
	controlSocket_.SetAlive();
	methods_ = methods;

	if (!is_continuation && tried_key_ && !keys_.empty()) {
		// Try the next key
		keys_.pop_back();
		tried_key_ = false;
	}

	next_auth();
}

void segment_connection::next_auth()
{
	if (method_available(methods_, "publickey"sv) && !tried_key_) {
		if (auth_with_key()) {
			return;
		}
	}

	if (method_available(methods_, "password"sv) && !tried_pw_ && !controlSocket_.credentials_.GetPass().empty()) {
		tried_pw_ = true;
		ssh_->auth_with_password(fz::to_utf8(controlSocket_.credentials_.GetPass()));
		return;
	}

	if (controlSocket_.credentials_.logonType_ != LogonType::anonymous && method_available(methods_, "keyboard-interactive"sv) && !tried_interactive_ && !controlSocket_.credentials_.GetPass().empty()) {
		tried_interactive_ = true;
		ssh_->auth_keyboard_interactive();
		return;
	}

	controlSocket_.log(logmsg::debug_info, L"No authentication methods available for segment connection");
	fail();
}

bool segment_connection::load_keys()
{
	if (keys_loaded_) {
		return false;
	}
	if (agent_) {
		// Still loading
		return true;
	}

	auto names = controlSocket_.credentials_.keyFile_;
	names += '\n';
	names += controlSocket_.GetEngine().GetOptions().get_string(OPTION_SFTP_KEYFILES);
	std::set<std::wstring> seen_names;
	for (auto k : fz::strtokenizer(names, L"\r\n"sv, true)) {
		if (!seen_names.emplace(k).second) {
			continue;
		}
		fz::buffer b;
		if (!fz::read_file(fz::to_native(k), b, 128*1024)) {
			controlSocket_.log(logmsg::debug_info, L"Could not read key file '%s'.", k);
			continue;
		}

		auto infos = fz::ssh::load_private_key_infos(b.to_view(), controlSocket_.logger_, {});
		for (auto & i : infos) {
			// Without the public key the key cannot be probed and, as segment
			// connections cannot ask for a key password, such keys are unusable.
			if (!i.pubkey_ || !used_keys_.emplace(i.pubkey_->pubkey_blob()).second) {
				continue;
			}
			i.name_ = fz::to_utf8(k);
			keys_.emplace_back(std::move(i));
		}
	}

	fz::ssh::agent_compatibility_flags flags{};
#ifdef USE_MAC_SANDBOX
	flags |= fz::ssh::agent_compatibility_flags::suppress_agent_connection_error;
#endif
	if (controlSocket_.currentServer_.GetExtraParameter("allow_agent_keys_of_unknown_type"sv) == L"1") {
		flags |= fz::ssh::agent_compatibility_flags::allow_keys_with_unknown_types;
	}
	agent_ = std::make_unique<fz::ssh::agent_connection>(controlSocket_.GetEngine().GetThreadPool(), *this, controlSocket_.logger_, flags);
	agent_pending_ = true;
	agent_->get_keys(*this);
	return true;
}

bool segment_connection::auth_with_key()
{
	if (load_keys()) {
		return true;
	}

	while (!keys_.empty()) {
		auto & key = keys_.back();

		if (key.pubkey_) {
			tried_key_ = true;
			used_keys_.emplace(key.pubkey_->pubkey_blob());
			ssh_->auth_with_key(key.pubkey_);
			return true;
		}

		keys_.pop_back();
	}

	return false;
}

void segment_connection::on_agent_keys(fz::ssh::agent_connection* conn, std::vector<std::unique_ptr<fz::ssh::private_key>> & keys)
{
	if (!agent_ || conn != agent_.get()) {
		return;
	}
	agent_pending_ = false;

	for (auto & k : keys) {
		if (!used_keys_.emplace(k->pubkey_blob()).second) {
			continue;
		}
		fz::ssh::private_key_info i;
		i.privkey_ = std::move(k);
		i.pubkey_ = i.privkey_->pubkey();
		keys_.emplace_back(std::move(i));
	}

	set_keys_loaded();
	next_auth();
}

void segment_connection::set_keys_loaded()
{
	if (keys_loaded_) {
		return;
	}

	keys_loaded_ = true;
	used_keys_.clear();

	std::reverse(keys_.begin(), keys_.end());
}

void segment_connection::on_auth_pubkey_ok(fz::ssh::session*)
{
	controlSocket_.SetAlive();

	if (keys_.empty()) {
		fail();
		return;
	}

	auto & key = keys_.back();
	if (!key.privkey_) {
		// The key is encrypted and segment connections cannot ask for the password
		keys_.pop_back();
		tried_key_ = false;
		next_auth();
		return;
	}

	ssh_->auth_with_key(key.privkey_, true);
}

void segment_connection::on_auth_signature_failed(fz::ssh::session*)
{
	if (!keys_.empty()) {
		keys_.pop_back();
	}
	tried_key_ = false;
	next_auth();
}

bool segment_connection::interactive_prompt_asks_for_password(std::string_view prompt)
{
	fz::rtrim(prompt, "\r\n :"sv);
	if (fz::equal_insensitive_ascii(prompt, "password"sv)) {
		return true;
	}

	if (fz::equal_insensitive_ascii(prompt, fz::sprintf("password for %s:%s"sv, fz::to_utf8(controlSocket_.currentServer_.GetUser()), fz::to_utf8(controlSocket_.currentServer_.GetHost())))) {
		return true;
	}

	return false;
}

void segment_connection::on_auth_keyboard_interactive_prompt(fz::ssh::session*, std::string const&, std::string const&, std::vector<fz::ssh::keyboard_interactive_prompt> & prompts)
{
	controlSocket_.SetAlive();

	if (tried_interactive_ && prompts.size() == 1 && !controlSocket_.credentials_.GetPass().empty() && interactive_prompt_asks_for_password(prompts[0].prompt_)) {
		std::vector<std::string> responses;
		responses.emplace_back(fz::to_utf8(controlSocket_.credentials_.GetPass()));
		ssh_->auth_keyboard_interactive_response(std::move(responses));
		return;
	}

	// Segment connections cannot ask the user, fail just this connection
	controlSocket_.log(logmsg::debug_info, L"Cannot answer keyboard-interactive prompt for segment connection");
	fail();
}

void segment_connection::on_auth_password_change_requested(fz::ssh::session*)
{
	controlSocket_.log(logmsg::debug_info, L"Server requested password change on segment connection");
	fail();
}

void segment_connection::on_auth_done(fz::ssh::session*)
{
	controlSocket_.SetAlive();

	auto si = ssh_->open_channel(fz::ssh::channel_type::subsystem, "sftp"sv);
	if (!si) {
		controlSocket_.log(logmsg::debug_info, L"Could not open SFTP channel on segment connection");
		fail();
		return;
	}

	fz::ssh::sftp::compatibility_flags flags{};
	if (controlSocket_.currentServer_.GetExtraParameter("ignore_unknown_flags_in_attributes"sv) == L"1") {
		flags |= fz::ssh::sftp::compatibility_flags::ignore_unknown_flags_in_attributes;
	}

	sftp_ = std::make_unique<fz::ssh::sftp::sftp_client>(std::move(si), *this, controlSocket_.logger_, flags);
}

void segment_connection::on_sftp_ready(fz::ssh::sftp::sftp_client*)
{
	controlSocket_.SetAlive();

	if (connect_timer_) {
		stop_timer(connect_timer_);
		connect_timer_ = 0;
	}

	ready_ = true;
	op_.on_segment_connection_ready(*this);
}

void segment_connection::on_sftp_done(fz::ssh::sftp::sftp_client*)
{
	if (failed_) {
		return;
	}
	if (ready_) {
		failed_ = true;
		op_.on_segment_connection_dropped(*this);
	}
	else {
		fail();
	}
}

void segment_connection::on_session_done(fz::ssh::session*)
{
	if (failed_) {
		return;
	}
	if (ready_) {
		failed_ = true;
		op_.on_segment_connection_dropped(*this);
	}
	else {
		fail();
	}
}
