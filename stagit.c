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

#include <git2.h>

#include "compat.h"

struct deltainfo {
	git_patch *patch;

	size_t addcount;
	size_t delcount;
};

struct commitinfo {
	const git_oid *id;

	char oid[GIT_OID_HEXSZ + 1];
	char parentoid[GIT_OID_HEXSZ + 1];

	const git_signature *author;
	const git_signature *committer;
	const char          *summary;
	const char          *msg;

	git_diff   *diff;
	git_commit *commit;
	git_commit *parent;
	git_tree   *commit_tree;
	git_tree   *parent_tree;

	size_t addcount;
	size_t delcount;
	size_t filecount;

	struct deltainfo **deltas;
	size_t ndeltas;
};

/* summary length (bytes) in the log */
static const unsigned summarylen = 70;
/* display line count or file size in file tree index */
static const int showlinecount = 1;

static git_repository *repo;

static const char *relpath = "";
static const char *repodir;

static char *name = "";
static char *strippedname = "";
static char description[255];
static char cloneurl[1024];
static int haslicense, hasreadme, hassubmodules;

/* cache */
static git_oid lastoid;
static char lastoidstr[GIT_OID_HEXSZ + 2]; /* id + newline + nul byte */
static FILE *rcachefp, *wcachefp;
static const char *cachefile;

#ifndef USE_PLEDGE
int
pledge(const char *promises, const char *paths[])
{
	return 0;
}
#endif

void
joinpath(char *buf, size_t bufsiz, const char *path, const char *path2)
{
	int r;

	r = snprintf(buf, bufsiz, "%s%s%s",
		path, path[0] && path[strlen(path) - 1] != '/' ? "/" : "", path2);
	if (r == -1 || (size_t)r >= bufsiz)
		errx(1, "path truncated: '%s%s%s'",
			path, path[0] && path[strlen(path) - 1] != '/' ? "/" : "", path2);
}

void
deltainfo_free(struct deltainfo *di)
{
	if (!di)
		return;
	git_patch_free(di->patch);
	di->patch = NULL;
	free(di);
}

int
commitinfo_getstats(struct commitinfo *ci)
{
	struct deltainfo *di;
	const git_diff_delta *delta;
	const git_diff_hunk *hunk;
	const git_diff_line *line;
	git_patch *patch = NULL;
	size_t ndeltas, nhunks, nhunklines;
	size_t i, j, k;

	ndeltas = git_diff_num_deltas(ci->diff);
	if (ndeltas && !(ci->deltas = calloc(ndeltas, sizeof(struct deltainfo *))))
		err(1, "calloc");

	for (i = 0; i < ndeltas; i++) {
		if (!(di = calloc(1, sizeof(struct deltainfo))))
			err(1, "calloc");
		if (git_patch_from_diff(&patch, ci->diff, i)) {
			git_patch_free(patch);
			goto err;
		}
		di->patch = patch;
		ci->deltas[i] = di;

		delta = git_patch_get_delta(patch);

		/* skip stats for binary data */
		if (delta->flags & GIT_DIFF_FLAG_BINARY)
			continue;

		nhunks = git_patch_num_hunks(patch);
		for (j = 0; j < nhunks; j++) {
			if (git_patch_get_hunk(&hunk, &nhunklines, patch, j))
				break;
			for (k = 0; ; k++) {
				if (git_patch_get_line_in_hunk(&line, patch, j, k))
					break;
				if (line->old_lineno == -1) {
					di->addcount++;
					ci->addcount++;
				} else if (line->new_lineno == -1) {
					di->delcount++;
					ci->delcount++;
				}
			}
		}
	}
	ci->ndeltas = i;
	ci->filecount = i;

	return 0;

err:
	if (ci->deltas)
		for (i = 0; i < ci->ndeltas; i++)
			deltainfo_free(ci->deltas[i]);
	free(ci->deltas);
	ci->deltas = NULL;
	ci->ndeltas = 0;
	ci->addcount = 0;
	ci->delcount = 0;
	ci->filecount = 0;

	return -1;
}

void
commitinfo_free(struct commitinfo *ci)
{
	size_t i;

	if (!ci)
		return;
	if (ci->deltas)
		for (i = 0; i < ci->ndeltas; i++)
			deltainfo_free(ci->deltas[i]);
	free(ci->deltas);
	ci->deltas = NULL;
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

	if (!(ci = calloc(1, sizeof(struct commitinfo))))
		err(1, "calloc");

	if (git_commit_lookup(&(ci->commit), repo, id))
		goto err;
	ci->id = id;

	git_oid_tostr(ci->oid, sizeof(ci->oid), git_commit_id(ci->commit));
	git_oid_tostr(ci->parentoid, sizeof(ci->parentoid), git_commit_parent_id(ci->commit, 0));

	ci->author = git_commit_author(ci->commit);
	ci->committer = git_commit_committer(ci->commit);
	ci->summary = git_commit_summary(ci->commit);
	ci->msg = git_commit_message(ci->commit);

	if (git_tree_lookup(&(ci->commit_tree), repo, git_commit_tree_id(ci->commit)))
		goto err;
	if (!git_commit_parent(&(ci->parent), ci->commit, 0)) {
		if (git_tree_lookup(&(ci->parent_tree), repo, git_commit_tree_id(ci->parent))) {
			ci->parent = NULL;
			ci->parent_tree = NULL;
		}
	}

	git_diff_init_options(&opts, GIT_DIFF_OPTIONS_VERSION);
	opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
	if (git_diff_tree_to_tree(&(ci->diff), repo, ci->parent_tree, ci->commit_tree, &opts))
		goto err;
	if (commitinfo_getstats(ci) == -1)
		goto err;

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

int
mkdirp(const char *path)
{
	char tmp[PATH_MAX], *p;

	if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
		errx(1, "path truncated: '%s'", path);
	for (p = tmp + (tmp[0] == '/'); *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, S_IRWXU | S_IRWXG | S_IRWXO) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, S_IRWXU | S_IRWXG | S_IRWXO) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

void
printtimez(FILE *fp, const git_time *intime)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t)intime->time;
	if (!(intm = gmtime(&t)))
		return;
	strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", intm);
	fputs(out, fp);
}

void
printtime(FILE *fp, const git_time *intime)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t)intime->time + (intime->offset * 60);
	if (!(intm = gmtime(&t)))
		return;
	strftime(out, sizeof(out), "%a %b %e %H:%M:%S", intm);
	if (intime->offset < 0)
		fprintf(fp, "%s -%02d%02d", out,
		            -(intime->offset) / 60, -(intime->offset) % 60);
	else
		fprintf(fp, "%s +%02d%02d", out,
		            intime->offset / 60, intime->offset % 60);
}

void
printtimeshort(FILE *fp, const git_time *intime)
{
	struct tm *intm;
	time_t t;
	char out[32];

	t = (time_t)intime->time;
	if (!(intm = gmtime(&t)))
		return;
	strftime(out, sizeof(out), "%Y-%m-%d %H:%M", intm);
	fputs(out, fp);
}

void
writeheader(FILE *fp, const char *title)
{
	fputs("<!DOCTYPE html>\n"
		"<html>\n<head>\n"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
		"<title>", fp);
	xmlencode(fp, title, strlen(title));
	if (title[0] && strippedname[0])
		fputs(" - ", fp);
	xmlencode(fp, strippedname, strlen(strippedname));
	if (description[0])
		fputs(" - ", fp);
	xmlencode(fp, description, strlen(description));
	fprintf(fp, "</title>\n<link rel=\"icon\" type=\"image/png\" href=\"%sfavicon.png\" />\n", relpath);
	fprintf(fp, "<link rel=\"alternate\" type=\"application/atom+xml\" title=\"%s Atom Feed\" href=\"%satom.xml\" />\n",
		name, relpath);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"%sstyle.css\" />\n", relpath);
	fputs("</head>\n<body>\n<table><tr><td>", fp);
	fprintf(fp, "<a href=\"../%s\"><img src=\"%slogo.png\" alt=\"\" width=\"32\" height=\"32\" /></a>",
	        relpath, relpath);
	fputs("</td><td><h1>", fp);
	xmlencode(fp, strippedname, strlen(strippedname));
	fputs("</h1><span class=\"desc\">", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</span></td></tr>", fp);
	if (cloneurl[0]) {
		fputs("<tr class=\"url\"><td></td><td>git clone <a href=\"", fp);
		xmlencode(fp, cloneurl, strlen(cloneurl));
		fputs("\">", fp);
		xmlencode(fp, cloneurl, strlen(cloneurl));
		fputs("</a></td></tr>", fp);
	}
	fputs("<tr><td></td><td>\n", fp);
	fprintf(fp, "<a href=\"%slog.html\">Log</a> | ", relpath);
	fprintf(fp, "<a href=\"%sfiles.html\">Files</a> | ", relpath);
	fprintf(fp, "<a href=\"%srefs.html\">Refs</a>", relpath);
	if (hassubmodules)
		fprintf(fp, " | <a href=\"%sfile/.gitmodules.html\">Submodules</a>", relpath);
	if (hasreadme)
		fprintf(fp, " | <a href=\"%sfile/README.html\">README</a>", relpath);
	if (haslicense)
		fprintf(fp, " | <a href=\"%sfile/LICENSE.html\">LICENSE</a>", relpath);
	fputs("</td></tr></table>\n<hr/>\n<div id=\"content\">\n", fp);
}

void
writefooter(FILE *fp)
{
	fputs("</div>\n</body>\n</html>\n", fp);
}

int
writeblobhtml(FILE *fp, const git_blob *blob)
{
	off_t i;
	size_t n = 0;
	char *nfmt = "<a href=\"#l%d\" id=\"l%d\">%d</a>\n";
	const char *s = git_blob_rawcontent(blob);
	git_off_t len = git_blob_rawsize(blob);

	fputs("<table id=\"blob\"><tr><td class=\"num\"><pre>\n", fp);

	if (len) {
		n++;
		fprintf(fp, nfmt, n, n, n);
		for (i = 0; i < len - 1; i++) {
			if (s[i] == '\n') {
				n++;
				fprintf(fp, nfmt, n, n, n);
			}
		}
	}

	fputs("</pre></td><td><pre>\n", fp);
	xmlencode(fp, s, (size_t)len);
	fputs("</pre></td></tr></table>\n", fp);

	return n;
}

void
printcommit(FILE *fp, struct commitinfo *ci)
{
	fprintf(fp, "<b>commit</b> <a href=\"%scommit/%s.html\">%s</a>\n",
		relpath, ci->oid, ci->oid);

	if (ci->parentoid[0])
		fprintf(fp, "<b>parent</b> <a href=\"%scommit/%s.html\">%s</a>\n",
			relpath, ci->parentoid, ci->parentoid);

	if (ci->author) {
		fputs("<b>Author:</b> ", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs(" &lt;<a href=\"mailto:", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("\">", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("</a>&gt;\n<b>Date:</b>   ", fp);
		printtime(fp, &(ci->author->when));
		fputc('\n', fp);
	}
	if (ci->msg) {
		fputc('\n', fp);
		xmlencode(fp, ci->msg, strlen(ci->msg));
		fputc('\n', fp);
	}
}

void
printshowfile(FILE *fp, struct commitinfo *ci)
{
	const git_diff_delta *delta;
	const git_diff_hunk *hunk;
	const git_diff_line *line;
	git_patch *patch;
	size_t nhunks, nhunklines, changed, add, del, total, i, j, k;
	char linestr[80];

	printcommit(fp, ci);

	if (!ci->deltas)
		return;

	if (ci->filecount > 1000   ||
	    ci->ndeltas   > 1000   ||
	    ci->addcount  > 100000 ||
	    ci->delcount  > 100000) {
		fputs("Diff is too large, output suppressed.\n", fp);
		return;
	}

	/* diff stat */
	fputs("<b>Diffstat:</b>\n<table>", fp);
	for (i = 0; i < ci->ndeltas; i++) {
		delta = git_patch_get_delta(ci->deltas[i]->patch);
		fprintf(fp, "<tr><td><a href=\"#h%zu\">", i);
		xmlencode(fp, delta->old_file.path, strlen(delta->old_file.path));
		if (strcmp(delta->old_file.path, delta->new_file.path)) {
			fputs(" -&gt; ", fp);
			xmlencode(fp, delta->new_file.path, strlen(delta->new_file.path));
		}

		add = ci->deltas[i]->addcount;
		del = ci->deltas[i]->delcount;
		changed = add + del;
		total = sizeof(linestr) - 2;
		if (changed > total) {
			if (add)
				add = ((float)total / changed * add) + 1;
			if (del)
				del = ((float)total / changed * del) + 1;
		}
		memset(&linestr, '+', add);
		memset(&linestr[add], '-', del);

		fprintf(fp, "</a></td><td> | </td><td class=\"num\">%zu</td><td><span class=\"i\">",
		        ci->deltas[i]->addcount + ci->deltas[i]->delcount);
		fwrite(&linestr, 1, add, fp);
		fputs("</span><span class=\"d\">", fp);
		fwrite(&linestr[add], 1, del, fp);
		fputs("</span></td></tr>\n", fp);
	}
	fprintf(fp, "</table>%zu file%s changed, %zu insertion%s(+), %zu deletion%s(-)\n",
		ci->filecount, ci->filecount == 1 ? "" : "s",
	        ci->addcount,  ci->addcount  == 1 ? "" : "s",
	        ci->delcount,  ci->delcount  == 1 ? "" : "s");

	fputs("<hr/>", fp);

	for (i = 0; i < ci->ndeltas; i++) {
		patch = ci->deltas[i]->patch;
		delta = git_patch_get_delta(patch);
		fprintf(fp, "<b>diff --git a/<a id=\"h%zu\" href=\"%sfile/%s.html\">%s</a> b/<a href=\"%sfile/%s.html\">%s</a></b>\n",
			i, relpath, delta->old_file.path, delta->old_file.path,
			relpath, delta->new_file.path, delta->new_file.path);

		/* check binary data */
		if (delta->flags & GIT_DIFF_FLAG_BINARY) {
			fputs("Binary files differ.\n", fp);
			continue;
		}

		nhunks = git_patch_num_hunks(patch);
		for (j = 0; j < nhunks; j++) {
			if (git_patch_get_hunk(&hunk, &nhunklines, patch, j))
				break;

			fprintf(fp, "<a href=\"#h%zu-%zu\" id=\"h%zu-%zu\" class=\"h\">", i, j, i, j);
			xmlencode(fp, hunk->header, hunk->header_len);
			fputs("</a>", fp);

			for (k = 0; ; k++) {
				if (git_patch_get_line_in_hunk(&line, patch, j, k))
					break;
				if (line->old_lineno == -1)
					fprintf(fp, "<a href=\"#h%zu-%zu-%zu\" id=\"h%zu-%zu-%zu\" class=\"i\">+",
						i, j, k, i, j, k);
				else if (line->new_lineno == -1)
					fprintf(fp, "<a href=\"#h%zu-%zu-%zu\" id=\"h%zu-%zu-%zu\" class=\"d\">-",
						i, j, k, i, j, k);
				else
					fputc(' ', fp);
				xmlencode(fp, line->content, line->content_len);
				if (line->old_lineno == -1 || line->new_lineno == -1)
					fputs("</a>", fp);
			}
		}
	}
}

void
writelogline(FILE *fp, struct commitinfo *ci)
{
	size_t len;

	fputs("<tr><td>", fp);
	if (ci->author)
		printtimeshort(fp, &(ci->author->when));
	fputs("</td><td>", fp);
	if (ci->summary) {
		fprintf(fp, "<a href=\"%scommit/%s.html\">", relpath, ci->oid);
		if ((len = strlen(ci->summary)) > summarylen) {
			xmlencode(fp, ci->summary, summarylen - 1);
			fputs("…", fp);
		} else {
			xmlencode(fp, ci->summary, len);
		}
		fputs("</a>", fp);
	}
	fputs("</td><td>", fp);
	if (ci->author)
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
	fputs("</td><td class=\"num\">", fp);
	fprintf(fp, "%zu", ci->filecount);
	fputs("</td><td class=\"num\">", fp);
	fprintf(fp, "+%zu", ci->addcount);
	fputs("</td><td class=\"num\">", fp);
	fprintf(fp, "-%zu", ci->delcount);
	fputs("</td></tr>\n", fp);
}

int
writelog(FILE *fp, const git_oid *oid)
{
	struct commitinfo *ci;
	git_revwalk *w = NULL;
	git_oid id;
	char path[PATH_MAX];
	FILE *fpfile;
	int r;

	git_revwalk_new(&w, repo);
	git_revwalk_push(w, oid);
	git_revwalk_sorting(w, GIT_SORT_TIME);
	git_revwalk_simplify_first_parent(w);

	while (!git_revwalk_next(&id, w)) {
		relpath = "";

		if (cachefile && !memcmp(&id, &lastoid, sizeof(id)))
			break;
		if (!(ci = commitinfo_getbyoid(&id)))
			break;

		writelogline(fp, ci);
		if (cachefile)
			writelogline(wcachefp, ci);

		relpath = "../";

		r = snprintf(path, sizeof(path), "commit/%s.html", ci->oid);
		if (r == -1 || (size_t)r >= sizeof(path))
			errx(1, "path truncated: 'commit/%s.html'", ci->oid);

		/* check if file exists if so skip it */
		if (access(path, F_OK)) {
			fpfile = efopen(path, "w");
			writeheader(fpfile, ci->summary);
			fputs("<pre>", fpfile);
			printshowfile(fpfile, ci);
			fputs("</pre>\n", fpfile);
			writefooter(fpfile);
			fclose(fpfile);
		}
		commitinfo_free(ci);
	}
	git_revwalk_free(w);

	relpath = "";

	return 0;
}

void
printcommitatom(FILE *fp, struct commitinfo *ci)
{
	fputs("<entry>\n", fp);

	fprintf(fp, "<id>%s</id>\n", ci->oid);
	if (ci->author) {
		fputs("<published>", fp);
		printtimez(fp, &(ci->author->when));
		fputs("</published>\n", fp);
	}
	if (ci->committer) {
		fputs("<updated>", fp);
		printtimez(fp, &(ci->committer->when));
		fputs("</updated>\n", fp);
	}
	if (ci->summary) {
		fputs("<title type=\"text\">", fp);
		xmlencode(fp, ci->summary, strlen(ci->summary));
		fputs("</title>\n", fp);
	}
	fprintf(fp, "<link rel=\"alternate\" type=\"text/html\" href=\"commit/%s.html\" />",
	        ci->oid);

	if (ci->author) {
		fputs("<author><name>", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs("</name>\n<email>", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("</email>\n</author>\n", fp);
	}

	fputs("<content type=\"text\">", fp);
	fprintf(fp, "commit %s\n", ci->oid);
	if (ci->parentoid[0])
		fprintf(fp, "parent %s\n", ci->parentoid);
	if (ci->author) {
		fputs("Author: ", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs(" &lt;", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("&gt;\nDate:   ", fp);
		printtime(fp, &(ci->author->when));
		fputc('\n', fp);
	}
	if (ci->msg) {
		fputc('\n', fp);
		xmlencode(fp, ci->msg, strlen(ci->msg));
	}
	fputs("\n</content>\n</entry>\n", fp);
}

int
writeatom(FILE *fp)
{
	struct commitinfo *ci;
	git_revwalk *w = NULL;
	git_oid id;
	size_t i, m = 100; /* last 'm' commits */

	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	      "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n<title>", fp);
	xmlencode(fp, strippedname, strlen(strippedname));
	fputs(", branch HEAD</title>\n<subtitle>", fp);
	xmlencode(fp, description, strlen(description));
	fputs("</subtitle>\n", fp);

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);
	git_revwalk_sorting(w, GIT_SORT_TIME);
	git_revwalk_simplify_first_parent(w);

	for (i = 0; i < m && !git_revwalk_next(&id, w); i++) {
		if (!(ci = commitinfo_getbyoid(&id)))
			break;
		printcommitatom(fp, ci);
		commitinfo_free(ci);
	}
	git_revwalk_free(w);

	fputs("</feed>", fp);

	return 0;
}

int
writeblob(git_object *obj, const char *fpath, const char *filename, git_off_t filesize)
{
	char tmp[PATH_MAX] = "", *d;
	const char *p;
	int lc = 0;
	FILE *fp;

	if (strlcpy(tmp, fpath, sizeof(tmp)) >= sizeof(tmp))
		errx(1, "path truncated: '%s'", fpath);
	if (!(d = dirname(tmp)))
		err(1, "dirname");
	if (mkdirp(d))
		return -1;

	for (p = fpath, tmp[0] = '\0'; *p; p++) {
		if (*p == '/' && strlcat(tmp, "../", sizeof(tmp)) >= sizeof(tmp))
			errx(1, "path truncated: '../%s'", tmp);
	}
	relpath = tmp;

	fp = efopen(fpath, "w");
	writeheader(fp, filename);
	fputs("<p> ", fp);
	xmlencode(fp, filename, strlen(filename));
	fprintf(fp, " (%juB)", (uintmax_t)filesize);
	fputs("</p><hr/>", fp);

	if (git_blob_is_binary((git_blob *)obj)) {
		fputs("<p>Binary file.</p>\n", fp);
	} else {
		lc = writeblobhtml(fp, (git_blob *)obj);
		if (ferror(fp))
			err(1, "fwrite");
	}
	writefooter(fp);
	fclose(fp);

	relpath = "";

	return lc;
}

const char *
filemode(git_filemode_t m)
{
	static char mode[11];

	memset(mode, '-', sizeof(mode) - 1);
	mode[10] = '\0';

	if (S_ISREG(m))
		mode[0] = '-';
	else if (S_ISBLK(m))
		mode[0] = 'b';
	else if (S_ISCHR(m))
		mode[0] = 'c';
	else if (S_ISDIR(m))
		mode[0] = 'd';
	else if (S_ISFIFO(m))
		mode[0] = 'p';
	else if (S_ISLNK(m))
		mode[0] = 'l';
	else if (S_ISSOCK(m))
		mode[0] = 's';
	else
		mode[0] = '?';

	if (m & S_IRUSR) mode[1] = 'r';
	if (m & S_IWUSR) mode[2] = 'w';
	if (m & S_IXUSR) mode[3] = 'x';
	if (m & S_IRGRP) mode[4] = 'r';
	if (m & S_IWGRP) mode[5] = 'w';
	if (m & S_IXGRP) mode[6] = 'x';
	if (m & S_IROTH) mode[7] = 'r';
	if (m & S_IWOTH) mode[8] = 'w';
	if (m & S_IXOTH) mode[9] = 'x';

	if (m & S_ISUID) mode[3] = (mode[3] == 'x') ? 's' : 'S';
	if (m & S_ISGID) mode[6] = (mode[6] == 'x') ? 's' : 'S';
	if (m & S_ISVTX) mode[9] = (mode[9] == 'x') ? 't' : 'T';

	return mode;
}

int
writefilestree(FILE *fp, git_tree *tree, const char *path)
{
	const git_tree_entry *entry = NULL;
	git_submodule *module = NULL;
	git_object *obj = NULL;
	git_off_t filesize;
	const char *entryname;
	char filepath[PATH_MAX], entrypath[PATH_MAX];
	size_t count, i;
	int lc, r, ret;

	count = git_tree_entrycount(tree);
	for (i = 0; i < count; i++) {
		if (!(entry = git_tree_entry_byindex(tree, i)) ||
		    !(entryname = git_tree_entry_name(entry)))
			return -1;
		joinpath(entrypath, sizeof(entrypath), path, entryname);

		r = snprintf(filepath, sizeof(filepath), "file/%s.html",
		         entrypath);
		if (r == -1 || (size_t)r >= sizeof(filepath))
			errx(1, "path truncated: 'file/%s.html'", entrypath);

		if (!git_tree_entry_to_object(&obj, repo, entry)) {
			switch (git_object_type(obj)) {
			case GIT_OBJ_BLOB:
				break;
			case GIT_OBJ_TREE:
				/* NOTE: recurses */
				ret = writefilestree(fp, (git_tree *)obj,
				                     entrypath);
				git_object_free(obj);
				if (ret)
					return ret;
				continue;
			default:
				git_object_free(obj);
				continue;
			}

			filesize = git_blob_rawsize((git_blob *)obj);
			lc = writeblob(obj, filepath, entryname, filesize);

			fputs("<tr><td>", fp);
			fputs(filemode(git_tree_entry_filemode(entry)), fp);
			fprintf(fp, "</td><td><a href=\"%s%s\">", relpath, filepath);
			xmlencode(fp, entrypath, strlen(entrypath));
			fputs("</a></td><td class=\"num\">", fp);
			if (showlinecount && lc > 0)
				fprintf(fp, "%dL", lc);
			else
				fprintf(fp, "%juB", (uintmax_t)filesize);
			fputs("</td></tr>\n", fp);
		} else if (!git_submodule_lookup(&module, repo, entryname)) {
			fprintf(fp, "<tr><td>m---------</td><td><a href=\"%sfile/.gitmodules.html\">",
				relpath);
			xmlencode(fp, entrypath, strlen(entrypath));
			git_submodule_free(module);
			fputs("</a></td><td class=\"num\"></td></tr>\n", fp);
		}
	}

	return 0;
}

int
writefiles(FILE *fp, const git_oid *id)
{
	git_tree *tree = NULL;
	git_commit *commit = NULL;
	int ret = -1;

	fputs("<table id=\"files\"><thead>\n<tr>"
	      "<td>Mode</td><td>Name</td><td class=\"num\">Size</td>"
	      "</tr>\n</thead><tbody>\n", fp);

	if (!git_commit_lookup(&commit, repo, id) &&
	    !git_commit_tree(&tree, commit))
		ret = writefilestree(fp, tree, "");

	fputs("</tbody></table>", fp);

	git_commit_free(commit);
	git_tree_free(tree);

	return ret;
}

int
refs_cmp(const void *v1, const void *v2)
{
	git_reference *r1 = (*(git_reference **)v1);
	git_reference *r2 = (*(git_reference **)v2);
	int r;

	if ((r = git_reference_is_branch(r1) - git_reference_is_branch(r2)))
		return r;

	return strcmp(git_reference_shorthand(r1),
	              git_reference_shorthand(r2));
}

int
writerefs(FILE *fp)
{
	struct commitinfo *ci;
	const git_oid *id = NULL;
	git_object *obj = NULL;
	git_reference *dref = NULL, *r, *ref = NULL;
	git_reference_iterator *it = NULL;
	git_reference **refs = NULL;
	size_t count, i, j, refcount;
	const char *titles[] = { "Branches", "Tags" };
	const char *ids[] = { "branches", "tags" };
	const char *name;

	if (git_reference_iterator_new(&it, repo))
		return -1;

	for (refcount = 0; !git_reference_next(&ref, it); refcount++) {
		if (!(refs = reallocarray(refs, refcount + 1, sizeof(git_reference *))))
			err(1, "realloc");
		refs[refcount] = ref;
	}
	git_reference_iterator_free(it);

	/* sort by type then shorthand name */
	qsort(refs, refcount, sizeof(git_reference *), refs_cmp);

	for (j = 0; j < 2; j++) {
		for (i = 0, count = 0; i < refcount; i++) {
			if (!(git_reference_is_branch(refs[i]) && j == 0) &&
			    !(git_reference_is_tag(refs[i]) && j == 1))
				continue;

			switch (git_reference_type(refs[i])) {
			case GIT_REF_SYMBOLIC:
				if (git_reference_resolve(&dref, refs[i]))
					goto err;
				r = dref;
				break;
			case GIT_REF_OID:
				r = refs[i];
				break;
			default:
				continue;
			}
			if (!git_reference_target(r) ||
			    git_reference_peel(&obj, r, GIT_OBJ_ANY))
				goto err;
			if (!(id = git_object_id(obj)))
				goto err;
			if (!(ci = commitinfo_getbyoid(id)))
				break;

			/* print header if it has an entry (first). */
			if (++count == 1) {
				fprintf(fp, "<h2>%s</h2><table id=\"%s\"><thead>\n<tr><td>Name</td>"
				      "<td>Last commit date</td><td>Author</td>\n</tr>\n</thead><tbody>\n",
				      titles[j], ids[j]);
			}

			relpath = "";
			name = git_reference_shorthand(r);

			fputs("<tr><td>", fp);
			xmlencode(fp, name, strlen(name));
			fputs("</td><td>", fp);
			if (ci->author)
				printtimeshort(fp, &(ci->author->when));
			fputs("</td><td>", fp);
			if (ci->author)
				xmlencode(fp, ci->author->name, strlen(ci->author->name));
			fputs("</td></tr>\n", fp);

			relpath = "../";

			commitinfo_free(ci);
			git_object_free(obj);
			obj = NULL;
			git_reference_free(dref);
			dref = NULL;
		}
		/* table footer */
		if (count)
			fputs("</tbody></table><br/>", fp);
	}

err:
	git_object_free(obj);
	git_reference_free(dref);

	for (i = 0; i < refcount; i++)
		git_reference_free(refs[i]);
	free(refs);

	return 0;
}

void
usage(char *argv0)
{
	fprintf(stderr, "%s [-c cachefile] repodir\n", argv0);
	exit(1);
}

int
main(int argc, char *argv[])
{
	git_object *obj = NULL;
	const git_oid *head = NULL;
	const git_error *e = NULL;
	FILE *fp, *fpread;
	char path[PATH_MAX], repodirabs[PATH_MAX + 1], *p;
	char tmppath[64] = "cache.XXXXXXXXXXXX", buf[BUFSIZ];
	size_t n;
	int i, fd;

	if (pledge("stdio rpath wpath cpath", NULL) == -1)
		err(1, "pledge");

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			if (repodir)
				usage(argv[0]);
			repodir = argv[i];
		} else if (argv[i][1] == 'c') {
			if (i + 1 >= argc)
				usage(argv[0]);
			cachefile = argv[++i];
		}
	}
	if (!repodir)
		usage(argv[0]);

	if (!realpath(repodir, repodirabs))
		err(1, "realpath");

	git_libgit2_init();

	if (git_repository_open_ext(&repo, repodir,
		GIT_REPOSITORY_OPEN_NO_SEARCH, NULL) < 0) {
		e = giterr_last();
		fprintf(stderr, "%s: %s\n", argv[0], e->message);
		return 1;
	}

	/* find HEAD */
	if (!git_revparse_single(&obj, repo, "HEAD"))
		head = git_object_id(obj);
	git_object_free(obj);

	/* don't cache if there is no HEAD */
	if (!head)
		cachefile = NULL;

	/* use directory name as name */
	if ((name = strrchr(repodirabs, '/')))
		name++;
	else
		name = "";

	/* strip .git suffix */
	if (!(strippedname = strdup(name)))
		err(1, "strdup");
	if ((p = strrchr(strippedname, '.')))
		if (!strcmp(p, ".git"))
			*p = '\0';

	/* read description or .git/description */
	joinpath(path, sizeof(path), repodir, "description");
	if (!(fpread = fopen(path, "r"))) {
		joinpath(path, sizeof(path), repodir, ".git/description");
		fpread = fopen(path, "r");
	}
	if (fpread) {
		if (!fgets(description, sizeof(description), fpread))
			description[0] = '\0';
		fclose(fpread);
	}

	/* read url or .git/url */
	joinpath(path, sizeof(path), repodir, "url");
	if (!(fpread = fopen(path, "r"))) {
		joinpath(path, sizeof(path), repodir, ".git/url");
		fpread = fopen(path, "r");
	}
	if (fpread) {
		if (!fgets(cloneurl, sizeof(cloneurl), fpread))
			cloneurl[0] = '\0';
		cloneurl[strcspn(cloneurl, "\n")] = '\0';
		fclose(fpread);
	}

	/* check LICENSE */
	haslicense = (!git_revparse_single(&obj, repo, "HEAD:LICENSE") &&
		git_object_type(obj) == GIT_OBJ_BLOB);
	git_object_free(obj);

	/* check README */
	hasreadme = (!git_revparse_single(&obj, repo, "HEAD:README") &&
		git_object_type(obj) == GIT_OBJ_BLOB);
	git_object_free(obj);

	hassubmodules = (!git_revparse_single(&obj, repo, "HEAD:.gitmodules") &&
		git_object_type(obj) == GIT_OBJ_BLOB);
	git_object_free(obj);

	/* log for HEAD */
	fp = efopen("log.html", "w");
	relpath = "";
	mkdir("commit", 0755);
	writeheader(fp, "Log");
	fputs("<table id=\"log\"><thead>\n<tr><td>Date</td><td>Commit message</td>"
		  "<td>Author</td><td class=\"num\">Files</td><td class=\"num\">+</td>"
		  "<td class=\"num\">-</td></tr>\n</thead><tbody>\n", fp);

	if (cachefile) {
		/* read from cache file (does not need to exist) */
		if ((rcachefp = fopen(cachefile, "r"))) {
			if (!fgets(lastoidstr, sizeof(lastoidstr), rcachefp))
				errx(1, "%s: no object id", cachefile);
			if (git_oid_fromstr(&lastoid, lastoidstr))
				errx(1, "%s: invalid object id", cachefile);
		}

		/* write log to (temporary) cache */
		if ((fd = mkstemp(tmppath)) == -1)
			err(1, "mkstemp");
		if (!(wcachefp = fdopen(fd, "w")))
			err(1, "fdopen");
		/* write last commit id (HEAD) */
		git_oid_tostr(buf, sizeof(buf), head);
		fprintf(wcachefp, "%s\n", buf);

		writelog(fp, head);

		if (rcachefp) {
			/* append previous log to log.html and the new cache */
			while (!feof(rcachefp)) {
				n = fread(buf, 1, sizeof(buf), rcachefp);
				if (ferror(rcachefp))
					err(1, "fread");
				if (fwrite(buf, 1, n, fp) != n ||
				    fwrite(buf, 1, n, wcachefp) != n)
					err(1, "fwrite");
			}
			fclose(rcachefp);
		}
		fclose(wcachefp);
	} else {
		if (head)
			writelog(fp, head);
	}

	fputs("</tbody></table>", fp);
	writefooter(fp);
	fclose(fp);

	/* files for HEAD */
	fp = efopen("files.html", "w");
	writeheader(fp, "Files");
	if (head)
		writefiles(fp, head);
	writefooter(fp);
	fclose(fp);

	/* summary page with branches and tags */
	fp = efopen("refs.html", "w");
	writeheader(fp, "Refs");
	writerefs(fp);
	writefooter(fp);
	fclose(fp);

	/* Atom feed */
	fp = efopen("atom.xml", "w");
	writeatom(fp);
	fclose(fp);

	/* rename new cache file on success */
	if (cachefile && rename(tmppath, cachefile))
		err(1, "rename: '%s' to '%s'", tmppath, cachefile);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return 0;
}
