#!/usr/bin/env bash

# Install fluster in /usr/local.

git clone https://github.com/fluendo/fluster.git --single-branch --no-checkout

pushd fluster
git checkout ${FLUSTER_REVISION}
popd

if [ "${SKIP_UPDATE_FLUSTER_VECTORS}" != 1 ]; then
    # Download the necessary vectors
    fluster/fluster.py download JCT-VC-HEVC_V1 JCT-VC-SCC JCT-VC-RExt JCT-VC-MV-HEVC JVT-AVC_V1 JVT-FR-EXT VP9-TEST-VECTORS VP9-TEST-VECTORS-HIGH

    # Build fluster vectors archive and upload it
    tar --zstd -cf "vectors.tar.zst" fluster/resources/
    ci-fairy s3cp --token-file "${S3_JWT_FILE}" "vectors.tar.zst" \
          "https://${S3_PATH_FLUSTER}/vectors.tar.zst"

    touch /lava-files/done
    ci-fairy s3cp --token-file "${S3_JWT_FILE}" /lava-files/done https://${S3_PATH_FLUSTER}/done

    # Don't include the vectors in the rootfs
    rm -fr fluster/resources/*
fi

mkdir -p $ROOTFS/usr/local/
mv fluster $ROOTFS/usr/local/

