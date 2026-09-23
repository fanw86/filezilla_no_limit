#include "../filezilla.h"

#include "../directorycache.h"
#include "filetransfer.h"

#include "../../include/engine_options.h"

#include <libfilezilla/file.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/process.hpp>
#include <libfilezilla/string.hpp>

#include <assert.h>

using namespace std::literals;

namespace {
enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_list,
	filetransfer_stat,
	filetransfer_mkdir,
	filetransfer_checkoverwrite,
	filetransfer_transfer,
	filetransfer_segmented,
	filetransfer_concat,
};

struct can_send_event_type{};
typedef fz::simple_event<can_send_event_type> can_send_event;
}

CSftpFileTransferOpData::~CSftpFileTransferOpData()
{
	remove_handler();
	reader_.reset();
	cleanup_segments();
	if (sftp_) {
		sftp_->cancel_wait(this);
	}
}

int CSftpFileTransferOpData::Send()
{
	if (!sftp_) {
		return FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED;
	}

	if (opState == filetransfer_init) {
		if (download()) {
			std::wstring filename = remotePath_.FormatFilename(remoteFile_);
			log(logmsg::status, _("Starting download of %s"), filename);
		}
		else {
			log(logmsg::status, _("Starting upload of %s"), localName_);
		}

		localFileSize_ = download() ? writer_factory_.size() : reader_factory_.size();
		localFileTime_ = download() ? writer_factory_.mtime() : reader_factory_.mtime();

		return CheckRemoteFile(false);
	}
	else if (opState == filetransfer_checkoverwrite) {
		opState = filetransfer_transfer;
		int res = controlSocket_.CheckOverwriteFile();
		if (res != FZ_REPLY_OK) {
			return res;
		}
		return FZ_REPLY_CONTINUE;
	}
	else if (opState == filetransfer_list) {
		controlSocket_.List(remotePath_, LIST_FLAG_REFRESH);
		return FZ_REPLY_CONTINUE;
	}
	else if (opState == filetransfer_transfer) {
		if (download()) {
			size_t const segments = segment_count();
			if (segments > 1) {
				return start_segmented(segments);
			}
			if (!resume_) {
				// Fresh single-stream download, delete stale parts from
				// interrupted segmented downloads.
				if (fz::local_filesys::get_size(fz::to_native(segment_manifest_path())) >= 0) {
					delete_segment_files(max_segment_count_);
				}
			}
		}

		// Bit convoluted as we need to use server encoding for remote filenames.
		std::string remoteFile;
		std::wstring logstr;
		if (resume_) {
			logstr = L"re";
		}
		fz::ssh::sftp::file_flags flags{};
		if (download()) {
			engine_.transfer_status_.Init(remoteFileSize_, resume_ ? localFileSize_ : 0, false);
			logstr += L"get ";

			remoteFile = controlSocket_.ConvToServer(remotePath_.FormatFilename(remoteFile_));
			if (remoteFile.empty()) {
				log(logmsg::error, _("Could not convert command to server encoding"));
				return FZ_REPLY_ERROR;
			}
			logstr += controlSocket_.QuoteFilename(remotePath_.FormatFilename(remoteFile_)) + L" ";

			std::wstring localFile = controlSocket_.QuoteFilename(localName_);
			logstr += localFile;

			flags = fz::ssh::sftp::file_flags::SSH_FXF_READ;
		}
		else {
			engine_.transfer_status_.Init(localFileSize_, resume_ ? remoteFileSize_ : 0, false);
			logstr += L"put ";

			std::wstring localFile = controlSocket_.QuoteFilename(localName_);
			logstr += localFile + L" ";

			remoteFile = controlSocket_.ConvToServer(remotePath_.FormatFilename(remoteFile_));
			if (remoteFile.empty()) {
				log(logmsg::error, _("Could not convert command to server encoding"));
				return FZ_REPLY_ERROR;
			}
			logstr += controlSocket_.QuoteFilename(remotePath_.FormatFilename(remoteFile_));

			flags = fz::ssh::sftp::file_flags::SSH_FXF_WRITE;
			if (!resume_) {
				flags |= fz::ssh::sftp::file_flags::SSH_FXF_CREAT | fz::ssh::sftp::file_flags::SSH_FXF_TRUNC;
			}

			request_offset_ = resume_ ? remoteFileSize_ : 0;
			reader_ = reader_factory_->open(*controlSocket_.buffer_pool_, request_offset_, fz::aio_base::nosize, controlSocket_.max_buffer_count());
			if (!reader_) {
				return FZ_REPLY_ERROR;
			}
		}
		engine_.transfer_status_.SetStartTime();
		transferInitiated_ = true;
		controlSocket_.SetWait(true);

		controlSocket_.log_raw(logmsg::command, logstr);
		sftp_->open(this, remoteFile, flags);
		return FZ_REPLY_WOULDBLOCK;
	}
	else if (opState == filetransfer_stat) {
		std::string remoteFile = controlSocket_.ConvToServer(remotePath_.FormatFilename(remoteFile_));
		if (remoteFile.empty()) {
			log(logmsg::error, _("Could not convert command to server encoding"));
			return FZ_REPLY_ERROR;
		}
		sftp_->stat(this, remoteFile);
		return FZ_REPLY_WOULDBLOCK;
	}
	else if (opState == filetransfer_mkdir) {
		controlSocket_.Mkdir(remotePath_);
		return FZ_REPLY_CONTINUE;
	}

	return FZ_REPLY_INTERNALERROR;
}

int CSftpFileTransferOpData::CheckRemoteFile(bool after_listing)
{
	CDirentry entry;
	bool dirDidExist;
	bool matchedCase;
	bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, remotePath_, remoteFile_, dirDidExist, matchedCase);
	if (!found) {
		if (!dirDidExist) {
			if (!after_listing) {
				opState = filetransfer_list;
			}
			else {
				if (!download() && remotePath_.HasParent()) {
					opState = filetransfer_mkdir;
				}
				else {
					opState = filetransfer_stat;
				}
			}
		}
		else if (download() && options_.get_int(OPTION_PRESERVE_TIMESTAMPS)) {
			opState = filetransfer_stat;
		}
		else {
			opState = filetransfer_checkoverwrite;
		}
	}
	else {
		if (entry.is_unsure()) {
			if (!after_listing) {
				opState = filetransfer_list;
			}
			else {
				opState = filetransfer_stat;
			}
		}
		else {
			if (matchedCase) {
				remoteFileSize_ = entry.size;
				if (entry.has_date()) {
					remoteFileTime_ = entry.time;
				}

				if (download() && !entry.has_time() &&
					options_.get_int(OPTION_PRESERVE_TIMESTAMPS))
				{
					opState = filetransfer_stat;
				}
				else {
					opState = filetransfer_checkoverwrite;
				}
			}
			else {
				opState = filetransfer_stat;
			}
		}
	}

	return FZ_REPLY_CONTINUE;
}

int CSftpFileTransferOpData::SubcommandResult(int prevResult, COpData const&)
{
	if (opState == filetransfer_list) {
		if (prevResult == FZ_REPLY_OK) {
			return CheckRemoteFile(true);
		}
		else {
			if (!download() && remotePath_.HasParent()) {
				opState = filetransfer_mkdir;
			}
			else {
				opState = filetransfer_stat;
			}
			return FZ_REPLY_CONTINUE;
		}
	}
	else if (opState == filetransfer_mkdir) {
		// Ignore errors

		opState = filetransfer_stat;
		return FZ_REPLY_CONTINUE;
	}

	log(logmsg::debug_warning, L"  Unknown opState (%d)", opState);
	return FZ_REPLY_INTERNALERROR;
}

void CSftpFileTransferOpData::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::aio_buffer_event, fz::ssh::sftp::outbuf_empty_event, fz::timer_event>(ev, this,
		&CSftpFileTransferOpData::OnBufferAvailability,
		&CSftpFileTransferOpData::on_can_send,
		&CSftpFileTransferOpData::OnTimer
	)) {
		return;
	}

	CSftpOpData::operator()(ev);
}

void CSftpFileTransferOpData::OnBufferAvailability(fz::aio_waitable const* w)
{
	if (opState == filetransfer_concat) {
		if (w == writer_.get()) {
			if (concat_finalizing_) {
				concat_finalize();
			}
			else {
				concat_pump();
			}
		}
		else if (w == reader_.get()) {
			concat_pump();
		}
		return;
	}

	if (w == reader_.get()) {
		on_can_send(nullptr);
	}
	else if (w == writer_.get()) {
		if (finalizing_) {
			finalize();
		}
		else {
			auto r = get_next_download_buffer();
			if (r == fz::aio_result::ok) {
				sftp_->unblock_read();
				request_data();
			}
		}
	}
}

int CSftpFileTransferOpData::Reset(int result)
{
	if (sftp_ && !handle_.empty()) {
		sftp_->close(nullptr, handle_);
	}
	handle_.clear();

	if (download()) {
		if (result == FZ_REPLY_OK && opState != filetransfer_segmented && opState != filetransfer_concat) {
			// Finished single-stream download, remove stale parts of interrupted segmented downloads
			if (fz::local_filesys::get_size(fz::to_native(segment_manifest_path())) >= 0) {
				delete_segment_files(max_segment_count_);
			}
		}
		else if (result != FZ_REPLY_OK && opState == filetransfer_concat) {
			// The partially merged file can be reproduced from the parts. Remove it
			// so that the next attempt resumes the segments instead of going down
			// the single-stream path.
			reader_.reset();
			writer_.reset();
			fz::remove_file(fz::to_native(localName_), false);
		}
	}

	cleanup_segments();

	return result;
}

void CSftpFileTransferOpData::cleanup_segments()
{
	for (auto & seg : segments_) {
		if (auto * channel = segment_channel(seg)) {
			channel->cancel(seg.get());
			if (!seg->handle_.empty()) {
				channel->close(nullptr, seg->handle_);
				seg->handle_.clear();
			}
		}
		if (seg->stall_channel_) {
			// The channel is stalled by a segment waiting for buffers. Unblock it
			// so that the replies to the cancelled requests can be discarded.
			seg->stall_channel_->unblock_read();
			seg->stall_channel_ = nullptr;
		}
	}
	segments_.clear();
	segment_connections_.clear();
}

void CSftpFileTransferOpData::finalize()
{
	finalizing_ = true;
	auto r = writer_->add_buffer(std::move(buffer_), *this);
	if (r == fz::aio_result::ok) {
		r = writer_->finalize(*this);
	}
	if (r == fz::aio_result::error) {
		trigger_reset(FZ_REPLY_ERROR);
	}
	else if (r == fz::aio_result::ok) {
		if (options_.get_int(OPTION_PRESERVE_TIMESTAMPS) && remoteFileTime_) {
			if (!writer_->set_mtime(remoteFileTime_)) {
				log(logmsg::debug_warning, L"Could not set modification time");
			}
		}
		trigger_reset(FZ_REPLY_OK);
	}
}

fz::aio_result CSftpFileTransferOpData::get_next_upload_buffer()
{
	fz::aio_result r;
	std::tie(r, buffer_) = reader_->get_buffer(*this);
	if (r == fz::aio_result::wait) {
		return r;
	}
	if (r == fz::aio_result::error) {
		trigger_reset(FZ_REPLY_ERROR);
		return r;
	}
	return r;
}

fz::aio_result CSftpFileTransferOpData::get_next_download_buffer()
{
	auto r = writer_->add_buffer(std::move(buffer_), *this);
	if (r == fz::aio_result::ok) {
		buffer_ = controlSocket_.buffer_pool_->get_buffer(*this);
		if (!buffer_) {
			r = fz::aio_result::wait;
		}
	}
	if (r == fz::aio_result::error) {
		trigger_reset(FZ_REPLY_ERROR);
	}
	return r;
}

CSftpOpData::continuation CSftpFileTransferOpData::process_handle(std::string_view handle)
{
	controlSocket_.SetAlive();

	handle_ = handle;

	if (download()) {
		if (resume_) {
			request_offset_ = writer_factory_.size();
			if (request_offset_ == fz::aio_base::nosize) {
				log(fz::logmsg::error, fztranslate("Cannot resume, could not get size of local file"));
				trigger_reset(FZ_REPLY_ERROR);
				return continuation::next;
			}
		}
		else {
			request_offset_ = 0;
		}
		response_offset_ = request_offset_;

		writer_ = controlSocket_.OpenWriter(writer_factory_, request_offset_, true);
		if (!writer_) {
			trigger_reset(FZ_REPLY_ERROR);
			return continuation::next;
		}

		fz::aio_result r = get_next_download_buffer();
		if (r == fz::aio_result::wait) {
			return continuation::wait;
		}
		else if (r == fz::aio_result::ok) {
			request_data();
		}
	}
	else {
#ifdef FZ_WINDOWS
		// For send buffer tuning
		add_timer(fz::duration::from_seconds(1), false);
#endif

		send_event<fz::ssh::sftp::outbuf_empty_event>(nullptr);
	}

	return continuation::next;
}

void CSftpFileTransferOpData::request_data()
{
	if (rtt_.requested_ && response_offset_ == rtt_.offset_) {
		// We've got the RTT
		auto ms = (fz::monotonic_clock::now() - rtt_.requested_).get_milliseconds();
		rtt_.requested_ = fz::monotonic_clock();

		if (ms < 1) {
			ms = 1;
		}

		// Aim for 500ms between queuing the request and the reply
		size_t ideal_pending = max_pending_ * 500 / ms;

		if (ideal_pending > max_pending_) {
			size_t divisor = (ideal_pending > max_pending_ * 2) ? 2 : 8;
			max_pending_ += max_pending_ / divisor;
			if (max_pending_ > 1024*16) {
				max_pending_ = 1024*16;
			}
		}
		else if (ideal_pending < max_pending_) {
			size_t divisor = (ideal_pending * 2 < max_pending_) ? 2 : 8;
			max_pending_ -= max_pending_ / divisor;
			if (max_pending_ < initial_max_pending_) {
				max_pending_ = initial_max_pending_;
			}
		}
	}

	size_t i{};
	for (i = 0; i < 2 && sftp_->pending_requests() < max_pending_ && sftp_->can_send_packets(); ++i) {
		sftp_->read(this, handle_, request_offset_, blocksize_);
		request_offset_ += blocksize_;
	}

	if (i && sftp_->pending_requests() == max_pending_ && !rtt_.requested_) {
		// Begin RTT measurement at full capacity
		rtt_.requested_ = fz::monotonic_clock::now();
		rtt_.offset_ = request_offset_;
	}
}

CSftpOpData::continuation CSftpFileTransferOpData::process_data(std::string_view data)
{
	controlSocket_.SetAlive();

	if (data.size() > blocksize_) {
		log(fz::logmsg::error, L"Server sent a block of data larger than requested."sv);
		return continuation::error;
	}

	if (data.size() != blocksize_) {
		if (short_read_) {
			// Instead of error, we could disable pipelining. As the file may not be seekable, this would require re-opening the file and restarting from the beginning
			log(fz::logmsg::error, fztranslate("Got a short read not at the end of a file, this is not permissiable on normal files"));
			trigger_reset(FZ_REPLY_CRITICALERROR);
			return continuation::next;
		}
		short_read_ = true;
	}

	buffer_->append(data);

	response_offset_ += data.size();

	if (buffer_->capacity() - buffer_->size() < blocksize_) {
		auto r = get_next_download_buffer();
		if (r == fz::aio_result::wait) {
			return continuation::wait;
		}
		else if (r == fz::aio_result::error) {
			return continuation::next;
		}
	}

	request_data();

	return continuation::next;
}

CSftpOpData::continuation CSftpFileTransferOpData::do_process_status(fz::ssh::sftp::status_code code, std::wstring_view msg)
{
	controlSocket_.SetAlive();

	if (download()) {
		if (opState == filetransfer_stat) {
			log(logmsg::error, fztranslate("Could not get file attributes: %s"), msg);
			trigger_reset(FZ_REPLY_ERROR);
		}
		else if (!handle_.empty()) {
			if (code == fz::ssh::sftp::status_code::SSH_FX_EOF) {
				sftp_->cancel(this);
				finalize();
			}
			else {
				log(logmsg::error, fztranslate("Could not read from remote file: %s"), msg);
				trigger_reset(FZ_REPLY_ERROR);
			}
		}
		else {
			log(logmsg::error, fztranslate("Could not open remote file: %s"), msg);
			trigger_reset(FZ_REPLY_ERROR);
		}
	}
	else {
		if (opState == filetransfer_stat) {
			// Ignore errors. At worse, overwrite check won't see that file already exists.
			// This behavior is similar to the FTP(S) implementation.
			//
			// We could potentially use SSH_FXF_EXCL, but that raises two different questions:
			// - What is the error code? There's nothing to distinguish between "already exists"
			//   and other errors.
			// - Potential lack of server support for this rarely used flag

			log(logmsg::debug_warning, "Could not get file attributes: %s"sv, msg);
			opState = filetransfer_checkoverwrite;
			trigger_next();
		}
		else if (code == fz::ssh::sftp::status_code::SSH_FX_OK) {
			engine_.transfer_status_.SetMadeProgress();
			if (finalizing_ && !sftp_->pending_requests()) {
				trigger_reset(FZ_REPLY_OK);
			}
			return continuation::next;
		}
		else if (handle_.empty() && !finalizing_) {
			log(logmsg::error, fztranslate("Could not open remote file: %s"), msg);
			trigger_reset(FZ_REPLY_ERROR);
		}
		else {
			if (called_fsetstat_ && sftp_->pending_requests() == 1) {
				log(logmsg::error, fztranslate("Could not set remote file time: %s"), msg);
			}
			else {
				log(logmsg::error, fztranslate("Could not write to remote file: %s"), msg);
				trigger_reset(FZ_REPLY_ERROR);
			}
		}
	}

	return continuation::next;
}

CSftpOpData::continuation CSftpFileTransferOpData::process_attributes(fz::ssh::sftp::attributes & attrs)
{
	if (opState != filetransfer_stat) {
		log(logmsg::debug_warning, L"  Unknown opState (%d)", opState);
		trigger_reset(FZ_REPLY_INTERNALERROR);
		return continuation::error;
	}

	if (attrs.modified_) {
		remoteFileTime_ = *attrs.modified_;
		remoteFileTime_+= fz::duration::from_minutes(currentServer_.GetTimezoneOffset());
	}
	if (attrs.size_) {
		remoteFileSize_ = *attrs.size_;
	}

	opState = filetransfer_checkoverwrite;
	trigger_next();
	return continuation::next;
}

void CSftpFileTransferOpData::on_can_send(fz::ssh::sftp::sftp_client*)
{
	if (buffer_->empty()) {
		auto r = get_next_upload_buffer();
		if (r != fz::aio_result::ok) {
			return;
		}
		if (buffer_->empty()) {
			finalizing_ = true;
			if (options_.get_int(OPTION_PRESERVE_TIMESTAMPS)) {
				fz::datetime  mtime = reader_->mtime();
				if (!mtime) {
					mtime = reader_factory_.mtime();
				}
				if (mtime) {
					mtime -= fz::duration::from_minutes(currentServer_.GetTimezoneOffset());
					fz::ssh::sftp::attributes attr;
					attr.modified_ = mtime;
					sftp_->fsetstat(this, handle_, attr);
					called_fsetstat_ = true;
				}
			}
			sftp_->close(this, handle_);
			handle_.clear();
			return;
		}
	}
	if (!sftp_->can_send_packets(*this)) {
		return;
	}

	auto size = std::min(blocksize_, buffer_->size());
	sftp_->write(this, handle_, request_offset_, buffer_->to_view().substr(0, size));
	request_offset_ += size;
	buffer_->consume(size);

	engine_.transfer_status_.Update(size);

	resend_current_event();
}

void CSftpFileTransferOpData::OnTimer(fz::timer_id)
{
#if FZ_WINDOWS
	auto *socket = controlSocket_.socket_.get();
	if (socket && socket->is_connected()) {
		int const ideal_send_buffer = socket->ideal_send_buffer_size();
		if (ideal_send_buffer != -1) {
			socket->set_buffer_sizes(-1, ideal_send_buffer);
		}
	}
#endif
}

CSftpFileTransferOpData::segment_handler::segment_handler(CSftpFileTransferOpData & op, size_t index, uint64_t offset, uint64_t length)
	: fz::event_handler(op.controlSocket_, fz::child_event_handler)
	, op_(op)
	, index_(index)
	, offset_(offset)
	, length_(length)
	, request_offset_(offset)
{
}

CSftpFileTransferOpData::segment_handler::~segment_handler()
{
	remove_handler();
	if (op_.controlSocket_.buffer_pool_) {
		op_.controlSocket_.buffer_pool_->remove_waiter(*this);
	}
	writer_.reset();
}

fz::ssh::sftp::continuation CSftpFileTransferOpData::segment_handler::process_status(fz::ssh::sftp::status_code code, std::string_view description)
{
	return op_.process_segment_status(*this, code, fz::to_wstring(description));
}

fz::ssh::sftp::continuation CSftpFileTransferOpData::segment_handler::process_handle(std::string_view handle)
{
	return op_.on_segment_open(*this, handle);
}

fz::ssh::sftp::continuation CSftpFileTransferOpData::segment_handler::process_data(std::string_view data)
{
	return op_.process_segment_data(*this, data);
}

fz::ssh::sftp::continuation CSftpFileTransferOpData::segment_handler::failure()
{
	op_.seg_failed_ = true;
	op_.trigger_reset(FZ_REPLY_ERROR);
	return fz::ssh::sftp::continuation::next;
}

void CSftpFileTransferOpData::segment_handler::operator()(fz::event_base const& ev)
{
	fz::dispatch<fz::aio_buffer_event>(ev, this, &segment_handler::OnBufferAvailability);
}

void CSftpFileTransferOpData::segment_handler::OnBufferAvailability(fz::aio_waitable const* w)
{
	op_.on_segment_buffer_availability(*this, w);
}

size_t CSftpFileTransferOpData::segment_count() const
{
	if (!download() || resume_ || remoteFileSize_ <= 0 || localFileSize_ != fz::aio_base::nosize) {
		return 1;
	}

	int const wanted = options_.get_int(OPTION_SFTP_DOWNLOAD_SEGMENTS);
	if (wanted < 2) {
		return 1;
	}

	uint64_t const total = static_cast<uint64_t>(remoteFileSize_);
	uint64_t const minsize = static_cast<uint64_t>(options_.get_int(OPTION_SFTP_SEGMENT_MIN_SIZE)) * 1024 * 1024;

	size_t n = static_cast<size_t>(wanted);
	while (n > 1 && total < n * minsize) {
		--n;
	}
	return n;
}

std::wstring CSftpFileTransferOpData::segment_manifest_path() const
{
	return localName_ + L".fzseg";
}

std::wstring CSftpFileTransferOpData::segment_part_path(size_t i) const
{
	return fz::sprintf(L"%s.fzseg%d", localName_, static_cast<int>(i));
}

void CSftpFileTransferOpData::delete_segment_files(size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		fz::remove_file(fz::to_native(segment_part_path(i)), false);
	}
	fz::remove_file(fz::to_native(segment_manifest_path()), false);
}

bool CSftpFileTransferOpData::read_segment_manifest(uint64_t total)
{
	fz::file f(fz::to_native(segment_manifest_path()), fz::file::reading);
	if (!f.opened()) {
		return false;
	}

	int64_t const size = f.size();
	if (size <= 0 || size > 64 * 1024) {
		return false;
	}

	std::string data;
	data.resize(static_cast<size_t>(size));
	int64_t read = 0;
	while (read < size) {
		int64_t const r = f.read(data.data() + read, size - read);
		if (r <= 0) {
			return false;
		}
		read += r;
	}

	auto parse_pair = [](std::string_view line, uint64_t & a, uint64_t & b) {
		auto toks = fz::strtokenizer(line, " "sv, true);
		auto it = toks.begin();
		if (it == toks.end()) {
			return false;
		}
		a = fz::to_integral<uint64_t>(*it, fz::aio_base::nosize);
		++it;
		if (it == toks.end()) {
			return false;
		}
		b = fz::to_integral<uint64_t>(*it, fz::aio_base::nosize);
		++it;
		return it == toks.end() && a != fz::aio_base::nosize && b != fz::aio_base::nosize;
	};

	auto lines = fz::strtokenizer(data, "\r\n"sv, true);
	auto it = lines.begin();
	if (it == lines.end() || *it != "fzseg1"sv) {
		return false;
	}
	++it;

	uint64_t mtotal{};
	uint64_t mcount{};
	if (it == lines.end() || !parse_pair(*it, mtotal, mcount)) {
		return false;
	}
	++it;

	if (mtotal != total || mcount != segments_.size()) {
		return false;
	}

	for (auto & seg : segments_) {
		if (it == lines.end()) {
			return false;
		}
		uint64_t moffset{};
		uint64_t mlength{};
		if (!parse_pair(*it, moffset, mlength) || moffset != seg->offset_ || mlength != seg->length_) {
			return false;
		}
		++it;
	}

	return it == lines.end();
}

bool CSftpFileTransferOpData::write_segment_manifest(uint64_t total)
{
	std::string data = "fzseg1\n";
	data += std::to_string(total) + " " + std::to_string(segments_.size()) + "\n";
	for (auto & seg : segments_) {
		data += std::to_string(seg->offset_) + " " + std::to_string(seg->length_) + "\n";
	}

	fz::file f(fz::to_native(segment_manifest_path()), fz::file::writing, fz::file::empty);
	if (!f.opened()) {
		return false;
	}

	size_t written = 0;
	while (written < data.size()) {
		int64_t const w = f.write(data.data() + written, static_cast<int64_t>(data.size() - written));
		if (w <= 0) {
			return false;
		}
		written += static_cast<size_t>(w);
	}
	return true;
}

int CSftpFileTransferOpData::start_segmented(size_t count)
{
	uint64_t const total = static_cast<uint64_t>(remoteFileSize_);

	std::string const remoteFile = controlSocket_.ConvToServer(remotePath_.FormatFilename(remoteFile_));
	if (remoteFile.empty()) {
		log(logmsg::error, _("Could not convert command to server encoding"));
		return FZ_REPLY_ERROR;
	}

	// Deterministic equal split of the file into contiguous ranges
	uint64_t const base = total / count;
	uint64_t const remainder = total % count;
	uint64_t pos = 0;
	for (size_t i = 0; i < count; ++i) {
		uint64_t const length = base + (i < remainder ? 1 : 0);
		segments_.push_back(std::make_unique<segment_handler>(*this, i, pos, length));
		pos += length;
	}

	bool resume_valid = read_segment_manifest(total);
	if (resume_valid) {
		for (size_t i = 0; i < count; ++i) {
			int64_t const size = fz::local_filesys::get_size(fz::to_native(segment_part_path(i)));
			uint64_t const part_size = (size > 0) ? static_cast<uint64_t>(size) : 0;
			if (part_size > segments_[i]->length_) {
				resume_valid = false;
				break;
			}
			segments_[i]->part_size_ = part_size;
		}
	}

	if (!resume_valid) {
		delete_segment_files(max_segment_count_);
		if (!write_segment_manifest(total)) {
			log(logmsg::error, fztranslate("Could not write the segment manifest file"));
			return FZ_REPLY_WRITEFAILED;
		}
	}

	uint64_t resumed = 0;
	for (size_t i = 0; i < count; ++i) {
		auto & seg = *segments_[i];
		resumed += seg.part_size_;
		seg.received_ = seg.part_size_;
		seg.request_offset_ = seg.offset_ + seg.part_size_;
		seg.writer_factory_ = std::make_unique<fz::file_writer_factory>(segment_part_path(i), engine_.GetThreadPool());
		if (seg.part_size_ >= seg.length_) {
			seg.done_ = true;
			++segments_done_;
		}
	}

	engine_.transfer_status_.Init(remoteFileSize_, resumed, false);
	engine_.transfer_status_.SetStartTime();
	transferInitiated_ = true;
	controlSocket_.SetWait(true);

	std::wstring logstr = L"get ";
	logstr += controlSocket_.QuoteFilename(remotePath_.FormatFilename(remoteFile_)) + L" ";
	logstr += controlSocket_.QuoteFilename(localName_);
	controlSocket_.log_raw(logmsg::command, logstr);
	log(logmsg::status, fztranslate("Downloading file in %d segments"), static_cast<int>(count));

	opState = filetransfer_segmented;

	if (segments_done_ == count) {
		start_concat();
		return FZ_REPLY_WOULDBLOCK;
	}

	segmented_remote_file_ = remoteFile;

	for (auto & seg : segments_) {
		if (!seg->done_) {
			sftp_->open(seg.get(), remoteFile, fz::ssh::sftp::file_flags::SSH_FXF_READ);
		}
	}

	// Bring up additional connections for the segments, one at a time
	start_next_segment_connection();

	return FZ_REPLY_WOULDBLOCK;
}

CSftpOpData::continuation CSftpFileTransferOpData::on_segment_open(segment_handler & seg, std::string_view handle)
{
	controlSocket_.SetAlive();

	seg.handle_ = handle;

	if (seg_failed_) {
		return continuation::next;
	}

	if (!seg.writer_) {
		seg.writer_ = controlSocket_.OpenWriter(seg.writer_factory_, seg.part_size_, true);
		if (!seg.writer_) {
			seg_failed_ = true;
			trigger_reset(FZ_REPLY_WRITEFAILED);
			return continuation::next;
		}

		fz::aio_result r = get_next_segment_buffer(seg);
		if (r == fz::aio_result::wait) {
			seg.stall_channel_ = segment_channel(seg);
			return continuation::wait;
		}
		if (r == fz::aio_result::error) {
			return continuation::next;
		}
	}

	request_segments(seg.conn_);
	return continuation::next;
}

CSftpOpData::continuation CSftpFileTransferOpData::process_segment_data(segment_handler & seg, std::string_view data)
{
	controlSocket_.SetAlive();

	if (seg_failed_) {
		return continuation::next;
	}

	if (data.size() > blocksize_) {
		log(fz::logmsg::error, L"Server sent a block of data larger than requested."sv);
		return continuation::error;
	}

	if (seg.outstanding_.empty() || !seg.buffer_) {
		log(fz::logmsg::error, L"Server sent unexpected data."sv);
		return continuation::error;
	}

	uint32_t const expected = seg.outstanding_.front();
	seg.outstanding_.pop_front();

	if (data.size() > expected) {
		log(fz::logmsg::error, L"Server sent a block of data larger than requested."sv);
		return continuation::error;
	}

	if (data.size() < expected) {
		// Requests tile the segment exactly and the part file is sequential-append,
		// so a short read would leave a hole that could never be repaired.
		log(fz::logmsg::error, fztranslate("Got a short read not at the end of a file, this is not permissiable on normal files"));
		seg_failed_ = true;
		trigger_reset(FZ_REPLY_ERROR);
		return continuation::next;
	}

	seg.buffer_->append(data);

	seg.received_ += data.size();
	segment_pipe(seg).response_offset_ += data.size();

	if (seg.received_ >= seg.length_) {
		finalize_segment(seg);
		return continuation::next;
	}

	if (seg.buffer_->capacity() - seg.buffer_->size() < blocksize_) {
		fz::aio_result r = get_next_segment_buffer(seg);
		if (r == fz::aio_result::wait) {
			seg.stall_channel_ = segment_channel(seg);
			return continuation::wait;
		}
		else if (r == fz::aio_result::error) {
			return continuation::next;
		}
	}

	request_segments(seg.conn_);

	return continuation::next;
}

CSftpOpData::continuation CSftpFileTransferOpData::process_segment_status(segment_handler & seg, fz::ssh::sftp::status_code code, std::wstring_view msg)
{
	controlSocket_.SetAlive();

	if (seg.handle_.empty()) {
		log(logmsg::error, fztranslate("Could not open remote file: %s"), msg);
		if (seg.received_ == seg.part_size_) {
			log(logmsg::status, fztranslate("The server may limit the number of concurrently open files. Lowering the number of SFTP download segments in the settings might help."));
		}
		seg_failed_ = true;
		trigger_reset(FZ_REPLY_ERROR);
	}
	else if (code == fz::ssh::sftp::status_code::SSH_FX_EOF) {
		if (!seg.outstanding_.empty()) {
			seg.outstanding_.pop_front();
		}
		if (seg.received_ >= seg.length_) {
			finalize_segment(seg);
		}
		else {
			log(logmsg::error, fztranslate("Remote file ended before the segment was complete"));
			seg_failed_ = true;
			trigger_reset(FZ_REPLY_ERROR);
		}
	}
	else {
		log(logmsg::error, fztranslate("Could not read from remote file: %s"), msg);
		seg_failed_ = true;
		trigger_reset(FZ_REPLY_ERROR);
	}

	return continuation::next;
}

fz::aio_result CSftpFileTransferOpData::get_next_segment_buffer(segment_handler & seg)
{
	fz::aio_result r = seg.writer_->add_buffer(std::move(seg.buffer_), seg);
	if (r == fz::aio_result::ok) {
		seg.buffer_ = controlSocket_.buffer_pool_->get_buffer(seg);
		if (!seg.buffer_) {
			r = fz::aio_result::wait;
		}
	}
	if (r == fz::aio_result::error) {
		log(logmsg::error, fztranslate("Could not write to local file"));
		seg_failed_ = true;
		trigger_reset(FZ_REPLY_WRITEFAILED);
	}
	return r;
}

void CSftpFileTransferOpData::finalize_segment(segment_handler & seg)
{
	seg.finalizing_ = true;
	fz::aio_result r = seg.writer_->add_buffer(std::move(seg.buffer_), seg);
	if (r == fz::aio_result::ok) {
		r = seg.writer_->finalize(seg);
	}
	if (r == fz::aio_result::error) {
		log(logmsg::error, fztranslate("Could not write to local file"));
		seg_failed_ = true;
		trigger_reset(FZ_REPLY_WRITEFAILED);
	}
	else if (r == fz::aio_result::ok) {
		seg.writer_.reset();
		seg.done_ = true;
		++segments_done_;
		if (segments_done_ == segments_.size()) {
			start_concat();
		}
	}
}

fz::ssh::sftp::sftp_client* CSftpFileTransferOpData::segment_channel(segment_handler & seg)
{
	return seg.conn_ ? seg.conn_->sftp_.get() : sftp_.get();
}

segment_pipe_state & CSftpFileTransferOpData::segment_pipe(segment_handler & seg)
{
	return seg.conn_ ? seg.conn_->pipe_ : primary_pipe_;
}

CSftpFileTransferOpData::segment_handler* CSftpFileTransferOpData::next_request_segment(segment_connection* conn)
{
	size_t const count = segments_.size();
	auto & pipe = conn ? conn->pipe_ : primary_pipe_;
	for (size_t i = 0; i < count; ++i) {
		size_t const idx = (pipe.next_segment_ + i) % count;
		auto & seg = *segments_[idx];
		if (seg.conn_ == conn && !seg.handle_.empty() && !seg.finalizing_ && !seg.done_ && seg.buffer_ &&
			seg.request_offset_ < seg.offset_ + seg.length_)
		{
			pipe.next_segment_ = (idx + 1) % count;
			return &seg;
		}
	}
	return nullptr;
}

void CSftpFileTransferOpData::request_segments(segment_connection* conn)
{
	if (seg_failed_) {
		return;
	}

	auto & pipe = conn ? conn->pipe_ : primary_pipe_;
	auto * sftp = conn ? conn->sftp_.get() : sftp_.get();
	if (!sftp) {
		return;
	}

	if (pipe.rtt_requested_ && pipe.response_offset_ >= pipe.rtt_offset_) {
		// We've got the RTT
		auto ms = (fz::monotonic_clock::now() - pipe.rtt_requested_).get_milliseconds();
		pipe.rtt_requested_ = fz::monotonic_clock();

		if (ms < 1) {
			ms = 1;
		}

		// Aim for 500ms between queuing the request and the reply
		size_t ideal_pending = pipe.max_pending_ * 500 / ms;

		if (ideal_pending > pipe.max_pending_) {
			size_t divisor = (ideal_pending > pipe.max_pending_ * 2) ? 2 : 8;
			pipe.max_pending_ += pipe.max_pending_ / divisor;
			if (pipe.max_pending_ > 1024*16) {
				pipe.max_pending_ = 1024*16;
			}
		}
		else if (ideal_pending < pipe.max_pending_) {
			size_t divisor = (ideal_pending * 2 < pipe.max_pending_) ? 2 : 8;
			pipe.max_pending_ -= pipe.max_pending_ / divisor;
			if (pipe.max_pending_ < initial_max_pending_) {
				pipe.max_pending_ = initial_max_pending_;
			}
		}
	}
	else if (pipe.rtt_requested_ && pipe.response_offset_ == pipe.requested_offset_) {
		// All requests have been answered without reaching the measurement mark,
		// it can no longer be reached. Abandon the measurement.
		pipe.rtt_requested_ = fz::monotonic_clock();
	}

	size_t i{};
	for (i = 0; i < 2 && sftp->pending_requests() < pipe.max_pending_ && sftp->can_send_packets(); ++i) {
		segment_handler* seg = next_request_segment(conn);
		if (!seg) {
			break;
		}
		uint64_t const remaining = seg->offset_ + seg->length_ - seg->request_offset_;
		uint32_t const size = static_cast<uint32_t>(std::min<uint64_t>(blocksize_, remaining));
		seg->outstanding_.push_back(size);
		sftp->read(seg, seg->handle_, seg->request_offset_, size);
		seg->request_offset_ += size;
		pipe.requested_offset_ += size;
	}

	if (i && sftp->pending_requests() == pipe.max_pending_ && !pipe.rtt_requested_) {
		// Begin RTT measurement at full capacity
		pipe.rtt_requested_ = fz::monotonic_clock::now();
		pipe.rtt_offset_ = pipe.requested_offset_;
	}
}

void CSftpFileTransferOpData::on_segment_buffer_availability(segment_handler & seg, fz::aio_waitable const* w)
{
	if (seg_failed_) {
		return;
	}

	if (w == seg.writer_.get()) {
		if (seg.finalizing_) {
			finalize_segment(seg);
		}
		else {
			fz::aio_result r = get_next_segment_buffer(seg);
			if (r == fz::aio_result::ok) {
				if (seg.stall_channel_) {
					seg.stall_channel_->unblock_read();
					seg.stall_channel_ = nullptr;
				}
				request_segments(seg.conn_);
			}
		}
	}
	else if (!seg.buffer_ && !seg.finalizing_ && !seg.done_) {
		// The segment was waiting for a buffer from the pool while its writer was idle
		seg.buffer_ = controlSocket_.buffer_pool_->get_buffer(seg);
		if (seg.buffer_) {
			if (seg.stall_channel_) {
				seg.stall_channel_->unblock_read();
				seg.stall_channel_ = nullptr;
			}
			request_segments(seg.conn_);
		}
	}
}

void CSftpFileTransferOpData::migrate_segment(segment_handler & seg, segment_connection & conn)
{
	if (auto * old = segment_channel(seg)) {
		old->cancel(&seg);
		if (!seg.handle_.empty()) {
			old->close(nullptr, seg.handle_);
			seg.handle_.clear();
		}
	}

	// The cancelled requests will never be answered, keep the old
	// channel's pipelining counters balanced
	uint64_t lost = 0;
	for (auto s : seg.outstanding_) {
		lost += s;
	}
	segment_pipe(seg).requested_offset_ -= lost;
	seg.outstanding_.clear();

	seg.request_offset_ = seg.offset_ + seg.received_;
	seg.conn_ = &conn;
	conn.sftp_->open(&seg, segmented_remote_file_, fz::ssh::sftp::file_flags::SSH_FXF_READ);
}

void CSftpFileTransferOpData::start_next_segment_connection()
{
	if (opState != filetransfer_segmented || seg_failed_) {
		return;
	}
	if (segment_connections_.size() + 1 < segments_.size()) {
		segment_connections_.push_back(std::make_unique<segment_connection>(*this, controlSocket_));
		segment_connections_.back()->start();
	}
}

void CSftpFileTransferOpData::on_segment_connection_ready(segment_connection & conn)
{
	if (opState != filetransfer_segmented || seg_failed_) {
		return;
	}

	size_t target = 0;
	for (size_t i = 0; i < segment_connections_.size(); ++i) {
		if (segment_connections_[i].get() == &conn) {
			target = i + 1;
			break;
		}
	}
	if (!target) {
		return;
	}

	log(logmsg::status, fztranslate("Established additional connection %d of %d for the segmented download"), static_cast<int>(target), static_cast<int>(segments_.size() - 1));

	for (auto & seg : segments_) {
		if (!seg->conn_ && !seg->done_ && !seg->finalizing_ && seg->index_ == target) {
			migrate_segment(*seg, conn);
		}
	}

	start_next_segment_connection();
}

void CSftpFileTransferOpData::on_segment_connection_failed(segment_connection &)
{
	// The server may limit the number of concurrent connections. The segments
	// stay on the remaining connections.
	if (opState != filetransfer_segmented || seg_failed_) {
		return;
	}
	log(logmsg::status, fztranslate("Could not establish an additional connection, continuing the segmented download with fewer connections"));
	start_next_segment_connection();
}

void CSftpFileTransferOpData::on_segment_connection_dropped(segment_connection &)
{
	if (opState != filetransfer_segmented || seg_failed_) {
		return;
	}
	log(logmsg::error, fztranslate("An additional connection of the segmented download was lost"));
	seg_failed_ = true;
	trigger_reset(FZ_REPLY_ERROR);
}

void CSftpFileTransferOpData::on_segment_hostkey_mismatch()
{
	seg_failed_ = true;
	trigger_reset(FZ_REPLY_CRITICALERROR);
}

void CSftpFileTransferOpData::start_concat()
{
	opState = filetransfer_concat;

	log(logmsg::status, fztranslate("Merging segments..."));

	for (auto & seg : segments_) {
		if (!seg->handle_.empty()) {
			if (auto * channel = segment_channel(seg)) {
				channel->close(nullptr, seg->handle_);
			}
			seg->handle_.clear();
		}
	}

	writer_ = controlSocket_.OpenWriter(writer_factory_, 0, false);
	if (!writer_) {
		trigger_reset(FZ_REPLY_WRITEFAILED);
		return;
	}

	concat_part_ = 0;
	concat_received_ = 0;
	concat_pump();
}

void CSftpFileTransferOpData::concat_pump()
{
	while (opState == filetransfer_concat && !concat_finalizing_) {
		if (!reader_) {
			if (concat_part_ >= segments_.size()) {
				if (concat_received_ != static_cast<uint64_t>(remoteFileSize_)) {
					log(logmsg::error, fztranslate("Downloaded segments are incomplete"));
					trigger_reset(FZ_REPLY_ERROR);
					return;
				}
				concat_finalize();
				return;
			}

			concat_reader_factory_ = std::make_unique<fz::file_reader_factory>(segment_part_path(concat_part_), engine_.GetThreadPool());
			reader_ = concat_reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize, controlSocket_.max_buffer_count());
			if (!reader_) {
				log(logmsg::error, fztranslate("Could not open segment file for reading"));
				trigger_reset(FZ_REPLY_ERROR);
				return;
			}
			++concat_part_;
		}

		if (!buffer_) {
			fz::aio_result r;
			std::tie(r, buffer_) = reader_->get_buffer(*this);
			if (r == fz::aio_result::wait) {
				return;
			}
			if (r == fz::aio_result::error) {
				log(logmsg::error, fztranslate("Could not read from local file"));
				trigger_reset(FZ_REPLY_ERROR);
				return;
			}
			if (buffer_->empty()) {
				buffer_.release();
				reader_.reset();
				continue;
			}
		}

		concat_received_ += buffer_->size();

		fz::aio_result const r = writer_->add_buffer(std::move(buffer_), *this);
		if (r == fz::aio_result::error) {
			log(logmsg::error, fztranslate("Could not write to local file"));
			trigger_reset(FZ_REPLY_WRITEFAILED);
			return;
		}
		if (r == fz::aio_result::wait) {
			return;
		}
	}
}

void CSftpFileTransferOpData::concat_finalize()
{
	concat_finalizing_ = true;
	fz::aio_result const r = writer_->finalize(*this);
	if (r == fz::aio_result::error) {
		log(logmsg::error, fztranslate("Could not write to local file"));
		trigger_reset(FZ_REPLY_WRITEFAILED);
		return;
	}
	if (r == fz::aio_result::ok) {
		if (options_.get_int(OPTION_PRESERVE_TIMESTAMPS) && remoteFileTime_) {
			if (!writer_->set_mtime(remoteFileTime_)) {
				log(logmsg::debug_warning, L"Could not set modification time");
			}
		}
		reader_.reset();
		writer_.reset();
		delete_segment_files(segments_.size());
		trigger_reset(FZ_REPLY_OK);
	}
}
