#include "suites/FileSystemSuite.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

	CS2ST_CALL(context, "IFileSystem::IsFileInReadOnlySearchPath", {
		context.Detail("-> %s",
			YesNo(fs_->IsFileInReadOnlySearchPath(kProbePathId, kProbeFile)));
	});

	context.SkipCall("IFileSystem::Precache", "loads the file into the engine's cache");
	context.SkipCall("IFileSystem::SetFileWritable", "changes file attributes");
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

	CS2ST_CALL(context, "IFileSystem::ReadToBuffer", {
		fs_->Seek(handle, 0, FILESYSTEM_SEEK_HEAD);
		CUtlBuffer buffer(0, 0, CUtlBuffer::BufferFlags_t::BF_NONE);
		const bool ok = fs_->ReadToBuffer(handle, buffer, 64, nullptr);
		context.Detail("-> %s, %d bytes", YesNo(ok), buffer.TellPut());
	});

	CS2ST_CALL(context, "IFileSystem::Close", {
		fs_->Close(handle);
		context.Detail("-> closed %p", handle);
	});

	context.SkipCall("IFileSystem::Write", "the probe handle is read-only; writing would touch game files");
	context.SkipCall("IFileSystem::FPrintf", "the probe handle is read-only; writing would touch game files");
	context.SkipCall("IFileSystem::unk001(FileHandle_t,bool)", "unknown semantics; may flush or discard");
}

// -----------------------------------------------------------------------------
//  Whole-file reads into caller buffers
// -----------------------------------------------------------------------------

void FileSystemSuite::ExerciseBufferReads(TestContext& context)
{
	context.Note("[IFileSystem] whole-file reads; the file length has several routes");

	CS2ST_CALL(context, "IFileSystem::ReadFile", {
		const int64 size = fs_->Size(kProbeFile, kProbePathId);
		CUtlBuffer buffer(0, 0, CUtlBuffer::BufferFlags_t::BF_NONE);
		const bool ok = fs_->ReadFile(kProbeFile, kProbePathId, buffer, 0, 0, nullptr);
		context.Detail("-> %s, %d bytes (Size %lld)", YesNo(ok), buffer.TellPut(),
			static_cast<long long>(size));
		if (ok && size >= 0 && buffer.TellPut() != size)
		{
			context.Fail("ReadFile delivered %d bytes of a %lld-byte file",
				buffer.TellPut(), static_cast<long long>(size));
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

	context.SkipCall("IFileSystem::WriteFile", "writes a file on the write path");
	context.SkipCall("IFileSystem::UnzipFile", "writes extracted files to disk");
	context.SkipCall("IFileSystem::CopyAFile", "writes a file on the write path");
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

	CS2ST_CALL(context, "IFileSystem::GetWritePath", {
		CBufferString path;
		const bool ok = fs_->GetWritePath(kProbeFile, kProbePathId, path);
		context.Detail("-> %s, \"%s\"", YesNo(ok), path.Get());
	});

	CS2ST_CALL(context, "IFileSystem::GetSearchPath", {
		CBufferString paths;
		const bool ok = fs_->GetSearchPath(kProbePathId, GET_SEARCH_PATH_ALL, paths, 4);
		context.Detail("-> %s, \"%.200s\"", YesNo(ok), paths.Get());
	});

	CS2ST_CALL(context, "IFileSystem::GetSearchPathsForPathID", {
		CUtlVector<CUtlString> paths;
		fs_->GetSearchPathsForPathID(kProbePathId, GET_SEARCH_PATH_ALL, paths);
		const int count = paths.Count();
		context.Detail("-> %d path%s", count, count == 1 ? "" : "s");
		// Bounded: the count is the engine's, and only the head is worth a row.
		for (int i = 0; i < count && i < 4; ++i)
		{
			context.Detail("   [%d] \"%.160s\"", i, paths[i].Get());
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

	context.SkipCall("IFileSystem::AddSearchPath", "mounts a search path");
	context.SkipCall("IFileSystem::RemoveSearchPath", "unmounts a search path");
	context.SkipCall("IFileSystem::SaveSearchPathState", "allocates engine state that must be restored or destroyed in order");
	context.SkipCall("IFileSystem::RestoreSearchPathState", "replaces the live search paths");
	context.SkipCall("IFileSystem::DestroySearchPathState", "pairs with SaveSearchPathState, which is skipped");
	context.SkipCall("IFileSystem::RemoveAllSearchPaths", "would empty every search path");
	context.SkipCall("IFileSystem::RemoveSearchPaths", "unmounts every path with a path ID");
	context.SkipCall("IFileSystem::MarkPathIDByRequestOnly", "changes how a path ID is searched");
	context.SkipCall("IFileSystem::SetSearchPathReadOnly", "changes search path attributes");
	context.SkipCall("IFileSystem::unk003", "unknown semantics; returns an object by value");
	context.SkipCall("IFileSystem::unk004", "unknown semantics");
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

	CS2ST_CALL(context, "IFileSystem::FindFileAbsoluteList", {
		CUtlVector<CUtlString> list;
		fs_->FindFileAbsoluteList(list, kProbeFile, kProbePathId);
		const int count = list.Count();
		context.Detail("-> %d match%s", count, count == 1 ? "" : "es");
		for (int i = 0; i < count && i < 4; ++i)
		{
			context.Detail("   [%d] \"%.160s\"", i, list[i].Get());
		}
	});

	CS2ST_CALL(context, "IFileSystem::GetAvailableDrives", {
		CUtlVector<CUtlString> drives;
		fs_->GetAvailableDrives(drives);
		const int count = drives.Count();
		context.Detail("-> %d drive%s", count, count == 1 ? "" : "s");
		for (int i = 0; i < count && i < 8; ++i)
		{
			context.Detail("   [%d] \"%s\"", i, drives[i].Get());
		}
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
}

// -----------------------------------------------------------------------------
//  Everything that changes state
// -----------------------------------------------------------------------------

void FileSystemSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[IFileSystem] state-changing slots: not called");

	context.SkipCall("IFileSystem::unk001(const char*,void*,bool)", "unknown semantics; takes a path");
	context.SkipCall("IFileSystem::RemoveFile", "deletes a file");
	context.SkipCall("IFileSystem::RenameFile", "renames a file");
	context.SkipCall("IFileSystem::CreateDirHierarchy", "creates directories on disk");
	context.SkipCall("IFileSystem::CreateDirHierarchyForFile", "creates directories on disk");
	context.SkipCall("IFileSystem::DeleteDirectory", "deletes a directory");
	context.SkipCall("IFileSystem::DeleteDirectoryAndContents_R", "recursively deletes a directory");
	context.SkipCall("IFileSystem::LoadModule", "loads a native library into the game process");
	context.SkipCall("IFileSystem::UnloadModule", "pairs with LoadModule, which is skipped");
	context.SkipCall("IFileSystem::SetWarningFunc", "replaces the file system's global warning callback");
	context.SkipCall("IFileSystem::SetWarningLevel", "changes global verbosity and there is no getter to restore it");
	context.SkipCall("IFileSystem::LoadKeyValues(type,filename,pathID)", "returns a KeyValues owned by the engine's allocator");
	context.SkipCall("IFileSystem::LoadKeyValues(head,type,filename,pathID)", "fills a KeyValues; its allocator would cross heaps");
	context.SkipCall("IFileSystem::OpenUGCFile", "opens a UGC file; no UGC content is mounted");
	context.SkipCall("IFileSystem::AddUGCVPKFile", "mounts a UGC VPK");
	context.SkipCall("IFileSystem::RemoveUGCVPKFile", "unmounts a UGC VPK");
	context.SkipCall("IFileSystem::EnableAutoVPKFileLoading", "changes global VPK loading policy");
	context.SkipCall("IFileSystem::ValidateLoadedVPKs", "hashes every mounted VPK; takes seconds");
	context.SkipCall("IFileSystem::LoadUGC", "loads UGC content");
	context.SkipCall("IFileSystem::UnloadUGC", "unloads UGC content");
	context.SkipCall("IFileSystem::SetIODelayAlarm", "arms a process-wide breakpoint alarm");
	context.SkipCall("IFileSystem::MarkContentCorrupt", "flags content as corrupt for the whole session");

	// EnableBlockingFileAccessTracking, IsBlockingFileAccessEnabled and
	// RetrieveBlockingFileAccessInfo sit behind #if defined(TRACK_BLOCKING_IO)
	// in the SDK header, which this project does not define: they are not
	// vtable slots here, so they get no row (audit-coverage.sh lists them).
}

} // namespace cs2st
