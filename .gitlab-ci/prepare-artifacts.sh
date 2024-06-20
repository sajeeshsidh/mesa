#!/usr/bin/env bash
# shellcheck disable=SC2038 # TODO: rewrite the find
# shellcheck disable=SC2086 # we want word splitting

section_switch prepare-artifacts "artifacts: prepare"

set -e
set -o xtrace

_STRIPPED=

strip_debug () {
    if [ -z "$_STRIPPED" ]; then
      find ./install -name "*.so" -exec $STRIP {} \;
    fi
}


CROSS_FILE=/cross_file-"$CROSS".txt

# Delete unused bin and includes from artifacts to save space.
rm -rf install/bin install/include

# Strip the drivers in the artifacts to cut 80% of the artifacts size.
if [ -n "$CROSS" ]; then
    STRIP=$(sed -n -E "s/strip\s*=\s*\[?'(.*)'\]?/\1/p" "$CROSS_FILE")
    if [ -z "$STRIP" ]; then
        echo "Failed to find strip command in cross file"
        exit 1
    fi
else
    STRIP="strip"
fi

[ -z "$ARTIFACTS_DEBUG_SYMBOLS" ] && strip_debug

# Test runs don't pull down the git tree, so put the dEQP helper
# script and associated bits there.
echo "$(cat VERSION) (git-$(git rev-parse HEAD | cut -b -10))" > install/VERSION
cp -Rp .gitlab-ci/bare-metal install/
cp -Rp .gitlab-ci/common install/
cp -Rp .gitlab-ci/piglit install/
cp -Rp .gitlab-ci/fossils.yml install/
cp -Rp .gitlab-ci/fossils install/
cp -Rp .gitlab-ci/fossilize-runner.sh install/
cp -Rp .gitlab-ci/crosvm-init.sh install/
cp -Rp .gitlab-ci/*.txt install/
cp -Rp .gitlab-ci/report-flakes.py install/
cp -Rp .gitlab-ci/vkd3d-proton install/
cp -Rp .gitlab-ci/setup-test-env.sh install/
cp -Rp .gitlab-ci/*-runner.sh install/
cp -Rp .gitlab-ci/bin/structured_logger.py install/
cp -Rp .gitlab-ci/bin/custom_logger.py install/
find . -path \*/ci/\*.txt \
    -o -path \*/ci/\*.toml \
    -o -path \*/ci/\*traces\*.yml \
    | xargs -I '{}' cp -p '{}' install/

# Tar up the install dir so that symlinks and hardlinks aren't each
# packed separately in the zip file.
mkdir -p artifacts/

cp -Rp .gitlab-ci/common artifacts/ci-common
cp -Rp .gitlab-ci/lava artifacts/
cp -Rp .gitlab-ci/b2c artifacts/

if [ -n "$S3_ARTIFACT_NAME" ]; then
    # Pass needed files to the test stage
    S3_ARTIFACT_NAME_UN="$S3_ARTIFACT_NAME-unstripped.tar.zst"
    S3_ARTIFACT_NAME_STR="$S3_ARTIFACT_NAME.tar.zst"
    zstd artifacts/install.tar -o ${S3_ARTIFACT_NAME}

    if [[ -n "$ARTIFACTS_DEBUG_SYMBOLS" ]] && [[ "${BUILDTYPE:-debug}" == *"debug"* ]]; then
	pushd _build
	find ./src -name '*.dwo' -print0 | xargs -0 dwp -o debug.dwp
	ci-fairy s3cp --token-file "${S3_JWT_FILE}" debug.dwp https://${PIPELINE_ARTIFACTS_BASE}/debug.dwp
	popd
    fi

    strip_debug  # never distribute debug symbols to the runners
    tar --zstd -cf "${S3_ARTIFACT_NAME_STR}" install
    ci-fairy s3cp --token-file "${S3_JWT_FILE}" ${S3_ARTIFACT_NAME} https://${PIPELINE_ARTIFACTS_BASE}/${S3_ARTIFACT_NAME_STR}
else
    tar -cf artifacts/install.tar install
fi

section_end prepare-artifacts
