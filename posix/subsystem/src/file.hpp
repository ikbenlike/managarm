#ifndef POSIX_SUBSYSTEM_FILE_HPP
#define POSIX_SUBSYSTEM_FILE_HPP

#include <variant>
#include <string.h> // for hel.h

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>
#include <protocols/fs/server.hpp>
#include "common.hpp"

struct File;
struct FsLink;
struct Process;

struct FileHandle { };

using SharedFilePtr = smarter::shared_ptr<File, FileHandle>;

// TODO: Rename this enum as is not part of the VFS.
enum class Error {
	success,
	noSuchFile,
	eof,
	fileClosed,

	// Indices that the given object does not support the operation
	// (e.g. readSymlink() is called on a file that is not a link).
	illegalOperationTarget,

	seekOnPipe,

	wouldBlock,

	brokenPipe
};

// TODO: Rename this enum as is not part of the VFS.
enum class VfsSeek {
	null, absolute, relative, eof
};

template<typename T>
using FutureMaybe = async::result<T>;

template<typename T>
using expected = async::result<std::variant<Error, T>>;

// ----------------------------------------------------------------------------

using MsgFlags = uint32_t;

inline constexpr MsgFlags msgNoWait = (1 << 1);
inline constexpr MsgFlags msgCloseOnExec = (1 << 2);

// ----------------------------------------------------------------------------
// File class.
// ----------------------------------------------------------------------------

using ReadEntriesResult = std::optional<std::string>;

using PollResult = std::tuple<uint64_t, int, int>;

using RecvResult = std::tuple<
	size_t,
	size_t,
	std::vector<char>
>;

using AcceptResult = smarter::shared_ptr<File, FileHandle>;

struct DisposeFileHandle { };

struct File : private smarter::crtp_counter<File, DisposeFileHandle> {
	friend struct smarter::crtp_counter<File, DisposeFileHandle>;
public:
	using DefaultOps = uint32_t;
	static inline constexpr DefaultOps defaultIsTerminal = 1 << 1;
	static inline constexpr DefaultOps defaultPipeLikeSeek = 1 << 2;

	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------

	static async::result<protocols::fs::SeekResult>
	ptSeekRel(void *object, int64_t offset);

	static async::result<protocols::fs::SeekResult>
	ptSeekAbs(void *object, int64_t offset);

	static async::result<protocols::fs::ReadResult>
	ptRead(void *object, const char *credentials, void *buffer, size_t length);

	static async::result<void>
	ptWrite(void *object, const char *credentials, const void *buffer, size_t length);

	static async::result<protocols::fs::ReadEntriesResult>
	ptReadEntries(void *object);

	static async::result<void>
	ptTruncate(void *object, size_t size);

	static async::result<void>
	ptAllocate(void *object, int64_t offset, size_t size);

	static async::result<int>
	ptGetOption(void *object, int option);

	static async::result<void>
	ptSetOption(void *object, int option, int value);

	static async::result<void>
	ptBind(void *object, const char *credentials,
			const void *addr_ptr, size_t addr_length);

	static async::result<void>
	ptConnect(void *object, const char *credentials,
			const void *addr_ptr, size_t addr_length);

	static async::result<size_t>
	ptSockname(void *object, void *addr_ptr, size_t max_addr_length);

	static async::result<void> 
	ptIoctl(void *object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);

	static constexpr auto fileOperations = protocols::fs::FileOperations{}
			.withSeekAbs(&ptSeekAbs)
			.withSeekRel(&ptSeekRel)
			.withRead(&ptRead)
			.withWrite(&ptWrite)
			.withReadEntries(&ptReadEntries)
			.withTruncate(&ptTruncate)
			.withFallocate(&ptAllocate)
			.withGetOption(&ptGetOption)
			.withSetOption(&ptSetOption)
			.withBind(&ptBind)
			.withConnect(&ptConnect)
			.withSockname(&ptSockname)
			.withIoctl(&ptIoctl);

	// ------------------------------------------------------------------------
	// Public File API.
	// ------------------------------------------------------------------------

	static smarter::shared_ptr<File, FileHandle> constructHandle(smarter::shared_ptr<File> ptr) {
		auto [file, object_ctr] = ptr.release();
		file->setup(smarter::adopt_rc, object_ctr, 1);
		return smarter::shared_ptr<File, FileHandle>{smarter::adopt_rc, file, file};
	}

	File(StructName struct_name, DefaultOps default_ops = 0)
	: _structName{struct_name}, _link{nullptr}, _defaultOps{default_ops}, _isOpen{true} { }

	File(StructName struct_name, std::shared_ptr<FsLink> link, DefaultOps default_ops = 0)
	: _structName{struct_name}, _link{std::move(link)}, _defaultOps{default_ops}, _isOpen{true} { }

	virtual ~File();

// TODO: Make this protected:
	void setupWeakFile(smarter::weak_ptr<File> ptr) {
		_weakPtr = std::move(ptr);
	}

public:
	const smarter::weak_ptr<File> &weakFile() {
		return _weakPtr;
	}

	StructName structName() {
		return _structName;
	}

	bool isOpen() {
		return _isOpen;
	}

	virtual void handleClose();

private:
	void dispose(DisposeFileHandle) {
		_isOpen = false;
		handleClose();
	}

public:
	// This is the link that was used to open the file.
	// Note that this might not be the only link that can be used
	// to reach the file's inode.
	std::shared_ptr<FsLink> associatedLink() {
		return _link;
	}

	bool isTerminal();

	FutureMaybe<void> readExactly(Process *process, void *data, size_t length);

	virtual expected<off_t> seek(off_t offset, VfsSeek whence);

	virtual expected<size_t> readSome(Process *process, void *data, size_t max_length);

	virtual FutureMaybe<void> writeAll(Process *process, const void *data, size_t length);

	virtual FutureMaybe<ReadEntriesResult> readEntries();

	virtual expected<RecvResult> recvMsg(Process *process, MsgFlags flags,
			void *data, size_t max_length,
			void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length);

	virtual expected<size_t> sendMsg(Process *process, MsgFlags flags,
			const void *data, size_t max_length,
			const void *addr_ptr, size_t addr_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files);

	virtual async::result<void> truncate(size_t size);

	virtual async::result<void> allocate(int64_t offset, size_t size);

	// poll() uses a sequence number mechansim for synchronization.
	// Before returning, it waits until current-sequence > in-sequence.
	// Returns (current-sequence, edges since in-sequence, current events).
	// current-sequence is incremented each time an edge (i.e. an event bit
	// transitions from clear to set) happens.
	// TODO: This request should be cancelable.
	virtual expected<PollResult> poll(Process *, uint64_t sequence);

	// Like poll, but only checks the current state. Does not return edges.
	virtual expected<PollResult> checkStatus(Process *);

	virtual async::result<int> getOption(int option);
	virtual async::result<void> setOption(int option, int value);

	virtual async::result<AcceptResult> accept(Process *process);

	virtual async::result<void> bind(Process *process,
			const void *addr_ptr, size_t addr_length);

	virtual async::result<void> connect(Process *process,
			const void *addr_ptr, size_t addr_length);

	virtual async::result<size_t> sockname(void *addr_ptr, size_t max_addr_length);

	// TODO: This should not depend on an offset.
	// Due to missing support from the kernel, we currently need multiple memory
	// objects per file for DRM device files.
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory(off_t offset = 0);

	virtual async::result<void> ioctl(Process *process, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);

	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;

private:
	smarter::weak_ptr<File> _weakPtr;
	StructName _structName;
	const std::shared_ptr<FsLink> _link;

	DefaultOps _defaultOps;

	bool _isOpen;
};

#endif // POSIX_SUBSYSTEM_FILE_HPP
