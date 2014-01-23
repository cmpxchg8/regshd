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
 
 The only way this will work is if both regsh and regshd monitor keys locally
 and each has access to registry which is slightly problematic...
 
 So if a command needs to be sent from regsh to regshd, it's written to remote
 registry key. The problem is that in order for regsh to know when output is
 available, regshd will need remote access to regsh key...
 
 it's a problem because you need to give remote registry access to each app
 otherwise RegNotifyChangeKeyValue won't play ball.
 
 Cannot monitor event logs remotely
 
 readdirectorychangesw does work on remote devices but is unreliable apparently
 */
#define APP_NAME     "RegSH"
#define APP_VERSION  "0.1"
#define AUTHOR_EMAIL "cmpxchg8"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <windows.h>

#pragma comment (lib, "advapi32.lib")

#define STDINPUT_KEY  "SOFTWARE\\REGSH\\hStdInput"     // write to here
#define STDOUTPUT_KEY "SOFTWARE\\REGSH\\hStdOutput"    // read from here

#define INPUT_KEY  0
#define OUTPUT_KEY 1
#define USER_INPUT 2

#define MAX_EVENTS 3

HANDLE ge[MAX_EVENTS];
HKEY k[2];
int verbose=0;

struct input_data {
  DWORD len;
  char buf[BUFSIZ];
  HANDLE evt[2];
};

#define WDEBUG vout

void vout (const char fmt[], ...) {
  va_list arglist;
  char    buffer[2048];

  if (verbose == 0) return;

  va_start (arglist, fmt);
  vsnprintf (buffer, sizeof (buffer) - 1, fmt, arglist);
  va_end (arglist);

  fprintf (stdout, "\n  [*] %s", buffer);
}

void xstrerror (const char fmt[], DWORD dwError, ...) {
  char    *error;
  va_list arglist;
  char    buffer[2048];
  
  va_start (arglist, fmt);
  vsnprintf (buffer, sizeof (buffer) - 1, fmt, arglist);
  va_end (arglist);
  
  FormatMessage (
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, dwError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), 
      (LPSTR)&error, 0, NULL);

  fprintf (stdout, "  %s : %s\n", buffer, error);
  LocalFree (error);
}

DWORD WINAPI GetInputThread (void *param) {
  struct input_data *idata = (struct input_data *)param;
  HANDLE hin;

  hin = GetStdHandle (STD_INPUT_HANDLE);

  while (ReadFile(hin, idata->buf, BUFSIZ, 
      &idata->len, NULL) && idata->len > 0) {
    SetEvent (idata->evt[0]);
    WaitForSingleObject (idata->evt[1], INFINITE);
  }

  idata->len = 0;
  SetEvent (idata->evt[0]);
  return 0;
}

void run_client (char computer[])
{
  const char        *subkey[2] = { STDINPUT_KEY, STDOUTPUT_KEY };
  int               i;
  LONG              lRes;
  BOOL              bError=FALSE;
  char              in[BUFSIZ], host[BUFSIZ];
  DWORD             rn, wn;
  struct input_data idata;
  HKEY              hRegistry;
  
  // format host if one provided and connect
  if (computer != NULL)
  {
    _snprintf (host, BUFSIZ, "\\\\%s", computer);
    lRes = RegConnectRegistry (host, HKEY_LOCAL_MACHINE, &hRegistry);
    if (lRes != ERROR_SUCCESS)
    {
      printf ("\n  Unable to connect to %s", host);
      return;
    } else {
      printf ("\n  Connected to %s", host);
    }
  } else {
    hRegistry = HKEY_LOCAL_MACHINE;    // connect locally for testing purposes
  }
  
  // monitor 2 keys representing input/output of cmd.exe process
  for (i=0; i<2; i++)
  {
    // open remote keys for read/write and notify access
    lRes = RegCreateKeyEx (hRegistry, 
        subkey[i], 0, NULL, 0, KEY_WRITE | KEY_NOTIFY | KEY_READ, 
        NULL, &k[i], NULL);
        
    // abort on error
    if (lRes != ERROR_SUCCESS) {
      xstrerror ("RegCreateKeyEx()", lRes);
      bError = TRUE;
      break;
    }
    
    // create event for this key
    ge[i] = CreateEvent (NULL, FALSE, FALSE, NULL);
    
    // abort on error
    if (ge[i] == NULL) {
      bError = TRUE;
      break;
    }
    
    // if this is write key, check for any data waiting
    if (i == OUTPUT_KEY) 
    {
      ZeroMemory (in, BUFSIZ);
      rn = BUFSIZ;
      lRes = RegQueryValueEx (k[OUTPUT_KEY], NULL, NULL, 
          NULL, (PBYTE)in, &rn);
    
      // if we have data
      if (lRes == ERROR_SUCCESS)
      {
        // write to console
        WDEBUG ("Writing %i bytes to console", rn);
        WriteFile (GetStdHandle (STD_OUTPUT_HANDLE), 
            in, rn, &wn, NULL);
            
        // notify me of deletion event
        RegNotifyChangeKeyValue (k[OUTPUT_KEY], TRUE, 
            REG_NOTIFY_CHANGE_LAST_SET, ge[OUTPUT_KEY], TRUE);
            
        // then delete the data
        // which indicates to remote server we're ready for more
        WDEBUG ("Deleting data");
        RegDeleteValue (k[OUTPUT_KEY], NULL);
            
        // wait for it to trigger
        WDEBUG ("Waiting for deletion");
        WaitForSingleObject (ge[OUTPUT_KEY], INFINITE);
      }
    }
  }
  // if no error
  if (!bError)
  {
    // initialize and create thread for accepting commands
    idata.evt[0] = ge[USER_INPUT] = CreateEvent (NULL, FALSE, FALSE, NULL);
    idata.evt[1] = CreateEvent (NULL, FALSE, FALSE, NULL);
    
    CreateThread (NULL, 0, GetInputThread, &idata, 0, NULL);
    
    // continue until user decides to exit
    while (1)
    {
      // notify me of remote output being available
      RegNotifyChangeKeyValue (k[OUTPUT_KEY], TRUE, 
          REG_NOTIFY_CHANGE_LAST_SET, ge[OUTPUT_KEY], TRUE);
      
      WDEBUG ("Waiting for events");
      
      // wait for input from user or output from registry
      i = WaitForMultipleObjects (MAX_EVENTS, ge, FALSE, INFINITE);
      
      WDEBUG ("Event is %i", i);
      
      // output available?
      if (i == OUTPUT_KEY)
      {
        ZeroMemory (in, BUFSIZ);
        rn = BUFSIZ;
        lRes = RegQueryValueEx (k[OUTPUT_KEY], NULL, 
            NULL, NULL, (PBYTE)in, &rn);
        
        if (lRes == ERROR_SUCCESS)
        {
          WDEBUG ("Writing %i bytes to console", rn);
          WriteFile (GetStdHandle (STD_OUTPUT_HANDLE), 
              in, rn, &wn, NULL);
              
          // notify me of deletion event
          RegNotifyChangeKeyValue (k[OUTPUT_KEY], TRUE, 
              REG_NOTIFY_CHANGE_LAST_SET, ge[OUTPUT_KEY], TRUE);
              
          WDEBUG ("Deleting data");
          RegDeleteValue (k[OUTPUT_KEY], NULL);
            
          // wait for it to trigger
          WDEBUG ("Waiting for deletion");
          WaitForSingleObject (ge[OUTPUT_KEY], INFINITE);
        
        } else {
          WDEBUG ("Event triggered but there's no data!");
        }
      } else
      
      // input available?
      if (i == USER_INPUT) 
      {
        WDEBUG ("Sending command \"%s\"", idata.buf);
        RegSetValueEx (k[INPUT_KEY], NULL, NULL, REG_BINARY, 
            (PBYTE)idata.buf, idata.len);
        SetEvent (idata.evt[1]);
        if (strnicmp ("exit", idata.buf, 4) == 0) 
        {
          printf ("\n\nTerminating connection...bye\n");
          break;
        }
      } else {
        break;
      }
    }
    // clean up
    for (i=0; i<2; i++)
    {
      CloseHandle (ge[i]);
      RegCloseKey (k[i]);
    }
    CloseHandle (idata.evt[0]);
    CloseHandle (idata.evt[1]);
  }
  RegCloseKey (hRegistry);
}

int main (int argc, char *argv[])
{
  char *address=NULL;
  int i;
  char opt;
  
  printf ("\n  Registry Shell PoC Client v"APP_VERSION
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
    } else {
      address = argv[i];
    }
  }
  run_client (address);
  return 0;
}
