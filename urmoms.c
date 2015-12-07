#include <sys/stat.h>

#include <err.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "git2.h"

static git_repository *repo;

static const char *relpath = "";
static const char *repodir;

static char name[255];
static char description[255];
static int hasreadme, haslicense;

int
writeheader(FILE *fp)
{
	fputs("<!DOCTYPE HTML>"
		"<html dir=\"ltr\" lang=\"en\">\n<head>\n"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
		"<meta http-equiv=\"Content-Language\" content=\"en\" />\n", fp);
	fprintf(fp, "<title>%s%s%s</title>\n", name, description[0] ? " - " : "", description);
	fprintf(fp, "<link rel=\"icon\" type=\"image/png\" href=\"%sfavicon.png\" />\n", relpath);
	fprintf(fp, "<link rel=\"alternate\" type=\"application/atom+xml\" title=\"%s Atom Feed\" href=\"%satom.xml\" />\n",
		name, relpath);
	fprintf(fp, "<link rel=\"stylesheet\" type=\"text/css\" href=\"%sstyle.css\" />\n", relpath);
	fputs("</head>\n<body>\n", fp);
	fprintf(fp, "<h1><img src=\"%slogo.png\" alt=\"\" /> %s <span class=\"desc\">%s</span></h1>\n",
		relpath, name, description);
	fprintf(fp, "<a href=\"%slog.html\">Log</a> | ", relpath);
	fprintf(fp, "<a href=\"%sfiles.html\">Files</a>", relpath);
	/*fprintf(fp, "| <a href=\"%sstats.html\">Stats</a>", relpath);*/
	if (hasreadme)
		fprintf(fp, " | <a href=\"%sreadme.html\">README</a>", relpath);
	if (haslicense)
		fprintf(fp, " | <a href=\"%slicense.html\">LICENSE</a>", relpath);
	fputs("\n<hr/>\n<pre>", fp);

	return 0;
}

int
writefooter(FILE *fp)
{
	return !fputs("</pre>\n</body>\n</html>", fp);
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
printtimez(FILE *fp, const git_time *intime)
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
	strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", intm);
	fputs(out, fp);
}

void
printtime(FILE *fp, const git_time *intime)
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

	fprintf(fp, "%s %c%02d%02d", out, sign, hours, minutes);
}

void
printcommit(FILE *fp, git_commit *commit)
{
	const git_signature *sig;
	char buf[GIT_OID_HEXSZ + 1];
	int i, count;
	const char *scan, *eol;

	/* TODO: show tag when commit has it */
	git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));
	fprintf(fp, "commit <a href=\"%scommit/%s.html\">%s</a>\n",
		relpath, buf, buf);

	if (git_oid_tostr(buf, sizeof(buf), git_commit_parent_id(commit, 0)))
		fprintf(fp, "parent <a href=\"%scommit/%s.html\">%s</a>\n",
			relpath, buf, buf);

	if ((count = (int)git_commit_parentcount(commit)) > 1) {
		fprintf(fp, "Merge:");
		for (i = 0; i < count; ++i) {
			git_oid_tostr(buf, 8, git_commit_parent_id(commit, i));
			fprintf(fp, " <a href=\"%scommit/%s.html\">%s</a>",
				relpath, buf, buf);
		}
		fputc('\n', fp);
	}
	if ((sig = git_commit_author(commit)) != NULL) {
		fprintf(fp, "Author: ");
		xmlencode(fp, sig->name, strlen(sig->name));
		fprintf(fp, " &lt;<a href=\"mailto:");
		xmlencode(fp, sig->email, strlen(sig->email));
		fputs("\">", fp);
		xmlencode(fp, sig->email, strlen(sig->email));
		fputs("</a>&gt;\nDate:   ", fp);
		printtime(fp, &sig->when);
		fputc('\n', fp);
	}
	fputc('\n', fp);

	for (scan = git_commit_message(commit); scan && *scan;) {
		for (eol = scan; *eol && *eol != '\n'; ++eol)	/* find eol */
			;

		fprintf(fp, "    %.*s\n", (int) (eol - scan), scan);
		scan = *eol ? eol + 1 : NULL;
	}
	fputc('\n', fp);
}

void
printshowfile(git_commit *commit)
{
	const git_diff_delta *delta = NULL;
	const git_diff_hunk *hunk = NULL;
	const git_diff_line *line = NULL;
	git_commit *parent = NULL;
	git_tree *commit_tree = NULL, *parent_tree = NULL;
	git_patch *patch = NULL;
	git_diff *diff = NULL;
	git_diff_stats *diffstats = NULL;
	git_buf diffstatsbuf;
	size_t i, j, k, ndeltas, nhunks = 0, nhunklines = 0;
	char buf[GIT_OID_HEXSZ + 1], path[PATH_MAX];
	FILE *fp;
	int error;

	git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));
	snprintf(path, sizeof(path), "commit/%s.html", buf);
	fp = efopen(path, "w+b");

	memset(&diffstatsbuf, 0, sizeof(diffstatsbuf));

	writeheader(fp);
	printcommit(fp, commit);

	if ((error = git_commit_parent(&parent, commit, 0)))
		return;
	if ((error = git_commit_tree(&commit_tree, commit)))
		goto err;
	if ((error = git_commit_tree(&parent_tree, parent)))
		goto err;
	if ((error = git_diff_tree_to_tree(&diff, repo, commit_tree, parent_tree, NULL)))
		goto err;

	/* diff stat */
	if (!git_diff_get_stats(&diffstats, diff)) {
		if (!git_diff_stats_to_buf(&diffstatsbuf, diffstats,
		    GIT_DIFF_STATS_FULL | GIT_DIFF_STATS_SHORT, 80)) {
			fputs("<hr/>", fp);
			fprintf(fp, "Diffstat:\n");
			fputs(diffstatsbuf.ptr, fp);
		}
		git_diff_stats_free(diffstats);
	}
	fputs("<hr/>", fp);

	ndeltas = git_diff_num_deltas(diff);
	for (i = 0; i < ndeltas; i++) {
		if (git_patch_from_diff(&patch, diff, i)) {
			git_patch_free(patch);
			break; /* TODO: handle error */
		}

		delta = git_patch_get_delta(patch);
		fprintf(fp, "diff --git a/<a href=\"%sfile/%s\">%s</a> b/<a href=\"%sfile/%s\">%s</a>\n",
			relpath, delta->old_file.path, delta->old_file.path,
			relpath, delta->new_file.path, delta->new_file.path);

#if 0
		switch (delta->flags) {
		case GIT_DIFF_FLAG_BINARY:
			/* "Binary files /dev/null and b/favicon.png differ" or so */
			continue; /* TODO: binary data */
		case GIT_DIFF_FLAG_NOT_BINARY:   break;
		case GIT_DIFF_FLAG_VALID_ID:     break; /* TODO: check */
		case GIT_DIFF_FLAG_EXISTS:       break; /* TODO: check */
		}
#endif

		nhunks = git_patch_num_hunks(patch);
		for (j = 0; j < nhunks; j++) {
			if (git_patch_get_hunk(&hunk, &nhunklines, patch, j))
				break; /* TODO: handle error ? */

			fprintf(fp, "%s\n", hunk->header);

			for (k = 0; ; k++) {
				if (git_patch_get_line_in_hunk(&line, patch, j, k))
					break;
				if (line->old_lineno == -1)
					fprintf(fp, "<span class=\"i\"><a href=\"#h%zu-%zu\" id=\"h%zu-%zu\">+",
						j, k, j, k);
				else if (line->new_lineno == -1)
					fprintf(fp, "<span class=\"d\"><a href=\"#h%zu-%zu\" id=\"h%zu-%zu\">-",
						j, k, j, k);
				else
					fputc(' ', fp);
				xmlencode(fp, line->content, line->content_len);
				if (line->old_lineno == -1 || line->new_lineno == -1)
					fputs("</a></span>", fp);
			}
		}
		git_patch_free(patch);
	}
	git_diff_free(diff);

	writefooter(fp);
	fclose(fp);
	return;

err:
	git_buf_free(&diffstatsbuf);
	fclose(fp);
}

int
writelog(FILE *fp)
{
	git_revwalk *w = NULL;
	git_oid id;
	git_commit *commit = NULL;
	const git_signature *author;
	git_diff_stats *stats;
	git_tree *commit_tree = NULL, *parent_tree = NULL;
	git_commit *parent = NULL;
	git_diff *diff = NULL;
	size_t i, nfiles, ndel, nadd;
	const char *summary;
	char buf[GIT_OID_HEXSZ + 1];
	int error;

	mkdir("commit", 0755);

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);

	/* TODO: also make "expanded" log ? (with message body) */
	i = 0; /* DEBUG: to limit commits */
	fputs("<table><thead>\n<tr><td>Commit message</td><td>Author</td><td align=\"right\">Age</td>"
	      "<td align=\"right\">Files</td><td align=\"right\">+</td><td align=\"right\">-</td></tr>\n</thead><tbody>\n", fp);
	while (!git_revwalk_next(&id, w)) {
		/* DEBUG */
		if (i++ > 100)
			break;

		relpath = "";

		if (git_commit_lookup(&commit, repo, &id))
			return 1; /* TODO: error */
		if ((error = git_commit_parent(&parent, commit, 0)))
			continue; /* TODO: handle error */
		if ((error = git_commit_tree(&commit_tree, commit)))
			continue; /* TODO: handle error */
		if ((error = git_commit_tree(&parent_tree, parent)))
			continue; /* TODO: handle error */
		if ((error = git_diff_tree_to_tree(&diff, repo, commit_tree, parent_tree, NULL)))
			continue; /* TODO: handle error */
		if (git_diff_get_stats(&stats, diff))
			continue; /* TODO: handle error */

		git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));

		ndel = git_diff_stats_deletions(stats);
		nadd = git_diff_stats_insertions(stats);
		nfiles = git_diff_stats_files_changed(stats);

		/* TODO: show tag when commit has it */

		/* TODO: collect stats per author and make stats.html page */
		author = git_commit_author(commit);
		summary = git_commit_summary(commit);

		fputs("<tr><td>", fp);
		if (summary) {
			fprintf(fp, "<a href=\"%scommit/%s.html\">", relpath, buf);
			xmlencode(fp, summary, strlen(summary));
			fputs("</a>", fp);
		}
		fputs("</td><td>", fp);
		if (author)
			xmlencode(fp, author->name, strlen(author->name));
		fputs("</td><td align=\"right\">", fp);
		printtime(fp, &author->when);
		fputs("</td><td align=\"right\">", fp);
		fprintf(fp, "%zu", nfiles);
		fputs("</td><td align=\"right\">", fp);
		fprintf(fp, "+%zu", nadd);
		fputs("</td><td align=\"right\">", fp);
		fprintf(fp, "-%zu", ndel);
		fputs("</td></tr>\n", fp);

		relpath = "../";
		printshowfile(commit);

		git_diff_free(diff);
		git_commit_free(commit);
	}
	fprintf(fp, "</tbody></table>");
	git_revwalk_free(w);
	relpath = "";

	return 0;
}

void
printcommitatom(FILE *fp, git_commit *commit)
{
	const git_signature *sig;
	char buf[GIT_OID_HEXSZ + 1];
	int i, count;
	const char *scan, *eol, *summary;

	fputs("<entry>\n", fp);

	/* TODO: show tag when commit has it */
	git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));
	fprintf(fp, "<id>%s</id>\n", buf);

	sig = git_commit_author(commit);

	if (sig) {
		fputs("<updated>", fp);
		printtimez(fp, &sig->when);
		fputs("</updated>\n", fp);
	}

	if ((summary = git_commit_summary(commit))) {
		fputs("<title type=\"text\">", fp);
		xmlencode(fp, summary, strlen(summary));
		fputs("</title>\n", fp);
	}

	fputs("<content type=\"text\">", fp);
	fprintf(fp, "commit %s\n", buf);
	if (git_oid_tostr(buf, sizeof(buf), git_commit_parent_id(commit, 0)))
		fprintf(fp, "parent %s\n", buf);

	if ((count = (int)git_commit_parentcount(commit)) > 1) {
		fprintf(fp, "Merge:");
		for (i = 0; i < count; ++i) {
			git_oid_tostr(buf, 8, git_commit_parent_id(commit, i));
			fprintf(fp, " %s", buf);
		}
		fputc('\n', fp);
	}

	if (sig) {
		fprintf(fp, "Author: ");
		xmlencode(fp, sig->name, strlen(sig->name));
		fprintf(fp, " &lt;");
		xmlencode(fp, sig->email, strlen(sig->email));
		fprintf(fp, "&gt;\nDate:   ");
		printtime(fp, &sig->when);
	}
	fputc('\n', fp);

	for (scan = git_commit_message(commit); scan && *scan;) {
		for (eol = scan; *eol && *eol != '\n'; ++eol)	/* find eol */
			;

		fprintf(fp, "    %.*s\n", (int) (eol - scan), scan);
		scan = *eol ? eol + 1 : NULL;
	}
	fputc('\n', fp);
	fputs("</content>\n", fp);
	if (sig) {
		fputs("<author><name>", fp);
		xmlencode(fp, sig->name, strlen(sig->name));
		fputs("</name>\n<email>", fp);
		xmlencode(fp, sig->email, strlen(sig->email));
		fputs("</email>\n</author>\n", fp);
	}
	fputs("</entry>\n", fp);
}

int
writeatom(FILE *fp)
{
	git_revwalk *w = NULL;
	git_oid id;
	git_commit *c = NULL;
	size_t i, m = 100; /* max */

	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
	fputs("<feed xmlns=\"http://www.w3.org/2005/Atom\">\n<title>", fp);
	xmlencode(fp, name, strlen(name));
	fputs(", branch master</title>\n<subtitle>", fp);

	xmlencode(fp, description, strlen(description));
	fputs("</subtitle>\n", fp);

	git_revwalk_new(&w, repo);
	git_revwalk_push_head(w);

	for (i = 0; i < m && !git_revwalk_next(&id, w); i++) {
		if (git_commit_lookup(&c, repo, &id))
			return 1; /* TODO: error */
		printcommitatom(fp, c);
		git_commit_free(c);
	}
	git_revwalk_free(w);

	fputs("</feed>", fp);

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
	fputs("<table><thead>\n<tr><td>Mode</td><td>Name</td><td align=\"right\">Size</td></tr>\n</thead><tbody>\n", fp);

	for (i = 0; i < count; i++) {
		entry = git_index_get_byindex(index, i);
		fputs("<tr><td>", fp);
		fprintf(fp, "%u", entry->mode); /* TODO: fancy print, like: "-rw-r--r--" */
		fprintf(fp, "</td><td><a href=\"%sfile/", relpath);
		xmlencode(fp, entry->path, strlen(entry->path));
		fputs("\">", fp);
		xmlencode(fp, entry->path, strlen(entry->path));
		fputs("</a></td><td align=\"right\">", fp);
		fprintf(fp, "%" PRIu64, entry->file_size);
		fputs("</td></tr>\n", fp);
	}
	fputs("</tbody></table>", fp);

	return 0;
}

void
writeblobhtml(FILE *fp, const git_blob *blob)
{
	xmlencode(fp, git_blob_rawcontent(blob), (size_t)git_blob_rawsize(blob));
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

	if ((status = git_repository_open(&repo, repodir)) < 0) {
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
	if (!git_revparse_single(&obj, repo, "HEAD:LICENSE"))
		haslicense = 1;
	/* read README */
	if (!git_revparse_single(&obj, repo, "HEAD:README"))
		hasreadme = 1;

	/* read LICENSE */
	if (!git_revparse_single(&obj, repo, "HEAD:LICENSE")) {
		fp = efopen("license.html", "w+b");
		writeheader(fp);
		writeblobhtml(fp, (git_blob *)obj);
		if (ferror(fp))
			err(1, "fwrite");
		writefooter(fp);

		fclose(fp);
	}

	/* read README */
	if (!git_revparse_single(&obj, repo, "HEAD:README")) {
		fp = efopen("readme.html", "w+b");
		writeheader(fp);
		writeblobhtml(fp, (git_blob *)obj);
		if (ferror(fp))
			err(1, "fwrite");
		writefooter(fp);
		fclose(fp);
	}

	fp = efopen("log.html", "w+b");
	writeheader(fp);
	writelog(fp);
	writefooter(fp);
	fclose(fp);

	fp = efopen("files.html", "w+b");
	writeheader(fp);
	writefiles(fp);
	writefooter(fp);
	fclose(fp);

	/* Atom feed */
	fp = efopen("atom.xml", "w+b");
	writeatom(fp);
	fclose(fp);

	/* cleanup */
	git_repository_free(repo);
	git_libgit2_shutdown();

	return 0;
}
