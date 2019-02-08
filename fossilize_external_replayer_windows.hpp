/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "layer/utils.hpp"
#include <string>
#include <atomic>

#define EXTERNAL_SHARED_MUTEX mutex
#include "fossilize_external_replayer_control_block.hpp"

namespace Fossilize
{
static std::atomic<int32_t> shm_index;

struct ExternalReplayer::Impl
{
	~Impl();

	HANDLE process = INVALID_HANDLE_VALUE;
	HANDLE mapping_handle = INVALID_HANDLE_VALUE;
	HANDLE mutex = INVALID_HANDLE_VALUE;
	SharedControlBlock *shm_block = nullptr;
	size_t shm_block_size = 0;

	bool start(const ExternalReplayer::Options &options);
	ExternalReplayer::PollResult poll_progress(Progress &progress);
	uintptr_t get_process_handle() const;
	bool wait();
	bool is_process_complete();
	bool kill();
};

ExternalReplayer::Impl::~Impl()
{
	if (shm_block)
		UnmapViewOfFile(shm_block);
	if (mapping_handle != INVALID_HANDLE_VALUE)
		CloseHandle(mapping_handle);
	if (mutex != INVALID_HANDLE_VALUE)
		CloseHandle(mutex);
	if (process != INVALID_HANDLE_VALUE)
		CloseHandle(process);
}

uintptr_t ExternalReplayer::Impl::get_process_handle() const
{
	return reinterpret_cast<uintptr_t>(process);
}

ExternalReplayer::PollResult ExternalReplayer::Impl::poll_progress(ExternalReplayer::Progress &progress)
{
	if (process == INVALID_HANDLE_VALUE)
		return ExternalReplayer::PollResult::Error;

	bool complete = shm_block->progress_complete.load(std::memory_order_acquire);

	if (!shm_block->progress_started.load(std::memory_order_acquire))
		return ExternalReplayer::PollResult::ResultNotReady;

	progress.compute.total = shm_block->total_compute.load(std::memory_order_relaxed);
	progress.compute.skipped = shm_block->skipped_compute.load(std::memory_order_relaxed);
	progress.compute.completed = shm_block->successful_compute.load(std::memory_order_relaxed);
	progress.graphics.total = shm_block->total_graphics.load(std::memory_order_relaxed);
	progress.graphics.skipped = shm_block->skipped_graphics.load(std::memory_order_relaxed);
	progress.graphics.completed = shm_block->successful_graphics.load(std::memory_order_relaxed);
	progress.total_modules = shm_block->total_modules.load(std::memory_order_relaxed);
	progress.banned_modules = shm_block->banned_modules.load(std::memory_order_relaxed);
	progress.clean_crashes = shm_block->clean_process_deaths.load(std::memory_order_relaxed);
	progress.dirty_crashes = shm_block->dirty_process_deaths.load(std::memory_order_relaxed);

	SHARED_CONTROL_BLOCK_LOCK(shm_block);
	size_t read_avail = shared_control_block_read_avail(shm_block);
	for (size_t i = ControlBlockMessageSize; i <= read_avail; i += ControlBlockMessageSize)
	{
		char buf[ControlBlockMessageSize] = {};
		shared_control_block_read(shm_block, buf, sizeof(buf));
		LOGI("From FIFO: %s\n", buf);
	}
	SHARED_CONTROL_BLOCK_UNLOCK(shm_block);
	return complete ? ExternalReplayer::PollResult::Complete : ExternalReplayer::PollResult::Running;
}

bool ExternalReplayer::Impl::is_process_complete()
{
	if (process == INVALID_HANDLE_VALUE)
		return true;
	return WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
}

bool ExternalReplayer::Impl::wait()
{
	if (process == INVALID_HANDLE_VALUE)
		return false;

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
		return false;

	// Pump the fifo through.
	poll_progress(progress);

	DWORD code = 0;
	GetExitCodeProcess(process, &code);
	CloseHandle(process);
	process = INVALID_HANDLE_VALUE;
	return code == 0;
}

bool ExternalReplayer::Impl::kill()
{
	if (process == INVALID_HANDLE_VALUE)
		return false;
	return TerminateProcess(process, 1);
}

bool ExternalReplayer::Impl::start(const ExternalReplayer::Options &options)
{
	// Reserve 4 kB for control data, and 64 kB for a cross-process SHMEM ring buffer.
	shm_block_size = 64 * 1024 + 4 * 1024;

	char shm_name[256];
	char shm_mutex_name[256];
	sprintf(shm_name, "fossilize-external-%lu-%d", GetCurrentProcessId(), shm_index.fetch_add(1, std::memory_order_relaxed));
	sprintf(shm_mutex_name, "fossilize-external-%lu-%d", GetCurrentProcessId(), shm_index.fetch_add(1, std::memory_order_relaxed));
	mapping_handle = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, shm_block_size, shm_name);

	if (mapping_handle == INVALID_HANDLE_VALUE)
	{
		LOGE("Failed to create file mapping.\n");
		return false;
	}

	shm_block = static_cast<SharedControlBlock *>(MapViewOfFile(mapping_handle, FILE_MAP_READ | FILE_MAP_WRITE,
	                                                            0, 0, shm_block_size));

	if (!shm_block)
	{
		LOGE("Failed to mmap shared block.\n");
		return false;
	}

	// I believe zero-filled pages are guaranteed, but don't take any chances.
	// Cast to void explicitly to avoid warnings on GCC 8.
	memset(static_cast<void *>(shm_block), 0, shm_block_size);
	shm_block->version_cookie = ControlBlockMagic;
	shm_block->ring_buffer_size = 64 * 1024;
	shm_block->ring_buffer_offset = 4 * 1024;

	mutex = CreateMutexA(nullptr, FALSE, shm_mutex_name);
	if (mutex == INVALID_HANDLE_VALUE)
	{
		LOGE("Failed to create named mutex.\n");
		return false;
	}

	std::string cmdline;

	cmdline += "\"";
	cmdline += options.external_replayer_path;
	cmdline += "\" ";
	cmdline += "\"";
	cmdline += options.database;
	cmdline += "\"";
	cmdline += " --master-process";
	cmdline += " --quiet-slave";
	cmdline += " --shm-name ";
	cmdline += shm_name;
	cmdline += " --shm-mutex-name ";
	cmdline += shm_mutex_name;

	if (options.pipeline_cache)
		cmdline += " --pipeline-cache";

	if (options.num_threads)
	{
		cmdline += " --num-threads ";
		cmdline += std::to_string(options.num_threads);
	}

	if (options.on_disk_pipeline_cache)
	{
		cmdline += " --on-disk-pipeline-cache ";
		cmdline += "\"";
		cmdline += options.on_disk_pipeline_cache;
		cmdline += "\"";
	}

	STARTUPINFO si = {};
	si.cb = sizeof(STARTUPINFO);
	si.dwFlags = STARTF_USESTDHANDLES;
	SECURITY_ATTRIBUTES attrs = {};
	attrs.bInheritHandle = TRUE;
	attrs.nLength = sizeof(attrs);

	HANDLE nul = INVALID_HANDLE_VALUE;
	if (options.quiet)
	{
		nul = CreateFileA("NUL", GENERIC_WRITE, 0, &attrs, OPEN_EXISTING, 0, nullptr);
		if (nul == INVALID_HANDLE_VALUE)
		{
			LOGE("Failed to open NUL file for writing.\n");
			return false;
		}

		si.hStdError = nul;
		si.hStdOutput = nul;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	}
	else
	{
		if (!SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		{
			LOGE("Failed to enable inheritance for stderror handle.\n");
			return false;
		}
		si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

		if (!SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		{
			LOGE("Failed to enable inheritance for stderror handle.\n");
			return false;
		}
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	}

	// For whatever reason, this string must be mutable. Dupe it.
	char *duped_string = _strdup(cmdline.c_str());
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessA(nullptr, duped_string, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
	                    &si, &pi))
	{
		LOGE("Failed to create child process.\n");
		free(duped_string);
		return false;
	}

	free(duped_string);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);

	CloseHandle(pi.hThread);
	process = pi.hProcess;
	return true;
}
}

