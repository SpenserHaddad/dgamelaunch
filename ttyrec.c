/*
 * Copyright (c) 1980 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* 1999-02-22 Arkadiusz Mi�kiewicz <misiek@misiek.eu.org>
 * - added Native Language Support
 */

/* 2000-12-27 Satoru Takabayashi <satoru@namazu.org>
 * - modify `script' to create `ttyrec'.
 */

/*
 * script
 */

#include "dgamelaunch.h"
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <termios.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#ifndef NOSTREAMS
# include <stropts.h>
#endif
#include <stdlib.h>

#include "ttyrec.h"
#include "io.h"

#ifndef XCASE
# define XCASE 0
#endif

int slave;
pid_t child, subchild;
pid_t input_child;
char* ipfile = NULL;

volatile int wait_for_menu = 0;

FILE *fscript;
int master;

struct termios tt;
struct winsize win;
int uflg;

int
ttyrec_main (int game, char *username, char *ttyrec_path, char* ttyrec_filename)
{
  char dirname[100];

  child = subchild = input_child = 0;

  if (ttyrec_path[strlen(ttyrec_path)-1] == '/')
      snprintf (dirname, 100, "%s%s", ttyrec_path, ttyrec_filename);
  else
      snprintf (dirname, 100, "%s/%s", ttyrec_path, ttyrec_filename);

  atexit(&remove_ipfile);
  if ((fscript = fopen (dirname, "w")) == NULL)
    {
      perror (dirname);
      fail ();
    }
  setbuf (fscript, NULL);

  fixtty ();

  (void) signal (SIGCHLD, finish);
  child = fork ();
  if (child < 0)
    {
      perror ("fork");
      fail ();
    }
  if (child == 0)
    {
      subchild = child = fork ();
      if (child < 0)
        {
          perror ("fork");
          fail ();
        }
      if (child)
        {
          close (slave);
          ipfile = gen_inprogress_lock (game, child, ttyrec_filename);
          dooutput ();
        }
      else
	  doshell (game, username);
    }

  (void) fclose (fscript);

  wait_for_menu = 1;
  input_child = fork();
  if (input_child < 0)
  {
      perror ("fork2");
      fail ();
  }
  if (!input_child)
      doinput ();
  else
  {
      while (wait_for_menu)
	  sleep(1);
  }

  unlink (ipfile);

  return 0;
}

void
doinput ()
{
  register int cc;
  char ibuf[BUFSIZ];

  while ((cc = read (0, ibuf, BUFSIZ)) > 0)
    (void) write (master, ibuf, cc);
  done ();
}

void
finish (int sig)
{
  int status;
  register int pid;
  register int die = 0;

  (void)sig; /* unused */

  while ((pid = wait3 (&status, WNOHANG, 0)) > 0)
  {
    if (pid == child)
      die = 1;
  }

  if (die)
  {
      if (wait_for_menu && input_child)
      {
	  // Need to kill the child that's writing input to pty.
	  kill(input_child, SIGTERM);
	  while ((pid = wait3(&status, WNOHANG, 0)) > 0);
	  wait_for_menu = 0;
      }
      else
	  done ();
  }
}

struct linebuf
{
  char str[BUFSIZ + 1];         /* + 1 for an additional NULL character. */
  int len;
};


void
check_line (const char *line)
{
  static int uuencode_mode = 0;
  static FILE *uudecode;

  if (uuencode_mode == 1)
    {
      fprintf (uudecode, "%s", line);
      if (strcmp (line, "end\n") == 0)
        {
          pclose (uudecode);
          uuencode_mode = 0;
        }
    }
  else
    {
      int dummy;
      char dummy2[BUFSIZ];
      if (sscanf (line, "begin %o %s", &dummy, dummy2) == 2)
        {
          /* 
           * uuencode line found! 
           */
          uudecode = popen ("uudecode", "w");
          fprintf (uudecode, "%s", line);
          uuencode_mode = 1;
        }
    }
}

void
check_output (const char *str, int len)
{
  static struct linebuf lbuf = { "", 0 };
  int i;

  for (i = 0; i < len; i++)
    {
      if (lbuf.len < BUFSIZ)
        {
          lbuf.str[lbuf.len] = str[i];
          if (lbuf.str[lbuf.len] == '\r')
            {
              lbuf.str[lbuf.len] = '\n';
            }
          lbuf.len++;
          if (lbuf.str[lbuf.len - 1] == '\n')
            {
              if (lbuf.len > 1)
                {               /* skip a blank line. */
                  lbuf.str[lbuf.len] = '\0';
                  check_line (lbuf.str);
                }
              lbuf.len = 0;
            }
        }
      else
        {                       /* buffer overflow */
          lbuf.len = 0;
        }
    }
}

void
dooutput ()
{
  int cc;
  time_t tvec, time ();
  char obuf[BUFSIZ], *ctime ();

  setbuf (stdout, NULL);
  (void) close (0);
  tvec = time ((time_t *) NULL);
  for (;;)
    {
      Header h;

      cc = read (master, obuf, BUFSIZ);
      if (cc <= 0)
        break;
      if (uflg)
        check_output (obuf, cc);
      h.len = cc;
      gettimeofday (&h.tv, NULL);
      (void) write (1, obuf, cc);
      (void) write_header (fscript, &h);
      (void) fwrite (obuf, 1, cc, fscript);
    }
  done ();
}

void
doshell (int game, char *username)
{
  getslave ();
  (void) close (master);
  (void) fclose (fscript);
  (void) dup2 (slave, 0);
  (void) dup2 (slave, 1);
  (void) dup2 (slave, 2);
  (void) close (slave);

  /*
  if (myconfig[game]->mkdir)
      (void) mkdir(myconfig[game]->mkdir, 0755);

  if (myconfig[game]->chdir)
      (void) chdir(myconfig[game]->chdir);
  */

  execvp (myconfig[game]->game_path, myconfig[game]->bin_args);

  fail ();
}

void
fixtty ()
{
  struct termios rtt;

  rtt = tt;
  rtt.c_iflag = 0;
  rtt.c_lflag &= ~(ISIG | ICANON | XCASE | ECHO | ECHOE | ECHOK | ECHONL);
  rtt.c_oflag = OPOST;
  rtt.c_cc[VINTR] = _POSIX_VDISABLE;
  rtt.c_cc[VQUIT] = _POSIX_VDISABLE;
  rtt.c_cc[VERASE] = _POSIX_VDISABLE;
  rtt.c_cc[VKILL] = _POSIX_VDISABLE;
  rtt.c_cc[VEOF] = 1;
  rtt.c_cc[VEOL] = 0;
  rtt.c_cc[VMIN] = 1;
  rtt.c_cc[VTIME] = 0;
  (void) tcsetattr (0, TCSAFLUSH, &rtt);
}

void
fail ()
{

  (void) kill (0, SIGTERM);
  done ();
}

void
done ()
{
  time_t tvec, time ();
  char *ctime ();

  if (subchild)
    {
      tvec = time ((time_t *) NULL);
      (void) fclose (fscript);
      (void) close (master);
    }
  else
    {
      (void) tcsetattr (0, TCSAFLUSH, &tt);
    }

  unlink(ipfile);
  graceful_exit (0);
}

void
getslave ()
{
  (void) setsid ();
  /* grantpt( master);
     unlockpt(master);
     if ((slave = open((const char *)ptsname(master), O_RDWR)) < 0) {
     perror((const char *)ptsname(master));
     fail();
     perror("open(fd, O_RDWR)");
     fail();
     } */
#ifndef NOSTREAMS
  if (isastream (slave))
    {
      if (ioctl (slave, I_PUSH, "ptem") < 0)
        {
          perror ("ioctl(fd, I_PUSH, ptem)");
          fail ();
        }
      if (ioctl (slave, I_PUSH, "ldterm") < 0)
        {
          perror ("ioctl(fd, I_PUSH, ldterm)");
          fail ();
        }
#ifndef _HPUX_SOURCE
      if (ioctl (slave, I_PUSH, "ttcompat") < 0)
        {
          perror ("ioctl(fd, I_PUSH, ttcompat)");
          fail ();
        }
#endif
    }
#endif /* !NOSTREAMS */
  (void) ioctl (0, TIOCGWINSZ, (char *) &win);
}

void
remove_ipfile (void)
{
  if (ipfile != NULL)
    unlink (ipfile);
}
