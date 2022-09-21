/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

#include "kernel/types.h"

CCL_NAMESPACE_BEGIN

struct GuidingParams {
  /* The subset of path guiding parameters that can trigger a creation/rebuild
   * of the guiding field. */
  bool use = false;
  bool use_surface_guiding = false;
  bool use_volume_guiding = false;

  GuidingDistributionType type = GUIDING_TYPE_PARALLAX_AWARE_VMM;
  int training_samples = 128;
  bool deterministic = false;

  GuidingParams() = default;

  bool modified(const GuidingParams &other) const
  {
    return !((use == other.use) && (use_surface_guiding == other.use_surface_guiding) &&
             (use_volume_guiding == other.use_volume_guiding) && (type == other.type) &&
             (training_samples == other.training_samples) &&
             (deterministic == other.deterministic));
  }
};

CCL_NAMESPACE_END
