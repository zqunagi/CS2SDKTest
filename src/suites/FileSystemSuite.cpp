#include "suites/FileSystemSuite.h"

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include <appframework/iappsystem.h>
#include <filesystem/ifilesystem.h>
#include <interfaces/interfaces.h>
#include <tier0/buffer_string.h>
#include <tier0/utlbuffer.h>
#include <tier0/utlstring.h>
#include <tier0/utlvector.h>

#include "core/Log.h"

namespace cs2st
{
namespace
{

// Present in every Source 2 game's content root; resolves through "GAME".
const char* const kProbeFile = "gameinfo.gi";
const char* const kProbePathId = "GAME";
// A name no content root has; the negative control for the existence probes.
const char* const kMissingFile = "cs2sdktest_no_such_file.bin";

const char* TierName(AppSystemTier_t tier)
{
	switch (tier)
	{
		case APP_SYSTEM_TIER0:      return "TIER0";
		case APP_SYSTEM_TIER1:      return "TIER1";
		case APP_SYSTEM_TIER2:      return "TIER2";
		case APP_SYSTEM_TIER3:      return "TIER3";
		case APP_SYSTEM_TIER4:      return "TIER4";
		case APP_SYSTEM_TIER_OTHER: return "TIER_OTHER";
		default:                    return "<out of range>";
	}
}

const char* YesNo(bool value)
{
	return value ? "true" : "false";
}

/** Path equality that ignores case and the two separator spellings. */
bool SamePath(const char* a, const char* b)
{
	if (a == nullptr || b == nullptr)
	{
		return a == b;
	}
	for (;; ++a, ++b)
	{
		const char ca = *a == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
		const char cb = *b == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
		if (ca != cb)
		{
			return false;
		}
		if (ca == '\0')
		{
			return true;
		}
	}
}

/**
 * pfnAlloc for the calls that let the caller own the allocation. Using this
 * module's malloc keeps allocation and free on one heap (the probe frees what
 * it is handed with free()); CS2SDKTEST_USE_MEMOVERRIDE is off.
 */
void* ProbeAlloc(const char* /*file_name*/, unsigned bytes)
{
	return std::malloc(bytes != 0 ? bytes : 1);
}

/**
 * Raw storage for a container the engine fills with its own allocations
 * (CUtlBuffer, CUtlVector<CUtlString>). The object is constructed in place and
 * never destroyed: its destructor would free engine-allocated memory on this
 * module's heap, and a cross-heap free aborts the process. The few bytes that
 * stay allocated go back to the OS when the process exits. One instance per
 * call site; each site runs once per process.
 */
template <typename T>
struct LeakedObject
{
	alignas(T) unsigned char bytes[sizeof(T)];

	template <typename... Args>
	T* Construct(Args&&... args)
	{
		return new (static_cast<void*>(bytes)) T(std::forward<Args>(args)...);
	}
};

// Everything the write-side rows create lives under these names, in the
// "GAME" path ID, and is removed again before the suite ends.
const char* const kScratchDir = "cstmp_fs";
const char* const kScratchTree = "cstmp_fs_tree";
const char* const kScratchTreeInner = "cstmp_fs_tree/inner";
const char* const kScratchTreeFile = "cstmp_fs_tree/inner/f.txt";
const char* const kScratchFileA = "cstmp_fs/a.txt";
const char* const kScratchFileB = "cstmp_fs/b.txt";
const char* const kScratchFileC = "cstmp_fs/c.txt";
const char* const kScratchFileW = "cstmp_fs/w.bin";
// A path ID nothing else uses, so mounting and unmounting it cannot change
// where any real file resolves.
const char* const kScratchPathId = "cstmp_pathid";

/**
 * Appends printf-formatted text. A row keeps only its last Detail, so a row
 * that makes several calls builds its text here and reports it once.
 */
void AppendF(std::string& out, const char* format, ...)
{
	char piece[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(piece, sizeof(piece), format, args);
	va_end(args);
	out += piece;
}

/** The directory part of a full path, without the trailing separator. */
std::string DirectoryOf(const char* full_path)
{
	std::string path(full_path != nullptr ? full_path : "");
	const std::size_t cut = path.find_last_of("/\\");
	return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

/** Reads up to capacity bytes of a file by name; -1 when it cannot be opened. */
int ReadBack(IFileSystem* fs, const char* name, char* out, int capacity)
{
	FileHandle_t file = fs->Open(name, "rb", kProbePathId);
	if (file == FILESYSTEM_INVALID_HANDLE)
	{
		return -1;
	}
	const int got = fs->Read(out, capacity, file);
	fs->Close(file);
	return got;
}

FileFindHandle_t InvalidFindHandle()
{
	FileFindHandle_t find;
	find.m_pFileSystem = nullptr;
	find.m_nFindDataIndex = FILESYSTEM_INVALID_FIND_HANDLE;
	return find;
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool FileSystemSuite::Acquire(TestContext& context)
{
	fs_ = static_cast<IFileSystem*>(
		context.QueryInterface("filesystem_stdio", FILESYSTEM_INTERFACE_VERSION));
	if (fs_ == nullptr)
	{
		fs_ = static_cast<IFileSystem*>(
			context.QueryInterfaceAnywhere(FILESYSTEM_INTERFACE_VERSION));
	}

	if (fs_ == nullptr)
	{
		const Module& module = context.GetModule("filesystem_stdio");
		if (!module.IsValid())
		{
			context.UnavailableCall("IFileSystem", "the filesystem_stdio module is not loaded");
		}
		else
		{
			context.UnavailableCall("IFileSystem",
				"filesystem_stdio is loaded but does not answer '%s'",
				FILESYSTEM_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(fs_),
		FILESYSTEM_INTERFACE_VERSION);
	return true;
}

void FileSystemSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseFileQueries(context);
	ExerciseHandleIo(context);
	ExerciseBufferReads(context);
	ExercisePaths(context);
	ExerciseFinds(context);
	ExerciseNameDictionary(context);
	ExerciseMiscState(context);
	ExerciseScratchCalls(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = fs_->GetTier();
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		const bool singleton = fs_->IsSingleton();
		context.Detail("-> %s", YesNo(singleton));
		if (!singleton)
		{
			context.Fail("the file system is a singleton; false here suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = fs_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = fs_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}
		// Capped: walking a garbage pointer to a null is how a probe hangs.
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface", {
		void* self = fs_->QueryInterface(FILESYSTEM_INTERFACE_VERSION);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(fs_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run file system startup");
	context.SkipCall("IAppSystem::Init", "would re-run file system startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin file system shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the file system down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down file system interfaces");
}

// -----------------------------------------------------------------------------
//  Name-based queries
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseFileQueries(TestContext& context)
{
	context.Note("[IFileSystem] name-based queries on %s (path ID %s)",
		kProbeFile, kProbePathId);

	CS2ST_CALL(context, "IFileSystem::FileExists", {
		const bool present = fs_->FileExists(kProbeFile, kProbePathId);
		const bool missing = fs_->FileExists(kMissingFile, kProbePathId);
		context.Detail("-> probe %s, control %s", YesNo(present), YesNo(missing));
		if (missing)
		{
			context.Fail("a file that cannot exist was reported present");
		}
		if (!present)
		{
			context.Suspect("%s not found in %s; the file-based oracles below have nothing to compare",
				kProbeFile, kProbePathId);
		}
	});

	CS2ST_CALL(context, "IFileSystem::Size(name)", {
		const int64 size = fs_->Size(kProbeFile, kProbePathId);
		const int64 missing = fs_->Size(kMissingFile, kProbePathId);
		context.Detail("-> probe %lld, control %lld", static_cast<long long>(size),
			static_cast<long long>(missing));
		if (missing > 0)
		{
			context.Fail("Size returned a positive length for a file that cannot exist");
		}
	});

	CS2ST_CALL(context, "IFileSystem::GetFileTime", {
		const int64 stamp = fs_->GetFileTime(kProbeFile, kProbePathId);
		context.Detail("-> %lld", static_cast<long long>(stamp));
	});

	CS2ST_CALL(context, "IFileSystem::IsFileWritable", {
		context.Detail("-> %s", YesNo(fs_->IsFileWritable(kProbeFile, kProbePathId)));
	});

	CS2ST_CALL(context, "IFileSystem::IsDirectory", {
		const bool as_dir = fs_->IsDirectory(kProbeFile, kProbePathId);
		context.Detail("-> %s (probe file as directory)", YesNo(as_dir));
		if (as_dir)
		{
			context.Fail("a regular file was reported to be a directory");
		}
	});

	CS2ST_CALL(context, "IFileSystem::IsPathInvalidForFilesystem", {
		const bool plain = fs_->IsPathInvalidForFilesystem(kProbeFile);
		const bool traversal = fs_->IsPathInvalidForFilesystem("../../outside");
		context.Detail("-> plain %s, traversal %s", YesNo(plain), YesNo(traversal));
	});

	CS2ST_CALL(context, "IFileSystem::GetPathTime", {
		const int64 stamp = fs_->GetPathTime(kProbeFile, kProbePathId);
		context.Detail("-> %lld", static_cast<long long>(stamp));
	});

	CS2ST_CALL(context, "IFileSystem::FileTimeToString", {
		CBufferString text;
		fs_->FileTimeToString(fs_->GetFileTime(kProbeFile, kProbePathId), text);
		context.Detail("-> \"%s\"", text.Get());
	});

	// The SDK header declares (pPathID, pFileName); the engine reads the first
	// string as the file and the second as the path ID (libfilesystem_stdio.so,
	// checked in IDA), so the arguments go in that order.
	CS2ST_CALL(context, "IFileSystem::IsFileInReadOnlySearchPath", {
		context.Detail("-> %s",
			YesNo(fs_->IsFileInReadOnlySearchPath(kProbeFile, kProbePathId)));
	});

	// Only warms the engine's cache with the probe file.
	CS2ST_CALL(context, "IFileSystem::Precache", {
		const bool ok = fs_->Precache(kProbeFile, kProbePathId);
		context.Detail("-> %s", YesNo(ok));
	});

	// SetFileWritable is called in ExerciseScratchCalls, on a cstmp_ file.
}

// -----------------------------------------------------------------------------
//  Handle I/O: one read-only handle, walked through every handle method
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseHandleIo(TestContext& context)
{
	context.Note("[IFileSystem] handle I/O on a read-only handle");

	FileHandle_t handle = FILESYSTEM_INVALID_HANDLE;
	int64 name_size = -1;

	CS2ST_CALL(context, "IFileSystem::Open", {
		name_size = fs_->Size(kProbeFile, kProbePathId);
		handle = fs_->Open(kProbeFile, "rb", kProbePathId);
		context.Detail("-> %p", handle);
		if (handle == FILESYSTEM_INVALID_HANDLE && name_size > 0)
		{
			context.Fail("Open failed on a file Size says is %lld bytes",
				static_cast<long long>(name_size));
		}
	});

	// Second route to the same file: OpenEx adds only a flags word. Closed at
	// once; nothing is read through it.
	CS2ST_CALL(context, "IFileSystem::OpenEx", {
		FileHandle_t extended = fs_->OpenEx(kProbeFile, "rb", 0, kProbePathId);
		context.Detail("-> %p (Open gave %p)", extended, handle);
		if ((extended == FILESYSTEM_INVALID_HANDLE) != (handle == FILESYSTEM_INVALID_HANDLE))
		{
			context.Fail("Open and OpenEx disagree about whether the file opens");
		}
		if (extended != FILESYSTEM_INVALID_HANDLE)
		{
			fs_->Close(extended);
		}
	});

	if (handle == FILESYSTEM_INVALID_HANDLE)
	{
		context.Note("no handle; the remaining handle calls are not made");
		return;
	}

	CS2ST_CALL(context, "IFileSystem::IsOk", {
		context.Detail("-> %s", YesNo(fs_->IsOk(handle)));
	});

	CS2ST_CALL(context, "IFileSystem::Size(handle)", {
		const int64 size = fs_->Size(handle);
		context.Detail("-> %lld (by name %lld)", static_cast<long long>(size),
			static_cast<long long>(name_size));
		if (size != name_size)
		{
			context.Fail("Size(handle) and Size(name) disagree about one file");
		}
	});

	CS2ST_CALL(context, "IFileSystem::Read", {
		char head[16];
		std::memset(head, 0, sizeof(head));
		const int got = fs_->Read(head, static_cast<int>(sizeof(head)), handle);
		const int64 pos = fs_->Tell(handle);
		context.Detail("-> %d bytes, Tell %lld", got, static_cast<long long>(pos));
		if (got > 0 && pos != got)
		{
			context.Fail("Tell is %lld after reading %d bytes from the start",
				static_cast<long long>(pos), got);
		}
	});

	CS2ST_CALL(context, "IFileSystem::Tell", {
		context.Detail("-> %lld", static_cast<long long>(fs_->Tell(handle)));
	});

	CS2ST_CALL(context, "IFileSystem::Seek", {
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		const int64 at_head = fs_->Tell(handle);
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_TAIL);
		const int64 at_tail = fs_->Tell(handle);
		context.Detail("-> head %lld, tail %lld (size %lld)", static_cast<long long>(at_head),
			static_cast<long long>(at_tail), static_cast<long long>(name_size));
		if (at_head != 0)
		{
			context.Fail("Seek(HEAD, 0) left Tell at %lld", static_cast<long long>(at_head));
		}
		if (at_tail != name_size)
		{
			context.Fail("Seek(TAIL, 0) left Tell at %lld, not the file size",
				static_cast<long long>(at_tail));
		}
	});

	CS2ST_CALL(context, "IFileSystem::EndOfFile", {
		// The handle is at the tail from the Seek row above.
		const bool at_end = fs_->EndOfFile(handle);
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		const bool at_start = fs_->EndOfFile(handle);
		context.Detail("-> at tail %s, at head %s", YesNo(at_end), YesNo(at_start));
		if (at_start && name_size > 0)
		{
			context.Fail("EndOfFile is true at offset 0 of a non-empty file");
		}
	});

	CS2ST_CALL(context, "IFileSystem::ReadLine(char*)", {
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		char line[128];
		std::memset(line, 0, sizeof(line));
		char* result = fs_->ReadLine(line, static_cast<int>(sizeof(line)), handle);
		context.Detail("-> %p, \"%.60s\"", static_cast<void*>(result),
			result != nullptr ? result : "");
	});

	CS2ST_CALL(context, "IFileSystem::ReadLine(CUtlString)", {
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		CUtlString line = fs_->ReadLine(handle, true);
		context.Detail("-> \"%.60s\"", line.Get());
	});

	CS2ST_CALL(context, "IFileSystem::ReadEx", {
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		char head[16];
		std::memset(head, 0, sizeof(head));
		const int got = fs_->ReadEx(head, static_cast<int>(sizeof(head)),
			static_cast<int>(sizeof(head)), handle);
		context.Detail("-> %d bytes", got);
	});

	CS2ST_CALL(context, "IFileSystem::SetBufferSize", {
		fs_->SetBufferSize(handle, 4096);
		context.Detail("-> set 4096");
	});

	CS2ST_CALL(context, "IFileSystem::Flush", {
		fs_->Flush(handle);
		context.Detail("-> flushed a read-only handle");
	});

	CS2ST_CALL(context, "IFileSystem::GetOptimalIOConstraints", {
		unsigned offset_align = 0;
		unsigned size_align = 0;
		unsigned buffer_align = 0;
		const bool ok = fs_->GetOptimalIOConstraints(handle, &offset_align,
			&size_align, &buffer_align);
		context.Detail("-> %s, offset %u, size %u, buffer %u", YesNo(ok),
			offset_align, size_align, buffer_align);
	});

	// One row for the pair: the buffer is only ever valid to hand back to the
	// engine that allocated it.
	CS2ST_CALL(context, "IFileSystem::AllocOptimalReadBuffer + FreeOptimalReadBuffer", {
		void* buffer = fs_->AllocOptimalReadBuffer(handle, 4096, 0);
		context.Detail("-> %p", buffer);
		if (buffer != nullptr)
		{
			fs_->FreeOptimalReadBuffer(buffer);
		}
	});

	// The engine allocates the CUtlBuffer storage; ~CUtlBuffer would free it here
	// with this module's CRT free, which crashed the process (heap critical
	// failure, not an exception SEH can turn into a CRASHED row). The buffer is
	// built in leaked storage and never destroyed.
	CS2ST_CALL(context, "IFileSystem::ReadToBuffer", {
		// Intentional leak: never destroyed, freed when the process exits.
		static LeakedObject<CUtlBuffer> storage;
		CUtlBuffer* buf = storage.Construct();
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		const bool ok = fs_->ReadToBuffer(handle, *buf);
		const int put = buf->TellPut();
		context.Detail("-> %s, %d bytes in the buffer (Size %lld)", YesNo(ok), put,
			static_cast<long long>(name_size));
		if (ok && name_size >= 0 && put != name_size)
		{
			context.Suspect("ReadToBuffer put %d bytes, Size says %lld", put,
				static_cast<long long>(name_size));
		}
		if (ok && put > 0 && buf->Base() == nullptr)
		{
			context.Fail("%d bytes reported but the buffer has no storage", put);
		}
	});

	// Read-only mapping through the open handle; the name overload is exercised
	// in ExercisePaths. Both should describe the same file.
	CS2ST_CALL(context, "IFileSystem::CreateMemoryMap(FileHandle_t,flags)", {
		IMemoryMappedFile* map = fs_->CreateMemoryMap(handle, MMF_READ);
		if (map == nullptr)
		{
			context.Detail("-> null (SDK allows NULL on failure)");
			return;
		}
		const size_t mapped = map->GetMappedSize();
		context.Detail("-> %p, memory %p, %llu bytes, flags %d", static_cast<void*>(map),
			map->GetMappedMemory(), static_cast<unsigned long long>(mapped),
			static_cast<int>(map->GetFlags()));
		if (name_size > 0 && mapped != static_cast<size_t>(name_size))
		{
			context.Suspect("mapping is %llu bytes, Size says %lld",
				static_cast<unsigned long long>(mapped), static_cast<long long>(name_size));
		}
		map->Release();
	});

	CS2ST_CALL(context, "IFileSystem::Close", {
		fs_->Close(handle);
		context.Detail("-> closed %p", handle);
	});

	// Write and FPrintf are called in ExerciseScratchCalls, on a handle to a
	// cstmp_ file; the probe handle stays read-only.
}

// -----------------------------------------------------------------------------
//  Whole-file reads into caller buffers
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseBufferReads(TestContext& context)
{
	context.Note("[IFileSystem] whole-file reads; the file length has several routes");

	// Fills a CUtlBuffer with engine-allocated storage; see ReadToBuffer for why
	// the buffer is built in leaked storage and never destroyed.
	CS2ST_CALL(context, "IFileSystem::ReadFile", {
		// Intentional leak: never destroyed, freed when the process exits.
		static LeakedObject<CUtlBuffer> storage;
		CUtlBuffer* buf = storage.Construct();
		const int64 size = fs_->Size(kProbeFile, kProbePathId);
		const bool ok = fs_->ReadFile(kProbeFile, kProbePathId, *buf);
		const int put = buf->TellPut();
		context.Detail("-> %s, %d bytes in the buffer (Size %lld)", YesNo(ok), put,
			static_cast<long long>(size));
		if (ok && size >= 0 && put != size)
		{
			context.Suspect("ReadFile put %d bytes, Size says %lld", put,
				static_cast<long long>(size));
		}
	});

	CS2ST_CALL(context, "IFileSystem::ReadFileEx", {
		const int64 size = fs_->Size(kProbeFile, kProbePathId);
		void* data = nullptr;
		const int got = fs_->ReadFileEx(kProbeFile, kProbePathId, &data,
			false, false, 0, 0, &ProbeAlloc);
		context.Detail("-> %d bytes at %p (Size %lld)", got, data,
			static_cast<long long>(size));
		if (got > 0 && size >= 0 && got != size)
		{
			context.Fail("ReadFileEx delivered %d bytes of a %lld-byte file", got,
				static_cast<long long>(size));
		}
		// Allocated by ProbeAlloc, so it goes back to the same heap.
		std::free(data);
	});

	// WriteFile and CopyAFile are called in ExerciseScratchCalls, on cstmp_ files.
	context.SkipCall("IFileSystem::UnzipFile", "needs a zip archive to extract and this suite has none; extraction writes to disk");
}

// -----------------------------------------------------------------------------
//  Path resolution and search paths
// -----------------------------------------------------------------------------

void FileSystemSuite::ExercisePaths(TestContext& context)
{
	context.Note("[IFileSystem] path resolution; a relative name should round-trip");

	CS2ST_CALL(context, "IFileSystem::RelativePathToFullPath", {
		CBufferString full;
		const char* result = fs_->RelativePathToFullPath(kProbeFile, kProbePathId, full,
			FILTER_NONE, nullptr);
		context.Detail("-> %p, \"%s\"", static_cast<const void*>(result), full.Get());
	});

	CS2ST_CALL(context, "IFileSystem::FullPathToRelativePath", {
		CBufferString full;
		fs_->RelativePathToFullPath(kProbeFile, kProbePathId, full, FILTER_NONE, nullptr);
		if (full.Length() == 0)
		{
			context.Detail("-> no full path to round-trip");
			return;
		}
		CBufferString relative;
		const bool ok = fs_->FullPathToRelativePath(full.Get(), kProbePathId, relative);
		context.Detail("-> %s, \"%s\" (from \"%s\")", YesNo(ok), relative.Get(), full.Get());
		if (ok && !SamePath(relative.Get(), kProbeFile))
		{
			// Not Fail: the engine may hand back a path relative to a
			// different search root than the one the name was found in.
			context.Suspect("round trip gave \"%s\", not \"%s\"", relative.Get(), kProbeFile);
		}
	});

	CS2ST_CALL(context, "IFileSystem::RelativePathToFullWritePath", {
		CBufferString path;
		const bool ok = fs_->RelativePathToFullWritePath(kProbeFile, kProbePathId, path);
		context.Detail("-> %s, \"%s\"", YesNo(ok), path.Get());
	});

	CS2ST_CALL(context, "IFileSystem::GetSearchPath", {
		CBufferString paths;
		const bool ok = fs_->GetSearchPath(kProbePathId, GET_SEARCH_PATH_ALL, paths, 4);
		context.Detail("-> %s, \"%.200s\"", YesNo(ok), paths.Get());
	});

	// The engine allocates the vector's array; the vector is built in leaked
	// storage and never destroyed (a cross-heap free aborts the process).
	CS2ST_CALL(context, "IFileSystem::GetSearchPathsForPathID", {
		// Intentional leak: never destroyed, freed when the process exits.
		static LeakedObject<CUtlVector<CUtlString>> storage;
		CUtlVector<CUtlString>* paths = storage.Construct();
		fs_->GetSearchPathsForPathID(kProbePathId, GET_SEARCH_PATH_ALL, *paths);
		const int count = paths->Count();
		std::string text;
		AppendF(text, "-> %d path(s) for %s", count, kProbePathId);
		for (int i = 0; i < count && i < 3; ++i)
		{
			AppendF(text, "; [%d] \"%.120s\"", i, (*paths)[i].Get());
		}
		context.Detail("%s", text.c_str());
		if (count <= 0)
		{
			context.Suspect("no search paths for %s, which resolves %s", kProbePathId, kProbeFile);
		}
	});

	CS2ST_CALL(context, "IFileSystem::GetSearchPathID", {
		CBufferString id;
		id.CopyFrom(kProbePathId);
		fs_->GetSearchPathID(id);
		context.Detail("-> \"%s\" (in-out, seeded \"%s\")", id.Get(), kProbePathId);
	});

	CS2ST_CALL(context, "IFileSystem::GetCurrentDirectory", {
		CBufferString dir;
		const bool ok = fs_->GetCurrentDirectory(dir);
		context.Detail("-> %s, \"%s\"", YesNo(ok), dir.Get());
		if (ok && dir.Length() == 0)
		{
			context.Suspect("success with an empty current directory");
		}
	});

	CS2ST_CALL(context, "IFileSystem::GetFileTypeForFullPath", {
		CBufferString full;
		fs_->RelativePathToFullPath(kProbeFile, kProbePathId, full, FILTER_NONE, nullptr);
		CBufferString type;
		const bool ok = fs_->GetFileTypeForFullPath(full.Get(), type);
		context.Detail("-> %s, \"%s\"", YesNo(ok), type.Get());
	});

	CS2ST_CALL(context, "IFileSystem::GetPathIndex", {
		const FileNameHandle_t name = fs_->FindFileName(kProbeFile);
		context.Detail("-> %d (for name handle %u)", fs_->GetPathIndex(name),
			static_cast<unsigned>(name));
	});

	CS2ST_CALL(context, "IFileSystem::PrintSearchPaths", {
		fs_->PrintSearchPaths();
		context.Detail("-> printed to the engine console");
	});

	// AddSearchPath, RemoveSearchPath, RemoveSearchPaths, MarkPathIDByRequestOnly,
	// SetSearchPathReadOnly and the Save/Restore/Destroy state triple are called
	// in ExerciseScratchCalls, against the cstmp_pathid path ID.
	context.SkipCall("IFileSystem::RemoveAllSearchPaths", "would empty every search path, and a failed restore would leave the game without any");

	CS2ST_CALL(context, "IFileSystem::CreateMemoryMap(name,pathID,flags)", {
		IMemoryMappedFile* map = fs_->CreateMemoryMap(kProbeFile, kProbePathId, MMF_READ);
		if (map == nullptr)
		{
			context.Detail("-> null (SDK allows NULL on failure)");
			return;
		}
		const size_t mapped = map->GetMappedSize();
		const int64 size = fs_->Size(kProbeFile, kProbePathId);
		context.Detail("-> %p, memory %p, %llu bytes (Size %lld)", static_cast<void*>(map),
			map->GetMappedMemory(), static_cast<unsigned long long>(mapped),
			static_cast<long long>(size));
		if (size > 0 && mapped != static_cast<size_t>(size))
		{
			context.Suspect("mapping is %llu bytes, Size says %lld",
				static_cast<unsigned long long>(mapped), static_cast<long long>(size));
		}
		map->Release();
	});

	// GetMergeTokenForPath feeds GetMergedPaths; the second only means something
	// when the first found a merge group, so they share one row.
	CS2ST_CALL(context, "IFileSystem::GetMergeTokenForPath + GetMergedPaths", {
		const CUtlStringToken token = fs_->GetMergeTokenForPath(kProbeFile, kProbePathId);
		context.Detail("-> token 0x%08x (%s)", static_cast<unsigned>(token.GetHashCode()),
			token.IsValid() ? "valid" : "empty");
		CBufferString merged;
		const int length = fs_->GetMergedPaths(token, &merged);
		context.Detail("-> %d, \"%.200s\"", length, merged.Get());
	});

	CS2ST_CALL(context, "IFileSystem::IsFileInUntrustedAddon", {
		context.Detail("-> %s (for \"%s\")", YesNo(fs_->IsFileInUntrustedAddon(kProbeFile)),
			kProbeFile);
	});
}

// -----------------------------------------------------------------------------
//  Directory finds
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseFinds(TestContext& context)
{
	context.Note("[IFileSystem] directory finds");

	// One row for the four-method find protocol: FindNext, FindIsDirectory and
	// FindClose are only meaningful on a handle FindFirst produced.
	CS2ST_CALL(context, "IFileSystem::FindFirst + FindNext + FindIsDirectory + FindClose", {
		FileFindHandle_t find = InvalidFindHandle();
		const char* first = fs_->FindFirst(kProbeFile, &find);
		context.Detail("-> first \"%s\", handle index %lld", first != nullptr ? first : "(null)",
			static_cast<long long>(find.m_nFindDataIndex));
		if (first != nullptr)
		{
			const bool dir = fs_->FindIsDirectory(find);
			const char* next = fs_->FindNext(find);
			context.Detail("   IsDirectory %s, next \"%s\"", YesNo(dir),
				next != nullptr ? next : "(null)");
			if (dir)
			{
				context.Fail("FindFirst on a regular file's name yielded a directory");
			}
		}
		fs_->FindClose(find);
	});

	CS2ST_CALL(context, "IFileSystem::FindFirstEx", {
		FileFindHandle_t find = InvalidFindHandle();
		const char* first = fs_->FindFirstEx(kProbeFile, kProbePathId, &find);
		context.Detail("-> \"%s\"", first != nullptr ? first : "(null)");
		fs_->FindClose(find);
	});

	// Both fill a CUtlVector<CUtlString> with engine-allocated storage; the
	// vectors are built in leaked storage and never destroyed.
	CS2ST_CALL(context, "IFileSystem::FindFileAbsoluteList", {
		// Intentional leak: never destroyed, freed when the process exits.
		static LeakedObject<CUtlVector<CUtlString>> storage;
		CUtlVector<CUtlString>* found = storage.Construct();
		fs_->FindFileAbsoluteList(*found, kProbeFile, kProbePathId);
		const int count = found->Count();
		std::string text;
		AppendF(text, "-> %d hit(s) for %s", count, kProbeFile);
		for (int i = 0; i < count && i < 3; ++i)
		{
			AppendF(text, "; [%d] \"%.120s\"", i, (*found)[i].Get());
		}
		context.Detail("%s", text.c_str());
		if (count <= 0 && fs_->FileExists(kProbeFile, kProbePathId))
		{
			context.Suspect("FileExists finds %s in %s, FindFileAbsoluteList lists none",
				kProbeFile, kProbePathId);
		}
	});

	CS2ST_CALL(context, "IFileSystem::GetAvailableDrives", {
		// Intentional leak: never destroyed, freed when the process exits.
		static LeakedObject<CUtlVector<CUtlString>> storage;
		CUtlVector<CUtlString>* drives = storage.Construct();
		fs_->GetAvailableDrives(*drives);
		const int count = drives->Count();
		std::string text;
		AppendF(text, "-> %d drive(s)", count);
		for (int i = 0; i < count && i < 3; ++i)
		{
			AppendF(text, "; [%d] \"%.60s\"", i, (*drives)[i].Get());
		}
		context.Detail("%s", text.c_str());
	});
}

// -----------------------------------------------------------------------------
//  The file-name dictionary
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseNameDictionary(TestContext& context)
{
	context.Note("[IFileSystem] file-name dictionary; name -> handle -> name should round-trip");

	CS2ST_CALL(context, "IFileSystem::FindOrAddFileName", {
		// Adds only a name that is already a real file's; a duplicate add is
		// the dictionary's own no-op.
		const FileNameHandle_t added = fs_->FindOrAddFileName(kProbeFile);
		const FileNameHandle_t found = fs_->FindFileName(kProbeFile);
		context.Detail("-> %u (FindFileName %u)", static_cast<unsigned>(added),
			static_cast<unsigned>(found));
		if (added != found)
		{
			context.Fail("FindOrAddFileName and FindFileName disagree about one name");
		}
	});

	CS2ST_CALL(context, "IFileSystem::FindFileName", {
		const FileNameHandle_t hit = fs_->FindFileName(kProbeFile);
		const FileNameHandle_t miss = fs_->FindFileName(kMissingFile);
		context.Detail("-> probe %u, control %u", static_cast<unsigned>(hit),
			static_cast<unsigned>(miss));
		if (hit != 0 && hit == miss)
		{
			context.Fail("a name that was never added resolved to the probe's handle");
		}
	});

	CS2ST_CALL(context, "IFileSystem::String", {
		const FileNameHandle_t handle = fs_->FindFileName(kProbeFile);
		CBufferString name;
		const bool ok = fs_->String(handle, name);
		context.Detail("-> %s, \"%s\"", YesNo(ok), name.Get());
		if (ok && !SamePath(name.Get(), kProbeFile))
		{
			context.Suspect("handle %u maps back to \"%s\", not \"%s\"",
				static_cast<unsigned>(handle), name.Get(), kProbeFile);
		}
	});
}

// -----------------------------------------------------------------------------
//  Statistics, UGC / VPK state and the rest of the read-only getters
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseMiscState(TestContext& context)
{
	context.Note("[IFileSystem] statistics, UGC/VPK state and other read-only getters");

	CS2ST_CALL(context, "IFileSystem::GetFilesystemStatistics", {
		const FileSystemStatistics* stats = fs_->GetFilesystemStatistics();
		if (stats == nullptr)
		{
			// The SDK allows this: "Returns NULL if not supported".
			context.Detail("-> null (not supported)");
			return;
		}
		context.Detail("-> %p, reads %u, writes %u, bytesRead %u, bytesWritten %u, seeks %u",
			static_cast<const void*>(stats), static_cast<unsigned>(stats->nReads),
			static_cast<unsigned>(stats->nWrites), static_cast<unsigned>(stats->nBytesRead),
			static_cast<unsigned>(stats->nBytesWritten), static_cast<unsigned>(stats->nSeeks));
	});

	CS2ST_CALL(context, "IFileSystem::GetAutoVPKFileLoading", {
		context.Detail("-> %s", YesNo(fs_->GetAutoVPKFileLoading()));
	});

	CS2ST_CALL(context, "IFileSystem::IsUGCVPKFileLoaded", {
		const bool loaded = fs_->IsUGCVPKFileLoaded("ugc:0");
		context.Detail("-> %s (for the never-mounted \"ugc:0\")", YesNo(loaded));
		if (loaded)
		{
			context.Suspect("a UGC VPK this suite never mounted reports as loaded");
		}
	});

	CS2ST_CALL(context, "IFileSystem::IsUGCLoaded", {
		const bool loaded = fs_->IsUGCLoaded(0);
		context.Detail("-> %s (for UGC handle 0)", YesNo(loaded));
	});

	CS2ST_CALL(context, "IFileSystem::ParseUGCHandleFromFilename", {
		char* remaining = nullptr;
		const UGCHandle_t ugc = fs_->ParseUGCHandleFromFilename(kProbeFile, &remaining);
		context.Detail("-> %llu, remaining \"%s\"", static_cast<unsigned long long>(ugc),
			remaining != nullptr ? remaining : "(null)");
	});

	CS2ST_CALL(context, "IFileSystem::CreateFilenameForUGCFile", {
		CBufferString out;
		fs_->CreateFilenameForUGCFile(out, 0, "vpk", CORRECT_PATH_SEPARATOR);
		context.Detail("-> \"%s\"", out.Get());
	});

	CS2ST_CALL(context, "IFileSystem::Trace_DumpUnclosedFiles", {
		fs_->Trace_DumpUnclosedFiles();
		context.Detail("-> dumped to the engine console");
	});

	// A nesting counter: the true call is undone by the matching false one.
	CS2ST_CALL(context, "IFileSystem::SuppressAsyncFileRequestFlushOnPathChange", {
		fs_->SuppressAsyncFileRequestFlushOnPathChange(true);
		fs_->SuppressAsyncFileRequestFlushOnPathChange(false);
		context.Detail("-> true then false, counter balanced");
	});

	// Switches the policy off and puts the original value back, so nothing is
	// auto-mounted by the call and the state afterwards is the state before.
	CS2ST_CALL(context, "IFileSystem::EnableAutoVPKFileLoading", {
		const bool original = fs_->GetAutoVPKFileLoading();
		fs_->EnableAutoVPKFileLoading(false);
		const bool disabled = fs_->GetAutoVPKFileLoading();
		fs_->EnableAutoVPKFileLoading(original);
		const bool restored = fs_->GetAutoVPKFileLoading();
		context.Detail("-> original %s, after false %s, restored %s", YesNo(original),
			YesNo(disabled), YesNo(restored));
		if (restored != original)
		{
			context.Fail("the original policy %s was not restored, it reads %s",
				YesNo(original), YesNo(restored));
		}
		else if (disabled)
		{
			context.Suspect("GetAutoVPKFileLoading still true after EnableAutoVPKFileLoading(false)");
		}
	});

	// UGC handle 0 names nothing: the open is expected to fail, with no effect.
	CS2ST_CALL(context, "IFileSystem::OpenUGCFile", {
		FileHandle_t ugc = fs_->OpenUGCFile(0);
		context.Detail("-> %p (for UGC handle 0)", ugc);
		if (ugc != FILESYSTEM_INVALID_HANDLE)
		{
			fs_->Close(ugc);
			context.Suspect("UGC handle 0 opened a file");
		}
	});

	// The header documents 0.0 as "turn off the functionality", which is also
	// the state it is in unless something armed it.
	CS2ST_CALL(context, "IFileSystem::SetIODelayAlarm", {
		fs_->SetIODelayAlarm(0.0f);
		context.Detail("-> disarmed (0.0)");
	});

	// Hashes every mounted VPK; takes seconds, but changes nothing.
	CS2ST_CALL(context, "IFileSystem::ValidateLoadedVPKs", {
		const bool ok = fs_->ValidateLoadedVPKs();
		context.Detail("-> %s", YesNo(ok));
	});
}

// -----------------------------------------------------------------------------
//  Scratch objects: calls that write, copy, rename, delete or mount
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseScratchCalls(TestContext& context)
{
	context.Note("[scratch] calls that write, rename, delete or mount, made against "
	             "cstmp_* files in %s and the %s path ID; every one is removed again",
		kProbePathId, kScratchPathId);

	// Which scratch files exist right now; later rows only act on what earlier
	// rows managed to create.
	bool a_ready = false;
	bool b_ready = false;
	// Full path of the scratch directory, for the rows that need a path the
	// engine does not resolve through a path ID.
	std::string scratch_dir_full;

	CS2ST_CALL(context, "IFileSystem::CreateDirHierarchy", {
		const bool ok = fs_->CreateDirHierarchy(kScratchDir, kProbePathId);
		const bool made = fs_->IsDirectory(kScratchDir, kProbePathId);
		context.Detail("-> %s, IsDirectory(%s) %s", YesNo(ok), kScratchDir, YesNo(made));
		if (!made)
		{
			context.Fail("%s is not a directory after CreateDirHierarchy", kScratchDir);
		}
		else if (!ok)
		{
			context.Suspect("CreateDirHierarchy returned false although %s exists", kScratchDir);
		}
	});

	CS2ST_CALL(context, "IFileSystem::CreateDirHierarchyForFile", {
		const bool ok = fs_->CreateDirHierarchyForFile(kScratchTreeFile, kProbePathId);
		const bool made = fs_->IsDirectory(kScratchTreeInner, kProbePathId);
		context.Detail("-> %s, IsDirectory(%s) %s", YesNo(ok), kScratchTreeInner, YesNo(made));
		if (!made)
		{
			context.Fail("%s is not a directory after CreateDirHierarchyForFile of %s",
				kScratchTreeInner, kScratchTreeFile);
		}
		else if (!ok)
		{
			context.Suspect("CreateDirHierarchyForFile returned false although %s exists",
				kScratchTreeInner);
		}
	});

	// A file written through Write, then a second handle appends through
	// FPrintf; reading the file back gives the bytes both calls were given.
	CS2ST_CALL(context, "IFileSystem::Write", {
		static const char text[] = "hello\n";
		const int expected = static_cast<int>(sizeof(text) - 1);
		FileHandle_t out = fs_->Open(kScratchFileA, "wb", kProbePathId);
		if (out == FILESYSTEM_INVALID_HANDLE)
		{
			context.Detail("-> could not open %s for writing", kScratchFileA);
			context.Suspect("%s has no writable path; the write rows have nothing to act on",
				kProbePathId);
			return;
		}
		const int wrote = fs_->Write(text, static_cast<uint64>(expected), out);
		fs_->Close(out);
		const int64 size = fs_->Size(kScratchFileA, kProbePathId);
		a_ready = fs_->FileExists(kScratchFileA, kProbePathId);
		context.Detail("-> wrote %d of %d bytes, Size %lld", wrote, expected,
			static_cast<long long>(size));
		if (wrote != expected || size != expected)
		{
			context.Fail("wrote %d of %d bytes and the file is %lld bytes", wrote, expected,
				static_cast<long long>(size));
		}
	});

	if (a_ready)
	{
		// The full path of the scratch directory comes from the file just made.
		CBufferString full;
		fs_->RelativePathToFullPath(kScratchFileA, kProbePathId, full, FILTER_NONE, nullptr);
		scratch_dir_full = DirectoryOf(full.Get());
	}

	if (a_ready)
	{
		CS2ST_CALL(context, "IFileSystem::FPrintf", {
			static const char expected_text[] = "hello\n42\n";
			const int expected_size = static_cast<int>(sizeof(expected_text) - 1);
			FileHandle_t out = fs_->Open(kScratchFileA, "ab", kProbePathId);
			if (out == FILESYSTEM_INVALID_HANDLE)
			{
				context.Detail("-> could not reopen %s for appending", kScratchFileA);
				context.Suspect("append open failed on a file Write just created");
				return;
			}
			const int printed = fs_->FPrintf(out, "%d\n", 42);
			fs_->Close(out);
			const int64 size = fs_->Size(kScratchFileA, kProbePathId);
			char back[32];
			std::memset(back, 0, sizeof(back));
			const int got = ReadBack(fs_, kScratchFileA, back, static_cast<int>(sizeof(back)));
			context.Detail("-> printed %d, Size %lld, read back %d bytes", printed,
				static_cast<long long>(size), got);
			if (size != expected_size || got != expected_size
				|| std::memcmp(back, expected_text, static_cast<std::size_t>(expected_size)) != 0)
			{
				context.Fail("after Write(\"hello\\n\") + FPrintf(\"42\\n\") the file is %lld bytes, "
				             "read back %d bytes, not the %d written",
					static_cast<long long>(size), got, expected_size);
			}
			else if (printed != 3)
			{
				context.Suspect("FPrintf returned %d for 3 characters", printed);
			}
		});
	}
	else
	{
		context.SkipCall("IFileSystem::FPrintf", "no scratch file could be written");
	}

	// WriteFile takes a CUtlBuffer this suite fills (storage is the engine's
	// allocator via the imported Put), so the buffer is never destroyed.
	CS2ST_CALL(context, "IFileSystem::WriteFile", {
		// Intentional leak: never destroyed, freed when the process exits.
		static LeakedObject<CUtlBuffer> storage;
		static const char payload[] = "cs2sdktest-writefile";
		const int length = static_cast<int>(sizeof(payload) - 1);
		CUtlBuffer* buf = storage.Construct();
		buf->Put(payload, length);
		const bool ok = fs_->WriteFile(kScratchFileW, kProbePathId, *buf);
		const int64 size = fs_->Size(kScratchFileW, kProbePathId);
		char back[64];
		std::memset(back, 0, sizeof(back));
		const int got = ReadBack(fs_, kScratchFileW, back, static_cast<int>(sizeof(back)));
		context.Detail("-> %s, Size %lld, read back %d bytes", YesNo(ok),
			static_cast<long long>(size), got);
		if (ok && (size != length || got != length
			|| std::memcmp(back, payload, static_cast<std::size_t>(length)) != 0))
		{
			context.Fail("WriteFile said ok for %d bytes; the file is %lld bytes, read back %d",
				length, static_cast<long long>(size), got);
		}
		else if (!ok)
		{
			context.Suspect("WriteFile returned false for a writable path");
		}
	});

	// The destination goes in as a full path: a relative one might be taken
	// relative to the process, outside the scratch directory.
	if (a_ready && !scratch_dir_full.empty())
	{
		CS2ST_CALL(context, "IFileSystem::CopyAFile", {
			const std::string destination = scratch_dir_full + "/" + "c.txt";
			const bool ok = fs_->CopyAFile(kScratchFileA, kProbePathId, destination.c_str());
			const bool present = fs_->FileExists(kScratchFileC, kProbePathId);
			const int64 source_size = fs_->Size(kScratchFileA, kProbePathId);
			const int64 copy_size = fs_->Size(kScratchFileC, kProbePathId);
			std::string text;
			AppendF(text, "-> %s, copy %s, Size %lld (source %lld)", YesNo(ok),
				present ? "present" : "absent", static_cast<long long>(copy_size),
				static_cast<long long>(source_size));
			if (present)
			{
				const bool again = fs_->CopyAFile(kScratchFileA, kProbePathId,
					destination.c_str(), true);
				AppendF(text, "; second copy with bDontOverwrite -> %s", YesNo(again));
			}
			context.Detail("%s", text.c_str());
			if (present && copy_size != source_size)
			{
				context.Fail("the copy is %lld bytes, the source %lld",
					static_cast<long long>(copy_size), static_cast<long long>(source_size));
			}
			else if (ok && !present)
			{
				context.Suspect("CopyAFile said ok but %s is not found through %s",
					kScratchFileC, kProbePathId);
			}
		});
	}
	else
	{
		context.SkipCall("IFileSystem::CopyAFile", "no scratch file or no full path to name a destination");
	}

	if (a_ready)
	{
		CS2ST_CALL(context, "IFileSystem::RenameFile", {
			// A leftover from an interrupted run would make the rename fail.
			if (fs_->FileExists(kScratchFileB, kProbePathId))
			{
				fs_->RemoveFile(kScratchFileB, kProbePathId);
			}
			const bool ok = fs_->RenameFile(kScratchFileA, kScratchFileB, kProbePathId);
			const bool a_after = fs_->FileExists(kScratchFileA, kProbePathId);
			const bool b_after = fs_->FileExists(kScratchFileB, kProbePathId);
			a_ready = a_after;
			b_ready = b_after;
			context.Detail("-> %s, old name %s, new name %s", YesNo(ok),
				a_after ? "present" : "gone", b_after ? "present" : "absent");
			if (ok && (a_after || !b_after))
			{
				context.Fail("RenameFile said ok but the old name is %s and the new name is %s",
					a_after ? "still there" : "gone", b_after ? "there" : "missing");
			}
		});
	}
	else
	{
		context.SkipCall("IFileSystem::RenameFile", "no scratch file could be written");
	}

	if (b_ready)
	{
		// Always ends with writable restored, so the clean-up below can delete it.
		CS2ST_CALL(context, "IFileSystem::SetFileWritable", {
			const bool before = fs_->IsFileWritable(kScratchFileB, kProbePathId);
			const bool locked = fs_->SetFileWritable(kScratchFileB, false, kProbePathId);
			const bool while_locked = fs_->IsFileWritable(kScratchFileB, kProbePathId);
			const bool unlocked = fs_->SetFileWritable(kScratchFileB, true, kProbePathId);
			const bool after = fs_->IsFileWritable(kScratchFileB, kProbePathId);
			context.Detail("-> writable %s; SetFileWritable(false) %s -> %s; SetFileWritable(true) %s -> %s",
				YesNo(before), YesNo(locked), YesNo(while_locked), YesNo(unlocked), YesNo(after));
			if (!after)
			{
				context.Fail("the file is not writable after SetFileWritable(true)");
			}
			else if (locked && while_locked)
			{
				context.Suspect("still writable after SetFileWritable(false) returned true");
			}
		});
	}
	else
	{
		context.SkipCall("IFileSystem::SetFileWritable", "no scratch file to change");
	}

	// ---- search paths: the scratch directory mounted under cstmp_pathid ----

	if (b_ready && !scratch_dir_full.empty())
	{
		// Marked before it has any path, as the header asks.
		CS2ST_CALL(context, "IFileSystem::MarkPathIDByRequestOnly", {
			fs_->MarkPathIDByRequestOnly(kScratchPathId, true);
			context.Detail("-> %s marked request-only", kScratchPathId);
		});

		CS2ST_CALL(context, "IFileSystem::AddSearchPath", {
			const bool before = fs_->FileExists("b.txt", kScratchPathId);
			fs_->AddSearchPath(scratch_dir_full.c_str(), kScratchPathId);
			const bool after = fs_->FileExists("b.txt", kScratchPathId);
			context.Detail("-> b.txt through %s: before %s, after %s", kScratchPathId,
				YesNo(before), YesNo(after));
			if (before)
			{
				context.Fail("b.txt resolved through %s before anything was mounted under it",
					kScratchPathId);
			}
			else if (!after)
			{
				context.Suspect("b.txt is not found through %s after AddSearchPath of \"%.120s\"",
					kScratchPathId, scratch_dir_full.c_str());
			}
		});

		// The SDK header declares (pPathID, <unnamed>, bReadOnly) and
		// (pPathID, pFileName) for the getter. The engine takes the path first and
		// the path ID second in both (libfilesystem_stdio.so, checked in IDA), so
		// the arguments go in that order; with the header's order the setter looks
		// the directory up as a path ID, matches nothing and changes nothing.
		CS2ST_CALL(context, "IFileSystem::SetSearchPathReadOnly", {
			fs_->SetSearchPathReadOnly(scratch_dir_full.c_str(), kScratchPathId, true);
			const bool read_only = fs_->IsFileInReadOnlySearchPath("b.txt", kScratchPathId);
			fs_->SetSearchPathReadOnly(scratch_dir_full.c_str(), kScratchPathId, false);
			const bool writable = !fs_->IsFileInReadOnlySearchPath("b.txt", kScratchPathId);
			context.Detail("-> read-only after true: %s; writable after false: %s",
				YesNo(read_only), YesNo(writable));
			if (!read_only || !writable)
			{
				context.Suspect("IsFileInReadOnlySearchPath did not follow SetSearchPathReadOnly "
				                "(true -> %s, false -> %s)", YesNo(read_only),
					writable ? "writable" : "read-only");
			}
		});

		CS2ST_CALL(context, "IFileSystem::RemoveSearchPath", {
			const bool ok = fs_->RemoveSearchPath(scratch_dir_full.c_str(), kScratchPathId);
			const bool after = fs_->FileExists("b.txt", kScratchPathId);
			context.Detail("-> %s, b.txt through %s afterwards: %s", YesNo(ok), kScratchPathId,
				after ? "found" : "not found");
			if (ok && after)
			{
				context.Fail("RemoveSearchPath said ok but b.txt still resolves through %s",
					kScratchPathId);
			}
		});

		// Mounts the directory again so RemoveSearchPaths has something to
		// remove, and leaves the path ID empty and no longer request-only.
		CS2ST_CALL(context, "IFileSystem::RemoveSearchPaths", {
			fs_->AddSearchPath(scratch_dir_full.c_str(), kScratchPathId);
			const bool mounted = fs_->FileExists("b.txt", kScratchPathId);
			fs_->RemoveSearchPaths(kScratchPathId);
			const bool after = fs_->FileExists("b.txt", kScratchPathId);
			fs_->MarkPathIDByRequestOnly(kScratchPathId, false);
			context.Detail("-> b.txt through %s: mounted %s, after RemoveSearchPaths %s",
				kScratchPathId, mounted ? "found" : "not found", after ? "found" : "not found");
			if (after)
			{
				context.Fail("b.txt still resolves through %s after RemoveSearchPaths", kScratchPathId);
			}
		});
	}
	else
	{
		const char* reason = "no scratch directory to mount";
		context.SkipCall("IFileSystem::MarkPathIDByRequestOnly", "%s", reason);
		context.SkipCall("IFileSystem::AddSearchPath", "%s", reason);
		context.SkipCall("IFileSystem::SetSearchPathReadOnly", "%s", reason);
		context.SkipCall("IFileSystem::RemoveSearchPath", "%s", reason);
		context.SkipCall("IFileSystem::RemoveSearchPaths", "%s", reason);
	}

	// Saves the live search paths, restores the same state, destroys it. Nothing
	// changes in between, so GAME resolves to the same paths before and after.
	CS2ST_CALL(context, "IFileSystem::SaveSearchPathState + RestoreSearchPathState + DestroySearchPathState", {
		CBufferString before;
		fs_->GetSearchPath(kProbePathId, GET_SEARCH_PATH_ALL, before, 64);
		SearchPathStateHandle_t* state = fs_->SaveSearchPathState("cstmp_state");
		if (state == nullptr)
		{
			context.Detail("-> Save returned null; Restore and Destroy were not called");
			context.Suspect("SaveSearchPathState returned null");
			return;
		}
		fs_->RestoreSearchPathState(state);
		fs_->DestroySearchPathState(state);
		CBufferString after;
		fs_->GetSearchPath(kProbePathId, GET_SEARCH_PATH_ALL, after, 64);
		const bool same = std::strcmp(before.Get(), after.Get()) == 0;
		context.Detail("-> state %p; %s search paths %s", static_cast<void*>(state), kProbePathId,
			same ? "unchanged" : "differ before and after");
		if (!same)
		{
			context.Suspect("%s search paths differ after Save + Restore of the same state", kProbePathId);
		}
	});

	// ---- clean-up: also the rows for the delete calls ----

	// Outside the macro: a braced list's commas would split the macro arguments.
	const char* const scratch_files[] = { kScratchFileA, kScratchFileB, kScratchFileC, kScratchFileW };

	CS2ST_CALL(context, "IFileSystem::RemoveFile", {
		std::string left;
		for (const char* file : scratch_files)
		{
			if (!fs_->FileExists(file, kProbePathId))
			{
				continue;
			}
			const bool removed = fs_->RemoveFile(file, kProbePathId);
			if (fs_->FileExists(file, kProbePathId))
			{
				AppendF(left, " %s", file);
			}
			else if (!removed)
			{
				context.Suspect("RemoveFile returned false although %s is gone", file);
			}
		}
		context.Detail("-> %s", left.empty() ? "every scratch file removed" : "some files remain");
		if (!left.empty())
		{
			context.Fail("still present after RemoveFile:%s", left.c_str());
		}
	});

	CS2ST_CALL(context, "IFileSystem::DeleteDirectory", {
		const bool ok = fs_->DeleteDirectory(kScratchDir, kProbePathId);
		const bool gone = !fs_->IsDirectory(kScratchDir, kProbePathId);
		context.Detail("-> %s, directory %s", YesNo(ok), gone ? "gone" : "still there");
		if (ok && !gone)
		{
			context.Fail("DeleteDirectory said ok but %s is still a directory", kScratchDir);
		}
		else if (!ok && !gone)
		{
			context.Suspect("DeleteDirectory failed on the emptied %s", kScratchDir);
		}
	});

	// The third parameter is not named by the SDK. true is tried first; if the
	// tree survives, false is tried and the row says which one worked. Whatever
	// is left is removed by hand, so the tree never outlives the suite.
	CS2ST_CALL(context, "IFileSystem::DeleteDirectoryAndContents_R", {
		FileHandle_t out = fs_->Open(kScratchTreeFile, "wb", kProbePathId);
		if (out != FILESYSTEM_INVALID_HANDLE)
		{
			fs_->Write("x", 1, out);
			fs_->Close(out);
		}
		const bool had_file = fs_->FileExists(kScratchTreeFile, kProbePathId);
		const bool ok_true = fs_->DeleteDirectoryAndContents_R(kScratchTree, kProbePathId, true);
		bool gone = !fs_->IsDirectory(kScratchTree, kProbePathId);
		bool ok_false = false;
		bool tried_false = false;
		if (!gone)
		{
			tried_false = true;
			ok_false = fs_->DeleteDirectoryAndContents_R(kScratchTree, kProbePathId, false);
			gone = !fs_->IsDirectory(kScratchTree, kProbePathId);
		}
		const bool file_gone = !fs_->FileExists(kScratchTreeFile, kProbePathId);
		std::string text;
		AppendF(text, "-> file made %s; (..., true) %s", YesNo(had_file), YesNo(ok_true));
		if (tried_false)
		{
			AppendF(text, "; tree survived, (..., false) %s", YesNo(ok_false));
		}
		AppendF(text, "; tree %s, file %s", gone ? "gone" : "still there",
			file_gone ? "gone" : "still there");
		context.Detail("%s", text.c_str());
		// By hand, whatever the call left behind.
		if (!file_gone)
		{
			fs_->RemoveFile(kScratchTreeFile, kProbePathId);
		}
		if (!gone)
		{
			fs_->DeleteDirectory(kScratchTreeInner, kProbePathId);
			fs_->DeleteDirectory(kScratchTree, kProbePathId);
		}
		if (had_file && !file_gone)
		{
			context.Fail("the file inside %s survived DeleteDirectoryAndContents_R", kScratchTree);
		}
		else if (!gone)
		{
			context.Suspect("the directory %s survived DeleteDirectoryAndContents_R", kScratchTree);
		}
	});
}

// -----------------------------------------------------------------------------
//  Everything that changes state
// -----------------------------------------------------------------------------

void FileSystemSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[IFileSystem] global state-changing slots with no way to put the state back: not called");

	context.SkipCall("IFileSystem::LoadModule", "loads a native library into the game process");
	context.SkipCall("IFileSystem::UnloadModule", "pairs with LoadModule, which is skipped");
	context.SkipCall("IFileSystem::SetWarningFunc", "replaces the file system's global warning callback");
	context.SkipCall("IFileSystem::SetWarningLevel", "changes global verbosity and there is no getter to restore it");
	// The returned KeyValues is only recorded: this plugin links no KeyValues
	// implementation, so it is neither read nor deleted (it is left allocated),
	// and the overload below cannot get a head object to fill.
	CS2ST_CALL(context, "IFileSystem::LoadKeyValues(type,filename,pathID)", {
		// Intentional leak: the result is never deleted, freed when the process exits.
		KeyValues* kv = fs_->LoadKeyValues(IFileSystem::TYPE_VMT, kProbeFile, kProbePathId);
		context.Detail("-> %p (left allocated, not read)", static_cast<void*>(kv));
	});
	context.SkipCall("IFileSystem::LoadKeyValues(head,type,filename,pathID)", "needs a constructed KeyValues head and this plugin links no KeyValues constructor");
	context.SkipCall("IFileSystem::AddUGCVPKFile", "mounts a UGC VPK and this suite has none to mount");
	context.SkipCall("IFileSystem::RemoveUGCVPKFile", "pairs with AddUGCVPKFile, which is skipped");
	context.SkipCall("IFileSystem::LoadUGC", "loads UGC content and no UGC content exists to load");
	context.SkipCall("IFileSystem::UnloadUGC", "pairs with LoadUGC, which is skipped");
	context.SkipCall("IFileSystem::MarkContentCorrupt", "flags content as corrupt for the whole session");
	context.SkipCall("IFileSystem::SetContentCorruptionReportingEnabled", "changes global corruption reporting and there is no getter to restore it");
	context.SkipCall("IFileSystem::WriteFileAccessLog", "writes a file to disk and may clear the access log");
	context.SkipCall("IFileSystem::SetVPKSignatureVerification", "changes the key and callbacks used to verify VPKs mounted afterwards");
	context.SkipCall("IFileSystem::MarkVPKAsUntrustedAddon", "marks a mounted VPK untrusted for the whole session");

	// EnableBlockingFileAccessTracking, IsBlockingFileAccessEnabled and
	// RetrieveBlockingFileAccessInfo sit behind #if defined(TRACK_BLOCKING_IO)
	// in the SDK header, which this project does not define: they are not
	// vtable slots here, so they get no row (audit-coverage.sh lists them).
}

} // namespace cs2st
