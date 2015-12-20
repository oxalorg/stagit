#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "git2.h"

struct commitinfo {
	const git_oid *id;

	char oid[GIT_OID_HEXSZ + 1];
	char parentoid[GIT_OID_HEXSZ + 1];

	const git_signature *author;
	const char *summary;
	const char *msg;

	git_diff_stats *stats;
	git_diff       *diff;
	git_commit     *commit;
	git_commit     *parent;
	git_tree       *commit_tree;
	git_tree       *parent_tree;

	size_t addcount;
	size_t delcount;
	size_t filecount;
};

static git_repository *repo;

static const char *relpath = "";
static const char *repodir;

static char description[255] = "Repositories";
static char name[255];
static char owner[255];

void
commitinfo_free(struct commitinfo *ci)
{
	if (!ci)
		return;

	git_diff_stats_free(ci->stats);
	git_diff_free(ci->diff);
	git_tree_free(ci->commit_tree);
	git_tree_free(ci->parent_tree);
	git_commit_free(ci->commit);
}

struct commitinfo *
commitinfo_getbyoid(const git_oid *id)
{
	struct commitinfo *ci;
	git_diff_options opts;
	int error;

	if (!(ci = calloc(1, sizeof(struct commitinfo))))
		err(1, "calloc");

	ci->id = id;
	if (git_commit_lookup(&(ci->commit), repo, id))
		goto err;

	/* TODO: show tags when commit has it */
	git_oid_tostr(ci->oid, sizeof(ci->oid), git_commit_id(ci->commit));
	git_oid_tostr(ci->parentoid, sizeof(ci->parentoid), git_commit_parent_id(ci->commit, 0));

	ci->author = git_commit_author(ci->commit);
	ci->summary = git_commit_summary(ci->commit);
	ci->msg = git_commit_message(ci->commit);

	if ((error = git_commit_tree(&(ci->commit_tree), ci->commit)))
		goto err; /* TODO: handle error */
	if (!(error = git_commit_parent(&(ci->parent), ci->commit, 0))) {
		if ((error = git_commit_tree(&(ci->parent_tree), ci->parent)))
			goto err;
	} else {
		ci->parent = NULL;
		ci->parent_tree = NULL;
	}

	git_diff_init_options(&opts, GIT_DIFF_OPTIONS_VERSION);
	opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
	if ((error = git_diff_tree_to_tree(&(ci->diff), repo, ci->parent_tree, ci->commit_tree, &opts)))
		goto err;
	if (git_diff_get_stats(&(ci->stats), ci->diff))
		goto err;

	ci->addcount = git_diff_stats_insertions(ci->stats);
	ci->delcount = git_diff_stats_deletions(ci->stats);
	ci->filecount = git_diff_stats_files_changed(ci->stats);

	/* TODO: show tag when commit has it */

	return ci;

err:
	commitinfo_free(ci);
	free(ci);

	return NULL;
}

FILE *
efopen(const char *name, const char *flags)
{
	FILE *fp;

	if (!(fp = fopen(name, flags)))
		err(1, "fopen");

	return fp;
}

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
	fputs("<!DOCTYPE HTML>"
		"<html dir=\"ltr\" lang=\"en\">\n<head>\n"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
		"<meta http-equiv=\"Content-Language\" content=\"en\" />\n<title>", fp);
	xmlencode(fp, description, strlen(description));
	fprintf(fp, "</title>\n<link rel=\"icon\" type=\"image/png\" href=\"%sfavicon.png\" />\n", relpath);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"%sstyle.css\" />\n", relpath);
	fputs("</head>\n<body>\n\n", fp);
	fprintf(fp, "<table>\n<tr><td><img src=\"%slogo.png\" alt=\"\" width=\"32\" height=\"32\" /></td>\n"
	        "<td><h1>%s</h1><span class=\"desc\">%s</span></td></tr><tr><td></td><td>\n",
		relpath, name, description);
	fputs("</td></tr>\n</table>\n<hr/><div id=\"content\">\n"
	      "<table><thead>\n"
	      "<tr><td>Name</td><td>Description</td><td>Owner</td><td>Last commit</td></tr>"
	      "</thead><tbody>\n", fp);

	return 0;
}

int
writefooter(FILE *fp)
{
	return !fputs("</tbody></table></div></body>\n</html>", fp);
}

int
writelog(FILE *fp)
{
	struct commitinfo *ci;
	git_revwalk *w = NULL;
	git_oid id;
	int ret = 0;

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);
	git_revwalk_sorting(w, GIT_SORT_TIME);
	git_revwalk_simplify_first_parent(w);

	if (git_revwalk_next(&id, w) ||
	    !(ci = commitinfo_getbyoid(&id))) {
		ret = -1;
		goto err;
	}

	fputs("<tr><td><a href=\"", fp);
	xmlencode(fp, name, strlen(name));
	fputs("/log.html\">", fp);
	xmlencode(fp, name, strlen(name));
	fputs("</a></td><td>", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</td><td>", fp);
	xmlencode(fp, owner, strlen(owner));
	fputs("</td><td>", fp);
	if (ci->author)
		printtimeshort(fp, &(ci->author->when));
	fputs("</td></tr>", fp);

err:
	git_revwalk_free(w);

	return ret;
}

int
main(int argc, char *argv[])
{
	const git_error *e = NULL;
	FILE *fp;
	char path[PATH_MAX], *p;
	int status;
	size_t i;

	if (argc < 2) {
		fprintf(stderr, "%s [repodir...]\n", argv[0]);
		return 1;
	}
	git_libgit2_init();

	writeheader(stdout);

	for (i = 1; i < argc; i++) {
		repodir = argv[i];

		if ((status = git_repository_open_ext(&repo, repodir,
			GIT_REPOSITORY_OPEN_NO_SEARCH, NULL)) < 0) {
			e = giterr_last();
			fprintf(stderr, "error %d/%d: %s\n", status, e->klass, e->message);
			return status;
		}

		/* use directory name as name */
		p = xbasename(repodir);
		snprintf(name, sizeof(name), "%s", p);
		free(p);

		/* read description or .git/description */
		description[0] = '\0';
		snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "description");
		if (!(fp = fopen(path, "r"))) {
			snprintf(path, sizeof(path), "%s%s%s",
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
		snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "owner");
		if (!(fp = fopen(path, "r"))) {
			snprintf(path, sizeof(path), "%s%s%s",
				repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/owner");
			fp = fopen(path, "r");
		}
		if (fp) {
			if (!fgets(owner, sizeof(owner), fp))
				owner[0] = '\0';
			fclose(fp);
		}
		writelog(stdout);
	}
	writefooter(stdout);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return 0;
}
