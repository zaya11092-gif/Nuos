// nuos_posed — Monado xrt_device driver: null HMD with the pose injected from
// /dev/shm/nuos_pose (seqlock, written by nuos-tracker). Best-effort: if the
// shm is absent the device reports identity pose (flat mode keeps working).
//
// This is a minimal driver following Monado's driver conventions; it is
// compiled as part of a patched Monado tree (see build-monado.sh).

#include "xrt/xrt_device.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_time.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "nuos_shm.h" // from proto spine; copied next to this file by build script

struct nuos_posed_device {
	struct xrt_device base;
	int shm_fd;
	struct nuos_pose_shm *shm;
};

static inline struct nuos_posed_device *
nuos_posed(struct xrt_device *xdev)
{
	return (struct nuos_posed_device *)xdev;
}

static void
nuos_posed_destroy(struct xrt_device *xdev)
{
	struct nuos_posed_device *d = nuos_posed(xdev);
	if (d->shm)
		munmap(d->shm, sizeof(struct nuos_pose_shm));
	if (d->shm_fd >= 0)
		close(d->shm_fd);
	free(d);
}

static bool
read_pose(struct nuos_posed_device *d, struct nuos_pose_shm *out)
{
	if (!d->shm || d->shm->magic != NUOS_SHM_MAGIC_POSE)
		return false;
	for (int tries = 0; tries < 8; tries++) {
		uint32_t s1 = nuos_seqlock_begin_read(&d->shm->seq);
		*out = *d->shm;
		if (nuos_seqlock_end_read(&d->shm->seq, s1) == 0)
			return true;
	}
	return false;
}

static xrt_result_t
nuos_posed_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            uint64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct nuos_posed_device *d = nuos_posed(xdev);
	struct nuos_pose_shm p;

	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID |
	    XRT_SPACE_RELATION_POSITION_VALID);

	if (read_pose(d, &p)) {
		out_relation->pose.orientation.w = p.quat[0];
		out_relation->pose.orientation.x = p.quat[1];
		out_relation->pose.orientation.y = p.quat[2];
		out_relation->pose.orientation.z = p.quat[3];
		out_relation->pose.position.x = p.pos[0];
		out_relation->pose.position.y = p.pos[1];
		out_relation->pose.position.z = p.pos[2];
	} else {
		// identity pose when tracker shm absent (graceful)
		out_relation->pose.orientation.w = 1.0f;
	}
	return XRT_SUCCESS;
}

struct xrt_device *
nuos_posed_create(int index)
{
	struct nuos_posed_device *d = U_CALLOC_WITH_CAST(struct nuos_posed_device, 1);
	if (!d)
		return NULL;

	d->shm_fd = shm_open(NUOS_SHM_POSE_NAME, O_RDONLY, 0);
	if (d->shm_fd >= 0) {
		d->shm = mmap(NULL, sizeof(struct nuos_pose_shm), PROT_READ,
		              MAP_SHARED, d->shm_fd, 0);
		if (d->shm == MAP_FAILED)
			d->shm = NULL;
		U_LOG(E_INFO, "nuos_posed: reading %s", NUOS_SHM_POSE_NAME);
	} else {
		U_LOG(E_INFO, "nuos_posed: %s absent, identity pose",
		      NUOS_SHM_POSE_NAME);
	}

	struct xrt_device *xd = &d->base;
	snprintf(xd->str, sizeof(xd->str), "NuOS Null HMD (posed)");
	snprintf(xd->serial, sizeof(xd->serial), "NUOS-POSED-%d", index);
	xd->device_type = XRT_DEVICE_TYPE_HMD;
	xd->name = XRT_GENERIC_NULL;
	xd->purchase_id = NULL;
	xd->hmd.vfov_min_and_max[0] = -48.0f; /* horizontal min/max, degrees */
	xd->hmd.vfov_min_and_max[1] = 48.0f;
	xd->hmd.vfov_min_and_max[2] = -25.0f; /* vertical */
	xd->hmd.vfov_min_and_max[3] = 25.0f;
	xd->hmd.blend_mode_count = 1;
	xd->hmd.blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	xd->hmd.screen_count = 2;
	xd->hmd.screens[0].w_pixels = 1280;
	xd->hmd.screens[0].h_pixels = 720;
	xd->hmd.screens[1].w_pixels = 1280;
	xd->hmd.screens[1].h_pixels = 720;
	xd->hmd.lens_count = 2;
	for (int e = 0; e < 2; e++) {
		xd->compute_distortion = NULL; // identity distortion
		xd->hmd.lens_centre[e].x_pixels = 640.0f;
		xd->hmd.lens_centre[e].y_pixels = 360.0f;
		xd->hmd.ipyd = 0.063f;
	}

	// one HMD pose input
	xd->num_inputs = 1;
	xd->inputs[0].type = XRT_INPUT_GENERIC_HEAD_POSE;
	xd->inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	snprintf(xd->inputs[0].name_string, sizeof(xd->inputs[0].name_string),
	         "head");

	xd->get_tracked_pose = nuos_posed_get_tracked_pose;
	xd->destroy = nuos_posed_destroy;

	return xd;
}
