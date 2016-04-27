#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <git2.h>

#include "compat.h"
#include "config.h"

static git_repository *repo;

static const char *relpath = "";
static const char *repodir;

static char description[255] = "Repositories";
static char *name = "";
static char owner[255];

/* Escape characters below as HTML 2.0 / XML 1.0. */
void
xmlencode(FILE *fp, const char *s, size_t len)
{
	size_t i;

	for (i = 0; *s && i < len; s++, i++) {
		switch(*s) {
		case '<':  fputs("&lt;",   fp); break;
		case '>':  fputs("&gt;",   fp); break;
		case '\'': fputs("&apos;", fp); break;
		case '&':  fputs("&amp;",  fp); break;
		case '"':  fputs("&quot;", fp); break;
		default:   fputc(*s, fp);
		}
	}
}

void
printtimeformat(FILE *fp, const git_time *intime, const char *fmt)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t) intime->time + (intime->offset * 60);
	intm = gmtime(&t);
	strftime(out, sizeof(out), fmt, intm);
	fputs(out, fp);
}

void
printtimeshort(FILE *fp, const git_time *intime)
{
	printtimeformat(fp, intime, "%Y-%m-%d %H:%M");
}

int
writeheader(FILE *fp)
{
	fputs("<!DOCTYPE html>\n"
		"<html dir=\"ltr\" lang=\"en\">\n<head>\n"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
		"<meta http-equiv=\"Content-Language\" content=\"en\" />\n<title>", fp);
	xmlencode(fp, description, strlen(description));
	fprintf(fp, "</title>\n<link rel=\"icon\" type=\"image/png\" href=\"%sfavicon.png\" />\n", relpath);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"%sstyle.css\" />\n", relpath);
	fputs("</head>\n<body>\n", fp);
	fprintf(fp, "<table>\n<tr><td><img src=\"%slogo.png\" alt=\"\" width=\"32\" height=\"32\" /></td>\n"
	        "<td><h1>%s</h1><span class=\"desc\">%s</span></td></tr><tr><td></td><td>\n",
		relpath, name, description);
	fputs("</td></tr>\n</table>\n<hr/>\n<div id=\"content\">\n"
	      "<table id=\"index\"><thead>\n"
	      "<tr><td>Name</td><td>Description</td><td>Owner</td><td>Last commit</td></tr>"
	      "</thead><tbody>\n", fp);

	return 0;
}

int
writefooter(FILE *fp)
{
	return !fputs("</tbody>\n</table>\n</div>\n</body>\n</html>\n", fp);
}

int
writelog(FILE *fp)
{
	char *stripped_name = NULL, *p;
	git_commit *commit = NULL;
	const git_signature *author;
	git_revwalk *w = NULL;
	git_oid id;
	int ret = 0;

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);
	git_revwalk_sorting(w, GIT_SORT_TIME);
	git_revwalk_simplify_first_parent(w);

	if (git_revwalk_next(&id, w) ||
	    git_commit_lookup(&commit, repo, &id)) {
		ret = -1;
		goto err;
	}

	author = git_commit_author(commit);

	/* strip .git suffix */
	if (!(stripped_name = strdup(name)))
		err(1, "strdup");
	if ((p = strrchr(stripped_name, '.')))
		if (!strcmp(p, ".git"))
			*p = '\0';

	fputs("<tr><td><a href=\"", fp);
	xmlencode(fp, stripped_name, strlen(stripped_name));
	fputs("/log.html\">", fp);
	xmlencode(fp, stripped_name, strlen(stripped_name));
	fputs("</a></td><td>", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</td><td>", fp);
	xmlencode(fp, owner, strlen(owner));
	fputs("</td><td>", fp);
	if (author)
		printtimeshort(fp, &(author->when));
	fputs("</td></tr>", fp);

	git_commit_free(commit);
err:
	git_revwalk_free(w);
	free(stripped_name);

	return ret;
}

int
main(int argc, char *argv[])
{
	const git_error *e = NULL;
	FILE *fp;
	char path[PATH_MAX], repodirabs[PATH_MAX + 1];
	int i, r, ret = 0;

	if (argc < 2) {
		fprintf(stderr, "%s [repodir...]\n", argv[0]);
		return 1;
	}
	git_libgit2_init();

	writeheader(stdout);

	for (i = 1; i < argc; i++) {
		repodir = argv[i];
		if (!realpath(repodir, repodirabs))
			err(1, "realpath");

		if (git_repository_open_ext(&repo, repodir,
		    GIT_REPOSITORY_OPEN_NO_SEARCH, NULL)) {
			e = giterr_last();
			fprintf(stderr, "%s: %s\n", argv[0], e->message);
			ret = 1;
			continue;
		}

		/* use directory name as name */
		if ((name = strrchr(repodirabs, '/')))
			name++;
		else
			name = "";

		/* read description or .git/description */
		description[0] = '\0';
		r = snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "description");
		if (r == -1 || (size_t)r >= sizeof(path))
			errx(1, "path truncated: '%s%s%s'",
			        repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "description");
		if (!(fp = fopen(path, "r"))) {
			r = snprintf(path, sizeof(path), "%s%s%s",
				repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/description");
			if (r == -1 || (size_t)r >= sizeof(path))
				errx(1, "path truncated: '%s%s%s'",
				        repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/description");
			fp = fopen(path, "r");
		}
		if (fp) {
			if (!fgets(description, sizeof(description), fp))
				description[0] = '\0';
			fclose(fp);
		}

		/* read owner or .git/owner */
		owner[0] = '\0';
		r = snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "owner");
		if (r == -1 || (size_t)r >= sizeof(path))
			errx(1, "path truncated: '%s%s%s'",
			        repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "owner");
		if (!(fp = fopen(path, "r"))) {
			r = snprintf(path, sizeof(path), "%s%s%s",
				repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/owner");
			if (r == -1 || (size_t)r >= sizeof(path))
				errx(1, "path truncated: '%s%s%s'",
				        repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/owner");
			fp = fopen(path, "r");
		}
		if (fp) {
			if (!fgets(owner, sizeof(owner), fp))
				owner[0] = '\0';
			owner[strcspn(owner, "\n")] = '\0';
			fclose(fp);
		}
		writelog(stdout);
	}
	writefooter(stdout);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return ret;
}
