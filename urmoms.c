#include <err.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "git2.h"

static git_repository *repo;

static const char *relpath;
static const char *repodir = ".";

static char name[255];
static char description[255];
static int hasreadme, haslicense;

FILE *
efopen(const char *name, const char *flags)
{
	FILE *fp;

	fp = fopen(name, flags);
	if (!fp)
		err(1, "fopen");

	return fp;
}

void
concat(FILE *fp1, FILE *fp2)
{
	char buf[BUFSIZ];
	size_t n;

	while ((n = fread(buf, 1, sizeof(buf), fp1))) {
		fwrite(buf, 1, n, fp2);

		if (feof(fp1) || ferror(fp1) || ferror(fp2))
			break;
	}
}

/* Some implementations of basename(3) return a pointer to a static
 * internal buffer (OpenBSD). Others modify the contents of `path` (POSIX).
 * This is a wrapper function that is compatible with both versions.
 * The program will error out if basename(3) failed, this can only happen
 * with the OpenBSD version. */
char *
xbasename(const char *path)
{
	char *p, *b;

	if (!(p = strdup(path)))
		err(1, "strdup");
	if (!(b = basename(p)))
		err(1, "basename");
	if (!(b = strdup(b)))
		err(1, "strdup");
	free(p);

	return b;
}

static void
printtime(FILE *fp, const git_time * intime, const char *prefix)
{
	struct tm *intm;
	time_t t;
	int offset, hours, minutes;
	char sign, out[32];

	offset = intime->offset;
	if (offset < 0) {
		sign = '-';
		offset = -offset;
	} else {
		sign = '+';
	}

	hours = offset / 60;
	minutes = offset % 60;

	t = (time_t) intime->time + (intime->offset * 60);

	intm = gmtime(&t);
	strftime(out, sizeof(out), "%a %b %e %T %Y", intm);

	fprintf(fp, "%s%s %c%02d%02d\n", prefix, out, sign, hours, minutes);
}

static void
printcommit(FILE *fp, git_commit * commit)
{
	const git_signature *sig;
	char buf[GIT_OID_HEXSZ + 1];
	int i, count;
	const char *scan, *eol;

	git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));
	fprintf(fp, "commit <a href=\"commit/%s.html\">%s</a>\n", buf, buf);

	if ((count = (int)git_commit_parentcount(commit)) > 1) {
		fprintf(fp, "Merge:");
		for (i = 0; i < count; ++i) {
			git_oid_tostr(buf, 8, git_commit_parent_id(commit, i));
			fprintf(fp, " %s", buf);
		}
		fprintf(fp, "\n");
	}
	if ((sig = git_commit_author(commit)) != NULL) {
		fprintf(fp, "Author: <a href=\"author/%s.html\">%s</a> <%s>\n",
			sig->name, sig->name, sig->email);
		printtime(fp, &sig->when, "Date:   ");
	}
	fprintf(fp, "\n");

	for (scan = git_commit_message(commit); scan && *scan;) {
		for (eol = scan; *eol && *eol != '\n'; ++eol)	/* find eol */
			;

		fprintf(fp, "    %.*s\n", (int) (eol - scan), scan);
		scan = *eol ? eol + 1 : NULL;
	}
	fprintf(fp, "\n");
}

int
writeheader(FILE *fp)
{
	fprintf(fp, "<!DOCTYPE HTML>"
		"<html dir=\"ltr\" lang=\"en\"><head>"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />"
		"<meta http-equiv=\"Content-Language\" content=\"en\" />");
	fprintf(fp, "<title>%s%s%s</title>", name, description[0] ? " - " : "", description);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" />"
		"</head><body><center>");
	fprintf(fp, "<h1><img src=\"%slogo.png\" alt=\"\" /> %s</h1>", relpath, name);
	fprintf(fp, "<span class=\"desc\">%s</span><br/>", description);
	fprintf(fp, "<a href=\"%slog.html\">Log</a> |", relpath);
	fprintf(fp, "<a href=\"%sfiles.html\">Files</a>| ", relpath);
	fprintf(fp, "<a href=\"%sstats.html\">Stats</a> | ", relpath);
	if (hasreadme)
		fprintf(fp, "<a href=\"%sreadme.html\">README</a> | ", relpath);
	if (haslicense)
		fprintf(fp, "<a href=\"%slicense.html\">LICENSE</a>", relpath);
	fprintf(fp, "</center><hr/><pre>");

	return 0;
}

int
writefooter(FILE *fp)
{
	fprintf(fp, "</pre></body></html>");

	return 0;
}

int
writelog(FILE *fp)
{
	git_revwalk *w = NULL;
	git_oid id;
	git_commit *c = NULL;

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);

	while (!git_revwalk_next(&id, w)) {
		if (git_commit_lookup(&c, repo, &id))
			return 1;
		printcommit(fp, c);
		git_commit_free(c);
	}
	git_revwalk_free(w);

	return 0;
}

int
writefiles(FILE *fp)
{
	git_index *index;
	const git_index_entry *entry;
	size_t count, i;

	git_repository_index(&index, repo);

	count = git_index_entrycount(index);
	for (i = 0; i < count; i++) {
		entry = git_index_get_byindex(index, i);
		fprintf(fp, "name: %s, size: %lu, mode: %lu\n",
			entry->path, entry->file_size, entry->mode);
	}

	return 0;
}

#if 0
int
writebranches(FILE *fp)
{
	git_branch_iterator *branchit = NULL;
	git_branch_t branchtype;
	git_reference *branchref;
	char branchbuf[BUFSIZ] = "";
	int status;

	git_branch_iterator_new(&branchit, repo, GIT_BRANCH_LOCAL);

	while ((status = git_branch_next(&branchref, &branchtype, branchit)) == GIT_ITEROVER) {
		git_reference_normalize_name(branchbuf, sizeof(branchbuf), git_reference_name(branchref), GIT_REF_FORMAT_ALLOW_ONELEVEL | GIT_REF_FORMAT_REFSPEC_SHORTHAND);

		/* fprintf(fp, "branch: |%s|\n", branchbuf); */
	}

	git_branch_iterator_free(branchit);

	return 0;
}
#endif

int
main(int argc, char *argv[])
{
	const git_error *e = NULL;
	FILE *fp, *fpread;
	char path[PATH_MAX], *p;
	int status;

	if (argc != 2) {
		fprintf(stderr, "%s <repodir>\n", argv[0]);
		return 1;
	}
	repodir = argv[1];

	git_libgit2_init();

	if ((status = git_repository_open(&repo, repodir)) < 0) {
		e = giterr_last();
		fprintf(stderr, "error %d/%d: %s\n", status, e->klass, e->message);
		exit(status);
	}

	/* use directory name as name */
	p = xbasename(repodir);
	snprintf(name, sizeof(name), "%s", p);
	free(p);

	/* read description or .git/description */
	snprintf(path, sizeof(path), "%s%s%s",
		repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "description");
	if (!(fpread = fopen(path, "r+b"))) {
		snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/description");
		fpread = fopen(path, "r+b");
	}
	if (fpread) {
		if (!fgets(description, sizeof(description), fpread))
			description[0] = '\0';
		fclose(fpread);
	}

	/* read LICENSE */
	snprintf(path, sizeof(path), "%s%s%s",
		repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "LICENSE");
	if ((fpread = fopen(path, "r+b"))) {
		fp = efopen("license.html", "w+b");
		writeheader(fp);
		concat(fpread, fp);
		if (ferror(fpread) || ferror(fp))
			err(1, "concat");
		writefooter(fp);

		fclose(fp);
		fclose(fpread);

		haslicense = 1;
	}

	/* read README */
	snprintf(path, sizeof(path), "%s%s%s",
		repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "README");
	if ((fpread = fopen(path, "r+b"))) {
		fp = efopen("readme.html", "w+b");
		writeheader(fp);
		concat(fpread, fp);
		if (ferror(fpread) || ferror(fp))
			err(1, "concat");
		writefooter(fp);
		fclose(fp);
		fclose(fpread);

		hasreadme = 1;
	}

	fp = efopen("logs.html", "w+b");
	writeheader(fp);
	writelog(fp);
	writefooter(fp);
	fclose(fp);

	fp = efopen("files.html", "w+b");
	writeheader(fp);
	writefiles(fp);
	writefooter(fp);
	fclose(fp);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return 0;
}
