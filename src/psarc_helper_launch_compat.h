#ifndef EOF_PSARC_HELPER_LAUNCH_COMPAT_H
#define EOF_PSARC_HELPER_LAUNCH_COMPAT_H

/*
 * psarc_export.c historically launches the helper with a shell command whose
 * executable, spec and log paths are all quoted.  On Windows, system() routes
 * that string through cmd.exe and cmd's first-quoted-token parsing can reject
 * the command before the redirection is established.  The result is exactly
 * the bad failure mode we want to avoid: EOF reports that psarc_export.log has
 * details, but the log file was never created.
 *
 * This second forced-include layer intercepts only the helper invocation.  It
 * creates the log before launching anything, then uses the canonical nested
 * cmd.exe quoting form and appends both stdout and stderr.  Even if the helper
 * cannot start at all, the log remains present and records the launcher return
 * code plus the exact helper/spec paths.
 */

#ifdef system
#undef system
#endif

static int eof_psarc_helper_launch_system_compat(const char *command)
{
	char helper[1024] = {0};
	char spec[1024] = {0};
	char logpath[1024] = {0};
	char wrapped[4096] = {0};
	FILE *fp;
	int result;

	if(!command)
		return -1;

	/* Leave the internal WAV compatibility layer and every unrelated command
	 * on the existing path. */
	if(!strstr(command, "eof_psarc_helper.exe"))
		return eof_psarc_system_compat(command);

	if(!eof_psarc_get_quoted_arg_compat(command, 0, helper, sizeof(helper)) ||
	   !eof_psarc_get_quoted_arg_compat(command, 1, spec, sizeof(spec)) ||
	   !eof_psarc_get_quoted_arg_compat(command, 2, logpath, sizeof(logpath)))
	{
		eof_log("PSARC helper launcher: could not parse helper/spec/log paths.", 1);
		return eof_psarc_system_compat(command);
	}

	/* Guarantee that the path EOF advertises to the user exists even if cmd or
	 * the .NET helper itself cannot start. */
	fp = fopen(logpath, "wb");
	if(fp)
	{
		fprintf(fp, "EOF PSARC helper launcher\r\n");
		fprintf(fp, "helper: %s\r\n", helper);
		fprintf(fp, "spec: %s\r\n", spec);
		fprintf(fp, "----------------------------------------\r\n");
		fclose(fp);
	}
	else
	{
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"PSARC helper launcher: could not create log file %.850s", logpath);
		eof_log(eof_log_string, 1);
	}

	/* /S /C requires the extra outer quote when the executable path itself is
	 * quoted.  Append instead of truncate so the launcher header above survives. */
	(void)snprintf(wrapped, sizeof(wrapped) - 1,
		"cmd.exe /D /S /C \"\"%s\" \"%s\" >> \"%s\" 2>&1\"",
		helper, spec, logpath);

	result = system(wrapped); /* real CRT system(): macro is undefined above */

	fp = fopen(logpath, "ab");
	if(fp)
	{
		fprintf(fp, "\r\n----------------------------------------\r\n");
		fprintf(fp, "launcher exit code: %d\r\n", result);
		fclose(fp);
	}

	if(result != 0)
	{
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			"PSARC helper failed with launcher exit code %d. Log: %.800s", result, logpath);
		eof_log(eof_log_string, 1);
	}
	return result;
}

#define system(command) eof_psarc_helper_launch_system_compat((command))

/* psarc_export.c's legacy post-build check only knows the PC suffix `_p.psarc`.
 * When the user intentionally selects Mac only, the official RSToolkit packer
 * correctly emits `_m.psarc`, so teach this translation unit's exists() shim to
 * recognize that Mac result without creating an unwanted unsuffixed/PC file. */
#ifdef exists
#undef exists
#endif

static int eof_psarc_platform_output_exists_compat(const char *path)
{
	const char *pc;
	const char *mac;
	size_t len;
	char macpath[1024] = {0};

	if(eof_psarc_exists_compat(path))
		return 1;
	if(!path)
		return 0;

	pc = getenv("EOF_PSARC_PC");
	mac = getenv("EOF_PSARC_MAC");
	if(!pc || strcmp(pc, "0") || !mac || strcmp(mac, "1"))
		return 0;

	len = strlen(path);
	if(len < 8U || ustricmp(path + len - 8U, "_p.psarc"))
		return 0;

	ustrzcpy(macpath, sizeof(macpath), path);
	macpath[len - 7U] = 'm';
	return eof_psarc_exists_compat(macpath);
}

#define exists(path) eof_psarc_platform_output_exists_compat((path))

#endif
