#!/usr/bin/env bash
# Maintainer helper, sourced by the build scripts: reports what a build left
# in dist/.
#
# dist/ is reused across builds, so packages of an older version stay there
# until someone removes them. A build that ended by listing every file of its
# kind invited attaching one of those to a release, so each build lists only
# the files carrying its own version and names the leftovers as leftovers.

# dist_report <repo> <version> <extension>
dist_report() {
    local repo="$1" version="$2" ext="$3"
    local current=() stale=() f

    shopt -s nullglob
    for f in "$repo"/dist/*."$ext"; do
        case "${f##*/}" in
            *"$version"*) current+=("$f") ;;
            *)            stale+=("$f") ;;
        esac
    done
    shopt -u nullglob

    if [ ${#current[@]} -eq 0 ]; then
        echo "No .$ext of version $version in dist/" >&2
        return 1
    fi

    (cd "$repo/dist" && sha256sum "${current[@]##*/}")

    if [ ${#stale[@]} -gt 0 ]; then
        echo >&2
        echo "Note: dist/ also holds .$ext files of another version; the release takes none of these:" >&2
        printf '  %s\n' "${stale[@]##*/}" >&2
    fi
}
