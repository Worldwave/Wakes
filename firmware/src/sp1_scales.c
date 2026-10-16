/* wakes-sp1 — the scale bank (#44). See sp1_scales.h. */
#include "sp1_scales.h"
#include "sp1_scales_gen.h"

#include <string.h>

_Static_assert(SP1_SCALES_GEN_SLOTS == SP1_SCALE_SLOTS, "scales.cnfg has 7 slots");
_Static_assert(SP1_SCALES_GEN_DEGREES == SP1_SCALE_DEGREES, "16 degrees a scale");

static const struct sp1_scale_bank builtin = SP1_SCALES_DEFAULTS;

static struct sp1_scale_bank banks[2] = { SP1_SCALES_DEFAULTS, SP1_SCALES_DEFAULTS };
static int      live;
static uint32_t gen;

static const struct sp1_scale_bank *bank(void)
{
	return &banks[__atomic_load_n(&live, __ATOMIC_ACQUIRE) & 1];
}

void sp1_scales_defaults(struct sp1_scale_bank *b)
{
	memcpy(b, &builtin, sizeof(*b));
}

void sp1_scales_install(const struct sp1_scale_bank *b)
{
	const int next = (__atomic_load_n(&live, __ATOMIC_ACQUIRE) & 1) ^ 1;
	bool any = false;
	for (int i = 0; i < SP1_SCALE_SLOTS; i++) {
		any = any || b->s[i].n != 0u;
	}
	memcpy(&banks[next], any ? b : &builtin, sizeof(banks[next]));
	__atomic_store_n(&live, next, __ATOMIC_RELEASE);
	__atomic_store_n(&gen, __atomic_load_n(&gen, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
}

uint32_t sp1_scales_gen(void)
{
	return __atomic_load_n(&gen, __ATOMIC_ACQUIRE);
}

const struct sp1_scale *sp1_scale_at(int slot)
{
	if (slot < 0 || slot >= SP1_SCALE_SLOTS) {
		return NULL;
	}
	const struct sp1_scale *s = &bank()->s[slot];
	return s->n != 0u ? s : NULL;
}

bool sp1_scale_filled(int slot)
{
	return sp1_scale_at(slot) != NULL;
}

int sp1_scale_first(void)
{
	for (int i = 0; i < SP1_SCALE_SLOTS; i++) {
		if (sp1_scale_filled(i)) {
			return i;
		}
	}
	return 0;                  /* never: install() refuses an empty bank */
}

const char *sp1_scale_name(int slot)
{
	const struct sp1_scale *s = sp1_scale_at(slot);
	return s != NULL ? s->name : "";
}
