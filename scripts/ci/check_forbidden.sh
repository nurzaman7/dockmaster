#!/bin/sh
# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0
#
# Spec section 15: the core never allocates from the heap, never submits to
# the system workqueue and never calls printk.

set -eu
cd "$(dirname "$0")/../.."

pattern='\b(k_malloc|k_calloc|k_realloc|k_work_submit|k_work_schedule|k_work_reschedule|printk)[[:space:]]*\('
if grep -rnE "$pattern" subsys/dockmaster; then
	echo "error: forbidden call in subsys/dockmaster (see spec section 15)" >&2
	exit 1
fi
echo "ok: no heap, system workqueue or printk calls in subsys/dockmaster"
