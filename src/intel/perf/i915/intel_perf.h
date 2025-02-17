/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct intel_perf_config;
struct drm_i915_perf_oa_config;

uint64_t i915_perf_get_oa_format(struct intel_perf_config *perf);

int i915_perf_stream_open(struct intel_perf_config *perf_config, int drm_fd,
                          uint32_t ctx_id, uint64_t metrics_set_id,
                          uint64_t report_format, uint64_t period_exponent,
                          bool hold_preemption, bool enable);
int i915_perf_stream_read_samples(int perf_stream_fd, uint8_t *buffer, size_t buffer_len);

struct intel_perf_registers *i915_perf_load_configurations(struct intel_perf_config *perf_cfg, int fd, const char *guid);

bool i915_oa_metrics_available(struct intel_perf_config *perf, int fd, bool use_register_snapshots);
