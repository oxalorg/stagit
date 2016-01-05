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
#include "config.h"

struct commitinfo {
	const git_oid *id;

	char oid[GIT_OID_HEXSZ + 1];
	char parentoid[GIT_OID_HEXSZ + 1];

	const git_signature *author;
	const char          *summary;
	const char          *msg;

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

static char name[255];
static char description[255];
static char cloneurl[1024];
static int hasreadme, haslicense;

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

	git_oid_tostr(ci->oid, sizeof(ci->oid), git_commit_id(ci->commit));
	git_oid_tostr(ci->parentoid, sizeof(ci->parentoid), git_commit_parent_id(ci->commit, 0));

	ci->author = git_commit_author(ci->commit);
	ci->summary = git_commit_summary(ci->commit);
	ci->msg = git_commit_message(ci->commit);

	if ((error = git_commit_tree(&(ci->commit_tree), ci->commit)))
		goto err;
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

/* Some implementations of dirname(3) return a pointer to a static
 * internal buffer (OpenBSD). Others modify the contents of `path` (POSIX).
 * This is a wrapper function that is compatible with both versions.
 * The program will error out if dirname(3) failed, this can only happen
 * with the OpenBSD version. */
char *
xdirname(const char *path)
{
	char *p, *b;

	if (!(p = strdup(path)))
		err(1, "strdup");
	if (!(b = dirname(p)))
		err(1, "basename");
	if (!(b = strdup(b)))
		err(1, "strdup");
	free(p);

	return b;
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

int
mkdirp(const char *path)
{
	char tmp[PATH_MAX], *p;

	strlcpy(tmp, path, sizeof(tmp));
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
printtimez(FILE *fp, const git_time *intime)
{
	printtimeformat(fp, intime, "%Y-%m-%dT%H:%M:%SZ");
}

void
printtime(FILE *fp, const git_time *intime)
{
	printtimeformat(fp, intime, "%a %b %e %T %Y");
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
	xmlencode(fp, name, strlen(name));
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
	xmlencode(fp, name, strlen(name));
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
	fprintf(fp, "<a href=\"%srefs.html\">Refs/branches</a>", relpath);
	if (hasreadme)
		fprintf(fp, " | <a href=\"%sfile/README.html\">README</a>", relpath);
	if (haslicense)
		fprintf(fp, " | <a href=\"%sfile/LICENSE.html\">LICENSE</a>", relpath);
	fputs("</td></tr></table>\n<hr/><div id=\"content\">\n", fp);

	return 0;
}

int
writefooter(FILE *fp)
{
	return !fputs("</div></body>\n</html>", fp);
}

void
writeblobhtml(FILE *fp, const git_blob *blob)
{
	off_t i;
	size_t n = 1;
	char *nfmt = "<a href=\"#l%d\" id=\"l%d\">%d</a>\n";
	const char *s = git_blob_rawcontent(blob);
	git_off_t len = git_blob_rawsize(blob);

	fputs("<table id=\"blob\"><tr><td class=\"num\"><pre>\n", fp);

	if (len) {
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
}

void
printcommit(FILE *fp, struct commitinfo *ci)
{
	fprintf(fp, "<b>commit</b> <a href=\"%scommit/%s.html\">%s</a>\n",
		relpath, ci->oid, ci->oid);

	if (ci->parentoid[0])
		fprintf(fp, "<b>parent</b> <a href=\"%scommit/%s.html\">%s</a>\n",
			relpath, ci->parentoid, ci->parentoid);

#if 0
	if ((count = (int)git_commit_parentcount(commit)) > 1) {
		fputs("<b>Merge:</b>", fp);
		for (i = 0; i < count; i++) {
			git_oid_tostr(buf, 8, git_commit_parent_id(commit, i));
			fprintf(fp, " <a href=\"%scommit/%s.html\">%s</a>",
				relpath, buf, buf);
		}
		fputc('\n', fp);
	}
#endif
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
	fputc('\n', fp);

	if (ci->msg)
		xmlencode(fp, ci->msg, strlen(ci->msg));

	fputc('\n', fp);
}

void
printshowfile(struct commitinfo *ci)
{
	const git_diff_delta *delta;
	const git_diff_hunk *hunk;
	const git_diff_line *line;
	git_patch *patch;
	git_buf statsbuf;
	size_t ndeltas, nhunks, nhunklines;
	FILE *fp;
	size_t i, j, k;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "commit/%s.html", ci->oid);
	/* check if file exists if so skip it */
	if (!access(path, F_OK))
		return;

	fp = efopen(path, "w");
	writeheader(fp);
	fputs("<pre>\n", fp);
	printcommit(fp, ci);

	memset(&statsbuf, 0, sizeof(statsbuf));

	/* diff stat */
	if (ci->stats) {
		if (!git_diff_stats_to_buf(&statsbuf, ci->stats,
		    GIT_DIFF_STATS_FULL | GIT_DIFF_STATS_SHORT, 80)) {
			if (statsbuf.ptr && statsbuf.ptr[0]) {
				fputs("<b>Diffstat:</b>\n", fp);
				fputs(statsbuf.ptr, fp);
			}
		}
	}

	fputs("<hr/>", fp);

	ndeltas = git_diff_num_deltas(ci->diff);
	for (i = 0; i < ndeltas; i++) {
		if (git_patch_from_diff(&patch, ci->diff, i)) {
			git_patch_free(patch);
			break;
		}

		delta = git_patch_get_delta(patch);
		fprintf(fp, "<b>diff --git a/<a href=\"%sfile/%s.html\">%s</a> b/<a href=\"%sfile/%s.html\">%s</a></b>\n",
			relpath, delta->old_file.path, delta->old_file.path,
			relpath, delta->new_file.path, delta->new_file.path);

		/* check binary data */
		if (delta->flags & GIT_DIFF_FLAG_BINARY) {
			fputs("Binary files differ\n", fp);
			git_patch_free(patch);
			continue;
		}

		nhunks = git_patch_num_hunks(patch);
		for (j = 0; j < nhunks; j++) {
			if (git_patch_get_hunk(&hunk, &nhunklines, patch, j))
				break;

			fprintf(fp, "<a href=\"#h%zu\" id=\"h%zu\" class=\"h\">", j, j);
			xmlencode(fp, hunk->header, hunk->header_len);
			fputs("</a>", fp);

			for (k = 0; ; k++) {
				if (git_patch_get_line_in_hunk(&line, patch, j, k))
					break;
				if (line->old_lineno == -1)
					fprintf(fp, "<a href=\"#h%zu-%zu\" id=\"h%zu-%zu\" class=\"i\">+",
						j, k, j, k);
				else if (line->new_lineno == -1)
					fprintf(fp, "<a href=\"#h%zu-%zu\" id=\"h%zu-%zu\" class=\"d\">-",
						j, k, j, k);
				else
					fputc(' ', fp);
				xmlencode(fp, line->content, line->content_len);
				if (line->old_lineno == -1 || line->new_lineno == -1)
					fputs("</a>", fp);
			}
		}
		git_patch_free(patch);
	}
	git_buf_free(&statsbuf);

	fputs("</pre>\n", fp);
	writefooter(fp);
	fclose(fp);
	return;
}

int
writelog(FILE *fp, const char *branch)
{
	struct commitinfo *ci;
	const git_oid *oid;
	git_revwalk *w = NULL;
	git_object *obj = NULL;
	git_oid id;
	size_t len;

	mkdir("commit", 0755);

	if (git_revparse_single(&obj, repo, branch))
		return -1;
	oid = git_object_id(obj);

	git_revwalk_new(&w, repo);
	git_revwalk_push(w, oid);
	git_revwalk_sorting(w, GIT_SORT_TIME);
	git_revwalk_simplify_first_parent(w);

	fputs("<table id=\"log\"><thead>\n<tr><td>Age</td><td>Commit message</td>"
		  "<td>Author</td><td>Files</td><td class=\"num\">+</td>"
		  "<td class=\"num\">-</td></tr>\n</thead><tbody>\n", fp);

	while (!git_revwalk_next(&id, w)) {
		relpath = "";

		if (!(ci = commitinfo_getbyoid(&id)))
			break;

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

		relpath = "../";
		printshowfile(ci);

		commitinfo_free(ci);
	}
	fputs("</tbody></table>", fp);

	git_revwalk_free(w);
	git_object_free(obj);

	relpath = "";

	return 0;
}

void
printcommitatom(FILE *fp, struct commitinfo *ci)
{
	fputs("<entry>\n", fp);

	fprintf(fp, "<id>%s</id>\n", ci->oid);
	if (ci->author) {
		fputs("<updated>", fp);
		printtimez(fp, &(ci->author->when));
		fputs("</updated>\n", fp);
	}
	if (ci->summary) {
		fputs("<title type=\"text\">", fp);
		xmlencode(fp, ci->summary, strlen(ci->summary));
		fputs("</title>\n", fp);
	}

	fputs("<content type=\"text\">", fp);
	fprintf(fp, "commit %s\n", ci->oid);
	if (ci->parentoid[0])
		fprintf(fp, "parent %s\n", ci->parentoid);

#if 0
	if ((count = (int)git_commit_parentcount(commit)) > 1) {
		fputs("Merge:", fp);
		for (i = 0; i < count; i++) {
			git_oid_tostr(buf, 8, git_commit_parent_id(commit, i));
			fprintf(fp, " %s", buf);
		}
		fputc('\n', fp);
	}
#endif

	if (ci->author) {
		fputs("Author: ", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs(" &lt;", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("&gt;\nDate:   ", fp);
		printtime(fp, &(ci->author->when));
	}
	fputc('\n', fp);

	if (ci->msg)
		xmlencode(fp, ci->msg, strlen(ci->msg));
	fputs("\n</content>\n", fp);
	if (ci->author) {
		fputs("<author><name>", fp);
		xmlencode(fp, ci->author->name, strlen(ci->author->name));
		fputs("</name>\n<email>", fp);
		xmlencode(fp, ci->author->email, strlen(ci->author->email));
		fputs("</email>\n</author>\n", fp);
	}
	fputs("</entry>\n", fp);
}

int
writeatom(FILE *fp)
{
	struct commitinfo *ci;
	git_revwalk *w = NULL;
	git_oid id;
	size_t i, m = 100; /* max */

	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	      "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n<title>", fp);
	xmlencode(fp, name, strlen(name));
	fputs(", branch master</title>\n<subtitle>", fp);

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
writeblob(git_object *obj, const char *filename, git_off_t filesize)
{
	char fpath[PATH_MAX];
	char tmp[PATH_MAX] = "";
	char *d, *p;
	FILE *fp;

	snprintf(fpath, sizeof(fpath), "file/%s.html", filename);
	d = xdirname(fpath);
	if (mkdirp(d)) {
		free(d);
		return 1;
	}
	free(d);

	p = fpath;
	while (*p) {
		if (*p == '/')
			strlcat(tmp, "../", sizeof(tmp));
		p++;
	}
	relpath = tmp;

	fp = efopen(fpath, "w");
	writeheader(fp);
	fputs("<p> ", fp);
	xmlencode(fp, filename, strlen(filename));
	fprintf(fp, " (%" PRIu32 "b)", filesize);
	fputs("</p><hr/>", fp);

	if (git_blob_is_binary((git_blob *)obj)) {
		fputs("<p>Binary file</p>\n", fp);
	} else {
		writeblobhtml(fp, (git_blob *)obj);
		if (ferror(fp))
			err(1, "fwrite");
	}
	writefooter(fp);
	fclose(fp);

	relpath = "";

	return 0;
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
writefilestree(FILE *fp, git_tree *tree, const char *branch, const char *path)
{
	const git_tree_entry *entry = NULL;
	const char *filename;
	char filepath[PATH_MAX];
	git_object *obj = NULL;
	git_off_t filesize;
	size_t count, i;
	int ret;

	count = git_tree_entrycount(tree);
	for (i = 0; i < count; i++) {
		if (!(entry = git_tree_entry_byindex(tree, i)) ||
		    git_tree_entry_to_object(&obj, repo, entry))
			return -1;
		filename = git_tree_entry_name(entry);
		switch (git_object_type(obj)) {
		case GIT_OBJ_BLOB:
			break;
		case GIT_OBJ_TREE:
			/* NOTE: recurses */
			ret = writefilestree(fp, (git_tree *)obj, branch,
			                     filename);
			git_object_free(obj);
			if (ret)
				return ret;
			continue;
		default:
			git_object_free(obj);
			continue;
		}
		if (path[0]) {
			snprintf(filepath, sizeof(filepath), "%s/%s",
			         path, filename);
			filename = filepath;
		}
		filesize = git_blob_rawsize((git_blob *)obj);

		fputs("<tr><td>", fp);
		fprintf(fp, "%s", filemode(git_tree_entry_filemode(entry)));
		fprintf(fp, "</td><td><a href=\"%sfile/", relpath);
		xmlencode(fp, filename, strlen(filename));
		fputs(".html\">", fp);
		xmlencode(fp, filename, strlen(filename));
		fputs("</a></td><td class=\"num\">", fp);
		fprintf(fp, "%" PRIu32, filesize);
		fputs("</td></tr>\n", fp);

		writeblob(obj, filename, filesize);
	}

	return 0;
}

int
writefiles(FILE *fp, const char *branch)
{
	const git_oid *id;
	git_tree *tree = NULL;
	git_object *obj = NULL;
	git_commit *commit = NULL;
	int ret = -1;

	fputs("<table id=\"files\"><thead>\n<tr>"
	      "<td>Mode</td><td>Name</td><td class=\"num\">Size</td>"
	      "</tr>\n</thead><tbody>\n", fp);

	if (git_revparse_single(&obj, repo, branch))
		goto err;
	id = git_object_id(obj);
	if (git_commit_lookup(&commit, repo, id) ||
	    git_commit_tree(&tree, commit))
		goto err;
	ret = writefilestree(fp, tree, branch, "");

err:
	fputs("</tbody></table>", fp);

	git_object_free(obj);
	git_commit_free(commit);
	git_tree_free(tree);

	return ret;
}

int
writebranches(FILE *fp)
{
	struct commitinfo *ci;
	git_branch_iterator *it = NULL;
	git_branch_t branch;
	git_reference *ref = NULL, *dref = NULL;
	const git_oid *id = NULL;
	const char *branchname = NULL;
	size_t len;
	int ret = -1;

	/* log for local branches */
	if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL))
		return -1;

	fputs("<h2>Branches</h2><table id=\"branches\"><thead>\n<tr><td>Branch</td><td>Age</td>"
		  "<td>Commit message</td>"
		  "<td>Author</td><td>Files</td><td class=\"num\">+</td>"
		  "<td class=\"num\">-</td></tr>\n</thead><tbody>\n", fp);

	while (!git_branch_next(&ref, &branch, it)) {
		if (git_branch_name(&branchname, ref))
			continue;

		id = NULL;
		switch (git_reference_type(ref)) {
		case GIT_REF_SYMBOLIC:
			if (git_reference_resolve(&dref, ref))
				goto err;
			id = git_reference_target(dref);
			break;
		case GIT_REF_OID:
			id = git_reference_target(ref);
			break;
		default:
			continue;
		}
		if (!id)
			goto err;
		if (!(ci = commitinfo_getbyoid(id)))
			break;

		relpath = "";

		fputs("<tr><td>", fp);
		xmlencode(fp, branchname, strlen(branchname));
		fputs("</td><td>", fp);
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

		relpath = "../";

		commitinfo_free(ci);
		git_reference_free(ref);
		git_reference_free(dref);
		ref = NULL;
		dref = NULL;
	}
	ret = 0;

err:
	fputs("</tbody></table>", fp);
	git_reference_free(ref);
	git_reference_free(dref);
	git_branch_iterator_free(it);

	return ret;
}

int
tagcompare(void *s1, void *s2)
{
	return strcmp(*(char **)s1, *(char **)s2);
}

int
writetags(FILE *fp)
{
	struct commitinfo *ci;
	git_strarray tagnames;
	git_object *obj = NULL;
	const git_oid *id = NULL;
	size_t i, len;

	/* summary page with branches and tags */
	memset(&tagnames, 0, sizeof(tagnames));
	if (git_tag_list(&tagnames, repo))
		return -1;
	if (!tagnames.count) {
		git_strarray_free(&tagnames);
		return 0;
	}

	/* sort names */
	qsort(tagnames.strings, tagnames.count, sizeof(char *),
	      (int (*)(const void *, const void *))&tagcompare);

	fputs("<h2>Tags</h2><table id=\"branches\"><thead>\n<tr><td>Tag</td>"
	      "<td>Age</td><td>Commit message</td>"
	      "<td>Author</td><td>Files</td><td class=\"num\">+</td>"
	      "<td class=\"num\">-</td></tr>\n</thead><tbody>\n", fp);

	for (i = 0; i < tagnames.count; i++) {
		if (git_revparse_single(&obj, repo, tagnames.strings[i]))
			continue;
		id = git_object_id(obj);
		if (!(ci = commitinfo_getbyoid(id)))
			break;

		relpath = "";

		fputs("<tr><td>", fp);
		xmlencode(fp, tagnames.strings[i], strlen(tagnames.strings[i]));
		fputs("</td><td>", fp);
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

		relpath = "../";

		commitinfo_free(ci);
		git_object_free(obj);
	}
	fputs("</tbody></table>", fp);
	git_strarray_free(&tagnames);

	return 0;
}

int
writerefs(FILE *fp)
{
	int ret;

	if ((ret = writebranches(fp)))
		return ret;
	fputs("<br/>", fp);
	return writetags(fp);
}

int
main(int argc, char *argv[])
{
	git_object *obj = NULL;
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
	snprintf(path, sizeof(path), "%s%s%s",
		repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "description");
	if (!(fpread = fopen(path, "r"))) {
		snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/description");
		fpread = fopen(path, "r");
	}
	if (fpread) {
		if (!fgets(description, sizeof(description), fpread))
			description[0] = '\0';
		fclose(fpread);
	}

	/* read url or .git/url */
	snprintf(path, sizeof(path), "%s%s%s",
		repodir, repodir[strlen(repodir)] == '/' ? "" : "/", "url");
	if (!(fpread = fopen(path, "r"))) {
		snprintf(path, sizeof(path), "%s%s%s",
			repodir, repodir[strlen(repodir)] == '/' ? "" : "/", ".git/url");
		fpread = fopen(path, "r");
	}
	if (fpread) {
		if (!fgets(cloneurl, sizeof(cloneurl), fpread))
			cloneurl[0] = '\0';
		fclose(fpread);
	}

	/* check LICENSE */
	haslicense = !git_revparse_single(&obj, repo, "HEAD:LICENSE");
	git_object_free(obj);
	/* check README */
	hasreadme = !git_revparse_single(&obj, repo, "HEAD:README");
	git_object_free(obj);

	/* log for HEAD */
	mkdir("log", 0755);
	fp = efopen("log.html", "w");
	relpath = "../";
	writeheader(fp);
	relpath = "";
	writelog(fp, "HEAD");
	writefooter(fp);
	fclose(fp);

	/* files for HEAD */
	fp = efopen("files.html", "w");
	writeheader(fp);
	writefiles(fp, "HEAD");
	writefooter(fp);
	fclose(fp);

	/* summary page with branches and tags */
	fp = efopen("refs.html", "w");
	writeheader(fp);
	writerefs(fp);
	writefooter(fp);
	fclose(fp);

	/* Atom feed */
	fp = efopen("atom.xml", "w");
	writeatom(fp);
	fclose(fp);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return 0;
}
