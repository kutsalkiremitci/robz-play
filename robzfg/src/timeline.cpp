// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - when each picture is shown (see timeline.h).

#include "robzfg/timeline.h"

#include <algorithm>
#include <cmath>

namespace robzfg
{

namespace
{

// A silence this long ends the old clock: what follows is anchored anew.
const double RESET_GAP_US = 300000.0;
// The line through the earliest arrivals is raised by at most this many slots per slot: far
// more than a real 60 fps clock differs from a nominal one, far less than jitter.
const double SLEW_PER_SLOT = 0.005;
// When every one of the last frames (StreamClock::RECENT of them) arrived this many slots
// later than the line, the line is wrong and moves up to them at once.
const double JUMP_SLOTS = 0.4;
// The typical lateness of new pictures follows a later one quickly and forgets it slowly,
// which keeps it near the latest of the recent ones. One that is this many slots late is a
// hitch and does not count.
const double PICTURE_LATE_UP = 0.2;
const double PICTURE_LATE_DOWN = 0.02;
const double PICTURE_LATE_LIMIT_SLOTS = 1.5;

// New pictures further apart than this many slots are a hitch, not a frame rate.
const int MAX_SLOTS = 4;
// How many spacings back the typical spacing is taken from, and how many of them have to
// agree for a spacing to count as typical.
const size_t RECENT_SPACINGS = 12;
const int TYPICAL_COUNT = 6;
// The delay moves towards what it should be by this much per new picture, so that a change
// is spread over many pictures instead of showing as a jump; faster for the first pictures
// after the frame rate of the source has changed.
const double LATENCY_STEP_US = 250.0;
const double LATENCY_FAST_STEP_US = 1000.0;
const int LATENCY_FAST_PICTURES = 24;
// Time set aside for getting a generated picture ready, and for frames arriving late.
const double MIN_READY_ALLOWANCE_US = 6000.0;
// The delay follows what generating costs at the motion detail the user chose, as far as the
// graphics card can keep that up at all: generating one pair has to be over, with room to
// spare, before the next pair comes. That is this share of the time between two new pictures.
const double MAX_READY_SHARE = 0.8;
const double MIN_LATE_ALLOWANCE_US = 1500.0;
const double MAX_LATE_ALLOWANCE_SLOTS = 1.25;
// A generated picture expected to be ready this long after it is due is still shown.
const double GRACE_US = 3000.0;

} // namespace

void StreamClock::reset(double new_period_us)
{
	period_us = new_period_us > 0.0 ? new_period_us : 1000000.0 / 60.0;
	anchored = false;
	current_slot = 0;
	phase_us = 0.0;
	last_arrival_us = 0;
	last_error_us = 0.0;
	recent_count = 0;
	recent_next = 0;
	jump_count = 0;
}

int64_t StreamClock::observe(uint64_t arrival_us, int advance)
{
	if(advance < 1)
		advance = 1;
	const double arrival = (double)arrival_us;

	if(!anchored || arrival_us < last_arrival_us || arrival - (double)last_arrival_us > RESET_GAP_US)
	{
		// the first frame, or the first after a silence: the clock starts (again) here
		current_slot = anchored ? current_slot + advance : 0;
		phase_us = arrival - (double)current_slot * period_us;
		anchored = true;
		last_arrival_us = arrival_us;
		last_error_us = 0.0;
		recent_offset_us[0] = phase_us;
		recent_count = 1;
		recent_next = 1 % RECENT;
		return current_slot;
	}

	last_arrival_us = arrival_us;
	current_slot += advance;
	const double offset = arrival - (double)current_slot * period_us;
	recent_offset_us[recent_next] = offset;
	recent_next = (recent_next + 1) % RECENT;
	if(recent_count < RECENT)
		recent_count++;
	double lowest = recent_offset_us[0];
	for(int i = 1; i < recent_count; i++)
		lowest = std::min(lowest, recent_offset_us[i]);

	if(offset < phase_us)
	{
		// earlier than the line allows: the line was too high
		phase_us = offset;
	}
	else if(recent_count == RECENT && lowest - phase_us > JUMP_SLOTS * period_us)
	{
		// Not one of the last frames came near the line. Frames that were only held up
		// arrive in a burst and are back on it within a few; these are not, so the line
		// itself has moved (see jumps()).
		phase_us = lowest;
		jump_count++;
	}
	else
	{
		// the real period is a little longer or shorter than the declared one
		phase_us += std::min(lowest - phase_us, SLEW_PER_SLOT * period_us * advance);
	}
	last_error_us = offset - phase_us;
	return current_slot;
}

void OutputTimeline::reset(double stream_period_us, unsigned int target_fps, int new_max_generated)
{
	clock.reset(stream_period_us);
	per_slot = std::max(1, (int)std::lround((double)target_fps * clock.period() / 1000000.0));
	max_generated = std::max(0, new_max_generated);
	have_previous = false;
	previous_slot = 0;
	nominal_slots = 1;
	recent_slots.clear();
	recent_next = 0;
	latency_valid = false;
	latency_us = 0.0;
	fast_slew_left = 0;
	picture_late_us = 0.0;
}

void OutputTimeline::noteSpacing(int spacing_now)
{
	if(recent_slots.size() < RECENT_SPACINGS)
		recent_slots.push_back(spacing_now);
	else
	{
		recent_slots[recent_next] = spacing_now;
		recent_next = (recent_next + 1) % RECENT_SPACINGS;
	}
	int counts[MAX_SLOTS + 1] = {};
	for(int spacing : recent_slots)
		counts[spacing]++;

	// The typical spacing is the widest one that half of the recent pictures had: sized
	// for it, the delay also covers every narrower one. A game at 30 fps that is a slot
	// late with every second picture is treated as the slower game it then is; one that
	// is late now and then keeps the shorter delay and leaves that one slot empty.
	int typical = 0;
	for(int spacing = MAX_SLOTS; spacing >= 1; spacing--)
	{
		if(counts[spacing] >= TYPICAL_COUNT)
		{
			typical = spacing;
			break;
		}
	}
	if(!typical || typical == nominal_slots)
		return;
	// wider at once; narrower only when the current spacing has all but gone
	if(typical > nominal_slots || counts[nominal_slots] <= 1)
	{
		nominal_slots = typical;
		fast_slew_left = LATENCY_FAST_PICTURES;
	}
}

double OutputTimeline::readyLimitUs() const
{
	return nominal_slots * clock.period() * MAX_READY_SHARE;
}

OutputTimeline::Plan OutputTimeline::plan(uint64_t now_us, const Costs &costs, bool may_generate)
{
	Plan result;
	result.slot = clock.slot();
	// how late this picture came: what the delay has to cover for pictures like it
	const double came_late = clock.lastError();
	if(came_late < PICTURE_LATE_LIMIT_SLOTS * clock.period())
		picture_late_us += (came_late - picture_late_us) * (came_late > picture_late_us ? PICTURE_LATE_UP : PICTURE_LATE_DOWN);
	if(have_previous)
		result.spacing = (int)std::min<int64_t>(result.slot - previous_slot, 1000);
	const int64_t from_slot = previous_slot;
	const bool counted = have_previous && result.spacing >= 1 && result.spacing <= MAX_SLOTS;
	have_previous = true;
	previous_slot = result.slot;
	// The typical spacing is learnt from pairs something can be generated for: the odd
	// refinement of a still screen says nothing about the frame rate of the game.
	if(counted && may_generate)
		noteSpacing(result.spacing);

	if(!clock.valid() || sourceAtTarget())
	{
		result.immediate = true;
		result.real_due_us = now_us;
		// the delay starts from what it should be when generating starts (again)
		latency_valid = false;
		return result;
	}

	// The delay: the first generated picture of a pair sits (nominal - 1/per_slot) slots
	// before the new picture it is made from, and can only be made once that picture is
	// here. On top comes the time making it takes and the time new pictures arrive late by.
	const double period = clock.period();
	const double ready = std::min(readyLimitUs(), std::max(MIN_READY_ALLOWANCE_US, costs.ready_delay_us * 1.15 + 1000.0));
	const double late = std::min(MAX_LATE_ALLOWANCE_SLOTS * period, std::max(MIN_LATE_ALLOWANCE_US, picture_late_us + 1000.0));
	const double wanted = period * ((double)nominal_slots - 1.0 / per_slot) + ready + late;
	if(!latency_valid)
	{
		latency_us = wanted;
		latency_valid = true;
	}
	else
	{
		const double step = fast_slew_left > 0 ? LATENCY_FAST_STEP_US : LATENCY_STEP_US;
		latency_us += std::max(-step, std::min(step, wanted - latency_us));
	}
	if(fast_slew_left > 0)
		fast_slew_left--;
	result.latency_us = latency_us;

	int count = 0;
	if(may_generate && counted)
		count = std::min(max_generated, result.spacing * per_slot - 1);
	// when the first kept picture of the pair is expected to be ready
	double ready_at = (double)now_us + costs.first_us;
	const double rest = costs.rest_us > 0.0 ? costs.rest_us : costs.first_us * 0.25;
	for(int k = 1; k <= count; k++)
	{
		const double position = (double)k / (double)(count + 1);
		const double due = clock.timeOf((double)from_slot + result.spacing * position) + latency_us;
		if(ready_at > due + GRACE_US)
		{
			// could not be on the screen in time: the picture before it stays instead
			result.held++;
			continue;
		}
		Generated generated;
		generated.t = (float)position;
		generated.due_us = (uint64_t)std::max(due, 0.0);
		result.generated.push_back(generated);
		ready_at += rest;
	}
	if(result.held > 0)
	{
		// The pictures left out were the first ones of the pair. What remains keeps its
		// slots but shares the way from the previous picture to this one evenly: after
		// the pause the motion goes on in equal steps instead of one large and one small.
		const size_t kept = result.generated.size();
		for(size_t j = 0; j < kept; j++)
			result.generated[j].t = (float)(j + 1) / (float)(kept + 1);
	}

	const double real_due = clock.timeOf((double)result.slot) + latency_us;
	result.real_due_us = (uint64_t)std::max(real_due, (double)now_us);
	return result;
}

} // namespace robzfg
