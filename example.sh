#!/bin/sh
# - Makes index for repositories in a single directory.
# - Makes static pages for each repository directory.
#
# NOTE, things to do manually (once):
# - copy style.css, logo.png and favicon.png manually, a style.css example
#   is included.
# - write clone url, for example "git://git.codemadness.org/dir" to the "url"
#   file for each repo.
#
# Usage:
# - mkdir -p htmldir && cd htmldir
# - sh example.sh

reposdir="/var/www/domains/git.codemadness.nl/home/src"
curdir=$(pwd)

# make index.
cd "${reposdir}"
find . -maxdepth 1 -type d | grep -v "^.$" | sort | xargs stagit-index |
        sed 's@<td>Last commit</td>@<td><a href="index-time.html">Last commit</a></td>@g' | \
        sed 's@<td>Name</td>@<td><a href="index.html">Name</a></td>@g' > "${curdir}/index.html"

# make index (sort by last commit author time).
find . -maxdepth 1 -type d | grep -v "^.$" | while read -r dir; do
	d=$(basename "${dir}")
	cd "${reposdir}/${d}"
	timestamp=$(git show -s --pretty="format:%at" || true)

	printf "%d %s\n" "${timestamp}" "${d}"
done | sort -n -k 1 | cut -f 2- -d ' ' | xargs stagit-index | \
	sed 's@<td>Last commit</td>@<td><a href="index-time.html">Last commit</a></td>@g' | \
	sed 's@<td>Name</td>@<td><a href="index.html">Name</a></td>@g' > "${curdir}/index-time.html"

# make files per repo.
cd "${reposdir}"
find . -maxdepth 1 -type d | grep -v "^.$" | sort | while read -r dir; do
	d=$(basename "${dir}")
	printf "%s..." "${d}"

	mkdir -p "${curdir}/${d}"
	cd "${curdir}/${d}"
	stagit "${reposdir}/${d}"

	# symlinks
	ln -sf log.html index.html
	ln -sf ../style.css style.css
	ln -sf ../logo.png logo.png
	ln -sf ../favicon.png favicon.png

	printf " done\n"
done
