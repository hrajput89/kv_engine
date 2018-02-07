/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "breakpad.h"
#include "memcached.h"
#include "utilities/terminate_handler.h"

#include <platform/platform.h>

#include <platform/backtrace.h>

using namespace google_breakpad;

// Unique_ptr which holds the pointer to the installed
// breakpad handler
static std::unique_ptr<ExceptionHandler> handler;

#if defined(WIN32) || defined(linux)
// These methods is called from breakpad when creating
// the dump. They're inside the #ifdef block to avoid
// compilers to complain about static functions never
// being used.

static void write_to_logger(void* ctx, const char* frame) {
    CB_CRIT("    {}", frame);
}

static void dump_stack() {
    CB_CRIT("Stack backtrace of crashed thread:");
    print_backtrace(write_to_logger, NULL);
    cb::logger::get()->flush();
}
#endif

// Unfortunately Breakpad use a different API on each platform,
// so we need a bit of #ifdef's..

#ifdef WIN32
static bool dumpCallback(const wchar_t* dump_path,
                         const wchar_t* minidump_id,
                         void* context,
                         EXCEPTION_POINTERS* exinfo,
                         MDRawAssertionInfo* assertion,
                         bool succeeded) {
    // Unfortnately the filenames is in wchar's and I got compiler errors
    // from fmt when trying to print them by using {}. Let's just format
    // it into a string first.
    char file[512];
    sprintf(file, "%S\\%S.dmp", dump_path, minidump_id);

    CB_CRIT("Breakpad caught crash in memcached version {}. Writing crash dump "
            "to {} before terminating.",
            get_server_version(),
            file);
    dump_stack();
    return succeeded;
}
#elif defined(linux)
static bool dumpCallback(const MinidumpDescriptor& descriptor,
                         void* context,
                         bool succeeded) {
    CB_CRIT("Breakpad caught crash in memcached version {}. Writing crash dump "
            "to {} before terminating.",
            get_server_version(),
            descriptor.path());

    dump_stack();
    return succeeded;
}
#endif

void create_handler(const std::string& minidump_dir) {
#ifdef WIN32
    // Takes a wchar_t* on Windows. Isn't the Breakpad API nice and
    // consistent? ;)
    size_t len = minidump_dir.length() + 1;
    std::wstring wc_minidump_dir(len, '\0');
    size_t wlen = 0;
    mbstowcs_s(
            &wlen, &wc_minidump_dir[0], len, minidump_dir.c_str(), _TRUNCATE);

    handler.reset(new ExceptionHandler(&wc_minidump_dir[0],
                                       /*filter*/ NULL,
                                       dumpCallback,
                                       /*callback-context*/ NULL,
                                       ExceptionHandler::HANDLER_ALL,
                                       MiniDumpNormal,
                                       /*pipe*/ (wchar_t*)NULL,
                                       /*custom_info*/ NULL));
#elif defined(linux)
    MinidumpDescriptor descriptor(minidump_dir.c_str());
    handler.reset(new ExceptionHandler(descriptor,
                                       /*filter*/ NULL,
                                       dumpCallback,
                                       /*callback-context*/ NULL,
                                       /*install_handler*/ true,
                                       /*server_fd*/ -1));
#else
// Not supported on this plaform
#endif
}

void cb::breakpad::initialize(const cb::breakpad::Settings& settings) {
    // We cannot actually change any of breakpad's settings once created, only
    // remove it and re-create with new settings.
    destroy();

    if (settings.enabled) {
        create_handler(settings.minidump_dir);
    }

    if (handler) {
        // Turn off the terminate handler's backtrace - otherwise we
        // just print it twice.
        set_terminate_handler_print_backtrace(false);

        CB_INFO("Breakpad enabled. Minidumps will be written to '{}'",
                settings.minidump_dir);
    } else {
        // If breakpad is off, then at least print the backtrace via
        // terminate_handler.
        set_terminate_handler_print_backtrace(true);
        CB_INFO("Breakpad disabled");
    }
}

void cb::breakpad::destroy() {
    if (handler) {
        CB_INFO("Disabling Breakpad");
        set_terminate_handler_print_backtrace(true);
    }
    handler.reset();
}