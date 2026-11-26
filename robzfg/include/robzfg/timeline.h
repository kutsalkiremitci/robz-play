// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - when each picture is shown.
//
// A stream is a row of slots, one every 1/60 s by the console's clock, and the console puts
// a frame into each. A game that renders 30 pictures a second fills every other slot with a
// repeat of the picture before. Frame generation puts a generated picture where the repeats
// were, so that again every slot shows something new.
//
// For that to look even, every picture has to appear at the moment its slot is due, however
// unevenly the frames arrived and however unevenly the game produced them:
//
//  - Frames do not arrive when their slot is due but later, each by a different amount. A
//    repeat is a few packets and is here within a millisecond or two; a new picture is a
//    hundred packets and more, which the console sends over most of a frame's time, so it
//    arrives some ten milliseconds after the repeats would have (measured on a real PS5
//    stream at 100 Mbit/s: 11.5 ms in the middle, 15 ms for nine in ten). StreamClock
//    recovers the clock of the stream from the arrival times: the earliest arrivals lie on
//    a straight line, and that line is the clock.
//
//  - A game that falls below its frame rate leaves two repeats instead of one now and then.
//    The pictures on both sides of the hole still belong to their own slots; moving the
//    whole timeline because one picture came a slot later would make every picture after it
//    late for a while. OutputTimeline therefore gives each picture the time of its slot plus
//    a fixed delay, and leaves out a generated picture that cannot be ready by then.

#ifndef ROBZFG_TIMELINE_H
#define ROBZFG_TIMELINE_H

#include <cstdint>
#include <vector>

namespace robzfg
{

// Recovers the clock of a stream from the arrival times of its frames.
//
// Not thread-safe.
class StreamClock
{
public:
	// `period_us`: time between two slots as the stream declares it (1/60 s). The real
	// period differs by a fraction of a percent; the clock follows it.
	void reset(double period_us);

	// Call for every frame of the stream, repeats included, in the order they arrived.
	// `advance`: slots since the frame before, 1 unless frames were lost in between.
	// Returns the slot of this frame.
	int64_t observe(uint64_t arrival_us, int advance = 1);

	// True once a frame has been observed.
	bool valid() const { return anchored; }
	double period() const { return period_us; }
	// The slot of the frame observed last.
	int64_t slot() const { return current_slot; }
	// When the frame of a slot arrives if nothing holds it up (fractions of a slot are fine).
	double timeOf(double slot) const { return phase_us + slot * period_us; }
	// How much later than that the last frame arrived.
	double lastError() const { return last_error_us; }
	// Times the line had to be moved up in one step: every recent frame was later than it by
	// more than lateness explains. The console sent nothing for a frame or two (everything
	// after that is later by as much), or the stream went from repeats and new pictures to
	// new pictures only.
	unsigned int jumps() const { return jump_count; }

private:
	double period_us = 1000000.0 / 60.0;
	bool anchored = false;
	int64_t current_slot = 0;
	double phase_us = 0.0;
	uint64_t last_arrival_us = 0;
	double last_error_us = 0.0;
	// arrival minus (slot * period) of the last frames: the line is their lowest point
	static constexpr int RECENT = 8;
	double recent_offset_us[RECENT] = {};
	int recent_count = 0;
	int recent_next = 0;
	unsigned int jump_count = 0;
};

// Decides, for every new picture of the stream, when it is shown and which generated
// pictures go in front of it.
//
// Not thread-safe.
class OutputTimeline
{
public:
	// What generating has been costing lately, in microseconds; 0 where not known yet.
	struct Costs
	{
		double first_us = 0.0;       // the first generated picture of a pair (includes the motion estimate)
		double rest_us = 0.0;        // each further one
		double ready_delay_us = 0.0; // from a frame's arrival until its first generated picture is ready
	};

	struct Generated
	{
		float t = 0.5f;      // position between the previous new picture (0) and this one (1)
		uint64_t due_us = 0; // when to show it
	};

	struct Plan
	{
		int64_t slot = 0;
		// Slots since the previous new picture; 0 when there is none to count from. (Not
		// called "slots": Qt makes that word a macro.)
		int spacing = 0;
		// Show the picture right away: the source already runs at the target rate, so there
		// is nothing to generate and no reason to hold anything back.
		bool immediate = false;
		std::vector<Generated> generated;
		uint64_t real_due_us = 0;
		// Generated pictures left out because they could not have been ready in time: the
		// picture before stays up for these slots.
		int held = 0;
		// Delay between a slot being due and its picture being shown.
		double latency_us = 0.0;
	};

	// `target_fps`: 60 shows one picture per slot of a 60 fps stream, 120 two.
	void reset(double stream_period_us, unsigned int target_fps, int max_generated);

	// Every frame of the stream, repeats included, in order (see StreamClock::observe).
	int64_t observe(uint64_t arrival_us, int advance = 1) { return clock.observe(arrival_us, advance); }

	// Call once for every frame that is a new picture, after observe(). `may_generate`:
	// false when nothing can be generated between the previous picture and this one (a
	// cut, a picture that barely changed, an engine without two frames).
	Plan plan(uint64_t now_us, const Costs &costs, bool may_generate);

	// Nothing is generated across what just happened (frames passed on untouched, an error).
	void forgetPrevious() { have_previous = false; }

	const StreamClock &streamClock() const { return clock; }
	// Slots between two new pictures as the source typically delivers them (2 for a
	// 30 fps game in a 60 fps stream).
	int nominalSlots() const { return nominal_slots; }
	// The same as a time.
	double cadenceUs() const { return nominal_slots * clock.period(); }
	// The longest the delay waits for the first generated picture of a pair to get ready.
	// A picture that takes longer than this is left out (see Plan::held).
	double readyLimitUs() const;
	// How much later than the clock new pictures typically arrive (see the top of this file):
	// close to the latest of the recent ones, leaving out hitches.
	double pictureLateness() const { return picture_late_us; }
	// True when the source leaves nothing to generate (see Plan::immediate).
	bool sourceAtTarget() const { return nominal_slots * per_slot <= 1; }

private:
	void noteSpacing(int spacing);

	StreamClock clock;
	int per_slot = 1;
	int max_generated = 3;
	bool have_previous = false;
	int64_t previous_slot = 0;
	int nominal_slots = 1;
	std::vector<int> recent_slots;
	size_t recent_next = 0;
	bool latency_valid = false;
	double latency_us = 0.0;
	int fast_slew_left = 0;
	double picture_late_us = 0.0;
};

} // namespace robzfg

#endif // ROBZFG_TIMELINE_H
