#ifndef AZ_OUTPUT_H
#define AZ_OUTPUT_H

/*
 * One place a frame is built.
 *
 * Before this file, asteroidz called wlr_scene_output_build_state() from four
 * places -- the ordinary frame, the screenshot capture, the HDR pending-change
 * fold-in, and the tearing path -- each with its own colour-transform
 * expression and its own error handling. That is four places a new renderer
 * would have to be taught about, and four places for them to disagree.
 *
 * az_output_build_frame() is the single seam. Which engine builds the frame is
 * decided here and nowhere else, and the callers keep doing what they were
 * doing with the resulting wlr_output_state.
 */

struct az_frame_options {
	/* The colour transform the frame should be built with, exactly as the
	 * caller would have put it in wlr_scene_output_state_options. */
	struct wlr_color_transform *color_transform;
};

/*
 * Build one frame of `m` into `state`.
 *
 * Returns false if no frame could be built, which the callers treat the same
 * way they always have.
 *
 * An AVK-mode output that cannot be composited by AVK no longer falls back:
 * it aborts, here or inside az_avk_build_frame(), naming the reason. A frame
 * that silently came from the other renderer is indistinguishable from a
 * correct one until something else goes wrong, and by then the frame that
 * caused it is long gone.
 */
static inline bool az_output_build_frame(Monitor *m,
		struct wlr_output_state *state, const struct az_frame_options *opts) {
	if (!avk_device_lost() &&
			az_avk_build_frame(m, state, opts->color_transform)) {
		return true;
	}
	/*
	 * ── A LOST DEVICE IS NOT A REFUSAL ───────────────────────────────────
	 *
	 * The abort below exists to catch a bug in AVK. A device loss is not one:
	 * the GPU was reset out from under this process, usually because some
	 * other client hung it, and the driver says so in as many words -- radv
	 * logs "The CS has been cancelled because the context is lost. This
	 * context is innocent." Aborting on it reports a fault in the one piece
	 * of software that did nothing wrong, and takes the session, every client
	 * and their unsaved work with it.
	 *
	 * There is still no frame to build. A reset loses VRAM, so every imported
	 * client image, every pipeline and every cache belongs to a device that no
	 * longer exists, and nothing can be rendered until the device is rebuilt
	 * -- which this does not yet do. So end the session the way a session
	 * ends: wl_display_terminate, clients disconnected, teardown run. That is
	 * not a recovery, and it is not meant to look like one; it is the
	 * difference between a compositor that exits when its GPU disappears and
	 * one that SIGABRTs mid-frame with the damage ring half rotated.
	 *
	 * Announced once. Every output reaches this on the same frame, and a
	 * terminate already in flight does not need repeating.
	 */
	if (avk_device_lost()) {
		static bool announced = false;
		if (!announced) {
			announced = true;
			wlr_log(WLR_ERROR,
				"the GPU was lost (VK_ERROR_DEVICE_LOST) and AVK cannot build "
				"another frame; ending the session.");
			quit_now(NULL);
		}
		return false;
	}
	/*
	 * ── THERE IS NOWHERE ELSE FOR A FRAME TO COME FROM ───────────────────
	 *
	 * az_avk_build_frame() aborts on an output it REFUSES, naming the reason.
	 * It also returns false without refusing anything -- AVK inactive, or the
	 * monitor has no scene output -- and control arrives here.
	 *
	 * This used to fall through to SceneFX on wlroots' GLES renderer, and the
	 * desktop looked fine, which is exactly what made it worth closing: no
	 * warning, no counter, just a frame that quietly came from the other
	 * renderer. That renderer is now gone from the build entirely, so the
	 * question is settled by construction rather than by this check -- but the
	 * check stays, because "AVK declined and nobody noticed" is still a bug and
	 * this is the frame that would prove it.
	 *
	 * No escape variable, for the same reason it is absent in az_avk.h: a
	 * switch that turns the fallback back on is the fallback.
	 */
	wlr_log(WLR_ERROR,
		"AVK declined to build a frame for %s (active=%d, scene_output=%p) "
		"and there is no other compositor to fall back to.",
		m->wlr_output != NULL ? m->wlr_output->name : "(output)",
		(int)avk.active, (void *)m->scene_output);
	abort();
}

/*
 * A commit that was built but did not land.
 *
 * wlr_scene_output_commit() calls wlr_damage_ring_add_whole() when the commit
 * fails, and asteroidz replicates that function by hand -- so it has to
 * replicate this too. Building a frame rotates the damage ring, which records
 * the damage as having been drawn into that buffer. It *was* drawn; the buffer
 * simply never reached the screen. Without trashing the ring, the next frame
 * inherits a region nobody will ever repaint, and the result is a rectangle of
 * stale pixels that survives until something else happens to damage it.
 *
 * This did nothing while AVK redrew everything every frame, which is exactly
 * why it is easy to leave out and hard to find afterwards.
 */
/*
 * ── LOUD ONCE, THEN ONCE A DECADE ─────────────────────────────────────────
 *
 * True at 1, 10, 100, 1000 ... and false in between, for a counter that is
 * incremented once per occurrence. A per-frame wlr_log() of a condition that
 * recurs at frame rate is not a diagnostic: it is a denial of service against
 * the log it is written to, and the one that motivated this wrote 8MB in forty
 * minutes while hiding its own rate inside the timestamps.
 *
 * The first occurrence is still loud, because "it never happens" and "it
 * happens constantly and nobody said" are the two failures worth avoiding, and
 * the exact count lives in the counter the caller already keeps.
 */
static inline bool az_log_decade(uint64_t n) {
	uint64_t d = 1;
	while (d < n) {
		d *= 10;
	}
	return d == n;
}

static inline void az_output_commit_failed(Monitor *m) {
	if (m->scene_output != NULL) {
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
	}
}

/*
 * The colour transform for an ordinary frame on `m`.
 *
 * The expression was written out at each of the four call sites and had to
 * stay in step between them: an output that carries its own image description
 * is already colour-managed by the connector, so applying the ICC transform on
 * top would apply it twice.
 */
/*
 * M6B/G2 adds the second half of the same rule. When C3 derived AZ_TF_LUT1D
 * the AVK encode pass is applying the profile itself, from the same file, so
 * handing wlroots the transform as well would apply it twice -- once as a
 * matrix and curve in the encode pass and once as a 3D LUT in SceneFX. Exactly
 * one owner, and the colour state names which.
 */
static inline struct wlr_color_transform *az_output_color_transform(Monitor *m) {
	if (m->wlr_output->image_description != NULL) {
		return NULL;
	}
	/* M6C adds CLUT3D, where the double application would be even more literal:
	 * the same 3D table, built from the same profile, sampled once in AVK's
	 * encode pass and once in SceneFX's. */
	if (m->color_state.encode_tf == AZ_TF_LUT1D
			|| m->color_state.encode_tf == AZ_TF_CLUT3D) {
		return NULL;
	}
	return m->icc_transform;
}

#endif /* AZ_OUTPUT_H */
