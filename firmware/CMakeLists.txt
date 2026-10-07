cmake_minimum_required(VERSION 3.20.0)

# ⚠️ NO CCACHE, EVER (issue #22, 2026-09-30). Zephyr wraps every compile in ccache when
# it finds one, and ccache's direct mode reuses an object whenever the source and the
# headers it read LAST time are unchanged. It cannot see a NEW header appearing earlier
# on the include path -- which is exactly what every Plaits override is. When the
# chord_engine.h override was added, voice.cc and string_machine_engine.cc came out of
# the cache still built against upstream's header: two layouts of plaits::Voice in one
# image, memory corrupted in Voice::Init at boot, and the device reset before STANDBY on
# every boot. check_image.py now also compares class layouts across objects.
set(USE_CCACHE 0 CACHE STRING "Never ccache: it cannot see the Plaits header overrides" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wakes-sp1)

# The USB device is ours since M5a (src/sp1_usbd.c, CONFIG_SP1_USB_* in Kconfig): Zephyr's
# sample helper, included here through M4e, hard-codes Zephyr's test vendor ID.

# ---- the engine list: generated from config/engines.csv ----
# The CSV is the USER's choice of engines, slots and glyphs; the facts about each engine
# (its index inside Plaits, its centre detents) live in tools/gen_engines.py and are not
# user-editable. The build fails with a message naming the line if the CSV is malformed.
set(SP1_ENGINES_CSV ${CMAKE_CURRENT_SOURCE_DIR}/../config/engines.csv)
set(SP1_ENGINES_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_engines.py)
set(SP1_ENGINES_H   ${CMAKE_CURRENT_BINARY_DIR}/sp1_generated/sp1_engines_gen.h)
add_custom_command(
	OUTPUT  ${SP1_ENGINES_H}
	COMMAND ${Python3_EXECUTABLE} ${SP1_ENGINES_GEN} ${SP1_ENGINES_CSV} ${SP1_ENGINES_H}
	DEPENDS ${SP1_ENGINES_CSV} ${SP1_ENGINES_GEN}
	COMMENT "Generating the engine table from config/engines.csv"
)
add_custom_target(sp1_engines_gen DEPENDS ${SP1_ENGINES_H})
add_dependencies(app sp1_engines_gen)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sp1_generated)

# ---- the MIDI script (M5a): generated from config/midi.ini ----
# Like the engine list: tools/gen_midi.py fails the build, naming the line, on anything
# invalid. Generated in EVERY configuration, because the UI files read its enum even where
# MIDI is compiled out (sp1_midi.h then stubs the calls). With MIDI in, it also writes this
# build's CC chart next to the firmware: wakes-sp1-midi.md and .csv.
set(SP1_MIDI_INI ${CMAKE_CURRENT_SOURCE_DIR}/../config/midi.ini)
set(SP1_MIDI_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_midi.py)
set(SP1_MIDI_H   ${CMAKE_CURRENT_BINARY_DIR}/sp1_generated/sp1_midi_gen.h)
set(SP1_MIDI_OUT ${SP1_MIDI_H})
set(SP1_MIDI_CHART "")
if(CONFIG_SP1_MIDI)
	set(SP1_CHART_DIR ${CMAKE_BINARY_DIR}/zephyr)
	set(SP1_MIDI_CHART --chart ${SP1_CHART_DIR})
	list(APPEND SP1_MIDI_OUT ${SP1_CHART_DIR}/wakes-sp1-midi.md ${SP1_CHART_DIR}/wakes-sp1-midi.csv)
endif()
add_custom_command(
	OUTPUT  ${SP1_MIDI_OUT}
	COMMAND ${Python3_EXECUTABLE} ${SP1_MIDI_GEN} ${SP1_MIDI_INI} ${SP1_MIDI_H} ${SP1_MIDI_CHART}
	DEPENDS ${SP1_MIDI_INI} ${SP1_MIDI_GEN}
	COMMENT "Generating the MIDI tables from config/midi.ini"
)
add_custom_target(sp1_midi_gen DEPENDS ${SP1_MIDI_OUT})
add_dependencies(app sp1_midi_gen)

# ---- the audio settings: config/audio.ini -> sp1_audio_gen.h (M5c) ----
# How sound leaves the device: USB audio out's level mode and the speaker while a host takes
# it. Same rule as the MIDI script: a bad value fails the build, naming the line.
set(SP1_AUDIO_INI ${CMAKE_CURRENT_SOURCE_DIR}/../config/audio.ini)
set(SP1_AUDIO_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_audio.py)
set(SP1_AUDIO_H   ${CMAKE_CURRENT_BINARY_DIR}/sp1_generated/sp1_audio_gen.h)
add_custom_command(
	OUTPUT  ${SP1_AUDIO_H}
	COMMAND ${Python3_EXECUTABLE} ${SP1_AUDIO_GEN} ${SP1_AUDIO_INI} ${SP1_AUDIO_H}
	DEPENDS ${SP1_AUDIO_INI} ${SP1_AUDIO_GEN}
	COMMENT "Generating the audio settings from config/audio.ini"
)
add_custom_target(sp1_audio_gen DEPENDS ${SP1_AUDIO_H})
add_dependencies(app sp1_audio_gen)

target_sources(app PRIVATE
	src/main.c
	src/sp1_led.c
	src/sp1_power.c
	src/sp1_batt.c
	src/sp1_standby.c
	src/sp1_console.c
	src/sp1_logbuf.c
	src/sp1_threads.c
	src/sp1_usbd.c
	src/sp1_controls.c
	src/sp1_display.c
	src/sp1_calib.c
	src/sp1_audio.c
	src/sp1_meter.c
	src/sp1_playrow.c
	src/sp1_plaits_ui.c
)

# ---- M6 (#43): the eMMC and the FAT volume on it (src/sp1_store.h) ----
if(CONFIG_SP1_STORAGE)
	target_sources(app PRIVATE src/sp1_emmc.c src/sp1_store.c)
	target_sources_ifdef(CONFIG_SP1_STORAGE_TEST app PRIVATE src/sp1_emmc_report.c)
	# ff.h, for FATFS and MKFS_PARM (Zephyr's fatfs module, ELMFAT interface).
	target_link_libraries(app PRIVATE ELMFAT)
endif()

# ---- M3: the Plaits voice (third_party/eurorack, vendored unmodified) ----
# Built as its own Zephyr library so it gets its OWN optimisation flags. The rest of
# the firmware is built -Os (CONFIG_SIZE_OPTIMIZATIONS); the DSP is the one place where
# cycles matter more than bytes, and the audio budget was measured assuming Plaits' own
# build: -O2 -funroll-loops, as in stmlib/makefile.inc for the Cortex-M4. Options added
# here come after the global ones on the command line, so this -O2 wins.
if(CONFIG_SP1_PLAITS)
	set(ER ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/eurorack)

	zephyr_library_named(sp1_plaits)

	# ---- Plaits overrides: trims without modifying the vendored code ----
	# Two kinds, both leave third_party/ untouched:
	#
	# sp1_plaits_override(<file under third_party/eurorack> <exact upstream text>
	#                     <replacement>)                                   (M3e)
	#   Writes a copy of the file with ONE passage replaced into the build directory.
	#   A header copy shadows upstream's through the include path (searched first);
	#   a .cc copy is compiled INSTEAD of upstream's. The build stops if the upstream
	#   text is not found exactly once, so an upstream change can never silently
	#   disable a trim. At most one override per file.
	#
	# sp1_plaits_replace(<file under third_party/eurorack>)                (M3f)
	#   Our own rewrite of a whole file, kept at src/plaits_ovr/<same path> (attributed
	#   to upstream in its header and in NOTICE). Headers there shadow upstream's; a
	#   .cc there is compiled instead of upstream's.
	#
	# Each trim has a static_assert in sp1_synth.cc proving what was compiled.
	set(SP1_OVR ${CMAKE_CURRENT_BINARY_DIR}/plaits_overrides)
	set(SP1_REPL ${CMAKE_CURRENT_SOURCE_DIR}/src/plaits_ovr)
	set_property(GLOBAL PROPERTY SP1_PLAITS_SWAP "")
	function(sp1_plaits_swap rel ours)
		get_property(swaps GLOBAL PROPERTY SP1_PLAITS_SWAP)
		if("${rel}" IN_LIST swaps)
			message(FATAL_ERROR "Plaits override: ${rel} is overridden twice")
		endif()
		set_property(GLOBAL APPEND PROPERTY SP1_PLAITS_SWAP ${rel})
		set_property(GLOBAL PROPERTY SP1_PLAITS_SWAP_TO_${rel} ${ours})
	endfunction()
	function(sp1_plaits_override rel from to)
		set(src ${ER}/${rel})
		file(READ ${src} text)
		string(REPLACE "${from}" "" stripped "${text}")
		string(LENGTH "${text}" n0)
		string(LENGTH "${stripped}" n1)
		string(LENGTH "${from}" nf)
		math(EXPR count "(${n0} - ${n1}) / ${nf}")
		if(NOT count EQUAL 1)
			message(FATAL_ERROR
				"Plaits override: expected '${from}' exactly once in ${rel}, "
				"found ${count}. Upstream changed? Re-check the trim.")
		endif()
		if(EXISTS ${SP1_REPL}/${rel})
			message(FATAL_ERROR "Plaits override: ${rel} is also replaced in src/plaits_ovr")
		endif()
		string(REPLACE "${from}" "${to}" text "${text}")
		set(text "// GENERATED by firmware/CMakeLists.txt (sp1_plaits_override) from\n// third_party/eurorack/${rel}. Do not edit; see the override in CMakeLists.txt.\n${text}")
		# Write via configure_file so the copy's timestamp only changes when its
		# content does (no needless full Plaits rebuild on every reconfigure).
		file(WRITE ${SP1_OVR}/${rel}.tmp "${text}")
		configure_file(${SP1_OVR}/${rel}.tmp ${SP1_OVR}/${rel} COPYONLY)
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
		if(rel MATCHES "\\.cc$")
			sp1_plaits_swap(${rel} ${SP1_OVR}/${rel})
		else()
			sp1_plaits_swap(${rel} "")
		endif()
	endfunction()
	function(sp1_plaits_replace rel)
		if(NOT EXISTS ${SP1_REPL}/${rel})
			message(FATAL_ERROR "Plaits replace: src/plaits_ovr/${rel} not found")
		endif()
		if(rel MATCHES "\\.cc$")
			sp1_plaits_swap(${rel} ${SP1_REPL}/${rel})
		else()
			sp1_plaits_swap(${rel} "")
		endif()
	endfunction()

	# #32: Plaits' block size, CONFIG_SP1_PLAITS_BLOCK samples (upstream 12). Its buffers
	# are sized for 24 already (kMaxBlockSize); sp1_synth.cc static_asserts the match.
	sp1_plaits_override(plaits/dsp/dsp.h
		"const size_t kBlockSize = 12;"
		"const size_t kBlockSize = ${CONFIG_SP1_PLAITS_BLOCK};")
	# M3e: String rings CONFIG_SP1_STRING_VOICES strings (upstream 3).
	sp1_plaits_override(plaits/dsp/engine/string_engine.h
		"const int kNumStrings = 3;"
		"const int kNumStrings = ${CONFIG_SP1_STRING_VOICES};")
	# M3f: Particle: CONFIG_SP1_PARTICLES particles (upstream 6). Plaits divides the
	# density by the count, so the event rate is unchanged.
	sp1_plaits_override(plaits/dsp/engine/particle_engine.h
		"const int kNumParticles = 6;"
		"const int kNumParticles = ${CONFIG_SP1_PARTICLES};")
	# M3f: Particle: skip the diffuser while MORPH >= centre, where Plaits mixes it in
	# at exactly 0 (diffusion = 0). Cleared when it comes back into use, so no stale
	# tail from the last time it ran is heard.
	sp1_plaits_override(plaits/dsp/engine/particle_engine.cc
		"  diffuser_.Process("
		"  // wakes-sp1 (M3f): the diffuser's mix amount is exactly 0 when diffusion is 0.\n  static bool sp1_diffusing = false;\n  if (diffusion > 0.0f && !sp1_diffusing) {\n    diffuser_.Reset();\n  }\n  sp1_diffusing = diffusion > 0.0f;\n  if (sp1_diffusing) diffuser_.Process(")
	# #36: the two fills that clear Plaits' shared engine RAM on an engine change go
	# through plaits::sp1_zero (src/sp1_zero.cc, word stores) instead of std::fill, which
	# GCC turns into picolibc's byte-at-a-time memset: Particle's 16 KB diffuser took
	# ~1.7 ms and String's delay lines ~1.1 ms of a 2 ms block. Same bytes zeroed. The
	# block-scope declaration names plaits::sp1_zero, as in x_y_generator.cc below.
	sp1_plaits_override(plaits/dsp/fx/fx_engine.h
		"    std::fill(&buffer_[0], &buffer_[size], 0);"
		"    // wakes-sp1 (#36): word stores, not picolibc's byte-loop memset.\n    void sp1_zero(void* dst, size_t n);\n    sp1_zero(&buffer_[0], sizeof(buffer_[0]) * size);")
	sp1_plaits_override(plaits/dsp/physical_modelling/delay_line.h
		"    std::fill(&line_[0], &line_[max_delay], T(0));"
		"    // wakes-sp1 (#36): word stores, not picolibc's byte-loop memset.\n    void sp1_zero(void* dst, size_t n);\n    sp1_zero(&line_[0], sizeof(line_[0]) * max_delay);")
	# M3f: Modal: CONFIG_SP1_MODAL_MODES resonator modes (upstream 24). Resonator is
	# used by the modal engine only.
	sp1_plaits_override(plaits/dsp/physical_modelling/resonator.h
		"const int kMaxNumModes = 24;"
		"const int kMaxNumModes = ${CONFIG_SP1_MODAL_MODES};")
	# M3f: Additive: our rewrite of the partial summation (same 24 + 8 partials).
	sp1_plaits_replace(plaits/dsp/engine/additive_engine.h)
	sp1_plaits_replace(plaits/dsp/engine/additive_engine.cc)
	# M3g: String machine: the ensemble skips its taps while its amount is exactly 0
	# (TIMBRE on the centre detent); the divide-down oscillator (also used by Chords)
	# holds its gains per block. Both headers only.
	sp1_plaits_replace(plaits/dsp/fx/ensemble.h)
	sp1_plaits_replace(plaits/dsp/oscillator/string_synth_oscillator.h)
	# issue #22: the variable-shape and variable-saw oscillators (virtual analog, phase
	# distortion, VA-VCF) take ONE divide per sample for their two slopes instead of two,
	# at the same cost whether PW moves or not. Same quantities, float rounding only.
	sp1_plaits_replace(plaits/dsp/oscillator/variable_shape_oscillator.h)
	sp1_plaits_replace(plaits/dsp/oscillator/variable_saw_oscillator.h)
	# ---- issue #22: Chords' organ/wavetable crossfade, bounded in time (Adara) ----
	# Each of the five notes fades from its divide-down organ to its wavetable over a
	# 0.02-wide slice of MORPH, and a note inside its slice renders BOTH oscillators --
	# six where there are normally five, ~12 % of the block, sustained for as long as
	# MORPH rests there. Two changes, chosen by ear from host renders:
	#   - the slice is a quarter as wide (0.005), around upstream's midpoint, and the
	#     fade still follows MORPH through it;
	#   - MORPH resting inside a slice for 50 ms resolves the note to the nearer side,
	#     every fade is slew-limited to 50 ms end to end, and only one note may start a
	#     fade while another is mid-way -- so six oscillators is the ceiling and is never
	#     sustained while MORPH rests.
	# And the five voices share ONE waveform position (MORPH), stepped once per sample
	# instead of once per voice -- same cost in every state (Adara). Both changes needed
	# more than one passage per file, so the chord engine and the wavetable oscillator
	# are full replacements in src/plaits_ovr (was: generated overrides of the engine).
	sp1_plaits_replace(plaits/dsp/engine/chord_engine.h)
	sp1_plaits_replace(plaits/dsp/engine/chord_engine.cc)
	sp1_plaits_replace(plaits/dsp/oscillator/wavetable_oscillator.h)
	# ---- M4a: a HARMONICS attenuverter (Adara) ----
	# Plaits' panel has attenuverters on FM, TIMBRE and MORPH but not on HARMONICS: its
	# HARMONICS CV is summed in raw, and -- the part that matters musically -- HARMONICS
	# is the one parameter the internal decay envelope can never reach, because that
	# routing is what an unpatched attenuverter means on the other three.
	#
	# HARMONICS gets the same treatment, which is a genuine addition to Plaits rather
	# than a re-binding: `harmonics_modulation_amount` in Patch, `harmonics_patched` in
	# Modulations, and HARMONICS run through the same ApplyModulations() as the other
	# three. The envelope amount is the plain decay envelope, like TIMBRE's: the speech
	# engine's `internal_envelope_amplitude` is computed FROM p.harmonics, so using it
	# here would be circular.
	#
	# ---- issue #22: ONE low-pass gate for the mix (Adara) ----
	# Plaits gates OUT and AUX separately; the SP-1 only ever plays one of OUT, AUX,
	# OUT+AUX, OUTxAUX through one output, so the voice now mixes first and gates once.
	# The reasoning and what is exact per mode are at the top of the replacement files.
	# That needed more than one passage, so M4a's two one-passage overrides became these
	# full replacements, which carry M4a's changes unchanged.
	#
	# ⚠️ Patch, Modulations and Voice are laid out by this header for EVERY Plaits file,
	# so it must shadow upstream for all of them (src/plaits_ovr is on the include path
	# ahead of the eurorack root). sp1_synth.cc static_asserts both changes, and
	# check_image.py fails any image where two objects disagree on a layout.
	sp1_plaits_replace(plaits/dsp/voice.h)
	sp1_plaits_replace(plaits/dsp/voice.cc)
	# ---- M4c: the X / Y voltage range becomes PER CHANNEL ----
	# Marbles' range is a per-GROUP setting and XYGenerator::Process writes it into each
	# channel's ScaleOffset itself, every block -- so anything we set per channel would be
	# overwritten. This makes that one switch ask us which range channel `i` should get.
	#
	# All the policy lives in sp1_marbles.cc (IntelligentRange); this override holds none,
	# and a non-INTELLIGENT group still gets exactly what upstream gave it. The local
	# extern declaration keeps it to a single replaced passage, which is the limit of one
	# override per file.
	sp1_plaits_override(marbles/random/x_y_generator.cc
		"    switch (settings.voltage_range) {"
		"    // wakes-sp1 (M4c): the range is per CHANNEL, not per group.
    // A block-scope extern declares the name in the innermost enclosing NAMESPACE, so
    // this is marbles::sp1_mrb_channel_range -- defined in firmware/src/sp1_marbles.cc.
    // (`extern \"C\"` is not allowed at block scope, hence C++ linkage.)
    extern int sp1_mrb_channel_range(int channel, int group_range);
    switch (static_cast<VoltageRange>(sp1_mrb_channel_range(
        static_cast<int>(i), static_cast<int>(settings.voltage_range)))) {")
	# M5b: MIDI clock. In external-clock mode with NO gate stream (a null external_clock,
	# which upstream never passes), the external ramp comes from the host's MIDI clock --
	# marbles::sp1_mrb_external_ramp in firmware/src/sp1_marbles.cc -- instead of from the
	# ramp extractor (32 kHz-only, and built for jittery analog clocks). Everything around it
	# -- RATE's ratio table and hysteresis, the t range, jitter, the slave ramps -- is
	# upstream's. On a fresh ramp (a reset, or the clock just became external) the step from
	# the last ramp value is not read as clock movement, and outside a reset the master clock
	# is aligned to the ramp. Same block-scope extern trick as x_y_generator.cc above.
	sp1_plaits_override(marbles/random/t_generator.cc
		"    ramp_extractor_.Process("
		"    // wakes-sp1 (M5b): no gate stream = MIDI clock (firmware/CMakeLists.txt).
    extern bool sp1_mrb_external_ramp(const Ratio& ratio, bool* reset, float* ramp,
                                      size_t size);
    if (!external_clock) {
      if (sp1_mrb_external_ramp(ratio, reset, ramps.external, size)) {
        previous_external_ramp_value_ = *reset ? 0.0f : ramps.external[0];
        if (!*reset) {
          master_phase_ = ramps.external[0];
        }
      }
    } else ramp_extractor_.Process(")
	# M4: Marbles' t generator gets a setter for its master phase, so PLAY starts the
	# clock on a beat (sp1_marbles.cc). Nothing else in Marbles is changed.
	sp1_plaits_override(marbles/random/t_generator.h
		"  inline void set_model(TGeneratorModel model) {"
		"  // wakes-sp1 (M4): PLAY starts the clock on a beat.\n  inline void set_master_phase(float phase) {\n    master_phase_ = phase;\n  }\n\n  inline void set_model(TGeneratorModel model) {")

	# ⚠️ Remove any generated file that is no longer an override (issue #22). SP1_OVR is
	# FIRST on the include path, so a stale copy left by an earlier configure -- voice.h
	# after it became a full replacement in src/plaits_ovr -- would shadow the file that
	# replaced it. Current overrides are left alone, so their timestamps (and the Plaits
	# objects built from them) survive a reconfigure.
	get_property(sp1_swaps GLOBAL PROPERTY SP1_PLAITS_SWAP)
	file(GLOB_RECURSE sp1_ovr_files LIST_DIRECTORIES false ${SP1_OVR}/*)
	foreach(f ${sp1_ovr_files})
		file(RELATIVE_PATH frel ${SP1_OVR} ${f})
		string(REGEX REPLACE "\\.tmp$" "" frel_base "${frel}")
		set(sp1_keep FALSE)
		if("${frel_base}" IN_LIST sp1_swaps AND NOT EXISTS ${SP1_REPL}/${frel_base})
			set(sp1_keep TRUE)
		endif()
		if(NOT sp1_keep)
			file(REMOVE ${f})
			message(STATUS "Plaits override: removed stale ${frel}")
		endif()
	endforeach()

	set(PLAITS_SOURCES
		${ER}/plaits/dsp/voice.cc
		${ER}/plaits/resources.cc
		${ER}/stmlib/dsp/atan.cc
		${ER}/stmlib/dsp/units.cc
		${ER}/stmlib/utils/random.cc
	)
	file(GLOB PLAITS_ENGINE_SOURCES
		${ER}/plaits/dsp/engine/*.cc
		${ER}/plaits/dsp/engine2/*.cc
		${ER}/plaits/dsp/chords/*.cc
		${ER}/plaits/dsp/fm/*.cc
		${ER}/plaits/dsp/physical_modelling/*.cc
		${ER}/plaits/dsp/speech/*.cc
	)
	list(APPEND PLAITS_SOURCES ${PLAITS_ENGINE_SOURCES})
	# M4: Marbles' generators (third_party/eurorack/README.md), same library and flags.
	file(GLOB MARBLES_SOURCES
		${ER}/marbles/random/*.cc
		${ER}/marbles/ramp/*.cc
	)
	list(APPEND PLAITS_SOURCES ${MARBLES_SOURCES} ${ER}/marbles/resources.cc)
	get_property(swaps GLOBAL PROPERTY SP1_PLAITS_SWAP)
	foreach(rel ${swaps})
		get_property(ours GLOBAL PROPERTY SP1_PLAITS_SWAP_TO_${rel})
		if(ours)
			list(FIND PLAITS_SOURCES ${ER}/${rel} idx)
			if(idx EQUAL -1)
				message(FATAL_ERROR "Plaits override: ${rel} is not a Plaits source")
			endif()
			list(REMOVE_AT PLAITS_SOURCES ${idx})
			list(APPEND PLAITS_SOURCES ${ours})
		endif()
	endforeach()
	zephyr_library_sources(src/sp1_synth.cc src/sp1_marbles.cc src/sp1_zero.cc ${PLAITS_SOURCES})
	# #36: or GCC recognises sp1_zero's loops as a fill and calls memset -- the byte loop
	# it exists to avoid (tools/ci/check_image.py checks). No unrolling: the body is
	# unrolled by hand, and GCC's copy of the 0-3 byte head loop came to 600 bytes.
	set_source_files_properties(src/sp1_zero.cc PROPERTIES
		COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns;-fno-unroll-loops")

	# ⚠️ ORDER MATTERS: generated overrides, our replacements, the shim, then the
	# eurorack root. The shim's plaits/user_data.h shadows upstream's (which reads an
	# STM32 flash address that is a bus fault on the nRF52840). See
	# src/plaits_shim/plaits/user_data.h.
	zephyr_library_include_directories(
		${SP1_OVR}
		${SP1_REPL}
		src/plaits_shim
		${ER}
	)
	zephyr_library_compile_options(
		# GNU dialect, as upstream's own build (strict -std=c++14 from CONFIG_STD_CPP14
		# otherwise), plus _DEFAULT_SOURCE: Zephyr's feature macros hide M_PI in
		# picolibc's <math.h>, and stmlib and Plaits use it throughout.
		$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++14>
		-D_DEFAULT_SOURCE
		-O2
		-funroll-loops
		-finline-functions-called-once
		-fno-move-loop-invariants
		# Upstream code, compiled as-is: silence what it was never written against.
		-Wno-unused-variable
		-Wno-unused-parameter
		-Wno-unused-local-typedefs
		-Wno-sign-compare
		-Wno-double-promotion
	)
	# A Zephyr library declared from the APPLICATION is not linked on its own.
	target_link_libraries(app PRIVATE sp1_plaits)
	target_sources(app PRIVATE src/sp1_marbles_ui.c)
	target_include_directories(app PRIVATE src)

	# ---- M5a: USB-MIDI in (the MIDI script is generated above, with the engines) ----
	add_dependencies(sp1_plaits sp1_midi_gen)
	zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR}/sp1_generated)

	if(CONFIG_SP1_MIDI)
		# The MIDI core runs in the audio thread: same library, same -O2, as the synth.
		zephyr_library_sources(src/sp1_midi.cc)

		# feldd's USB-MIDI 1.0 class, vendored unmodified in third_party/feldd. Our
		# changes are a patch applied to a copy in the build directory, STRICTLY: if
		# upstream text has moved under it, configuring stops (tools/apply_patch.py) --
		# the same rule as sp1_plaits_override(). third_party/feldd/README.md.
		set(FELDD ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/feldd)
		set(FELDD_OUT ${CMAKE_CURRENT_BINARY_DIR}/feldd/usb_midi1.c)
		execute_process(
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/../tools/apply_patch.py
				${FELDD}/src/usb_midi1.c ${FELDD}/patches/usb_midi1.patch ${FELDD_OUT}
			RESULT_VARIABLE feldd_rc)
		if(NOT feldd_rc EQUAL 0)
			message(FATAL_ERROR "third_party/feldd/patches/usb_midi1.patch does not apply")
		endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			${FELDD}/src/usb_midi1.c ${FELDD}/patches/usb_midi1.patch
			${CMAKE_CURRENT_SOURCE_DIR}/../tools/apply_patch.py)
		target_sources(app PRIVATE ${FELDD_OUT} ${FELDD}/src/usb_rt_parse.c)
		target_include_directories(app PRIVATE ${FELDD}/src)
	endif()

	if(CONFIG_SP1_USB_AUDIO)
		# M5c: USB audio out. The class is ours (src/sp1_uac.c); the ring and packet
		# regulator are Ryan Gilmore's uacring.c, vendored unmodified in
		# third_party/sp1-usb-audio, with Wakes' patch (word copies instead of picolibc's
		# byte-loop memcpy) applied to a copy STRICTLY, as for feldd above.
		set(UAC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sp1-usb-audio)
		set(UAC_OUT ${CMAKE_CURRENT_BINARY_DIR}/sp1-usb-audio/uacring.c)
		execute_process(
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/../tools/apply_patch.py
				${UAC}/upstream/usb-audio/uacring.c ${UAC}/patches/uacring.patch ${UAC_OUT}
			RESULT_VARIABLE uac_rc)
		if(NOT uac_rc EQUAL 0)
			message(FATAL_ERROR "third_party/sp1-usb-audio/patches/uacring.patch does not apply")
		endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			${UAC}/upstream/usb-audio/uacring.c ${UAC}/patches/uacring.patch
			${CMAKE_CURRENT_SOURCE_DIR}/../tools/apply_patch.py)
		target_sources(app PRIVATE src/sp1_uac.c ${UAC_OUT})
		target_include_directories(app PRIVATE ${UAC}/upstream/usb-audio)
		# Wakes' tuning (96-frame blocks) ahead of uacring.h, exactly as uactest builds it;
		# and no loop distribution, or GCC turns the word copies back into memcpy/memset
		# calls (tools/ci/check_image.py checks the image).
		set_source_files_properties(${UAC_OUT} PROPERTIES COMPILE_OPTIONS
			"-include;${CMAKE_CURRENT_SOURCE_DIR}/src/sp1_uac_tuning.h;-fno-tree-loop-distribute-patterns;-fno-unroll-loops")
	endif()
endif()
