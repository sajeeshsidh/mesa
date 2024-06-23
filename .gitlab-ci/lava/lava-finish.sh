#!/usr/bin/env bash
# cleanup in case job gets cancelled

[ ! -f results/lava-job_detail.json ] && exit 0

LAVA_JOB_ID=$(jq '.["dut_jobs"][].lava_job_id' results/lava_job_detail.json)

PYTHONPATH=artifacts/ artifacts/lava/lava_job_submitter.py \
	cancel --lava-job-id "${LAVA_JOB_ID}"
