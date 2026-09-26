/* wakes-sp1: eat the release of a button whose HOLD already did something.
 *
 * Unpatch (M4e) is "••" + Tn held 2 s, and the same Tn's ordinary shift action fires on
 * RELEASE. When the hold commits, that release must do nothing (issue #2).
 *
 * ⚠️ The guard lives until the button is seen UP, not until the end of the tick. The
 * hold commits while the finger is still down, and the release arrives any number of
 * ticks later. M4e cleared its flag at the end of every tick, so it was already gone by
 * the time the release it existed for reached the handlers -- it never ate anything.
 *
 * Use, once per control tick, after sp1_controls_scan():
 *   1. sp1_rg_arm(g, btn)             when the hold commits
 *   2. sp1_rg_eats(g, btn)            in every release handler for that button
 *   3. sp1_rg_tick_end(g, held(btn))  AFTER the handlers, so the release tick is eaten
 *
 * Pure C, header-only, no Zephyr: tools/host-tests/uitest.c drives it tick by tick. */
#ifndef SP1_RELEASE_GUARD_H_
#define SP1_RELEASE_GUARD_H_

#include <stdbool.h>

struct sp1_release_guard {
	int btn;        /* the button whose release is eaten, or -1 */
};

static inline void sp1_rg_init(struct sp1_release_guard *g)
{
	g->btn = -1;
}

static inline void sp1_rg_arm(struct sp1_release_guard *g, int btn)
{
	g->btn = btn;
}

/* True while `btn`'s release must be ignored. Only the button that was armed: another
 * button's release is its own. */
static inline bool sp1_rg_eats(const struct sp1_release_guard *g, int btn)
{
	return g->btn >= 0 && g->btn == btn;
}

/* The armed button (for the caller to ask whether it is still held), or -1. */
static inline int sp1_rg_button(const struct sp1_release_guard *g)
{
	return g->btn;
}

/* End of tick. `held` = is the armed button still down. Once it is up, the release has
 * been seen by this tick's handlers, and the guard has done its job. */
static inline void sp1_rg_tick_end(struct sp1_release_guard *g, bool held)
{
	if (g->btn >= 0 && !held) {
		g->btn = -1;
	}
}

#endif /* SP1_RELEASE_GUARD_H_ */
