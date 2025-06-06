/* $OpenBSD: doas.c,v 1.57 2016/06/19 19:29:43 martijn Exp $ */
/*
 * Copyright (c) 2015 Ted Unangst <tedu@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#endif

#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>

#if defined(HAVE_LOGIN_CAP_H)
#include <login_cap.h>
#endif

#if defined(USE_BSD_AUTH)
#include <bsd_auth.h>
#include <readpassphrase.h>
#endif

#if defined(USE_PAM)
#include <security/pam_appl.h>

#if defined(OPENPAM) /* BSD, MacOS & certain Linux distros */
#include <security/openpam.h>
static struct pam_conv pamc = { openpam_ttyconv, NULL };

#elif defined(__LINUX_PAM__) /* Linux */
#include <security/pam_misc.h>
static struct pam_conv pamc = { misc_conv, NULL };

#elif defined(SOLARIS_PAM) /* illumos & Solaris */
#include "pm_pam_conv.h"
// static struct pam_conv pamc = { pam_tty_conv, NULL };
#if defined(OMNIOS_PAM)
    static struct pam_conv pamc = { (int (*)(int, const struct pam_message **, struct pam_response **, void *))pam_tty_conv, NULL };
#else
    static struct pam_conv pamc = { pam_tty_conv, NULL };
#endif

#endif /* OPENPAM */
#endif /* USE_PAM */

#include "doas.h"

static void 
usage(void)
{
	fprintf(stderr, "usage: doas [-nSs] [-a style] [-C config] [-u user]"
	    " command [args]\n");
	exit(1);
}

static int
parseuid(const char *s, uid_t *uid)
{
	struct passwd *pw;
        pw = getpwnam(s);
	if (pw != NULL) {
		*uid = pw->pw_uid;
		return 0;
	}
	return -1;
}

static int
uidcheck(const char *s, uid_t desired)
{
	uid_t uid;

	if (parseuid(s, &uid) != 0)
		return -1;
	if (uid != desired)
		return -1;
	return 0;
}

static int
parsegid(const char *s, gid_t *gid)
{
	struct group *gr;
        gr = getgrnam(s);
	if (gr != NULL) {
		*gid = gr->gr_gid;
		return 0;
	}
	return -1;
}

static int
match(uid_t uid, gid_t *groups, int ngroups, uid_t target, const char *cmd,
    const char **cmdargs, struct rule *r)
{
	int i;

	if (r->ident[0] == ':') {
		gid_t rgid;
		if (parsegid(r->ident + 1, &rgid) == -1)
			return 0;
		for (i = 0; i < ngroups; i++) {
			if (rgid == groups[i])
				break;
		}
		if (i == ngroups)
			return 0;
	} else {
		if (uidcheck(r->ident, uid) != 0)
			return 0;
	}
	if (r->target && uidcheck(r->target, target) != 0)
		return 0;
	if (r->cmd) {
		if (strcmp(r->cmd, cmd))
			return 0;
		if (r->cmdargs) {
			/* if arguments were given, they should match explicitly */
			for (i = 0; r->cmdargs[i]; i++) {
				if (!cmdargs[i])
					return 0;
				if (strcmp(r->cmdargs[i], cmdargs[i]))
					return 0;
			}
			if (cmdargs[i])
				return 0;
		}
	}
	return 1;
}

static int
permit(uid_t uid, gid_t *groups, int ngroups, struct rule **lastr,
    uid_t target, const char *cmd, const char **cmdargs)
{
	int i;

	*lastr = NULL;
	for (i = 0; i < nrules; i++) {
		if (match(uid, groups, ngroups, target, cmd,
		    cmdargs, rules[i]))
			*lastr = rules[i];
	}
	if (!*lastr)
		return 0;
	return (*lastr)->action == PERMIT;
}

static void
parseconfig(const char *filename, int checkperms)
{
	extern FILE *yyfp;
	extern int yyparse(void);
	struct stat sb;

	yyfp = fopen(filename, "r");
	if (!yyfp)
		err(1, checkperms ? "doas is not enabled, %s" :
		    "could not open config file %s", filename);

	if (checkperms) {
		if (fstat(fileno(yyfp), &sb) != 0)
			err(1, "fstat(\"%s\")", filename);
		if ((sb.st_mode & (S_IWGRP|S_IWOTH)) != 0)
			errx(1, "%s is writable by group or other", filename);
		if (sb.st_uid != 0)
			errx(1, "%s is not owned by root", filename);
	}

	yyparse();
	fclose(yyfp);
	if (parse_errors)
		exit(1);
}

static void 
checkconfig(const char *confpath, int argc, char **argv,
    uid_t uid, gid_t *groups, int ngroups, uid_t target)
{
	struct rule *rule;
        int status;

	#if defined(__linux__) || defined(__FreeBSD__) || defined(__MidnightBSD__)
	status = setresuid(uid, uid, uid);
	#else
	status = setreuid(uid, uid);
	#endif
	if (status == -1)
		errx(1, "unable to set uid to %d", uid);

	parseconfig(confpath, 0);
	if (!argc)
		exit(0);

	if (permit(uid, groups, ngroups, &rule, target, argv[0],
	    (const char **)argv + 1)) {
		printf("permit%s\n", (rule->options & NOPASS) ? " nopass" : "");
		exit(0);
	} else {
		printf("deny\n");
		exit(1);
	}
}

#if defined(USE_BSD_AUTH)      
static void
authuser(char *myname, char *login_style, int persist)
{
	char *challenge = NULL, *response, rbuf[1024], cbuf[128];
	auth_session_t *as;
	int fd = -1;

	if (persist)
		fd = open("/dev/tty", O_RDWR);
	if (fd != -1) {
		if (ioctl(fd, TIOCCHKVERAUTH) == 0)
			goto good;
	}

	if (!(as = auth_userchallenge(myname, login_style, "auth-doas",
	    &challenge)))
		errx(1, "Authorization failed");
	if (!challenge) {
		char host[HOST_NAME_MAX + 1];
		if (gethostname(host, sizeof(host)))
			snprintf(host, sizeof(host), "?");
		snprintf(cbuf, sizeof(cbuf),
		    "\rdoas (%.32s@%.32s) password: ", myname, host);
		challenge = cbuf;
	}
	response = readpassphrase(challenge, rbuf, sizeof(rbuf),
	    RPP_REQUIRE_TTY);
	if (response == NULL && errno == ENOTTY) {
		syslog(LOG_AUTHPRIV | LOG_NOTICE,
		    "tty required for %s", myname);
		errx(1, "a tty is required");
	}
	if (!auth_userresponse(as, response, 0)) {
		syslog(LOG_AUTHPRIV | LOG_NOTICE,
		    "failed auth for %s", myname);
		errc(1, EPERM, NULL);
	}
	explicit_bzero(rbuf, sizeof(rbuf));
good:
	if (fd != -1) {
		int secs = 5 * 60;
		ioctl(fd, TIOCSETVERAUTH, &secs);
		close(fd);
	}
}
#endif

int
main(int argc, char **argv)
{
	const char *safepath = SAFE_PATH;
	const char *confpath = NULL;
	char *shargv[] = { NULL, NULL };
	char *sh;
	const char *cmd;
	char cmdline[LINE_MAX];
	char myname[_PW_NAME_LEN + 1], targetname[_PW_NAME_LEN + 1];
	struct passwd *original_pw, *target_pw, *temp_pw; 
	struct rule *rule;
	uid_t uid;
	uid_t target = 0;
	gid_t groups[NGROUPS_MAX + 1];
	int ngroups;
	int i, ch;
	int Sflag = 0;
	int sflag = 0;
	int nflag = 0;
	char cwdpath[PATH_MAX];
	const char *cwd;
        #if defined(USE_BSD_AUTH)
	char *login_style = NULL;
        #endif
	char **envp;

	setprogname("doas");

	closefrom(STDERR_FILENO + 1);

	uid = getuid();
	targetname[0] = '\0';

	while ((ch = getopt(argc, argv, "+a:C:nSsu:")) != -1) {
		switch (ch) {
                #if defined(USE_BSD_AUTH)
		case 'a':
			login_style = optarg;
			break;
                #endif
		case 'C':
			confpath = optarg;
			break;
/*		case 'L':
			i = open("/dev/tty", O_RDWR);
			if (i != -1)
				ioctl(i, TIOCCLRVERAUTH);
			exit(i != -1);
*/
		case 'u':
			if (strlcpy(targetname, optarg, sizeof(targetname)) >= sizeof(targetname))
				errx(1, "pw_name too long");
			if (parseuid(targetname, &target) != 0)
				errx(1, "unknown user");
			break;
		case 'n':
			nflag = 1;
			break;
		case 'S':
			Sflag = 1;	
		case 's':
			sflag = 1;
			break;
		default:
			usage();
			break;
		}
	}
	argv += optind;
	argc -= optind;

	if (confpath) {
		if (sflag)
			usage();
	} else if ((!sflag && !argc) || (sflag && argc))
		usage();

	temp_pw = getpwuid(uid);
        original_pw = copyenvpw(temp_pw);
	if (! original_pw)
		err(1, "getpwuid failed");
	if (strlcpy(myname, original_pw->pw_name, sizeof(myname)) >= sizeof(myname))
		errx(1, "pw_name too long");

	ngroups = getgroups(NGROUPS_MAX, groups);
	if (ngroups == -1)
		err(1, "can't get groups");
	groups[ngroups++] = getgid();

	if (sflag) {
		sh = getenv("SHELL");
		if (sh == NULL || *sh == '\0') {
			shargv[0] = strdup(original_pw->pw_shell);
			if (shargv[0] == NULL)
				err(1, NULL);
		} else
			shargv[0] = sh;
		argv = shargv;
		argc = 1;
	}

	if (confpath) {
		checkconfig(confpath, argc, argv, uid, groups, ngroups,
		    target);
		exit(1);	/* fail safe */
	}

	if (geteuid())
		errx(1, "not installed setuid");

	parseconfig(DOAS_CONF, 1);

	/* cmdline is used only for logging, no need to abort on truncate */
	(void)strlcpy(cmdline, argv[0], sizeof(cmdline));
	for (i = 1; i < argc; i++) {
		if (strlcat(cmdline, " ", sizeof(cmdline)) >= sizeof(cmdline))
			break;
		if (strlcat(cmdline, argv[i], sizeof(cmdline)) >= sizeof(cmdline))
			break;
	}

	cmd = argv[0];
	if (!permit(uid, groups, ngroups, &rule, target, cmd,
	    (const char **)argv + 1)) {
		syslog(LOG_AUTHPRIV | LOG_NOTICE,
		    "failed command for %s: %s", myname, cmdline);
		errc(1, EPERM, NULL);
	}

	if (Sflag) {
		argv[0] = "-doas";
	}

	if (!(rule->options & NOPASS)) {
		if (nflag)
			errx(1, "Authorization required");

#if defined(USE_BSD_AUTH) 
		authuser(myname, login_style, rule->options & PERSIST);
#elif defined(USE_PAM)
#define PAM_END(msg) do { 						\
	syslog(LOG_ERR, "%s: %s", msg, pam_strerror(pamh, pam_err)); 	\
	warnx("%s: %s", msg, pam_strerror(pamh, pam_err));		\
	pam_end(pamh, pam_err);						\
	exit(EXIT_FAILURE);						\
} while (/*CONSTCOND*/0)
		pam_handle_t *pamh = NULL;
		int pam_err;

/* #ifndef __linux__ */
		int temp_stdin;

		/* openpam_ttyconv checks if stdin is a terminal and
		 * if it is then does not bother to open /dev/tty.
		 * The result is that PAM writes the password prompt
		 * directly to stdout.  In scenarios where stdin is a
		 * terminal, but stdout is redirected to a file
		 * e.g. by running doas ls &> ls.out interactively,
		 * the password prompt gets written to ls.out as well.
		 * By closing stdin first we forces PAM to read/write
		 * to/from the terminal directly.  We restore stdin
		 * after authenticating. */
		temp_stdin = dup(STDIN_FILENO);
		if (temp_stdin == -1)
			err(1, "dup");
		close(STDIN_FILENO);
/* #else */
		/* force password prompt to display on stderr, not stdout */
		int temp_stdout = dup(1);
		if (temp_stdout == -1)
			err(1, "dup");
		close(1);
		if (dup2(2, 1) == -1)
			err(1, "dup2");
/* #endif */

		pam_err = pam_start("doas", myname, &pamc, &pamh);
		if (pam_err != PAM_SUCCESS) {
			if (pamh != NULL)
				PAM_END("pam_start");
			syslog(LOG_ERR, "pam_start failed: %s",
			    pam_strerror(pamh, pam_err));
			errx(EXIT_FAILURE, "pam_start failed");
		}

		switch (pam_err = pam_authenticate(pamh, PAM_SILENT)) {
		case PAM_SUCCESS:
			switch (pam_err = pam_acct_mgmt(pamh, PAM_SILENT)) {
			case PAM_SUCCESS:
				break;

			case PAM_NEW_AUTHTOK_REQD:
				pam_err = pam_chauthtok(pamh,
				    PAM_SILENT|PAM_CHANGE_EXPIRED_AUTHTOK);
				if (pam_err != PAM_SUCCESS)
					PAM_END("pam_chauthtok");
				break;

			case PAM_AUTH_ERR:
			case PAM_USER_UNKNOWN:
			case PAM_MAXTRIES:
				syslog(LOG_AUTHPRIV | LOG_NOTICE,
				    "failed auth for %s", myname);
                                errx(EXIT_FAILURE, "second authentication failed");
				break;

			default:
				PAM_END("pam_acct_mgmt");
				break;
			}
			break;

		case PAM_AUTH_ERR:
		case PAM_USER_UNKNOWN:
		case PAM_MAXTRIES:
			syslog(LOG_AUTHPRIV | LOG_NOTICE,
			    "failed auth for %s", myname);
                        errx(EXIT_FAILURE, "authentication failed");
			break;

		default:
			PAM_END("pam_authenticate");
			break;
		}
		pam_end(pamh, pam_err);

#ifndef __linux__
		/* Re-establish stdin */
		if (dup2(temp_stdin, STDIN_FILENO) == -1)
			err(1, "dup2");
		close(temp_stdin);
#else 
		/* Re-establish stdout */
		close(1);
		if (dup2(temp_stdout, 1) == -1)
			err(1, "dup2");
#endif
#else
#error	No auth module!
#endif
	}

        /*
	if (pledge("stdio rpath getpw exec id", NULL) == -1)
		err(1, "pledge");
        */
	if (targetname[0] == '\0')
		temp_pw = getpwuid(target);
	else
		temp_pw = getpwnam(targetname);
	if (temp_pw->pw_shell[0] == '\0')
		temp_pw->pw_shell = strdup(_PATH_BSHELL);
        target_pw = copyenvpw(temp_pw);
	if (! target_pw)
		errx(1, "no passwd entry for target");

        
#if defined(HAVE_LOGIN_CAP_H)
        if (setusercontext(NULL, target_pw, target, 
            LOGIN_SETGROUP | LOGIN_SETLOGINCLASS |
            LOGIN_SETPRIORITY | LOGIN_SETRESOURCES | LOGIN_SETUMASK |
            LOGIN_SETUSER) != 0)
            errx(1, "failed to set user context for target");
#else
	#if defined(__linux__) || defined(__FreeBSD__) || defined(__MidnightBSD__)
	if (setresgid(target_pw->pw_gid, target_pw->pw_gid, target_pw->pw_gid) == -1)
		err(1, "setresgid");
	#else
	if (setregid(target_pw->pw_gid, target_pw->pw_gid) == -1)
		err(1, "setregid");
	#endif
	if (initgroups(target_pw->pw_name, target_pw->pw_gid) == -1)
		err(1, "initgroups");
	#if defined(__linux__) || defined(__FreeBSD__) || defined(__MidnightBSD__)
	if (setresuid(target, target, target) == -1)
		err(1, "setresuid");
	#else
	if (setreuid(target, target) == -1)
		err(1, "setreuid");
	#endif
#endif
        /*
	if (pledge("stdio rpath exec", NULL) == -1)
		err(1, "pledge");
        */

	if (getcwd(cwdpath, sizeof(cwdpath)) == NULL)
		cwd = "(failed)";
	else
		cwd = cwdpath;

	/*
        if (pledge("stdio exec", NULL) == -1)
		err(1, "pledge");
        */

        /* skip logging if NOLOG is set */
        if (!(rule->options & NOLOG))
        {
	    syslog(LOG_AUTHPRIV | LOG_INFO, "%s ran command %s as %s from %s",
	            myname, cmdline, target_pw->pw_name, cwd);
        }

	envp = prepenv(rule, original_pw, target_pw);

	if (rule->cmd) {
		if (setenv("PATH", safepath, 1) == -1)
			err(1, "failed to set PATH '%s'", safepath);
	}
	execvpe(cmd, argv, envp);
	if (errno == ENOENT)
		errx(1, "%s: command not found", cmd);
	err(1, "%s", cmd);
}

