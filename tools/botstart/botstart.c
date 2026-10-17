// botstart.nro: says whether sys-botbase (program 430000000000000B) runs and
// why it might not have started at boot; installs the exefs.nsp in its RomFS
// (build.sh NSP) to start at the next boot; and with the argument "launch"
// starts it now. Sent with nxlink -s (tools/botstart/build.sh), its report
// comes back to the PC; it then returns to the homebrew menu.
//
// A launch that the kernel refuses can leave the console unstable (a
// sys-botbase built for newer firmware did, on 16.0.2): install, then reboot.
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#define BOTBASE 0x430000000000000BULL

static void list(const char* dir, const char* match)
{
	DIR* d = opendir(dir);
	if (!d) {
		printf("  %s: cannot open\n", dir);
		return;
	}
	int n = 0;
	for (struct dirent* e; (e = readdir(d));) {
		if (match && !strstr(e->d_name, match))
			continue;
		char path[512];
		struct stat st;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		printf("  %s/%s %lld\n", dir, e->d_name, stat(path, &st) == 0 ? (long long)st.st_size : -1LL);
		n++;
	}
	closedir(d);
	if (!n)
		printf("  %s: nothing%s%s\n", dir, match ? " matching " : "", match ? match : "");
}

static bool running(u64* pid)
{
	return R_SUCCEEDED(pmdmntGetProcessId(pid, BOTBASE));
}

// romfs:/exefs.nsp onto the SD card, with the flag that starts it at boot
static void install(void)
{
	FILE* in = fopen("romfs:/exefs.nsp", "rb");
	if (!in)
		return;
	mkdir("sdmc:/atmosphere/contents/430000000000000B", 0777);
	mkdir("sdmc:/atmosphere/contents/430000000000000B/flags", 0777);
	const char* dst = "sdmc:/atmosphere/contents/430000000000000B/exefs.nsp";
	FILE* out = fopen("sdmc:/atmosphere/contents/430000000000000B/exefs.nsp.part", "wb");
	static char buf[1 << 16];
	size_t n, total = 0;
	bool ok = out != NULL;
	while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) {
		ok = fwrite(buf, 1, n, out) == n;
		total += n;
	}
	fclose(in);
	ok = out && fclose(out) == 0 && ok;
	if (ok) {
		remove(dst);
		ok = rename("sdmc:/atmosphere/contents/430000000000000B/exefs.nsp.part", dst) == 0;
	}
	FILE* flag = fopen("sdmc:/atmosphere/contents/430000000000000B/flags/boot2.flag", "ab");
	if (flag)
		fclose(flag);
	printf("installed exefs.nsp (%zu bytes): %s\n", total, ok ? "ok" : "FAILED");
}

int main(int argc, char** argv)
{
	const bool launch = argc > 1 && !strcmp(argv[argc - 1], "launch");
	socketInitializeDefault();
	const int link = nxlinkStdio();
	printf("botstart\n");

	if (R_SUCCEEDED(splInitialize())) {
		u64 v = 0;
		if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &v)))
			printf("Atmosphere %u.%u.%u\n", (unsigned)(v >> 56) & 0xff, (unsigned)(v >> 48) & 0xff,
			       (unsigned)(v >> 40) & 0xff);
		splExit();
	}
	if (R_SUCCEEDED(setsysInitialize())) {
		SetSysFirmwareVersion fw;
		if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
			printf("firmware %s\n", fw.display_version);
		setsysExit();
	}
	if (R_SUCCEEDED(romfsInit())) {
		install();
		romfsExit();
	}
	list("sdmc:/atmosphere/contents/430000000000000B", NULL);
	list("sdmc:/atmosphere/contents/430000000000000B/flags", NULL);
	list("sdmc:/atmosphere/crash_reports", "430000000000000b");
	list("sdmc:/atmosphere/crash_reports", "430000000000000B");

	u64 pid = 0;
	Result rc = pmdmntInitialize();
	if (R_FAILED(rc)) {
		printf("pm:dmnt: 0x%x\n", rc);
	} else if (running(&pid)) {
		printf("sys-botbase is running (process %llu)\n", (unsigned long long)pid);
	} else if (!launch) {
		printf("sys-botbase is not running (reboot to start it; \"launch\" starts it now)\n");
	} else {
		printf("sys-botbase is not running; starting it\n");
		rc = pmshellInitialize();
		if (R_FAILED(rc)) {
			printf("pm:shell: 0x%x\n", rc);
		} else {
			const NcmProgramLocation loc = {.program_id = BOTBASE, .storageID = NcmStorageId_None};
			rc = pmshellLaunchProgram(0, &loc, &pid);
			printf("launch: 0x%x (module %u, description %u)\n", rc, R_MODULE(rc), R_DESCRIPTION(rc));
			pmshellExit();
			svcSleepThread(2000000000ULL);
			if (running(&pid))
				printf("sys-botbase is running now (process %llu)\n", (unsigned long long)pid);
			else
				printf("sys-botbase is still not running\n");
		}
	}
	pmdmntExit();
	printf("botstart: done\n");
	fflush(stdout);
	if (link >= 0)
		close(link);
	socketExit();
	return 0;
}
