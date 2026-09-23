#ifndef FILEZILLA_ENGINE_SFTP_FILETRANSFER_HEADER
#define FILEZILLA_ENGINE_SFTP_FILETRANSFER_HEADER

#include "segment_connection.h"

#include <deque>
#include <vector>

class CSftpFileTransferOpData final : public CFileTransferOpData, public CSftpOpData
{
public:
	CSftpFileTransferOpData(CSftpControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CSftpFileTransferOpData", cmd)
		, CSftpOpData(controlSocket)
	{}

	~CSftpFileTransferOpData();

	virtual int Send() override;
	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }
	virtual int SubcommandResult(int, COpData const&) override;
	virtual int Reset(int result) override;

private:
	virtual continuation process_handle(std::string_view handle) override;
	virtual continuation process_data(std::string_view data) override;
	virtual continuation do_process_status(fz::ssh::sftp::status_code code, std::wstring_view msg) override;
	virtual continuation process_attributes(fz::ssh::sftp::attributes & attrs) override;

	virtual void operator()(fz::event_base const& ev) override;
	void OnBufferAvailability(fz::aio_waitable const* w);
	void on_can_send(fz::ssh::sftp::sftp_client*);
	void OnTimer(fz::timer_id);

	fz::aio_result get_next_download_buffer();
	fz::aio_result get_next_upload_buffer();
	void finalize();
	void request_data();

	int CheckRemoteFile(bool after_listing);

	// Segmented downloads
	class segment_handler final : public fz::ssh::sftp::response_handler, public fz::event_handler
	{
	public:
		segment_handler(CSftpFileTransferOpData & op, size_t index, uint64_t offset, uint64_t length);
		~segment_handler();

		virtual fz::ssh::sftp::continuation process_status(fz::ssh::sftp::status_code, std::string_view description) override;
		virtual fz::ssh::sftp::continuation process_handle(std::string_view handle) override;
		virtual fz::ssh::sftp::continuation process_data(std::string_view data) override;
		virtual fz::ssh::sftp::continuation failure() override;

		virtual void operator()(fz::event_base const& ev) override;
		void OnBufferAvailability(fz::aio_waitable const* w);

		CSftpFileTransferOpData & op_;

		size_t const index_{};

		uint64_t const offset_{};
		uint64_t const length_{};
		uint64_t received_{};
		uint64_t part_size_{};
		uint64_t request_offset_{};

		// nullptr: the primary connection
		segment_connection* conn_{};

		std::string handle_;

		fz::writer_factory_holder writer_factory_;
		std::unique_ptr<fz::writer_base> writer_;
		fz::buffer_lease buffer_;

		// Sizes of unanswered read requests, in request order
		std::deque<uint32_t> outstanding_;

		// The sftp channel on which this segment stalled delivery
		fz::ssh::sftp::sftp_client* stall_channel_{};

		bool finalizing_{};
		bool done_{};
	};

	friend class segment_connection;

	size_t segment_count() const;
	int start_segmented(size_t count);

	std::wstring segment_manifest_path() const;
	std::wstring segment_part_path(size_t i) const;
	bool read_segment_manifest(uint64_t total);
	bool write_segment_manifest(uint64_t total);
	void delete_segment_files(size_t count);
	void cleanup_segments();

	fz::ssh::sftp::sftp_client* segment_channel(segment_handler & seg);
	segment_pipe_state & segment_pipe(segment_handler & seg);

	continuation on_segment_open(segment_handler & seg, std::string_view handle);
	continuation process_segment_data(segment_handler & seg, std::string_view data);
	continuation process_segment_status(segment_handler & seg, fz::ssh::sftp::status_code code, std::wstring_view msg);
	void on_segment_buffer_availability(segment_handler & seg, fz::aio_waitable const* w);
	fz::aio_result get_next_segment_buffer(segment_handler & seg);
	void finalize_segment(segment_handler & seg);
	void request_segments(segment_connection* conn);
	segment_handler* next_request_segment(segment_connection* conn);
	void migrate_segment(segment_handler & seg, segment_connection & conn);

	void start_next_segment_connection();
	void on_segment_connection_ready(segment_connection & conn);
	void on_segment_connection_failed(segment_connection & conn);
	void on_segment_connection_dropped(segment_connection & conn);
	void on_segment_hostkey_mismatch();

	void start_concat();
	void concat_pump();
	void concat_finalize();

	uint64_t request_offset_{};
	uint64_t response_offset_{};

	std::unique_ptr<fz::reader_base> reader_;
	std::unique_ptr<fz::writer_base> writer_;
	bool finalizing_{};
	bool short_read_{};
	bool called_fsetstat_{};

	fz::buffer_lease buffer_;

	std::string handle_;

	struct rtt {
		fz::monotonic_clock requested_;
		uint64_t offset_{};
	} rtt_;

	std::vector<std::unique_ptr<segment_handler>> segments_;
	size_t segments_done_{};
	segment_pipe_state primary_pipe_;
	bool seg_failed_{};

	std::vector<std::unique_ptr<segment_connection>> segment_connections_;
	std::string segmented_remote_file_;

	fz::reader_factory_holder concat_reader_factory_;
	size_t concat_part_{};
	uint64_t concat_received_{};
	bool concat_finalizing_{};

	constexpr static size_t max_segment_count_{8};

	constexpr static size_t blocksize_{32768};
	constexpr static size_t initial_max_pending_{16};
	size_t max_pending_{initial_max_pending_};
};

#endif
