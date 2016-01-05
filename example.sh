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

set -e

reposdir="/var/www/domains/git.codemadness.nl/home/src/"
curdir=$(pwd)

# make index.
cd "${reposdir}"
find . -maxdepth 1 -type d | grep -v "^.$" | sort | xargs stagit-index > "${curdir}/index.html"

# make files per repo.
find . -maxdepth 1 -type d | grep -v "^.$" | sort | while read -r dir; do
	cd "${reposdir}"
	d=$(basename "${dir}")

	printf "%s..." "${d}"
	cd "${curdir}"

	test -d "${d}" || mkdir -p "${d}"
	cd "${d}"
	stagit "${reposdir}${d}"

	printf " done\n"

	# symlinks
	ln -sf log.html index.html
	ln -sf ../style.css style.css
	ln -sf ../logo.png logo.png
	ln -sf ../favicon.png favicon.png
done
