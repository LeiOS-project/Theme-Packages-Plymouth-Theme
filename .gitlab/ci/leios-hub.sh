#!/bin/bash
# Release checks and publishing to the LeiOS Hub
# (API: https://api.leios.dev/docs/v1/openapi).
#
#   bash .gitlab/ci/leios-hub.sh check
#       Stops unless the tag (vVERSION) matches the latest debian/changelog
#       entry, and VERSION is not on the hub yet and newer than every release
#       there.
#   bash .gitlab/ci/leios-hub.sh publish <deb>...
#       Repeats the checks, verifies the built packages, then creates the
#       release on the hub (changelog: the latest debian/changelog entry) and
#       uploads the packages to it.
#
# Needs dpkg-dev, curl, jq and ca-certificates. publish also needs the CI
# variable LEIOS_API_KEY: an API key of a hub user who may publish releases of
# this package.

set -euo pipefail

LEIOS_API_URL="${LEIOS_API_URL:-https://api.leios.dev/v1}"

RESPONSE=$(mktemp)
trap 'rm -f "$RESPONSE"' EXIT

fail() {
	echo "ERROR: $*" >&2
	exit 1
}

# api METHOD PATH [CURL-ARGS...]: calls the LeiOS API. The response body ends
# up in $RESPONSE and the HTTP status in $HTTP_CODE (000 if no response).
api() {
	local method=$1 path=$2
	shift 2
	local args=(--silent --show-error --request "$method" --output "$RESPONSE" --write-out '%{http_code}')
	if [ "$method" = GET ]; then
		args+=(--retry 3)
	else
		args+=(--header "Authorization: Bearer ${LEIOS_API_KEY}")
	fi
	HTTP_CODE=$(curl "${args[@]}" "$@" "${LEIOS_API_URL}${path}") || HTTP_CODE=000
}

api_error() {
	local reason
	reason=$(jq -r '.message // (.error // empty | tostring)' "$RESPONSE" 2>/dev/null) || reason=$(head -c 300 "$RESPONSE")
	echo "ERROR: $1 (HTTP $HTTP_CODE${reason:+: $reason})" >&2
}

api_fail() {
	api_error "$1"
	exit 1
}

# The hub package name is the binary package name.
mapfile -t packages < <(sed -n 's/^Package:[[:space:]]*//p' debian/control)
[ "${#packages[@]}" -eq 1 ] || fail "debian/control has to define exactly one binary package"
PACKAGE=${packages[0]}

check() {
	[ -n "${CI_COMMIT_TAG:-}" ] || fail "CI_COMMIT_TAG is not set; releases are made from tags"
	TAG_VERSION=${CI_COMMIT_TAG#v}

	local changelog_version distribution
	changelog_version=$(dpkg-parsechangelog -S Version)
	distribution=$(dpkg-parsechangelog -S Distribution)
	echo "Package:            $PACKAGE"
	echo "Tag version:        $TAG_VERSION"
	echo "Changelog version:  $changelog_version ($distribution)"
	[ "$TAG_VERSION" = "$changelog_version" ] ||
		fail "tag $CI_COMMIT_TAG does not match the latest debian/changelog entry ($changelog_version)"
	[ "$distribution" != UNRELEASED ] ||
		fail "the latest debian/changelog entry is still UNRELEASED"

	api GET "/packages/$PACKAGE/releases"
	[ "$HTTP_CODE" = 200 ] || api_fail "could not list the releases of $PACKAGE on the LeiOS Hub"
	local released version
	mapfile -t released < <(jq -r '.data[].version_with_leios_patch' "$RESPONSE")
	for version in "${released[@]}"; do
		[ "$version" != "$TAG_VERSION" ] ||
			fail "$PACKAGE $TAG_VERSION already exists on the LeiOS Hub"
		dpkg --compare-versions "$TAG_VERSION" gt "$version" ||
			fail "$TAG_VERSION is not newer than $version, which is already on the LeiOS Hub"
	done
	echo "LeiOS Hub releases: ${released[*]:-none}"
	echo "$TAG_VERSION is new and newer than every release on the LeiOS Hub."
}

# The hub checks Package, Version, Architecture and Maintainer of an upload
# only after the release has been created; check them before that, so a
# mismatch cannot leave an empty release behind.
verify_debs() {
	local name publisher maintainer deb arch field expected actual

	api GET "/packages/$PACKAGE"
	[ "$HTTP_CODE" = 200 ] || api_fail "could not look up $PACKAGE on the LeiOS Hub"
	name=$(jq -r .data.name "$RESPONSE")
	# The hub's full package name is <publisher>.<name>.
	publisher=${PACKAGE%".$name"}
	api GET "/publishers/$publisher"
	[ "$HTTP_CODE" = 200 ] || api_fail "could not look up the publisher $publisher on the LeiOS Hub"
	maintainer=$(jq -r '.data | "\(.maintainer_contact_name) <\(.maintainer_contact_email)>"' "$RESPONSE")

	for deb in "$@"; do
		[ -f "$deb" ] || fail "$deb does not exist"
		arch=$(dpkg-deb --field "$deb" Architecture)
		case $arch in
			all | amd64 | arm64) ;;
			*) fail "$deb: the LeiOS Hub does not take packages for $arch" ;;
		esac
		for field in Package Version Maintainer; do
			case $field in
				Package) expected=$PACKAGE ;;
				Version) expected=$TAG_VERSION ;;
				Maintainer) expected=$maintainer ;;
			esac
			actual=$(dpkg-deb --field "$deb" "$field")
			[ "$actual" = "$expected" ] ||
				fail "$deb: $field is \"$actual\", the LeiOS Hub expects \"$expected\""
		done
		echo "Verified $(basename "$deb"): $PACKAGE $TAG_VERSION ($arch), $maintainer"
	done
}

# An empty release blocks its version: "check" stops on it. Delete it again
# as long as no package has been uploaded to it.
remove_empty_release() {
	api GET "/packages/$PACKAGE/releases/$TAG_VERSION"
	if [ "$HTTP_CODE" = 200 ] && jq -e '[.data.architectures[]] | any | not' "$RESPONSE" > /dev/null; then
		api DELETE "/packages/$PACKAGE/releases/$TAG_VERSION"
		if [ "$HTTP_CODE" = 200 ]; then
			echo "Deleted the empty release $TAG_VERSION from the LeiOS Hub again; the pipeline can be retried." >&2
			return
		fi
		api_error "could not delete the empty release $TAG_VERSION"
	fi
	echo "Release $TAG_VERSION is left on the LeiOS Hub as it is; check it there before retrying." >&2
}

publish() {
	[ -n "${LEIOS_API_KEY:-}" ] || fail "the CI variable LEIOS_API_KEY is not set"
	[ "$#" -gt 0 ] || fail "no packages to publish"

	check
	verify_debs "$@"

	local changelog deb
	changelog=$(dpkg-parsechangelog -S Changes | sed '0,/^\.$/d; s/^\.$//; s/^  //')
	[ -n "$changelog" ] || fail "the latest debian/changelog entry is empty"
	[ "${#changelog}" -le 10000 ] || fail "the latest debian/changelog entry is longer than the 10000 characters the LeiOS Hub takes"

	echo "Creating release $TAG_VERSION of $PACKAGE on the LeiOS Hub ..."
	api POST "/packages/$PACKAGE/releases" \
		--header 'Content-Type: application/json' \
		--data "$(jq -n --arg version "$TAG_VERSION" --arg changelog "$changelog" \
			'{version_with_leios_patch: $version, changelog: $changelog}')"
	[ "$HTTP_CODE" = 201 ] || api_fail "could not create release $TAG_VERSION of $PACKAGE"

	for deb in "$@"; do
		echo "Uploading $(basename "$deb") ..."
		api POST "/packages/$PACKAGE/releases/$TAG_VERSION/$(dpkg-deb --field "$deb" Architecture)" \
			--form "file=@$deb"
		if [ "$HTTP_CODE" != 201 ]; then
			api_error "could not upload $(basename "$deb")"
			remove_empty_release
			exit 1
		fi
	done
	echo "Published $PACKAGE $TAG_VERSION to the testing repository of the LeiOS Hub."
}

case ${1:-} in
	check) check ;;
	publish)
		shift
		publish "$@"
		;;
	*)
		echo "usage: $0 check | publish <deb>..." >&2
		exit 2
		;;
esac
