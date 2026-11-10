/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* rfxengine.exe: runs the engine in libxul (lib/rfxengine.cpp has the usage).
 *
 * Serving the app (no --shot), it is a watchdog by default: it runs the engine as a child
 * process (same arguments plus --child) and starts it again whenever it exits or crashes, so the
 * app only sees a short reconnect. The child's crash reports go to the same output. A child whose
 * main loop stops signalling a heartbeat event is killed and restarted too (a hung page). A child
 * that crashes within a minute doubles the wait before the next start (1 s up to 60 s), so a
 * crash at startup doesn't spin; a clean exit (the app asked for a restart) starts again at once.
 * --no-watchdog runs the engine in this process.
 *
 * xul.dll is delay-loaded (moz.build), so the watchdog itself never loads Gecko. */

#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" __declspec(dllimport) int RetrofoxEngineMain(int argc, char** argv);

static bool
HasArg(int argc, char** argv, const char* aArg)
{
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], aArg)) {
      return true;
    }
  }
  return false;
}

static void
Log(const char* aFormat, ...)
{
  SYSTEMTIME t;
  GetLocalTime(&t);
  printf("%02u:%02u:%02u rfxengine watchdog: ", t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, aFormat);
  vprintf(aFormat, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}

// The engine signals this event from its main loop (Heartbeat() in lib/rfxengine.cpp). Startup
// (Gecko itself, then waiting for the app) gets longer before the first signal.
static const DWORD kStartupMs = 90000;
static const DWORD kHungMs = 30000;

// Runs the engine as a child and waits for it; returns its exit code. An engine whose main loop
// stops signalling for kHungMs is killed, so a hung page restarts like a crashed one.
static DWORD
RunChild()
{
  SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
  HANDLE heartbeat = CreateEventW(&sa, FALSE, FALSE, nullptr);
  if (heartbeat) {
    char value[32];
    snprintf(value, sizeof(value), "%lu", (unsigned long)(uintptr_t)heartbeat);
    SetEnvironmentVariableA("RFXENGINE_HEARTBEAT", value);
  }

  // Our own command line, with --child added
  wchar_t cmd[4096];
  _snwprintf_s(cmd, sizeof(cmd) / sizeof(cmd[0]), _TRUNCATE, L"%s --child", GetCommandLineW());
  STARTUPINFOW si = { sizeof(si) };
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi;
  // No console for the child: ours usually belongs to the telnet session that started us, and
  // once that has closed a child that tries to attach to it fails to start (0xC0000142). Its
  // output still reaches our log through the handles above.
  if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, DETACHED_PROCESS, nullptr, nullptr,
                      &si, &pi)) {
    Log("can't start the engine (error %lu)", GetLastError());
    if (heartbeat) {
      CloseHandle(heartbeat);
    }
    return DWORD(-1);
  }
  CloseHandle(pi.hThread);
  if (heartbeat) {
    HANDLE both[2] = { pi.hProcess, heartbeat };
    DWORD wait = kStartupMs;
    for (;;) {
      DWORD r = WaitForMultipleObjects(2, both, FALSE, wait);
      if (r == WAIT_OBJECT_0) {
        break;                      // the engine exited
      }
      if (r == WAIT_OBJECT_0 + 1) {
        wait = kHungMs;             // alive
        continue;
      }
      Log("the engine stopped responding for %lu s, restarting it", wait / 1000);
      TerminateProcess(pi.hProcess, 0xDEADu);
      WaitForSingleObject(pi.hProcess, 5000);
      break;
    }
    CloseHandle(heartbeat);
  } else {
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return code;
}

int
main(int argc, char** argv)
{
  if (HasArg(argc, argv, "--child") || HasArg(argc, argv, "--shot") ||
      HasArg(argc, argv, "--no-watchdog")) {
    // RetrofoxEngineMain doesn't know these
    int n = 1;
    for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--child") && strcmp(argv[i], "--no-watchdog")) {
        argv[n++] = argv[i];
      }
    }
    argv[n] = nullptr;
    return RetrofoxEngineMain(n, argv);
  }

  DWORD delayMs = 1000;
  for (unsigned run = 1;; ++run) {
    ULONGLONG start = GetTickCount64();
    Log("starting the engine (run %u)", run);
    DWORD code = RunChild();
    ULONGLONG lived = (GetTickCount64() - start) / 1000;
    // A clean exit (0) is a restart the app asked for: right away, no backoff
    if (lived >= 60 || code == 0) {
      delayMs = 1000;
    }
    Log("the engine exited with 0x%08lx after %llu s, starting it again in %lu s",
        code, lived, delayMs / 1000);
    Sleep(delayMs);
    if (lived < 60 && code != 0) {
      delayMs = delayMs * 2 > 60000 ? 60000 : delayMs * 2;
    }
  }
}
